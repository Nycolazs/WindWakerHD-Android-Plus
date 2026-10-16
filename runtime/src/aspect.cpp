// Aspect ratio of the TV picture: 16:9 (original), the window's shape, 16:10, 21:9 or 32:9.
//
// The game keeps drawing its 1280x720 guest screen. What changes:
//  - 3D: the camera's projection uses the new aspect with the same vertical field of view (Hor+:
//    wider screens see more to the sides). For screens narrower than 16:9 the horizontal field of
//    view is kept and the vertical one grows (Vert+), so nothing of the original view is lost.
//    The culling frustum (mDoLib_clipper) gets the same values, so nothing pops at the new edges.
//  - Render targets (metal_surfaces.mm): screen-shaped buffers are allocated A/(16/9) times wider
//    (or taller) than their guest size; draws keep their guest viewports, which now cover the
//    wider texture, so every full-screen pass (post effects, depth sampling, sea, sky) lines up.
//  - 2D: the HD layouts (nw::lyt) drawn to the TV keep their proportions, centred, and panes near
//    the screen edges move out to the new edges (see "HD layouts" below).
//
// Game side (WWHD addresses, tools/recomp/hooks_aspect.txt):
//   camera_execute 024FFA3C: view.mAspect (+0xD8) = 16/9 constant 1004AAF0 every step   @024FFA98
//   init_phase2    02501F98: the same at camera creation                                @025020E0
//   camera_draw    024FFC40: C_MTXPerspective(proj, mFovy, mAspect, mNear, mFar)         @024FFD60
//   view_setup     024F8068: mDoLib_clipper::setup(mFovy, mAspect, mNear, far) (2 calls)  @024F811C @024F8168
//   dScnName_c::setView 025ADB04: C_MTXPerspective for the file-select scene             @025ADB38
//   nw::lyt::DrawInfo::LoadProjectionMtx 02874038, Pane::CalculateMtx 028766CC, Pane::Draw 02877100
// The aspect is latched once per frame on the main thread at GX2SwapScanBuffers; the render thread
// switches its render-target factors at the matching swap (the value travels with the swap
// command), so a change never lands in the middle of a frame.
#include "aspect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gx2/gx2_cmd.h"
#include "runtime.h"

