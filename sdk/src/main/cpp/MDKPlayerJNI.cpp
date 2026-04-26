/*
 * Copyright (c) 2018-2025 WangBin <wbsecg1 at gmail.com>
 */
#include "jmi/jmi.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h> // before any mdk header
#include <mdk/Player.h>
#include <mdk/MediaInfo.h>
#include <list>
#include <string>
#include <vector>
#include <limits>
#include <iostream>
#include <atomic>
#include <chrono>
#include <memory>
#define  DECODE_TO_SURFACEVIEW 0
#define USE_VULKAN 0

enum { // custom enum
    MEDIA_ERROR = -1,
    MEDIA_INFO,
    MEDIA_PREPARED,
    MEDIA_PLAYBACK_COMPLETE,
    MEDIA_BUFFERING_UPDATE,
    MEDIA_SEEK_COMPLETE,
    MEDIA_BIT_RATE_CHANGED,
};

enum {
    MEDIA_INFO_UNKNOWN				= 1,
    MEDIA_INFO_VIDEO_RENDERING_START= 3,
    MEDIA_INFO_BUFFERING_START		= 701,
    MEDIA_INFO_BUFFERING_END		= 702,
    MEDIA_INFO_VIDEO_DECODER_SELECTED = 7101,
    MEDIA_INFO_VIDEO_FRAME_HEARTBEAT = 7102,
};

enum {
    MEDIA_ERROR_TIMED_OUT							= -110,
};

enum {
    VIDEO_DECODER_MODE_SW = 0,
    VIDEO_DECODER_MODE_HW = 1,
    VIDEO_DECODER_MODE_HW_PLUS = 2,
};

enum {
    SEEK_COMPLETE_SOURCE_PREPARE = 1,
    SEEK_COMPLETE_SOURCE_MANUAL = 2,
};

static constexpr const char* kHardwareVideoDecoder =
    "AMediaCodec:dv=0:acquire=latest:ndk_codec=1:java=0:copy=0:surface=1:image=1:async=1:low_latency=1";
static constexpr int64_t kVideoFrameHeartbeatIntervalMs = 1000;
static constexpr int64_t kPostFirstFrameHeartbeatWindowMs = 15000;

static const char* DecoderModeLabel(int mode)
{
    switch (mode) {
    case VIDEO_DECODER_MODE_SW:
        return "SW";
    case VIDEO_DECODER_MODE_HW:
        return "HW";
    case VIDEO_DECODER_MODE_HW_PLUS:
        return "HW+";
    default:
        return "UNKNOWN";
    }
}

static bool IsValidDecoderMode(int mode)
{
    return mode >= VIDEO_DECODER_MODE_SW && mode <= VIDEO_DECODER_MODE_HW_PLUS;
}

static bool ThrowInvalidDecoderMode(JNIEnv* env, jint mode)
{
    if (IsValidDecoderMode(mode))
        return false;
    jclass ex = env->FindClass("java/lang/IllegalArgumentException");
    if (ex) {
        const std::string message = "Unsupported MDK decoder mode: " + std::to_string(mode);
        env->ThrowNew(ex, message.c_str());
    }
    return true;
}

static std::vector<std::string> BuildVideoDecoderChain(int mode)
{
    switch (mode) {
    case VIDEO_DECODER_MODE_SW:
        return {"FFmpeg"};
    case VIDEO_DECODER_MODE_HW:
    case VIDEO_DECODER_MODE_HW_PLUS:
        return {kHardwareVideoDecoder, "FFmpeg"};
    default:
        return {};
    }
}

static std::string JoinVideoDecoderChain(const std::vector<std::string>& decoders)
{
    std::string result;
    for (size_t i = 0; i < decoders.size(); ++i) {
        if (i > 0)
            result += " | ";
        result += decoders[i];
    }
    return result;
}

static jint ClampToJInt(int64_t value)
{
    if (value > std::numeric_limits<jint>::max())
        return std::numeric_limits<jint>::max();
    if (value < std::numeric_limits<jint>::min())
        return std::numeric_limits<jint>::min();
    return static_cast<jint>(value);
}

