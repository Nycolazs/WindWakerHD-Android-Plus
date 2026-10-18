package org.wwhdrecomp.app;

import android.app.Dialog;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.text.Layout;
import android.text.TextPaint;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Random;

/**
 * The in-game menu, in the style of the game's own pause menus (drawn in code, no game assets): a
 * parchment panel with a rough edge over the dimmed game, cyan tabs (Saves, Graphics, Mods, Controls), blue
 * bar buttons for actions, and a slot per setting with its value: switches (ON/OFF), choices
 * (◀ value ▶) and sub-menus (›). Changes apply at once. Works with touch and with a game
 * controller: L/R switch tabs, the D-pad moves and changes values, and as in the game the right
 * button selects and the bottom one goes back.
 */
final class OptionsMenu extends Dialog {
    static int lastTab;  // reopens where it was left

    private static final int SLATE = GameUi.SLATE, CYAN_DARK = GameUi.CYAN_DARK, INK = GameUi.INK, HINT = GameUi.HINT;
    private static final int YELLOW = GameUi.YELLOW, GLOW = GameUi.GLOW;

    private final MainActivity a;
    private final float dp;
    private int tab;
    private LinearLayout rows;
    private ScrollView scroll;
    private static final int TABS = 5;
    // Saves, Graphics, Mods, Controls
    private static final GameUi.Palette[] TAB_COLORS = {GameUi.GREEN, GameUi.VIOLET, GameUi.BLUE, GameUi.AMBER, GameUi.CORAL};
    private final TextView[] tabs = new TextView[TABS];

    private String languageAtOpen;  // the game language when the menu opened: a change asks for a restart on closing

