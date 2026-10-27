package com.wither.nsmbw;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.os.Process;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity running libmain.so (the translated game, its runtime, SDL and Dawn,
 * linked into one library), with the on-screen remote and the phone's motion on top.
 */
public final class GameActivity extends SDLActivity
        implements TouchControlsView.Listener, MotionInput.Listener {
    private static final String TAG = "NSMBW";

    /** Player 1's on-screen remote and motion (runtime/src/android/android_bridge.cpp). */
    static native void nativeSetTouchState(int buttons, boolean shake, float tilt);
    /** The touch controls setting, for the game's Keybinds tab (which can switch it too). */
    static native void nativeSetTouchControls(boolean on);

    /** The Display menu: aspect, render scale (480 lines per 1x), FPS counter (DisplaySettings). */
    static native void nativeApplyDisplaySettings(int aspect, float renderScale, boolean performanceMonitor);

    /** "GPU name\n1|0": the GPU the game draws with and whether its compatibility fixes are on. */
    static native String nativeGraphicsInfo();

    /** What this phone is called in LAN room lists (netplay). */
    static native void nativeSetDeviceName(String name);

    /** Whether this run is a LAN session, and leaving one (the host keeps the session's save). */
    static native boolean nativeSessionActive();
    static native void nativePrepareToLeave();
    /** A tap on the game's menus, as fractions of the screen. */
    static native void nativeTap(float x, float y);
    static native void nativePointer(float x, float y, boolean held);
    /** Whether a menu that taps drive is up (the remote is hidden then). */
    static native boolean nativeTapScreen();
    /** {render scale, show FPS, aspect} after the game's own settings window changed them, else null. */
    static native float[] nativeTakeSettings();
    /**
     * The app left the screen (true) or came back (false). Leaving, the game writes a backup Quick Save
     * of the progress so far (deleted again when the app comes back) and pauses; nativeGamePaused says
     * when it has (runtime/src/host_suspend.h). With the screen just turned off (backup false) it only
     * pauses: the game waits in memory, nothing runs or is written.
     */
    static native void nativeSetBackground(boolean background, boolean backup);
    static native boolean nativeGamePaused();
    /**
     * The file select's Update button (runtime/src/updater.cpp): what this app's updater found or is
     * doing, as Updater::State (1 available, 2 downloading, 3 installing, 5 failed); progress 0..1 or -1.
     */
    static native void nativeUpdateStatus(int state, String version, float progress, String message);
    private static final int UPDATE_AVAILABLE = 1, UPDATE_DOWNLOADING = 2, UPDATE_INSTALLING = 3, UPDATE_FAILED = 5;

    /** How long leaving the app waits for the backup save and the pause, before SDL's own pause. */
    private static final long SUSPEND_WAIT_MS = 1500;

    private WifiManager.MulticastLock multicastLock;

    private AppSettings settings;
    private boolean bootWidescreen;  // the game reads 4:3 versus widescreen once, when it starts
    private TouchControlsView touch;
    private MotionInput motion;
    private boolean nativeReady;
    private int touchButtons;
    private boolean touchShake, motionShake;
    private float motionTilt;
    private Updater.Release update;            // the newer release the Update button installs
    private boolean updateWaitsForPermission;  // sent to "Allow from this source" for it
    private android.content.BroadcastReceiver updateResult;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        settings = new AppSettings(this);
        bootWidescreen = settings.aspectMode() != AppSettings.ASPECT_4_3;
        try {
            AppFiles.prepareRuntime(this);
            AppFiles.writeConfig(this, settings);
            // Read by runtime/src/platform/host_platform.cpp before anything else starts.
            Os.setenv("WIICOMPILED_RUNTIME_DIR", AppFiles.runtimeDir(this).getAbsolutePath(), true);
            Os.setenv("WIICOMPILED_DATA_DIR", AppFiles.dataDir(this).getAbsolutePath(), true);
        } catch (Exception e) {
            Log.e(TAG, "could not prepare the game's files", e);
            Toast.makeText(this, "Could not prepare the game's files: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        super.onCreate(savedInstanceState);
        nativeReady = !SDLActivity.mBrokenLibraries;
        if (nativeReady) {
            nativeSetDeviceName(deviceName());
        }
        // LAN rooms are found with broadcasts, which many Wi-Fi drivers drop unless an app holds this.
        final WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("nsmbw-lan");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        touch = new TouchControlsView(this);
        touch.setListener(this);
        applyControlSettings();
        addContentView(touch, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        motion = new MotionInput(this, this);
        if (nativeReady) {
            listenForUpdateResult();
            checkForUpdate();
        }
    }

    // --- the file select's Update button --------------------------------------------------------

    /** Once per start: a newer GitHub release shows the game's Update button. */
    private void checkForUpdate() {
        new Thread(() -> {
            try {
                final Updater.Release release = Updater.latest();
                // debug.nsmbw.update_test_version stands in for the installed version (testing).
                String installed = systemProperty("debug.nsmbw.update_test_version");
                if (installed.isEmpty()) {
                    installed = Updater.installedVersion(this);
                }
                if (release != null && Updater.isNewer(release.version, installed)) {
                    Log.i(TAG, "update: " + release.version + " is out (this is " + installed + ")");
                    runOnUiThread(() -> {
                        update = release;
                        nativeUpdateStatus(UPDATE_AVAILABLE, release.version, -1f, "");
                    });
                }
            } catch (Exception e) {
                Log.i(TAG, "update: couldn't check for a newer release: " + e.getMessage());
            }
        }, "update-check").start();
    }

    /** The Update button, pressed (called from the game thread). */
    void startGameUpdate() {
        runOnUiThread(() -> {
            final Updater.Release release = update;
            if (release == null) {
                nativeUpdateStatus(UPDATE_FAILED, "", -1f, "Update from the app's start screen instead.");
                return;
            }
            if (!Updater.canInstall(this)) {
                new AlertDialog.Builder(this)
                        .setTitle("NSMBW " + release.version + " is available")
                        .setMessage("To install updates, let NSMBW install apps: turn on Allow from this source "
                                + "on the next screen, then come back to the game.")
                        .setPositiveButton("Open Settings", (d, w) -> {
                            updateWaitsForPermission = true;
                            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                    android.net.Uri.parse("package:" + getPackageName())));
                        })
                        .setNegativeButton("Not Now",
                                (d, w) -> nativeUpdateStatus(UPDATE_AVAILABLE, release.version, -1f, ""))
                        .setCancelable(false)
                        .show();
                return;
            }
            downloadUpdate(release);
        });
    }

    /** Streams the release into the system's installer; Android replaces the app (ending this process). */
    private void downloadUpdate(Updater.Release release) {
        nativeUpdateStatus(UPDATE_DOWNLOADING, release.version, 0f, "");
        settings.setReopenGameAfterUpdate(true);
        new Thread(() -> {
            try {
                Updater.downloadAndInstall(getApplicationContext(), release, (done, total) ->
                        nativeUpdateStatus(UPDATE_DOWNLOADING, "", total > 0 ? (float) done / total : -1f, ""));
                nativeUpdateStatus(UPDATE_INSTALLING, "", -1f, "");
            } catch (Exception e) {
                settings.setReopenGameAfterUpdate(false);
                nativeUpdateStatus(UPDATE_FAILED, "", -1f, "The update failed: " + e.getMessage());
            }
        }, "update-download").start();
    }

    /** UpdateReceiver (the app's main process) says an install the button started didn't happen. */
    private void listenForUpdateResult() {
        updateResult = new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                final String message = intent.getStringExtra(UpdateReceiver.EXTRA_MESSAGE);
                if (message != null) {
                    nativeUpdateStatus(UPDATE_FAILED, "", -1f, message);
                } else if (update != null) {
                    nativeUpdateStatus(UPDATE_AVAILABLE, update.version, -1f, "");  // cancelled: offer it again
                }
            }
        };
        final android.content.IntentFilter filter =
                new android.content.IntentFilter(UpdateReceiver.ACTION_GAME_UPDATE_RESULT);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(updateResult, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(updateResult, filter);
        }
    }

    private static String systemProperty(String name) {
        try {
            final Object value = Class.forName("android.os.SystemProperties")
                    .getMethod("get", String.class).invoke(null, name);
            return value != null ? value.toString().trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    /** SDL is linked into libmain.so, so that one library is all there is to load. */
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    // The game's menus take taps and the remote hides while they are up; it comes back a moment
    // after they go, so it doesn't blink while one menu screen hands over to the next.
    private final android.os.Handler menuPoll = new android.os.Handler(android.os.Looper.getMainLooper());
    private long lastTapScreen;
    private final Runnable pollMenu = new Runnable() {
        @Override
        public void run() {
            final long now = android.os.SystemClock.uptimeMillis();
            if (nativeReady) {
                // The game's settings window: keep its choices, the config is rewritten at each start.
                final float[] changed = nativeTakeSettings();
                if (changed != null) {
                    settings.setResolutionScale(changed[0]);
                    if (changed.length > 2) settings.setAspectMode(Math.round(changed[2]));
                    if (changed.length > 3) settings.setSpinningCoins(changed[3] > 0.5f);
                    if (changed.length > 4) settings.setPerformanceMonitor(changed[4] > 0.5f);
                    if (changed.length > 5) settings.setVSync(changed[5] > 0.5f);
                    if (changed.length > 6) settings.setFrameGraphSize(Math.round(changed[6]));
                    if (changed.length > 7 && changed[7] >= 0f) {
                        settings.setTouchControls(changed[7] > 0.5f);  // the Keybinds tab's checkbox
                        applyControlSettings();
                    }
                    final int renderer = changed.length > 8 ? Math.round(changed[8]) : -1;
                    if (renderer >= 0 && renderer < AppSettings.RENDERERS.length) {
                        settings.setGraphicsApi(AppSettings.RENDERERS[renderer]);  // the Video tab's Renderer
                    }
                }
            }
            if (nativeReady && nativeTapScreen()) {
                lastTapScreen = now;
                touch.setMenuMode(true);
            } else if (now - lastTapScreen > 600) {
                touch.setMenuMode(false);
            }
            menuPoll.postDelayed(this, 100);
        }
    };

    @Override
    public void onTap(float x, float y) {
        if (nativeReady) nativeTap(x, y);
    }

    @Override
    public void onPointer(float x, float y, boolean held) {
        if (nativeReady) nativePointer(x, y, held);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (nativeReady) {
            nativeSetBackground(false, false);  // the game carries on where it stopped
        }
        menuPoll.removeCallbacks(pollMenu);
        menuPoll.post(pollMenu);
        hideSystemBars();
        // Back from "Allow from this source": carry on with the update that was waiting for it.
        if (updateWaitsForPermission && update != null) {
            updateWaitsForPermission = false;
            if (Updater.canInstall(this)) {
                downloadUpdate(update);
            } else {
                nativeUpdateStatus(UPDATE_AVAILABLE, update.version, -1f, "");
            }
        }
        if (motion != null) {
            motion.start(settings.motionTilt(), settings.motionShake());
        }
    }

    @Override
    protected void onPause() {
        if (nativeReady) {
            // Before SDL's pause stops the game thread: let the game write its backup Quick Save (in
            // case Android ends the app while it is away) and pause itself. Bounded: a save that takes
            // longer is simply not published (the NAND only commits closed files).
            // The screen turning off (sleep) only pauses: no backup Quick Save, the game waits in memory.
            final android.os.PowerManager power = (android.os.PowerManager) getSystemService(POWER_SERVICE);
            final boolean screenOn = power == null || power.isInteractive();
            nativeSetBackground(true, screenOn);
            final long until = android.os.SystemClock.uptimeMillis() + SUSPEND_WAIT_MS;
            while (!nativeGamePaused() && android.os.SystemClock.uptimeMillis() < until) {
                try {
                    Thread.sleep(10);
                } catch (InterruptedException e) {
                    break;
                }
            }
        }
        menuPoll.removeCallbacks(pollMenu);
        if (motion != null) motion.stop();
        if (touch != null) touch.release();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        } else if (touch != null) {
            touch.release();  // a dialog or the shade took the screen: let go of every button
        }
    }

    @Override
    public void onBackPressed() {
        onMenu();
    }

    /** SDL takes the Back key as keyboard input before onBackPressed: the Back menu gets it first. */
    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == android.view.KeyEvent.ACTION_UP && !event.isCanceled()) {
                onMenu();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /** "Start Menu": back to the launcher (its settings, Skip Start Menu); the game's process ends. */
    private void openStartMenu(boolean inSession) {
        if (inSession) {
            nativePrepareToLeave();  // the host keeps the session's progress
        }
        startActivity(new Intent(this, LauncherActivity.class)
                .putExtra(LauncherActivity.EXTRA_SHOW_START_MENU, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
        finish();
    }

    /** "Leave LAN Game": keep the host's progress, then start the game again on its own. */
    private void leaveSession() {
        nativePrepareToLeave();
        restartForSession();  // with the session file gone, the next start is an ordinary one
    }

    /** The name the user gave the phone, else its model. */
    private String deviceName() {
        String name = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
        if (name == null || name.trim().isEmpty()) {
            name = Build.MODEL;
        }
        return name == null ? "Phone" : name.trim();
    }

    /**
     * Network play: the runtime has written the session (netplay_start.cpp); start the game again in a
     * fresh process. The launcher waits for this one to end, then opens the game, which finds the
     * session and boots into it. Called from the game thread.
     */
    void restartForSession() {
        runOnUiThread(() -> {
            final Intent intent = new Intent(this, LauncherActivity.class)
                    .putExtra(LauncherActivity.EXTRA_RESTART_GAME, true)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            startActivity(intent);
            finish();
        });
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        if (updateResult != null) {
            unregisterReceiver(updateResult);
            updateResult = null;
        }
        super.onDestroy();
        if (isFinishing()) {
            // The runtime starts once per process; the next start must get a fresh one.
            Process.killProcess(Process.myPid());
        }
    }

    // --- input ---------------------------------------------------------------------------------

    @Override
    public void onTouchInput(int buttons, boolean shake) {
        touchButtons = buttons;
        touchShake = shake;
        pushInput();
    }

    @Override
    public void onMotion(float tilt, boolean shake) {
        motionTilt = tilt;
        motionShake = shake;
        pushInput();
    }

    private void pushInput() {
        if (!nativeReady) return;
        try {
            nativeSetTouchState(touchButtons, touchShake || motionShake, motionTilt);
        } catch (UnsatisfiedLinkError e) {
            nativeReady = false;
            Log.e(TAG, "touch input bridge missing from libmain.so", e);
        }
    }

    private void applyControlSettings() {
        touch.setControlsVisible(settings.touchControls());
        try {
            nativeSetTouchControls(settings.touchControls());  // what the game's Keybinds tab shows
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "touch controls bridge missing from libmain.so", e);
        }
        touch.setOpacity(settings.touchOpacity());
        touch.setHaptics(settings.haptics());
    }

    private void hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT < 30) {
            // Android 8-10: the immersive flags the insets controller replaced.
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            return;
        }
        getWindow().setDecorFitsSystemWindows(false);
        final WindowInsetsController controller = getWindow().getInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    // --- menu ----------------------------------------------------------------------------------

    /**
     * The phone's Back gesture: back to the start menu (where the settings are), leave a LAN game,
     * or quit. There is no on-screen menu button; the game's own settings window (the gear on the
     * save file screen and the pause menus) covers resolution and the FPS counter while playing.
     */
    @Override
    public void onMenu() {
        touch.release();
        final boolean inSession = nativeReady && nativeSessionActive();
        final java.util.List<String> items = new java.util.ArrayList<>();
        items.add("Start Menu");
        if (inSession) items.add("Leave LAN Game");
        items.add("Quit Game");
        new AlertDialog.Builder(this)
                .setTitle("New Super Mario Bros. Wii")
                .setItems(items.toArray(new String[0]), (dialog, which) -> {
                    final String item = items.get(which);
                    if (item.equals("Start Menu")) openStartMenu(inSession);
                    else if (item.equals("Leave LAN Game")) leaveSession();
                    else finish();
                })
                .setNegativeButton("Back to the Game", null)
                .show();
    }
}