static int64_t ElapsedRealtimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

static void PostEvent(std::weak_ptr<jobject> wp, int what, int arg1 = 0, int arg2 = 0, jobject msg = nullptr)
{
    auto sp = wp.lock();
    if (!sp)
        return;
    auto obj = *sp;
    JNIEnv* env = jmi::getEnv();
    static jclass sClass = nullptr;
    static jmethodID sMethod = nullptr;
    if (!sMethod) {
        jclass clazz = env->GetObjectClass(obj);
        sClass = (jclass)env->NewGlobalRef(clazz);
        sMethod = env->GetStaticMethodID(sClass, "postEventFromNative", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    }
    env->CallStaticVoidMethod(sClass, sMethod, obj, what, arg1, arg2, msg);
}

using namespace MDK_NS;
extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    //freopen("/sdcard/log.txt", "wta", stdout); // java.lang.IllegalArgumentException: Primary directory null not allowed for content://media/external_primary/file; allowed directories are [Download, Documents] filePath = /storage/emulated/0/loge.txt callingPackageName = com.mediadevkit.mdkplayer
    //freopen("/sdcard/loge.txt", "w", stderr);
    setLogHandler([](LogLevel v, const char* msg){
        if (v < LogLevel::Info)
            __android_log_print(ANDROID_LOG_WARN, "MDK-JNI", "%s", msg);
        else
            __android_log_print(ANDROID_LOG_DEBUG, "MDK-JNI", "%s", msg);
    });

    SetGlobalOption("profiler.gpu", 1);
    SetGlobalOption("logLevel", "all");

    std::clog << "JNI_OnLoad" << std::endl;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**) &env, JNI_VERSION_1_4) != JNI_OK || !env) {
        std::clog << "GetEnv for JNI_VERSION_1_4 failed" << std::endl;
        return -1;
    }

    jmi::javaVM(vm);
    //SetGlobalOption("JavaVM", vm);
    return JNI_VERSION_1_4;
}

void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    std::cout << "JNI_OnUnload" << std::endl;
}

struct PlayerRef {
    ~PlayerRef() {
        alive->store(false);
        awaiting_first_video_frame->store(false);
        delete player;
    }

    Player* player = new Player();
    std::shared_ptr<jobject> spobj;
    jobject surface = nullptr;
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<std::atomic_bool> awaiting_first_video_frame = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> buffering_video_frame_heartbeat_enabled =
        std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic<int64_t>> last_buffering_video_frame_heartbeat_at_ms =
        std::make_shared<std::atomic<int64_t>>(0);
    std::shared_ptr<std::atomic<int64_t>> video_frame_heartbeat_until_ms =
        std::make_shared<std::atomic<int64_t>>(0);
    int decoder_mode = VIDEO_DECODER_MODE_HW_PLUS;
    std::string configured_video_decoders;
    std::string selected_video_decoder_name;
    int selected_video_decoder_stream = -1;
};

PlayerRef* ref(jlong obj_ptr) {
    return reinterpret_cast<PlayerRef*>(obj_ptr);
}

Player* get(jlong obj_ptr) {
    auto r = ref(obj_ptr);
    return r->player;
}

void ApplyVideoDecoderMode(PlayerRef* pr)
{
    const auto decoders = BuildVideoDecoderChain(pr->decoder_mode);
    pr->configured_video_decoders = JoinVideoDecoderChain(decoders);
    pr->selected_video_decoder_name.clear();
    pr->selected_video_decoder_stream = -1;
    pr->player->setDecoders(MediaType::Video, decoders);
    __android_log_print(
        ANDROID_LOG_INFO,
        "MDK-JNI",
        "Configured video decoder chain. mode=%s(%d), chain=%s",
        DecoderModeLabel(pr->decoder_mode),
        pr->decoder_mode,
        pr->configured_video_decoders.c_str()
    );
}

