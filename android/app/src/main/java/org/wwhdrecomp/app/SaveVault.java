package org.wwhdrecomp.app;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Map;
import java.util.Properties;
import java.util.TreeMap;

/**
 * Saves that survive uninstalling the app: the game save (files/save), the save states
 * (files/states) and the settings, mirrored to "WindWakerHD Plus" at the top of the phone's storage
 * and brought back by themselves on a new install.
 *
 * The app's own folders are deleted when it is uninstalled, and an update signed with another key
 * has to uninstall the old app first. Files an app writes to the shared storage stay, but a new
 * install can't read the old one's files without "All files access" (MANAGE_EXTERNAL_STORAGE): the
 * player grants it once per install, and from then on the copy is kept up to date (only the files
 * that changed) and a start without a game save takes everything back from it. With the access the
 * automatic backups of earlier versions (Downloads/WindWakerHD Saves, AutoBackup) are read too.
 */
final class SaveVault {
    private SaveVault() {}

    static final String FOLDER = "WindWakerHD Plus";
    private static final String TAG = "wwhd";
    private static final Object LOCK = new Object();
    private static final String SETTINGS = "settings.properties";
    // the save states: slots 1-5 and the autosave's (6), each with its picture
    private static final java.util.regex.Pattern STATE_FILE = java.util.regex.Pattern.compile("slot[1-6]\\.(bin|png)");
    private static String lastSettings;  // what was last written (this process)

    private static boolean test;  // a test build installed beside the app (-PwwhdTestId): a folder of its own

    static File root() { return new File(Environment.getExternalStorageDirectory(), test ? FOLDER + " (test)" : FOLDER); }

    static boolean testBuild(Context c) { return c.getPackageName().endsWith(".test"); }

    static boolean enabled(Context c) {
        test = testBuild(c);
        return Environment.isExternalStorageManager();
    }

