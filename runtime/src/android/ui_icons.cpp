// Official artwork for the touch controls, taken from the player's own game files at run time (never
// shipped with the app): item, equipment and HUD textures from the 2D pack
// (content/Common/Pack/permanent_2d_*.pack: a SARC of Yaz0-compressed layout SARCs whose timg/
// folder holds BFLIM textures). Each texture is decoded to RGBA8 and written as
// "<name>.rgba" (uint32 width, uint32 height, pixels); the app composes the button icons from them
// (IconForge.java).
#include "ui_icons.h"

#include <dirent.h>

#include <algorithm>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "../runtime.h"
#include "../vk/vk_formats.h"

namespace ui_icons {
namespace {
using Bytes = std::vector<uint8_t>;

bool read_file(const std::string& path, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool yaz0(const Bytes& in, Bytes& out) {
    if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4) != 0) {
        out = in;
        return true;
    }
    out.assign(be32(&in[4]), 0);
    size_t s = 16, d = 0;
    while (d < out.size() && s < in.size()) {
        uint8_t code = in[s++];
        for (int bit = 0; bit < 8 && d < out.size(); bit++, code <<= 1) {
            if (code & 0x80) {
                if (s >= in.size()) return false;
                out[d++] = in[s++];
                continue;
            }
            if (s + 1 >= in.size()) return false;
            uint32_t b1 = in[s], b2 = in[s + 1];
            s += 2;
            size_t dist = ((b1 & 0x0F) << 8 | b2) + 1, len = b1 >> 4;
            if (len == 0) {
                if (s >= in.size()) return false;
                len = in[s++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > d) return false;
            for (size_t i = 0; i < len && d < out.size(); i++, d++) out[d] = out[d - dist];
        }
    }
    return d == out.size();
}

// a file of a SARC archive by name (the archives here are big-endian)
bool sarc_file(const Bytes& b, const char* name, Bytes& out) {
    if (b.size() < 0x14 || memcmp(b.data(), "SARC", 4) != 0) return false;
    uint32_t hdr = be16(&b[4]), data = be32(&b[0xC]);
    size_t p = hdr;
    if (p + 12 > b.size() || memcmp(&b[p], "SFAT", 4) != 0) return false;
    uint32_t n = be16(&b[p + 6]);
    size_t nodes = p + 12, names = nodes + 16 * n + 8;
    if (names > b.size() || memcmp(&b[nodes + 16 * n], "SFNT", 4) != 0) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = &b[nodes + 16 * i];
        uint32_t attr = be32(e + 4), start = be32(e + 8), end = be32(e + 12);
        if (!(attr >> 24)) continue;
        size_t o = names + (attr & 0xFFFFFF) * 4;
        if (o >= b.size() || strcmp((const char*)&b[o], name) != 0) continue;
        if (data + end > b.size() || start > end) return false;
        out.assign(b.begin() + data + start, b.begin() + data + end);
        return true;
    }
    return false;
}

// BFLIM (Wii U): image data, then the FLIM header and the "imag" block (last 0x28 bytes)
bool decode_bflim(const Bytes& b, uint32_t& w, uint32_t& h, Bytes& rgba) {
    if (b.size() < 0x28 || memcmp(&b[b.size() - 0x28], "FLIM", 4) != 0) return false;
    const uint8_t* im = &b[b.size() - 0x14];
    w = be16(im + 8);
    h = be16(im + 10);
    uint8_t fmt = im[14], st = im[15];
    uint32_t dataSize = be32(im + 16);
    struct F { uint32_t gx2, bpb; bool comp; gfx::Convert cv; };
    F f;
    switch (fmt) {
        case 1: f = {0x01, 1, false, gfx::Convert::NONE}; break;    // A8
        case 3: f = {0x07, 2, false, gfx::Convert::NONE}; break;    // LA8
        case 15: case 16: f = {0x34, 8, true, gfx::Convert::BC4U}; break;  // BC4 luminance / alpha
        case 17: f = {0x35, 16, true, gfx::Convert::BC5U}; break;   // BC5 (luminance, alpha)
        case 20: f = {0x41A, 4, false, gfx::Convert::NONE}; break;  // RGBA8 sRGB
        case 21: f = {0x431, 8, true, gfx::Convert::BC1}; break;    // BC1 sRGB
        case 23: f = {0x433, 16, true, gfx::Convert::BC3}; break;   // BC3 sRGB
        default: return false;
    }
    if (dataSize > b.size() || !w || !h) return false;
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)f.gx2, w, h, 1, Latte::E_DIM::DIM_2D,
                                          Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)(st & 31)), 0, 0, &info);
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t swizzle = ((st >> 5) & 7) << 8, pipe = (swizzle >> 8) & 1, bank = (swizzle >> 9) & 3;
    uint32_t bw = f.comp ? (w + 3) / 4 : w, bh = f.comp ? (h + 3) / 4 : h, bpp = f.bpb * 8;
    // linear blocks
    Bytes lin((size_t)bw * bh * f.bpb);
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t off;
            if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, 0, 0, bpp, info.pitch, info.height, 1);
            else if (!Latte::TM_IsMacroTiled(tm))
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, 0, bpp, info.pitch, info.height, tm, false);
            else
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, 0, 0, bpp, info.pitch, info.height, 1, tm, false, pipe, bank);
            if (off + f.bpb <= dataSize) memcpy(&lin[((size_t)y * bw + x) * f.bpb], &b[off], f.bpb);
        }
    // texels: decoded blocks have 4 rows per block row; channels per texel as convert_row writes them
    uint32_t pw = f.comp ? bw * 4 : w, ph = f.comp ? bh * 4 : h;
    uint32_t ch = f.cv == gfx::Convert::BC4U ? 1 : f.cv == gfx::Convert::BC5U ? 2 : f.comp ? 4 : f.bpb;
    Bytes tex((size_t)pw * ph * ch);
    if (f.comp)
        for (uint32_t y = 0; y < bh; y++) gfx::convert_row(f.cv, &lin[(size_t)y * bw * f.bpb], &tex[(size_t)y * 4 * pw * ch], bw, pw * ch);
    else
        tex = lin;
    rgba.assign((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = &tex[((size_t)y * pw + x) * ch];
            uint8_t* d = &rgba[((size_t)y * w + x) * 4];
            switch (fmt) {
                case 1: d[0] = d[1] = d[2] = 255; d[3] = s[0]; break;
                case 3: case 17: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
                case 15: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
                case 16: d[0] = d[1] = d[2] = 255; d[3] = s[0]; break;
                default: memcpy(d, s, 4); break;
            }
        }
    return true;
}

