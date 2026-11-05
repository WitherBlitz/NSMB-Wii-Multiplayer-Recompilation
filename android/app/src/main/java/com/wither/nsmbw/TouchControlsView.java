package com.wither.nsmbw;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.SparseIntArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

/**
 * The on-screen Wii Remote, laid out the way the remote is held sideways: the cross-pad under the
 * left thumb, 1 and 2 under the right one.
 *
 * <p>Every finger is tracked on its own and stays live wherever it slides:
 * <ul>
 * <li>Over the buttons, whatever is under the finger is pressed, so a thumb can roll from 1 onto 2
 * (or across A, Shake, + and -), pass through the gaps between them and pick up the next button.
 * A finger that lands on empty space presses a button as soon as it slides onto one.</li>
 * <li>1 and 2 reach a little further than the other buttons, so their areas overlap: a thumb
 * resting on the seam holds both, for running jumps.</li>
 * <li>On the cross-pad the direction follows the finger through all eight directions. A finger on
 * the pad keeps steering it even when it drifts past the edge, and a finger that slides onto the
 * pad from elsewhere takes it over.</li>
 * <li>The menu button only opens when a press both starts and ends on it, never from a slide.</li>
 * </ul>
 */
final class TouchControlsView extends View {
    /** Bits as the native side expects them (VirtualRemote::TouchButton), plus local extras. */
    static final int LEFT = 0x001, RIGHT = 0x002, UP = 0x004, DOWN = 0x008;
    static final int ONE = 0x010, TWO = 0x020, A = 0x040, PLUS = 0x080, MINUS = 0x100, B = 0x200, HOME = 0x400;
    private static final int SHAKE = 0x1000, MENU = 0x2000, CHEAT = 0x4000;

    private static final int MODE_BUTTONS = 1;  // the finger presses whatever button it is over
    private static final int MODE_PAD = 2;      // the finger steers the cross-pad until lifted
    private static final int MODE_MENU = 3;     // the press started on the menu button
    private static final int MODE_REVEAL = 4;   // the press brought the hidden remote back; it does nothing else
    private static final int MODE_CHEAT = 5;    // the press started on the Cheat button; it stays with it

    interface Listener {
        void onTouchInput(int buttons, boolean shake);
        void onMenu();
        /** The Cheat button: pressed directly, never from a thumb sliding onto it. */
        void onCheat();
        /** A tap on the game while its menus are up (menu mode), as fractions of the view. */
        void onTap(float x, float y);
        /** A finger held on the game's menus (menu mode), as fractions of the view; held is false
         *  when it is lifted. The game's sliders follow it. */
        void onPointer(float x, float y, boolean held);
        /** A finger landed while a controller has the remote hidden: whether to show it again (only
         *  while playing, never on a menu). */
        boolean canRevealControls();
    }

    private static final class Button {
        final int bit;
        final String label;
        final float reach;  // hit radius as a multiple of the drawn radius
        float x, y, r;
        Button(int bit, String label, float reach) { this.bit = bit; this.label = label; this.reach = reach; }
        boolean hit(float px, float py) {
            final float dx = px - x, dy = py - y, radius = r * reach;
            return dx * dx + dy * dy <= radius * radius;
        }
    }

    private final Button[] buttons = {
        new Button(TWO, "2", 1.4f), new Button(ONE, "1", 1.4f), new Button(A, "A", 1.25f),
        new Button(SHAKE, "SHAKE", 1.25f), new Button(MINUS, "−", 1.3f), new Button(PLUS, "+", 1.3f),
        new Button(CHEAT, "CHEAT", 1.1f), new Button(B, "B", 1.25f), new Button(HOME, "HOME", 1.3f),
    };
    private boolean cheatButton;  // the settings' Cheat Button: off, it's neither shown nor pressed
    private float padX, padY, padR;
    private final SparseIntArray pointerModes = new SparseIntArray();  // pointer id -> MODE_*
    private final SparseIntArray pointerBits = new SparseIntArray();   // pointer id -> bits it holds
    private int held;
    private Listener listener;
    private float opacity = 0.6f;
    private boolean controlsVisible = true;
    private final android.util.SparseBooleanArray freeTaps = new android.util.SparseBooleanArray();
    private boolean menuMode;  // the game's menus are up: no remote, taps go to the game
    private boolean controllerHidden;  // a controller is in use: no remote until a tap while playing
    private boolean haptics = true;
    private final android.util.SparseArray<float[]> tapStarts = new android.util.SparseArray<>();
    private int dragId = -1;  // the finger the game's menus follow (onPointer), or -1
    private final android.util.SparseLongArray tapTimes = new android.util.SparseLongArray();

