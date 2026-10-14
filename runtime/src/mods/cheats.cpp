// Cheats (Gameplay menu): edits of the live save data, applied by the game's main thread at the top
// of the frame (hook_0203593C in interp.cpp), like the save states. One-shot cheats run once per
// click; the infinite health / magic / ammo switches top up every frame.
//
// The save data is dSv_info_c of the GameCube decompilation (zeldaret/tww include/d/d_save.h),
// unchanged in WWHD as far as used here. Each cheat does what the game's own item_func_*
// (d_item.cpp) does for that item. It lives on the heap at the same address in every session; the
// copy the file select reads/writes is elsewhere (0x144FDA00), so cheats last once the game is saved.
//
// Note: the game itself takes the sword away in Forsaken Fortress (first visit) and the bow in the
// Tower of the Gods when those stages load (d_s_play.cpp), cheat or not.
//
// Moon jump (a switch like the infinite ones, our own: the GameCube Gecko/Action Replay codes don't
// apply to WWHD): while L is held Link rises and the left stick steers him; on release gravity takes
// over again (moon_jump_step). WWHD_MOON_JUMP_SPEED / _LIFT / _FLY override rise, lift-off and flight
// speed.
//
// Test switch: WWHD_CHEAT=items,sword,stats,songs,triforce,dungeon,key applies those once a save
// file is loaded; WWHD_CHEAT_INFINITE=health,magic,ammo,moonjump switches those on.
// WWHD_CHEAT_SAVE_ADDR=hex overrides the address (another game version).
// Test switch: WWHD_TEST_WARP=stage:room:point,stage:room:point,... once a file is loaded, goes to
// each place in turn, one every WWHD_TEST_WARP_SECS seconds (default 25), as
// dComIfGp_setNextStage does (layer -1: the one for the story progress). For trying the game's
// places without playing up to them (performance, rendering).
// WWHD_TEST_LOAD_STATE=slot loads that save state 8 s after a save file is loaded, and
// WWHD_TEST_SAVE_STATE=slot saves one once the warps are done (the last place).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <cstdio>
#include <string>
#include <vector>

#include "mods.h"
#include "interp.h"
#include "true60.h"
#include "release.h"
#include "runtime.h"
#include "savestate.h"

