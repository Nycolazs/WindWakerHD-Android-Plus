package org.wwhdrecomp.app;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/**
 * The fan translation into Brazilian Portuguese (Triforce-Heroes) as a mod: the user chooses the
 * translation's folder (the Cemu graphic pack or the console version, as they are distributed); its
 * message packs (content/Common/Pack/permanent_2d_*.pack) are copied to files/mods/ptbr/content/...
 * and, while the mod is on, the game reads them instead of its own (runtime/src/hle/fs.cpp, content
 * overlay). The translation replaces the English text, so the game language is set to English.
 * The app also carries it as a patch over the player's own pack (installBuiltIn, PackPatch).
 */
final class Translation {
    private Translation() {}

    static File dir(Context c, File base) { return new File(base, "mods/ptbr"); }

    static File packDir(File dir) { return new File(dir, "content/Common/Pack"); }

    static boolean installed(File dir) {
        File[] packs = packDir(dir).listFiles((d, n) -> n.toLowerCase(Locale.ROOT).startsWith("permanent_2d_") && n.endsWith(".pack"));
        return packs != null && packs.length > 0;
    }

    /** the translation's name and version from its rules.txt, or "" */
    static String label(File dir) {
        File f = new File(dir, "label.txt");
        if (!f.exists()) return "";
        try {
            String l = new String(java.nio.file.Files.readAllBytes(f.toPath()), java.nio.charset.StandardCharsets.UTF_8).trim();
            int nl = l.indexOf('\n');
            return nl >= 0 ? l.substring(0, nl) : l;
        } catch (IOException e) {
            return "";
        }
    }