// the 2D pack: any language (the textures used here are the same in all of them)
std::string find_pack(const std::string& gameDir) {
    std::string dir = gameDir + "/content/Common/Pack";
    std::string found;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("permanent_2d_", 0) == 0 && n.size() > 5 && n.compare(n.size() - 5, 5, ".pack") == 0)
                if (found.empty() || n.find("UsEnglish") != std::string::npos) found = dir + "/" + n;
        }
        closedir(d);
    }
    return found;
}
}  // namespace

namespace {
// BFFNT version 3 (Wii U): FINF, then TGLP (glyph sheets), CWDH, CMAP blocks. Sheets are GX2
// surfaces (2D thin tiled, no swizzle) stored bottom-up, RGBA8 (the icon font) or BC4 (the text
// fonts: one channel, the glyphs' coverage); cells are (cw + 1) x (ch + 1) apart with a 1-texel
// border, glyph g on sheet g / (cols * rows), at column g % cols and row (g / cols) % rows.
struct Font {
    uint32_t cw = 0, ch = 0, cols = 0, rows = 0, w = 0, h = 0, off = 0, sheetSize = 0, sheets = 0;
    bool bc4 = false;
    std::vector<std::pair<uint16_t, uint16_t>> cmap;  // code, glyph
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
};

bool parse_font(const uint8_t* b, size_t n, Font& f) {
    if (n < 0x40 || memcmp(b, "FFNT", 4) != 0 || be16(b + 4) != 0xFEFF) return false;
    size_t p = be16(b + 6);
    bool tglp = false;
    while (p + 8 <= n) {
        uint32_t size = be32(b + p + 4);
        if (!memcmp(b + p, "TGLP", 4) && p + 32 <= n) {
            f.cw = b[p + 8];
            f.ch = b[p + 9];
            f.sheets = b[p + 10];
            f.sheetSize = be32(b + p + 12);
            uint16_t fmt = be16(b + p + 18) & 0x3FFF;
            f.cols = be16(b + p + 20);
            f.rows = be16(b + p + 22);
            f.w = be16(b + p + 24);
            f.h = be16(b + p + 26);
            f.off = be32(b + p + 28);
            f.bc4 = fmt == 12;
            bool rgba = fmt == 0 || fmt == 14;
            if (!(rgba || f.bc4) || !f.sheets || !f.w || !f.h || f.w % 4 || f.h % 4) return false;
            if (f.sheetSize != (f.bc4 ? f.w * f.h / 2 : f.w * f.h * 4) || f.off + (size_t)f.sheets * f.sheetSize > n) return false;
            tglp = true;
        } else if (!memcmp(b + p, "CMAP", 4) && p + 20 <= n) {
            uint16_t lo = be16(b + p + 8), hi = be16(b + p + 10), method = be16(b + p + 12);
            size_t q = p + 20;
            if (method == 0 && q + 2 <= n) {
                uint16_t first = be16(b + q);
                for (uint32_t c = lo; c <= hi; c++) f.cmap.push_back({(uint16_t)c, (uint16_t)(first + c - lo)});
            } else if (method == 1) {
                for (uint32_t c = lo; c <= hi && q + 2 * (c - lo) + 2 <= n; c++) {
                    uint16_t g = be16(b + q + 2 * (c - lo));
                    if (g != 0xFFFF) f.cmap.push_back({(uint16_t)c, g});
                }
            } else if (method == 2 && q + 2 <= n) {
                uint16_t cnt = be16(b + q);
                for (uint32_t i = 0; i < cnt && q + 2 + 4 * i + 4 <= n; i++)
                    f.cmap.push_back({be16(b + q + 2 + 4 * i), be16(b + q + 4 + 4 * i)});
            }
        }
        if (size < 8) break;
        p += size;
    }
    if (!tglp || !f.cols || !f.rows) return false;
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)(f.bc4 ? 0x34 : 0x1A), f.w, f.h, f.sheets,
                                          f.sheets > 1 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D,
                                          Latte::MakeGX2TileMode(Latte::E_HWTILEMODE::TM_2D_TILED_THIN1), 0, 0, &f.info);
    return f.info.surfSize <= (uint64_t)f.sheets * f.sheetSize;
}