namespace mods {
namespace {
// dSv_player_c offsets
constexpr uint32_t kMaxLife = 0x00, kLife = 0x02, kRupee = 0x04;  // u16, life in quarter hearts
constexpr uint32_t kSelectEquip = 0x0E;  // u8[4]: sword, shield, bracelet
constexpr uint32_t kWallet = 0x12, kMaxMagic = 0x13, kMagic = 0x14;
constexpr uint32_t kReturnName = 0x30;  // char[8] stage
constexpr uint32_t kItems = 0x3C;       // u8[21] item number per inventory slot (0xFF empty)
constexpr uint32_t kGetItems = 0x51;    // u8[21] "got" bits per slot (bow: 1 fire/ice, 2 light)
constexpr uint32_t kArrowNum = 0x69, kBombNum = 0x6A, kArrowMax = 0x6F, kBombMax = 0x70;
constexpr uint32_t kCollect = 0xB4;     // u8[8]: [0] swords, [1] shields, [2] bracelets
constexpr uint32_t kTact = 0xBD, kTriforce = 0xBE;  // song bits (6), Triforce shard bits (8)
// dSv_info_c::mMemory: the current stage's dSv_memBit_c (copied back to its save table on leaving)
constexpr uint32_t kStageKeys = 0x778 + 0x20, kStageDungeonItems = 0x778 + 0x21;  // bits: map, compass, boss key

// the heap block g_dComIfG_gameInfo lives in (pointer in static data; the save data at +0xC92C,
// 0x145AC92C in USA sessions); USA addresses, release::Data maps them for EUR
const release::Data kInfoPointer{0x10114B90}, kStageInfo{0x1046F0B0};
}  // namespace

// public (mods.h): the autosave uses these to know when a file is actually being played
uint32_t save_addr() {
    static const uint32_t over = getenv("WWHD_CHEAT_SAVE_ADDR") ? (uint32_t)strtoul(getenv("WWHD_CHEAT_SAVE_ADDR"), nullptr, 16) : 0;
    return over ? over : ld32(kInfoPointer) + 0xC92C;
}

std::string stage() {
    const char* s = (const char*)mem::ptr(kStageInfo + 0x5134);  // current stage, as in savestate.cpp
    return std::string(s, strnlen(s, 8));
}

// false on the title screen (it has placeholder save data that a file load replaces), before a file
// is loaded, or if the address is wrong
bool save_loaded(uint32_t s) {
    std::string st = stage();
    if (st.empty() || st == "sea_T" || st == "Name") return false;  // title, file select
    uint16_t max = ld16(s + kMaxLife), life = ld16(s + kLife);
    // no "% 4": the game counts quarter hearts, so a collected heart piece leaves the maximum
    // at 12 + odd
    if (max < 12 || max > 80 || life > max || ld16(s + kRupee) > 5000 || ld8(s + kWallet) > 2) return false;
    // the return stage: a NUL-terminated name in 8 bytes, at least 3 characters ("sea", "sea_T", "M_NewD2")
    int n = 0;
    for (; n < 8; n++) {
        uint8_t ch = ld8(s + kReturnName + n);
        if (!ch) break;
        if (ch < '0' || ch > 'z') return false;
    }
    return n >= 3;
}
namespace {

void give(uint32_t s, int slot, uint8_t item, uint8_t got_bits = 1) {
    st8(s + kItems + slot, item);
    st8(s + kGetItems + slot, ld8(s + kGetItems + slot) | got_bits);
}

void all_items(uint32_t s) {
    give(s, 0, 0x20);         // telescope
    give(s, 1, 0x78);         // sail
    give(s, 2, 0x22);         // Wind Waker (songs are not given: they drive story events)
    give(s, 3, 0x25);         // grappling hook
    give(s, 4, 0x24);         // spoils bag
    give(s, 5, 0x2D);         // boomerang
    give(s, 6, 0x34);         // Deku leaf
    give(s, 8, 0x26, 3);      // deluxe picto box
    give(s, 9, 0x29);         // iron boots
    give(s, 10, 0x2A);        // magic armor
    give(s, 11, 0x2C);        // bait bag
    give(s, 12, 0x36, 7);     // bow with fire/ice and light arrows
    give(s, 13, 0x31);        // bombs
    for (int b = 14; b < 18; b++)
        if (ld8(s + kItems + b) == 0xFF) st8(s + kItems + b, 0x50);  // empty bottles, contents kept
    give(s, 18, 0x30);        // delivery bag
    give(s, 19, 0x2F);        // hookshot
    give(s, 20, 0x33);        // skull hammer
    st8(s + kCollect + 2, ld8(s + kCollect + 2) | 1);  // power bracelets
    st8(s + kSelectEquip + 2, 0x28);
    st8(s + kArrowMax, 99);
    st8(s + kArrowNum, 99);
    st8(s + kBombMax, 99);
    st8(s + kBombNum, 99);
    if (ld8(s + kMaxMagic) < 16) st8(s + kMaxMagic, 16);  // the Deku leaf needs magic
    st8(s + kMagic, ld8(s + kMaxMagic));
}

void best_sword(uint32_t s) {
    // Sword ownership is story progress: bit 2 removes Medli from Dragon Roost and the Earth
    // Temple; bit 3 removes Makar from his earlier locations. Equip the upgrades without claiming
    // those story milestones; the game's equipment refresh restores the earned equipment on reload
    // (original project 7280fcd).
    st8(s + kSelectEquip + 0, 0x3E);
    st8(s + kSelectEquip + 1, 0x3C);
}

void max_stats(uint32_t s) {
    st16(s + kMaxLife, 80);  // 20 hearts
    st16(s + kLife, 80);
    st8(s + kWallet, 2);     // 5000
    st16(s + kRupee, 5000);
    st8(s + kMaxMagic, 32);  // double magic
    st8(s + kMagic, 32);
}

int parse_env(const char* var, std::initializer_list<std::pair<const char*, int>> names) {
    const char* e = getenv(var);
    int w = 0;
    for (auto& [n, bit] : names)
        if (e && strstr(e, n)) w |= bit;
    return w;
}

std::atomic<int> g_pending{parse_env("WWHD_CHEAT", {{"items", kCheatItems}, {"sword", kCheatSword}, {"stats", kCheatStats},
                                                    {"songs", kCheatSongs}, {"triforce", kCheatTriforce},
                                                    {"dungeon", kCheatDungeon}, {"key", kCheatKey}})};
std::atomic<int> g_infinite{parse_env("WWHD_CHEAT_INFINITE",
                                      {{"health", kInfHealth}, {"magic", kInfMagic}, {"ammo", kInfAmmo}, {"moonjump", kMoonJump}, {"nofall", kMoonNoFall},
                                       {"allanims", kMoonAllAnims}})};
}  // namespace

void request_cheat(int which) { g_pending |= which; }
bool infinite(int which) { return g_infinite.load(std::memory_order_relaxed) & which; }
void set_infinite(int which, bool on) {
    if (on) g_infinite |= which;
    else g_infinite &= ~which;
    LOG("[cheats] %s %s",
        which == kInfHealth  ? "infinite health"
        : which == kInfMagic ? "infinite magic"
        : which == kInfAmmo  ? "infinite ammo"
        : which == kMoonJump ? "moon jump"
        : which == kMoonNoFall ? "no fall damage after flying"
                             : "random lift-off animations",
        on ? "on" : "off");
}

// Moon jump. fopAc_ac_c fields (WWHD: base fields +0x11C; docs/decomp-notes.md): current.angle at
// +0x320, shape_angle +0x328, speed +0x33C; daPy_lk_c::mModeFlg +0x6A70 (bit 2: in the air), mCurProc
// +0x65F0. Speeds per 30 Hz step, like the game's. While L (the shoulder button) is held:
//  - he's put into the game's fall, procFall_init (proc 0x27), also from swimming: on the ground his procedures ignore
//    speed.y and the ground snapping undoes a lift; the jump off a ledge (proc 0x24) moves him along
//    its own take-off line. His collision's ground flags are cleared too, or the fall lands at once;
//  - speed.y is set upward every step.
// Until he lands, also after L is let go, the left stick steers as when he runs: he turns towards the
// stick's direction relative to the camera at a limited rate, and the horizontal part of his
// `current.pos += speed` (true60_link.cpp, moon_jump_move) is the way he faces; stick released: no
// horizontal move. His angles are written before and after his step. The
// fall itself sets his speed and direction every step and pins his position, so neither speed fields
// nor a position written from outside steer him. The collision after the move still applies (walls).
// No fall damage after flying (a switch): his fall start height (+0x6A48: posMove stores the height at
// the top of a jump, 023FD38C; landing compares it with his height for the hard landing, 0241C8B8) is
// kept at his height before and after every step of the flight, until he lands.
constexpr uint32_t kAngleY = 0x320 + 2, kShapeAngleY = 0x328 + 2;
constexpr uint32_t kCurProc = 0x65F0, kProcFall = 0x27;  // daPy_lk_c::mCurProc, daPy_PROC_FALL
constexpr uint32_t kSpeedY = 0x33C + 4, kPosY = 0x314 + 4, kFallStartY = 0x6A48;
uint32_t g_move_link = 0;        // the step being steered (moon_jump_move), and its move
float g_move_x = 0, g_move_z = 0;
bool g_flight = false;           // a moon jump began and Link hasn't landed yet: the stick steers
int16_t g_face = 0;              // the way he faces and flies (the fall resets his angles every step)
bool g_face_set = false;
float env_float(const char* name, float def) {
    const char* e = getenv(name);
    return e ? (float)atof(e) : def;
}
std::atomic<int> g_rise_pct{100}, g_fly_pct{100};  // the menu's speeds (set_moon_jump_speeds)
float rise_speed() {
    static const float base = env_float("WWHD_MOON_JUMP_SPEED", 25.0f);
    return base * g_rise_pct.load(std::memory_order_relaxed) / 100.0f;
}
float fly_speed() {
    static const float base = env_float("WWHD_MOON_JUMP_FLY", 40.0f);
    return base * g_fly_pct.load(std::memory_order_relaxed) / 100.0f;
}
void face(uint32_t link) {
    st16(link + kAngleY, (uint16_t)g_face);
    st16(link + kShapeAngleY, (uint16_t)g_face);
}
constexpr uint32_t kModeFlg = 0x6A70, kModeMidair = 0x2, kModeSwim = 0x40000;
// daPy_lk_c::mAcch (dBgS_Acch) at +0x80C: its m_flags (+0x28) with GROUND_HIT 0x20, GROUND_FIND 0x40,
// GROUND_LANDING 0x80 (autoGroundHit, 023FE2F0, tests 0x20 there). Left set, the fall lands at once.
constexpr uint32_t kAcchFlags = 0x80C + 0x28, kAcchGround = 0x20 | 0x40 | 0x80;
const release::Code kFallInit{0x023F6564};
// daPy_lk_c::setSingleMoveAnime(anm r4, f32 rate f1, f32 start f2, s16 end r5, f32 morf f3)
const release::Code kSetAnime{0x023E0A04};
// the pose at each lift-off (each new L press), chosen at random: the two the user liked best (the
// game's animations for the jump off a ledge and its neighbour), or with the switch "random lift-off
// animations" any of the ones tried (from the airborne procedures and the Deku leaf ones)
const int kLiftAnims[] = {0x3C, 0x3D};
const int kAllLiftAnims[] = {0x0D, 0x3C, 0x3D, 0x3E, 0x30, 0xD3, 0x4C, 0x55, 0x56, 0x57, 0x5D, 0x5E, 0x5F, 0x60};
bool g_l_was = false;
uint32_t g_rand = 0x9E3779B9u;
int random_below(int n) {  // xorshift seeded by the clock; any spread will do for a pose
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        g_rand ^= (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1u;
    }
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return (int)(g_rand % (uint32_t)n);
}  // daPy_lk_c::procFall_init(int, f32) (calls commonProcInit(0x27))
// the stick's horizontal move for this step (moon_jump_move uses it), as when he runs: the stick's
// direction relative to the camera, Link turns towards it at a limited rate and flies the way he faces
void moon_jump_steer(uint32_t link) {
    const float fly = fly_speed();
    static const int turn = (int)env_float("WWHD_MOON_JUMP_TURN", 3000.0f);  // per 30 Hz step (0x8000 = 180 degrees)
    if (!g_face_set) {
        g_face = (int16_t)ld16(link + kShapeAngleY);
        g_face_set = true;
    }
    float sx, sy;
    left_stick(sx, sy);
    const float mag = std::min(1.0f, std::sqrt(sx * sx + sy * sy));
    float vx = 0, vz = 0;
    uint32_t cam = interp::last_camera();
    if (mag >= 0.15f && cam) {
        // camera forward on the ground plane (center - eye); right = forward x up = (-fz, fx)
        float fx = (float)(ldf32(cam + 0xE8) - ldf32(cam + 0xDC)), fz = (float)(ldf32(cam + 0xF0) - ldf32(cam + 0xE4));
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-3f) {
            fx /= fl;
            fz /= fl;
            const float wx = fx * sy - fz * sx, wz = fz * sy + fx * sx;
            const int16_t target = (int16_t)std::lround(std::atan2(wx, wz) * (32768.0 / 3.14159265358979));
            const int step = std::max(1, (int)std::lround(turn * true60::dt()));
            const int16_t diff = (int16_t)(target - g_face);
            g_face = (int16_t)(g_face + std::clamp<int>(diff, -step, step));
            const float a = g_face * (3.14159265358979f / 32768.0f);
            vx = std::sin(a) * fly * mag;
            vz = std::cos(a) * fly * mag;
        }
    }
    face(link);
    g_move_link = link;
    g_move_x = vx;
    g_move_z = vz;
}