    OptionsMenu(MainActivity a) {
        super(GameUi.fitted(a), android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        this.a = a;
        languageAtOpen = a.gameLanguage();
        dp = getContext().getResources().getDisplayMetrics().density;  // fitted to the screen
        tab = lastTab;
    }

    private int px(float v) { return Math.round(v * dp); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(SLATE));
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        w.getAttributes().layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        FrameLayout root = new FrameLayout(getContext());
        LinearLayout panel = new LinearLayout(getContext());
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(new GameUi.Parchment(getContext()));
        panel.setPadding(px(28), px(18), px(28), px(14));
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(-1, -1);
        plp.setMargins(px(26), px(18), px(26), px(18));
        root.addView(panel, plp);

        // tabs and Back
        LinearLayout top = new LinearLayout(getContext());
        top.setGravity(Gravity.CENTER_VERTICAL);
        int[] names = {R.string.opt_saves, R.string.opt_game, R.string.opt_graphics, R.string.opt_mods, R.string.opt_controls};
        for (int i = 0; i < TABS; i++) {
            final int t = i;
            TextView v = new GameUi.OutlinedText(getContext());
            v.setText(a.getString(names[i]).toUpperCase(java.util.Locale.ROOT));
            v.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
            v.setTextColor(Color.WHITE);
            v.setGravity(Gravity.CENTER);
            v.setSingleLine(true);
            v.setPadding(px(18), px(6), px(18), px(8));
            v.setFocusable(true);
            v.setOnClickListener(x -> selectTab(t));
            tabs[i] = v;
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.rightMargin = px(8);
            top.addView(v, lp);
        }
        View spacer = new View(getContext());
        top.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1));
        TextView back = barButton(a.getString(R.string.opt_back), v -> dismiss());
        top.addView(back, new LinearLayout.LayoutParams(px(130), px(46)));
        panel.addView(top, new LinearLayout.LayoutParams(-1, -2));

        // actions on the left, the tab's settings on the right
        LinearLayout body = new LinearLayout(getContext());
        LinearLayout left = new LinearLayout(getContext());
        left.setOrientation(LinearLayout.VERTICAL);
        left.setPadding(0, px(14), px(18), 0);
        addAction(left, R.string.opt_about, a::showLicenses);
        addAction(left, R.string.opt_quit, a::quitApp);
        body.addView(left, new LinearLayout.LayoutParams(px(220), -1));
        scroll = new ScrollView(getContext());
        rows = new LinearLayout(getContext());
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(0, px(10), 0, px(10));
        scroll.addView(rows, new FrameLayout.LayoutParams(-1, -2));
        body.addView(scroll, new LinearLayout.LayoutParams(0, -1, 1));
        panel.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));

        TextView hint = new TextView(getContext());
        hint.setText(R.string.opt_pad_hint);
        hint.setTextColor(HINT);
        hint.setTextSize(13);
        hint.setGravity(Gravity.END);
        panel.addView(hint, new LinearLayout.LayoutParams(-1, -2));

        setContentView(root);
        WindowInsetsController ic = w.getInsetsController();  // the window's views exist from here on
        if (ic != null) {
            ic.hide(WindowInsets.Type.systemBars());
            ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        selectTab(tab);
    }

    // sub-dialogs (frame generation, save states...) open on top; show their changes when back
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) fill();
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_L1: selectTab((tab + TABS - 1) % TABS); return true;
            case KeyEvent.KEYCODE_BUTTON_R1: selectTab((tab + 1) % TABS); return true;
            // by position, as in the game: the bottom button (Android's A) is the Wii U's B, back;
            // the right button (Android's B) is the Wii U's A, select
            case KeyEvent.KEYCODE_BUTTON_A: onBackPressed(); return true;
            case KeyEvent.KEYCODE_BUTTON_MODE: return true;  // closes on release (below): the game opens on release
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                // held for 0.6 s closes, as it opens; only a fresh press (the one that opened the
                // menu may still be held, its repeats arrive here)
                if (e.getRepeatCount() == 0) {
                    selectDown = true;
                    rows.postDelayed(selectHeld, 600);
                }
                return true;
            case KeyEvent.KEYCODE_BUTTON_START:
                // Select + Start closes, as it opens (a fresh Select press: the one that opened the
                // menu may still be held)
                if (e.getRepeatCount() == 0 && selectDown) startCloses = true;
                return true;
            case KeyEvent.KEYCODE_BUTTON_B: {
                View f = getCurrentFocus();
                if (f != null) f.performClick();
                return true;
            }
            default: return super.onKeyDown(code, e);
        }
    }

    // a page inside a tab (e.g. the performance overlay's settings) instead of the tab's own rows
    private Runnable page;

    private void openPage(Runnable p) {
        page = p;
        fill();
        scroll.scrollTo(0, 0);
        if (rows.getChildCount() > 1) rows.getChildAt(1).requestFocus();
    }

    @Override
    public void onBackPressed() {
        if (page != null) {
            page = null;
            fill();
        } else {
            super.onBackPressed();
        }
    }

    private void pageTitle(int title) {
        TextView t = new TextView(getContext());
        t.setText("‹  " + a.getString(title));
        t.setTextColor(INK);
        t.setTextSize(20);
        t.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
        t.setPadding(px(12), px(6), px(12), px(10));
        t.setFocusable(true);
        t.setBackground(states(new ColorDrawable(0), slot(GLOW, YELLOW)));
        t.setOnClickListener(v -> onBackPressed());
        rows.addView(t, new LinearLayout.LayoutParams(-2, -2));
    }

    // frame generation (Lossless Scaling's, with the user's own Lossless.dll)
    private void frameGenPage() {
        pageTitle(R.string.opt_framegen);
        if (!a.hasFrameGenDll()) {
            note(a.getString(R.string.fg_about));
            submenu(R.string.opt_fg_choose_dll, 0, "", a::pickDll);
            return;
        }
        String err = Native.frameGenError();
        if (a.prefs.getBoolean("fg_enabled", false) && !err.isEmpty()) note(a.getString(R.string.fg_error, err));
        toggle(R.string.opt_framegen, 0, a.prefs.getBoolean("fg_enabled", false), on -> a.setFrameGen("fg_enabled", on));
        // the rate frame generation reaches: frames shown without it (30, or interpolated) times the
        // multiplier; above the display's refresh rate it generates nothing (the renderer checks)
        String[] mult = new String[3];
        int base = a.shownFps(), maxHz = a.maxRefreshHz();
        for (int m = 2; m <= 4; m++)
            mult[m - 2] = a.getString(base * m > maxHz * 1.1 ? R.string.fg_multiplier_item_over : R.string.fg_multiplier_item, m, base * m);
        choice(R.string.opt_fg_multiplier, R.string.opt_fg_multiplier_hint, mult, a.prefs.getInt("fg_multiplier", 2) - 2,
                i -> a.setFrameGenMultiplier(i + 2));
        String[] modes = {a.getString(R.string.fg_performance), a.getString(R.string.fg_quality)};
        choice(R.string.opt_fg_network, 0, modes, a.prefs.getBoolean("fg_quality", false) ? 1 : 0, i -> a.setFrameGen("fg_quality", i == 1));
        String[] flows = new String[MainActivity.FG_FLOW_SCALES.length];
        for (int i = 0; i < flows.length; i++) flows[i] = Math.round(Float.parseFloat(MainActivity.FG_FLOW_SCALES[i]) * 100) + "%";
        choice(R.string.opt_fg_flow, R.string.opt_fg_flow_hint, flows, indexOf(MainActivity.FG_FLOW_SCALES, a.prefs.getString("fg_flow_scale", "0.5"), 1),
                i -> a.setFrameGenFlow(MainActivity.FG_FLOW_SCALES[i]));
        toggle(R.string.opt_fg_ui, R.string.opt_fg_ui_hint, a.prefs.getBoolean("fg_ui_detection", true), on -> a.setFrameGen("fg_ui_detection", on));
        submenu(R.string.opt_fg_replace_dll, a.frameGenDllTested() ? R.string.fg_dll_tested_hint : R.string.fg_dll_untested_hint, "",
                a::pickDll);
        submenu(R.string.opt_fg_remove_dll, 0, "", () -> new GameDialog(getContext()).title(R.string.opt_fg_remove_dll)
                .message(R.string.opt_fg_remove_confirm)
                .button(R.string.opt_cancel, null)
                .button(R.string.opt_fg_remove_dll, () -> { a.removeFrameGenDll(); fill(); }).show());
    }

    // the GPU driver (Adreno): the system's or a driver package the user installed
    private void gpuDriverPage() {
        pageTitle(R.string.opt_gpu_driver);
        note(a.getString(R.string.gpu_driver_about));
        String running = Native.gpuDriverInfo();
        if (!running.isEmpty()) note(a.getString(R.string.gpu_driver_running, running));
        if (Native.gpuDriverFellBack()) note(a.getString(R.string.gpu_driver_fell_back, a.gpuDriverLabel()));
        String cur = a.gpuDriverId();
        driverRow(a.getString(R.string.gpu_driver_system), a.getString(R.string.gpu_driver_system_hint), cur.isEmpty(),
                () -> a.chooseGpuDriver("", a.getString(R.string.gpu_driver_system)));
        for (GpuDrivers.Driver d : GpuDrivers.list(getContext())) {
            boolean active = d.id.equals(cur);
            driverRow(d.name, d.version, active, () -> {
                GameDialog dlg = new GameDialog(getContext()).title(d.name)
                        .message(d.version + (d.description.isEmpty() ? "" : "\n\n" + d.description))
                        .button(R.string.opt_cancel, null)
                        .button(R.string.gpu_driver_remove, () -> a.removeGpuDriver(d.id, this::fill));
                if (!active) dlg.button(R.string.gpu_driver_use, () -> a.chooseGpuDriver(d.id, d.name));
                dlg.show();
            });
        }
        submenu(R.string.gpu_driver_install, R.string.gpu_driver_install_hint, "", a::pickDriver);
    }

    private void driverRow(String label, String hint, boolean active, Runnable click) {
        LinearLayout r = rowText(label, hint == null || hint.isEmpty() ? null : hint);
        TextView v = new TextView(getContext());
        v.setText(active ? "✓" : "›");
        v.setTextColor(active ? GameUi.GREEN.dark : INK);
        v.setTextSize(active ? 22 : 16);
        v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        r.addView(v, new LinearLayout.LayoutParams(-2, -2));
        r.setOnClickListener(x -> click.run());
    }

    // a paragraph of explanation on a page
    private void note(String text) {
        TextView t = new TextView(getContext());
        t.setText(text);
        t.setTextColor(INK);
        t.setTextSize(16);
        t.setPadding(px(12), px(4), px(12), px(14));
        rows.addView(t, new LinearLayout.LayoutParams(-1, -2));
    }

    private void perfPage() {
        pageTitle(R.string.opt_perf);
        toggle(R.string.perf_show, 0, a.prefs.getBoolean("perf_hud", false), a::setPerfHud);
        int bits = a.prefs.getInt("perf_items", ControlsView.PERF_ALL);
        for (int i = 0; i < MainActivity.PERF_BITS.length; i++) {
            int bit = MainActivity.PERF_BITS[i];
            indentNext = true;  // the overlay's values, below its switch
            toggle(MainActivity.PERF_LABELS[i], 0, (bits & bit) != 0, on -> a.setPerfItem(bit, on));
        }
        submenu(R.string.opt_perf_move, R.string.opt_perf_move_hint, "", () -> {
            a.movePerfOverlay();
            dismiss();  // the game, to drag it on
        });
        submenu(R.string.opt_perf_reset, 0, "", () -> { a.resetPerfOverlay(); fill(); });
    }

    private final Runnable selectHeld = this::dismiss;
    private boolean selectDown, startCloses;

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) {
            selectDown = false;
            rows.removeCallbacks(selectHeld);
            return true;
        }
        // on release, so it doesn't reach the game screen
        if (code == KeyEvent.KEYCODE_BUTTON_START) {
            if (startCloses) dismiss();
            startCloses = false;
            return true;
        }
        // Home / Guide: on release, so the release doesn't reach the game screen and reopen the menu
        if (code == KeyEvent.KEYCODE_BUTTON_MODE) {
            dismiss();
            return true;
        }
        return super.onKeyUp(code, e);
    }

    @Override
    public void dismiss() {
        if (!a.gameLanguage().equals(languageAtOpen)) {
            languageAtOpen = a.gameLanguage();
            a.askRestartForLanguage();
        }
        rows.removeCallbacks(selectHeld);
        super.dismiss();
    }

    private void selectTab(int t) {
        page = null;
        tab = lastTab = t;
        for (int i = 0; i < TABS; i++) {
            boolean on = i == t;
            GameUi.Palette pal = TAB_COLORS[i];
            tabs[i].setBackground(new GameUi.TabPlate(getContext(), on, pal));
            tabs[i].setTextSize(on ? 24 : 20);
            ((GameUi.OutlinedText) tabs[i]).outline = on ? pal.dark : pal.muted().dark;
        }
        fill();
        scroll.scrollTo(0, 0);
    }

    // ---- the tabs' settings
    private void fill() {
        if (rows == null) return;
        View focused = getCurrentFocus();
        int focusIndex = focused != null && focused.getParent() == rows ? rows.indexOfChild(focused) : -1;
        rows.removeAllViews();
        if (page != null) page.run();
        else if (tab == 0) saves();
        else if (tab == 1) game();
        else if (tab == 2) graphics();
        else if (tab == 3) mods();
        else controls();
        if (focusIndex >= 0 && focusIndex < rows.getChildCount()) rows.getChildAt(focusIndex).requestFocus();
    }

    // save states (5 slots with their picture, time and place) and the import / export of saves
    private void saves() {
        // the game save first: export, import and its automatic backup; then the save states
        submenu(R.string.opt_export, R.string.opt_export_hint, "", a::chooseExport);
        submenu(R.string.opt_import, R.string.opt_import_hint, "", () -> a.pickFolder(MainActivity.PICK_IMPORT));
        // saves kept outside the app (SaveVault): on once Android's "All files access" is allowed
        boolean vault = SaveVault.enabled(a);
        submenu(R.string.opt_vault, 0, a.getString(vault ? R.string.opt_on : R.string.opt_off), () -> {
            if (SaveVault.enabled(a)) a.info(a.getString(R.string.vault_about_on, SaveVault.FOLDER));
            else new GameDialog(getContext()).title(R.string.vault_title).message(a.getString(R.string.vault_message, SaveVault.FOLDER))
                    .button(R.string.vault_later, null)
                    .button(R.string.vault_allow, () -> {
                        dismiss();
                        a.requestVault(false);
                    }).show();
        });
        note(a.getString(vault ? R.string.opt_vault_hint_on : R.string.opt_vault_hint, SaveVault.FOLDER));
        toggle(R.string.opt_auto_backup, R.string.opt_auto_backup_hint, a.prefs.getBoolean("auto_backup", true), on -> {
            a.prefs.edit().putBoolean("auto_backup", on).apply();
            if (on) a.autoBackup();
        });
        // autosave: on/off and interval (runtime/src/savestate.cpp); Off is the first entry
        final int[] AUTO_SECS = {0, 60, 120, 300, 600};
        int autoCur = 0;
        {
            int secs = a.prefs.getInt("autosave_secs", MainActivity.DEFAULT_AUTOSAVE_SECS);
            for (int i = 0; i < AUTO_SECS.length; i++) if (AUTO_SECS[i] == secs) { autoCur = i; break; }
        }
        choice(R.string.opt_autosave, R.string.opt_autosave_hint,
               a.getResources().getStringArray(R.array.autosave_intervals), autoCur, i -> {
                   a.prefs.edit().putInt("autosave_secs", AUTO_SECS[i]).apply();
                   Native.setOption("autosave_secs", AUTO_SECS[i]);
               });
        // the autosave's own slot (kAutoSlot, 6), shown above slot 1
        {
            final int slot = 6;
            String[] info = Native.saveSlotInfo(slot);
            boolean used = info[0].equals("1"), compatible = info[1].equals("1");
            String desc = !used ? a.getString(R.string.opt_slot_empty)
                    : info[3].isEmpty() ? info[2] : info[2] + " · " + info[3];
            if (used && !compatible) desc = a.getString(R.string.opt_slot_incompatible, desc);
            LinearLayout r = rowText(a.getString(R.string.opt_slot_auto), desc);
            ImageView pic = new ImageView(getContext());
            pic.setScaleType(ImageView.ScaleType.CENTER_CROP);
            android.graphics.Bitmap bmp = used ? thumbnail(slot) : null;
            if (bmp != null) pic.setImageBitmap(bmp);
            GradientDrawable frame = new GradientDrawable();
            frame.setColor(0x33000000);
            frame.setCornerRadius(px(6));
            pic.setBackground(frame);
            pic.setClipToOutline(true);
            LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(px(112), px(63));
            plp.rightMargin = px(16);
            r.addView(pic, 0, plp);
            TextView save = smallButton(a.getString(R.string.opt_slot_save), true, v -> saveTo(slot));
            TextView load = smallButton(a.getString(R.string.opt_slot_load), used && compatible, v -> {
                Native.loadState(slot);
                a.showStateResult();
                dismiss();  // see the game come back
            });
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(px(96), px(44));
            blp.leftMargin = px(10);
            r.addView(save, blp);
            r.addView(load, new LinearLayout.LayoutParams(blp));
            r.setFocusable(false);
            r.setBackground(slot(0x1F000000, 0));
        }
        for (int slot = 1; slot <= 5; slot++) {
            String[] info = Native.saveSlotInfo(slot);
            boolean used = info[0].equals("1"), compatible = info[1].equals("1");
            String desc = !used ? a.getString(R.string.opt_slot_empty)
                    : info[3].isEmpty() ? info[2] : info[2] + " · " + info[3];
            if (used && !info[4].isEmpty())  // the controls it was saved with (and loads with)
                desc += " · " + a.getString(info[4].equals("pro") ? R.string.controller_pro : R.string.controller_gamepad);
            if (used && !compatible) desc = a.getString(R.string.opt_slot_incompatible, desc);
            LinearLayout r = rowText(a.getString(R.string.opt_slot, slot), desc);
            ImageView pic = new ImageView(getContext());
            pic.setScaleType(ImageView.ScaleType.CENTER_CROP);
            android.graphics.Bitmap bmp = used ? thumbnail(slot) : null;
            if (bmp != null) pic.setImageBitmap(bmp);
            GradientDrawable frame = new GradientDrawable();
            frame.setColor(0x33000000);
            frame.setCornerRadius(px(6));
            pic.setBackground(frame);
            pic.setClipToOutline(true);
            LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(px(112), px(63));
            plp.rightMargin = px(16);
            r.addView(pic, 0, plp);
            final int n = slot;
            TextView save = smallButton(a.getString(R.string.opt_slot_save), true, v -> {
                if (!used) saveTo(n);
                else new GameDialog(getContext()).title(a.getString(R.string.opt_slot, n)).message(a.getString(R.string.state_overwrite, n))
                        .button(R.string.opt_cancel, null)
                        .button(R.string.opt_slot_save, () -> saveTo(n)).show();
            });
            TextView load = smallButton(a.getString(R.string.opt_slot_load), used && compatible, v -> {
                Native.loadState(n);
                a.showStateResult();
                dismiss();  // see the game come back
            });
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(px(96), px(44));
            blp.leftMargin = px(10);
            r.addView(save, blp);
            r.addView(load, new LinearLayout.LayoutParams(blp));
            // the D-pad goes to the Save and Load buttons themselves, not the row
            r.setFocusable(false);
            r.setBackground(slot(0x1F000000, 0));
        }
    }

    // a state is captured at the next frame boundary and written in the background (a few seconds
    // for its ~250 MB): show the slot again once its file has changed
    private void saveTo(int slot) {
        final String before = String.join("|", Native.saveSlotInfo(slot));
        final long until = android.os.SystemClock.uptimeMillis() + 20000;
        a.saveState(slot);
        rows.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!isShowing()) return;
                if (!String.join("|", Native.saveSlotInfo(slot)).equals(before)) {
                    fill();
                    rows.postDelayed(() -> { if (isShowing()) fill(); }, 1500);  // its picture is written separately
                }
                else if (android.os.SystemClock.uptimeMillis() < until) rows.postDelayed(this, 300);
            }
        }, 300);
    }

    private android.graphics.Bitmap thumbnail(int slot) {
        java.io.File f = new java.io.File(new java.io.File(a.baseDir(), "states"), "slot" + slot + ".png");
        if (!f.exists()) return null;
        android.graphics.BitmapFactory.Options o = new android.graphics.BitmapFactory.Options();
        o.inSampleSize = 8;  // about 320 px wide is plenty for the slot picture
        return android.graphics.BitmapFactory.decodeFile(f.getPath(), o);
    }

    private TextView smallButton(String text, boolean enabled, View.OnClickListener click) {
        TextView b = barButton(text, click);
        b.setTextSize(15);
        b.setEnabled(enabled);
        b.setFocusable(enabled);
        if (!enabled) b.setAlpha(0.4f);
        return b;
    }

    private void graphics() {
        String[] res = new String[MainActivity.RES_SCALES.length];
        for (int i = 0; i < res.length; i++) res[i] = a.resolutionLabel(MainActivity.RES_SCALES[i]);
        // 30, 60 (generated), 60 and true 60 everywhere; 120 and 40 fps only on 120 Hz panels (fps60.cpp's modes)
        int[] fpsModes = a.has120Hz() ? new int[] {0, 5, 1, 6, 4, 3} : new int[] {0, 5, 1, 3};
        String[] fpsNames = new String[fpsModes.length];
        int fpsMode = a.prefs.getBoolean("fg_enabled", false) ? 0 : a.savedFpsMode(), fpsIndex = 0;
        for (int i = 0; i < fpsModes.length; i++) {
            fpsNames[i] = a.fpsModeName(fpsModes[i]);
            if (fpsModes[i] == fpsMode) fpsIndex = i;
        }
        choice(R.string.opt_fps, R.string.opt_fps_hint, fpsNames, fpsIndex, i -> a.setFpsMode(fpsModes[i]));
        // one switch for weaker GPUs: the settings below that cost the most GPU time for the least picture
        choice(R.string.opt_light, R.string.opt_light_hint, a.getResources().getStringArray(R.array.light_modes),
               a.lightGraphics() ? 1 : 0, i -> {
                   a.setLightGraphics(i == 1);
                   fill();
               });
        choice(R.string.opt_drawdone, R.string.opt_drawdone_hint, a.getResources().getStringArray(R.array.drawdone_modes),
               a.prefs.getInt("drawdone_mode", 0), i -> {
                   a.prefs.edit().putInt("drawdone_mode", i).apply();
                   Native.setOption("drawdone_mode", i);
               });
        choice(R.string.opt_core_mode, R.string.opt_core_mode_hint, a.getResources().getStringArray(R.array.core_modes),
               a.prefs.getInt("core_mode", MainActivity.DEFAULT_CORE_MODE), i -> {
                   a.prefs.edit().putInt("core_mode", i).apply();
                   Native.setOption("core_mode", i);
               });
        choice(R.string.opt_resolution, 0, res, indexOf(MainActivity.RES_SCALES, a.prefs.getString("res_scale", "1"), 2), a::setResolution);
        String hot = a.thermalResolution();
        toggleText(a.getString(R.string.opt_dynamic_res), a.getString(R.string.opt_dynamic_res_hint)
                        + (hot != null ? "\n" + a.getString(R.string.dynamic_res_now, hot) : ""),
                a.dynamicResolution(), a::setDynamicResolution);
        submenu(R.string.opt_framegen, 0, a.frameGenLabel(), () -> openPage(this::frameGenPage));
        choice(R.string.opt_render_aspect, R.string.opt_render_aspect_hint, a.getResources().getStringArray(R.array.render_aspect_modes),
               a.prefs.getInt("render_aspect", 0), i -> {
                   a.prefs.edit().putInt("render_aspect", i).apply();
                   Native.setOption("render_aspect", i);
               });
        choice(R.string.opt_aspect, 0, a.getResources().getStringArray(R.array.aspect_modes), a.prefs.getInt("tv_aspect", 0), i -> {
            a.prefs.edit().putInt("tv_aspect", i).apply();
            Native.setOption("tv_aspect", i);
        });
        if (a.hasSecondDisplay()) {
            String[] where = {a.getString(R.string.drc_second_display), a.getString(R.string.drc_in_layout)};
            choice(R.string.opt_drc_display, R.string.opt_drc_display_hint, where, a.drcOnSecondDisplay() ? 0 : 1,
                    i -> a.setDrcOnSecondDisplay(i == 0));
        }
        choice(R.string.opt_layout, 0, a.getResources().getStringArray(R.array.layouts), a.prefs.getInt("layout", MainActivity.LAYOUT_INSET), i -> {
            a.prefs.edit().putInt("layout", i).apply();
            a.updateLayout();
        });
        submenu(R.string.opt_hud, R.string.opt_hud_hint, a.getString(a.hudMoved() ? R.string.opt_buttons_custom : R.string.opt_buttons_default),
                () -> {
                    dismiss();  // the game, with the editor over it
                    a.editHud();
                });
        choice(R.string.opt_ao, 0, a.getResources().getStringArray(R.array.ao_modes), Native.getOption("ao_mode"), a::setAo);
        {  // bloom strength, percent (100: the original game)
            int[] pcts = {0, 25, 50, 75, 100, 125, 150, 200};
            String[] labels = new String[pcts.length];
            int cur = 4, saved = a.prefs.getInt("bloom_strength", 100);
            for (int i = 0; i < pcts.length; i++) {
                labels[i] = pcts[i] == 0 ? a.getString(R.string.opt_off) : pcts[i] + "%";
                if (pcts[i] == saved) cur = i;
            }
            choice(R.string.opt_bloom, R.string.opt_bloom_hint, labels, cur, i -> {
                a.prefs.edit().putInt("bloom_strength", pcts[i]).apply();
                Native.setOption("bloom_strength", pcts[i]);
            });
        }
        toggle(R.string.opt_area_blur, R.string.opt_area_blur_hint, a.prefs.getBoolean("area_blur", false), on -> a.setBool("area_blur", on));
        toggle(R.string.opt_ao_hires, 0, Native.getOption("ao_hires") != 0, on -> a.setBool("ao_hires", on));
        toggle(R.string.opt_aniso, 0, Native.getOption("aniso") != 0, on -> a.setBool("aniso", on));
        toggle(R.string.opt_distant_shadows, R.string.opt_distant_shadows_hint, Native.getOption("distant_shadows") != 0,
                on -> a.setBool("distant_shadows", on));
        toggle(R.string.opt_shaders_at_start, R.string.opt_shaders_at_start_hint, a.prefs.getBoolean("shaders_at_start", true),
                on -> a.prefs.edit().putBoolean("shaders_at_start", on).apply());
        submenu(R.string.opt_perf, R.string.opt_perf_hint, a.getString(a.prefs.getBoolean("perf_hud", false) ? R.string.opt_on : R.string.opt_off),
                () -> openPage(this::perfPage));
        if (GpuDrivers.supported())
            submenu(R.string.opt_gpu_driver, R.string.opt_gpu_driver_hint, a.gpuDriverLabel(), () -> openPage(this::gpuDriverPage));
        choice(R.string.opt_gpu_safe, R.string.opt_gpu_safe_hint, a.getResources().getStringArray(R.array.gpu_safe_modes), a.gpuSafeLevel(), i -> {
            a.setGpuSafeLevel(i);
            a.askRestart(R.string.opt_gpu_safe);
        });
        choice(R.string.opt_record_thread, R.string.opt_record_thread_hint, a.getResources().getStringArray(R.array.record_thread_modes),
                a.prefs.getInt("record_thread", 0), a::setRecordThread);
        submenu(R.string.opt_shaders, R.string.opt_shaders_hint, "", a::askClearShaders);
        if (a.debuggable())  // a debugging aid: debug builds only
            submenu(R.string.opt_capture, R.string.opt_capture_hint, "", () -> Native.setOption("capture", 1));
    }

    // the Brazilian Portuguese fan translation (Translation.java)
    private void ptbrPage() {
        pageTitle(R.string.opt_ptbr);
        note(a.getString(R.string.ptbr_about));
        if (!a.ptbrInstalled()) {
            submenu(R.string.ptbr_install, R.string.ptbr_install_hint, "", () -> a.pickFolder(MainActivity.PICK_TRANSLATION));
            return;
        }
        String label = Translation.label(a.ptbrDir());
        if (!label.isEmpty()) note(a.getString(R.string.ptbr_installed, label));
        toggle(R.string.opt_ptbr, R.string.ptbr_toggle_hint, a.ptbrEnabled(), on -> {
            dismiss();
            a.setPtbr(on);
        });
        submenu(R.string.ptbr_install_other, 0, "", () -> a.pickFolder(MainActivity.PICK_TRANSLATION));
        submenu(R.string.ptbr_remove, 0, "", () -> new GameDialog(getContext()).title(R.string.ptbr_remove).message(R.string.ptbr_remove_confirm)
                .button(R.string.opt_cancel, null)
                .button(R.string.ptbr_remove, () -> {
                    boolean was = a.ptbrEnabled();
                    a.removePtbr();
                    fill();
                    if (was) a.askRestartForLanguage();
                }).show());
    }

    // cheats (runtime/src/mods/cheats.cpp): one-shot save data edits, and infinite health / magic / ammo
    private void cheatsPage() {
        note(a.getString(R.string.cheat_note));
        int[] labels = {R.string.cheat_items, R.string.cheat_sword, R.string.cheat_stats, R.string.cheat_songs,
                        R.string.cheat_triforce, R.string.cheat_dungeon, R.string.cheat_key};
        int[] bits = {1, 2, 4, 8, 16, 32, 64};
        for (int i = 0; i < labels.length; i++) {
            final int bit = bits[i];
            submenu(labels[i], i >= 3 ? R.string.cheat_story_hint : 0, a.getString(R.string.cheat_apply), () -> {
                Native.setOption("cheat", bit);
                a.info(a.getString(R.string.cheat_done));
            });
        }
        String[] keys = {"inf_health", "inf_magic", "inf_ammo", "cheat_moon_jump"};
        int[] names = {R.string.cheat_inf_health, R.string.cheat_inf_magic, R.string.cheat_inf_ammo, R.string.cheat_moon_jump};
        int[] hints = {0, 0, 0, R.string.cheat_moon_jump_hint};
        for (int i = 0; i < keys.length; i++) {
            final String k = keys[i];
            toggle(names[i], hints[i], a.prefs.getBoolean(k, false), on -> {
                a.prefs.edit().putBoolean(k, on).apply();
                Native.setOption(k, on ? 1 : 0);
            });
        }
        // the moon jump's rise and flight speed, in percent of the tested values (from the Android port)
        String[] moonKey = {"cheat_moon_rise", "cheat_moon_fly"};
        int[] moonLabel = {R.string.cheat_moon_rise, R.string.cheat_moon_fly};
        int[][] moonSpeeds = {MainActivity.MOON_RISE_SPEEDS, MainActivity.MOON_FLY_SPEEDS};
        for (int i = 0; i < moonKey.length; i++) {
            String key = moonKey[i];
            int[] speeds = moonSpeeds[i];
            String[] pct = new String[speeds.length];
            for (int j = 0; j < pct.length; j++) pct[j] = speeds[j] + "%";
            int cur = java.util.Arrays.binarySearch(speeds, a.prefs.getInt(key, 100));
            if (cur < 0) cur = java.util.Arrays.binarySearch(speeds, 100);
            choice(moonLabel[i], 0, pct, cur, k -> {
                a.prefs.edit().putInt(key, speeds[k]).apply();
                Native.setOption(key, speeds[k]);
            });
        }
        String[] moonSwitch = {"cheat_moon_nofall", "cheat_moon_anims"};
        int[][] moonSwitchText = {{R.string.cheat_moon_nofall, R.string.cheat_moon_nofall_hint},
                                  {R.string.cheat_moon_anims, R.string.cheat_moon_anims_hint}};
        for (int i = 0; i < moonSwitch.length; i++) {
            final String k = moonSwitch[i];
            toggle(moonSwitchText[i][0], moonSwitchText[i][1], a.prefs.getBoolean(k, false), on -> {
                a.prefs.edit().putBoolean(k, on).apply();
                Native.setOption(k, on ? 1 : 0);
            });
        }
    }

    // the game itself: its language (the release's languages; applies after a restart)
    private void game() {
        int[] langs = a.gameLanguages();
        // Português (Brasil) is the built-in translation (Mods > Portuguese translation): on top of English
        final boolean ptbr = a.ptbrInstalled();
        String[] names = new String[langs.length + (ptbr ? 1 : 0)];
        int curLang = 0;
        for (int i = 0; i < langs.length; i++) {
            names[i] = MainActivity.LANGUAGE_NAMES[langs[i]];
            if (MainActivity.LANGUAGES[langs[i]].equals(a.gameLanguage())) curLang = i;
        }
        if (ptbr) {
            names[langs.length] = a.getString(R.string.language_ptbr);
            if (a.ptbrEnabled()) curLang = langs.length;
        }
        final int startLang = curLang;
        choice(R.string.opt_language, R.string.opt_language_hint, names, curLang, i -> {
            boolean toPtbr = ptbr && i == langs.length;
            if (!toPtbr && !a.ptbrEnabled()) {  // one of the game's own: asked to restart as the menu closes
                a.setGameLanguage(MainActivity.LANGUAGES[langs[i]]);
                return;
            }
            if (i == startLang) return;
            dismiss();  // the translation switched on or off: setPtbr asks to restart
            if (!toPtbr) a.setGameLanguage(MainActivity.LANGUAGES[langs[i]]);
            a.setPtbr(toPtbr);
        });
        choice(R.string.opt_app_language, R.string.opt_app_language_hint, a.getResources().getStringArray(R.array.app_languages),
                a.prefs.getInt("app_language", 0), i -> {
                    a.prefs.edit().putInt("app_language", i).commit();
                    rows.post(() -> {  // the menus again in the new language
                        dismiss();
                        a.recreateUi();
                    });
                });
        String[] sizes = new String[MainActivity.MENU_SIZES.length];
        int curSize = 3;
        for (int i = 0; i < sizes.length; i++) {
            sizes[i] = Math.round(MainActivity.MENU_SIZES[i] * 100) + "%";
            if (Math.abs(MainActivity.MENU_SIZES[i] - a.prefs.getFloat("menu_size", 1f)) < 0.01f) curSize = i;
        }
        choice(R.string.opt_menu_size, R.string.opt_menu_size_hint, sizes, curSize, i -> {
            a.prefs.edit().putFloat("menu_size", MainActivity.MENU_SIZES[i]).apply();
            // laid out again at the new size, where it was
            rows.post(() -> {
                dismiss();
                new OptionsMenu(a).show();
            });
        });
    }

    private void mods() {
        submenu(R.string.opt_ptbr, R.string.opt_ptbr_hint, a.ptbrInstalled() ? a.getString(a.ptbrEnabled() ? R.string.opt_on : R.string.opt_off)
                : a.getString(R.string.ptbr_not_installed), () -> openPage(this::ptbrPage));
        submenu(R.string.opt_cheats, R.string.opt_cheats_hint, "", () -> openPage(this::cheatsPage));
        choice(R.string.opt_button_style, R.string.opt_button_style_hint, a.getResources().getStringArray(R.array.button_styles),
               a.buttonStyle(), a::setButtonStyle);
        int[] labels = {R.string.opt_mod_direct_camera, R.string.opt_mod_first_person, R.string.opt_mod_climb,
                        R.string.opt_mod_quick_doors, R.string.opt_mod_fast_scenes, R.string.opt_mod_ff_cutscenes,
                        R.string.opt_mod_ff_dialogues};
        int[] hints = {R.string.opt_mod_direct_camera_hint, 0, R.string.opt_mod_climb_hint, R.string.opt_mod_speed_hint,
                       R.string.opt_mod_speed_hint, R.string.opt_mod_ff_hint, R.string.opt_mod_ff_hint};
        for (int i = 0; i < MainActivity.MODS.length; i++) {
            String key = MainActivity.MODS[i];
            toggle(labels[i], hints[i], a.prefs.getBoolean(key, false), on -> a.setMod(key, on));
            if (i == 0) {  // its speed right below it
                String[] speeds = new String[MainActivity.CAMERA_SPEEDS.length];
                int cur = 1;
                for (int k = 0; k < speeds.length; k++) {
                    speeds[k] = String.format(java.util.Locale.ROOT, "%.1f×", MainActivity.CAMERA_SPEEDS[k] / 100f);
                    if (MainActivity.CAMERA_SPEEDS[k] == a.prefs.getInt("mod_camera_speed", 100)) cur = k;
                }
                indentNext = true;  // a setting of the mod above
                choice(R.string.opt_mod_camera_speed, 0, speeds, cur, k -> {
                    a.prefs.edit().putInt("mod_camera_speed", MainActivity.CAMERA_SPEEDS[k]).apply();
                    Native.setOption("mod_camera_speed", MainActivity.CAMERA_SPEEDS[k]);
                });
            }
        }
        String[] runs = new String[MainActivity.RUN_SPEEDS.length];
        int curRun = 0, curSwim = 0;
        for (int k = 0; k < runs.length; k++) {
            runs[k] = MainActivity.RUN_SPEEDS[k] == 100 ? a.getString(R.string.opt_mod_run_off)
                    : String.format(java.util.Locale.ROOT, "%.2f×", MainActivity.RUN_SPEEDS[k] / 100f).replace("0×", "×");
            if (MainActivity.RUN_SPEEDS[k] == a.prefs.getInt("mod_run_speed", 100)) curRun = k;
            if (MainActivity.RUN_SPEEDS[k] == a.prefs.getInt("mod_swim_speed", 100)) curSwim = k;
        }
        moveSpeed("mod_run", R.string.opt_mod_run, R.string.opt_mod_run_hint, runs, curRun);
        moveSpeed("mod_swim", R.string.opt_mod_swim, R.string.opt_mod_swim_hint, runs, curSwim);
    }

    // faster running or swimming: its speed, and below it (when on) when it applies and how L3 works
    private void moveSpeed(String mod, int label, int hint, String[] values, int cur) {
        choice(label, hint, values, cur, k -> {
            a.prefs.edit().putInt(mod + "_speed", MainActivity.RUN_SPEEDS[k]).apply();
            Native.setOption(mod + "_speed", MainActivity.RUN_SPEEDS[k]);
        });
        if (MainActivity.RUN_SPEEDS[cur] == 100) return;
        boolean withL3 = a.prefs.getBoolean(mod + "_l3", false), hold = a.prefs.getBoolean(mod + "_l3_hold", false);
        String[] when = {a.getString(R.string.opt_mod_run_always), a.getString(R.string.opt_mod_run_with_l3)};
        indentNext = true;
        choice(R.string.opt_mod_run_when, 0, when, withL3 ? 1 : 0, k -> a.setMoveL3(mod, k == 1, hold));
        if (withL3) {
            String[] how = {a.getString(R.string.opt_mod_run_l3_switch), a.getString(R.string.opt_mod_run_l3_hold)};
            indentNext = true;
            choice(R.string.opt_mod_run_l3, 0, how, hold ? 1 : 0, k -> a.setMoveL3(mod, true, k == 1));
        }
    }

    private void controls() {
        toggle(R.string.opt_onscreen, 0, a.prefs.getBoolean("controls_visible", true), a::setControlsVisible);
        String[] sizes = new String[MainActivity.CONTROL_SIZES.length];
        int cur = 2;
        for (int i = 0; i < sizes.length; i++) {
            sizes[i] = Math.round(MainActivity.CONTROL_SIZES[i] * 100) + "%";
            if (Math.abs(MainActivity.CONTROL_SIZES[i] - a.prefs.getFloat("controls_scale", 1f)) < 0.01f) cur = i;
        }
        choice(R.string.opt_onscreen_size, 0, sizes, cur, i -> {
            a.prefs.edit().putFloat("controls_scale", MainActivity.CONTROL_SIZES[i]).apply();
            a.applyControlsAppearance();
        });
        submenu(R.string.opt_layout_edit, R.string.opt_layout_edit_hint, "", () -> {
            dismiss();
            a.editTouchLayout();
        });
        toggle(R.string.opt_camera_stick, R.string.opt_camera_stick_hint, a.prefs.getBoolean("camera_stick", true), on -> {
            a.prefs.edit().putBoolean("camera_stick", on).apply();
            a.applyControlsAppearance();
        });
        String[] sens = new String[MainActivity.CAMERA_SENSITIVITIES.length];
        int curSens = 2;
        for (int i = 0; i < sens.length; i++) {
            sens[i] = Math.round(MainActivity.CAMERA_SENSITIVITIES[i] * 100) + "%";
            if (Math.abs(MainActivity.CAMERA_SENSITIVITIES[i] - a.prefs.getFloat("camera_sensitivity", 1f)) < 0.01f) curSens = i;
        }
        choice(R.string.opt_camera_sensitivity, R.string.opt_camera_sensitivity_hint, sens, curSens, i -> {
            a.prefs.edit().putFloat("camera_sensitivity", MainActivity.CAMERA_SENSITIVITIES[i]).apply();
            a.applyControlsAppearance();
        });
        choice(R.string.opt_combat_buttons, R.string.opt_combat_buttons_hint, a.getResources().getStringArray(R.array.combat_buttons),
               a.prefs.getInt("combat_buttons", 0), i -> {
                   a.prefs.edit().putInt("combat_buttons", i).apply();
                   a.applyControlsAppearance();
               });
        toggle(R.string.opt_haptics, 0, a.prefs.getBoolean("haptics", true), on -> {
            a.prefs.edit().putBoolean("haptics", on).apply();
            a.applyControlsAppearance();
        });
        String[] kinds = {a.getString(R.string.controller_gamepad), a.getString(R.string.controller_pro)};
        choice(R.string.opt_controller, 0, kinds, a.proControllerSetting() ? 1 : 0, i -> a.setProController(i == 1));
        toggleText(a.getString(R.string.opt_motion), a.getString(R.string.opt_motion_hint) + "\n" + a.motionSourceLabel(),
                a.prefs.getBoolean("motion", true), a::setMotion);
        if (a.prefs.getBoolean("motion", true))
            submenu(R.string.opt_gyro_recalibrate, R.string.opt_gyro_recalibrate_hint, "", a::recalibrateGyro);
        toggle(R.string.opt_rumble, R.string.opt_rumble_hint, a.prefs.getBoolean("rumble", true), a::setRumble);
        toggle(R.string.opt_select_map, R.string.opt_select_map_hint, a.prefs.getBoolean("select_map", true),
                on -> a.prefs.edit().putBoolean("select_map", on).apply());
        submenu(R.string.opt_buttons, R.string.opt_buttons_hint,
                a.getString(a.inputMapper().isDefaultMap() ? R.string.opt_buttons_default : R.string.opt_buttons_custom),
                () -> openPage(this::buttonsPage));
    }

    // controller buttons: each Wii U button and the controller button that presses it
    private void buttonsPage() {
        pageTitle(R.string.opt_buttons);
        note(a.getString(R.string.opt_buttons_note));
        InputMapper m = a.inputMapper();
        String[] names = a.getResources().getStringArray(R.array.wiiu_buttons);
        for (int i = 0; i < InputMapper.WIIU.length; i++) {
            final int n = i;
            LinearLayout r = rowText(names[i], null);
            TextView v = new TextView(getContext());
            v.setText(InputMapper.buttonName(getContext(), m.map[i]) + "  ›");
            v.setTextColor(INK);
            v.setTextSize(16);
            v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
            r.addView(v, new LinearLayout.LayoutParams(-2, -2));
            r.setOnClickListener(x -> new GameDialog(getContext()).title(names[n])
                    .message(a.getString(R.string.opt_buttons_press, names[n], InputMapper.buttonName(getContext(), m.map[n])))
                    .button(R.string.opt_cancel, null)
                    .captureButton(code -> { a.assignButton(n, code); fill(); })
                    .show());
        }
        submenu(R.string.opt_buttons_reset, 0, "", () -> { a.resetButtons(); fill(); });
    }

    private static int indexOf(String[] arr, String v, int fallback) {
        for (int i = 0; i < arr.length; i++)
            if (arr[i].equals(v)) return i;
        return fallback;
    }

    // ---- rows
    interface IntSetter { void set(int v); }
    interface BoolSetter { void set(boolean v); }

    private boolean indentNext;

    private LinearLayout row(int label, int hint) {
        return rowText(a.getString(label), hint != 0 ? a.getString(hint) : null);
    }

    private LinearLayout rowText(String label, String hint) {
        LinearLayout r = new LinearLayout(getContext());
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(px(18), px(10), px(14), px(10));
        r.setMinimumHeight(px(58));
        r.setFocusable(true);
        r.setBackground(states(slot(0x1F000000, 0), slot(GLOW, YELLOW)));
        LinearLayout text = new LinearLayout(getContext());
        text.setOrientation(LinearLayout.VERTICAL);
        TextView t = new TextView(getContext());
        t.setText(label);
        t.setTextColor(INK);
        t.setTextSize(18);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        text.addView(t);
        if (hint != null) {
            TextView h = new TextView(getContext());
            h.setText(hint);
            h.setTextColor(HINT);
            h.setTextSize(13);
            text.addView(h);
        }
        r.addView(text, new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = px(8);
        if (indentNext) lp.leftMargin = px(36);
        indentNext = false;
        rows.addView(r, lp);
        return r;
    }

    private void toggle(int label, int hint, boolean on, BoolSetter set) {
        toggleText(a.getString(label), hint != 0 ? a.getString(hint) : null, on, set);
    }

    private void toggleText(String label, String hint, boolean on, BoolSetter set) {
        LinearLayout r = rowText(label, hint);
        TextView pill = GameUi.pill(getContext(), on);
        r.addView(pill, new LinearLayout.LayoutParams(px(92), px(38)));
        r.setOnClickListener(v -> { set.set(!on); fill(); });
    }

    private void choice(int label, int hint, String[] values, int cur, IntSetter set) {
        choice(a.getString(label), hint != 0 ? a.getString(hint) : null, values, cur, set);
    }

    private void choice(String label, String hint, String[] values, int cur, IntSetter set) {
        LinearLayout r = rowText(label, hint);
        int idx = Math.max(0, Math.min(values.length - 1, cur));
        TextView prev = arrow("◀"), next = arrow("▶");
        TextView value = new TextView(getContext());
        value.setText(values[idx]);
        value.setTextColor(INK);
        value.setTextSize(16);
        value.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        value.setGravity(Gravity.CENTER);
        value.setMinWidth(px(150));
        r.addView(prev, new LinearLayout.LayoutParams(px(40), px(40)));
        r.addView(value, new LinearLayout.LayoutParams(-2, -2));
        r.addView(next, new LinearLayout.LayoutParams(px(40), px(40)));
        Runnable dec = () -> { set.set((idx + values.length - 1) % values.length); fill(); };
        Runnable inc = () -> { set.set((idx + 1) % values.length); fill(); };
        prev.setOnClickListener(v -> dec.run());
        next.setOnClickListener(v -> inc.run());
        r.setOnClickListener(v -> inc.run());
        r.setOnKeyListener((v, code, e) -> {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return false;
            if (code == KeyEvent.KEYCODE_DPAD_LEFT) { dec.run(); return true; }
            if (code == KeyEvent.KEYCODE_DPAD_RIGHT) { inc.run(); return true; }
            return false;
        });
    }

    private void submenu(int label, int hint, String value, Runnable open) {
        LinearLayout r = row(label, hint);
        TextView v = new TextView(getContext());
        v.setText(value.isEmpty() ? "›" : value + "  ›");
        v.setTextColor(INK);
        v.setTextSize(16);
        v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        r.addView(v, new LinearLayout.LayoutParams(-2, -2));
        r.setOnClickListener(x -> open.run());
    }

    private TextView arrow(String s) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextColor(0xFFD9B400);
        t.setTextSize(20);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    private void addAction(LinearLayout col, int label, Runnable run) {
        TextView b = barButton(a.getString(label), v -> run.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, px(54));
        lp.bottomMargin = px(12);
        col.addView(b, lp);
    }

    private TextView barButton(String text, View.OnClickListener click) { return GameUi.barButton(getContext(), text, click); }

    private Drawable slot(int fill, int stroke) { return GameUi.slot(getContext(), fill, stroke); }

    private static StateListDrawable states(Drawable normal, Drawable lit) { return GameUi.states(normal, lit); }
}