// byte offset in the file of an element of a sheet: a texel (RGBA8) or a 4x4 block (BC4), in the
// sheet's stored (bottom-up) coordinates
size_t element(const Font& f, uint32_t sheet, uint32_t x, uint32_t y) {
    auto tm = (Latte::E_HWTILEMODE)f.info.hwTileMode;
    uint32_t bpp = f.bc4 ? 64 : 32;
    // the sheets are the slices of one 2D array surface (the tiling's bank and pipe depend on the slice)
    uint32_t a = Latte::TM_IsMacroTiled(tm)
                     ? LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, sheet, 0, bpp, f.info.pitch, f.info.height, 1, tm, false, 0, 0)
                     : LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, sheet, bpp, f.info.pitch, f.info.height, tm, false);
    return f.off + a;
}

void bc4_decode(const uint8_t* blk, uint8_t out[16]) {
    uint8_t v[8] = {blk[0], blk[1]};
    if (v[0] > v[1])
        for (int i = 1; i < 7; i++) v[i + 1] = (uint8_t)(((7 - i) * v[0] + i * v[1]) / 7);
    else {
        for (int i = 1; i < 5; i++) v[i + 1] = (uint8_t)(((5 - i) * v[0] + i * v[1]) / 5);
        v[6] = 0;
        v[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = v[(bits >> (3 * i)) & 7];
}

void bc4_encode(const uint8_t in[16], uint8_t blk[8]) {
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) lo = std::min(lo, in[i]), hi = std::max(hi, in[i]);
    if (hi == lo) {  // flat: every index 0
        blk[0] = hi;
        blk[1] = lo;
        memset(blk + 2, 0, 6);
        return;
    }
    uint8_t v[8] = {hi, lo};
    for (int i = 1; i < 7; i++) v[i + 1] = (uint8_t)(((7 - i) * hi + i * lo) / 7);
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bestErr = 1 << 30;
        for (int k = 0; k < 8; k++) {
            int e = std::abs((int)v[k] - (int)in[i]);
            if (e < bestErr) bestErr = e, best = k;
        }
        bits |= (uint64_t)best << (3 * i);
    }
    blk[0] = hi;
    blk[1] = lo;
    for (int i = 0; i < 6; i++) blk[2 + i] = (uint8_t)(bits >> (8 * i));
}

