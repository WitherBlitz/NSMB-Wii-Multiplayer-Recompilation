package com.wither.nsmbw;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * The start menu's Settings, laid out like the game's own settings window: the Video, Audio, Controls,
 * Gameplay and Language tabs along the top, one setting a row (a label and its value between arrows, a
 * checkbox bar, or a slider), a line explaining the row last touched, and OK. In landscape the rows
 * are the game's (label and value side by side); in portrait each label sits above its value, with
 * room to spare. Choices are saved to the app's preferences and written to Config.toml before every
 * start (AppFiles.writeConfig); the volumes go straight into Config.toml, where the game keeps them.
 */
public final class SettingsActivity extends Activity {
    private static final String TAG = "NSMBW";
    private static final String[] TABS = {"Video", "Audio", "Controls", "Gameplay", "Language"};
    private static final String EXTRA_TAB = "tab";

    // The game's window: cream with faint stripes, green buttons outlined in brown, yellow arrows.
    private static final int CREAM = Color.rgb(255, 250, 222), STRIPE = Color.rgb(250, 240, 200);
    private static final int FRAME = Color.rgb(196, 178, 96), INK = Color.rgb(64, 44, 14);
    private static final int GREEN_TOP = Color.rgb(240, 255, 190), GREEN_MID = Color.rgb(170, 226, 80),
            GREEN_BOTTOM = Color.rgb(120, 196, 40);
    private static final int GREY_TOP = Color.rgb(226, 230, 214), GREY_BOTTOM = Color.rgb(170, 180, 160);
    private static final int YELLOW = Color.rgb(255, 214, 40);

    private AppSettings settings;
    private int tab;
    private boolean portrait;
    private float density;
    private TextView hint;
    private LinearLayout rows;
    private ScrollView scroll;

    static void open(Activity from) {
        from.startActivity(new Intent(from, SettingsActivity.class));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Lang.load(this);
        settings = new AppSettings(this);
        density = getResources().getDisplayMetrics().density;
        tab = savedInstanceState != null ? savedInstanceState.getInt(EXTRA_TAB, 0) : 0;
        final DisplayMetrics m = getResources().getDisplayMetrics();
        portrait = m.heightPixels > m.widthPixels;
        build();
        if (!portrait) TouchEditorActivity.hideSystemBars(this);  // the game's window, full screen
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt(EXTRA_TAB, tab);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from Edit Touch Controls, or the language changed: show what's set now.
        Lang.load(this);
        build();
    }

    private void build() {
        // A change redraws the page: it stays scrolled where it was (a new tab starts at the top).
        final int keepY = scroll != null ? scroll.getScrollY() : 0;
        final FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(36, 74, 128));
        final LinearLayout window = new LinearLayout(this);
        window.setOrientation(LinearLayout.VERTICAL);
        window.setBackground(new WindowDrawable());
        final int inset = dp(portrait ? 16 : 12);
        window.setPadding(inset, inset, inset, inset);