void moon_jump_step(Cpu* c, uint32_t link, bool before) {
    if (before) g_move_link = 0;
    if (!link || !(g_infinite.load(std::memory_order_relaxed) & kMoonJump)) {
        g_flight = g_face_set = false;
        return;
    }
    if (g_flight && (g_infinite.load(std::memory_order_relaxed) & kMoonNoFall))
        st32(link + kFallStartY, ld32(link + kPosY));  // no fall height: no hard landing
    const uint32_t mode = ld32(link + kModeFlg);
    if (!l_held()) {
        if (before) g_l_was = false;
        // let go: gravity takes over, the stick still steers until he lands (or swims)
        if (g_flight && (!(mode & kModeMidair) || (mode & kModeSwim))) g_flight = g_face_set = false;
        if (g_flight) {
            if (before) moon_jump_steer(link);
            else if (g_face_set) face(link);
        }
        return;
    }
    const float rise = rise_speed();
    if (before) {
        // into the game's fall: from the ground, from swimming, and from the jump off a ledge (it
        // keeps its own take-off line)
        const bool press = !g_l_was;
        if (ld32(link + kCurProc) != kProcFall) {
            const Cpu saved = *c;
            c->f[1].ps0 = c->f[1].ps1 = 5.0;  // animation blend
            guest_call(c, kFallInit, {link, 0});
            *c = saved;
        }
        if (press) {
            const bool all = g_infinite.load(std::memory_order_relaxed) & kMoonAllAnims;
            const int anm = all ? kAllLiftAnims[random_below((int)(sizeof kAllLiftAnims / sizeof kAllLiftAnims[0]))]
                                : kLiftAnims[random_below((int)(sizeof kLiftAnims / sizeof kLiftAnims[0]))];
            const Cpu saved = *c;
            c->f[1].ps0 = c->f[1].ps1 = 1.0;  // rate
            c->f[2].ps0 = c->f[2].ps1 = 0.0;  // start
            c->f[3].ps0 = c->f[3].ps1 = 5.0;  // blend
            guest_call(c, kSetAnime, {link, (uint32_t)anm, 0xFFFFFFFFu});
            *c = saved;
        }
        st32(link + kAcchFlags, ld32(link + kAcchFlags) & ~kAcchGround);  // not standing: the rise carries him off
        g_flight = true;
    }
    stf32(link + kSpeedY, rise);
    if (before) g_l_was = true;
    if (!g_flight) return;
    if (before) moon_jump_steer(link);
    else if (g_face_set) face(link);
}