    /** Android's page for this app's "All files access" switch */
    static Intent permissionIntent(Context c) {
        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + c.getPackageName()));
        if (i.resolveActivity(c.getPackageManager()) == null) i = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
        return i;
    }

    // ------------------------------------------------------------------ the copy
    /** Copies what changed (game save, save states, settings); safe to call often. */
    static void syncAsync(Context context, File base, SharedPreferences prefs) {
        if (!enabled(context)) return;
        new Thread(() -> {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
            synchronized (LOCK) {
                try {
                    int n = sync(base, prefs);
                    if (n > 0) Log.i(TAG, "save vault: " + n + " files copied to " + root());
                } catch (Exception e) {
                    Log.w(TAG, "save vault failed", e);
                }
            }
        }, "save-vault").start();
    }

    private static int sync(File base, SharedPreferences prefs) throws IOException {
        File r = root();
        if (!r.isDirectory() && !r.mkdirs()) throw new IOException("cannot create " + r);
        // the first copy of an install that didn't take the folder's saves back (the access was
        // allowed later, after a new game began): what another install left there is kept aside
        if (!prefs.getBoolean("vault_synced", false)) {
            if (!prefs.getBoolean("vault_restored", false)) {
                String stamp = new java.text.SimpleDateFormat("yyyy-MM-dd HH.mm", java.util.Locale.ROOT).format(new java.util.Date());
                for (String sub : new String[] {"save", "states"}) {
                    File old = new File(r, sub);
                    if (old.isDirectory() && !old.renameTo(new File(r, sub + " before " + stamp))) throw new IOException("cannot keep " + old);
                }
            }
            prefs.edit().putBoolean("vault_synced", true).apply();
        }
        int copied = 0;
        // the game save, unless it is still the empty one the game made at the first boot (it would
        // replace a real save in the copy; AutoBackup.startedWithoutSave)
        File sav = new File(base, "save/user/cking.sav");
        String fresh = prefs.getString("backup_fresh", null);
        if ("pending".equals(fresh) && sav.isFile()) {  // as AutoBackup does (it may be turned off)
            fresh = AutoBackup.sha1(sav);
            prefs.edit().putString("backup_fresh", fresh).apply();
        }
        boolean emptySave = !sav.isFile() || fresh != null && fresh.equals(AutoBackup.sha1(sav));
        if (!emptySave)
            for (String sub : new String[] {"save/user", "save/common"}) copied += mirror(new File(base, sub), new File(r, sub), null);
        copied += mirror(new File(base, "states"), new File(r, "states"), STATE_FILE.pattern());
        writeSettings(r, prefs);
        return copied;
    }

    /** the files of `src` (matching `pattern`, if any) whose size or time differ from their copy in `dst` */
    private static int mirror(File src, File dst, String pattern) throws IOException {
        File[] files = src.listFiles(File::isFile);
        if (files == null) return 0;
        int n = 0;
        for (File f : files) {
            if (pattern != null && !f.getName().matches(pattern)) continue;
            File d = new File(dst, f.getName());
            if (d.length() == f.length() && Math.abs(d.lastModified() - f.lastModified()) < 2000) continue;
            if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("cannot create " + dst);
            copyFile(f, d);
            n++;
        }
        return n;
    }

    /** through a temporary file: an interrupted copy never replaces a good one */
    private static void copyFile(File src, File dst) throws IOException {
        File tmp = new File(dst.getParentFile(), dst.getName() + ".part");
        try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
        //noinspection ResultOfMethodCallIgnored
        dst.delete();
        if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
        //noinspection ResultOfMethodCallIgnored
        dst.setLastModified(src.lastModified());
    }

    // settings that belong to one install (its backups, crash logs, driver files, Android's grants)
    private static boolean installOnly(String k) {
        return k.startsWith("backup_") || k.startsWith("crash_") || k.startsWith("rotation_") || k.startsWith("asked_")
                || k.equals("restore_offered") || k.equals("exit_seen_ms") || k.equals("gpu_driver") || k.equals("gpu_safe")
                || k.startsWith("vault_");
    }

    private static void writeSettings(File r, SharedPreferences prefs) throws IOException {
        TreeMap<String, String> v = new TreeMap<>();
        for (Map.Entry<String, ?> e : prefs.getAll().entrySet()) {
            Object o = e.getValue();
            String k = e.getKey();
            if (installOnly(k)) continue;
            if (o instanceof Boolean) v.put(k, "b:" + o);
            else if (o instanceof Integer) v.put(k, "i:" + o);
            else if (o instanceof Long) v.put(k, "l:" + o);
            else if (o instanceof Float) v.put(k, "f:" + o);
            else if (o instanceof String) v.put(k, "s:" + o);
        }
        String text = v.toString();
        if (text.equals(lastSettings) && new File(r, SETTINGS).isFile()) return;
        Properties p = new Properties();
        p.putAll(v);
        File tmp = new File(r, SETTINGS + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            p.store(out, "WindWakerHD Android Plus settings");
        }
        if (!tmp.renameTo(new File(r, SETTINGS))) throw new IOException("cannot write " + SETTINGS);
        lastSettings = text;
    }

    // ------------------------------------------------------------------ bringing it back
    /** where a game save to bring back is: the copy, else the newest automatic backup (or null) */
    static File findSave() {
        File own = new File(root(), "save");
        if (new File(own, "user/cking.sav").isFile()) return own;
        File best = null;
        long bestTime = Long.MIN_VALUE;
        File[] installs = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                AutoBackup.FOLDER).listFiles(File::isDirectory);
        if (installs != null)
            for (File d : installs) {
                File s = new File(d, "save/user/cking.sav");
                if (s.isFile() && s.lastModified() > bestTime) {
                    bestTime = s.lastModified();
                    best = new File(d, "save");
                }
            }
        return best;
    }

    static boolean hasSomething() {
        File[] states = new File(root(), "states").listFiles((d, n) -> STATE_FILE.matcher(n).matches());
        return findSave() != null || states != null && states.length > 0 || new File(root(), SETTINGS).isFile();
    }

    /** What restore() brought back. */
    static final class Restored {
        boolean save, settings;
        int states;
    }

    /**
     * A start without a game save: the save, the save states this install doesn't have and the
     * settings (applied over this install's; restart for them to take effect) come back.
     */
    static Restored restore(File base, SharedPreferences prefs) throws IOException {
        Restored out = new Restored();
        synchronized (LOCK) {
            File from = findSave();
            if (from != null && !new File(base, "save/user/cking.sav").exists()) {
                File incoming = new File(base, "save-importing"), save = new File(base, "save");
                Backup.deleteTree(incoming);
                for (String sub : new String[] {"user", "common"}) {
                    File[] files = new File(from, sub).listFiles(File::isFile);
                    if (files == null) continue;
                    File d = new File(incoming, sub);
                    if (!d.isDirectory() && !d.mkdirs()) throw new IOException("cannot create " + d);
                    for (File f : files) copyFile(f, new File(d, f.getName()));
                }
                Backup.deleteTree(save);
                if (!incoming.renameTo(save)) throw new IOException("cannot put the save in place");
                out.save = true;
            }
            File[] states = new File(root(), "states").listFiles((d, n) -> STATE_FILE.matcher(n).matches());
            File dir = new File(base, "states");
            if (states != null)
                for (File f : states) {
                    File d = new File(dir, f.getName());
                    if (d.exists()) continue;
                    if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
                    copyFile(f, d);
                    if (f.getName().endsWith(".bin")) out.states++;
                }
            out.settings = readSettings(prefs);
        }
        return out;
    }

    private static boolean readSettings(SharedPreferences prefs) {
        File f = new File(root(), SETTINGS);
        if (!f.isFile()) return false;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(f)) {
            p.load(in);
        } catch (IOException e) {
            Log.w(TAG, "save vault: cannot read " + f, e);
            return false;
        }
        SharedPreferences.Editor e = prefs.edit();
        int n = 0;
        for (String k : p.stringPropertyNames()) {
            String v = p.getProperty(k);
            if (installOnly(k) || v.length() < 2 || v.charAt(1) != ':') continue;
            String x = v.substring(2);
            try {
                switch (v.charAt(0)) {
                    case 'b': e.putBoolean(k, Boolean.parseBoolean(x)); break;
                    case 'i': e.putInt(k, Integer.parseInt(x)); break;
                    case 'l': e.putLong(k, Long.parseLong(x)); break;
                    case 'f': e.putFloat(k, Float.parseFloat(x)); break;
                    case 's': e.putString(k, x); break;
                    default: continue;
                }
                n++;
            } catch (NumberFormatException ignored) {
            }
        }
        e.commit();
        return n > 0;
    }
}
