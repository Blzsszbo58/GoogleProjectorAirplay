package hu.balazs.mirrortv;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.security.SecureRandom;
import java.util.Collections;

public class MainActivity extends Activity implements NativeBridge.Listener, SurfaceHolder.Callback {

    private static final String PREFS = "settings";
    private static final String KEY_NAME = "name";
    private static final String KEY_HWADDR = "hwaddr";
    private static final String KEY_FPS = "fps";

    private SurfaceView surfaceView;
    private View idleOverlay;
    private TextView statusText;
    private TextView nameText;
    private TextView addressText;
    private Button fpsButton;

    private MdnsAdvertiser advertiser;
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;
    private HandlerThread worker;
    private Handler workerHandler;

    private boolean serverRunning = false;
    private boolean mirroring = false;
    private int videoWidth = 0, videoHeight = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surface);
        idleOverlay = findViewById(R.id.idle_overlay);
        statusText = findViewById(R.id.status);
        nameText = findViewById(R.id.device_name);
        addressText = findViewById(R.id.address);
        fpsButton = findViewById(R.id.fps_button);
        Button renameButton = findViewById(R.id.rename_button);

        surfaceView.getHolder().addCallback(this);
        renameButton.setOnClickListener(v -> showRenameDialog());
        fpsButton.setOnClickListener(v -> toggleFps());

        advertiser = new MdnsAdvertiser(this);
        NativeBridge.setAdvertiser(advertiser);

        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            int mode = Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                   : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wifi.createWifiLock(mode, "MirrorTV");
            multicastLock = wifi.createMulticastLock("MirrorTV");
            multicastLock.setReferenceCounted(false);
        }

        worker = new HandlerThread("airplay-control");
        worker.start();
        workerHandler = new Handler(worker.getLooper());

        renameButton.requestFocus();
        updateLabels();
    }

    @Override
    protected void onStart() {
        super.onStart();
        hideSystemUi();
        NativeBridge.setListener(this);
        if (wifiLock != null && !wifiLock.isHeld()) wifiLock.acquire();
        if (multicastLock != null) multicastLock.acquire();
        startServer();
    }

    @Override
    protected void onStop() {
        stopServer();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        NativeBridge.setListener(null);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        worker.quitSafely();
        NativeBridge.setAdvertiser(null);
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && mirroring) {
            // Back while mirroring: disconnect the iPhone instead of closing the app
            workerHandler.post(NativeBridge::nativeDisconnect);
            showIdle();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    // ----- server control -----

    private void startServer() {
        if (serverRunning) return;
        serverRunning = true;
        final String name = getDeviceName();
        final byte[] hw = getHwAddr();
        final String keyFile = new File(getFilesDir(), "airplay.pem").getAbsolutePath();
        DisplayMetrics dm = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        final int w = Math.max(dm.widthPixels, dm.heightPixels);
        final int h = Math.min(dm.widthPixels, dm.heightPixels);
        final int fps = getFps();
        final boolean debug = BuildConfig.DEBUG;

        statusText.setText(R.string.status_starting);
        workerHandler.post(() -> {
            int port = NativeBridge.nativeStart(name, hw, keyFile, Math.min(w, 1920), Math.min(h, 1080), fps, debug);
            runOnUiThread(() -> {
                if (port > 0) {
                    statusText.setText(R.string.status_ready);
                } else {
                    statusText.setText(getString(R.string.status_error, port));
                    serverRunning = false;
                }
            });
        });
    }

    private void stopServer() {
        if (!serverRunning) return;
        serverRunning = false;
        workerHandler.post(() -> {
            NativeBridge.nativeStop();
            advertiser.unregisterAll();
        });
        showIdle();
    }

    private void restartServer() {
        stopServer();
        startServer();
        updateLabels();
    }

    // ----- NativeBridge.Listener -----

    @Override
    public void onVideoSize(int width, int height) {
        videoWidth = width;
        videoHeight = height;
        layoutSurface();
    }

    @Override
    public void onFirstFrame() {
        mirroring = true;
        idleOverlay.setVisibility(View.GONE);
    }

    @Override
    public void onClient(String name, String model) {
        statusText.setText(getString(R.string.status_connecting, name));
    }

    @Override
    public void onConnections(int count) {
        if (count == 0) showIdle();
    }

    @Override
    public void onMirrorStopped() {
        showIdle();
    }

    private void showIdle() {
        mirroring = false;
        idleOverlay.setVisibility(View.VISIBLE);
        if (serverRunning) statusText.setText(R.string.status_ready);
    }

    // ----- SurfaceHolder.Callback -----

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeBridge.nativeSetSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // the decoder scales to whatever size the surface has
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.nativeSetSurface(null);
    }

    /** Letterbox the video inside the screen while keeping its aspect ratio. */
    private void layoutSurface() {
        View parent = (View) surfaceView.getParent();
        int pw = parent.getWidth(), ph = parent.getHeight();
        if (videoWidth <= 0 || videoHeight <= 0 || pw <= 0 || ph <= 0) return;
        float scale = Math.min((float) pw / videoWidth, (float) ph / videoHeight);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        lp.width = Math.round(videoWidth * scale);
        lp.height = Math.round(videoHeight * scale);
        surfaceView.setLayoutParams(lp);
    }

    // ----- settings -----

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private String getDeviceName() {
        return prefs().getString(KEY_NAME, getString(R.string.default_device_name));
    }

    private int getFps() {
        return prefs().getInt(KEY_FPS, 30);
    }

    private byte[] getHwAddr() {
        String stored = prefs().getString(KEY_HWADDR, null);
        if (stored != null && stored.length() == 12) {
            byte[] out = new byte[6];
            for (int i = 0; i < 6; i++) {
                out[i] = (byte) Integer.parseInt(stored.substring(2 * i, 2 * i + 2), 16);
            }
            return out;
        }
        byte[] hw = new byte[6];
        new SecureRandom().nextBytes(hw);
        hw[0] = (byte) ((hw[0] & 0xFC) | 0x02);   // locally administered, unicast
        StringBuilder sb = new StringBuilder();
        for (byte b : hw) sb.append(String.format("%02x", b & 0xff));
        prefs().edit().putString(KEY_HWADDR, sb.toString()).apply();
        return hw;
    }

    private void showRenameDialog() {
        final EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setSingleLine(true);
        input.setText(getDeviceName());
        input.setSelection(input.getText().length());
        new AlertDialog.Builder(this)
                .setTitle(R.string.rename_title)
                .setView(input)
                .setPositiveButton(android.R.string.ok, (d, which) -> {
                    String name = sanitizeName(input.getText().toString());
                    if (name.isEmpty()) {
                        Toast.makeText(this, R.string.rename_invalid, Toast.LENGTH_SHORT).show();
                        return;
                    }
                    prefs().edit().putString(KEY_NAME, name).apply();
                    restartServer();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private static String sanitizeName(String s) {
        // '@' and '.' would break the mDNS instance names
        String cleaned = s.replace('@', ' ').replace('.', ' ').trim();
        return cleaned.length() > 40 ? cleaned.substring(0, 40) : cleaned;
    }

    private void toggleFps() {
        int fps = getFps() == 30 ? 60 : 30;
        prefs().edit().putInt(KEY_FPS, fps).apply();
        restartServer();
    }

    private void updateLabels() {
        nameText.setText(getDeviceName());
        fpsButton.setText(getString(R.string.fps_button, getFps()));
        String ip = localIpAddress();
        addressText.setText(ip == null ? getString(R.string.no_network) : getString(R.string.address, ip));
    }

    private static String localIpAddress() {
        try {
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp() || nif.isLoopback()) continue;
                for (InetAddress addr : Collections.list(nif.getInetAddresses())) {
                    if (addr instanceof Inet4Address && !addr.isLoopbackAddress()) {
                        return addr.getHostAddress();
                    }
                }
            }
        } catch (Exception ignored) {
            // no network
        }
        return null;
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }
}
