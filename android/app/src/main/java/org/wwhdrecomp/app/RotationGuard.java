package org.wwhdrecomp.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.OrientationEventListener;
import android.view.Surface;

/**
 * Keeps the game in landscape where the system ignores the activity's orientation. Some ROMs turn on
 * "fixed to user rotation" at every boot (seen on a Galaxy S20 FE with a custom ROM and a handheld
 * launcher): the display then follows the user's rotation setting only. Samsung's game tools turn
 * auto-rotate on when a game starts, so with the phone upright the display went to portrait with the
 * game in a strip. With "Modify system settings" (Settings.System.canWrite, asked for once with an
 * explanation the first time the game shows in portrait) the rotation is locked to landscape while the
 * game is in the foreground (the side the phone is held, from the sensor; re-locked if something turns
 * auto-rotate back on) and the user's own settings come back when it leaves.
 */
final class RotationGuard {
    private final Activity a;
    private final SharedPreferences prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean resumed, applied;
    private int landscape = Surface.ROTATION_90;  // ROTATION_90 or ROTATION_270, from the sensor
    private OrientationEventListener sensor;
    private ContentObserver observer;

    RotationGuard(Activity a, SharedPreferences prefs) {
        this.a = a;
        this.prefs = prefs;
        restoreLeftover();
    }

    void onResume() {
        resumed = true;
        // with the permission: always locked while playing (before the first frame), so the game is
        // landscape however the system treats the activity's orientation
        if (Settings.System.canWrite(a)) apply();
        // the system rotates (or doesn't) after the window is shown
        handler.postDelayed(this::check, 600);
        handler.postDelayed(this::check, 2000);
    }


    void onPause() {
        resumed = false;
        handler.removeCallbacksAndMessages(null);
        restore();
    }

    private boolean portrait() {
        int r = a.getDisplay() != null ? a.getDisplay().getRotation() : Surface.ROTATION_90;
        return r == Surface.ROTATION_0 || r == Surface.ROTATION_180;
    }

    private void check() {
        if (!resumed || applied || !portrait()) return;
        if (Settings.System.canWrite(a)) {
            apply();
            return;
        }
        if (prefs.getBoolean("rotation_asked", false)) return;
        prefs.edit().putBoolean("rotation_asked", true).apply();
        new GameDialog(a).title(R.string.rotation_title).message(R.string.rotation_message)
                .button(R.string.gpu_driver_later, null)
                .button(R.string.rotation_allow, () -> {
                    Intent i = new Intent(Settings.ACTION_MANAGE_WRITE_SETTINGS, Uri.parse("package:" + a.getPackageName()));
                    try {
                        a.startActivity(i);
                    } catch (RuntimeException ignored) {
                    }
                }).show();
    }

    private void apply() {
        if (!Settings.System.canWrite(a)) return;
        android.content.ContentResolver cr = a.getContentResolver();
        if (!applied) {
            // the user's settings, kept in the preferences too: put back at the next start if the
            // process ends while they are changed
            int accel = Settings.System.getInt(cr, Settings.System.ACCELEROMETER_ROTATION, 1);
            int user = Settings.System.getInt(cr, Settings.System.USER_ROTATION, 0);
            boolean landscapeUser = user == Surface.ROTATION_90 || user == Surface.ROTATION_270;
            // Samsung's game tools turn auto-rotate on as a game starts (before this) and leave it on,
            // and the display then writes the sensor's portrait as the user rotation: what is found
            // here is not always the user's choice. A rotation locked in landscape is (a handheld
            // setup); anything else replaces the saved settings only the first time.
            if (accel != 0 && landscapeUser) accel = 0;  // what the game tools leave of a landscape lock
            SharedPreferences.Editor e = prefs.edit().putBoolean("rotation_locked", true);
            if ((accel == 0 && landscapeUser) || !prefs.contains("rotation_saved_user"))
                e.putInt("rotation_saved_accel", accel).putInt("rotation_saved_user", user);
            e.commit();
            applied = true;
            if (sensor == null)
                sensor = new OrientationEventListener(a) {
                    @Override
                    public void onOrientationChanged(int deg) {
                        if (deg == ORIENTATION_UNKNOWN) return;
                        // the phone's top to the left: 90; to the right: 270 (with some hysteresis)
                        int want = landscape;
                        if (deg >= 240 && deg <= 300) want = Surface.ROTATION_90;
                        else if (deg >= 60 && deg <= 120) want = Surface.ROTATION_270;
                        if (want != landscape) {
                            landscape = want;
                            lock();
                        }
                    }
                };
            if (observer == null)
                observer = new ContentObserver(handler) {
                    @Override
                    public void onChange(boolean self) {
                        if (applied && resumed) lock();  // e.g. a game launcher turning auto-rotate back on
                    }
                };
        }
        sensor.enable();
        cr.registerContentObserver(Settings.System.getUriFor(Settings.System.ACCELEROMETER_ROTATION), false, observer);
        lock();
    }

    private void lock() {
        try {
            android.content.ContentResolver cr = a.getContentResolver();
            if (Settings.System.getInt(cr, Settings.System.ACCELEROMETER_ROTATION, 1) != 0)
                Settings.System.putInt(cr, Settings.System.ACCELEROMETER_ROTATION, 0);
            if (Settings.System.getInt(cr, Settings.System.USER_ROTATION, 0) != landscape)
                Settings.System.putInt(cr, Settings.System.USER_ROTATION, landscape);
        } catch (RuntimeException ignored) {  // the permission was taken back
        }
    }

    private void restore() {
        if (sensor != null) sensor.disable();
        if (observer != null) a.getContentResolver().unregisterContentObserver(observer);
        if (!applied) return;
        applied = false;
        restoreLeftover();
    }

    private void restoreLeftover() {
        if (!prefs.getBoolean("rotation_locked", false) || !Settings.System.canWrite(a)) return;
        final int user = prefs.getInt("rotation_saved_user", 0), accel = prefs.getInt("rotation_saved_accel", 1);
        putBack(user, accel);
        prefs.edit().putBoolean("rotation_locked", false).apply();
        // the game tools change the rotation again as the game leaves the screen (auto-rotate back on,
        // then the sensor's portrait written as the user rotation): the user's settings are put back
        // whenever they change in the next seconds
        final android.content.ContentResolver cr = a.getContentResolver();
        final ContentObserver after = new ContentObserver(handler) {
            @Override
            public void onChange(boolean self) {
                if (!resumed) putBack(user, accel);
            }
        };
        cr.registerContentObserver(Settings.System.getUriFor(Settings.System.ACCELEROMETER_ROTATION), false, after);
        cr.registerContentObserver(Settings.System.getUriFor(Settings.System.USER_ROTATION), false, after);
        handler.postDelayed(() -> cr.unregisterContentObserver(after), 5000);
    }

    private void putBack(int user, int accel) {
        try {
            android.content.ContentResolver cr = a.getContentResolver();
            if (Settings.System.getInt(cr, Settings.System.USER_ROTATION, 0) != user)
                Settings.System.putInt(cr, Settings.System.USER_ROTATION, user);
            if (Settings.System.getInt(cr, Settings.System.ACCELEROMETER_ROTATION, 1) != accel)
                Settings.System.putInt(cr, Settings.System.ACCELEROMETER_ROTATION, accel);
        } catch (RuntimeException ignored) {
        }
    }
}
