package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.KeyEvent;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;

/**
 * The controller button style mod (Mods > Button icons): Wii U (the game's own), Xbox or PlayStation.
 * The touch controls' letters follow it, and so do the button icons the game shows in its HUD and
 * text ("Press A"), which come from its icon font (CKingPic.bffnt in the 2D message packs, glyphs at
 * U+E000 and up). Those glyphs are redrawn over the originals (the letter painted out with the
 * button's own shading, the new symbol drawn on top) and the packs are written to files/mods/buttons,
 * read instead of the game's (content overlay; built from the Portuguese translation's packs when it
 * is on). Each Wii U button shows the controller button that presses it (InputMapper's assignment,
 * by Android's positional key codes: A bottom, B right, X left, Y top), so a remapped button shows
 * its own symbol; the touch controls use the default assignment, which is laid out the same way.
 */
final class ButtonStyle {
    private ButtonStyle() {}

    static final int WIIU = 0, XBOX = 1, PLAYSTATION = 2;
    private static final int VERSION = 8;  // bump to rebuild the packs after a change here

    // the icon font's button glyphs
    private static final int G_A = 0xE000, G_B = 0xE001, G_X = 0xE002, G_Y = 0xE003, G_PLUS = 0xE045, G_MINUS = 0xE046,
            G_L = 0xE083, G_R = 0xE084, G_ZL = 0xE085, G_ZR = 0xE086, G_STAR_A = 0xE0A8;
    private static final int[] GLYPHS = {G_A, G_B, G_X, G_Y, G_PLUS, G_MINUS, G_L, G_R, G_ZL, G_ZR, G_STAR_A};
    // the Wii U button each glyph stands for
    private static final int[] GLYPH_BUTTON = {Native.A, Native.B, Native.X, Native.Y, Native.PLUS, Native.MINUS, Native.L,
            Native.R, Native.ZL, Native.ZR, Native.A};

    // The HUD's button guide (top right: A, B, X, Y, R, ZR) writes its letters as text in the text font
    // (CKingMain) over a plain button picture. Its letter panes get a katakana each that the game's
    // English, French and Spanish text never uses, and those glyphs of the text font are redrawn as
    // the style's symbols (in the font's two tones: the fill, a half-covered rim).
    private static final String[] HUD_LAYOUTS = {"CommandGuide_00", "CommandA_00"};
    private static final String[] HUD_PANES = {"T_A_00", "T_A_01", "T_B_00", "T_X_00", "T_Y_00", "T_R_00", "T_ZR_00"};
    private static final int[] HUD_PANE_BUTTON = {Native.A, Native.A, Native.B, Native.X, Native.Y, Native.R, Native.ZR};
    private static final int[] HUD_BUTTONS = {Native.A, Native.B, Native.X, Native.Y, Native.R, Native.ZR};
    private static final int[] HUD_CHARS = {0x30F4, 0x30F5, 0x30F6, 0x30FD, 0x30FE, 0x30EE};  // ヴ ヵ ヶ ヽ ヾ ヮ

    static File dir(File base) { return new File(base, "mods/buttons"); }

    // ---------------------------------------------------------------- symbols
    // what the style calls the controller button with Android key code `key`; null: no symbol of its own
    static String symbol(int style, int key) {
        if (style == XBOX) {
            switch (key) {
                case KeyEvent.KEYCODE_BUTTON_A: return "A";
                case KeyEvent.KEYCODE_BUTTON_B: return "B";
                case KeyEvent.KEYCODE_BUTTON_X: return "X";
                case KeyEvent.KEYCODE_BUTTON_Y: return "Y";
                case KeyEvent.KEYCODE_BUTTON_L1: return "LB";
                case KeyEvent.KEYCODE_BUTTON_R1: return "RB";
                case KeyEvent.KEYCODE_BUTTON_L2: return "LT";
                case KeyEvent.KEYCODE_BUTTON_R2: return "RT";
                case KeyEvent.KEYCODE_BUTTON_START: return "≡";
                case KeyEvent.KEYCODE_BUTTON_SELECT: return "⧉";
                case KeyEvent.KEYCODE_BUTTON_THUMBL: return "LS";
                case KeyEvent.KEYCODE_BUTTON_THUMBR: return "RS";
                default: return null;
            }
        }
        if (style == PLAYSTATION) {
            switch (key) {
                case KeyEvent.KEYCODE_BUTTON_A: return "✕";
                case KeyEvent.KEYCODE_BUTTON_B: return "○";
                case KeyEvent.KEYCODE_BUTTON_X: return "□";
                case KeyEvent.KEYCODE_BUTTON_Y: return "△";
                case KeyEvent.KEYCODE_BUTTON_L1: return "L1";
                case KeyEvent.KEYCODE_BUTTON_R1: return "R1";
                case KeyEvent.KEYCODE_BUTTON_L2: return "L2";
                case KeyEvent.KEYCODE_BUTTON_R2: return "R2";
                case KeyEvent.KEYCODE_BUTTON_START: return "≡";
                case KeyEvent.KEYCODE_BUTTON_SELECT: return "⤴";
                case KeyEvent.KEYCODE_BUTTON_THUMBL: return "L3";
                case KeyEvent.KEYCODE_BUTTON_THUMBR: return "R3";
                default: return null;
            }
        }
        return null;
    }

