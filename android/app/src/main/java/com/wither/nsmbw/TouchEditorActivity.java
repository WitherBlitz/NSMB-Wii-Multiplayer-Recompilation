package com.wither.nsmbw;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;

/**
 * Edit Touch Controls from the start menu: the on-screen remote over a plain background, full
 * screen and landscape like the game, so it lands where it will be when playing.
 */
public final class TouchEditorActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Lang.load(this);
        final AppSettings settings = new AppSettings(this);
        final FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(36, 74, 128));
        final TouchControlsView touch = new TouchControlsView(this);
        touch.setOpacity(settings.touchOpacity());
        touch.setLayout(settings.touchLayout());
        root.addView(touch, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);
        hideSystemBars(this);
        TouchLayoutEditor.open(this, touch, settings, this::finish);
    }

    /** Full screen, as the game is (also the start menu's Settings in landscape). */
    static void hideSystemBars(Activity activity) {
        if (android.os.Build.VERSION.SDK_INT < 30) {
            activity.getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            return;
        }
        final WindowInsetsController controller = activity.getWindow().getInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
}