        // The tabs: a row of three over a row of two in portrait, one row of five in landscape, as in
        // the game.
        final LinearLayout tabRows = new LinearLayout(this);
        tabRows.setOrientation(LinearLayout.VERTICAL);
        LinearLayout tabRow = null;
        for (int i = 0; i < TABS.length; ++i) {
            if (tabRow == null || (portrait && i == 3)) {
                tabRow = new LinearLayout(this);
                tabRow.setOrientation(LinearLayout.HORIZONTAL);
                tabRows.addView(tabRow, rowParams(i == 0 ? 0 : dp(8)));
            }
            final int index = i;
            final TextView button = greenButton(Lang.t(TABS[i]), i == tab, 17, dp(portrait ? 46 : 40));
            button.setOnClickListener(v -> {
                tab = index;
                pendingHint = null;
                scroll = null;
                build();
            });
            final LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(portrait ? 46 : 40), 1f);
            if (tabRow.getChildCount() > 0) p.leftMargin = dp(10);
            tabRow.addView(button, p);
        }
        window.addView(tabRows, rowParams(0));

        rows = new LinearLayout(this);
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(dp(portrait ? 4 : 24), 0, dp(portrait ? 4 : 24), 0);
        final ScrollView scroll = new ScrollView(this);
        scroll.addView(rows);
        this.scroll = scroll;
        final LinearLayout.LayoutParams scrollParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        scrollParams.topMargin = dp(portrait ? 16 : 10);
        window.addView(scroll, scrollParams);

        hint = outlined("", portrait ? 15 : 14);
        hint.setGravity(Gravity.CENTER);
        hint.setMinLines(portrait ? 2 : 1);
        window.addView(hint, rowParams(dp(8)));
        final TextView ok = greenButton(Lang.t("OK"), true, 20, dp(portrait ? 50 : 40));
        ok.setOnClickListener(v -> finish());
        final LinearLayout.LayoutParams okParams = new LinearLayout.LayoutParams(dp(portrait ? 200 : 220), dp(portrait ? 50 : 40));
        okParams.gravity = Gravity.CENTER_HORIZONTAL;
        okParams.topMargin = dp(portrait ? 8 : 4);
        window.addView(ok, okParams);

        if (tab == 0) videoTab();
        else if (tab == 1) audioTab();
        else if (tab == 2) controlsTab();
        else if (tab == 3) gameplayTab();
        else languageTab();

        final FrameLayout.LayoutParams windowParams;
        if (portrait) {
            windowParams = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
            windowParams.setMargins(dp(10), dp(28), dp(10), dp(20));
        } else {
            final DisplayMetrics m = getResources().getDisplayMetrics();
            windowParams = new FrameLayout.LayoutParams(Math.min(m.widthPixels - dp(32), dp(760)),
                    ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER_HORIZONTAL);
            windowParams.setMargins(0, dp(10), 0, dp(10));
        }
        root.addView(window, windowParams);
        setContentView(root);
        scroll.post(() -> scroll.scrollTo(0, keepY));
    }

    // --- the tabs --------------------------------------------------------------------------------

    private void videoTab() {
        // The game's Video tab, row for row (Resolution, Renderer, V-Sync, Performance Monitor with its
        // size, Spinning Coins), then the two the start menu adds.
        final float[] scales = AppSettings.RESOLUTION_SCALES;
        int scaleIndex = 3;
        for (int i = 0; i < scales.length; ++i) {
            if (Math.abs(scales[i] - settings.resolutionScale()) < 0.01f) scaleIndex = i;
        }
        final int current = scaleIndex;
        choice(Lang.t("Resolution"), AppSettings.resolutionLabel(settings.resolutionScale()),
                Lang.t("Rendered at this many lines, then scaled to the screen. Match screen: the screen's own."),
                step -> {
                    settings.setResolutionScale(scales[Math.floorMod(current + step, scales.length)]);
                    save();
                });
        final int renderer = Math.max(0, java.util.Arrays.asList(AppSettings.RENDERERS).indexOf(settings.graphicsApi()));
        final int renderers = AppSettings.OPENGLES_AVAILABLE ? 3 : 2;
        value(Lang.t("Renderer"), Lang.t(AppSettings.rendererLabel(settings.graphicsApi())),
                Lang.t("Automatic: Vulkan, else OpenGL ES. Takes effect at the next start."),
                () -> settings.setGraphicsApi(AppSettings.RENDERERS[Math.floorMod(renderer + 1, renderers)]));
        value(Lang.t("V-Sync"), Lang.t(settings.vsync() ? "On" : "Off"),
                Lang.t("No tearing: frames wait for the display, the game keeps its pace."),
                () -> settings.setVSync(!settings.vsync()));
        final String monitorHelp = Lang.t("FPS and each frame's time, top right. The slider sets its size.");
        final View monitor = toggleBar(Lang.t("Performance Monitor"), settings.performanceMonitor(), monitorHelp,
                () -> settings.setPerformanceMonitor(!settings.performanceMonitor()));
        final View size = sliderControl(settings.frameGraphSize(), 5, 200, 5, monitorHelp,
                value -> settings.setFrameGraphSize(value));
        if (portrait) {
            rows.addView(monitor, rowParams(dp(14)));
            rows.addView(size, rowParams(dp(6)));
        } else {
            // As in the game: the switch, and beside it the slider for its size.
            final LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.addView(monitor, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.55f));
            final LinearLayout.LayoutParams sizeParams = new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 0.45f);
            sizeParams.leftMargin = dp(12);
            row.addView(size, sizeParams);
            rows.addView(row, rowParams(dp(6)));
        }
        value(Lang.t("Spinning Coins"), Lang.t(settings.spinningCoins() ? "On" : "Off"),
                Lang.t("Animated coins and blocks. Off saves a little GPU work."),
                () -> settings.setSpinningCoins(!settings.spinningCoins()));
        final String[] aspects = {Lang.t("Original 4:3"), "16:9", Lang.t("Fill Screen")};
        choice(Lang.t("Aspect Ratio"), aspects[Math.max(0, Math.min(2, settings.aspectMode()))],
                Lang.t("Fill Screen: a wider view filling the whole screen. 16:9 and 4:3 add black bars."),
                step -> {
                    settings.setAspectMode(Math.floorMod(settings.aspectMode() + step, 3));
                    save();
                });
        final String[] compat = {Lang.t("Auto"), Lang.t("On"), Lang.t("Off")};
        choice(Lang.t("Graphics Compatibility"), compat[Math.max(0, Math.min(2, settings.gpuCompat()))],
                Lang.t("Fixes for phone GPUs that scramble the game's models. Auto: on for the GPUs that need them."),
                step -> {
                    settings.setGpuCompat(Math.floorMod(settings.gpuCompat() + step, 3));
                    save();
                });
        hint.setText(Lang.t("Rendered at this many lines, then scaled to the screen. Match screen: the screen's own."));
    }

    private void audioTab() {
        final String[][] volumes = {
            {"Master", "volume", "The volume of everything."},
            {"Music", "music_volume", "The background music."},
            {"Sound Effects", "sound_effects_volume", "Jumps, coins, enemies and the rest."},
        };
        for (String[] v : volumes) {
            slider(Lang.t(v[0]), Math.round(AppFiles.audioVolume(this, v[1]) * 100), 0, 100, 5, Lang.t(v[2]), value -> {
                try {
                    AppFiles.setAudioVolume(this, v[1], value / 100f);
                } catch (Exception e) {
                    Log.e(TAG, "could not save the volume", e);
                }
            });
        }
        hint.setText(Lang.t("The volume of everything."));
    }

    private void controlsTab() {
        toggle(Lang.t("Touch Controls"), settings.touchControls(),
                Lang.t("The on-screen Wii Remote. A controller hides it while you use it."),
                () -> settings.setTouchControls(!settings.touchControls()));
        slider(Lang.t("Touch Control Opacity"), Math.round(settings.touchOpacity() * 100), 20, 100, 5,
                Lang.t("How solid the on-screen remote looks."), value -> settings.setTouchOpacity(value / 100f));
        final TextView edit = greenButton(Lang.t("Edit Touch Controls"), true, portrait ? 17 : 18, rowH());
        edit.setOnClickListener(v -> startActivity(new Intent(this, TouchEditorActivity.class)));
        rows.addView(edit, rowParams(dp(portrait ? 14 : 8)));
        toggle(Lang.t("Tilt (turn the phone)"), settings.motionTilt(),
                Lang.t("Turning the phone tilts the Wii Remote, for the levels and platforms that use it."),
                () -> settings.setMotionTilt(!settings.motionTilt()));
        toggle(Lang.t("Shake (jolt the phone)"), settings.motionShake(),
                Lang.t("A jolt of the phone shakes the Wii Remote: spin jumps and picking things up."),
                () -> settings.setMotionShake(!settings.motionShake()));
        toggle(Lang.t("Vibration"), settings.haptics(),
                Lang.t("A short buzz when a touch button is pressed."),
                () -> settings.setHaptics(!settings.haptics()));
        hint.setText(Lang.t("The on-screen Wii Remote. A controller hides it while you use it."));
    }

    /** The game's Gameplay tab: its [game] keys, kept in Config.toml (AppFiles.setGameSetting). P1's
     *  character is picked on the game's file select, one a save file. */
    private void gameplayTab() {
        gameToggle("No Time Limit", "no_time_limit", "The course timer stands still: take your time.");
        gameToggle("Infinite Lives", "infinite_lives", "A miss costs no life.");
        hint.setText(Lang.t("These keep the game's own rules in LAN play."));
    }

    private void gameToggle(String label, String key, String help) {
        final boolean on = "true".equals(AppFiles.gameSetting(this, key));
        toggle(Lang.t(label), on, Lang.t(help), () -> setGameSetting(key, on ? "false" : "true"));
    }

    private void setGameSetting(String key, String literal) {
        try {
            AppFiles.setGameSetting(this, key, literal);
        } catch (Exception e) {
            Log.e(TAG, "could not save " + key, e);
        }
    }

    private void languageTab() {
        for (int i = 0; i < Lang.CODES.length; ++i) {
            final int index = i;
            final String note = Lang.onDisc(i) || AppFiles.languagePackInstalled(this, Lang.CODES[i])
                    ? Lang.t("Restart the game to play in this language.")
                    : Lang.t("The game's own text needs a language pack: the start menu's language button imports one.");
            toggle(Lang.NAMES[i], Lang.index() == i, note, () -> {
                Lang.set(this, index);
                Lang.load(this);
            });
        }
        hint.setText(Lang.t("Restart the game to play in this language."));
    }

    private void save() {
        try {
            AppFiles.writeConfig(this, settings);
        } catch (Exception e) {
            Log.e(TAG, "could not save the settings", e);
        }
    }

    // --- rows ------------------------------------------------------------------------------------

    private interface Step { void by(int step); }

    private interface Value { void set(int value); }

    /** A label, then its value between a left and a right arrow; the value itself steps forward. */
    private void choice(String label, String value, String help, Step step) {
        final LinearLayout row = labeledRow(label);
        final LinearLayout control = new LinearLayout(this);
        control.setOrientation(LinearLayout.HORIZONTAL);
        control.setGravity(Gravity.CENTER_VERTICAL);
        final View left = new ArrowView(this, false), right = new ArrowView(this, true);
        final TextView button = greenButton(value, true, portrait ? 17 : 18, rowH());
        final View.OnClickListener back = v -> { step.by(-1); hint(help); build(); };
        final View.OnClickListener forward = v -> { step.by(1); hint(help); build(); };
        left.setOnClickListener(back);
        right.setOnClickListener(forward);
        button.setOnClickListener(forward);
        control.addView(left, new LinearLayout.LayoutParams(rowH(), rowH()));
        final LinearLayout.LayoutParams middle = new LinearLayout.LayoutParams(0, rowH(), 1f);
        middle.leftMargin = middle.rightMargin = dp(8);
        control.addView(button, middle);
        control.addView(right, new LinearLayout.LayoutParams(rowH(), rowH()));
        addControl(row, control);
    }

    /** A label and, beside it, a button showing the setting; a press moves it on (the game's rows). */
    private void value(String label, String value, String help, Runnable press) {
        final LinearLayout row = labeledRow(label);
        final LinearLayout control = new LinearLayout(this);
        control.setOrientation(LinearLayout.HORIZONTAL);
        final TextView button = greenButton(value, true, portrait ? 17 : 18, rowH());
        button.setOnClickListener(v -> {
            press.run();
            save();
            hint(help);
            build();
        });
        final LinearLayout.LayoutParams middle = new LinearLayout.LayoutParams(0, rowH(), 1f);
        middle.leftMargin = middle.rightMargin = dp(8) + rowH();
        control.addView(button, middle);
        addControl(row, control);
    }

    /** The game's checkbox bar: a box, then the setting's name on a green bar. */
    private void toggle(String label, boolean on, String help, Runnable flip) {
        rows.addView(toggleBar(label, on, help, flip), rowParams(dp(portrait ? 14 : 6)));
    }

    private View toggleBar(String label, boolean on, String help, Runnable flip) {
        final FrameLayout bar = new FrameLayout(this);
        final TextView name = greenButton(label, true, portrait ? 17 : 18, rowH());
        bar.addView(name, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, rowH()));
        final View box = new CheckView(this, on);
        final int boxSize = rowH() - dp(10);
        final FrameLayout.LayoutParams boxParams = new FrameLayout.LayoutParams(boxSize, boxSize,
                Gravity.START | Gravity.CENTER_VERTICAL);
        boxParams.leftMargin = dp(8);
        bar.addView(box, boxParams);
        name.setPadding(boxSize + dp(14), 0, dp(12), 0);
        bar.setOnClickListener(v -> {
            flip.run();
            save();
            hint(help);
            build();
        });
        name.setClickable(false);
        return bar;
    }

    /** Rows are a thumb's height in portrait, and tighter in landscape so the game's Video tab fits. */
    private int rowH() { return dp(portrait ? 46 : 36); }

    /** A label, then a slider and its value. */
    private void slider(String label, int value, int min, int max, int stepSize, String help, Value set) {
        addControl(labeledRow(label), sliderControl(value, min, max, stepSize, help, set));
    }

    private View sliderControl(int value, int min, int max, int stepSize, String help, Value set) {
        final LinearLayout control = new LinearLayout(this);
        control.setOrientation(LinearLayout.HORIZONTAL);
        control.setGravity(Gravity.CENTER_VERTICAL);
        final SeekBar bar = new SeekBar(this);
        bar.setMax((max - min) / stepSize);
        bar.setProgress((Math.max(min, Math.min(max, value)) - min) / stepSize);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(GREEN_BOTTOM));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(INK));
        bar.setThumbTintList(android.content.res.ColorStateList.valueOf(YELLOW));
        final TextView shown = outlined(value + "%", portrait ? 17 : 18);
        shown.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                shown.setText((min + progress * stepSize) + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar s) { hint(help); }
            @Override public void onStopTrackingTouch(SeekBar s) {
                set.set(min + s.getProgress() * stepSize);
                save();
            }
        });
        control.addView(bar, new LinearLayout.LayoutParams(0, rowH(), 1f));
        control.addView(shown, new LinearLayout.LayoutParams(dp(72), rowH()));
        return control;
    }

    /** Landscape: label and control side by side, as in the game; portrait: the label above it. */
    private LinearLayout labeledRow(String label) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(portrait ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setGravity(portrait ? Gravity.START : Gravity.CENTER_VERTICAL);
        final TextView name = outlined(label, portrait ? 18 : 19);
        name.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        if (portrait) {
            row.addView(name, rowParams(0));
        } else {
            row.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.42f));
        }
        rows.addView(row, rowParams(dp(portrait ? 16 : 6)));
        return row;
    }

    private void addControl(LinearLayout row, View control) {
        if (portrait) {
            final LinearLayout.LayoutParams p = rowParams(dp(6));
            row.addView(control, p);
        } else {
            row.addView(control, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.58f));
        }
    }

    private String pendingHint;

    /** The explanation of the row last touched, kept across the redraw the change causes. */
    private void hint(String text) { pendingHint = text; }

    @Override
    public void setContentView(View view) {
        super.setContentView(view);
        if (pendingHint != null && hint != null) hint.setText(pendingHint);
    }

    // --- drawing ---------------------------------------------------------------------------------

    private LinearLayout.LayoutParams rowParams(int top) {
        final LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = top;
        return p;
    }

    /** White text with a dark edge, the game's lettering. */
    private TextView outlined(String value, int sp) {
        final TextView view = new TextView(this);
        view.setText(value);
        view.setTextColor(Color.WHITE);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setShadowLayer(dp(3), 0, dp(1), Color.rgb(40, 28, 8));
        return view;
    }

    /** The game's green button (grey when not picked, for tabs), its label in its lettering. */
    private TextView greenButton(String label, boolean lit, int sp, int height) {
        final TextView view = outlined(label, sp);
        view.setGravity(Gravity.CENTER);
        view.setMaxLines(2);
        view.setMinHeight(height);
        final GradientDrawable fill = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, lit
                ? new int[] {GREEN_TOP, GREEN_MID, GREEN_BOTTOM} : new int[] {GREY_TOP, GREY_BOTTOM});
        fill.setCornerRadius(dp(10));
        fill.setStroke(dp(2), lit ? INK : Color.rgb(140, 140, 120));
        view.setBackground(fill);
        if (!lit) view.setAlpha(0.8f);
        view.setClickable(true);
        return view;
    }

    private int dp(float v) { return Math.round(v * density); }

    /** The window: cream with faint horizontal stripes, a gold frame. */
    private final class WindowDrawable extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        @Override public void draw(Canvas canvas) {
            final android.graphics.Rect b = getBounds();
            final float r = dp(18);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(CREAM);
            canvas.drawRoundRect(b.left, b.top, b.right, b.bottom, r, r, paint);
            paint.setColor(STRIPE);
            canvas.save();
            final Path clip = new Path();
            clip.addRoundRect(b.left, b.top, b.right, b.bottom, r, r, Path.Direction.CW);
            canvas.clipPath(clip);
            for (float y = b.top; y < b.bottom; y += dp(8)) canvas.drawRect(b.left, y, b.right, y + dp(4), paint);
            canvas.restore();
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(6));
            paint.setColor(FRAME);
            canvas.drawRoundRect(b.left + dp(3), b.top + dp(3), b.right - dp(3), b.bottom - dp(3), r, r, paint);
        }
        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(android.graphics.ColorFilter filter) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    /** The game's yellow arrow, outlined in brown. */
    private static final class ArrowView extends View {
        private final boolean right;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        ArrowView(Context context, boolean right) {
            super(context);
            this.right = right;
            setClickable(true);
        }
        @Override protected void onDraw(Canvas canvas) {
            final float w = getWidth(), h = getHeight(), m = Math.min(w, h) * 0.18f;
            path.reset();
            if (right) {
                path.moveTo(m, m);
                path.lineTo(w - m, h / 2);
                path.lineTo(m, h - m);
            } else {
                path.moveTo(w - m, m);
                path.lineTo(m, h / 2);
                path.lineTo(w - m, h - m);
            }
            path.close();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(YELLOW);
            canvas.drawPath(path, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setStrokeWidth(Math.min(w, h) * 0.07f);
            paint.setColor(INK);
            canvas.drawPath(path, paint);
        }
    }

    /** The game's checkbox: a white rounded square, a green tick when on. */
    private static final class CheckView extends View {
        private final boolean on;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        CheckView(Context context, boolean on) {
            super(context);
            this.on = on;
        }
        @Override protected void onDraw(Canvas canvas) {
            final float s = Math.min(getWidth(), getHeight()), stroke = s * 0.09f, r = s * 0.2f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.WHITE);
            canvas.drawRoundRect(stroke, stroke, s - stroke, s - stroke, r, r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(stroke);
            paint.setColor(INK);
            canvas.drawRoundRect(stroke, stroke, s - stroke, s - stroke, r, r, paint);
            if (!on) return;
            paint.setColor(Color.rgb(40, 160, 40));
            paint.setStrokeWidth(s * 0.14f);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeJoin(Paint.Join.ROUND);
            final Path tick = new Path();
            tick.moveTo(s * 0.25f, s * 0.52f);
            tick.lineTo(s * 0.43f, s * 0.70f);
            tick.lineTo(s * 0.76f, s * 0.30f);
            canvas.drawPath(tick, paint);
        }
    }
}