namespace aspect {
namespace {
constexpr float kBase = 16.0f / 9.0f;

int parse_env_mode(float& custom) {
    const char* e = getenv("WWHD_ASPECT");
    if (!e || !*e) return kOriginal;
    if (!strcmp(e, "window") || !strcmp(e, "match")) return kWindow;
    if (!strcmp(e, "16:9")) return kOriginal;
    if (!strcmp(e, "16:10")) return k16x10;
    if (!strcmp(e, "21:9")) return k21x9;
    if (!strcmp(e, "32:9")) return k32x9;
    float a = 0;
    if (const char* c = strchr(e, ':')) a = (float)atof(e) / (float)atof(c + 1);
    else a = (float)atof(e);
    if (!(a > 0.5f && a < 8.0f)) return kOriginal;
    custom = a;
    return kCustom;
}
float g_custom = kBase;
std::atomic<int> g_mode{parse_env_mode(g_custom)};
std::atomic<float> g_window{kBase};
// aspect of the frame being built: written by the main thread at the swap, read by the threads
// that compute and record the frame
std::atomic<float> g_game{kBase};

float clamp_aspect(float a) { return std::fmin(std::fmax(a, 1.0f), 4.0f); }

// horizontal/vertical widening of the 16:9 view for aspect a
void factors(float a, float& kx, float& ky) {
    kx = a >= kBase ? a / kBase : 1.0f;
    ky = a >= kBase ? 1.0f : kBase / a;
}

bool same(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::fmax(1.0f, std::fabs(a)); }

// the main camera's projection: is this the game's standard aspect (16:9 or one we put there)?
// aspects this module stored into the camera (a switch back to 16:9 replaces leftovers)
float g_stored[4] = {};
bool ours(float v) {
    for (float x : g_stored)
        if (x && same(v, x)) return true;
    return false;
}
void remember(float v) {
    if (same(v, kBase) || ours(v)) return;
    memmove(g_stored + 1, g_stored, sizeof g_stored - sizeof *g_stored);
    g_stored[0] = v;
}
bool standard_aspect(float v) { return same(v, kBase) || same(v, g_game) || ours(v); }
float vert_plus_fovy(float fovyDeg, float ky) {
    if (ky == 1.0f) return fovyDeg;
    float t = std::tan(fovyDeg * 0.5f * (float)M_PI / 180.0f) * ky;
    return 2.0f * std::atan(t) * 180.0f / (float)M_PI;
}
// f(fovy), f(aspect) of a projection / clipper call of the main camera
// at 16:9 every hook leaves the game's values untouched (bit-identical original behaviour)
bool original() { return g_game == kBase; }
void adjust_projection_args(Cpu* c, int fovyReg, int aspectReg) {
    if (original() && !ours((float)c->f[aspectReg].ps0)) return;
    if (!standard_aspect((float)c->f[aspectReg].ps0)) return;  // a special aspect (demo): leave it
    remember(g_game);
    float kx, ky;
    factors(g_game, kx, ky);
    c->f[aspectReg].ps0 = c->f[aspectReg].ps1 = g_game;
    c->f[fovyReg].ps0 = c->f[fovyReg].ps1 = vert_plus_fovy((float)c->f[fovyReg].ps0, ky);
}
}  // namespace

const char* mode_name(int m) {
    static const char* n[kModes] = {"16:9 (original)", "Match window", "16:10", "21:9", "32:9", "custom"};
    return m >= 0 && m < kModes ? n[m] : "?";
}
float mode_value(int m) {
    switch (m) {
    case kOriginal: return kBase;
    case k16x10: return 16.0f / 10.0f;
    case k21x9: return 64.0f / 27.0f;  // 2560x1080 / 3440x1440-class monitors sold as 21:9
    case k32x9: return 32.0f / 9.0f;
    case kCustom: return g_custom;
    default: return 0;
    }
}
int mode() { return g_mode.load(std::memory_order_relaxed); }
void set_mode(int m) {
    if (m < 0 || m >= kModes) return;
    g_mode = m;
    LOG("[aspect] %s", mode_name(m));
}
void set_window_aspect(float a) {
    if (!(a > 0.1f && a < 20.0f)) return;
    // window resizes are continuous; steps of 0.5% keep render targets from being remade every frame
    float q = std::round(a * 200.0f) / 200.0f;
    if (std::fabs(q - kBase) < 0.006f) q = kBase;  // a 16:9 window (rounded pixel sizes) is 16:9
    g_window.store(q, std::memory_order_relaxed);
}
float requested() {
    int m = mode();
    return clamp_aspect(m == kWindow ? g_window.load(std::memory_order_relaxed) : mode_value(m));
}
float game() { return g_game; }
static uint64_t g_swaps = 0, g_changed_swap = 0;
uint64_t game_frame() { return g_swaps; }

float on_swap() {
    // test aid: WWHD_ASPECT_AT=frame:aspect,... switches the aspect at those swaps (aspect 0 = window)
    static uint64_t swaps = 0;
    static std::vector<std::pair<uint64_t, float>> at = [] {
        std::vector<std::pair<uint64_t, float>> v;
        if (const char* e = getenv("WWHD_ASPECT_AT"))
            for (char* p = (char*)e; *p;) {
                uint64_t f = strtoull(p, &p, 10);
                if (*p++ != ':') break;
                v.push_back({f, (float)strtod(p, &p)});
                while (*p == ',') p++;
            }
        return v;
    }();
    swaps++;
    g_swaps = swaps;
    for (auto& [f, v] : at)
        if (f == swaps) {
            if (v == 0) set_mode(kWindow);
            else if (same(v, kBase)) set_mode(kOriginal);
            else { g_custom = v; set_mode(kCustom); }
        }
    float a = requested();
    bool changed = !same(a, g_game);
    // the GPU work of the frame being submitted now was built with the camera of the previous
    // frame (the painter draws the previous pass's lists): render targets follow one swap later
    float render = g_game.load();
    g_game = a;
    if (changed) {
        LOG("[aspect] game aspect %.4f from swap %llu", a, (unsigned long long)swaps);
        g_changed_swap = swaps;
    }
    return render;
}

}  // namespace aspect

// ---- instruction hooks (tools/recomp/hooks_aspect.txt)
using namespace aspect;
// camera_execute / init_phase2: stfs f0 (16/9), view.mAspect
// (the stored value is the 16/9 constant; at 16:9 nothing is touched)
static void store_aspect(Cpu* c) {
    if (original() || !standard_aspect((float)c->f[0].ps0)) return;
    remember(game());
    c->f[0].ps0 = c->f[0].ps1 = game();
}
extern "C" void site_024FFA98(Cpu* c) { store_aspect(c); }
extern "C" void site_025020E0(Cpu* c) { store_aspect(c); }
// camera_draw: C_MTXPerspective(view.mProjMtx, f1 fovy, f2 aspect, f3 near, f4 far)
extern "C" void site_024FFD60(Cpu* c) { adjust_projection_args(c, 1, 2); }
// view_setup: mDoLib_clipper::setup(f1 fovy, f2 aspect, f3 near, f4 far)
extern "C" void site_024F811C(Cpu* c) { adjust_projection_args(c, 1, 2); }
extern "C" void site_024F8168(Cpu* c) { adjust_projection_args(c, 1, 2); }
// dScnName_c::setView: C_MTXPerspective (file select)
extern "C" void site_025ADB38(Cpu* c) { adjust_projection_args(c, 1, 2); }

