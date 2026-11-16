package org.wwhdrecomp.app;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * The GamePad picture on a second display: the lower screen of dual-screen devices (AYN Thor and
 * the like), or any other presentation display. An Android Presentation on that display holds a
 * surface the renderer presents the GamePad image to (runtime/src/vk/vk_device.cpp,
 * present_drc_window); touches there are the GamePad's touch screen. Without a second display, or
 * with the option off, the GamePad picture stays in the main window's layout.
 *
 * The picture is 16:9, with bars where the display has another shape, or stretched over the whole
 * display (the option "GamePad aspect ratio"): the lower screens of dual-screen devices are nearly
 * square (the Thor's is 1240 x 1080), and the 16:9 picture covers under two thirds of them.
 */
final class DrcDisplay implements DisplayManager.DisplayListener {
    private static final float ASPECT = 16f / 9f;  // the GamePad picture's shape
    private static boolean DEBUG;  // debuggable build: log touches
    interface Listener {
        /** the GamePad picture moved to the second display (true) or back to the main window */
        void onDrcDisplayChanged(boolean active);
    }

    private final MainActivity activity;
    private final Listener listener;
    private final DisplayManager displays;
    private Screen screen;
    private boolean wanted;  // the game runs, the activity is visible and the option is on
    private boolean stretch;  // the picture over the whole display, instead of 16:9 with bars

    DrcDisplay(MainActivity a, Listener l) {
        activity = a;
        listener = l;
        DEBUG = a.debuggable();
        displays = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
        displays.registerDisplayListener(this, new Handler(Looper.getMainLooper()));
    }

    /** a second display the GamePad picture can use, or null */
    Display available() {
        Display[] d = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display x : d)
            if (x.getDisplayId() != Display.DEFAULT_DISPLAY && x.isValid()) return x;
        return null;
    }

    boolean active() { return screen != null; }

    /** show the GamePad picture there whenever possible (false: keep it in the main window) */
    void setWanted(boolean w) {
        wanted = w;
        update();
    }

    /** the picture stretched over the whole display (true), or 16:9 with bars */
    void setStretch(boolean s) {
        stretch = s;
        if (screen != null) screen.setStretch(s);
    }

    void release() {
        wanted = false;
        update();
        displays.unregisterDisplayListener(this);
    }

    private void update() {
        Display d = wanted ? available() : null;
        if (screen != null && (d == null || screen.getDisplay().getDisplayId() != d.getDisplayId())) {
            Screen s = screen;
            screen = null;
            s.dismiss();  // its surface goes: the renderer lets go of the window
            listener.onDrcDisplayChanged(false);
        }
        if (screen == null && d != null) {
            try {
                Screen s = new Screen(activity, d, stretch);
                // closed by someone else (the system): the GamePad picture goes back to the main window
                s.setOnDismissListener(x -> {
                    if (screen == s) {
                        screen = null;
                        listener.onDrcDisplayChanged(false);
                    }
                });
                screen = s;
                s.show();
                listener.onDrcDisplayChanged(true);
            } catch (WindowManager.InvalidDisplayException e) {
                screen = null;  // removed meanwhile
            }
        }
    }

    @Override
    public void onDisplayAdded(int id) { update(); }

    @Override
    public void onDisplayRemoved(int id) { update(); }

    @Override
    public void onDisplayChanged(int id) {}

    /**
     * The surface's size for a stretched picture in a view of w x h, or null where the view has the
     * picture's shape anyway. The renderer keeps the picture at 16:9 inside its surface, so the
     * surface gets that shape (the picture then fills it) and Android scales it to the view. It
     * covers the view, so that Android shrinks one direction and enlarges none (the Thor's
     * 1240 x 1080: a surface of 1920 x 1080), and is Full HD at most.
     */
    static int[] stretchedSurface(int w, int h) {
        if (w <= 0 || h <= 0) return null;
        float a = (float) w / h;
        if (Math.abs(a - ASPECT) < 0.01f * ASPECT) return null;  // the renderer fills such a surface as it is
        int sw = w, sh = h;
        if (a < ASPECT) sw = Math.round(h * ASPECT);
        else sh = Math.round(w / ASPECT);
        if (sw > 1920) {
            sw = 1920;
            sh = 1080;
        }
        return new int[] {sw, sh};
    }

    /** where the picture is in a view of w x h: {left, top, width, height} */
    static float[] pictureRect(float w, float h, boolean stretch) {
        float pw = w, ph = h, px = 0, py = 0;
        if (stretch) return new float[] {px, py, pw, ph};
        if (pw / ph > ASPECT) {
            pw = ph * ASPECT;
            px = (w - pw) / 2;
        } else {
            ph = pw / ASPECT;
            py = (h - ph) / 2;
        }
        return new float[] {px, py, pw, ph};
    }

    /** the presentation: a black window with the picture surface over the whole display */
    private static final class Screen extends Presentation implements SurfaceHolder.Callback {
        private SurfaceView surfaceView;
        private boolean stretch;
        private int fixedW, fixedH;  // the surface's size while the picture is stretched (0: the view's)

        Screen(Context outer, Display d, boolean stretch) {
            super(outer, d);
            this.stretch = stretch;
        }

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            // touches only: keys and controllers stay with the game's window (a dialog would also
            // take a controller's B as Back and close itself)
            setCancelable(false);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            SurfaceView v = new SurfaceView(getContext()) {
                // the whole window, whatever the surface's size (SurfaceView asks for a fixed one's)
                @Override
                protected void onMeasure(int widthSpec, int heightSpec) {
                    setMeasuredDimension(getDefaultSize(0, widthSpec), getDefaultSize(0, heightSpec));
                }
            };
            surfaceView = v;
            v.setBackgroundColor(Color.TRANSPARENT);
            v.getHolder().addCallback(this);
            v.setOnTouchListener((view, e) -> touch(e));
            // the stretched surface's size goes by the view's: its first layout, a display that changes
            v.addOnLayoutChangeListener((view, l, t, r, b, ol, ot, or, ob) -> {
                if (r - l != or - ol || b - t != ob - ot) view.post(this::applyShape);
            });
            setContentView(v);
            applyShape();  // before the surface exists, from the display's size
            getWindow().getDecorView().setBackgroundColor(Color.BLACK);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }

        void setStretch(boolean s) {
            stretch = s;
            applyShape();
        }

        // 16:9 with bars: the surface has the view's size and the renderer letterboxes the picture in
        // it. Stretched: the surface is fixed at 16:9 (stretchedSurface) and Android scales it to the view.
        private void applyShape() {
            if (surfaceView == null) return;  // not created yet
            int w = surfaceView.getWidth(), h = surfaceView.getHeight();
            if (w == 0 || h == 0) {  // not laid out yet: the window covers the display
                Point p = new Point();
                getDisplay().getRealSize(p);
                w = p.x;
                h = p.y;
            }
            int[] size = stretch ? stretchedSurface(w, h) : null;
            int fw = size != null ? size[0] : 0, fh = size != null ? size[1] : 0;
            if (fw == fixedW && fh == fixedH) return;
            fixedW = fw;
            fixedH = fh;
            if (size != null) surfaceView.getHolder().setFixedSize(fw, fh);
            else surfaceView.getHolder().setSizeFromLayout();
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {}

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            Native.drcSurfaceChanged(holder.getSurface());
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            Native.drcSurfaceChanged(null);
        }

        // the GamePad picture fills the display at 16:9 (bars where the display has another shape),
        // or all of it when stretched; touches come in the view's pixels, not the surface's
        private boolean touch(MotionEvent e) {
            if (surfaceView == null) return false;
            float w = surfaceView.getWidth(), h = surfaceView.getHeight();
            if (w == 0 || h == 0) return false;
            float[] p = pictureRect(w, h, stretch);
            int a = e.getActionMasked();
            boolean down = a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_CANCEL;
            float tx = Math.max(0, Math.min(1, (e.getX() - p[0]) / p[2])), ty = Math.max(0, Math.min(1, (e.getY() - p[1]) / p[3]));
            Native.setTouch(down, tx, ty);
            if (DEBUG) android.util.Log.d("wwhd", "GamePad display touch " + (down ? "down" : "up") + " " + tx + "," + ty);
            return true;
        }
    }
}
