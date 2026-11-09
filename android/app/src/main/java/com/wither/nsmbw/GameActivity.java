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
public class GameActivity extends SDLActivity
        implements TouchControlsView.Listener, MotionInput.Listener {
    private static final String TAG = "NSMBW";

    /** Player 1's on-screen remote and motion (runtime/src/android/android_bridge.cpp). */
    static native void nativeSetTouchState(int buttons, boolean shake, float tilt);
    /** The Cheat button: whether this save is marked "Cheated" already, and giving the Power-Up. */
    static native boolean nativeFileCheated();
    static native void nativeGivePowerUp(int powerUp);
    /** The game's Gameplay tab's Cheat Button. */
    static native boolean nativeCheatButton();
    static native void nativeSetCheatButton(boolean on);
    /** Whether player 1's controller has motion sensors of its own. */
    static native boolean nativeControllerMotion();
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
    /** Whether the player is in a course or on the world map with no menu up. */
    static native boolean nativePlaying();
    /** A line into the game's run log (console.log, which Download Logs includes). */
    static native void nativeLog(String line);
    /** Join's Direct Connection: the address typed into askDirectAddress's dialog. */
    static native void nativeDirectAddress(String address);
    /** {render scale, show FPS, aspect} after the game's own settings window changed them, else null. */
    static native float[] nativeTakeSettings();
    /**
     * The app left the screen (true) or came back (false). Leaving, the game saves the progress so far
     * to the player's file as a backup and pauses; nativeGamePaused says
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
        Lang.load(this);
        settings = new AppSettings(this);
        bootWidescreen = settings.aspectMode() != AppSettings.ASPECT_4_3;
        try {
            AppFiles.prepareRuntime(this);
            AppFiles.writeConfig(this, settings);
            // Read by runtime/src/platform/host_platform.cpp before anything else starts.
            Os.setenv("WIICOMPILED_RUNTIME_DIR", AppFiles.runtimeDir(this).getAbsolutePath(), true);
            Os.setenv("WIICOMPILED_DATA_DIR", AppFiles.dataDir(this).getAbsolutePath(), true);
            GpuDrivers.applyToGame(this);
        } catch (Exception e) {
            Log.e(TAG, "could not prepare the game's files", e);
            Toast.makeText(this, "Could not prepare the game's files: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        super.onCreate(savedInstanceState);
        nativeReady = !SDLActivity.mBrokenLibraries;
        if (nativeReady) {
            nativeSetDeviceName(deviceName());
        }
        final android.content.Context app = getApplicationContext();
        new android.os.Handler(getMainLooper()).postDelayed(() -> GpuDrivers.markProven(app), GpuDrivers.PROVEN_AFTER_MS);
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
        watchControllers();
        if (nativeReady) {
            listenForUpdateResult();
            checkForUpdate();
        }
    }

    // --- the file select's Update button --------------------------------------------------------

    /** When the game last looked for a newer release (SystemClock.elapsedRealtime), or -1. */
    private static long sLastUpdateCheck = -1;

    /** At start, and on coming back to the app at most once every five minutes: a newer GitHub release shows
     *  the game's Update button. */
    private void checkForUpdate() {
        sLastUpdateCheck = android.os.SystemClock.elapsedRealtime();
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
                        .setTitle(Lang.f("NSMBW %s is available", release.version))
                        .setMessage(Lang.t("To install updates, let NSMBW install apps: turn on Allow from this "
                                + "source on the next screen, then come back to the game."))
                        .setPositiveButton(Lang.t("Open Settings"), (d, w) -> {
                            updateWaitsForPermission = true;
                            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                    android.net.Uri.parse("package:" + getPackageName())));
                        })
                        .setNegativeButton(Lang.t("Not Now"),
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

    /**
     * Newer Super Mario Bros. Wii runs its own build of the same library, the downloaded module
     * (NewerModule), in place of libmain.so. Each start is a new process, so only one is ever loaded.
     */
    @Override
    public void loadLibraries() {
        if (AppFiles.newerActive(this)) {
            System.load(NewerModule.libraryToLoad(this));
        } else {
            super.loadLibraries();
        }
    }

    @Override
    protected String getMainSharedObject() {
        return AppFiles.newerActive(this) ? NewerModule.library(this).getAbsolutePath() : super.getMainSharedObject();
    }

    // The game's menus take taps and the remote hides while they are up; it comes back a moment
    // after they go, so it doesn't blink while one menu screen hands over to the next.
    private final android.os.Handler menuPoll = new android.os.Handler(android.os.Looper.getMainLooper());
    private long lastTapScreen;
    private boolean lastCheatButton;
    private final Runnable pollMenu = new Runnable() {
        @Override
        public void run() {
            final long now = android.os.SystemClock.uptimeMillis();
            if (nativeReady) {
                keepGameSettings();
                // The game's Gameplay tab's Cheat Button, when it changes (the start menu's: onResume).
                final boolean cheat = nativeCheatButton();
                if (cheat != lastCheatButton) {
                    lastCheatButton = cheat;
                    applyControlSettings();
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

    /** The game's settings window: keep its choices (the config is rewritten from them at each start). */
    private void keepGameSettings() {
        final float[] changed = nativeTakeSettings();
        if (changed == null) return;
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
        if (changed.length > 9) settings.setDepthBlur(changed[9] > 0.5f);  // the Video tab's Depth Blur
        if (changed.length > 10) settings.setBatterySaver(changed[10] > 0.5f);  // ... and its Battery Saver
        if (changed.length > 11) settings.setTextureReplacements(changed[11] > 0.5f);  // the Mods tab's HD Texture Pack
        if (changed.length > 12) settings.setFrameInterpolationFps(Math.round(changed[12]));  // ... Frame Interpolation
    }

    /**
     * The game's Mods tab downloads the HD Texture Pack (hd_texture_pack.cpp) on a thread of its own, which
     * calls this: GETs url into the file at path, following redirects. Null, or why it failed.
     */
    String downloadForGame(String url, String path) {
        java.net.HttpURLConnection connection = null;
        try {
            connection = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(30000);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("User-Agent", "NSMBW");
            if (connection.getResponseCode() != java.net.HttpURLConnection.HTTP_OK) {
                return "HTTP " + connection.getResponseCode();
            }
            try (java.io.InputStream in = connection.getInputStream();
                 java.io.OutputStream out = new java.io.FileOutputStream(path)) {
                final byte[] buffer = new byte[1 << 16];
                for (int n; (n = in.read(buffer)) > 0; ) {
                    out.write(buffer, 0, n);
                }
            }
            return null;
        } catch (Exception e) {
            Log.w("NSMBW", "download " + url, e);
            return String.valueOf(e);
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

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
        // Android often keeps the game in memory, so reopening the app comes back here without a new start.
        if (nativeReady && update == null && sLastUpdateCheck >= 0
                && android.os.SystemClock.elapsedRealtime() - sLastUpdateCheck >= 5 * 60 * 1000) {
            checkForUpdate();
        }
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
            // Before SDL's pause stops the game thread: let the game write its backup save (in
            // case Android ends the app while it is away) and pause itself. Bounded: a save that takes
            // longer is simply not published (the NAND only commits closed files).
            // The screen turning off (sleep) only pauses: no backup save, the game waits in memory.
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
        if (nativeReady) keepGameSettings();  // a change since the last poll, before the app may be ended
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

    // --- controllers ---------------------------------------------------------------------------

    /**
     * A controller hides the on-screen remote: when one connects (or is there at the start) and
     * whenever its buttons or sticks are used. A tap while playing shows it again
     * (canRevealControls); it comes back by itself when the last controller goes.
     */
    private void watchControllers() {
        final android.hardware.input.InputManager input =
                (android.hardware.input.InputManager) getSystemService(Context.INPUT_SERVICE);
        if (input == null) return;
        input.registerInputDeviceListener(new android.hardware.input.InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) {
                logInputDevice(android.view.InputDevice.getDevice(id), "connected");
                if (isController(android.view.InputDevice.getDevice(id))) touch.setControllerHidden(true);
            }
            @Override public void onInputDeviceRemoved(int id) {
                if (!anyController()) touch.setControllerHidden(false);
            }
            @Override public void onInputDeviceChanged(int id) {}
        }, new android.os.Handler(android.os.Looper.getMainLooper()));
        if (anyController()) touch.setControllerHidden(true);
        // Once the game has its run log open: every input device, for reports like issue #24.
        menuPoll.postDelayed(() -> {
            for (int id : android.view.InputDevice.getDeviceIds()) {
                logInputDevice(android.view.InputDevice.getDevice(id), "present");
            }
        }, 4000);
    }

    /** "input device present: Ipega 9087S, 2dc8:3016, sources 0x00000501, buttons A B" (not virtual ones). */
    private void logInputDevice(android.view.InputDevice device, String what) {
        if (device == null || device.isVirtual() || !nativeReady) return;
        final boolean[] keys = device.hasKeys(android.view.KeyEvent.KEYCODE_BUTTON_A,
                android.view.KeyEvent.KEYCODE_BUTTON_B, android.view.KeyEvent.KEYCODE_BUTTON_X,
                android.view.KeyEvent.KEYCODE_BUTTON_Y, android.view.KeyEvent.KEYCODE_BUTTON_START);
        final String[] names = {"A", "B", "X", "Y", "Start"};
        final StringBuilder buttons = new StringBuilder();
        for (int i = 0; i < keys.length; ++i) {
            if (keys[i]) buttons.append(' ').append(names[i]);
        }
        final String line = String.format(java.util.Locale.ROOT,
                "input device %s: %s, %04x:%04x, sources 0x%08x, keyboard type %d, buttons%s, stick %s",
                what, device.getName(), device.getVendorId(), device.getProductId(), device.getSources(),
                device.getKeyboardType(), buttons.length() > 0 ? buttons : " none",
                device.getMotionRange(android.view.MotionEvent.AXIS_X) != null ? "yes" : "no");
        try {
            nativeLog(line);
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, line);
        }
    }

    /**
     * A real controller: sticks or a joystick pad. Not the gamepad-like TV remotes some systems add
     * (NVIDIA's Shield remote service on Shield-based Android), which have no stick.
     */
    private static boolean isController(android.view.InputDevice device) {
        return device != null && !device.isVirtual()
                && (device.getSources() & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK
                && device.getMotionRange(android.view.MotionEvent.AXIS_X, android.view.InputDevice.SOURCE_JOYSTICK) != null;
    }

    private static boolean anyController() {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            if (isController(android.view.InputDevice.getDevice(id))) return true;
        }
        return false;
    }

    private static boolean fromGamepad(android.view.InputEvent event) {
        final int sources = event.getSource();
        return event.getDeviceId() >= 0
                && ((sources & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                || (sources & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK);
    }

    /**
     * The Wii Remote's rumble on the phone (player 1 with no controller), from the game thread
     * (android_bridge.cpp Vibrate). Off with the Vibration setting. 2 s at most; the game stops it.
     */
    @SuppressWarnings("deprecation")
    void vibrateForGame(boolean on) {
        if (!settings.haptics()) return;
        final android.os.Vibrator vibrator = android.os.Build.VERSION.SDK_INT >= 31
                ? ((android.os.VibratorManager) getSystemService(VIBRATOR_MANAGER_SERVICE)).getDefaultVibrator()
                : (android.os.Vibrator) getSystemService(VIBRATOR_SERVICE);
        if (vibrator == null || !vibrator.hasVibrator()) return;
        if (on) {
            // As a game's, not touch feedback: the phone's touch vibration setting doesn't mute it.
            vibrator.vibrate(android.os.VibrationEffect.createOneShot(2000, 180),
                    new android.media.AudioAttributes.Builder()
                            .setUsage(android.media.AudioAttributes.USAGE_GAME).build());
        } else {
            vibrator.cancel();
        }
    }

    @Override
    public boolean canRevealControls() {
        if (!nativeReady) return false;
        try {
            return nativePlaying();
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    /** A stick pushed well past its centre (or a trigger pulled): the controller is in use. */
    @Override
    public boolean dispatchGenericMotionEvent(android.view.MotionEvent event) {
        if (fromGamepad(event) && event.getAction() == android.view.MotionEvent.ACTION_MOVE) {
            final int[] axes = {android.view.MotionEvent.AXIS_X, android.view.MotionEvent.AXIS_Y,
                android.view.MotionEvent.AXIS_Z, android.view.MotionEvent.AXIS_RZ,
                android.view.MotionEvent.AXIS_HAT_X, android.view.MotionEvent.AXIS_HAT_Y,
                android.view.MotionEvent.AXIS_LTRIGGER, android.view.MotionEvent.AXIS_RTRIGGER};
            for (int axis : axes) {
                if (Math.abs(event.getAxisValue(axis)) > 0.5f) {
                    touch.setControllerHidden(true);
                    break;
                }
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    /** SDL takes the Back key as keyboard input before onBackPressed: the Back menu gets it first. */
    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        // Gamepad buttons from any real device (some controllers say they're a keyboard), the D-pad from
        // a gamepad.
        if (event.getAction() == android.view.KeyEvent.ACTION_DOWN && event.getDeviceId() >= 0
                && (android.view.KeyEvent.isGamepadButton(event.getKeyCode())
                || (fromGamepad(event) && event.getKeyCode() >= android.view.KeyEvent.KEYCODE_DPAD_UP
                && event.getKeyCode() <= android.view.KeyEvent.KEYCODE_DPAD_RIGHT))) {
            touch.setControllerHidden(true);
        }
        if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == android.view.KeyEvent.ACTION_UP && !event.isCanceled()) {
                onMenu();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /** The game, in the process AppSettings picks for this phone (Xiaomi Mode: the main one). */
    static Intent intent(Context context) {
        return new Intent(context, new AppSettings(context).gameInMainProcess()
                ? MainProcessGameActivity.class : GameActivity.class);
    }

    /**
     * Leaves the game for `next`. The game's process ends with it (onDestroy); when that is the app's
     * main process (Xiaomi Mode), `next` opens in a fresh one (RestartActivity).
     */
    private void leaveFor(Intent next) {
        if (this instanceof MainProcessGameActivity) {
            RestartActivity.restart(this, next);
        } else {
            startActivity(next);
        }
        finish();
    }

    /** "Start Menu": back to the launcher (its settings, Skip Start Menu); the game's process ends. */
    private void openStartMenu(boolean inSession) {
        if (inSession) {
            nativePrepareToLeave();  // the host keeps the session's progress
        }
        leaveFor(new Intent(this, LauncherActivity.class)
                .putExtra(LauncherActivity.EXTRA_SHOW_START_MENU, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
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
        runOnUiThread(() -> leaveFor(new Intent(this, LauncherActivity.class)
                .putExtra(LauncherActivity.EXTRA_RESTART_GAME, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP)));
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
            // A controller with motion sensors of its own has the remote's motion: the phone's is for
            // one without.
            final boolean phoneShake = motionShake && !nativeControllerMotion();
            nativeSetTouchState(touchButtons, touchShake || phoneShake, motionTilt);
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
        touch.setLayout(TouchLayoutEditor.startingLayout(this, settings));
        // LAN play keeps the game's own rules, so there's nothing for it to do there.
        touch.setCheatButton("true".equals(AppFiles.gameSetting(this, "cheat_button"))
                && !(nativeReady && nativeSessionActive()));
    }

    /** The Cheat button: the Power-Up picked in the settings. Using a cheat asks first, once a save. */
    @Override
    public void onCheat() {
        if (!nativeReady) return;
        final int powerUp = SettingsActivity.powerUpChoice(this);
        if (nativeFileCheated()) {
            nativeGivePowerUp(powerUp);
            return;
        }
        touch.release();
        new AlertDialog.Builder(this)
                .setTitle(Lang.t("Are you sure?"))
                .setMessage(Lang.t("This will mark your save as cheated."))
                .setPositiveButton(Lang.t("Yes"), (dialog, which) -> nativeGivePowerUp(powerUp))
                .setNegativeButton(Lang.t("No"), null)
                .show();
    }

    /** The touch editor's Add or Delete of the Power-Up button. */
    void cheatButtonChanged(boolean on) {
        if (!nativeReady) return;
        nativeSetCheatButton(on);
        lastCheatButton = on;
    }

    /** The game's Controls tab's Edit Touch Layout (android_bridge.cpp, off the UI thread). */
    void editTouchLayoutFromGame() {
        runOnUiThread(this::editTouchControls);
    }

    /** The menu's Edit Touch Controls: the game waits (as with the screen off) while the remote is edited. */
    private void editTouchControls() {
        if (nativeReady) nativeSetBackground(true, false);
        TouchLayoutEditor.open(this, touch, settings, () -> {
            if (nativeReady) nativeSetBackground(false, false);
        });
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
        if (settings.touchControls()) items.add("Edit Touch Controls");
        items.add("Quit Game");
        final String[] labels = new String[items.size()];
        for (int i = 0; i < labels.length; ++i) labels[i] = Lang.t(items.get(i));
        new AlertDialog.Builder(this)
                .setTitle(AppFiles.newerActive(this) ? "Newer Super Mario Bros. Wii" : "New Super Mario Bros. Wii")
                .setItems(labels, (dialog, which) -> {
                    final String item = items.get(which);
                    if (item.equals("Start Menu")) openStartMenu(inSession);
                    else if (item.equals("Leave LAN Game")) leaveSession();
                    else if (item.equals("Edit Touch Controls")) editTouchControls();
                    else finish();
                })
                .setNegativeButton(Lang.t("Back to the Game"), null)
                .show();
    }

    /**
     * Join's Direct Connection (called from the game thread): asks for the host's IP:Port, which the
     * host's room shows at its top. The game passes its own title and hint, in its language.
     */
    void askDirectAddress(String title, String hint) {
        runOnUiThread(() -> {
            if (touch != null) touch.release();
            final android.widget.EditText field = new android.widget.EditText(this);
            field.setSingleLine(true);
            field.setHint("192.168.1.20:52130");
            field.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_URI);
            // In landscape the keyboard would take the whole screen and hide the dialog; its Go connects.
            field.setImeOptions(android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
                    | android.view.inputmethod.EditorInfo.IME_ACTION_GO);
            final android.widget.FrameLayout box = new android.widget.FrameLayout(this);
            final int pad = (int) (20 * getResources().getDisplayMetrics().density);
            box.setPadding(pad, pad / 2, pad, 0);
            box.addView(field);
            final AlertDialog dialog = new AlertDialog.Builder(this)
                    .setTitle(title)
                    .setMessage(hint)
                    .setView(box)
                    .setPositiveButton(Lang.t("Connect"), (d, w) -> nativeDirectAddress(field.getText().toString().trim()))
                    .setNegativeButton(Lang.t("Cancel"), null)
                    .create();
            field.setOnEditorActionListener((v, action, event) -> {
                // Go, or Enter on a hardware keyboard.
                final boolean enter = event != null && event.getKeyCode() == android.view.KeyEvent.KEYCODE_ENTER
                        && event.getAction() == android.view.KeyEvent.ACTION_DOWN;
                if (action != android.view.inputmethod.EditorInfo.IME_ACTION_GO && !enter) return false;
                nativeDirectAddress(field.getText().toString().trim());
                dialog.dismiss();
                return true;
            });
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
            dialog.show();
            field.requestFocus();
        });
    }
}
