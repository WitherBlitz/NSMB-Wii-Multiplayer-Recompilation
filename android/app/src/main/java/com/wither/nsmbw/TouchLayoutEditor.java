package com.wither.nsmbw;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

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

        row.addView(button(activity, Lang.t("Smaller"), v -> touch.resizeSelected(1f / 1.1f)));
        row.addView(button(activity, Lang.t("Bigger"), v -> touch.resizeSelected(1.1f)));
        row.addView(button(activity, Lang.t("Reset"), v -> touch.resetLayout()));
        row.addView(button(activity, Lang.t("Done"), v -> {
            settings.setTouchLayout(touch.layout());
            touch.setEditing(false);
            ((ViewGroup) panel.getParent()).removeView(panel);
            if (done != null) done.run();
        }));
        touch.setEditing(true);
        activity.addContentView(panel, where);
    }

    private static Button button(Activity activity, String label, android.view.View.OnClickListener onClick) {
        final Button view = new Button(activity);
        view.setText(label);
        view.setAllCaps(false);
        view.setOnClickListener(onClick);
        return view;
    }
}
