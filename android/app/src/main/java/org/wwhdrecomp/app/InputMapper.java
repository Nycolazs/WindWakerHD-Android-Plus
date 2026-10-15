package org.wwhdrecomp.app;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Game controllers and hardware keyboards, read as a Wii U GamePad. Controllers map by button
 * position by default (the bottom face button is the Wii U's B, as on macOS), and the user can
 * assign every Wii U button to another controller button (Controls › Controller buttons); the
 * keyboard layout matches the macOS build: WASD move, arrows camera, K/Space = A, J = B, L = X,
 * I = Y, Q/E = L/R, Left Shift = ZL, C = ZR, Enter = +, Tab = -, H = Home, 1-4 = D-pad, X/V =
 * stick clicks.
 */
final class InputMapper {
    int padButtons, keyButtons;

    // ---- controller button assignment: for each Wii U button, the Android key code of the
    // controller button that presses it (analog triggers count as L2 / R2, a hat D-pad as the D-pad)
    static final int[] WIIU = {Native.A, Native.B, Native.X, Native.Y, Native.L, Native.R, Native.ZL, Native.ZR,
            Native.MINUS, Native.PLUS, Native.STICK_L, Native.STICK_R, Native.UP, Native.DOWN, Native.LEFT, Native.RIGHT};
    static final int[] DEFAULT_MAP = {KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2,
            KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT};
    final int[] map = DEFAULT_MAP.clone();
    private final java.util.Set<Integer> pressed = new java.util.HashSet<>();  // controller buttons held
    private int pulse;  // Wii U buttons held for a moment by the activity (a short press of Select)

    /** the stored assignment ("" or anything unreadable: the default) */
    void loadMap(String s) {
        System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
        String[] f = s == null ? new String[0] : s.split(",");
        if (f.length != map.length) return;
        try {
            for (int i = 0; i < map.length; i++) map[i] = Integer.parseInt(f[i].trim());
        } catch (NumberFormatException e) {
            System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
        }
    }

    String mapString() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < map.length; i++) b.append(i == 0 ? "" : ",").append(map[i]);
        return b.toString();
    }

    boolean isDefaultMap() { return java.util.Arrays.equals(map, DEFAULT_MAP); }

    /** Wii U button i goes to controller button `code`; the button that had it gets i's old one */
    void assign(int i, int code) {
        for (int j = 0; j < map.length; j++)
            if (j != i && map[j] == code) map[j] = map[i];
        map[i] = code;
        recompute();
    }

    /** the Wii U buttons the controller button presses */
    int bitFor(int code) {
        if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;  // some controllers send BACK for View
        int bits = 0;
        for (int i = 0; i < map.length; i++)
            if (map[i] == code) bits |= WIIU[i];
        return bits;
    }

    void pulse(int bits, boolean on) {
        pulse = on ? (pulse | bits) : (pulse & ~bits);
        recompute();
    }

    private void press(int code, boolean down) {
        if (down) pressed.add(code);
        else pressed.remove(code);
    }

    private void recompute() {
        int b = pulse;
        for (int code : pressed) b |= bitFor(code);
        padButtons = b;
    }

    /** a controller button a user can assign (not Home / Guide, which opens the menu) */
    static boolean assignable(int code) {
        return code != KeyEvent.KEYCODE_BUTTON_MODE && code != KeyEvent.KEYCODE_HOME
                && (KeyEvent.isGamepadButton(code) || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_DPAD_UP
                        || code == KeyEvent.KEYCODE_DPAD_DOWN || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT);
    }

    /** the controller buttons an analog event holds down right now: triggers as L2 / R2, a hat as the D-pad */
    static java.util.List<Integer> analogButtons(MotionEvent e) {
        java.util.List<Integer> l = new java.util.ArrayList<>();
        InputDevice d = e.getDevice();
        Layout lay = d != null ? layout(d, e) : null;
        if (lay != null && lay.lt != 0 && trigger(e, d, lay.lt) > 0.5f) l.add(KeyEvent.KEYCODE_BUTTON_L2);
        if (lay != null && lay.rt != 0 && trigger(e, d, lay.rt) > 0.5f) l.add(KeyEvent.KEYCODE_BUTTON_R2);
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (hx < -0.5f) l.add(KeyEvent.KEYCODE_DPAD_LEFT);
        if (hx > 0.5f) l.add(KeyEvent.KEYCODE_DPAD_RIGHT);
        if (hy < -0.5f) l.add(KeyEvent.KEYCODE_DPAD_UP);
        if (hy > 0.5f) l.add(KeyEvent.KEYCODE_DPAD_DOWN);
        return l;
    }

    /** a controller button's name, by position for the face buttons (their letters differ by brand) */
    static String buttonName(android.content.Context c, int code) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return c.getString(R.string.pad_face_bottom);
            case KeyEvent.KEYCODE_BUTTON_B: return c.getString(R.string.pad_face_right);
            case KeyEvent.KEYCODE_BUTTON_X: return c.getString(R.string.pad_face_left);
            case KeyEvent.KEYCODE_BUTTON_Y: return c.getString(R.string.pad_face_top);
            case KeyEvent.KEYCODE_BUTTON_L1: return "L1 / LB";
            case KeyEvent.KEYCODE_BUTTON_R1: return "R1 / RB";
            case KeyEvent.KEYCODE_BUTTON_L2: return "L2 / LT";
            case KeyEvent.KEYCODE_BUTTON_R2: return "R2 / RT";
            case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_BACK: return "Select / View / Share";
            case KeyEvent.KEYCODE_BUTTON_START: return "Start / Menu / Options";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: case KeyEvent.KEYCODE_BUTTON_THUMBR:
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_DPAD_DOWN: case KeyEvent.KEYCODE_DPAD_LEFT: case KeyEvent.KEYCODE_DPAD_RIGHT: {
                // as the Wii U buttons' names (wiiu_buttons: 10 L3, 11 R3, 12..15 D-pad)
                int i = code == KeyEvent.KEYCODE_BUTTON_THUMBL ? 10 : code == KeyEvent.KEYCODE_BUTTON_THUMBR ? 11
                        : code == KeyEvent.KEYCODE_DPAD_UP ? 12 : code == KeyEvent.KEYCODE_DPAD_DOWN ? 13 : code == KeyEvent.KEYCODE_DPAD_LEFT ? 14 : 15;
                return c.getResources().getStringArray(R.array.wiiu_buttons)[i];
            }
            default: return KeyEvent.keyCodeToString(code).replace("KEYCODE_", "").replace('_', ' ');
        }
    }
    float lx, ly, rx, ry;          // controller sticks
    private boolean kW, kA, kS, kD, kUp, kDown, kLeft, kRight;
    private float hatX, hatY;
    long lastControllerInput;      // uptime ms, to hide the on-screen controls while a controller is used

    static boolean isController(InputDevice d) {
        if (d == null) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int keyboardBit(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_K: case KeyEvent.KEYCODE_SPACE: return Native.A;
            case KeyEvent.KEYCODE_J: return Native.B;
            case KeyEvent.KEYCODE_L: return Native.X;
            case KeyEvent.KEYCODE_I: return Native.Y;
            case KeyEvent.KEYCODE_Q: return Native.L;
            case KeyEvent.KEYCODE_E: return Native.R;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return Native.ZL;
            case KeyEvent.KEYCODE_C: return Native.ZR;
            case KeyEvent.KEYCODE_ENTER: return Native.PLUS;
            case KeyEvent.KEYCODE_TAB: return Native.MINUS;
            case KeyEvent.KEYCODE_H: return Native.HOME;
            case KeyEvent.KEYCODE_1: return Native.UP;
            case KeyEvent.KEYCODE_2: return Native.DOWN;
            case KeyEvent.KEYCODE_3: return Native.LEFT;
            case KeyEvent.KEYCODE_4: return Native.RIGHT;
            case KeyEvent.KEYCODE_X: return Native.STICK_L;
            case KeyEvent.KEYCODE_V: return Native.STICK_R;
            default: return 0;
        }
    }

    /** True if the event was used. */
    boolean onKey(KeyEvent e) {
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (e.getAction() != KeyEvent.ACTION_DOWN && e.getAction() != KeyEvent.ACTION_UP) return false;
        int code = e.getKeyCode();
        InputDevice dev = e.getDevice();
        if (isController(dev) || KeyEvent.isGamepadButton(code)) {
            if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;
            if (bitFor(code) == 0) return false;
            press(code, down);
            recompute();
            lastControllerInput = e.getEventTime();
            return true;
        }
        switch (code) {
            case KeyEvent.KEYCODE_W: kW = down; return true;
            case KeyEvent.KEYCODE_A: kA = down; return true;
            case KeyEvent.KEYCODE_S: kS = down; return true;
            case KeyEvent.KEYCODE_D: kD = down; return true;
            case KeyEvent.KEYCODE_DPAD_UP: kUp = down; return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: kDown = down; return true;
            case KeyEvent.KEYCODE_DPAD_LEFT: kLeft = down; return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: kRight = down; return true;
            default: break;
        }
        int bit = keyboardBit(code);
        if (bit == 0) return false;
        keyButtons = down ? (keyButtons | bit) : (keyButtons & ~bit);
        return true;
    }

    // How far each stick reaches in each direction (per controller and axis): some sticks stop short
    // of their reported range on one side (a Retroid Pocket 6 reaches about 0.85 pushed up), and the
    // game only runs with the stick all the way out. A direction counts as full from 92% of its
    // range, or from the furthest it has been pushed if that is less (at least 75%).
    private final java.util.Map<String, float[]> reach = new java.util.HashMap<>();
    private static final float REACH_FLOOR = 0.75f, OUTER = 0.92f, INNER = 0.12f;

    /** a stick's two axes: dead zone in the middle, the direction's reach as full, within the circle */
    private float[] stick(MotionEvent e, InputDevice dev, int ax, int ay) {
        InputDevice.MotionRange rx = dev.getMotionRange(ax, e.getSource()), ry = dev.getMotionRange(ay, e.getSource());
        if (rx == null || ry == null) return new float[2];
        float x = e.getAxisValue(ax), y = e.getAxisValue(ay);
        String key = dev.getDescriptor() + "/" + ax + "/" + ay;
        float[] r = reach.get(key);  // furthest seen: -x, +x, -y, +y
        if (r == null) reach.put(key, r = new float[] {REACH_FLOOR, REACH_FLOOR, REACH_FLOOR, REACH_FLOOR});
        int ix = x < 0 ? 0 : 1, iy = y < 0 ? 2 : 3;
        r[ix] = Math.max(r[ix], Math.abs(x));
        r[iy] = Math.max(r[iy], Math.abs(y));
        // each side scaled so that its reach (at most 92% of the range) is 1
        x /= Math.min(r[ix], OUTER * (x < 0 ? -rx.getMin() : rx.getMax()));
        y /= Math.min(r[iy], OUTER * (y < 0 ? -ry.getMin() : ry.getMax()));
        float m = (float) Math.hypot(x, y), flat = Math.max(Math.max(rx.getFlat(), ry.getFlat()), INNER);
        if (m < flat) return new float[2];
        float k = Math.min(1f, (m - flat) / (1f - flat)) / m;
        return new float[] {x * k, y * k};
    }

    // which axes are the right stick and the triggers, per controller (kept: devices don't change)
    private static final class Layout { int rx, ry, lt, rt; }
    private static final java.util.Map<String, Layout> layouts = new java.util.HashMap<>();

    private static boolean has(InputDevice d, int axis, int source) { return d.getMotionRange(axis, source) != null; }
    // an axis reading at the bottom of its range: a trigger at rest (a stick rests at its centre)
    private static boolean atMin(MotionEvent e, InputDevice d, int axis) {
        InputDevice.MotionRange r = d.getMotionRange(axis, e.getSource());
        return r != null && e.getAxisValue(axis) <= r.getMin() + 0.15f * (r.getMax() - r.getMin());
    }

    /**
     * The axes of the right stick and the triggers, decided from the controller's first event (sticks
     * centred, triggers released): Z/RZ and RX/RY can be either (Xbox and DualSense with Android's key
     * layouts: stick on Z/RZ, triggers on BRAKE/GAS or LTRIGGER/RTRIGGER; generic HID such as 8BitDo
     * in DInput mode or a DualSense without a layout: stick on Z/RZ and triggers on RX/RY, or the
     * other way round).
     */
    static Layout layout(InputDevice d, MotionEvent e) {
        int source = e.getSource();
        String key = d.getDescriptor() + "/" + source;
        Layout l = layouts.get(key);
        if (l != null) return l;
        l = new Layout();
        boolean z = has(d, MotionEvent.AXIS_Z, source) && has(d, MotionEvent.AXIS_RZ, source);
        boolean r = has(d, MotionEvent.AXIS_RX, source) && has(d, MotionEvent.AXIS_RY, source);
        boolean zRest = z && atMin(e, d, MotionEvent.AXIS_Z) && atMin(e, d, MotionEvent.AXIS_RZ);
        boolean rRest = r && atMin(e, d, MotionEvent.AXIS_RX) && atMin(e, d, MotionEvent.AXIS_RY);
        int trigA = 0, trigB = 0;
        if (z && r && zRest && !rRest) {          // Z/RZ released triggers, RX/RY the stick
            l.rx = MotionEvent.AXIS_RX; l.ry = MotionEvent.AXIS_RY; trigA = MotionEvent.AXIS_Z; trigB = MotionEvent.AXIS_RZ;
        } else if (z) {                           // the stick on Z/RZ (RX/RY, if there, the triggers)
            l.rx = MotionEvent.AXIS_Z; l.ry = MotionEvent.AXIS_RZ;
            if (r) { trigA = MotionEvent.AXIS_RX; trigB = MotionEvent.AXIS_RY; }
        } else {
            l.rx = MotionEvent.AXIS_RX; l.ry = MotionEvent.AXIS_RY;
        }
        l.lt = has(d, MotionEvent.AXIS_LTRIGGER, source) ? MotionEvent.AXIS_LTRIGGER
                : has(d, MotionEvent.AXIS_BRAKE, source) ? MotionEvent.AXIS_BRAKE : trigA;
        l.rt = has(d, MotionEvent.AXIS_RTRIGGER, source) ? MotionEvent.AXIS_RTRIGGER
                : has(d, MotionEvent.AXIS_GAS, source) ? MotionEvent.AXIS_GAS : trigB;
        layouts.put(key, l);
        android.util.Log.i("wwhd", "controller " + d.getName() + " (" + Integer.toHexString(d.getVendorId()) + ":" + Integer.toHexString(d.getProductId())
                + "): right stick " + MotionEvent.axisToString(l.rx) + "/" + MotionEvent.axisToString(l.ry) + ", triggers "
                + (l.lt != 0 ? MotionEvent.axisToString(l.lt) : "buttons") + "/" + (l.rt != 0 ? MotionEvent.axisToString(l.rt) : "buttons"));
        return l;
    }

    // a trigger's travel 0..1, also for axes that rest at -1
    private static float trigger(MotionEvent e, InputDevice d, int axis) {
        InputDevice.MotionRange r = d.getMotionRange(axis, e.getSource());
        if (r == null) return 0;
        float v = e.getAxisValue(axis), min = r.getMin(), max = r.getMax();
        return max > min ? (v - min) / (max - min) : 0;
    }

    /** Sticks, triggers and hat switches of a controller; true if used. */
    boolean onMotion(MotionEvent e) {
        InputDevice dev = e.getDevice();
        if (!isController(dev) || e.getAction() != MotionEvent.ACTION_MOVE) return false;
        float[] ls = stick(e, dev, MotionEvent.AXIS_X, MotionEvent.AXIS_Y);
        lx = ls[0];
        ly = -ls[1];
        // right stick: Z/RZ (Xbox, DualSense and others with a key layout) or RX/RY (generic HID:
        // 8BitDo in DInput / Switch mode, DualSense without a layout), where Z/RZ are the triggers.
        // A trigger axis rests at its minimum (range 0..1 or -1..1 resting at -1); a stick centres at 0.
        Layout l = layout(dev, e);
        float[] rs = stick(e, dev, l.rx, l.ry);
        rx = rs[0];
        ry = -rs[1];
        // analog triggers (controllers without L2/R2 key events) as L2 / R2
        if (l.lt != 0) press(KeyEvent.KEYCODE_BUTTON_L2, trigger(e, dev, l.lt) > 0.5f);
        if (l.rt != 0) press(KeyEvent.KEYCODE_BUTTON_R2, trigger(e, dev, l.rt) > 0.5f);
        // D-pad reported as a hat, as the D-pad's buttons
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (hx != hatX || hy != hatY) {
            hatX = hx;
            hatY = hy;
            press(KeyEvent.KEYCODE_DPAD_LEFT, hx < -0.5f);
            press(KeyEvent.KEYCODE_DPAD_RIGHT, hx > 0.5f);
            press(KeyEvent.KEYCODE_DPAD_UP, hy < -0.5f);
            press(KeyEvent.KEYCODE_DPAD_DOWN, hy > 0.5f);
        }
        recompute();
        lastControllerInput = e.getEventTime();
        return true;
    }

    float keyLX() { return (kD ? 1f : 0f) - (kA ? 1f : 0f); }
    float keyLY() { return (kW ? 1f : 0f) - (kS ? 1f : 0f); }
    float keyRX() { return (kRight ? 1f : 0f) - (kLeft ? 1f : 0f); }
    float keyRY() { return (kUp ? 1f : 0f) - (kDown ? 1f : 0f); }

    /** Drop everything held (the activity lost focus). */
    void reset() {
        padButtons = keyButtons = pulse = 0;
        pressed.clear();
        lx = ly = rx = ry = 0;
        kW = kA = kS = kD = kUp = kDown = kLeft = kRight = false;
        hatX = hatY = 0;
    }
}