    private static final int EDIT_SELECTED = Color.rgb(229, 57, 53);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arrow = new Path();
    private final RectF rect = new RectF();

    TouchControlsView(Context context) {
        super(context);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setFocusable(false);
    }

    void setListener(Listener listener) { this.listener = listener; }

    void setOpacity(float opacity) { this.opacity = opacity; invalidate(); }

    void setHaptics(boolean haptics) { this.haptics = haptics; }

    void setCheatButton(boolean on) { cheatButton = on; invalidate(); }

    /** Hidden controls still leave the menu button, so the settings stay reachable. */
    void setControlsVisible(boolean visible) {
        controlsVisible = visible;
        release();
        invalidate();
    }

    /**
     * Menu mode, while the game's own menus are up (title, file select, player count, LAN menus):
     * the remote is hidden apart from the menu button, and a tap on the game is passed on as one.
     */
    void setMenuMode(boolean on) {
        if (menuMode == on) return;
        menuMode = on;
        release();
        invalidate();
    }

    /**
     * Hidden while a controller is in use (it connected, or its buttons or sticks moved); a tap while
     * playing shows the remote again. The menu button stays either way.
     */
    void setControllerHidden(boolean hidden) {
        if (controllerHidden == hidden) return;
        controllerHidden = hidden;
        release();
        invalidate();
    }

    private boolean showsControls() { return controlsVisible && !menuMode && !controllerHidden; }

    /** Lets go of everything, e.g. when the app loses focus or a menu opens. */
    void release() {
        pointerModes.clear();
        pointerBits.clear();
        update(0);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls(w, h);
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        layoutControls(getWidth(), getHeight());
        return super.onApplyWindowInsets(insets);
    }

    private void layoutControls(int w, int h) {
        if (w == 0 || h == 0) return;
        // Keep the thumbs' controls clear of a camera cutout on either short edge.
        float left = 0, right = 0;
        final WindowInsets insets = getRootWindowInsets();
        if (android.os.Build.VERSION.SDK_INT >= 28 && insets != null && insets.getDisplayCutout() != null) {
            left = insets.getDisplayCutout().getSafeInsetLeft();
            right = insets.getDisplayCutout().getSafeInsetRight();
        }
        final float u = h;  // everything scales with the short side
        padR = 0.21f * u;
        padX = left + 0.07f * u + padR;
        padY = 0.66f * u;
        if (custom[0] != null) {
            padR *= custom[0][2];
            padX = custom[0][0] * w;
            padY = custom[0][1] * h;
        }
        place(TWO, w - right - 0.17f * u, 0.74f * u, 0.11f * u);
        place(ONE, w - right - 0.43f * u, 0.80f * u, 0.11f * u);
        place(A, w - right - 0.43f * u, 0.52f * u, 0.075f * u);
        place(SHAKE, w - right - 0.17f * u, 0.44f * u, 0.085f * u);
        place(MINUS, w / 2f - 0.13f * u, 0.08f * u, 0.05f * u);
        place(PLUS, w / 2f + 0.13f * u, 0.08f * u, 0.05f * u);
        place(MENU, w - right - 0.08f * u, 0.08f * u, 0.05f * u);
        place(CHEAT, left + 0.10f * u, 0.10f * u, 0.075f * u);  // top left, the size of A
        place(B, w - right - 0.43f * u, 0.28f * u, 0.075f * u);
        place(HOME, w / 2f, 0.08f * u, 0.05f * u);
        invalidate();
    }

    private void place(int bit, float x, float y, float r) {
        for (int i = 0; i < buttons.length; ++i) {
            final Button b = buttons[i];
            if (b.bit != bit) continue;
            final float[] c = custom[i + 1];
            b.x = c != null ? c[0] * getWidth() : x;
            b.y = c != null ? c[1] * getHeight() : y;
            b.r = c != null ? r * c[2] : r;
        }
    }

    // --- the layout editor (TouchLayoutEditor): each control moved and resized by the player --------

