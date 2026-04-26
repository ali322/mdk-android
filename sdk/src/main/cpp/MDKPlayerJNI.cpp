#include <jni.h>
#include <android/log.h>
#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <set>
#include <map>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <limits>
#include "mdk/Player.h"
#include "mdk/MediaInfo.h"
#include "jmi/jmi.h"

using namespace MDK_NS;

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
        return {kHardwareVideoDecoder};
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

extern "C" {
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
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
    std::shared_ptr<std::atomic_bool> awaiting_first_video_frame = std::make_shared<std::atomic_bool>(true);
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
    // Use fixed constant for MEDIA_SEEK_COMPLETE = 4
    PostEvent(w, 4, ClampToJInt(position_ms), source);
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

static MediaType MediaTypeFromJava(jint mediaType)
{
    switch (mediaType) {
    case 1:
        return MediaType::Audio;
    case 0:
        return MediaType::Video;
    case 3:
        return MediaType::Subtitle;
    default:
        return MediaType::Unknown;
    }
}

static jstring ToJavaString(JNIEnv* env, const std::string& value)
{
    return env->NewStringUTF(value.c_str());
}

static std::string ToUtf8String(const char* value)
{
    return value ? std::string(value) : std::string();
}

static std::string TrimString(std::string value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static std::string LowercaseString(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

static void ReadMetadata(
        const std::unordered_map<std::string, std::string>& metadata,
        std::string& title,
        std::string& language,
        std::string& detail)
{
    for (const auto& entry : metadata) {
        const auto key = LowercaseString(TrimString(entry.first));
        const auto value = TrimString(entry.second);
        if (value.empty())
            continue;
        if (title.empty() && (key == "title" || key == "handler_name" || key == "name"))
            title = value;
        else if (language.empty() && (key == "language" || key == "lang"))
            language = value;
        else if (detail.empty() && (key == "display_title" || key == "comment"))
            detail = value;
    }
}

static jobject CreateTrackInfoObject(
        JNIEnv* env,
        jint index,
        jlong startTimeMs,
        jlong durationMs,
        jlong frames,
        const std::string& codec,
        const std::string& language,
        const std::string& title,
        const std::string& detail)
{
    static jclass trackInfoClass = nullptr;
    static jmethodID ctor = nullptr;
    if (!trackInfoClass) {
        auto localClass = env->FindClass("com/mediadevkit/sdk/MDKPlayer$TrackInfo");
        trackInfoClass = (jclass)env->NewGlobalRef(localClass);
        env->DeleteLocalRef(localClass);
        ctor = env->GetMethodID(
                trackInfoClass,
                "<init>",
                "(IJJJLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"
        );
    }
    auto codecString = ToJavaString(env, codec);
    auto languageString = ToJavaString(env, language);
    auto titleString = ToJavaString(env, title);
    auto detailString = ToJavaString(env, detail);
    auto result = env->NewObject(
            trackInfoClass,
            ctor,
            index,
            startTimeMs,
            durationMs,
            frames,
            codecString,
            languageString,
            titleString,
            detailString
    );
    env->DeleteLocalRef(codecString);
    env->DeleteLocalRef(languageString);
    env->DeleteLocalRef(titleString);
    env->DeleteLocalRef(detailString);
    return result;
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
        // Use fixed constant for MEDIA_ERROR = -1, MEDIA_ERROR_TIMED_OUT = -1004?
        // README didn't define MEDIA_ERROR_TIMED_OUT but MDKPlayer.java has MEDIA_ERROR = -1.
        PostEvent(w, -1, -1);
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
            // MEDIA_INFO = 0, MEDIA_INFO_BUFFERING_START = 701
            PostEvent(w, 0, 701);
        }
        if (flags_removed(oldVal, newVal, MediaStatus::Buffering)) {
            buffering_video_frame_heartbeat_enabled->store(false);
            last_buffering_video_frame_heartbeat_at_ms->store(0);
            // MEDIA_INFO = 0, MEDIA_INFO_BUFFERING_END = 702
            PostEvent(w, 0, 702);
        }
        if (flags_added(oldVal, newVal, MediaStatus::Prepared))
            // MEDIA_PREPARED = 1
            PostEvent(w, 1);
        if (flags_added(oldVal, newVal, MediaStatus::End))
            // MEDIA_PLAYBACK_COMPLETE = 2
            PostEvent(w, 2);
        return true;
    });
    p->onEvent([w, alive](const MediaEvent& e){
        if (!alive->load())
            return false;
        //__android_log_print(ANDROID_LOG_DEBUG, "MDK-JNI", "MediaEvent %d: %s %s", e.error, e.category.data(), e.detail.data());
        if (e.category == "reader.buffering") {
            // MEDIA_BUFFERING_UPDATE = 3
            PostEvent(w, 3, e.error);
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
            // MEDIA_INFO = 0, MEDIA_INFO_VIDEO_DECODER_SELECTED = 7101
            PostEvent(w, 0, 7101, e.decoder.stream, decoder_name);
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
        if (frame.width() <= 0 || frame.height() <= 0)
            return 0;

        if (awaiting_first_video_frame->load()) {
            awaiting_first_video_frame->store(false);
            // MEDIA_INFO = 0, MEDIA_INFO_VIDEO_RENDERING_START = 3
            PostEvent(w, 0, 3);
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
        // MEDIA_INFO = 0, MEDIA_INFO_VIDEO_FRAME_HEARTBEAT = 7102
        PostEvent(w, 0, 7102, track, heartbeat);
        event_env->DeleteLocalRef(heartbeat);
        return 0;
    });
    ApplyVideoDecoderMode(pr);
    putenv("GL_YUV_SAMPLER=1");
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

MDK_JNI(void, MDKPlayer_nativeSetMediaForType, jstring url, jint mediaType)
{
    if (!url) {
        get(obj_ptr)->setMedia(nullptr, MediaTypeFromJava(mediaType));
        return;
    }
    const char* s = env->GetStringUTFChars(url, nullptr);
    get(obj_ptr)->setMedia(s, MediaTypeFromJava(mediaType));
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
    url_list->pop_front();
    p->currentMediaChanged([=]{
        if (url_list->empty())
            return;
        p->setNextMedia(url_list->front().data());
        url_list->pop_front();
    });
    p->setMedia(url0.data());
}

MDK_JNI(void, MDKPlayer_nativePrepare, jlong position_ms)
{
    auto pr = ref(obj_ptr);
    if (!pr) return;
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
    if (!obj_ptr) return;
    get(obj_ptr)->set((State)state);
}

MDK_JNI(jint, MDKPlayer_nativeState)
{
    if (!obj_ptr) return 0;
    return jint(get(obj_ptr)->state());
}

MDK_JNI(void, MDKPlayer_nativeResizeVideoSurface, int width, int height)
{
    if (!obj_ptr) return;
    get(obj_ptr)->setVideoSurfaceSize(width, height);
}

MDK_JNI(void, MDKPlayer_nativeRenderVideo)
{
    if (!obj_ptr) return;
    get(obj_ptr)->renderVideo();
}

MDK_JNI(void, MDKPlayer_nativeSetPlaybackRate, jfloat value)
{
    if (!obj_ptr) return;
    get(obj_ptr)->setPlaybackRate(value);
}

MDK_JNI(jfloat, MDKPlayer_nativePlaybackRate)
{
    if (!obj_ptr) return 1.0f;
    return get(obj_ptr)->playbackRate();
}

MDK_JNI(void, MDKPlayer_nativeSetDecoderMode, jint mode)
{
    if (ThrowInvalidDecoderMode(env, mode))
        return;
    auto pr = ref(obj_ptr);
    if (!pr) return;
    pr->decoder_mode = mode;
    ApplyVideoDecoderMode(pr);
}

MDK_JNI(jstring, MDKPlayer_nativeGetConfiguredVideoDecoders)
{
    auto pr = ref(obj_ptr);
    if (!pr) return nullptr;
    return env->NewStringUTF(pr->configured_video_decoders.c_str());
}

MDK_JNI(jlong, MDKPlayer_nativeSetSurface, jobject s, jlong win, int w, int h)
{
    if (!obj_ptr)
        return 0;
    auto pr = ref(obj_ptr);
    auto p = pr->player;
    p->updateNativeSurface(s, w, h);
    pr->surface = s;
    return (jlong)s;
}

MDK_JNI(jlong, MDKPlayer_nativeGetBufferedDuration)
{
    if (!obj_ptr) return 0;
    int64_t bytes = 0;
    return (jlong)get(obj_ptr)->buffered(&bytes);
}

MDK_JNI(jlong, MDKPlayer_nativeGetBufferedBytes)
{
    if (!obj_ptr) return 0;
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

MDK_JNI(jobject, MDKPlayer_nativeGetMediaInfoSnapshot)
{
    if (!obj_ptr)
        return nullptr;
    auto p = get(obj_ptr);
    const auto& mediaInfo = p->mediaInfo();

    static jclass trackInfoClass = nullptr;
    static jclass mediaInfoSnapshotClass = nullptr;
    static jmethodID mediaInfoSnapshotCtor = nullptr;
    if (!trackInfoClass) {
        auto localTrackInfoClass = env->FindClass("com/mediadevkit/sdk/MDKPlayer$TrackInfo");
        trackInfoClass = (jclass)env->NewGlobalRef(localTrackInfoClass);
        env->DeleteLocalRef(localTrackInfoClass);
    }
    if (!mediaInfoSnapshotClass) {
        auto localSnapshotClass = env->FindClass("com/mediadevkit/sdk/MDKPlayer$MediaInfoSnapshot");
        mediaInfoSnapshotClass = (jclass)env->NewGlobalRef(localSnapshotClass);
        env->DeleteLocalRef(localSnapshotClass);
        mediaInfoSnapshotCtor = env->GetMethodID(
                mediaInfoSnapshotClass,
                "<init>",
                "(JJJ[Lcom/mediadevkit/sdk/MDKPlayer$TrackInfo;[Lcom/mediadevkit/sdk/MDKPlayer$TrackInfo;)V"
        );
    }

    const auto audioCount = (jsize)mediaInfo.audio.size();
    auto audioArray = env->NewObjectArray(audioCount, trackInfoClass, nullptr);
    for (jsize i = 0; i < audioCount; ++i) {
        const auto& streamInfo = mediaInfo.audio[(size_t)i];
        std::string title;
        std::string language;
        std::string detail;
        ReadMetadata(streamInfo.metadata, title, language, detail);
        auto trackInfo = CreateTrackInfoObject(
                env,
                streamInfo.index,
                streamInfo.start_time,
                streamInfo.duration,
                streamInfo.frames,
                TrimString(ToUtf8String(streamInfo.codec.codec)),
                language,
                title,
                detail
        );
        env->SetObjectArrayElement(audioArray, i, trackInfo);
        env->DeleteLocalRef(trackInfo);
    }

    const auto subtitleCount = (jsize)mediaInfo.subtitle.size();
    auto subtitleArray = env->NewObjectArray(subtitleCount, trackInfoClass, nullptr);
    for (jsize i = 0; i < subtitleCount; ++i) {
        const auto& streamInfo = mediaInfo.subtitle[(size_t)i];
        std::string title;
        std::string language;
        std::string detail;
        ReadMetadata(streamInfo.metadata, title, language, detail);
        auto trackInfo = CreateTrackInfoObject(
                env,
                streamInfo.index,
                streamInfo.start_time,
                streamInfo.duration,
                0,
                TrimString(ToUtf8String(streamInfo.codec.codec)),
                language,
                title,
                detail
        );
        env->SetObjectArrayElement(subtitleArray, i, trackInfo);
        env->DeleteLocalRef(trackInfo);
    }

    auto snapshot = env->NewObject(
            mediaInfoSnapshotClass,
            mediaInfoSnapshotCtor,
            (jlong)mediaInfo.start_time,
            (jlong)mediaInfo.duration,
            (jlong)mediaInfo.bit_rate,
            audioArray,
            subtitleArray
    );
    env->DeleteLocalRef(audioArray);
    env->DeleteLocalRef(subtitleArray);
    return snapshot;
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
    auto pr = ref(obj_ptr);
    auto p = pr->player;
    auto w = pr->spobj;
    p->seek(ms, [w](int64_t position){
        PostSeekComplete(w, position, SEEK_COMPLETE_SOURCE_MANUAL);
    });
}

MDK_JNI(void, MDKPlayer_nativeSetVolume, jfloat value)
{
    if (!obj_ptr) return;
    get(obj_ptr)->setVolume(value);
}

MDK_JNI(jfloat, MDKPlayer_nativeVolume)
{
    if (!obj_ptr) return 1.0f;
    return get(obj_ptr)->volume();
}

MDK_JNI(void, MDKPlayer_nativeSetMute, jboolean value)
{
    if (!obj_ptr) return;
    get(obj_ptr)->setMute(value);
}

MDK_JNI(jboolean, MDKPlayer_nativeIsMute)
{
    if (!obj_ptr) return false;
    return (jboolean)get(obj_ptr)->isMute();
}

MDK_JNI(void, MDKPlayer_nativeSetActiveTracks, jint mediaType, jintArray tracks)
{
    if (!obj_ptr)
        return;
    std::set<int> selected_tracks;
    if (tracks) {
        const jsize length = env->GetArrayLength(tracks);
        std::vector<jint> values(length);
        env->GetIntArrayRegion(tracks, 0, length, values.data());
        for (auto track : values)
            selected_tracks.insert((int)track);
    }
    get(obj_ptr)->setActiveTracks(MediaTypeFromJava(mediaType), selected_tracks);
}

MDK_JNI(jint, MDKPlayer_nativeGetVideoWidth)
{
    if (!obj_ptr) return 0;
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jint)stream->codec.width : 0;
}

MDK_JNI(jint, MDKPlayer_nativeGetVideoHeight)
{
    if (!obj_ptr) return 0;
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jint)stream->codec.height : 0;
}

MDK_JNI(jfloat, MDKPlayer_nativeGetVideoFrameRate)
{
    if (!obj_ptr) return 0.0f;
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jfloat)stream->codec.frame_rate : 0.0f;
}

MDK_JNI(jlong, MDKPlayer_nativeGetVideoBitRate)
{
    if (!obj_ptr) return 0;
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    return stream ? (jlong)stream->codec.bit_rate : 0;
}

MDK_JNI(jstring, MDKPlayer_nativeGetVideoCodec)
{
    if (!obj_ptr) return nullptr;
    const auto* stream = firstVideoStreamInfo(get(obj_ptr));
    if (!stream || !stream->codec.codec)
        return nullptr;
    return env->NewStringUTF(stream->codec.codec);
}

MDK_JNI(void, MDKPlayer_nativeSetColorSpace, jint value)
{
    if (!obj_ptr) return;
    auto r = ref(obj_ptr);
    auto p = get(obj_ptr);
    p->set(ColorSpace(value)); // store default value globally, will be used if surface is changed
    p->set(ColorSpace(value), r->surface); // apply for current surface
}

MDK_JNI(void, MDKPlayer_nativeSetAudioBackends, jstring backends)
{
    if (!obj_ptr || !backends)
        return;
    const char* s = env->GetStringUTFChars(backends, nullptr);
    get(obj_ptr)->setAudioBackends({s});
    env->ReleaseStringUTFChars(backends, s);
}

MDK_JNI(void, MDKPlayer_nativeSetProperty, jstring key, jstring value)
{
    if (!obj_ptr || !key || !value)
        return;
    const char* k = env->GetStringUTFChars(key, nullptr);
    const char* v = env->GetStringUTFChars(value, nullptr);
    get(obj_ptr)->setProperty(k, v);
    env->ReleaseStringUTFChars(key, k);
    env->ReleaseStringUTFChars(value, v);
}

MDK_JNI(jstring, MDKPlayer_nativeGetProperty, jstring key)
{
    if (!obj_ptr || !key)
        return nullptr;
    const char* k = env->GetStringUTFChars(key, nullptr);
    const auto value = get(obj_ptr)->property(k);
    env->ReleaseStringUTFChars(key, k);
    return env->NewStringUTF(value.c_str());
}

JNIEXPORT void JNICALL Java_com_mediadevkit_sdk_MDKPlayer_nativeSetGlobalOption(JNIEnv* env, jclass, jstring key, jstring value)
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