// ---- HD layouts (nw::lyt, WWHD's nw4f): TV draws see the 16:9 layout space centred in the wider
// screen, panes near the left/right edges move out to the new edges.
//   DrawInfo::LoadProjectionMtx 02874038: GX2SetVertexUniformReg(uProjection, 16, DrawInfo+0).
//     The layers' DrawInfos hold a perspective projection (fovy 40, aspect 16/9; the layout plane
//     at z = -989 maps 1 unit to 1 pixel). For TV-shaped render targets the x row is divided by kx
//     (y row by ky) for the upload: everything keeps its proportions, centred.
//   Pane (nw::lyt::Pane): +0x00 sibling link (next), +0x08 vtable, +0x0C parent, +0x14 child list
//     sentinel, +0x1C translate x/y/z, +0x34 scale x/y, +0x3C size w/h, +0x44 flags, +0x48 global
//     matrix 3x4, +0x80 name. Pane::CalculateMtx 028766CC (vtable, recursive over children),
//     Pane::Draw 02877100 (plain panes; layout roots).
//   Which screen a layout goes to is only known on the render thread (TV and GamePad layouts share
//     DrawInfos; they are recorded on threads that don't bind the target): projection uploads and
//     root draws are tagged in the command stream (OP_SET_PROJ_REGS, OP_LAYOUT_ROOT, gx2_core.cpp).
//   Anchoring, while a TV layout's root computes its matrices: root children further than 300 units
//     from the centre (hearts, magic, rupees, buttons, keys, place names...) are moved out by the
//     extra half width (height); L_EnemyHP_00 / L_CommandA_00 and roots the game moved off 0,0
//     (placed at an actor's projected position) are scaled by kx, ky instead; leaf panes covering the
//     whole 1280x720 screen (backgrounds, fades) are stretched. The translation/scale is changed only
//     for the call (dirty bit forced so the matrix is recomputed), so animations and game code keep
//     their values. Panes the game parks off the 16:9 screen are not drawn (they would show in the
//     extra space).
namespace interp { bool menu_open(); }
extern "C" {
void f_02874038_orig(Cpu* c);
void f_028766CC_orig(Cpu* c);
void f_02877100_orig(Cpu* c);
}
namespace aspect {
namespace {
constexpr uint32_t kPaneFlags = 0x44, kPaneMtxDirty = 0x10, kPaneGlobal = 0x48;
constexpr uint32_t kPaneParent = 0x0C, kPaneChildren = 0x14, kPaneTrans = 0x1C, kPaneScale = 0x34, kPaneSize = 0x3C, kPaneName = 0x80;

std::mutex g_root_mu;
// layout root pane -> the swaps it was last drawn at to the TV and to the GamePad. A root drawn to
// both (the controller select and file select screens, menus in the hybrid layout) shares one set
// of matrices for both pictures: it is laid out as a GamePad layout (unanchored; its TV draws are
// still centred by the projection) for as long as it keeps being drawn there. Deciding by the last
// screen it was drawn to made the choice race the render thread: the game thread computing the
// matrices between the TV draw and the GamePad draw stretched the screen-wide background over the
// wider picture for that frame, and the screen's edges flickered.
struct RootScreens {
    uint32_t root;
    uint64_t tv, drc;  // g_swaps + 1 of the last draw there (0: never)
};
std::vector<RootScreens> g_root_tv;
bool root_on_tv(uint32_t root) {
    std::lock_guard<std::mutex> lk(g_root_mu);
    for (auto& e : g_root_tv)
        if (e.root == root) return !e.drc || g_swaps + 1 - e.drc > 30;
    return true;  // not reported (yet): layouts are TV layouts unless seen on the GamePad
}
// the swap each layout root was last drawn at (any screen): layouts the game keeps but doesn't draw
// (a parked copy of the HUD) don't give the HUD editor their bounds
std::unordered_map<uint32_t, uint64_t> g_root_drawn;
bool root_drawn_recently(uint32_t root) {
    std::lock_guard<std::mutex> lk(g_root_mu);
    auto it = g_root_drawn.find(root);
    return it != g_root_drawn.end() && g_swaps - it->second < 30;
}
void mark_drawn(uint32_t root) {
    std::lock_guard<std::mutex> lk(g_root_mu);
    if (g_root_drawn.size() > 512) g_root_drawn.clear();
    g_root_drawn[root] = g_swaps;
}
void set_root_tv(uint32_t root, bool tv) {
    mark_drawn(root);
    std::lock_guard<std::mutex> lk(g_root_mu);
    for (auto& e : g_root_tv)
        if (e.root == root) {
            (tv ? e.tv : e.drc) = g_swaps + 1;
            return;
        }
    if (g_root_tv.size() > 256) g_root_tv.erase(g_root_tv.begin());
    g_root_tv.push_back({root, tv ? g_swaps + 1 : 0, tv ? 0 : g_swaps + 1});
}

// the anchoring state each root's matrices were last computed with: a root whose state changes
// (its screen became known, the aspect changed) recomputes its whole tree once
std::vector<std::pair<uint32_t, bool>> g_root_calc;
bool anchor_changed(uint32_t root, bool anchor) {
    std::lock_guard<std::mutex> lk(g_root_mu);
    for (auto& e : g_root_calc)
        if (e.first == root) {
            bool ch = e.second != anchor;
            e.second = anchor;
            return ch;
        }
    if (g_root_calc.size() > 256) g_root_calc.erase(g_root_calc.begin());
    g_root_calc.push_back({root, anchor});
    return true;  // first time seen: its cached matrices may come from another aspect (save state, earlier session)
}

// offsets applied to anchored panes in their last matrix calculation (root children and placed
// roots): the draw hook tells panes the game parked off the 16:9 screen from panes we moved there
std::unordered_map<uint32_t, std::pair<float, float>> g_offsets;
void set_offset(uint32_t pane, float dx, float dy) {
    std::lock_guard<std::mutex> lk(g_root_mu);
    if (dx == 0.0f && dy == 0.0f) g_offsets.erase(pane);
    else g_offsets[pane] = {dx, dy};
}
std::pair<float, float> offset_of(uint32_t pane) {
    std::lock_guard<std::mutex> lk(g_root_mu);
    auto it = g_offsets.find(pane);
    return it == g_offsets.end() ? std::pair<float, float>{0, 0} : it->second;
}

bool name_is(uint32_t pane, const char* n) { return !strncmp((const char*)mem::ptr(pane + kPaneName), n, 24); }

// HUD parts that belong at the screen edges (only these are moved out; anything else stays where the
// game put it). Not N_All_00: a generic container name several layouts use (the button cluster, but
// also the message window's choice box, whose cursor the game places from the drawn row positions:
// moving it misplaced the Yes/No cursor at 16:10). The game reads the drawn
// positions of message rows to place the choice cursor, so moving them misplaces the cursor)
bool is_hud_edge_pane(uint32_t pane) {
    static const char* const kEdge[] = {
        "N_TV_00", "N_DRC_00", "N_Default_00", "N_HeartPos_01", "L_Rupy_00", "L_CompassClock_00",
        "L_DungeonKey_00", "N_SwimTimePos_00", "L_BossHP_00", "L_Arrow_00", "L_BatteryVol1_00",
        "L_RupySwordCounter_00", "N_Position_00", "N_Time_00"};
    for (const char* n : kEdge)
        if (name_is(pane, n)) return true;
    return false;
}

// The player's HUD positions (Controls > HUD positions): an offset in layout units (1280x720 picture)
// per HUD part, added to the part's translation while its matrices are computed, like the anchoring
// above (the game's own values stay untouched). TV layouts only.
std::atomic<int32_t> g_hud_off[aspect::kHudParts][2];
std::atomic<int32_t> g_hud_scale[aspect::kHudParts] = {100, 100, 100, 100, 100};  // percent
std::atomic<bool> g_hud_hidden[aspect::kHudParts];
// where each part was last drawn (layout units, y up): the union of its visible panes
std::mutex g_bounds_mu;
float g_bounds[aspect::kHudParts][4];
bool g_bounds_ok[aspect::kHudParts];
float g_bounds_acc[aspect::kHudParts][4];
bool g_bounds_any[aspect::kHudParts];
thread_local int t_part = -1;  // the part whose panes are being computed
int hud_part(uint32_t pane) {
    // root children by name; WWHD_HUD_LOG=1 lists them
    static const char* const kNames[aspect::kHudParts][3] = {
        {"N_TV_00", "N_Default_00", nullptr},  // hearts and magic (top left; N_DRC_00 is the GamePad's)
        {"L_Rupy_00", nullptr, nullptr},  // rupees
        {nullptr, nullptr, nullptr},  // the button cluster: an N_All_00 holding N_X_00 (below)
        {"L_DungeonKey_00", nullptr, nullptr},  // small keys
        {"L_CompassClock_00", nullptr, nullptr},  // the wind compass and clock
    };
    for (int i = 0; i < aspect::kHudParts; i++)
        for (const char* n : kNames[i])
            if (n && name_is(pane, n)) return i;
    // N_All_00 is a container many layouts use (the message box's choices too): the item buttons'
    // is the one with the X button among its children
    if (name_is(pane, "N_All_00")) {
        uint32_t sentinel = pane + kPaneChildren;
        uint32_t n = ld32(sentinel);
        for (int k = 0; k < 64 && n && n != sentinel; k++, n = ld32(n))
            if (name_is(n, "N_X_00")) return aspect::kHudButtons;
    }
    return -1;
}

thread_local uint32_t t_root = 0;   // layout root whose matrices are being computed
thread_local bool t_anchor = false; // ... and it is a TV layout at another aspect ratio
thread_local bool t_tv = false;     // ... and it is a TV layout
}  // namespace
}  // namespace aspect