    /** Saved names of the movable controls: the cross-pad, then `buttons` in order. */
    private static final String[] KEYS = {"pad", "two", "one", "a", "shake", "minus", "plus", "cheat", "b", "home"};
    /** Which controls are on screen (the editor's Add and Delete); the Power-Up's is `cheatButton`. */
    private final boolean[] shown = new boolean[KEYS.length];

    private static boolean shownByDefault(String key) { return !key.equals("b") && !key.equals("home"); }

    private boolean isShown(int index) { return KEYS[index].equals("cheat") ? cheatButton : shown[index]; }

    /** The saved names of the controls not on screen, for the editor's Add. */
    java.util.List<String> hiddenControls() {
        final java.util.List<String> keys = new java.util.ArrayList<>();
        for (int i = 0; i < KEYS.length; ++i) {
            if (!isShown(i)) keys.add(KEYS[i]);
        }
        return keys;
    }

    void showControl(String key) {
        if (key.equals("cheat")) {
            cheatButton = true;
        } else {
            shown[java.util.Arrays.asList(KEYS).indexOf(key)] = true;
        }
        invalidate();
    }

    /** The editor's Delete: takes the picked control off the screen; its saved name, or null with none picked. */
    String deleteSelected() {
        if (selected < 0) return null;
        final String key = KEYS[selected];
        if (key.equals("cheat")) cheatButton = false;
        else shown[selected] = false;
        selected = -1;
        invalidate();
        return key;
    }

    /** Per control (KEYS order): {centre x, centre y as fractions of the view, size as a multiple of
     *  the default}, or null where it keeps the default place. */
    private final float[][] custom = new float[KEYS.length][];
    private boolean editing;
    private int selected = -1;
    private int editDragId = -1, editPinchId = -1;
    private float grabX, grabY, pinchStart, pinchScale;

    /** The saved layout ("pad:0.2,0.6,1.1;two:..."); empty for the default one. */
    void setLayout(String spec) {
        java.util.Arrays.fill(custom, null);
        for (int i = 0; i < KEYS.length; ++i) shown[i] = shownByDefault(KEYS[i]);
        if (spec != null) {
            for (String part : spec.split(";")) {
                if (part.startsWith("shown:")) {
                    final java.util.List<String> keys = java.util.Arrays.asList(part.substring(6).split(","));
                    for (int i = 0; i < KEYS.length; ++i) shown[i] = keys.contains(KEYS[i]);
                    continue;
                }
                final int colon = part.indexOf(':');
                final int index = colon > 0 ? java.util.Arrays.asList(KEYS).indexOf(part.substring(0, colon)) : -1;
                final String[] v = colon > 0 ? part.substring(colon + 1).split(",") : new String[0];
                if (index < 0 || v.length != 3) continue;
                try {
                    custom[index] = new float[] {clamp(Float.parseFloat(v[0]), 0f, 1f),
                            clamp(Float.parseFloat(v[1]), 0f, 1f), clamp(Float.parseFloat(v[2]), MIN_SCALE, MAX_SCALE)};
                } catch (NumberFormatException ignored) {
                    // a damaged entry keeps that control's default place
                }
            }
        }
        layoutControls(getWidth(), getHeight());
    }

    String layout() {
        final StringBuilder out = new StringBuilder();
        for (int i = 0; i < KEYS.length; ++i) {
            if (custom[i] == null) continue;
            if (out.length() > 0) out.append(';');
            out.append(String.format(java.util.Locale.ROOT, "%s:%.4f,%.4f,%.3f", KEYS[i], custom[i][0], custom[i][1], custom[i][2]));
        }
        boolean changed = false;
        final StringBuilder keys = new StringBuilder();
        for (int i = 0; i < KEYS.length; ++i) {
            changed |= shown[i] != shownByDefault(KEYS[i]);
            if (shown[i]) keys.append(keys.length() > 0 ? "," : "").append(KEYS[i]);
        }
        if (changed) out.append(out.length() > 0 ? ";" : "").append("shown:").append(keys);
        return out.toString();
    }

    private static final float MIN_SCALE = 0.5f, MAX_SCALE = 2.5f;

    /** Editing: every control shows, a finger moves the one it lands on and two fingers resize it. */
    void setEditing(boolean on) {
        editing = on;
        selected = -1;
        editDragId = editPinchId = -1;
        release();
        invalidate();
    }

    /** Bigger or Smaller: the picked control (or, with none picked, every one) by `factor`. */
    void resizeSelected(float factor) {
        for (int i = 0; i < KEYS.length; ++i) {
            if (selected < 0 || selected == i) setScale(i, scaleOf(i) * factor);
        }
    }