void set_moon_jump_speeds(int rise, int fly) {
    g_rise_pct = std::clamp(rise, 10, 1000);
    g_fly_pct = std::clamp(fly, 10, 1000);
    LOG("[cheats] moon jump speeds: rise %d%%, flight %d%%", g_rise_pct.load(), g_fly_pct.load());
}
int moon_jump_rise() { return g_rise_pct.load(); }
int moon_jump_fly() { return g_fly_pct.load(); }

bool moon_jump_move(uint32_t link, float& x, float& z) {
    if (!link || link != g_move_link) return false;
    x = g_move_x;
    z = g_move_z;
    // his facing once more: the fall resets the angles in its procedure (before this move), and the
    // model matrix is built after it (setWorldMatrix, end of his step), so he's drawn turned
    if (g_face_set) face(link);
    return true;
}

// dComIfG_play_c::mNextStage (dStage_nextStage_c): name[8], point s16, room s8, layer s8, enabled s8, wipe u8
constexpr uint32_t kNextStage = 0x5140;

void warp_service() {
    static std::vector<std::string> places;
    static size_t next = 0;
    static int frames = -1, period = 0;
    static int startFrames = 0;  // counted once a save file is loaded (a state can't load at the title)
    if (startFrames >= 0 && save_addr() >= 0x10000000 && save_loaded(save_addr()) && ++startFrames > 30 * 8) {
        startFrames = -1;
        if (const char* e = getenv("WWHD_TEST_LOAD_STATE")) ss::request_load(atoi(e));
    }
    if (frames == -1) {
        frames = 0;
        const char* e = getenv("WWHD_TEST_WARP");
        for (std::string rest = e ? e : ""; !rest.empty();) {
            size_t c = rest.find(',');
            places.push_back(rest.substr(0, c));
            rest = c == std::string::npos ? "" : rest.substr(c + 1);
        }
        const char* sec = getenv("WWHD_TEST_WARP_SECS");
        period = 30 * (sec ? atoi(sec) : 25);
    }
    if (next >= places.size()) {
        static int saveIn = 30 * 15;  // in the last place, after it loaded
        if (!places.empty() && saveIn > 0 && --saveIn == 0)
            if (const char* e = getenv("WWHD_TEST_SAVE_STATE")) ss::request_save(atoi(e));
        return;
    }
    uint32_t s = save_addr();
    if (s < 0x10000000 || !save_loaded(s)) return;
    if (++frames < (next == 0 ? 30 * 5 : period)) return;
    frames = 0;
    const std::string& p = places[next++];
    char name[9] = {};
    int room = 0, point = 0;
    sscanf(p.c_str(), "%8[^:]:%d:%d", name, &room, &point);
    uint32_t n = kStageInfo + kNextStage;
    for (int i = 0; i < 8; i++) st8(n + i, (uint8_t)name[i]);
    st16(n + 8, (uint16_t)(int16_t)point);
    st8(n + 10, (uint8_t)(int8_t)room);
    st8(n + 11, 0xFF);  // layer -1
    st8(n + 13, 0);     // wipe
    st8(n + 12, 1);     // enabled
    LOG("[warp] from %s to %s room %d point %d", stage().c_str(), name, room, point);
}