// the game's "Skip" prompt (T_Skip_00): its layout runs, hidden until + is pressed once, for as long
// as a skippable scene plays (the prologue, story cutscenes) and not otherwise
static std::atomic<uint64_t> g_skip_swap{0};
bool aspect::skippable_scene() {
    uint64_t s = g_skip_swap.load(std::memory_order_relaxed);
    return s && g_swaps - s <= 8;
}

static void calc_mtx(Cpu* c);

// a pane of the part being computed, after its matrices: grows the part's bounds
static void add_bounds(uint32_t pane, int part) {
    using namespace aspect;
    // drawn: visible itself and up to the layout root, and not fully transparent (global alpha)
    if (ld8(pane + 0x46) == 0) return;
    for (uint32_t p = pane; p; p = ld32(p + kPaneParent))
        if (!(ld8(p + kPaneFlags) & 1) || ld8(p + 0x45) == 0) return;
    float w = (float)ldf32(pane + kPaneSize), h = (float)ldf32(pane + kPaneSize + 4);
    float gx = (float)ldf32(pane + kPaneGlobal + 0xC), gy = (float)ldf32(pane + kPaneGlobal + 0x1C);
    float ax = std::fabs((float)ldf32(pane + kPaneGlobal)), ay = std::fabs((float)ldf32(pane + kPaneGlobal + 0x14));
    if (!(w > 0.0f && h > 0.0f && w < 1200.0f && h < 700.0f) || !std::isfinite(gx) || !std::isfinite(gy) || !(ax > 0.0f && ax < 20.0f)) return;
    float hw = 0.5f * w * ax, hh = 0.5f * h * ay;
    // panes the game parks off the screen (unused prompts, a second copy of the layout) don't count
    float kx, ky;
    factors(g_game, kx, ky);
    if (std::fabs(gx) - hw > 640.0f * kx || std::fabs(gy) - hh > 360.0f * ky) return;
    static const bool log_b = getenv("WWHD_HUD_LOG") != nullptr;
    if (log_b) {
        static std::mutex mu;
        static std::unordered_map<uint32_t, int> seen;
        std::lock_guard<std::mutex> lk(mu);
        if (seen[pane]++ == 0)
            LOG("[hudb] part %d '%.24s' g %.0f,%.0f half %.0f,%.0f size %.0fx%.0f vt %08X", part, (const char*)mem::ptr(pane + kPaneName), gx, gy, hw, hh, w, h, ld32(pane + 8));
    }
    float* b = g_bounds_acc[part];
    if (!g_bounds_any[part]) { b[0] = gx - hw; b[1] = gy - hh; b[2] = gx + hw; b[3] = gy + hh; g_bounds_any[part] = true; return; }
    b[0] = std::min(b[0], gx - hw); b[1] = std::min(b[1], gy - hh);
    b[2] = std::max(b[2], gx + hw); b[3] = std::max(b[3], gy + hh);
}

