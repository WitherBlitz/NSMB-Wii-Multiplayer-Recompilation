package com.wither.nsmbw;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

/**
 * Moving and resizing the on-screen remote (TouchControlsView's editing mode), in the game and from
 * the start menu (TouchEditorActivity): a bar at the top with Smaller, Bigger, Reset and Done. Done
 * keeps the layout (AppSettings.touchLayout); the game uses it from then on.
 */
final class TouchLayoutEditor {
    private TouchLayoutEditor() {}

    static void open(Activity activity, TouchControlsView touch, AppSettings settings, Runnable done) {
        final float density = activity.getResources().getDisplayMetrics().density;
        final LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER_HORIZONTAL);
        final int pad = Math.round(10 * density);
        panel.setPadding(pad, pad, pad, pad);
        final GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(215, 16, 22, 28));
        background.setCornerRadius(18 * density);
        panel.setBackground(background);

        final TextView hint = new TextView(activity);
        hint.setText(Lang.t("Drag a control to move it. Pinch it, or tap it and use Bigger or Smaller, to resize it."));
        hint.setTextColor(Color.WHITE);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        hint.setGravity(Gravity.CENTER);
        panel.addView(hint);

        final LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        panel.addView(row);
        // Just under - and + (top centre), where the remote has nothing in its own layout.
        final FrameLayout.LayoutParams where = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        final android.util.DisplayMetrics screen = activity.getResources().getDisplayMetrics();
        where.topMargin = Math.round(0.16f * Math.min(screen.widthPixels, screen.heightPixels));

        // What Cancel goes back to.
        final String startLayout = touch.layout(), startSaved = settings.touchLayout();
        final String startActive = read(new File(profiles(activity), "active"));
        final boolean startCheat = !touch.hiddenControls().contains("cheat");
        row.addView(button(activity, Lang.t("Smaller"), v -> touch.resizeSelected(1f / 1.1f)));
        row.addView(button(activity, Lang.t("Bigger"), v -> touch.resizeSelected(1.1f)));
        row.addView(button(activity, Lang.t("Reset"), v -> {
            touch.resetLayout();
            setActiveProfile(activity, null);  // the default layout, not a profile, from now on
        }));
        final LinearLayout row2 = new LinearLayout(activity);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.setGravity(Gravity.CENTER);
        panel.addView(row2);
        row2.addView(button(activity, Lang.t("Add"), v -> askAdd(activity, touch)));
        row2.addView(button(activity, Lang.t("Delete"), v -> {
            if ("cheat".equals(touch.deleteSelected())) setCheatButton(activity, false);
        }));
        row2.addView(button(activity, Lang.t("Profiles"), v -> askProfile(activity, touch, settings)));
        row.addView(button(activity, Lang.t("Done"), v -> {
            settings.setTouchLayout(touch.layout());
            touch.setEditing(false);
            ((ViewGroup) panel.getParent()).removeView(panel);
            if (done != null) done.run();
        }));
        row.addView(button(activity, Lang.t("Cancel"), v -> {
            touch.setLayout(startLayout);
            settings.setTouchLayout(startSaved);
            setActiveProfile(activity, startActive);
            if (touch.hiddenControls().contains("cheat") == startCheat) {
                touch.setCheatButton(startCheat);
                setCheatButton(activity, startCheat);
            }
            touch.setEditing(false);
            ((ViewGroup) panel.getParent()).removeView(panel);
            if (done != null) done.run();
        }));
        // The panel is as wide as its buttons: the hint wraps to them instead of stretching it.
        final int unspecified = android.view.View.MeasureSpec.UNSPECIFIED;
        row.measure(unspecified, unspecified);
        hint.setMaxWidth(row.getMeasuredWidth());
        touch.setEditing(true);
        activity.addContentView(panel, where);
        // The panel moves with a finger on anything but its buttons, kept on the screen.
        final float[] grab = new float[2];
        panel.setOnTouchListener((view, e) -> {
            if (e.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) {
                grab[0] = view.getTranslationX() - e.getRawX();
                grab[1] = view.getTranslationY() - e.getRawY();
            } else if (e.getActionMasked() == android.view.MotionEvent.ACTION_MOVE) {
                final android.view.View parent = (android.view.View) view.getParent();
                view.setTranslationX(Math.max(-view.getLeft(),
                        Math.min(parent.getWidth() - view.getRight(), e.getRawX() + grab[0])));
                view.setTranslationY(Math.max(-view.getTop(),
                        Math.min(parent.getHeight() - view.getBottom(), e.getRawY() + grab[1])));
            }
            return true;
        });
    }

    // --- Add and Delete ----------------------------------------------------------------------------

    private static String controlName(String key) {
        switch (key) {
            case "pad": return Lang.t("D-Pad");
            case "two": return "2";
            case "one": return "1";
            case "a": return "A";
            case "b": return "B";
            case "shake": return Lang.t("Shake");
            case "minus": return "−";
            case "plus": return "+";
            case "home": return "Home";
            default: return Lang.t("Power-Up");
        }
    }

    private static void askAdd(Activity activity, TouchControlsView touch) {
        final java.util.List<String> keys = touch.hiddenControls();
        if (keys.isEmpty()) {
            new AlertDialog.Builder(activity).setMessage(Lang.t("Every button is on the screen already."))
                    .setPositiveButton(Lang.t("OK"), null).show();
            return;
        }
        final String[] names = new String[keys.size()];
        for (int i = 0; i < names.length; ++i) names[i] = controlName(keys.get(i));
        new AlertDialog.Builder(activity).setTitle(Lang.t("Add")).setItems(names, (dialog, which) -> {
            final String key = keys.get(which);
            touch.showControl(key);
            if (key.equals("cheat")) setCheatButton(activity, true);
        }).show();
    }

    private static void setCheatButton(Activity activity, boolean on) {
        try {
            AppFiles.setGameSetting(activity, "cheat_button", on ? "true" : "false");
        } catch (java.io.IOException e) {
            android.util.Log.e("NSMBW", "could not save the Power-Up button", e);
        }
        if (activity instanceof GameActivity) ((GameActivity) activity).cheatButtonChanged(on);
    }

    // --- Profiles: a layout saved as a file of its own (the app's folder in Android/data, TouchProfiles).
    // Updates keep the folder; the active profile is what the controls start from when the app has no
    // layout of its own.

    private static File profiles(Context context) {
        final File base = context.getExternalFilesDir(null);
        final File dir = new File(base != null ? base : context.getFilesDir(), "TouchProfiles");
        dir.mkdirs();
        return dir;
    }

    private static String read(File file) {
        try {
            return new String(java.nio.file.Files.readAllBytes(file.toPath()), java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (java.io.IOException e) {
            return null;
        }
    }

    private static void write(File file, String text) throws java.io.IOException {
        java.nio.file.Files.write(file.toPath(), text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    private static void setActiveProfile(Context context, String name) {
        final File active = new File(profiles(context), "active");
        try {
            if (name == null) active.delete();
            else write(active, name);
        } catch (java.io.IOException e) {
            android.util.Log.e("NSMBW", "could not save the active touch profile", e);
        }
    }

    /** The app's saved layout, or with none (as after a reinstall of its settings) the active profile's. */
    static String startingLayout(Context context, AppSettings settings) {
        final String saved = settings.touchLayout();
        if (saved != null && !saved.isEmpty()) return saved;
        final String active = read(new File(profiles(context), "active"));
        if (active == null) return saved;
        final String profile = read(new File(profiles(context), active + ".txt"));
        return profile != null ? profile : saved;
    }

    private static void askProfile(Activity activity, TouchControlsView touch, AppSettings settings) {
        final java.util.List<String> names = new java.util.ArrayList<>();
        final File[] files = profiles(activity).listFiles((dir, name) -> name.endsWith(".txt"));
        if (files != null) {
            for (File file : files) names.add(file.getName().substring(0, file.getName().length() - 4));
        }
        java.util.Collections.sort(names);
        final java.util.List<String> items = new java.util.ArrayList<>(names);
        items.add(0, Lang.t("Save as a new profile"));
        new AlertDialog.Builder(activity).setTitle(Lang.t("Profiles"))
                .setItems(items.toArray(new String[0]), (dialog, which) -> {
                    if (which == 0) {
                        askSaveProfile(activity, touch, settings);
                        return;
                    }
                    final String name = names.get(which - 1);
                    final String layout = read(new File(profiles(activity), name + ".txt"));
                    if (layout != null) {
                        touch.setLayout(layout);
                        settings.setTouchLayout(layout);
                        setActiveProfile(activity, name);
                    }
                }).show();
    }

    private static void askSaveProfile(Activity activity, TouchControlsView touch, AppSettings settings) {
        final EditText field = new EditText(activity);
        field.setSingleLine(true);
        new AlertDialog.Builder(activity).setTitle(Lang.t("Save as a new profile")).setView(field)
                .setPositiveButton(Lang.t("OK"), (dialog, which) -> {
                    final String name = field.getText().toString().trim().replaceAll("[^\\p{L}\\p{N} _-]", "");
                    if (name.isEmpty()) return;
                    final String layout = touch.layout();
                    try {
                        write(new File(profiles(activity), name + ".txt"), layout);
                        settings.setTouchLayout(layout);
                        setActiveProfile(activity, name);
                    } catch (java.io.IOException e) {
                        android.util.Log.e("NSMBW", "could not save the touch profile", e);
                    }
                })
                .setNegativeButton(Lang.t("No"), null).show();
    }

    private static Button button(Activity activity, String label, android.view.View.OnClickListener onClick) {
        final Button view = new Button(activity);
        view.setText(label);
        view.setAllCaps(false);
        view.setOnClickListener(onClick);
        return view;
    }
}