// a glyph's sheet and top-left texel (picture coordinates, top row first), or false
bool cell_of(const Font& f, uint16_t code, uint32_t& sheet, uint32_t& x, uint32_t& y) {
    for (auto& [c, g] : f.cmap)
        if (c == code) {
            uint32_t per = f.cols * f.rows;
            sheet = g / per;
            uint32_t k = g % per;
            x = (k % f.cols) * (f.cw + 1) + 1;
            y = (k / f.cols) * (f.ch + 1) + 1;
            return sheet < f.sheets && x + f.cw <= f.w && y + f.ch <= f.h;
        }
    return false;
}

// texel (x, y) of a sheet's picture as RGBA (BC4: white with the coverage as alpha)
void get_texel(const Font& f, const uint8_t* font, size_t size, uint32_t sheet, uint32_t x, uint32_t y, uint8_t px[4]) {
    uint32_t sy = f.h - 1 - y;
    if (!f.bc4) {
        size_t o = element(f, sheet, x, sy);
        if (o + 4 <= size) memcpy(px, font + o, 4);
        return;
    }
    size_t o = element(f, sheet, x / 4, sy / 4);
    uint8_t t[16] = {};
    if (o + 8 <= size) bc4_decode(font + o, t);
    px[0] = px[1] = px[2] = 255;
    px[3] = t[(sy % 4) * 4 + x % 4];
}
}  // namespace

bool font_cells(const uint8_t* font, size_t size, const std::vector<uint16_t>& codes, std::vector<uint8_t>& rgba,
                uint32_t& cw, uint32_t& ch) {
    Font f;
    if (!parse_font(font, size, f)) return false;
    cw = f.cw;
    ch = f.ch;
    rgba.assign((size_t)codes.size() * f.cw * f.ch * 4, 0);
    for (size_t i = 0; i < codes.size(); i++) {
        uint32_t s, x0, y0;
        if (!cell_of(f, codes[i], s, x0, y0)) continue;  // not in this font: left transparent
        for (uint32_t y = 0; y < f.ch; y++)
            for (uint32_t x = 0; x < f.cw; x++) get_texel(f, font, size, s, x0 + x, y0 + y, &rgba[((i * f.ch + y) * f.cw + x) * 4]);
    }
    return true;
}

