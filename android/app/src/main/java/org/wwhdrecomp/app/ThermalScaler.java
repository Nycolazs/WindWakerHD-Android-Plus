package org.wwhdrecomp.app;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

/**
 * Dynamic resolution by temperature: while the phone heats up (the system's thermal status, and the
 * thermal headroom forecast where the device reports one) the 3D resolution steps down from the
 * chosen one, at most to 1x; it steps back up once the phone has cooled. The GPU is the phone's
 * largest heat source while playing (an Adreno 650 at 1.5x is busy ~27 of 33 ms a frame), and a hot
 * phone throttles its CPU too, which costs more frames than the lower resolution does. The renderer
 * changes the scale between frames (gfx::set_resolution_scale); the chosen one stays the setting.
 */
final class ThermalScaler {
    private static final long PERIOD_MS = 3000, DOWN_AFTER_MS = 9000, UP_AFTER_MS = 45000;

    private final Context context;
    private final PowerManager power;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean running, enabled;
    private float chosen = 1f, current = 1f;
    private long lastChange;
    private Listener listener;

    interface Listener { void onScale(float scale, float chosen); }

    ThermalScaler(Context c) {
        context = c;
        power = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
    }

    void setListener(Listener l) { listener = l; }

    /** the scale chosen in the settings, and whether to follow the temperature */
    void configure(float chosenScale, boolean on) {
        if (!configured) current = chosenScale;  // what the renderer started with (WWHD_RES_SCALE)
        configured = true;
        chosen = chosenScale;
        enabled = on;
        if (!on || current > chosen || current < Math.min(chosen, 1f)) apply(chosen);
    }
    private boolean configured;

    float current() { return current; }

    void start() {
        if (running) return;
        running = true;
        handler.postDelayed(tick, PERIOD_MS);
    }

    void stop() {
        running = false;
        handler.removeCallbacks(tick);
    }

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            step();
            handler.postDelayed(this, PERIOD_MS);
        }
    };

    // 0 cool .. 1 throttling now (the forecast for the next 10 s where reported, else the status)
    private float heat() {
        float status;
        switch (power.getCurrentThermalStatus()) {
            case PowerManager.THERMAL_STATUS_NONE: status = 0f; break;
            case PowerManager.THERMAL_STATUS_LIGHT: status = 0.6f; break;
            case PowerManager.THERMAL_STATUS_MODERATE: status = 0.9f; break;
            default: status = 1f; break;
        }
        float headroom = power.getThermalHeadroom(10);
        return Float.isNaN(headroom) || headroom <= 0f ? status : Math.max(status, headroom);
    }

    private void step() {
        if (!enabled || chosen <= 1f) return;
        float h = heat();
        long now = android.os.SystemClock.uptimeMillis();
        float next = current;
        if (h >= 0.85f && now - lastChange >= DOWN_AFTER_MS) next = lower(current);
        else if (h <= 0.55f && now - lastChange >= UP_AFTER_MS) next = higher(current);
        if (next != current) {
            Log.i("wwhd", "thermal: heat " + h + ", resolution " + current + "x -> " + next + "x");
            apply(next);
        }
    }

    private void apply(float scale) {
        if (scale == current) return;
        current = scale;
        lastChange = android.os.SystemClock.uptimeMillis();
        Native.setOption("res_scale", Math.round(scale * 100));
        if (listener != null) listener.onScale(current, chosen);
    }

    // the settings' scales between 1x and the chosen one
    private float lower(float s) {
        float best = s;
        for (String v : MainActivity.RES_SCALES) {
            float f = Float.parseFloat(v);
            if (f < s && f >= Math.min(1f, chosen) && (best == s || f > best)) best = f;
        }
        return best;
    }

    private float higher(float s) {
        float best = s;
        for (String v : MainActivity.RES_SCALES) {
            float f = Float.parseFloat(v);
            if (f > s && f <= chosen && (best == s || f < best)) best = f;
        }
        return best;
    }
}