void PostSeekComplete(std::weak_ptr<jobject> w, int64_t position_ms, int source)
{
    __android_log_print(
        ANDROID_LOG_INFO,
        "MDK-JNI",
        "Seek completed. source=%d, positionMs=%lld",
        source,
        static_cast<long long>(position_ms)
    );
    PostEvent(w, MEDIA_SEEK_COMPLETE, ClampToJInt(position_ms), source);
}

const VideoStreamInfo* firstVideoStreamInfo(Player* player) {
    const auto& info = player->mediaInfo();
    if (info.video.empty())
        return nullptr;
    return &info.video.front();
}

static std::string MetadataValue(
    const std::unordered_map<std::string, std::string>& metadata,
    const char* key
)
{
    const auto it = metadata.find(key);
    if (it == metadata.end())
        return {};
    return it->second;
}

static jclass TrackInfoSnapshotClass(JNIEnv* env)
{
    static jclass sClass = nullptr;
    if (sClass)
        return sClass;
    jclass localClass = env->FindClass("com/mediadevkit/sdk/MDKPlayer$TrackInfoSnapshot");
    sClass = (jclass)env->NewGlobalRef(localClass);
    env->DeleteLocalRef(localClass);
    return sClass;
}

static jmethodID TrackInfoSnapshotCtor(JNIEnv* env)
{
    static jmethodID sCtor = nullptr;
    if (sCtor)
        return sCtor;
    sCtor = env->GetMethodID(
        TrackInfoSnapshotClass(env),
        "<init>",
        "(IILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"
    );
    return sCtor;
}

static jobject NewTrackInfoSnapshot(
    JNIEnv* env,
    jint track_number,
    jint stream_index,
    const std::string& title,
    const std::string& language,
    const char* codec
)
{
    jstring jtitle = env->NewStringUTF(title.c_str());
    jstring jlanguage = env->NewStringUTF(language.c_str());
    jstring jcodec = env->NewStringUTF(codec ? codec : "");
    jobject result = env->NewObject(
        TrackInfoSnapshotClass(env),
        TrackInfoSnapshotCtor(env),
        track_number,
        stream_index,
        jtitle,
        jlanguage,
        jcodec
    );
    env->DeleteLocalRef(jtitle);
    env->DeleteLocalRef(jlanguage);
    env->DeleteLocalRef(jcodec);
    return result;
}

static jobjectArray NewTrackInfoSnapshotArray(JNIEnv* env, jsize size)
{
    return env->NewObjectArray(size, TrackInfoSnapshotClass(env), nullptr);
}

static jobject NewMediaInfoSnapshot(JNIEnv* env, jobjectArray audio, jobjectArray subtitle)
{
    static jclass sClass = nullptr;
    static jmethodID sCtor = nullptr;
    if (!sClass) {
        jclass localClass = env->FindClass("com/mediadevkit/sdk/MDKPlayer$MediaInfoSnapshot");
        sClass = (jclass)env->NewGlobalRef(localClass);
        env->DeleteLocalRef(localClass);
    }
    if (!sCtor) {
        sCtor = env->GetMethodID(
            sClass,
            "<init>",
            "([Lcom/mediadevkit/sdk/MDKPlayer$TrackInfoSnapshot;[Lcom/mediadevkit/sdk/MDKPlayer$TrackInfoSnapshot;)V"
        );
    }
    return env->NewObject(sClass, sCtor, audio, subtitle);
}