namespace {
// a glyph's widths (CWDH: left, glyph width, advance) set to the whole cell: a redrawn text font
// character (narrow katakana) then shows all of its new picture
void full_width(uint8_t* b, size_t n, const Font& f, uint16_t code) {
    int glyph = -1;
    for (auto& [c, g] : f.cmap)
        if (c == code) glyph = g;
    if (glyph < 0) return;
    size_t p = be16(b + 6);
    while (p + 8 <= n) {
        uint32_t size = be32(b + p + 4);
        if (!memcmp(b + p, "CWDH", 4) && p + 16 <= n) {
            uint16_t first = be16(b + p + 8), last = be16(b + p + 10);
            if (glyph >= first && glyph <= last && p + 16 + 3 * (size_t)(glyph - first) + 3 <= n) {
                uint8_t* e = b + p + 16 + 3 * (glyph - first);
                e[0] = 0;
                e[1] = (uint8_t)f.cw;
                e[2] = (uint8_t)f.cw;
            }
        }
        if (size < 8) break;
        p += size;
    }
}
}  // namespace

bool font_patch(uint8_t* font, size_t size, const std::vector<uint16_t>& codes, const uint8_t* rgba, size_t rgbaSize) {
    Font f;
    if (!parse_font(font, size, f) || rgbaSize < (size_t)codes.size() * f.cw * f.ch * 4) return false;
    if (f.bc4)
        for (uint16_t c : codes) full_width(font, size, f, c);
    for (size_t i = 0; i < codes.size(); i++) {
        uint32_t s, x0, y0;
        if (!cell_of(f, codes[i], s, x0, y0)) continue;
        if (!f.bc4) {
            for (uint32_t y = 0; y < f.ch; y++)
                for (uint32_t x = 0; x < f.cw; x++) {
                    size_t o = element(f, s, x0 + x, f.h - 1 - (y0 + y));
                    if (o + 4 <= size) memcpy(font + o, &rgba[((i * f.ch + y) * f.cw + x) * 4], 4);
                }
            continue;
        }
        // BC4: every block the cell touches is decoded, the cell's texels (alpha) set, encoded again
        uint32_t sy0 = f.h - (y0 + f.ch), sy1 = f.h - 1 - y0;  // the cell's stored rows
        for (uint32_t by = sy0 / 4; by <= sy1 / 4; by++)
            for (uint32_t bx = x0 / 4; bx <= (x0 + f.cw - 1) / 4; bx++) {
                size_t o = element(f, s, bx, by);
                if (o + 8 > size) continue;
                uint8_t t[16];
                bc4_decode(font + o, t);
                for (int k = 0; k < 16; k++) {
                    uint32_t x = bx * 4 + k % 4, sy = by * 4 + k / 4, y = f.h - 1 - sy;
                    if (x < x0 || x >= x0 + f.cw || y < y0 || y >= y0 + f.ch) continue;
                    t[k] = rgba[((i * f.ch + (y - y0)) * f.cw + (x - x0)) * 4 + 3];
                }
                bc4_encode(t, font + o);
            }
    }
    return true;
}

int extract(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests) {
    Bytes pack;
    std::string packPath = find_pack(gameDir);
    if (packPath.empty() || !read_file(packPath, pack)) {
        LOG("[icons] no 2D pack in %s", gameDir.c_str());
        return 0;
    }
    int written = 0;
    std::string lastLayout;
    Bytes layout;
    for (const Request& r : requests) {
        if (r.layout != lastLayout) {
            Bytes packed;
            layout.clear();
            if (sarc_file(pack, (r.layout + ".szs").c_str(), packed)) yaz0(packed, layout);
            lastLayout = r.layout;
        }
        Bytes flim, rgba;
        uint32_t w = 0, h = 0;
        if (layout.empty() || !sarc_file(layout, ("timg/" + r.texture + ".bflim").c_str(), flim) || !decode_bflim(flim, w, h, rgba)) {
            LOG("[icons] %s/%s not found", r.layout.c_str(), r.texture.c_str());
            continue;
        }
        std::string path = outDir + "/" + r.texture + ".rgba";
        if (FILE* f = fopen(path.c_str(), "wb")) {
            uint32_t hdr[2] = {w, h};
            fwrite(hdr, 4, 2, f);
            fwrite(rgba.data(), 1, rgba.size(), f);
            fclose(f);
            written++;
        }
    }
    LOG("[icons] %d of %zu textures from %s", written, requests.size(), packPath.c_str());
    return written;
}
}  // namespace ui_icons
