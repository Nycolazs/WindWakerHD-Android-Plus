package org.wwhdrecomp.app;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

/**
 * Dynamic resolution, by temperature and by GPU load:
 * - while the phone heats up (the system's thermal status, and the thermal headroom forecast where
 *   the device reports one) the 3D resolution steps down from the chosen one, at most to 1x; it
 *   steps back up once the phone has cooled. The GPU is the phone's largest heat source while
 *   playing (an Adreno 650 at 1.5x is busy ~27 of 33 ms a frame), and a hot phone throttles its CPU
 *   too, which costs more frames than the lower resolution does.
 * - while the GPU is saturated (busy 92% or more of the time for 9 s: frames are about to be late,
 *   or are) it steps down too, also below 1x (to 0.6x) on GPUs too weak for the chosen one; back up
 *   after 30 s under 70%, never straight back to a scale that was too heavy a moment ago.
 * The renderer changes the scale between frames (gfx::set_resolution_scale); the chosen one stays
 * the setting.
 */
final class ThermalScaler {
    private static final long PERIOD_MS = 3000, DOWN_AFTER_MS = 9000, UP_AFTER_MS = 45000;
    // GPU load: the scales it steps through (below the chosen one), and how long a too-heavy scale is avoided
    private static final float[] LOAD_STEPS = {0.6f, 0.75f, 0.85f, 1f, 1.25f, 1.5f, 2f, 3f};
    private static final float LOAD_FLOOR = 0.6f, BUSY = 92f, IDLE = 70f;
    private static final long LOAD_DOWN_AFTER_MS = 9000, LOAD_UP_AFTER_MS = 30000, TOO_HEAVY_MS = 120000;
    private final PerfStats gpu;
    private int busySamples, idleSamples;
    private float tooHeavy = Float.MAX_VALUE;  // a scale the GPU couldn't keep up with ...
    private long tooHeavyUntil;                // ... until then

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
        gpu = new PerfStats(c);
    }

    void setListener(Listener l) { listener = l; }

    /** the scale chosen in the settings, and whether to follow the temperature */
    void configure(float chosenScale, boolean on) {
        if (!configured) {  // what the renderer started with (the setting, or WWHD_RES_SCALE)
            int inUse = Native.getOption("res_scale");
            current = inUse > 0 ? inUse / 100f : chosenScale;
        }
        configured = true;
        chosen = chosenScale;
        enabled = on;
        if (!on || current > chosen || current < Math.min(chosen, LOAD_FLOOR)) apply(chosen);
        busySamples = idleSamples = 0;
        tooHeavy = Float.MAX_VALUE;
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
        if (!enabled) return;
        float h = heat();
        float load = gpu.gpuLoad();  // -1 where the GPU's busy counter isn't readable
        if (load >= BUSY) { busySamples++; idleSamples = 0; }
        else if (load >= 0 && load <= IDLE) { idleSamples++; busySamples = 0; }
        else busySamples = idleSamples = 0;
        long now = android.os.SystemClock.uptimeMillis();
        float next = current;
        String why = null;
        if (h >= 0.85f && chosen > 1f && current > 1f && now - lastChange >= DOWN_AFTER_MS) {
            next = lower(current, 1f);
            why = "heat " + h;
        } else if (busySamples * PERIOD_MS >= LOAD_DOWN_AFTER_MS && now - lastChange >= LOAD_DOWN_AFTER_MS) {
            next = lower(current, Math.min(chosen, LOAD_FLOOR));
            if (next != current) {
                tooHeavy = current;
                tooHeavyUntil = now + TOO_HEAVY_MS;
            }
            why = "GPU busy " + Math.round(load) + "%";
        } else if (current < chosen && h <= 0.55f && idleSamples * PERIOD_MS >= LOAD_UP_AFTER_MS
                && now - lastChange >= (current < 1f || chosen <= 1f ? LOAD_UP_AFTER_MS : UP_AFTER_MS)) {
            next = higher(current);
            if (next >= tooHeavy && now < tooHeavyUntil) next = current;
            why = "cool, GPU busy " + Math.round(load) + "%";
        }
        if (next != current) {
            Log.i("wwhd", "dynamic resolution: " + why + ", " + current + "x -> " + next + "x");
            busySamples = idleSamples = 0;
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

    // the next step down, not below `floor`
    private float lower(float s, float floor) {
        float best = s;
        for (float f : LOAD_STEPS)
            if (f < s && f >= floor && (best == s || f > best)) best = f;
        return best;
    }

    // the next step up, at most the chosen scale
    private float higher(float s) {
        float best = chosen;
        for (float f : LOAD_STEPS)
            if (f > s && f < best) best = f;
        return Math.max(best, s);
    }
}