#define MDK_JNI_FUNC(Name) Java_com_mediadevkit_sdk_##Name
#define MDK_JNI(Return, Name, ...) \
    JNIEXPORT Return JNICALL MDK_JNI_FUNC(Name) (JNIEnv *env, jobject thiz, jlong obj_ptr, ##__VA_ARGS__)

MDK_JNI(jlong, MDKPlayer_nativeCreate)
{
    auto pr = new PlayerRef();
    jobject objr = env->NewGlobalRef(thiz); // none weak is fine too
    pr->spobj = std::shared_ptr<jobject>(new jobject(objr), [](jobject* p){
        jmi::getEnv()->DeleteGlobalRef(*p);
        delete p;
    });
    auto p = pr->player;
    std::weak_ptr<jobject> w = pr->spobj;
    auto alive = pr->alive;
    auto awaiting_first_video_frame = pr->awaiting_first_video_frame;
    auto buffering_video_frame_heartbeat_enabled = pr->buffering_video_frame_heartbeat_enabled;
    auto last_buffering_video_frame_heartbeat_at_ms = pr->last_buffering_video_frame_heartbeat_at_ms;
    auto video_frame_heartbeat_until_ms = pr->video_frame_heartbeat_until_ms;
    p->setTimeout(20000, [w, alive](int64_t t){
        if (!alive->load())
            return false;
        PostEvent(w, MEDIA_ERROR, MEDIA_ERROR_TIMED_OUT);
        return true;
    });
    p->onStateChanged([alive](PlaybackState s){
        (void)s;
        if (!alive->load())
            return;
    });
    p->onMediaStatus([w, alive, buffering_video_frame_heartbeat_enabled, last_buffering_video_frame_heartbeat_at_ms](MediaStatus oldVal, MediaStatus newVal){
        if (!alive->load())
            return false;
        if (flags_added(oldVal, newVal, MediaStatus::Buffering)) {
            buffering_video_frame_heartbeat_enabled->store(true);
            last_buffering_video_frame_heartbeat_at_ms->store(0);
            PostEvent(w, MEDIA_INFO, MEDIA_INFO_BUFFERING_START);
        }
        if (flags_removed(oldVal, newVal, MediaStatus::Buffering)) {
            buffering_video_frame_heartbeat_enabled->store(false);
            last_buffering_video_frame_heartbeat_at_ms->store(0);
            PostEvent(w, MEDIA_INFO, MEDIA_INFO_BUFFERING_END);
        }
        if (flags_added(oldVal, newVal, MediaStatus::Prepared))
            PostEvent(w, MEDIA_PREPARED);
        if (flags_added(oldVal, newVal, MediaStatus::End))
            PostEvent(w, MEDIA_PLAYBACK_COMPLETE);
        return true;
    });
    p->onEvent([w, alive](const MediaEvent& e){
        if (!alive->load())
            return false;
        //__android_log_print(ANDROID_LOG_DEBUG, "MDK-JNI", "MediaEvent %d: %s %s", e.error, e.category.data(), e.detail.data());
        if (e.category == "reader.buffering") { // TODO: hash map
            PostEvent(w, MEDIA_BUFFERING_UPDATE, e.error);
            return false;
        }
        if (e.category == "decoder.video" && !e.detail.empty() && e.detail != "size") {
            __android_log_print(
                ANDROID_LOG_INFO,
                "MDK-JNI",
                "Selected video decoder. stream=%d, decoder=%s",
                e.decoder.stream,
                e.detail.c_str()
            );
            JNIEnv* event_env = jmi::getEnv();
            jstring decoder_name = event_env->NewStringUTF(e.detail.c_str());
            PostEvent(w, MEDIA_INFO, MEDIA_INFO_VIDEO_DECODER_SELECTED, e.decoder.stream, decoder_name);
            event_env->DeleteLocalRef(decoder_name);
            return false;
        }
        return false;
    });
    p->onFrame<VideoFrame>([w, alive, awaiting_first_video_frame, buffering_video_frame_heartbeat_enabled, last_buffering_video_frame_heartbeat_at_ms, video_frame_heartbeat_until_ms](VideoFrame& frame, int track){
        if (!alive->load())
            return 0;
        if (track < 0 || !frame.isValid())
            return 0;

        const auto timestamp = frame.timestamp();
        if (timestamp == TimestampEOS)
            return 0;
        if (awaiting_first_video_frame->load()) {
            awaiting_first_video_frame->store(false);
            PostEvent(w, MEDIA_INFO, MEDIA_INFO_VIDEO_RENDERING_START);
            return 0;
        }

        const auto now_ms = ElapsedRealtimeMs();
        const auto is_buffering = buffering_video_frame_heartbeat_enabled->load();
        if (!is_buffering && now_ms > video_frame_heartbeat_until_ms->load())
            return 0;

        const auto last_report_ms = last_buffering_video_frame_heartbeat_at_ms->load();
        if (last_report_ms > 0 && now_ms - last_report_ms < kVideoFrameHeartbeatIntervalMs)
            return 0;
        last_buffering_video_frame_heartbeat_at_ms->store(now_ms);

        const std::string payload =
            "mediaTimeMs=" + std::to_string(static_cast<long long>(timestamp)) +
            ",width=" + std::to_string(frame.width()) +
            ",height=" + std::to_string(frame.height()) +
            ",reason=" + (is_buffering ? "buffering" : "post_first_frame");
        JNIEnv* event_env = jmi::getEnv();
        jstring heartbeat = event_env->NewStringUTF(payload.c_str());
        PostEvent(w, MEDIA_INFO, MEDIA_INFO_VIDEO_FRAME_HEARTBEAT, track, heartbeat);
        event_env->DeleteLocalRef(heartbeat);
        return 0;
    });
    //p->setActiveTracks(MediaType::Audio, {});
    //p->setAudioBackends({ "OpenSL"});
    //p->setDecoders(MediaType::Audio, {"AMediaCodec:java=0", "FFmpeg"}); // AMediaCodec: higher cpu? FIXME: wrong result on x86
   // name: c2.android.hevc.decoder,c2.qti.hevc.decoder.low_latency,c2.qti.hevc.decoder.secure, OMX.google.hevc.decoder, OMX.qcom.video.decoder.hevc.low_latency, c2.dolby.avc-hevc.decoder, OMX.google.hevc.decoder
    //p->setDecoders(MediaType::Video, {"MediaCodec:ndk_codec=1", "FFmpeg"});
    ApplyVideoDecoderMode(pr);
    //putenv("EGL_HDR_METADATA=0");
    putenv("GL_YUV_SAMPLER=1");
    //putenv("LOG_SHADER=1");
    return jlong(pr);
}

MDK_JNI(void, MDKPlayer_nativeDestroy)
{
    auto pr = ref(obj_ptr);
    if (!pr)
        return;
    pr->alive->store(false);
    pr->awaiting_first_video_frame->store(false);
    delete pr;
}

MDK_JNI(void, MDKPlayer_nativeSetMedia, jstring url)
{
    if (!url) {
        get(obj_ptr)->setMedia(nullptr);
        return;
    }
    const char* s = env->GetStringUTFChars(url, nullptr);
    get(obj_ptr)->setMedia(s);
    env->ReleaseStringUTFChars(url, s);
}

MDK_JNI(void, MDKPlayer_nativeSetMediaForType, jstring url, jint media_type)
{
    if (!url) {
        get(obj_ptr)->setMedia(nullptr, MediaType(media_type));
        return;
    }
    const char* s = env->GetStringUTFChars(url, nullptr);
    get(obj_ptr)->setMedia(s, MediaType(media_type));
    env->ReleaseStringUTFChars(url, s);
}

MDK_JNI(void, MDKPlayer_nativeSetNextMedia, jstring url)
{
    if (!url) {
        get(obj_ptr)->setNextMedia(nullptr);
        return;
    }
    const char* s = env->GetStringUTFChars(url, nullptr);
    get(obj_ptr)->setNextMedia(s);
    env->ReleaseStringUTFChars(url, s);
}

MDK_JNI(void, MDKPlayer_nativeSetPlayList, jobjectArray urls)
{
    auto p = get(obj_ptr);
    auto url_list = std::make_shared<std::list<std::string>>();
    jsize len = env->GetArrayLength(urls);
    for (jsize i = 0; i < len; ++i) {
        auto si = (jstring)env->GetObjectArrayElement(urls, i);
        const char* s = env->GetStringUTFChars(si, nullptr);
        url_list->push_back(s);
        env->ReleaseStringUTFChars(si, s);
    }
    env->DeleteLocalRef(urls);
    const std::string url0 = url_list->front();
    __android_log_print(ANDROID_LOG_INFO, "MDK.JNI", "***************Play list 1st media: %s, count: %d", url_list->front().data(), (int)url_list->size());
    url_list->pop_front();
    p->currentMediaChanged([=]{
        if (url_list->empty()) {
            __android_log_print(ANDROID_LOG_INFO, "MDK.JNI", "***************Play list finished");
            return;
        }
        __android_log_print(ANDROID_LOG_INFO, "MDK.JNI", "*************currentMediaChanged now: %s, next: %s", p->url(), url_list->front().data());
        p->setNextMedia(url_list->front().data());
        url_list->pop_front();
    });
    p->setMedia(url0.data()); // set after currentMediaChanged(), otherwise 1st setNextMedia won't be called
}

MDK_JNI(void, MDKPlayer_nativePrepare, jlong position_ms)
{
    auto pr = ref(obj_ptr);
    pr->awaiting_first_video_frame->store(true);
    pr->buffering_video_frame_heartbeat_enabled->store(false);
    pr->last_buffering_video_frame_heartbeat_at_ms->store(0);
    pr->video_frame_heartbeat_until_ms->store(
        ElapsedRealtimeMs() + kPostFirstFrameHeartbeatWindowMs
    );
    if (position_ms > 0) {
        pr->player->prepare(
            position_ms,
            [w = std::weak_ptr<jobject>(pr->spobj)](int64_t position, bool* boost){
                (void)boost;
                PostSeekComplete(w, position, SEEK_COMPLETE_SOURCE_PREPARE);
                // MDK PrepareCallback 必须返回 true 才会继续保留已加载媒体。
                // 返回 false 只适合“只读 MediaInfo 后立即卸载”的探测场景。
                return true;
            },
            SeekFlag::FromStart
        );
        return;
    }
    pr->player->prepare(position_ms);
}

MDK_JNI(void, MDKPlayer_nativeSetState, int state)
{
    get(obj_ptr)->set((State)state);
}

MDK_JNI(jint, MDKPlayer_nativeState)
{
    return jint(get(obj_ptr)->state());
}

MDK_JNI(void, MDKPlayer_nativeResizeVideoSurface, int width, int height)
{
#if !(DECODE_TO_SURFACEVIEW + 0)
    get(obj_ptr)->setVideoSurfaceSize(width, height);
#endif
}

MDK_JNI(void, MDKPlayer_nativeRenderVideo)
{
#if !(DECODE_TO_SURFACEVIEW + 0)
    get(obj_ptr)->renderVideo();
#endif
}

MDK_JNI(void, MDKPlayer_nativeSetPlaybackRate, jfloat value)
{
    get(obj_ptr)->setPlaybackRate(value);
}

MDK_JNI(jfloat, MDKPlayer_nativePlaybackRate)
{
    return get(obj_ptr)->playbackRate();
}

MDK_JNI(void, MDKPlayer_nativeSetDecoderMode, jint mode)
{
    if (ThrowInvalidDecoderMode(env, mode))
        return;
    auto pr = ref(obj_ptr);
    pr->decoder_mode = mode;
    ApplyVideoDecoderMode(pr);
}

MDK_JNI(jstring, MDKPlayer_nativeGetConfiguredVideoDecoders)
{
    auto pr = ref(obj_ptr);
    return env->NewStringUTF(pr->configured_video_decoders.c_str());
}

MDK_JNI(jlong, MDKPlayer_nativeSetSurface, jobject s, jlong win, int w, int h)
{
    std::cout << "~~~~~~~~~~~nativeSetSurface: " << s <<  std::endl;
    if (!obj_ptr)
        return 0; // called in surfaceDestroyed when player was already destroyed in onPause
    auto p = get(obj_ptr);
#if (DECODE_TO_SURFACEVIEW + 0)
    if (s) {
        //ANativeWindow* anw = s ? ANativeWindow_fromSurface(env, s) : nullptr; // TODO: release
        //p->setProperty("video.decoder", "window=" + std::to_string((intptr_t)anw));
        auto ss = (jobject)env->NewGlobalRef(s); // TODO: release
        p->setProperty("video.decoder", "surface=" + std::to_string((intptr_t)ss));
    }
#else
# if (USE_VULKAN + 0)
    p->setProperty("video.decoder", "surface=0"); // surface is not supported yet
    static jobject ss = nullptr;
    //if (ss)
    //    return (jlong)s;
    if (w <= 0 || h <= 0) // TODO: required by vk. BUS_ADRALN
        return (jlong)s;
    ss = s;
    VulkanRenderAPI vkra{};
    //vkra.debug = 1; // crash if no layer found
    std::clog << w << "x" << h << "device_index: " << vkra.device_index << std::endl;
    p->setRenderAPI(&vkra, s);
# endif
    p->updateNativeSurface(s, w, h);
#endif
    reinterpret_cast<PlayerRef*>(obj_ptr)->surface = s;
    return (jlong)s;
}

MDK_JNI(jlong, MDKPlayer_nativeGetBufferedDuration)
{
    int64_t bytes = 0;
    return (jlong)get(obj_ptr)->buffered(&bytes);
}

MDK_JNI(jlong, MDKPlayer_nativeGetBufferedBytes)
{
    int64_t bytes = 0;
    get(obj_ptr)->buffered(&bytes);
    return (jlong)bytes;
}

MDK_JNI(jint, MDKPlayer_nativeGetDuration)
{
    if (!obj_ptr)
        return 0;
    auto p = get(obj_ptr);
    return (jint)p->mediaInfo().duration;
}

MDK_JNI(jint, MDKPlayer_nativePosition)
{
    if (!obj_ptr)
        return 0;
    auto p = get(obj_ptr);
    return (jint)p->position();
}

MDK_JNI(void, MDKPlayer_nativeSeek, jint ms)
{
    if (!obj_ptr)
        return;
    auto p = get(obj_ptr);
    auto w = ref(obj_ptr)->spobj;
    p->seek(ms, [weak = std::weak_ptr<jobject>(w)](int64_t position){
        PostSeekComplete(weak, position, SEEK_COMPLETE_SOURCE_MANUAL);
    });
}

MDK_JNI(void, MDKPlayer_nativeSetVolume, jfloat value)
{
    get(obj_ptr)->setVolume(value);
}

MDK_JNI(jfloat, MDKPlayer_nativeVolume)
{
    return get(obj_ptr)->volume();
}

MDK_JNI(void, MDKPlayer_nativeSetMute, jboolean value)
{
    get(obj_ptr)->setMute(value);
}

MDK_JNI(jboolean, MDKPlayer_nativeIsMute)
{
    return (jboolean)get(obj_ptr)->isMute();
}

MDK_JNI(void, MDKPlayer_nativeSetActiveTracks, jint media_type, jintArray tracks)
{
    std::set<int> selected_tracks;
    if (tracks) {
        const jsize length = env->GetArrayLength(tracks);
        std::vector<jint> values(length);
        env->GetIntArrayRegion(tracks, 0, length, values.data());
        for (auto track : values)
            selected_tracks.insert((int)track);
    }
    get(obj_ptr)->setActiveTracks(MediaType(media_type), selected_tracks);
}

MDK_JNI(jobject, MDKPlayer_nativeGetMediaInfoSnapshot)
{
    const auto& info = get(obj_ptr)->mediaInfo();
    jobjectArray audio = NewTrackInfoSnapshotArray(env, (jsize)info.audio.size());
    for (jsize i = 0; i < (jsize)info.audio.size(); ++i) {
        const auto& stream = info.audio[(size_t)i];
        jobject item = NewTrackInfoSnapshot(
            env,
            i,
            (jint)stream.index,
            MetadataValue(stream.metadata, "title"),
            MetadataValue(stream.metadata, "language"),
            stream.codec.codec
        );
        env->SetObjectArrayElement(audio, i, item);
        env->DeleteLocalRef(item);
    }

    jobjectArray subtitle = NewTrackInfoSnapshotArray(env, (jsize)info.subtitle.size());
    for (jsize i = 0; i < (jsize)info.subtitle.size(); ++i) {
        const auto& stream = info.subtitle[(size_t)i];
        jobject item = NewTrackInfoSnapshot(
            env,
            i,
            (jint)stream.index,
            MetadataValue(stream.metadata, "title"),
            MetadataValue(stream.metadata, "language"),
            stream.codec.codec
        );
        env->SetObjectArrayElement(subtitle, i, item);
        env->DeleteLocalRef(item);
    }

    jobject snapshot = NewMediaInfoSnapshot(env, audio, subtitle);
    env->DeleteLocalRef(audio);
    env->DeleteLocalRef(subtitle);
    return snapshot;
}

MDK_JNI(jint, MDKPlayer_nativeGetVideoWidth)
{
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jint)stream->codec.width : 0;
}