// Pane::CalculateMtx(this, DrawInfo&, bool parentDirty)
extern "C" void hook_028766CC(Cpu* c) {
    using namespace aspect;
    uint32_t pane = c->r[3], parent = ld32(pane + kPaneParent);
    int part = parent && parent == t_root && t_tv && t_part < 0 ? hud_part(pane) : -1;
    if (part >= 0) {
        g_bounds_any[part] = false;
        t_part = part;
        calc_mtx(c);
        t_part = -1;
        add_bounds(pane, part);
        if (g_bounds_any[part] && !g_hud_hidden[part].load(std::memory_order_relaxed) && root_drawn_recently(t_root)) {
            std::lock_guard<std::mutex> lk(g_bounds_mu);
            memcpy(g_bounds[part], g_bounds_acc[part], sizeof g_bounds[part]);
            g_bounds_ok[part] = true;
        }
        return;
    }
    calc_mtx(c);
    if (t_part >= 0 && parent) add_bounds(pane, t_part);
}

static void calc_mtx(Cpu* c) {
    using namespace aspect;
    uint32_t pane = c->r[3];
    if (ld32(pane + kPaneName) == 0x545F536B /* "T_Sk" */ && name_is(pane, "T_Skip_00")) g_skip_swap.store(g_swaps ? g_swaps : 1, std::memory_order_relaxed);
    uint32_t parent = ld32(pane + kPaneParent);
    float kx, ky;
    factors(g_game, kx, ky);
    if (!parent) {  // a layout's root: decide for the whole tree
        uint32_t saveRoot = t_root;
        bool saveAnchor = t_anchor;
        t_root = pane;
        t_tv = root_on_tv(pane);
        t_anchor = (kx != 1.0f || ky != 1.0f) && t_tv;
        // a root placed by the game (layouts are authored with the root at 0,0): small layouts put at
        // an actor's projected screen position (cursors, markers) -> scaled out with the 3D view
        float tx = (float)ldf32(pane + kPaneTrans), ty = (float)ldf32(pane + kPaneTrans + 4);
        // not in the game's menus (pause / item menu, title, file select): their cursors are roots
        // placed on menu panes, which stay in 16:9 layout space
        bool placed = t_anchor && (tx != 0.0f || ty != 0.0f) && !interp::menu_open();
        // debug: WWHD_ASPECT_LOG=1 logs each placed root (name, translation) once per name
        static const bool log_roots = getenv("WWHD_ASPECT_LOG") != nullptr;
        if (log_roots && placed) {
            static std::mutex mu;
            static std::unordered_map<uint32_t, std::pair<float, float>> seen;
            std::string nm((const char*)mem::ptr(pane + kPaneName), strnlen((const char*)mem::ptr(pane + kPaneName), 24));
            std::lock_guard<std::mutex> lk(mu);
            auto& v = seen[pane];
            if (v.first != tx || v.second != ty) {
                v = {tx, ty};
                uint32_t first = ld32(pane + kPaneChildren);
                std::string child = first && first != pane + kPaneChildren
                    ? std::string((const char*)mem::ptr(first + kPaneName), strnlen((const char*)mem::ptr(first + kPaneName), 24)) : "";
                LOG("[aspect] placed root '%s' %08X at %.1f, %.1f size %.0fx%.0f first child '%s'", nm.c_str(), pane, tx, ty,
                    (float)ldf32(pane + kPaneSize), (float)ldf32(pane + kPaneSize + 4), child.c_str());
            }
        }
        // matrices are only recomputed for dirty panes: after an aspect change, the whole tree
        if (anchor_changed(pane, t_anchor) || (g_changed_swap && g_swaps - g_changed_swap < 4)) c->r[5] = 1;
        if (placed) { stf32(pane + kPaneTrans, tx * kx); stf32(pane + kPaneTrans + 4, ty * ky); }
        set_offset(pane, placed ? tx * (kx - 1.0f) : 0.0f, placed ? ty * (ky - 1.0f) : 0.0f);
        f_028766CC_orig(c);
        if (placed) { stf32(pane + kPaneTrans, tx); stf32(pane + kPaneTrans + 4, ty); }
        t_root = saveRoot;
        t_anchor = saveAnchor;
        return;
    }
    // debug: WWHD_HUD_LOG=1 lists every TV layout's root children (name, translation) once
    static const bool log_hud = getenv("WWHD_HUD_LOG") != nullptr;
    if (log_hud && t_tv && parent != t_root && ld32(parent + kPaneParent) == t_root) {  // and one level down
        static std::mutex mu2;
        static std::unordered_map<std::string, int> seen2;
        auto nm = [](uint32_t p) { return std::string((const char*)mem::ptr(p + kPaneName), strnlen((const char*)mem::ptr(p + kPaneName), 24)); };
        std::string key = nm(parent) + "/" + nm(pane);
        std::lock_guard<std::mutex> lk(mu2);
        if (seen2[key]++ == 0)
            LOG("[hud]   '%s' at %.1f, %.1f", key.c_str(), (float)ldf32(pane + kPaneTrans), (float)ldf32(pane + kPaneTrans + 4));
    }
    if (log_hud && parent == t_root && t_tv) {
        static std::mutex mu;
        static std::unordered_map<std::string, int> seen;
        auto nm = [](uint32_t p) { return std::string((const char*)mem::ptr(p + kPaneName), strnlen((const char*)mem::ptr(p + kPaneName), 24)); };
        std::string key = nm(t_root) + "/" + nm(pane);
        std::lock_guard<std::mutex> lk(mu);
        if (seen[key]++ == 0)
            LOG("[hud] '%s' at %.1f, %.1f", key.c_str(), (float)ldf32(pane + kPaneTrans), (float)ldf32(pane + kPaneTrans + 4));
    }
    // the player's HUD position for this part
    int part = parent == t_root && t_tv ? hud_part(pane) : -1;
    float ux = part >= 0 ? (float)g_hud_off[part][0].load(std::memory_order_relaxed) : 0.0f;
    float uy = part >= 0 ? (float)g_hud_off[part][1].load(std::memory_order_relaxed) : 0.0f;
    // the part's size (percent) and whether it is shown
    float us = part < 0 ? 1.0f : g_hud_hidden[part].load(std::memory_order_relaxed) ? 0.0f
                                : (float)g_hud_scale[part].load(std::memory_order_relaxed) / 100.0f;
    if (log_hud && part >= 0) {
        static uint64_t last[aspect::kHudParts];
        if (g_swaps - last[part] > 300) {
            last[part] = g_swaps;
            LOG("[hud] part %d pane %08X at %.1f, %.1f offset %.0f, %.0f anchor %d", part, pane, (float)ldf32(pane + kPaneTrans),
                (float)ldf32(pane + kPaneTrans + 4), ux, uy, (int)t_anchor);
        }
    }
    if (!t_anchor) {
        if (parent == t_root) set_offset(pane, ux, -uy);
        if (ux == 0.0f && uy == 0.0f && us == 1.0f) {
            f_028766CC_orig(c);
            return;
        }
        float tx = (float)ldf32(pane + kPaneTrans), ty = (float)ldf32(pane + kPaneTrans + 4);
        float sx = (float)ldf32(pane + kPaneScale), sy = (float)ldf32(pane + kPaneScale + 4);
        st8(pane + kPaneFlags, ld8(pane + kPaneFlags) | kPaneMtxDirty);
        stf32(pane + kPaneTrans, tx + ux);
        stf32(pane + kPaneTrans + 4, ty - uy);  // layout y points up; the setting's y points down
        stf32(pane + kPaneScale, sx * us);
        stf32(pane + kPaneScale + 4, sy * us);
        f_028766CC_orig(c);
        stf32(pane + kPaneTrans, tx);
        stf32(pane + kPaneTrans + 4, ty);
        stf32(pane + kPaneScale, sx);
        stf32(pane + kPaneScale + 4, sy);
        return;
    }
    float tx = (float)ldf32(pane + kPaneTrans), ty = (float)ldf32(pane + kPaneTrans + 4);
    float sx = (float)ldf32(pane + kPaneScale), sy = (float)ldf32(pane + kPaneScale + 4);
    float nx = tx, ny = ty, nsx = sx, nsy = sy;
    if (parent == t_root) {
        float half = 640.0f * (kx - 1.0f), halfY = 360.0f * (ky - 1.0f);
        // parts the game moves to an actor's projected position: enemy health, the floating A action
        if (name_is(pane, "L_EnemyHP_00") || name_is(pane, "L_CommandA_00")) {
            nx = tx * kx;
            ny = ty * ky;
        } else if (is_hud_edge_pane(pane)) {
            if (tx <= -300.0f) nx = tx - half;
            if (tx >= 300.0f) nx = tx + half;
            if (ty <= -200.0f) ny = ty - halfY;
            if (ty >= 200.0f) ny = ty + halfY;
        }
    }
    // a leaf covering the whole screen (background, fade): made to cover the new shape. One of the
    // screen's shape is stretched to it; another shape (the telescope's and the first-person items'
    // round view, W_Wipe_00: 1716x1716) keeps its proportions, scaled evenly just enough to cover
    // (it usually covers the wider screen already): stretched, the round view became an ellipse
    uint32_t sentinel = pane + kPaneChildren;
    if (ld32(sentinel) == sentinel && std::fabs(tx) < 8.0f && std::fabs(ty) < 8.0f) {
        float w = (float)ldf32(pane + kPaneSize) * std::fabs(sx), h = (float)ldf32(pane + kPaneSize + 4) * std::fabs(sy);
        const bool screenShaped = h > 0.0f && std::fabs(w / h / (16.0f / 9.0f) - 1.0f) < 0.05f;
        const float needX = 1280.0f * kx / std::max(w, 1.0f), needY = 720.0f * ky / std::max(h, 1.0f);
        if (w >= 1270.0f && h >= 710.0f && !screenShaped) {
            const float k = std::max(1.0f, std::max(needX, needY));
            nsx = sx * k;
            nsy = sy * k;
        } else if (w >= 1270.0f && h >= 710.0f) {
            nsx = sx * kx;
            nsy = sy * ky;
            static const bool log_st = getenv("WWHD_ASPECT_LOG") != nullptr;
            if (log_st) {
                static std::mutex mu;
                static std::unordered_map<std::string, int> seen;
                std::string key = std::string((const char*)mem::ptr(t_root + kPaneName), strnlen((const char*)mem::ptr(t_root + kPaneName), 24)) +
                                  "/" + std::string((const char*)mem::ptr(pane + kPaneName), strnlen((const char*)mem::ptr(pane + kPaneName), 24));
                std::lock_guard<std::mutex> lk(mu);
                if (seen[key]++ == 0) LOG("[aspect] stretched '%s' (vtable %08X) %.0fx%.0f", key.c_str(), ld32(pane), w, h);
            }
        }
    }
    nx += ux;  // the player's HUD position and size on top
    ny -= uy;
    nsx *= us;
    nsy *= us;
    bool moved = nx != tx || ny != ty, scaled = nsx != sx || nsy != sy;
    static const bool log_moves = getenv("WWHD_ASPECT_LOG") != nullptr;
    if (log_moves && moved && parent == t_root) {
        static std::mutex mu;
        static std::unordered_map<std::string, int> seen;
        auto nm = [](uint32_t p) { return std::string((const char*)mem::ptr(p + kPaneName), strnlen((const char*)mem::ptr(p + kPaneName), 24)); };
        std::string key = nm(t_root) + "/" + nm(pane);
        std::lock_guard<std::mutex> lk(mu);
        if (seen[key]++ == 0) LOG("[aspect] moved '%s' %.1f,%.1f -> %.1f,%.1f", key.c_str(), tx, ty, nx, ny);
    }
    if (parent == t_root) set_offset(pane, nx - tx, ny - ty);
    if (moved || scaled) st8(pane + kPaneFlags, ld8(pane + kPaneFlags) | kPaneMtxDirty);  // recompute with the offsets
    if (moved) { stf32(pane + kPaneTrans, nx); stf32(pane + kPaneTrans + 4, ny); }
    if (scaled) { stf32(pane + kPaneScale, nsx); stf32(pane + kPaneScale + 4, nsy); }
    f_028766CC_orig(c);
    if (moved) { stf32(pane + kPaneTrans, tx); stf32(pane + kPaneTrans + 4, ty); }
    if (scaled) { stf32(pane + kPaneScale, sx); stf32(pane + kPaneScale + 4, sy); }
}

