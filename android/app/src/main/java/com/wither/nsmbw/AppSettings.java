package com.wither.nsmbw;

import android.content.Context;
import android.content.SharedPreferences;

/** Player settings, kept in one SharedPreferences file shared by the launcher and the game. */
final class AppSettings {
    static final int ASPECT_4_3 = 0;
    static final int ASPECT_16_9 = 1;
    static final int ASPECT_FILL = 2;

    /** GPU driver workarounds: picked from the phone's GPU, or forced (AuroraGpuCompat). */
    static final int GPU_COMPAT_AUTO = 0;
    static final int GPU_COMPAT_ON = 1;
    static final int GPU_COMPAT_OFF = 2;

    /** Render scales offered in the menu; 1x renders 480 lines, whatever the aspect; 0 the screen's own. */
    static final float[] RESOLUTION_SCALES = {0f, 0.5f, 0.75f, 1f, 1.5f, 2f, 3f, 4f};

    private static final String FILE = "settings";
    private final SharedPreferences prefs;

    AppSettings(Context context) {
        // Both processes (launcher and :game) read it. Writes use commit(): the game process is
        // ended right after its menu closes, which would drop a pending apply().
        prefs = context.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    int aspectMode() { return prefs.getInt("aspect_mode", ASPECT_FILL); }
    void setAspectMode(int mode) { prefs.edit().putInt("aspect_mode", mode).commit(); }

    float resolutionScale() { return prefs.getFloat("resolution_scale", 1f); }
    void setResolutionScale(float scale) { prefs.edit().putFloat("resolution_scale", scale).commit(); }

    int gpuCompat() { return prefs.getInt("gpu_compat", GPU_COMPAT_AUTO); }
    void setGpuCompat(int mode) { prefs.edit().putInt("gpu_compat", mode).commit(); }

    /** Auto Update: opening the app installs the latest GitHub release (off by default). */
    boolean autoUpdate() { return prefs.getBoolean("auto_update", false); }
    void setAutoUpdate(boolean on) { prefs.edit().putBoolean("auto_update", on).commit(); }

    boolean spinningCoins() { return prefs.getBoolean("spinning_coins", true); }
    void setSpinningCoins(boolean on) { prefs.edit().putBoolean("spinning_coins", on).commit(); }

    /** The Performance Monitor: FPS counter and frametime graph (the old FPS Counter switch turns it on). */
    boolean performanceMonitor() {
        return prefs.getBoolean("frame_graph", false) || prefs.getBoolean("show_fps", false);
    }
    void setPerformanceMonitor(boolean on) {
        prefs.edit().putBoolean("frame_graph", on).putBoolean("show_fps", false).commit();
    }

    /** V-Sync: frames go out in step with the display and the game's clock follows its vsync. */
    boolean vsync() { return prefs.getBoolean("vsync", true); }

    /** The frametime graph's size in percent (5-200, 50 its original size), set in the game's Video tab. */
    int frameGraphSize() { return prefs.getInt("frame_graph_size", 50); }
    void setFrameGraphSize(int percent) { prefs.edit().putInt("frame_graph_size", percent).commit(); }
    void setVSync(boolean on) { prefs.edit().putBoolean("vsync", on).commit(); }

    /** The renderer, as Config.toml's [video] graphics_api: "auto" (Vulkan, else OpenGL ES), "vulkan"
     *  or "opengles". The game's Video tab and the Display menu both set it; it applies at the next start. */
    static final String[] RENDERERS = {"auto", "vulkan", "opengles"};
    static final String[] RENDERER_LABELS = {"Automatic", "Vulkan", "OpenGL ES"};
    String graphicsApi() {
        final String value = prefs.getString("graphics_api", "auto");
        return java.util.Arrays.asList(RENDERERS).contains(value) ? value : "auto";
    }
    void setGraphicsApi(String value) { prefs.edit().putString("graphics_api", value).commit(); }
    static String rendererLabel(String value) {
        final int index = java.util.Arrays.asList(RENDERERS).indexOf(value);
        return RENDERER_LABELS[Math.max(0, index)];
    }

    boolean touchControls() { return prefs.getBoolean("touch_controls", true); }
    void setTouchControls(boolean on) { prefs.edit().putBoolean("touch_controls", on).commit(); }

    float touchOpacity() { return prefs.getFloat("touch_opacity", 0.6f); }
    void setTouchOpacity(float opacity) { prefs.edit().putFloat("touch_opacity", opacity).commit(); }

    boolean motionTilt() { return prefs.getBoolean("motion_tilt", true); }
    void setMotionTilt(boolean on) { prefs.edit().putBoolean("motion_tilt", on).commit(); }

    boolean motionShake() { return prefs.getBoolean("motion_shake", true); }
    void setMotionShake(boolean on) { prefs.edit().putBoolean("motion_shake", on).commit(); }

    boolean haptics() { return prefs.getBoolean("haptics", true); }
    void setHaptics(boolean on) { prefs.edit().putBoolean("haptics", on).commit(); }

    /** Open the game straight away when the app starts; the in-game menu leads back here. */
    boolean skipStartMenu() { return prefs.getBoolean("skip_start_menu", false); }
    void setSkipStartMenu(boolean on) { prefs.edit().putBoolean("skip_start_menu", on).commit(); }

    /** "2× (960p)": the scale and the number of lines it renders, as the menu shows it. */
    static String resolutionLabel(float scale) {
        if (scale <= 0f) return "Match screen";
        final String factor = scale == Math.rint(scale)
                ? String.valueOf((int) scale) : String.valueOf(scale);
        return factor + "× (" + Math.round(480 * scale) + "p)";
    }
}