    private static int keyFor(int[] map, int wiiuButton) {
        for (int i = 0; i < InputMapper.WIIU.length; i++)
            if (InputMapper.WIIU[i] == wiiuButton) return map[i];
        return 0;
    }

    /** The touch controls' labels by Wii U button bit (only the ones the style changes). */
    static Map<Integer, String> touchLabels(int style) {
        Map<Integer, String> m = new HashMap<>();
        if (style == WIIU) return m;
        for (int b : new int[] {Native.A, Native.B, Native.X, Native.Y, Native.L, Native.R, Native.ZL, Native.ZR}) {
            String s = symbol(style, keyFor(InputMapper.DEFAULT_MAP, b));
            if (s != null) m.put(b, s);
        }
        return m;
    }

    // the face buttons' colours on those controllers
    private static int color(String s) {
        switch (s) {
            case "A": return 0xFF3FA535;
            case "B": return 0xFFD9302C;
            case "X": return 0xFF1D74D8;
            case "Y": return 0xFFE8B10D;
            case "✕": return 0xFF5B84D8;
            case "○": return 0xFFE0424A;
            case "□": return 0xFFD46FB6;
            case "△": return 0xFF2BAE8C;
            default: return 0xFF505050;  // the game's dark grey
        }
    }

    // ---------------------------------------------------------------- the game's packs
    /**
     * Makes files/mods/buttons for `style` and the controller assignment `map`, from the packs in
     * `sources` (the translation's, then the game's: the first that has a pack wins). Nothing to do
     * if it is already made for the same inputs. Returns null, or why not.
     */
    static String build(File base, File[] sources, int style, int[] map) {
        File out = dir(base);
        if (style == WIIU) return null;
        Map<String, File> packs = new java.util.TreeMap<>();
        for (File src : sources) {
            File[] l = new File(src, "content/Common/Pack").listFiles((d, n) -> n.startsWith("permanent_2d_") && n.endsWith(".pack"));
            if (l != null) for (File f : l) packs.putIfAbsent(f.getName(), f);
        }
        if (packs.isEmpty()) return "no 2D pack";
        StringBuilder stamp = new StringBuilder("v" + VERSION + " style " + style + " map");
        for (int k : map) stamp.append(' ').append(k);
        for (File f : packs.values()) stamp.append('\n').append(f.getPath()).append(' ').append(f.length()).append(' ').append(f.lastModified());
        File stampFile = new File(out, "stamp.txt");
        try {
            if (stampFile.exists() && new String(Files.readAllBytes(stampFile.toPath()), StandardCharsets.UTF_8).contentEquals(stamp)) return null;
        } catch (IOException ignored) {
        }
        File tmp = new File(out.getParentFile(), "buttons.part");
        Backup.deleteTree(tmp);
        File tmpPacks = new File(tmp, "content/Common/Pack");
        if (!tmpPacks.mkdirs()) return "cannot create " + tmpPacks;
        String err = null;
        int built = 0;
        for (File f : packs.values()) {
            try {
                // the letters in the game's own text font (its glyphs, from this pack)
                byte[] main = PackPatch.readInner(f, "CKingMain_bffnt.szs", "CKingMain.bffnt");
                GameFont gf = main != null ? GameFont.load(main) : null;
                Map<String, Map<String, PackPatch.Edit>> edits = new HashMap<>();
                edits.computeIfAbsent("CKingPic_bffnt.szs", k -> new HashMap<>()).put("CKingPic.bffnt", font -> restyle(font, style, map, gf));
                // (the Japanese text uses those characters; another translation's font may lack them)
                if (!f.getName().contains("Jp") && main != null && hasGlyphs(main, HUD_CHARS)) {
                    edits.computeIfAbsent("CKingMain_bffnt.szs", k -> new HashMap<>()).put("CKingMain.bffnt", font -> hudFont(font, style, map, gf));
                    for (String l : HUD_LAYOUTS)
                        edits.computeIfAbsent(l + ".szs", k -> new HashMap<>()).put("blyt/" + l + ".bflyt", ButtonStyle::hudLayout);
                    // the guide's letters as the game sets them: its messages
                    edits.computeIfAbsent("CommandGuide_00_msbt.szs", k -> new HashMap<>()).put("CommandGuide_00.msbt", ButtonStyle::hudMessages);
                }
                PackPatch.replaceInner(f, edits, new File(tmpPacks, f.getName()));
                built++;
            } catch (IOException e) {
                err = f.getName() + ": " + e.getMessage();
            }
        }
        if (built == 0) {
            Backup.deleteTree(tmp);
            return err;
        }
        try (OutputStream o = new FileOutputStream(new File(tmp, "stamp.txt"))) {
            o.write(stamp.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Backup.deleteTree(tmp);
            return e.getMessage();
        }
        Backup.deleteTree(out);
        if (!tmp.renameTo(out)) return "cannot move " + tmp;
        return err;
    }

    /** whether the packs for the current settings are there (build made them) */
    static boolean ready(File base) { return new File(dir(base), "stamp.txt").exists(); }

    // the icon font with the button glyphs redrawn; throws if it isn't the expected font
    private static byte[] restyle(byte[] font, int style, int[] map, GameFont gf) throws IOException {
        byte[] cells = Native.fontCells(font, GLYPHS);
        if (cells == null) throw new IOException("unexpected icon font");
        ByteBuffer h = ByteBuffer.wrap(cells, 0, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        int w = h.getInt(), ht = h.getInt(), size = w * ht * 4;
        byte[] out = new byte[GLYPHS.length * size];
        for (int i = 0; i < GLYPHS.length; i++) {
            Bitmap bm = Bitmap.createBitmap(w, ht, Bitmap.Config.ARGB_8888);
            int[] px = new int[w * ht];
            for (int p = 0; p < px.length; p++) {
                int o = 8 + i * size + p * 4;
                int r = cells[o] & 0xFF, g = cells[o + 1] & 0xFF, b = cells[o + 2] & 0xFF, a = cells[o + 3] & 0xFF;
                px[p] = a << 24 | r << 16 | g << 8 | b;
            }
            String sym = symbol(style, keyFor(map, GLYPH_BUTTON[i]));
            if (sym != null) {
                erase(px, w, ht, GLYPHS[i] == G_STAR_A ? 130 : 200);
                bm.setPixels(px, 0, w, 0, 0, w, ht);
                draw(new Canvas(bm), sym, w, ht, GLYPHS[i] == G_STAR_A, GLYPHS[i] >= G_L && GLYPHS[i] <= G_ZR, gf);
                bm.getPixels(px, 0, w, 0, 0, w, ht);
            }
            for (int p = 0; p < px.length; p++) {
                int o = i * size + p * 4, c = px[p];
                out[o] = (byte) (c >> 16);
                out[o + 1] = (byte) (c >> 8);
                out[o + 2] = (byte) c;
                out[o + 3] = (byte) (c >>> 24);
            }
        }
        byte[] f = font.clone();
        if (!Native.patchFontCells(f, GLYPHS, out)) throw new IOException("cannot write the icon font");
        return f;
    }

    // the text font with the HUD guide's characters drawn as the buttons' symbols (white: the pane
    // colours them like the letters)
    private static byte[] hudFont(byte[] font, int style, int[] map, GameFont gf) throws IOException {
        byte[] cells = Native.fontCells(font, HUD_CHARS);
        if (cells == null) throw new IOException("unexpected text font");
        ByteBuffer h = ByteBuffer.wrap(cells, 0, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        int w = h.getInt(), ht = h.getInt(), size = w * ht * 4;
        byte[] out = java.util.Arrays.copyOfRange(cells, 8, cells.length);
        for (int i = 0; i < HUD_CHARS.length; i++) {
            String sym = symbol(style, keyFor(map, HUD_BUTTONS[i]));
            if (sym == null) sym = WIIU_NAMES[i];
            Bitmap bm = Bitmap.createBitmap(w, ht, Bitmap.Config.ARGB_8888);
            hudGlyph(new Canvas(bm), sym, w, ht, gf);
            int[] px = new int[w * ht];
            bm.getPixels(px, 0, w, 0, 0, w, ht);
            for (int p = 0; p < px.length; p++) {
                int o = i * size + p * 4;
                out[o] = out[o + 1] = out[o + 2] = (byte) 0xFF;
                out[o + 3] = (byte) (px[p] >>> 24);
            }
        }
        byte[] f = font.clone();
        if (!Native.patchFontCells(f, HUD_CHARS, out)) throw new IOException("cannot write the text font");
        return f;
    }

    private static final String[] WIIU_NAMES = {"A", "B", "X", "Y", "R", "ZR"};

    // a symbol in the text font's style: the shape covered fully, a rim around it half covered
    private static void hudGlyph(Canvas c, String s, int w, int h, GameFont gf) {
        if (gf != null && gf.has(s)) {  // letters: the game's own glyphs, both tones as they are
            Bitmap t = gf.text(s, false, 0);
            float k = Math.min(1f, Math.min((float) w / t.getWidth(), (float) h / t.getHeight()));
            float tw = t.getWidth() * k, th = t.getHeight() * k;
            c.drawBitmap(t, null, new RectF((w - tw) / 2, (h - th) / 2, (w + tw) / 2, (h + th) / 2), new Paint(Paint.FILTER_BITMAP_FLAG));
            return;
        }
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        float u = Math.min(w, h), cx = w / 2f, cy = h * 0.52f;
        Path path = null;
        float r = u * 0.3f;
        switch (s) {
            case "✕":
                path = new Path();
                path.moveTo(cx - r, cy - r); path.lineTo(cx + r, cy + r);
                path.moveTo(cx + r, cy - r); path.lineTo(cx - r, cy + r);
                break;
            case "○":
                path = new Path();
                path.addCircle(cx, cy, r, Path.Direction.CW);
                break;
            case "□":
                path = new Path();
                path.addRect(cx - r * 0.9f, cy - r * 0.9f, cx + r * 0.9f, cy + r * 0.9f, Path.Direction.CW);
                break;
            case "△":
                path = new Path();
                path.moveTo(cx, cy - r * 1.05f); path.lineTo(cx + r * 1.15f, cy + r * 0.85f); path.lineTo(cx - r * 1.15f, cy + r * 0.85f); path.close();
                break;
            default:
                break;
        }
        for (int pass = 0; pass < 2; pass++) {  // the rim (alpha 128), then the fill (255)
            p.setColor(pass == 0 ? 0x80FFFFFF : 0xFFFFFFFF);
            if (path != null) {
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(u * (pass == 0 ? 0.2f : 0.11f));
                c.drawPath(path, p);
            } else {
                p.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
                p.setTextAlign(Paint.Align.CENTER);
                p.setTextSize(s.length() == 1 ? u * 0.78f : u * 0.56f);
                p.setStyle(pass == 0 ? Paint.Style.FILL_AND_STROKE : Paint.Style.FILL);
                p.setStrokeWidth(u * 0.09f);
                c.drawText(s, cx, cy - (p.descent() + p.ascent()) / 2, p);
            }
        }
    }

    // the HUD guide's layout with its letter panes showing the redrawn characters
    private static byte[] hudLayout(byte[] l) throws IOException {
        ByteBuffer b = ByteBuffer.wrap(l);
        if (l.length < 0x14 || b.getInt(0) != 0x464C5954) throw new IOException("not a layout");
        int p = b.getShort(6) & 0xFFFF;
        byte[] out = l.clone();
        ByteBuffer o = ByteBuffer.wrap(out);
        while (p + 8 <= l.length) {
            int magic = b.getInt(p), size = b.getInt(p + 4);
            if (size < 8) break;
            if (magic == 0x74787431 && p + 0x68 <= l.length) {  // txt1
                int z = p + 0x0C;
                while (z < p + 0x24 && l[z] != 0) z++;
                String name = new String(l, p + 0x0C, z - (p + 0x0C), StandardCharsets.US_ASCII);
                for (int i = 0; i < HUD_PANES.length; i++) {
                    if (!HUD_PANES[i].equals(name)) continue;
                    int buf = b.getShort(p + 0x54) & 0xFFFF, text = p + b.getInt(p + 0x64);
                    if (buf < 4 || text + buf > l.length) continue;
                    int ch = HUD_CHARS[indexOf(HUD_BUTTONS, HUD_PANE_BUTTON[i])];
                    o.putShort(text, (short) ch);
                    for (int k = 2; k < buf; k++) out[text + k] = 0;
                    o.putShort(p + 0x56, (short) 4);
                }
            }
            p += size;
        }
        return out;
    }

    // the guide's messages: an entry that is just a button's letter ("A", "ZR"...) becomes its character
    private static byte[] hudMessages(byte[] m) throws IOException {
        ByteBuffer b = ByteBuffer.wrap(m);
        if (m.length < 0x20 || b.getLong(0) != 0x4D7367537464426EL || b.getShort(8) != (short) 0xFEFF)
            throw new IOException("not a big-endian message file");
        byte[] out = m.clone();
        ByteBuffer o = ByteBuffer.wrap(out);
        int p = 0x20;
        while (p + 0x10 <= m.length) {
            int magic = b.getInt(p), size = b.getInt(p + 4), data = p + 0x10;
            if (magic == 0x54585432 && data + 4 <= m.length) {  // TXT2: count, offsets, UTF-16 strings
                int n = b.getInt(data);
                for (int i = 0; i < n; i++) {
                    int s = data + b.getInt(data + 4 + 4 * i), e = i + 1 < n ? data + b.getInt(data + 8 + 4 * i) : data + size;
                    if (s < data || e > m.length || e <= s) continue;
                    String t = new String(m, s, e - s, StandardCharsets.UTF_16BE);
                    int z = t.indexOf('\0');
                    if (z >= 0) t = t.substring(0, z);
                    for (int k = 0; k < WIIU_NAMES.length; k++) {
                        if (!WIIU_NAMES[k].equals(t)) continue;
                        o.putShort(s, (short) HUD_CHARS[k]);
                        for (int q = s + 2; q < e; q++) out[q] = 0;
                    }
                }
            }
            p = (data + size + 15) & ~15;
        }
        return out;
    }

    // whether the font draws every one of `codes` (a code it lacks comes back as an empty cell)
    private static boolean hasGlyphs(byte[] font, int[] codes) {
        byte[] cells = Native.fontCells(font, codes);
        if (cells == null) return false;
        ByteBuffer h = ByteBuffer.wrap(cells, 0, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        int size = h.getInt() * h.getInt() * 4;
        for (int i = 0; i < codes.length; i++) {
            boolean any = false;
            for (int p = 3; p < size && !any; p += 4) any = cells[8 + i * size + p] != 0;
            if (!any) return false;
        }
        return true;
    }

    private static int indexOf(int[] a, int v) {
        for (int i = 0; i < a.length; i++) if (a[i] == v) return i;
        return 0;
    }

    // paints out the dark letter of a glyph: each dark texel takes the average colour of the light
    // ones in its row (the button's shading runs top to bottom)
    private static void erase(int[] px, int w, int h, int dark) {
        for (int y = 0; y < h; y++) {
            long r = 0, g = 0, b = 0;
            int n = 0;
            for (int x = 0; x < w; x++) {
                int c = px[y * w + x];
                if ((c >>> 24) < 250 || lum(c) < dark) continue;
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
                n++;
            }
            if (n == 0) continue;
            int fill = 0xFF000000 | (int) (r / n) << 16 | (int) (g / n) << 8 | (int) (b / n);
            // the letter: dark texels away from the outline (an outline texel has a transparent neighbour)
            for (int x = 1; x < w - 1; x++) {
                int c = px[y * w + x];
                if ((c >>> 24) < 250 || lum(c) >= dark) continue;
                boolean edge = false;
                for (int dy = -2; dy <= 2 && !edge; dy++)
                    for (int dx = -2; dx <= 2 && !edge; dx++) {
                        int xx = x + dx, yy = y + dy;
                        edge = xx < 0 || yy < 0 || xx >= w || yy >= h || (px[yy * w + xx] >>> 24) < 128;
                    }
                if (!edge) px[y * w + x] = fill;
            }
        }
    }

    private static int lum(int c) { return (((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF)) / 10; }

    // the new symbol over the cleaned glyph
    private static void draw(Canvas c, String s, int w, int h, boolean star, boolean plate, GameFont gf) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        float cx = w / 2f, cy = h / 2f, u = Math.min(w, h);
        // on the action star: letters dark as the game's, PlayStation's shapes in their colours
        int col = star && s.length() == 1 && Character.isLetter(s.charAt(0)) ? 0xFF404040 : color(s);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        switch (s) {
            case "✕": case "○": case "□": case "△": {
                float r = u * 0.2f;
                Path path = new Path();
                if (s.equals("✕")) {
                    path.moveTo(cx - r, cy - r); path.lineTo(cx + r, cy + r);
                    path.moveTo(cx + r, cy - r); path.lineTo(cx - r, cy + r);
                } else if (s.equals("○")) {
                    path.addCircle(cx, cy, r, Path.Direction.CW);
                } else if (s.equals("□")) {
                    path.addRect(cx - r * 0.9f, cy - r * 0.9f, cx + r * 0.9f, cy + r * 0.9f, Path.Direction.CW);
                } else {
                    path.moveTo(cx, cy - r * 1.05f); path.lineTo(cx + r * 1.1f, cy + r * 0.8f); path.lineTo(cx - r * 1.1f, cy + r * 0.8f); path.close();
                }
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(u * 0.12f);  // a dark rim, then the colour
                p.setColor(0x99202020);
                c.drawPath(path, p);
                p.setStrokeWidth(u * 0.08f);
                p.setColor(col);
                c.drawPath(path, p);
                return;
            }
            case "≡": {
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(u * 0.07f);
                p.setColor(0xFF505050);
                for (int i = -1; i <= 1; i++) c.drawLine(cx - u * 0.17f, cy + i * u * 0.12f, cx + u * 0.17f, cy + i * u * 0.12f, p);
                return;
            }
            case "⧉": {
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(u * 0.06f);
                p.setColor(0xFF505050);
                float a = u * 0.12f;
                c.drawRoundRect(new RectF(cx - a * 1.4f, cy - a * 1.4f, cx + a * 0.4f, cy + a * 0.4f), a * 0.3f, a * 0.3f, p);
                c.drawRoundRect(new RectF(cx - a * 0.4f, cy - a * 0.4f, cx + a * 1.4f, cy + a * 1.4f), a * 0.3f, a * 0.3f, p);
                return;
            }
            case "⤴": {  // PlayStation's Create: a tray with an arrow out of it
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(u * 0.065f);
                p.setColor(0xFF505050);
                float a = u * 0.17f;
                c.drawLine(cx, cy + a * 0.5f, cx, cy - a, p);
                c.drawLine(cx - a * 0.5f, cy - a * 0.45f, cx, cy - a, p);
                c.drawLine(cx + a * 0.5f, cy - a * 0.45f, cx, cy - a, p);
                Path tray = new Path();
                tray.moveTo(cx - a, cy + 0.1f * a); tray.lineTo(cx - a, cy + a); tray.lineTo(cx + a, cy + a); tray.lineTo(cx + a, cy + 0.1f * a);
                c.drawPath(tray, p);
                return;
            }
            default: {  // letters: the controller's colour (face buttons), dark grey (shoulders, sticks)
                if (gf != null && gf.has(s)) {  // in the game's own text font
                    boolean face = s.length() == 1 && !star;
                    Bitmap t = gf.text(s, true, col);
                    float boxH = u * (s.length() == 1 ? (star ? 0.44f : 0.46f) : plate ? 0.38f : 0.36f), boxW = u * (plate ? 0.6f : 0.5f);
                    float k = Math.min(boxW / t.getWidth(), boxH / t.getHeight());
                    float tw = t.getWidth() * k, th = t.getHeight() * k;
                    RectF r = new RectF(cx - tw / 2, cy - th / 2, cx + tw / 2, cy + th / 2);
                    if (face) {  // a dark rim so the colour reads on the white button
                        Paint rim = new Paint(Paint.FILTER_BITMAP_FLAG);
                        rim.setColorFilter(new android.graphics.PorterDuffColorFilter(0x99202020, android.graphics.PorterDuff.Mode.SRC_IN));
                        float o = u * 0.025f;
                        for (int dx = -1; dx <= 1; dx++)
                            for (int dy = -1; dy <= 1; dy++)
                                if (dx != 0 || dy != 0) c.drawBitmap(t, null, new RectF(r.left + dx * o, r.top + dy * o, r.right + dx * o, r.bottom + dy * o), rim);
                    }
                    c.drawBitmap(t, null, r, new Paint(Paint.FILTER_BITMAP_FLAG));
                    return;
                }
                p.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
                p.setTextAlign(Paint.Align.CENTER);
                float size = s.length() == 1 ? u * 0.5f : u * (plate ? 0.36f : 0.34f);
                p.setTextSize(size);
                float y = cy - (p.descent() + p.ascent()) / 2;
                if (s.length() == 1 && !star) {  // a dark rim so the colour reads on the white button
                    p.setStyle(Paint.Style.STROKE);
                    p.setStrokeWidth(u * 0.06f);
                    p.setColor(0x99202020);
                    c.drawText(s, cx, y, p);
                }
                p.setStyle(Paint.Style.FILL);
                p.setColor(col);
                c.drawText(s, cx, y, p);
            }
        }
    }

    /** The game's text font (CKingMain) as pictures of its glyphs, for writing labels in it. */
    private static final class GameFont {
        private final Map<Character, Bitmap> glyphs = new HashMap<>();  // trimmed to the glyph, alpha: coverage
        private int h;

        static GameFont load(byte[] font) {
            String chars = "ABXYLRTSZ0123456789";
            int[] codes = new int[chars.length()];
            for (int i = 0; i < codes.length; i++) codes[i] = chars.charAt(i);
            byte[] cells = Native.fontCells(font, codes);
            if (cells == null) return null;
            ByteBuffer hb = ByteBuffer.wrap(cells, 0, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            int w = hb.getInt(), ht = hb.getInt(), size = w * ht * 4;
            GameFont gf = new GameFont();
            gf.h = ht;
            for (int i = 0; i < codes.length; i++) {
                int[] px = new int[w * ht];
                int x0 = w, x1 = -1;
                for (int p = 0; p < px.length; p++) {
                    int a = cells[8 + i * size + p * 4 + 3] & 0xFF;
                    px[p] = a << 24 | 0xFFFFFF;
                    if (a > 8) { x0 = Math.min(x0, p % w); x1 = Math.max(x1, p % w); }
                }
                if (x1 < x0) continue;  // not in the font
                Bitmap full = Bitmap.createBitmap(px, w, ht, Bitmap.Config.ARGB_8888);
                gf.glyphs.put(chars.charAt(i), Bitmap.createBitmap(full, x0, 0, x1 - x0 + 1, ht));
            }
            return gf;
        }

        boolean has(String s) {
            for (char ch : s.toCharArray()) if (!glyphs.containsKey(ch)) return false;
            return !s.isEmpty();
        }

        /**
         * `s` in the font, trimmed to its rows. fillOnly: the fully covered part only (the font's
         * half-covered rim left out), in `color`; else both tones in white as the font has them.
         */
        Bitmap text(String s, boolean fillOnly, int color) {
            int gap = Math.max(1, h / 30), width = 0;
            for (char ch : s.toCharArray()) width += glyphs.get(ch).getWidth() + gap;
            width -= gap;
            Bitmap out = Bitmap.createBitmap(Math.max(1, width), h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(out);
            int x = 0;
            for (char ch : s.toCharArray()) {
                Bitmap g = glyphs.get(ch);
                c.drawBitmap(g, x, 0, null);
                x += g.getWidth() + gap;
            }
            int[] px = new int[out.getWidth() * h];
            out.getPixels(px, 0, out.getWidth(), 0, 0, out.getWidth(), h);
            int y0 = h, y1 = -1;
            for (int i = 0; i < px.length; i++) {
                int a = px[i] >>> 24;
                if (fillOnly) a = Math.max(0, Math.min(255, (a - 150) * 255 / 90));
                px[i] = fillOnly ? a << 24 | (color & 0xFFFFFF) : px[i];
                if ((px[i] >>> 24) > 8) { y0 = Math.min(y0, i / out.getWidth()); y1 = Math.max(y1, i / out.getWidth()); }
            }
            out.setPixels(px, 0, out.getWidth(), 0, 0, out.getWidth(), h);
            return y1 < y0 ? out : Bitmap.createBitmap(out, 0, y0, out.getWidth(), y1 - y0 + 1);
        }
    }
}