void cheats_service() {
    warp_service();
    int inf = g_infinite.load(std::memory_order_relaxed) & ~(kMoonJump | kMoonNoFall | kMoonAllAnims);
    if (!g_pending.load(std::memory_order_relaxed) && !inf) return;
    uint32_t s = save_addr();
    if (s < 0x10000000 || !save_loaded(s)) return;  // stays pending until a file is loaded
    // ponytail: topped up once per frame, so a single hit bigger than your whole health still kills
    if (inf & kInfHealth) st16(s + kLife, ld16(s + kMaxLife));
    if (inf & kInfMagic) st8(s + kMagic, ld8(s + kMaxMagic));
    if (inf & kInfAmmo) {
        st8(s + kArrowNum, ld8(s + kArrowMax));
        st8(s + kBombNum, ld8(s + kBombMax));
    }
    if (!g_pending.load(std::memory_order_relaxed)) return;
    int w = g_pending.exchange(0);
    if (w & kCheatItems) all_items(s);
    if (w & kCheatSword) best_sword(s);
    if (w & kCheatStats) max_stats(s);
    if (w & kCheatSongs) st8(s + kTact, 0x3F);  // Wind's Requiem .. Song of Passing
    if (w & kCheatTriforce) st8(s + kTriforce, 0xFF);
    if (w & kCheatDungeon) st8(s + kStageDungeonItems, ld8(s + kStageDungeonItems) | 7);
    if (w & kCheatKey && ld8(s + kStageKeys) < 99) st8(s + kStageKeys, ld8(s + kStageKeys) + 1);
    LOG("[cheats] applied %d in %s: life %u/%u, rupees %u, magic %u, items %02X..%02X, collect %02X %02X, songs %02X, triforce %02X, keys %u, dungeon items %02X",
        w, stage().c_str(), ld16(s + kLife), ld16(s + kMaxLife), ld16(s + kRupee), ld8(s + kMagic), ld8(s + kItems), ld8(s + kItems + 20),
        ld8(s + kCollect), ld8(s + kCollect + 1), ld8(s + kTact), ld8(s + kTriforce), ld8(s + kStageKeys), ld8(s + kStageDungeonItems));
}

}  // namespace mods
