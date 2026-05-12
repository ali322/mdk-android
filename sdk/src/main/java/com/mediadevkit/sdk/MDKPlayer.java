package com.mediadevkit.sdk;

import android.opengl.GLSurfaceView;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class MDKPlayer implements SurfaceHolder.Callback {
    public static final int MEDIA_TYPE_VIDEO = 0;
    public static final int MEDIA_TYPE_AUDIO = 1;
    public static final int MEDIA_TYPE_SUBTITLE = 3;
    public static final int DECODER_MODE_SW = 0;
    public static final int DECODER_MODE_HW = 1;
    public static final int DECODER_MODE_HW_PLUS = 2;
    public static final int MEDIA_INFO_VIDEO_DECODER_SELECTED = 7101;
    public static final int MEDIA_INFO_VIDEO_FRAME_HEARTBEAT = 7102;
    public static final int SEEK_COMPLETE_SOURCE_PREPARE = 1;
    public static final int SEEK_COMPLETE_SOURCE_USER = 2;
    public static final int ASPECT_RATIO_IGNORE = 0;
    public static final int ASPECT_RATIO_KEEP = 1;
    public static final int ASPECT_RATIO_KEEP_CROP = 2;

    public interface ReleaseCallback {
        void onReleased(MDKPlayer player);
    }

    public Handler mHandler = new Handler() {
        public void handleMessage(Message msg) {
            Log.i("mdk.MDKPlayer", "handleMessage " + msg);
        }
    };

    public static final class TrackInfoSnapshot {
        public final int trackNumber;
        public final int streamIndex;
        public final String title;
        public final String language;
        public final String codec;

        public TrackInfoSnapshot(int trackNumber, int streamIndex, String title, String language, String codec) {
            this.trackNumber = trackNumber;
            this.streamIndex = streamIndex;
            this.title = title != null ? title : "";
            this.language = language != null ? language : "";
            this.codec = codec != null ? codec : "";
        }
    }

    public static final class MediaInfoSnapshot {
        public final TrackInfoSnapshot[] audio;
        public final TrackInfoSnapshot[] subtitle;

        public MediaInfoSnapshot(TrackInfoSnapshot[] audio, TrackInfoSnapshot[] subtitle) {
            this.audio = audio != null ? audio : new TrackInfoSnapshot[0];
            this.subtitle = subtitle != null ? subtitle : new TrackInfoSnapshot[0];
        }
    }

    private static void postEventFromNative(Object tgt, int what, int arg1, int arg2, Object obj) {
        Log.i("mdk.MDKPlayer", tgt + " postEventFromNative what=" + what + " arg1=" + arg1 + " arg2=" + arg2 + " obj=" + obj);
        //MDKPlayer mp = (MDKPlayer)((WeakReference<?>)tgt).get();
        MDKPlayer mp = (MDKPlayer)tgt;
        if (mp == null)
            return;
        if (mp.releaseRequested)
            return;
        if (mp.mHandler != null) {
            Message msg = mp.mHandler.obtainMessage(what, arg1, arg2, obj);
            msg.sendToTarget();
        }
    }

    public MDKPlayer() { native_ptr = nativeCreate(); }
    public void setMedia(String url) { nativeSetMedia(native_ptr, url); }
    public void setMedia(String url, int mediaType) { nativeSetMediaForType(native_ptr, url, mediaType); }
    public void setNextMedia(String url) { nativeSetNextMedia(native_ptr, url); }
    public void setPlayList(String[] urls) { nativeSetPlayList(native_ptr, urls); }
    public void prepare(long positionMs) { nativePrepare(native_ptr, positionMs); }

    //public void setPreloadNextImmediately(bool value) { nativeSetPreloadNextImmediately(native_ptr, value); }
    public void setState(int state) { nativeSetState(native_ptr, state); }
    public int state() {return nativeState(native_ptr);}
    public void resizeVideoSurface(int width, int height) { nativeResizeVideoSurface(native_ptr, width, height);}
    public void renderVideo() { nativeRenderVideo(native_ptr);}
    public void setAspectRatioMode(int mode) {
        if (releaseRequested || native_ptr == 0)
            return;
        if (mode != ASPECT_RATIO_IGNORE && mode != ASPECT_RATIO_KEEP && mode != ASPECT_RATIO_KEEP_CROP) {
            throw new IllegalArgumentException("Unsupported MDK aspect ratio mode: " + mode);
        }
        aspectRatioMode = mode;
        nativeSetAspectRatioMode(native_ptr, mode);
        attachCurrentSurfaceIfReady(sh, "aspect_ratio_changed");
    }
    public void seek(int ms) { nativeSeek(native_ptr, ms);}
    public int position() { return nativePosition(native_ptr);}
    public void setPlaybackRate(float value) { nativeSetPlaybackRate(native_ptr, value); }
    public float playbackRate() { return nativePlaybackRate(native_ptr); }
    public void setDecoderMode(int mode) {
        if (mode != DECODER_MODE_SW && mode != DECODER_MODE_HW && mode != DECODER_MODE_HW_PLUS) {
            throw new IllegalArgumentException("Unsupported MDK decoder mode: " + mode);
        }
        nativeSetDecoderMode(native_ptr, mode);
    }
    public String getConfiguredVideoDecoders() { return nativeGetConfiguredVideoDecoders(native_ptr); }

    public long getBufferedDuration() { return nativeGetBufferedDuration(native_ptr); }
    public long getBufferedBytes() { return nativeGetBufferedBytes(native_ptr); }
    public void armVideoFrameHeartbeat(long durationMs) {
        if (releaseRequested || native_ptr == 0)
            return;
        nativeArmVideoFrameHeartbeat(native_ptr, durationMs);
    }

    public int getDuration() { return nativeGetDuration(native_ptr); }
    public void setVolume(float value) { nativeSetVolume(native_ptr, value); }
    public float volume() { return nativeVolume(native_ptr); }
    public void setMute(boolean value) { nativeSetMute(native_ptr, value); }
    public boolean isMute() { return nativeIsMute(native_ptr); }
    public void setActiveTracks(int mediaType, int[] tracks) { nativeSetActiveTracks(native_ptr, mediaType, tracks); }
    public MediaInfoSnapshot getMediaInfoSnapshot() { return nativeGetMediaInfoSnapshot(native_ptr); }
    public int getVideoWidth() { return nativeGetVideoWidth(native_ptr); }
    public int getVideoHeight() { return nativeGetVideoHeight(native_ptr); }
    public float getVideoFrameRate() { return nativeGetVideoFrameRate(native_ptr); }
    public long getVideoBitRate() { return nativeGetVideoBitRate(native_ptr); }
    public String getVideoCodec() { return nativeGetVideoCodec(native_ptr); }

    public void setColorSpace(int value) { nativeSetColorSpace(native_ptr, value);}
    public void setAudioBackend(String backend) { nativeSetAudioBackends(native_ptr, backend); }
    public void setProperty(String key, String value) { nativeSetProperty(native_ptr, key, value); }
    public String getProperty(String key) { return nativeGetProperty(native_ptr, key); }
    public static void setGlobalOption(String key, String value) { nativeSetGlobalOption(key, value); }

    public void release() {
        releaseAsync(null);
    }

    public void releaseAsync(ReleaseCallback callback) {
        final long ptr = beginRelease(callback);
        if (ptr <= 0)
            return;
        Thread releaseThread = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    performRelease(ptr);
                } finally {
                    notifyReleaseCompleted();
                }
            }
        }, "mdk-player-release");
        releaseThread.start();
    }

    public boolean releaseAndWait(long timeoutMs) {
        if (Looper.getMainLooper() != null && Looper.getMainLooper().getThread() == Thread.currentThread()) {
            Log.w("mdk.MDKPlayer", "releaseAndWait must not be called on the main thread. Falling back to releaseAsync.");
            releaseAsync(null);
            return false;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final long ptr = beginRelease(new ReleaseCallback() {
            @Override
            public void onReleased(MDKPlayer player) {
                latch.countDown();
            }
        });
        if (ptr > 0) {
            try {
                performRelease(ptr);
            } finally {
                notifyReleaseCompleted();
            }
        } else if (ptr == 0L) {
            return true;
        }
        try {
            if (timeoutMs <= 0) {
                latch.await();
                return true;
            }
            return latch.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    public boolean isReleased() {
        synchronized (releaseLock) {
            return releaseCompleted;
        }
    }

    private long beginRelease(ReleaseCallback callback) {
        boolean notifyImmediately = false;
        synchronized (releaseLock) {
            if (releaseCompleted || native_ptr == 0) {
                notifyImmediately = true;
            } else {
                if (callback != null) {
                    releaseCallbacks.add(callback);
                }
                if (releaseRequested) {
                    return RELEASE_PTR_IN_PROGRESS;
                }
                releaseRequested = true;
                return native_ptr;
            }
        }
        if (notifyImmediately && callback != null) {
            callback.onReleased(this);
        }
        return 0L;
    }

    private void performRelease(long ptr) {
        mHandler = null;
        SurfaceHolder holder = sh;
        sh = null;
        if (holder != null) {
            holder.removeCallback(this);
        }
        long nativeWin = native_win;
        native_win = 0;
        if (ptr != 0 && nativeWin != 0)
            nativeSetSurface(ptr, null, nativeWin, 0, 0);
        if (ptr != 0)
            nativeRelease(ptr);
        synchronized (releaseLock) {
            native_ptr = 0;
        }
        Log.w("mdk.MDKPlayer", "release completed. player: " + ptr);
    }

    private void notifyReleaseCompleted() {
        List<ReleaseCallback> callbacks;
        synchronized (releaseLock) {
            releaseCompleted = true;
            callbacks = new ArrayList<>(releaseCallbacks);
            releaseCallbacks.clear();
        }
        for (ReleaseCallback callback : callbacks) {
            if (callback != null)
                callback.onReleased(this);
        }
    }

    protected void finalize() {
        release();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (releaseRequested || native_ptr == 0)
            return;
        native_win = setSurface(holder.getSurface(), width, height);
        Log.i("mdk.MDKPlayer", "surfaceChanged. native_win: " + native_win + " player: " + native_ptr);
    }
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (releaseRequested || native_ptr == 0)
            return;
        setSurface(null, 0, 0); // if surfaceDestroyed() was not called
        native_win = setSurface(holder.getSurface(), -1, -1);
        Log.i("mdk.MDKPlayer", Thread.currentThread() + " surface string: " + holder.getSurface().toString() + " surfaceCreated. native_win: " + native_win + " player: " + native_ptr);
    }
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i("mdk.MDKPlayer", "surfaceDestroyed. native_win: " + native_win + " player: " + native_ptr);
        if (releaseRequested || native_ptr == 0)
            return;
        if (native_win == 0)
            return;
        native_win = setSurface(null, 0, 0);
        if (holder == sh)
            sh = null;
        holder.removeCallback(this);
    }

    public void setSurfaceView(SurfaceView sv) {
        if (sv instanceof GLSurfaceView) {

        } else {
            if (sv == null)
                setSurfaceHolder(null);
            else
                setSurfaceHolder(sv.getHolder());
        }
    }
    public void setSurfaceHolder(SurfaceHolder holder) {
        if (releaseRequested || native_ptr == 0)
            return;
        if (sh == holder) {
            if (native_win == 0)
                attachCurrentSurfaceIfReady(holder, "same_holder");
            return;
        }
        if (sh != null) {
            sh.removeCallback(this);
        }
        if (native_win != 0) {
            native_win = setSurface(null, 0, 0);
            Log.i("mdk.MDKPlayer", "surfaceDetached. native_win: " + native_win + " player: " + native_ptr);
        }
        sh = holder;
        if (sh != null) {
            sh.addCallback(this);
            attachCurrentSurfaceIfReady(sh, "set_holder");
        }
    }

    private void attachCurrentSurfaceIfReady(SurfaceHolder holder, String reason) {
        if (releaseRequested || native_ptr == 0)
            return;
        if (holder == null)
            return;
        Surface surface = currentValidSurface(holder);
        if (surface == null)
            return;
        Rect frame = holder.getSurfaceFrame();
        int width = frame != null ? frame.width() : -1;
        int height = frame != null ? frame.height() : -1;
        native_win = setSurface(surface, width, height);
        nativeSetAspectRatioMode(native_ptr, aspectRatioMode);
        Log.i("mdk.MDKPlayer", "surfaceAttached. reason=" + reason + " native_win: " + native_win + " player: " + native_ptr + " size=" + width + "x" + height);
    }
    private Surface currentValidSurface(SurfaceHolder holder) {
        if (holder == null)
            return null;
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid())
            return null;
        return surface;
    }
    private long setSurface(Surface surface, int w, int h) {
        if (native_ptr == 0)
            return 0;
        return nativeSetSurface(native_ptr, surface, native_win, w, h);
    }

    private volatile long native_ptr;
    private volatile long native_win;
    private SurfaceHolder sh;
    private static final long RELEASE_PTR_IN_PROGRESS = -1L;
    private volatile boolean releaseRequested;
    private volatile boolean releaseCompleted;
    private final Object releaseLock = new Object();
    private final List<ReleaseCallback> releaseCallbacks = new ArrayList<>();
    private int aspectRatioMode = ASPECT_RATIO_KEEP;
    private native long nativeCreate();
    private native void nativeDestroy(long obj_ptr);
    private native void nativeRelease(long obj_ptr);
    private native void nativeSetMedia(long obj_ptr, String url);
    private native void nativeSetMediaForType(long obj_ptr, String url, int mediaType);
    private native void nativeSetNextMedia(long obj_ptr, String url);
    private native void nativeSetPlayList(long obj_ptr, String[] urls);
    private native void nativePrepare(long obj_ptr, long positionMs);

    //private native void nativeSetPreloadNextImmediately(long obj_ptr, boolean value);
    private native void nativeSetState(long obj_ptr, int state);
    private native int nativeState(long obj_ptr);
    private native void nativeResizeVideoSurface(long obj_ptr, int width, int height);
    private native void nativeRenderVideo(long obj_ptr);
    private native void nativeSetAspectRatioMode(long obj_ptr, int mode);
    private native void nativeSetPlaybackRate(long obj_ptr, float value);
    private native float nativePlaybackRate(long obj_ptr);
    private native void nativeSetDecoderMode(long obj_ptr, int mode);
    private native String nativeGetConfiguredVideoDecoders(long obj_ptr);

    private native long nativeSetSurface(long obj_ptr, Surface surface, long win, int w, int h);

    private native long nativeGetBufferedDuration(long obj_ptr);
    private native long nativeGetBufferedBytes(long obj_ptr);
    private native void nativeArmVideoFrameHeartbeat(long obj_ptr, long durationMs);
    private native int nativeGetDuration(long obj_ptr);
    private native int nativePosition(long obj_ptr);
    private native void nativeSeek(long obj_ptr, int msec);
    private native void nativeSetVolume(long obj_ptr, float value);
    private native float nativeVolume(long obj_ptr);
    private native void nativeSetMute(long obj_ptr, boolean value);
    private native boolean nativeIsMute(long obj_ptr);
    private native void nativeSetActiveTracks(long obj_ptr, int mediaType, int[] tracks);
    private native MediaInfoSnapshot nativeGetMediaInfoSnapshot(long obj_ptr);
    private native int nativeGetVideoWidth(long obj_ptr);
    private native int nativeGetVideoHeight(long obj_ptr);
    private native float nativeGetVideoFrameRate(long obj_ptr);
    private native long nativeGetVideoBitRate(long obj_ptr);
    private native String nativeGetVideoCodec(long obj_ptr);

    private native void nativeSetColorSpace(long obj_ptr, int value);
    private native void nativeSetAudioBackends(long obj_ptr, String backends);
    private native void nativeSetProperty(long obj_ptr, String key, String value);
    private native String nativeGetProperty(long obj_ptr, String key);
    private static native void nativeSetGlobalOption(String key, String value);

    static {
        // android 4.2 linker can not load dependencies in apk, so manually load them
        try {
            System.loadLibrary("c++_shared");
        } catch(UnsatisfiedLinkError e) {}
        try {
            System.loadLibrary("ffmpeg");
        } catch(UnsatisfiedLinkError e) {}
        System.loadLibrary("mdk");
        try {
            System.loadLibrary("mdk-avglue");
        } catch(UnsatisfiedLinkError e) {}
        System.loadLibrary("mdk-jni");
    }
}
