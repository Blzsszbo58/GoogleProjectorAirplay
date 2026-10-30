package hu.balazs.mirrortv;

import android.os.Handler;
import android.os.Looper;
import android.view.Surface;

/**
 * Thin JNI bridge to the native AirPlay receiver (libmirrortv.so).
 * Callbacks arrive on native threads and are forwarded to the listener on the main thread.
 */
public final class NativeBridge {

    public interface Listener {
        void onVideoSize(int width, int height);
        void onFirstFrame();
        void onClient(String name, String model);
        void onConnections(int count);
        void onMirrorStopped();
    }

    static {
        System.loadLibrary("mirrortv");
    }

    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile Listener listener;
    private static volatile MdnsAdvertiser advertiser;

    private NativeBridge() {
    }

    static void setListener(Listener l) {
        listener = l;
    }

    static void setAdvertiser(MdnsAdvertiser a) {
        advertiser = a;
    }

    /** @return the TCP port on success, a negative error code otherwise. */
    static native int nativeStart(String name, byte[] hwAddr, String keyFile, int width, int height, int fps,
                                  boolean debug);

    static native void nativeStop();

    static native void nativeSetSurface(Surface surface);

    /** Drops the current client connection. */
    static native void nativeDisconnect();

    // ----- called from native code -----

    @SuppressWarnings("unused")
    static void onMdnsRegister(int service, String name, int port, String[] keys, String[] values) {
        MdnsAdvertiser a = advertiser;
        if (a != null) a.register(service, name, port, keys, values);
    }

    @SuppressWarnings("unused")
    static void onMdnsUnregister(int service) {
        MdnsAdvertiser a = advertiser;
        if (a != null) a.unregister(service);
    }

    @SuppressWarnings("unused")
    static void onVideoSize(int width, int height) {
        MAIN.post(() -> {
            Listener l = listener;
            if (l != null) l.onVideoSize(width, height);
        });
    }

    @SuppressWarnings("unused")
    static void onFirstFrame() {
        MAIN.post(() -> {
            Listener l = listener;
            if (l != null) l.onFirstFrame();
        });
    }

    @SuppressWarnings("unused")
    static void onClient(String name, String model) {
        MAIN.post(() -> {
            Listener l = listener;
            if (l != null) l.onClient(name, model);
        });
    }

    @SuppressWarnings("unused")
    static void onConnections(int count) {
        MAIN.post(() -> {
            Listener l = listener;
            if (l != null) l.onConnections(count);
        });
    }

    @SuppressWarnings("unused")
    static void onMirrorStopped() {
        MAIN.post(() -> {
            Listener l = listener;
            if (l != null) l.onMirrorStopped();
        });
    }
}