    void resetLayout() { setLayout(""); }

    private float scaleOf(int index) { return custom[index] != null ? custom[index][2] : 1f; }

    private float[] centreOf(int index) {
        if (index == 0) return new float[] {padX, padY};
        return new float[] {buttons[index - 1].x, buttons[index - 1].y};
    }

    private void moveTo(int index, float x, float y) {
        final float[] c = custom[index] != null ? custom[index] : new float[] {0, 0, 1f};
        c[0] = clamp(x / getWidth(), 0f, 1f);
        c[1] = clamp(y / getHeight(), 0f, 1f);
        custom[index] = c;
        layoutControls(getWidth(), getHeight());
    }

    private void setScale(int index, float scale) {
        final float[] centre = centreOf(index);
        final float[] c = custom[index] != null ? custom[index]
                : new float[] {centre[0] / getWidth(), centre[1] / getHeight(), 1f};
        c[2] = clamp(scale, MIN_SCALE, MAX_SCALE);
        custom[index] = c;
        layoutControls(getWidth(), getHeight());
    }

    /** The control nearest the point within reach of it, or -1. */
    private int controlAt(float x, float y) {
        int best = -1;
        float bestDistance = Float.MAX_VALUE;
        for (int i = 0; i < KEYS.length; ++i) {
            if (!isShown(i)) continue;
            final float[] c = centreOf(i);
            final float reach = (i == 0 ? padR : buttons[i - 1].r) * 1.2f;
            final float d = (float) Math.hypot(x - c[0], y - c[1]);
            if (d <= reach && d < bestDistance) {
                best = i;
                bestDistance = d;
            }
        }
        return best;
    }