MDK_JNI(jint, MDKPlayer_nativeGetVideoHeight)
{
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jint)stream->codec.height : 0;
}

MDK_JNI(jfloat, MDKPlayer_nativeGetVideoFrameRate)
{
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jfloat)stream->codec.frame_rate : 0.0f;
}

MDK_JNI(jlong, MDKPlayer_nativeGetVideoBitRate)
{
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jlong)stream->codec.bit_rate : 0;
}

MDK_JNI(jstring, MDKPlayer_nativeGetVideoCodec)
{
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    if (!stream || !stream->codec.codec)
        return nullptr;
    return env->NewStringUTF(stream->codec.codec);
}

MDK_JNI(void, MDKPlayer_nativeSetColorSpace, jint value)
{
    auto r = ref(obj_ptr);
    auto p = get(obj_ptr);
    p->set(ColorSpace(value)); // store default value globally, will be used if surface is changed
    p->set(ColorSpace(value), r->surface); // apply for current surface
}

MDK_JNI(void, MDKPlayer_nativeSetAudioBackends, jstring backends)
{
    if (!backends)
        return;
    const char* s = env->GetStringUTFChars(backends, nullptr);
    get(obj_ptr)->setAudioBackends({s});
    env->ReleaseStringUTFChars(backends, s);
}