    /**
     * Copies the message packs found anywhere under `tree` (a folder the user chose) into `dir`.
     * Returns null, or why not.
     */
    static String install(Context c, Uri tree, File dir) {
        ContentResolver cr = c.getContentResolver();
        File packs = packDir(dir);
        File tmp = new File(dir.getParentFile(), "ptbr.part");
        Backup.deleteTree(tmp);
        File tmpPacks = packDir(tmp);
        if (!tmpPacks.mkdirs()) return "cannot create " + tmpPacks;
        int[] copied = {0};
        String[] label = {""};
        try {
            walk(cr, tree, DocumentsContract.getTreeDocumentId(tree), 0, (name, uri) -> {
                String lower = name.toLowerCase(Locale.ROOT);
                if (lower.startsWith("permanent_2d_") && lower.endsWith(".pack")) {
                    File out = new File(tmpPacks, name);
                    if (out.exists()) return;  // the first one found (both versions hold the same files)
                    copy(cr, uri, out);
                    copied[0]++;
                } else if (lower.equals("rules.txt") && label[0].isEmpty()) {
                    label[0] = rulesLabel(cr, uri);
                }
            });
        } catch (IOException e) {
            Backup.deleteTree(tmp);
            return e.getMessage();
        }
        if (copied[0] == 0) {
            Backup.deleteTree(tmp);
            return c.getString(R.string.ptbr_not_found);
        }
        // installed by the user (any language): the built-in Portuguese one doesn't replace it
        try {
            //noinspection ResultOfMethodCallIgnored
            new File(tmp, USER_MARK).createNewFile();
        } catch (IOException ignored) {
        }
        try (OutputStream o = new FileOutputStream(new File(tmp, "label.txt"))) {
            o.write(label[0].getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
        Backup.deleteTree(dir);
        if (!tmp.renameTo(dir)) return "cannot move the translation into place";
        return null;
    }

    static void remove(File dir) { Backup.deleteTree(dir); }

    // ---- the translation built into the app: a patch over the player's own message pack
    // (tools/translation/make_pack_patch.py), so the app carries the translated texts and the
    // changed font glyphs only, nothing of the game. Built into files/mods/ptbr on the first start,
    // again when the patch changes (its CRC in label.txt).
    static final String PATCH_ASSET = "ptbr/patch.bin", LABEL_ASSET = "ptbr/label.txt";
    // in a translation the user installed from a folder (Korean, say): kept, the built-in one never
    // replaces it (until it is removed)
    static final String USER_MARK = "user_installed";
    static final String[] ENGLISH_PACKS = {"permanent_2d_UsEnglish.pack", "permanent_2d_EuEnglish.pack"};

    /** null when the translation is ready (built now or before), else why not */
    static String installBuiltIn(Context c, File dir, File gameDir) {
        byte[] patch;
        String label;
        try {
            patch = readAsset(c, PATCH_ASSET);
            label = new String(readAsset(c, LABEL_ASSET), java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "no built-in translation";
        }
        java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        crc.update(patch);
        String stamp = label + "\n" + Long.toHexString(crc.getValue());
        if (installed(dir) && (stamp.equals(rawLabel(dir)) || new File(dir, USER_MARK).exists())) return null;
        File tmp = new File(dir.getParentFile(), "ptbr.part");
        Backup.deleteTree(tmp);
        File out = packDir(tmp);
        if (!out.mkdirs()) return "cannot create " + out;
        int built = 0;
        String err = null;
        for (String name : ENGLISH_PACKS) {
            File orig = new File(gameDir, "content/Common/Pack/" + name);
            if (!orig.exists()) continue;
            try {
                PackPatch.apply(orig, patch, new File(out, name));
                built++;
            } catch (IOException e) {
                err = name + ": " + e.getMessage();
            }
        }
        if (built == 0) {
            Backup.deleteTree(tmp);
            return err != null ? err : "the game has no English message pack";
        }
        try (OutputStream o = new FileOutputStream(new File(tmp, "label.txt"))) {
            o.write(stamp.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException e) {
            Backup.deleteTree(tmp);
            return e.getMessage();
        }
        Backup.deleteTree(dir);
        if (!tmp.renameTo(dir)) {
            Backup.deleteTree(tmp);
            return "cannot move the translation into place";
        }
        return null;
    }

    static boolean hasBuiltIn(Context c) {
        try (InputStream in = c.getAssets().open(PATCH_ASSET)) {
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static byte[] readAsset(Context c, String name) throws IOException {
        try (InputStream in = c.getAssets().open(name)) {
            return in.readAllBytes();
        }
    }

    private static String rawLabel(File dir) {
        try {
            return new String(java.nio.file.Files.readAllBytes(new File(dir, "label.txt").toPath()), java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    private interface Visitor { void file(String name, Uri uri) throws IOException; }

    private static void walk(ContentResolver cr, Uri tree, String docId, int depth, Visitor v) throws IOException {
        if (depth > 12) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor cur = cr.query(children, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (cur != null && cur.moveToNext()) {
                String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) walk(cr, tree, id, depth + 1, v);
                else v.file(name, DocumentsContract.buildDocumentUriUsingTree(tree, id));
            }
        }
    }

    private static void copy(ContentResolver cr, Uri from, File to) throws IOException {
        try (InputStream in = cr.openInputStream(from); OutputStream out = new FileOutputStream(to)) {
            if (in == null) throw new IOException("cannot read " + from);
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
    }

    // "path = ".../Tradução para PT-BR v2025.08.05 by Triforce-Heroes (100.0%)"": its last part
    private static String rulesLabel(ContentResolver cr, Uri uri) {
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null) return "";
            String s = new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
            for (String line : s.split("\n")) {
                line = line.trim();
                if (line.startsWith("path")) {
                    int q1 = line.indexOf('"'), q2 = line.lastIndexOf('"');
                    if (q1 >= 0 && q2 > q1) {
                        String p = line.substring(q1 + 1, q2);
                        return p.substring(p.lastIndexOf('/') + 1);
                    }
                }
            }
        } catch (IOException ignored) {
        }
        return "";
    }
}