    private void editTouch(MotionEvent e) {
        final int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            selected = controlAt(e.getX(), e.getY());
            editDragId = selected >= 0 ? e.getPointerId(0) : -1;
            if (selected >= 0) {
                final float[] c = centreOf(selected);
                grabX = c[0] - e.getX();
                grabY = c[1] - e.getY();
            }
        } else if (action == MotionEvent.ACTION_POINTER_DOWN && selected >= 0 && editPinchId < 0 && e.getPointerCount() >= 2) {
            editPinchId = e.getPointerId(e.getActionIndex());
            pinchStart = Math.max(1f, spread(e));
            pinchScale = scaleOf(selected);
        } else if (action == MotionEvent.ACTION_MOVE && selected >= 0) {
            if (editPinchId >= 0 && e.findPointerIndex(editPinchId) >= 0 && e.findPointerIndex(editDragId) >= 0) {
                setScale(selected, pinchScale * spread(e) / pinchStart);
            } else {
                final int i = e.findPointerIndex(editDragId);
                if (i >= 0) moveTo(selected, e.getX(i) + grabX, e.getY(i) + grabY);
            }
        } else if (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP
                || action == MotionEvent.ACTION_CANCEL) {
            final int id = e.getPointerId(e.getActionIndex());
            if (action != MotionEvent.ACTION_POINTER_UP || id == editDragId || id == editPinchId) {
                // A pinch ends with both fingers' drag, so the one left doesn't jerk the control.
                editDragId = editPinchId = -1;
            }
        }
        invalidate();
    }

    /** How far apart the drag and pinch fingers are. */
    private float spread(MotionEvent e) {
        final int a = e.findPointerIndex(editDragId), b = e.findPointerIndex(editPinchId);
        if (a < 0 || b < 0) return 1f;
        return (float) Math.hypot(e.getX(a) - e.getX(b), e.getY(a) - e.getY(b));
    }

    private static float clamp(float v, float lo, float hi) { return Math.max(lo, Math.min(hi, v)); }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing) {
            editTouch(e);
            return true;
        }
        final int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            endDrag(e.getX(), e.getY());
            release();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int i = e.getActionIndex();
            tapStarts.put(e.getPointerId(i), new float[] {e.getX(i), e.getY(i)});
            tapTimes.put(e.getPointerId(i), e.getEventTime());
            if (controllerHidden && controlsVisible && !menuMode && !menuAt(e.getX(i), e.getY(i))
                    && listener != null && listener.canRevealControls()) {
                setControllerHidden(false);
                pointerModes.put(e.getPointerId(i), MODE_REVEAL);
                return true;
            }
            press(e.getPointerId(i), e.getX(i), e.getY(i));
            if (menuMode && dragId < 0 && pointerModes.get(e.getPointerId(i), 0) != MODE_MENU) {
                dragId = e.getPointerId(i);
                sendPointer(e.getX(i), e.getY(i), true);
            }
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); ++i) {
                slide(e.getPointerId(i), e.getX(i), e.getY(i));
                if (e.getPointerId(i) == dragId) sendPointer(e.getX(i), e.getY(i), true);
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            final int i = e.getActionIndex();
            final int id = e.getPointerId(i);
            if (id == dragId) endDrag(e.getX(i), e.getY(i));
            final boolean openMenu = pointerModes.get(id, 0) == MODE_MENU && menuAt(e.getX(i), e.getY(i));
            final float[] start = tapStarts.get(id);
            // Taps go to the game on its menus, and anywhere off the remote's buttons otherwise (the
            // settings gear over the pause menus).
            final boolean tappable = menuMode ? pointerModes.get(id, 0) != MODE_MENU : freeTaps.get(id, false);
            if (tappable && start != null && listener != null
                    && Math.hypot(e.getX(i) - start[0], e.getY(i) - start[1]) < 0.06f * getHeight()
                    && e.getEventTime() - tapTimes.get(id) < 800 && getWidth() > 0 && getHeight() > 0) {
                listener.onTap(e.getX(i) / getWidth(), e.getY(i) / getHeight());
            }
            tapStarts.remove(id);
            tapTimes.delete(id);
            freeTaps.delete(id);
            pointerModes.delete(id);
            pointerBits.delete(id);
            if (openMenu && listener != null) {
                listener.onMenu();
            }
        }
        int combined = 0;
        for (int i = 0; i < pointerBits.size(); ++i) {
            combined |= pointerBits.valueAt(i);
        }
        update(combined);
        return true;
    }

    private void sendPointer(float x, float y, boolean held) {
        if (listener != null && getWidth() > 0 && getHeight() > 0) {
            listener.onPointer(x / getWidth(), y / getHeight(), held);
        }
    }

    private void endDrag(float x, float y) {
        if (dragId >= 0) {
            dragId = -1;
            sendPointer(x, y, false);
        }
    }

    private void press(int id, float x, float y) {
        if (menuAt(x, y)) {
            pointerModes.put(id, MODE_MENU);
            pointerBits.put(id, MENU);
        } else if (cheatAt(x, y)) {
            pointerModes.put(id, MODE_CHEAT);
            pointerBits.put(id, CHEAT);
            if (listener != null) listener.onCheat();
        } else if (onPad(x, y)) {
            pointerModes.put(id, MODE_PAD);
            pointerBits.put(id, padDirection(x, y));
        } else {
            pointerModes.put(id, MODE_BUTTONS);
            pointerBits.put(id, buttonsAt(x, y));
            if (buttonsAt(x, y) == 0) freeTaps.put(id, true);
        }
    }

    private void slide(int id, float x, float y) {
        final int mode = pointerModes.get(id, 0);
        if (mode == MODE_PAD) {
            pointerBits.put(id, padDirection(x, y));
        } else if (mode == MODE_BUTTONS) {
            if (onPad(x, y)) {
                pointerModes.put(id, MODE_PAD);
                pointerBits.put(id, padDirection(x, y));
            } else {
                pointerBits.put(id, buttonsAt(x, y));
            }
        } else if (mode == MODE_MENU) {
            pointerBits.put(id, menuAt(x, y) ? MENU : 0);
        }
    }

    private boolean onPad(float x, float y) {
        if (!showsControls() || !isShown(0)) return false;
        final float dx = x - padX, dy = y - padY, reach = padR * 1.26f;
        return dx * dx + dy * dy <= reach * reach;
    }

    private boolean menuAt(float x, float y) {
        for (Button b : buttons) {
            if (b.bit == MENU) return b.hit(x, y);
        }
        return false;
    }

    private boolean cheatAt(float x, float y) {
        if (!cheatButton || !showsControls()) return false;
        for (Button b : buttons) {
            if (b.bit == CHEAT) return b.hit(x, y);
        }
        return false;
    }

    /** Every button under the point (1 and 2 overlap on their seam); never the menu or Cheat. */
    private int buttonsAt(float x, float y) {
        if (!showsControls()) return 0;
        int bits = 0;
        for (int i = 0; i < buttons.length; ++i) {
            final Button b = buttons[i];
            if (b.bit != MENU && b.bit != CHEAT && isShown(i + 1) && b.hit(x, y)) bits |= b.bit;
        }
        return bits;
    }

    /** Eight-way: a direction counts within 67.5 degrees of its axis, so diagonals press two. */
    private int padDirection(float x, float y) {
        final float dx = x - padX, dy = y - padY;
        if (dx * dx + dy * dy < (0.22f * padR) * (0.22f * padR)) return 0;
        int bits = 0;
        final float slope = 0.4142136f;  // tan(22.5 degrees)
        if (Math.abs(dx) >= slope * Math.abs(dy)) bits |= dx < 0 ? LEFT : RIGHT;
        if (Math.abs(dy) >= slope * Math.abs(dx)) bits |= dy < 0 ? UP : DOWN;
        return bits;
    }

    private void update(int bits) {
        if (bits == held) return;
        final int pressed = bits & ~held & ~MENU;
        held = bits;
        if (pressed != 0 && haptics) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        if (listener != null) {
            listener.onTouchInput(bits & 0x7FF, (bits & SHAKE) != 0);
        }
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final int base = editing ? Math.max(170, Math.round(255 * opacity)) : Math.round(255 * opacity);
        final boolean visible = editing || showsControls();
        if (visible && isShown(0)) {
            drawPad(canvas, base);
            if (editing && selected == 0) {
                stroke.setColor(EDIT_SELECTED);
                stroke.setStrokeWidth(Math.max(4f, padR * 0.04f));
                canvas.drawCircle(padX, padY, padR * 1.08f, stroke);
            }
        }
        for (int i = 0; i < buttons.length; ++i) {
            final Button b = buttons[i];
            if (!visible && b.bit != MENU) continue;
            if (!isShown(i + 1)) continue;
            final boolean down = (held & b.bit) != 0;
            fill.setColor(Color.argb(down ? base * 3 / 4 : base / 3, 255, 255, 255));
            canvas.drawCircle(b.x, b.y, b.r, fill);
            final boolean picked = editing && selected == i + 1;
            stroke.setStrokeWidth(Math.max(picked ? 4f : 2f, b.r * (picked ? 0.1f : 0.06f)));
            stroke.setColor(picked ? EDIT_SELECTED : Color.argb(base, 255, 255, 255));
            canvas.drawCircle(b.x, b.y, b.r, stroke);
            text.setColor(Color.argb(Math.min(255, base + 60), down ? 30 : 255, down ? 30 : 255, down ? 30 : 255));
            text.setTextSize(b.label.length() > 2 ? b.r * 0.42f : b.r * 0.9f);
            canvas.drawText(b.label, b.x, b.y - (text.descent() + text.ascent()) / 2, text);
        }
    }

    private void drawPad(Canvas canvas, int base) {
        final float arm = padR * 0.36f;  // half the width of each bar
        fill.setColor(Color.argb(base / 3, 255, 255, 255));
        stroke.setStrokeWidth(Math.max(2f, padR * 0.025f));
        stroke.setColor(Color.argb(base, 255, 255, 255));
        rect.set(padX - padR, padY - arm, padX + padR, padY + arm);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, fill);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, stroke);
        rect.set(padX - arm, padY - padR, padX + arm, padY + padR);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, fill);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, stroke);
        drawArrow(canvas, base, LEFT, -1, 0);
        drawArrow(canvas, base, RIGHT, 1, 0);
        drawArrow(canvas, base, UP, 0, -1);
        drawArrow(canvas, base, DOWN, 0, 1);
    }

    private void drawArrow(Canvas canvas, int base, int bit, int ux, int uy) {
        final boolean down = (held & bit) != 0;
        final float tipX = padX + ux * padR * 0.86f, tipY = padY + uy * padR * 0.86f;
        final float backX = padX + ux * padR * 0.56f, backY = padY + uy * padR * 0.56f;
        final float half = padR * 0.16f;
        arrow.reset();
        arrow.moveTo(tipX, tipY);
        arrow.lineTo(backX - uy * half, backY - ux * half);
        arrow.lineTo(backX + uy * half, backY + ux * half);
        arrow.close();
        fill.setColor(Color.argb(down ? Math.min(255, base + 80) : base, 255, 255, 255));
        canvas.drawPath(arrow, fill);
    }
}