MDK_JNI(void, MDKPlayer_nativeSetProperty, jstring key, jstring value)
{
    if (!key || !value)
        return;
    const char* k = env->GetStringUTFChars(key, nullptr);
    const char* v = env->GetStringUTFChars(value, nullptr);
    get(obj_ptr)->setProperty(k, v);
    env->ReleaseStringUTFChars(key, k);
    env->ReleaseStringUTFChars(value, v);
}

MDK_JNI(jstring, MDKPlayer_nativeGetProperty, jstring key)
{
    if (!key)
        return nullptr;
    const char* k = env->GetStringUTFChars(key, nullptr);
    const auto value = get(obj_ptr)->property(k);
    env->ReleaseStringUTFChars(key, k);
    return env->NewStringUTF(value.c_str());
}

JNIEXPORT void JNICALL Java_com_mediadevkit_sdk_MDKPlayer_nativeSetGlobalOption(JNIEnv *env, jclass clazz, jstring key, jstring value)
{
    if (!key || !value)
        return;
    const char* k = env->GetStringUTFChars(key, nullptr);
    const char* v = env->GetStringUTFChars(value, nullptr);
    SetGlobalOption(k, v);
    env->ReleaseStringUTFChars(key, k);
    env->ReleaseStringUTFChars(value, v);
}
}
