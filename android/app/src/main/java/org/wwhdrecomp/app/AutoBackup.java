package org.wwhdrecomp.app;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.MediaStore;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Automatic copy of the game save (files/save: user/ and common/) to the phone's Downloads, in
 * "WindWakerHD Saves/&lt;this install&gt;/save". The app's own folder is deleted when the app is
 * uninstalled (an update signed with another key has to uninstall the old app first); this copy is
 * not, and the import (Backup.java) reads it back. Written through MediaStore: no permission needed.
 * A new install can't overwrite the files of the one before, so each install has its own folder.
 * Only files that changed since the last copy are written (the save is ~11 MB).
 */
final class AutoBackup {
    private AutoBackup() {}

    static final String FOLDER = "WindWakerHD Saves";
    private static final String TAG = "wwhd";
    private static final Object LOCK = new Object();

    /** Copies what changed; safe to call often (runs on a background thread). */
    static void runAsync(Context context, File base, SharedPreferences prefs) {
        // a test build installed beside the app (-PwwhdTestId) never writes over the app's backup
        if (context.getPackageName().endsWith(".test")) return;
        final Context app = context.getApplicationContext();
        new Thread(() -> {
            synchronized (LOCK) {
                try {
                    int n = run(app.getContentResolver(), base, prefs);
                    if (n > 0) Log.i(TAG, "save backup: " + n + " files copied to Downloads/" + FOLDER);
                } catch (Exception e) {
                    Log.w(TAG, "save backup failed", e);
                }
            }
        }, "save-backup").start();
    }

    private static String installFolder(SharedPreferences prefs) {
        String id = prefs.getString("backup_install", "");
        if (id.isEmpty()) {
            id = new SimpleDateFormat("yyyy-MM-dd HH.mm.ss", Locale.ROOT).format(new Date());
            prefs.edit().putString("backup_install", id).apply();
        }
        return id;
    }

    /**
     * This install started without a game save (MainActivity): the game makes an empty one at boot.
     * That one is not backed up (it would be the newest save in the backups and an import would take
     * it): backups start once cking.sav differs from it (the player saved, or a save was imported).
     */
    static void startedWithoutSave(SharedPreferences prefs) {
        if (!prefs.contains("backup_fresh")) prefs.edit().putString("backup_fresh", "pending").apply();
    }

    private static int run(ContentResolver cr, File base, SharedPreferences prefs) throws IOException {
        File save = new File(base, "save"), sav = new File(save, "user/cking.sav");
        if (!sav.isFile()) return 0;  // nothing played yet (or being imported)
        String fresh = prefs.getString("backup_fresh", null);
        if (fresh != null) {
            String hash = sha1(sav);
            if (fresh.equals("pending")) {
                prefs.edit().putString("backup_fresh", hash).apply();
                return 0;
            }
            if (fresh.equals(hash)) return 0;
        }
        String root = android.os.Environment.DIRECTORY_DOWNLOADS + "/" + FOLDER + "/" + installFolder(prefs) + "/save/";
        int copied = 0;
        for (String sub : new String[] {"user", "common"}) {
            File[] files = new File(save, sub).listFiles(File::isFile);
            if (files == null) continue;
            for (File f : files) {
                String key = "backup_" + sub + "/" + f.getName();
                String stamp = f.lastModified() + ":" + f.length();
                if (stamp.equals(prefs.getString(key, ""))) continue;
                write(cr, f, root + sub + "/");
                prefs.edit().putString(key, stamp).apply();
                copied++;
            }
        }
        return copied;
    }

    private static void write(ContentResolver cr, File src, String relativePath) throws IOException {
        Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        Uri target = null;
        // this install's earlier copy of the file (others' files are not visible to it)
        try (Cursor c = cr.query(collection, new String[] {MediaStore.MediaColumns._ID},
                MediaStore.MediaColumns.RELATIVE_PATH + "=? AND " + MediaStore.MediaColumns.DISPLAY_NAME + "=?",
                new String[] {relativePath, src.getName()}, null)) {
            if (c != null && c.moveToFirst()) target = Uri.withAppendedPath(collection, String.valueOf(c.getLong(0)));
        }
        if (target == null) {
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, src.getName());
            v.put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream");
            v.put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath);
            target = cr.insert(collection, v);
            if (target == null) throw new IOException("cannot create " + relativePath + src.getName());
        }
        try (InputStream in = new FileInputStream(src); OutputStream out = cr.openOutputStream(target, "wt")) {
            if (out == null) throw new IOException("cannot write " + target);
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
    }

    private static String sha1(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-1");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest()) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }

    /**
     * Where the folder picker opens for an import (it may ignore it): the backups' folder, else
     * Downloads (where exports usually go; Downloads itself can't be picked, a folder in it can).
     */
    static Uri pickerStart() {
        boolean backups = new File(android.os.Environment.getExternalStoragePublicDirectory(
                android.os.Environment.DIRECTORY_DOWNLOADS), FOLDER).isDirectory();
        return Uri.parse("content://com.android.externalstorage.documents/document/primary%3ADownload"
                + (backups ? "%2F" + Uri.encode(FOLDER) : ""));
    }
}