// Pane::Draw(this, DrawInfo&): the render thread reports where each layout root is drawn. The
// game thread can't tell: TV and GamePad layouts share DrawInfos and are recorded on threads that
// don't bind the targets.
extern "C" void hook_02877100(Cpu* c) {
    using namespace aspect;
    uint32_t pane = c->r[3];
    uint32_t parent = ld32(pane + kPaneParent);
    if (!parent) gx2::emit(gx2::OP_LAYOUT_ROOT, {pane});  // also at 16:9: the HUD editor wants the drawn roots
    if (original()) { f_02877100_orig(c); return; }
    if (parent) {
        // the wider (taller) picture shows layout space the game uses to park panes out of sight:
        // a pane whose centre, without our offsets, is off the 16:9 screen stays hidden
        uint32_t child = pane, root = parent;
        for (int i = 0; i < 16 && ld32(root + kPaneParent); i++) { child = root; root = ld32(root + kPaneParent); }
        // the item menu's page arrows: the game hides the one with no page beyond it (the left arrow on
        // the first page, the right one on the last) by placing it just off the 16:9 screen, which the
        // wider picture shows. They are placed in screen space (not slid with the pages): drawn only
        // when they reach into the 16:9 screen
        if (pane == child && ld32(pane + kPaneName) == 0x4C5F4D65 /* "L_Me" */ && !strncmp((const char*)mem::ptr(pane + kPaneName), "L_MenuArrow", 11) &&
            !ld32(root + kPaneParent) && root_on_tv(root)) {
            float gx = (float)ldf32(pane + kPaneGlobal + 0xC);
            float hw = 0.5f * (float)ldf32(pane + kPaneSize) * std::fabs((float)ldf32(pane + kPaneGlobal));
            if (!(hw >= 0.0f && hw < 4096.0f)) hw = 0.0f;
            if (std::fabs(gx) - hw >= 640.0f) return;
        }
        // not in the game's menus: the item menu slides its pages with the layout's view, not the
        // panes, so the page being shown has panes "beyond" the screen (they all vanished)
        if (!ld32(root + kPaneParent) && root_on_tv(root) && !interp::menu_open()) {
            auto a = offset_of(child), b = offset_of(root);
            float gx = (float)ldf32(pane + kPaneGlobal + 0xC) - a.first - b.first;
            float gy = (float)ldf32(pane + kPaneGlobal + 0x1C) - a.second - b.second;
            // the pane's half extent (size times its global scale): hidden only when it lies wholly
            // outside the 16:9 screen (the item menu's tabs and page arrows reach into it from beyond)
            float hw = 0.5f * (float)ldf32(pane + kPaneSize) * std::fabs((float)ldf32(pane + kPaneGlobal));
            float hh = 0.5f * (float)ldf32(pane + kPaneSize + 4) * std::fabs((float)ldf32(pane + kPaneGlobal + 0x14));
            if (!(hw >= 0.0f && hw < 4096.0f)) hw = 0.0f;
            if (!(hh >= 0.0f && hh < 4096.0f)) hh = 0.0f;
            if (std::fabs(gx) - hw > 640.0f + 32.0f || std::fabs(gy) - hh > 360.0f + 32.0f) {
                static const bool log_hide = getenv("WWHD_ASPECT_LOG") != nullptr;
                if (log_hide) {
                    static std::mutex mu;
                    static std::unordered_map<uint32_t, int> seen;
                    std::lock_guard<std::mutex> lk(mu);
                    if (seen[pane]++ % 300 == 0)
                        LOG("[aspect] hidden '%.24s' %08X (child '%.24s', root %08X): %.1f, %.1f", (const char*)mem::ptr(pane + kPaneName), pane,
                            (const char*)mem::ptr(child + kPaneName), root, gx, gy);
                }
                static const bool log_hud = getenv("WWHD_HUD_LOG") != nullptr;
                if (log_hud && (a.first != 0.0f || a.second != 0.0f)) {
                    static uint64_t n = 0;
                    if (n++ % 600 == 0)
                        LOG("[hud] hidden pane %08X under %08X: global %.1f, %.1f, offsets %.1f, %.1f", pane, child,
                            (float)ldf32(pane + kPaneGlobal + 0xC), (float)ldf32(pane + kPaneGlobal + 0x1C), a.first, a.second);
                }
                return;
            }
        }
    }
    f_02877100_orig(c);
}

