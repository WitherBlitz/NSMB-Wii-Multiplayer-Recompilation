package com.wither.nsmbw;

import android.app.Activity;
import android.app.AlertDialog;
import android.util.DisplayMetrics;
import android.util.Log;

/**
 * The app's settings (Display and Controls), on the start menu. They are saved to the app's
 * preferences and written to Config.toml before every start; the game's own settings window (the
 * gear on the save file screen and the pause menus) changes resolution, aspect and the FPS counter
 * while playing, and the app keeps those choices.
 */
final class SettingsMenu {
    private static final String TAG = "NSMBW";

    private final Activity activity;
    private final AppSettings settings;

    private SettingsMenu(Activity activity) {
        this.activity = activity;
        this.settings = new AppSettings(activity);
    }

    static void show(Activity activity) {
        new SettingsMenu(activity).showMain();
    }

    private void showMain() {
        final String[] items = {"Display", "Controls"};
        new AlertDialog.Builder(activity)
                .setTitle("Settings")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showDisplayMenu();
                    else showControlsMenu();
                })
                .setNegativeButton("Done", null)
                .show();
    }

    private void save() {
        try {
            AppFiles.writeConfig(activity, settings);
        } catch (Exception e) {
            Log.e(TAG, "could not save the settings", e);
        }
    }

    private void showDisplayMenu() {
        final String[] items = {
            "Aspect Ratio: " + aspectLabel(settings.aspectMode()),
            "Render Resolution: " + AppSettings.resolutionLabel(settings.resolutionScale()),
            "FPS Counter: " + (settings.showFps() ? "On" : "Off"),
            "Frametime Graph: " + (settings.frameGraph() ? "On" : "Off"),
            "Spinning Coins: " + (settings.spinningCoins() ? "On" : "Off"),
            "V-Sync: " + (settings.vsync() ? "On" : "Off"),
            "Graphics Compatibility: " + gpuCompatLabel(settings.gpuCompat()),
            "Renderer: " + AppSettings.rendererLabel(settings.graphicsApi()),
        };
        new AlertDialog.Builder(activity)
                .setTitle("Display")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showAspectChoice();
                    else if (which == 1) showResolutionChoice();
                    else if (which == 2) { settings.setShowFps(!settings.showFps()); save(); showDisplayMenu(); }
                    else if (which == 3) { settings.setFrameGraph(!settings.frameGraph()); save(); showDisplayMenu(); }
                    else if (which == 4) { settings.setSpinningCoins(!settings.spinningCoins()); save(); showDisplayMenu(); }
                    else if (which == 5) { settings.setVSync(!settings.vsync()); save(); showDisplayMenu(); }
                    else if (which == 6) showGpuCompatChoice();
                    else showRendererChoice();
                })
                .setNegativeButton("Back", (dialog, which) -> showMain())
                .show();
    }

    /**
     * The graphics API: Automatic uses Vulkan, or OpenGL ES 3.1+ on a phone without a working Vulkan
     * driver; either can be forced (a phone whose Vulkan driver misdraws can try OpenGL ES).
     */
    private void showRendererChoice() {
        final String[] labels = {
            "Automatic (recommended): Vulkan, else OpenGL ES",
            "Vulkan",
            "OpenGL ES 3.1+",
        };
        final int selected = java.util.Arrays.asList(AppSettings.RENDERERS).indexOf(settings.graphicsApi());
        new AlertDialog.Builder(activity)
                .setTitle("Renderer")
                .setSingleChoiceItems(labels, Math.max(0, selected), (dialog, which) -> {
                    settings.setGraphicsApi(AppSettings.RENDERERS[which]);
                    save();
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
                .show();
    }

    private static String gpuCompatLabel(int mode) {
        if (mode == AppSettings.GPU_COMPAT_ON) return "On";
        if (mode == AppSettings.GPU_COMPAT_OFF) return "Off";
        return "Auto";
    }

    /**
     * Fixes for phone GPUs whose drivers break the game's 3D models (scrambled textures, exploding
     * characters). Auto turns them on for the GPUs known to need them; On and Off force them.
     */
    private void showGpuCompatChoice() {
        final String[] labels = {
            "Auto (recommended): on for the GPUs that need it",
            "On: always use the GPU fixes",
            "Off: never use them",
        };
        new AlertDialog.Builder(activity)
                .setTitle("Graphics Compatibility")
                .setSingleChoiceItems(labels, settings.gpuCompat(), (dialog, which) -> {
                    settings.setGpuCompat(which);
                    save();
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
                .show();
    }

    private static String aspectLabel(int mode) {
        switch (mode) {
            case AppSettings.ASPECT_4_3: return "Original 4:3";
            case AppSettings.ASPECT_16_9: return "16:9";
            default: return "Fill Screen";
        }
    }

    private void showAspectChoice() {
        final String[] labels = {"Original 4:3", "16:9", "Fill Screen"};
        new AlertDialog.Builder(activity)
                .setTitle("Aspect Ratio")
                .setSingleChoiceItems(labels, settings.aspectMode(), (dialog, which) -> {
                    settings.setAspectMode(which);
                    save();
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
                .show();
    }

    /** The aspect the game renders: Fill follows the screen (landscape), never narrower than 16:9. */
    private float renderAspect() {
        switch (settings.aspectMode()) {
            case AppSettings.ASPECT_4_3: return 4f / 3f;
            case AppSettings.ASPECT_16_9: return 16f / 9f;
            default: {
                final DisplayMetrics m = activity.getResources().getDisplayMetrics();
                final int w = Math.max(m.widthPixels, m.heightPixels);
                final int h = Math.min(m.widthPixels, m.heightPixels);
                return h > 0 ? Math.max((float) w / h, 16f / 9f) : 16f / 9f;
            }
        }
    }

    private void showResolutionChoice() {
        final float[] scales = AppSettings.RESOLUTION_SCALES;
        final String[] labels = new String[scales.length];
        final float aspect = renderAspect();
        int selected = 3;
        for (int i = 0; i < scales.length; ++i) {
            if (scales[i] <= 0f) {
                final DisplayMetrics m = activity.getResources().getDisplayMetrics();
                labels[i] = AppSettings.resolutionLabel(0f) + "  ·  " + Math.max(m.widthPixels, m.heightPixels)
                        + "×" + Math.min(m.widthPixels, m.heightPixels);
                if (settings.resolutionScale() <= 0f) selected = i;
                continue;
            }
            final int lines = Math.round(480 * scales[i]);
            labels[i] = AppSettings.resolutionLabel(scales[i]) + "  ·  " + Math.round(lines * aspect) + "×" + lines;
            if (Math.abs(scales[i] - settings.resolutionScale()) < 0.01f) selected = i;
        }
        new AlertDialog.Builder(activity)
                .setTitle("Render Resolution")
                .setSingleChoiceItems(labels, selected, (dialog, which) -> {
                    settings.setResolutionScale(scales[which]);
                    save();
                    dialog.dismiss();
                    showDisplayMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showDisplayMenu())
                .show();
    }

    private void showControlsMenu() {
        final String[] items = {
            "Touch Controls: " + (settings.touchControls() ? "On" : "Off"),
            "Touch Control Opacity: " + Math.round(settings.touchOpacity() * 100) + "%",
            "Tilt (turn the phone): " + (settings.motionTilt() ? "On" : "Off"),
            "Shake (jolt the phone): " + (settings.motionShake() ? "On" : "Off"),
            "Vibration: " + (settings.haptics() ? "On" : "Off"),
        };
        new AlertDialog.Builder(activity)
                .setTitle("Controls")
                .setItems(items, (dialog, which) -> {
                    switch (which) {
                        case 0: settings.setTouchControls(!settings.touchControls()); break;
                        case 1: {
                            final float next = settings.touchOpacity() >= 0.95f ? 0.2f : settings.touchOpacity() + 0.2f;
                            settings.setTouchOpacity(Math.min(1f, next));
                            break;
                        }
                        case 2: settings.setMotionTilt(!settings.motionTilt()); break;
                        case 3: settings.setMotionShake(!settings.motionShake()); break;
                        default: settings.setHaptics(!settings.haptics()); break;
                    }
                    showControlsMenu();
                })
                .setNegativeButton("Back", (dialog, which) -> showMain())
                .show();
    }
}