// the projection matrix uploaded during `fn` is tagged: the render thread narrows it (rows x and y
// divided by kx, ky) when the target is the TV picture (gx2_core.cpp OP_SET_PROJ_REGS)
static thread_local int t_tag = 0;
static void with_tv_projection(Cpu* c, void (*fn)(Cpu*)) {
    if (aspect::original()) { fn(c); return; }
    t_tag++;
    fn(c);
    t_tag--;
}
namespace aspect {
bool tagged_projection() { return t_tag > 0; }
// at 16:9 the target can't be told apart (no factors): only that the root was drawn
void layout_root_target(uint32_t root, bool tv) {
    if (original()) mark_drawn(root);
    else set_root_tv(root, tv);
}
}  // namespace aspect

// DrawInfo::LoadProjectionMtx(this): TV layouts are drawn into the wider screen undistorted
extern "C" void hook_02874038(Cpu* c) { with_tv_projection(c, f_02874038_orig); }
// nw::font text drawing (TextBox panes): content+4 -> projection matrix (multiplied with the view
// matrix at content+8 and uploaded)
extern "C" void f_028F8250_orig(Cpu* c);
extern "C" void hook_028F8250(Cpu* c) { with_tv_projection(c, f_028F8250_orig); }


// a loaded save state brings layout matrices cached under whatever aspect was active when it was
// saved: forget which roots were computed, so each recomputes its whole tree once
void aspect::set_hud_offset(int part, int dx, int dy) {
    if (part < 0 || part >= kHudParts) return;
    g_hud_off[part][0] = dx;
    g_hud_off[part][1] = dy;
    g_changed_swap = g_swaps ? g_swaps : 1;  // every layout recomputes its matrices for a few frames
}
void aspect::set_hud_scale(int part, int pct, bool hidden) {
    if (part < 0 || part >= kHudParts) return;
    g_hud_scale[part] = std::clamp(pct, 25, 400);
    g_hud_hidden[part] = hidden;
    g_changed_swap = g_swaps ? g_swaps : 1;
}
void aspect::refresh_hud() { g_changed_swap = g_swaps ? g_swaps : 1; }
void aspect::hud_bounds(float out[kHudParts * 4 + 2]) {
    {
        std::lock_guard<std::mutex> lk(g_bounds_mu);
        for (int p = 0; p < kHudParts; p++)
            for (int i = 0; i < 4; i++) out[p * 4 + i] = g_bounds_ok[p] ? g_bounds[p][i] : NAN;
    }
    float kx, ky;
    factors(g_game, kx, ky);
    out[kHudParts * 4] = kx;
    out[kHudParts * 4 + 1] = ky;
}
int aspect::hud_offset(int part, int axis) { return part >= 0 && part < kHudParts ? g_hud_off[part][axis & 1].load() : 0; }

void aspect::ss_reset() {
    std::lock_guard<std::mutex> lk(aspect::g_root_mu);
    aspect::g_root_calc.clear();
}
