#include <pthread.h>
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "gx2_texture_regs.h"
#include "platform.h"
#include "runtime.h"
#ifdef __ANDROID__
#include "android/display_vsync.h"
#include "android/fps60.h"
#include "android/perf_hint.h"
namespace gfx::rp {  // vk/reproj.cpp: 60 fps (generated)
void layout_root(const uint32_t* regs, bool tv);
void note_camera(const float eye[3], const float center[3], const float up[3], float fovy, float znear, float zfar);
}  // namespace gfx::rp
#endif
#include "mem_writes.h"
#include "../aspect.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
// Register blocks ever written (in g_regs or any context). Every other block is zero everywhere, so
// a context switch copies only these instead of the whole 256 KiB register file.
constexpr uint32 kRegBlock = 64;
static bool g_reg_touched[kNumRegs / kRegBlock];
static std::vector<uint16_t> g_reg_blocks;
static void touch_regs_slow(uint32 first, uint32 n) {
    for (uint32 b = first / kRegBlock, e = (first + n - 1) / kRegBlock; b <= e && b < kNumRegs / kRegBlock; b++)
        if (!g_reg_touched[b]) {
            g_reg_touched[b] = true;
            g_reg_blocks.push_back((uint16_t)b);
        }
}
__attribute__((always_inline)) static inline void touch_regs(uint32 first, uint32 n) {
    if (!n) return;
    uint32 b = first / kRegBlock;  // (mostly one block, touched long ago)
    if (b == (first + n - 1) / kRegBlock && b < kNumRegs / kRegBlock && g_reg_touched[b]) return;
    touch_regs_slow(first, n);
}
static void touch_all_regs() { touch_regs(0, kNumRegs); }
static std::recursive_mutex g_exec_mutex;

uint32* regs() { return g_regs; }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }
// any register change outside shader_irrelevant (the draw fast path in vk_draw.cpp: the shader
// filter leaves out texture addresses and the like, which a draw must resolve again)
extern "C" { uint64_t g_draw_state_gen = 1; }

static bool shader_irrelevant(uint32 reg) {
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32 base : {(uint32)mmSQ_VTX_UNIFORM_BLOCK_START, (uint32)mmSQ_PS_UNIFORM_BLOCK_START, (uint32)mmSQ_GS_UNIFORM_BLOCK_START})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32 w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}

// The renderer may name exactly the register changes that alter its shader key (vk_draw.cpp);
// without a filter every register outside shader_irrelevant counts (the Metal renderer).
extern "C" { bool (*g_shader_reg_filter)(uint32 reg, uint32 oldv, uint32 newv) = nullptr; }
// ... and which registers it needs to look at (by register: kShaderRegNever, kShaderRegAlways, or
// kShaderRegCheck = ask the filter); null: ask the filter for every register
extern "C" { const uint8_t* g_shader_reg_kind = nullptr; }
// ... and is told after a register command changed any of those (first, count: the command's range;
// g_regs holds the new values), so it can keep its own summary of them up to date
extern "C" { void (*g_shader_regs_changed)(uint32 first, uint32 n) = nullptr; }
// bumped when registers change other than through register commands (a context loaded, a save
// state): the renderer's summary is to be rebuilt
extern "C" { uint64_t g_shader_bulk_gen = 1; }
// shader_irrelevant by register (looked up per changed register instead of the range checks)
static uint8_t g_irrelevant[kNumRegs];
static const bool g_irrelevant_filled = [] {
    for (uint32 r = 0; r < kNumRegs; r++) g_irrelevant[r] = shader_irrelevant(r);
    return true;
}();

// debug: WWHD_GEN_STATS=1 counts what bumps g_shader_state_gen (logged with the frame line)
static const bool g_gen_stats = getenv("WWHD_GEN_STATS") != nullptr;
static uint64_t g_gen_ctx = 0, g_gen_ctx_calls = 0, g_gen_regs = 0;
static std::unordered_map<uint32, uint64_t> g_gen_by_reg;

// register writes: copies of a few words mostly (display lists set registers in groups of 1-6), so
// short ones copy inline instead of calling memcpy
__attribute__((always_inline)) static inline void copy_words(uint32* d, const uint32* v, uint32 n) {
    if (n > 8) {
        memcpy(d, v, n * 4);
        return;
    }
    switch (n) {  // (a loop would become a memcpy call again)
    case 8: d[7] = v[7]; [[fallthrough]];
    case 7: d[6] = v[6]; [[fallthrough]];
    case 6: d[5] = v[5]; [[fallthrough]];
    case 5: d[4] = v[4]; [[fallthrough]];
    case 4: d[3] = v[3]; [[fallthrough]];
    case 3: d[2] = v[2]; [[fallthrough]];
    case 2: d[1] = v[1]; [[fallthrough]];
    case 1: d[0] = v[0]; [[fallthrough]];
    default: break;
    }
}
// (file scope: no guard check on every call, as a function-local static has)
static const bool g_coarse_gen = getenv("WWHD_COARSE_SHADER_GEN") != nullptr;  // debug: the old rule
// a write that changes a register (from word i on): which generations it bumps, then the copy
__attribute__((noinline)) static void apply_changed(uint32 first, const uint32* v, uint32 n, uint32 i) {
    const bool coarse = g_coarse_gen;
    bool draw = false, shader = false;
    const uint8_t* kind = coarse ? nullptr : g_shader_reg_kind;
    for (; i < n && !(draw && shader); i++) {
        const uint32 r = first + i;
        if (g_regs[r] == v[i] || g_irrelevant[r]) continue;
        draw = true;  // any state a draw resolves (textures, samplers, targets, ...)
        if (shader) continue;
        bool relevant;
        if (!g_shader_reg_filter || coarse) relevant = true;
        else if (kind) relevant = kind[r] == kShaderRegAlways || (kind[r] == kShaderRegCheck && g_shader_reg_filter(r, g_regs[r], v[i]));
        else relevant = g_shader_reg_filter(r, g_regs[r], v[i]);
        if (relevant) {
            shader = true;
            if (g_gen_stats) {
                g_gen_regs++;
                g_gen_by_reg[first + i]++;
            }
        }
    }
    if (shader) g_shader_state_gen++;
    if (draw) g_draw_state_gen++;
    copy_words(&g_regs[first], v, n);
    if (shader && g_shader_regs_changed) g_shader_regs_changed(first, n);
}

// debug (WWHD_GEN_STATS): register commands by kind, size and adjacency to the previous one
static uint64_t g_rs_calls, g_rs_words, g_rs_alu, g_rs_unchanged, g_rs_adjacent, g_rs_small[5], g_rs_prev_end;
static void reg_stats(uint32 first, uint32 n, bool alu, bool unchanged) {
    g_rs_calls++;
    g_rs_words += n;
    g_rs_alu += alu;
    g_rs_unchanged += unchanged;
    g_rs_adjacent += first == g_rs_prev_end;
    g_rs_prev_end = first + n;
    g_rs_small[n <= 1 ? 0 : n <= 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : 4]++;
}

__attribute__((always_inline)) static inline void apply_regs(uint32 first, const uint32* v, uint32 n) {
    if (first + n > kNumRegs) return;
    touch_regs(first, n);
    // uniform constants (most of the register traffic): no draw or shader state, nothing to compare
    if (first >= mmSQ_ALU_CONSTANT0_0 && first + n <= mmSQ_ALU_CONSTANT0_0 + 0x1000) {
        if (g_gen_stats) reg_stats(first, n, true, false);
        copy_words(&g_regs[first], v, n);
        if (g_shadow) copy_words(&g_shadow[first], v, n);
        return;
    }
    const uint32* r = &g_regs[first];
    uint32 i = 0;
    while (i < n && r[i] == v[i]) i++;
    if (g_gen_stats) reg_stats(first, n, false, i == n);
    if (i < n) apply_changed(first, v, n, i);
    if (g_shadow) copy_words(&g_shadow[first], v, n);
}

// ---------------------------------------------------------------- display list recording
// Display lists being recorded, one slot per recording thread, found by the thread pointer. A
// thread_local here cost a dynamic TLS lookup per command (in a dlopened library every access goes
// through the TLS resolver): the WorkerMgr worker records nearly all the time, so every emit on
// every thread paid it (~4% of the worker on a Helio G85). A slot is only written by its owner.
// The owners sit apart from the positions: every emit on every thread scans the owners, while the
// recording thread writes its position per command (in one cache line, the line kept moving between
// the cores: ~2% of the worker on a Helio G85).
struct alignas(64) Recording {
    uint32 start = 0, pos = 0, end = 0;
};
constexpr int kMaxRecorders = 16;
alignas(64) static std::atomic<uintptr_t> g_rec_owner[kMaxRecorders];
static Recording g_recs[kMaxRecorders];
alignas(64) static std::atomic<int> g_recording{0};  // slots in use
static std::atomic<int> g_rec_slots{0};  // slots ever claimed (the scan's end)
static inline uintptr_t self_id() {
#if defined(__aarch64__) || defined(__x86_64__)
    return (uintptr_t)__builtin_thread_pointer();
#else
    return (uintptr_t)pthread_self();
#endif
}
static inline Recording* find_rec(uintptr_t id) {
    for (int i = 0, n = g_rec_slots.load(std::memory_order_relaxed); i < n; i++)
        if (g_rec_owner[i].load(std::memory_order_relaxed) == id) return &g_recs[i];
    return nullptr;
}
static inline Recording* recording() {
    if (!g_recording.load(std::memory_order_relaxed)) return nullptr;
    return find_rec(self_id());
}
static Recording* claim_rec() {
    uintptr_t id = self_id();
    if (Recording* r = find_rec(id)) return r;
    for (int i = 0; i < kMaxRecorders; i++) {
        uintptr_t z = 0;
        if (g_rec_owner[i].compare_exchange_strong(z, id)) {
            int n = g_rec_slots.load();
            while (n < i + 1 && !g_rec_slots.compare_exchange_weak(n, i + 1)) {}
            g_recording++;
            return &g_recs[i];
        }
    }
    LOG("[gx2] too many threads recording display lists");
    return nullptr;
}
static void release_rec(Recording* r) {
    r->start = r->pos = r->end = 0;
    g_rec_owner[r - g_recs].store(0);
    g_recording--;
}

static void execute_one(Op op, const uint32* p, uint32 n);

// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into GPU work (Metal or Vulkan). WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv, g_q_done_cv;
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;
static uint64_t g_fence_issued = 0, g_fence_done = 0;

static void render_thread_main() {
    platform::set_thread_name("GX2 render");
    platform::set_thread_high_priority();
#ifdef __ANDROID__
    perf_hint::register_render_thread();
    platform::apply_thread_cores(true);  // the app's core setting
#endif
    if (!getenv("WWHD_RENDER_LITTLE")) platform::prefer_big_cores(false);  // test switch: =1 leaves it to the system
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            gfx::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
    }
}

// Commands queued since the last completed render_sync that may read guest memory the game can
// rewrite once GX2DrawDone returns (draws, display lists, copies, clears...). The game ends its frame
// with GX2DrawDone, copy to the scan buffers and swap, then GX2DrawDone again: that second wait only
// waited for the render thread to present (0.5 ms, up to 12 ms at 60 fps on a Snapdragon 7+ Gen 3), with nothing
// of the game's left to protect. GX2DrawDone now waits only when such a command is pending.
// WWHD_LAZY_DRAWDONE=0: always (as before).
static bool g_pending_reads = true;  // (under g_q_mutex)
static uint32 g_pending_ops = 0;     // debug (WWHD_TRACE_DRAWDONE): bit per op that set it
static bool reads_guest(Op op) {
    switch (op) {
    case OP_NOP: case OP_SET_REGS: case OP_COPY_TO_SCAN: case OP_SET_CONTEXT: case OP_SET_PROJ_REGS: case OP_FLUSH:
    case OP_DRAW_DONE: case OP_SWAP: case OP_SETUP_CONTEXT: case OP_FENCE: case OP_WARM:
    case OP_INVALIDATE:  // marks surfaces for a check at their next use (a draw, behind a later wait)
        return false;  // their payload carries what they need (or host state only)
    default:
        return true;
    }
}
static void enqueue2(Op op, bool hasHead, uint32 head, const uint32* payload, uint32 n) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(render_thread_main).detach(); });
    uint32 total = n + (hasHead ? 1 : 0);
    std::lock_guard<std::mutex> lk(g_q_mutex);
    if (reads_guest(op)) {
        g_pending_reads = true;
        g_pending_ops |= 1u << (op & 31);
    }
    size_t at = g_q_pending.size();
    g_q_pending.resize(at + 1 + total);
    uint32* w = g_q_pending.data() + at;
    *w++ = op | (total << 8);
    if (hasHead) *w++ = head;
    memcpy(w, payload, n * 4);
    if (g_q_waiting) g_q_cv.notify_one();
}
static void enqueue(Op op, const uint32* payload, uint32 n) { enqueue2(op, false, 0, payload, n); }

// a fence after everything queued so far, and waiting for the render thread to reach one
static uint64_t issue_fence() {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        id = ++g_fence_issued;
        g_pending_reads = false;  // everything queued so far is done once the fence is
    }
    uint32 w = (uint32)id;
    enqueue(OP_FENCE, &w, 1);
    return id;
}
static void wait_fence(uint64_t id) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    g_q_done_cv.wait(lk, [&] { return g_fence_done >= id; });
}

// block the game thread until the render thread has executed everything queued so far
static void render_sync() {
    if (!g_render_thread) return;
    wait_fence(issue_fence());
}

// experiment (app option): how GX2DrawDone waits for the render thread. 0 for everything queued
// (correct), 1 only for what was queued before the previous GX2DrawDone, 2 not at all. 1 and 2 let
// the game run ahead while the render thread works, at the risk of the game rewriting data the
// render thread still has to read (it reads vertex, index and uniform data when it gets to a draw)
static std::atomic<int> g_drawdone_mode{0};
void set_drawdone_mode(int m) {
    g_drawdone_mode = std::clamp(m, 0, 2);
    LOG("[gx2] GX2DrawDone waits %s", m == 0 ? "for the render thread" : m == 1 ? "one GX2DrawDone behind" : "never");
}
int drawdone_mode() { return g_drawdone_mode; }

// one command made of a head word (if hasHead) and a payload: written straight into the display
// list being recorded, or queued (no staging copy, set_regs)
__attribute__((noinline)) static void emit2_unrecorded(Op op, bool hasHead, uint32 head, const uint32* payload, uint32 n) {
    uint32 total = n + (hasHead ? 1 : 0);
    if (g_render_thread) {
        enqueue2(op, hasHead, head, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    if (!hasHead) {
        execute_one(op, payload, n);
        return;
    }
    std::vector<uint32> buf(total);
    buf[0] = head;
    memcpy(&buf[1], payload, n * 4);
    execute_one(op, buf.data(), total);
}
static inline void emit2(Op op, bool hasHead, uint32 head, const uint32* payload, uint32 n) {
    uint32 total = n + (hasHead ? 1 : 0);
    Recording* r = recording();
    if (!r) {
        emit2_unrecorded(op, hasHead, head, payload, n);
        return;
    }
    uint32 bytes = 4 * (total + 1);
    if (r->pos + bytes > r->end) [[unlikely]] {
        LOG("[gx2] display list overflow at %08X", r->start);
        return;
    }
    uint32* w = (uint32*)mem::ptr(r->pos);
    r->pos += bytes;
    *w++ = op | (total << 8);
    if (hasHead) *w++ = head;
    copy_words(w, payload, n);
}

void emit(Op op, const uint32* payload, uint32 n) { emit2(op, false, 0, payload, n); }

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    execute_one(op, payload.begin(), (uint32)payload.size());
}

// 60 fps (generated, vk/reproj.cpp): the 3D camera of the frame being drawn, in order with its drawing
void framegen_camera(const float v[12]) {
    uint32 w[12];
    memcpy(w, v, sizeof w);
    if (g_render_thread) {
        enqueue(OP_FRAMEGEN_CAMERA, w, 12);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    execute_one(OP_FRAMEGEN_CAMERA, w, 12);
}

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (count) emit2(OP_SET_REGS, true, first, values, count);
}
// (no stack canary for the value passed by address: it is only read)
__attribute__((no_stack_protector)) void set_reg(uint32 reg, uint32 value) { emit2(OP_SET_REGS, true, reg, &value, 1); }

void execute(const uint32* words, uint32 count) {
    uint32 i = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || i + 1 + n > count) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        // register writes (~37000 commands a frame at Windfall, most rewriting the same values) right here,
        // without the calls through execute_one
        if (op == OP_SET_REGS && n) apply_regs(words[i + 1], &words[i + 2], n - 1);
        else execute_one(op, &words[i + 1], n);
        i += 1 + n;
    }
}

// The context whose registers g_regs holds since set_context loaded it: register writes go to both
// (apply_regs), so setting it again changes nothing. Skipping that saved the copy of every used
// register block and, as no shader-relevant register changes, the shader key lookups after it
// (~3% of the render thread at 60 fps). Unknown again after a new context or a loaded save state.
static const uint32* g_loaded_context = nullptr;
static void set_context(uint32 ctx) {
    if (g_gen_stats) g_gen_ctx_calls++;
    if (!ctx) {
        g_shadow = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        return;
    }
    if (g_shadow == it->second.data() && g_loaded_context == g_shadow) return;
    g_shadow = it->second.data();
    static const bool init = (touch_regs(REGADDR::VGT_PRIMITIVE_TYPE, 1), true);  // gfx::draw writes it
    (void)init;
    for (uint16_t b : g_reg_blocks) memcpy(&g_regs[b * kRegBlock], &g_shadow[b * kRegBlock], kRegBlock * 4);
    g_loaded_context = g_shadow;
    g_shader_state_gen++;
    g_shader_bulk_gen++;
    g_draw_state_gen++;
    if (g_gen_stats) g_gen_ctx++;
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}

static void execute_one(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS: apply_regs(p[0], p + 1, n - 1); break;
    case OP_DRAW: gfx::draw(g_regs, p[0], p[1], 0, 0, p[2], p[3]); break;
    case OP_DRAW_INDEXED: gfx::draw(g_regs, p[0], p[1], p[2], p[3], p[4], p[5]); break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        gfx::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        gfx::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 0), bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        uint32 cb = unpack_struct(p, kColorBufferWords, 0), db = unpack_struct(p + kColorBufferWords, kDepthBufferWords, 1);
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        gfx::clear_color(g_regs, cb, rgba);
        gfx::clear_depth_stencil(g_regs, db, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        uint32 src = unpack_struct(p, kSurfaceWords, 0);
        const uint32* q = p + kSurfaceWords;
        uint32 dst = unpack_struct(q + 2, kSurfaceWords, 1);
        const uint32* r = q + 2 + kSurfaceWords;
        gfx::copy_surface(src, q[0], q[1], dst, r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN: gfx::copy_to_scan(unpack_struct(p, kColorBufferWords, 0), p[kColorBufferWords]); break;
    case OP_CALL: execute((const uint32*)mem::ptr(p[0]), p[1] / 4); break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: gfx::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_FLUSH: gfx::flush(); break;
    case OP_DRAW_DONE: gfx::draw_done(); break;
    case OP_SET_PROJ_REGS: {
        uint32 v[16];
        memcpy(v, p + 1, sizeof v);
        float kx, ky;
        if (n == 17 && gfx::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky))
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = gx2::fbits(gx2::bitsf(v[i]) / kx);
                v[4 + i] = gx2::fbits(gx2::bitsf(v[4 + i]) / ky);
            }
        apply_regs(p[0], v, std::min<uint32>(n - 1, 16));
        break;
    }
    case OP_PEEK_Z: gfx::peek_z(p, n); break;
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        const bool tv = gfx::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky);
        aspect::layout_root_target(p[0], tv);
#ifdef __ANDROID__
        gfx::rp::layout_root(g_regs, tv);
#endif
        break;
    }
#ifdef __ANDROID__
    case OP_FRAMEGEN_CAMERA:
        if (n >= 12) {
            float v[12];
            memcpy(v, p, sizeof v);
            gfx::rp::note_camera(v, v + 3, v + 6, v[9], v[10], v[11]);
        }
        break;
#endif
    case OP_SWAP:
        if (n) gfx::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        gfx::swap();
#ifdef __ANDROID__
        {
            static uint32_t swaps = 0;
            if (++swaps % 30 == 0) platform::apply_thread_cores(true);  // the app's core setting (restored if changed)
        }
#endif
        break;
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        g_loaded_context = nullptr;
        break;
    case OP_FENCE: {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_fence_done = std::max<uint64_t>(g_fence_done, p[0]);
        g_q_done_cv.notify_all();
        break;
    }
    case OP_WARM: break;  // (the shader cache warming of the Android port v0.9.5: not here)
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
static uint64_t g_swap_count = 0, g_flip_count = 0;
namespace gx2 { uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); } }  // live fps in the title
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); int vsync_rate(); bool mode40(); }
static std::mutex g_flip_mutex;
static constexpr int64_t kVsyncPeriod = 16683333;  // ns, 59.94 Hz
// The guest vsync clock: index = base_index + (now - base_ns) / period. It runs free at 59.94 Hz;
// on Android it locks to the display's vsync when the panel runs at a multiple of ~60 Hz
// (gx2_display_vsync), so flips land on display refreshes instead of drifting against them
// (a repeated or skipped frame every few seconds). WWHD_NO_VSYNC_LOCK=1 keeps it free running.
static std::mutex g_vclock_mutex;
static int64_t g_vclock_base_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
static uint64_t g_vclock_base_index = 0, g_vclock_last = 0;
static int64_t g_vclock_period = kVsyncPeriod;
// fast forward (mods/turbo.cpp): the clock runs g_vclock_rate times as fast, so the game flips more
// often and plays more frames per second, every one calculated and drawn as usual
static double g_vclock_rate = 1.0;
static int64_t vclock_period() { return g_vclock_rate == 1.0 ? g_vclock_period : (int64_t)(g_vclock_period / g_vclock_rate); }
static int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static uint64_t vclock_index(int64_t t) {  // g_vclock_mutex held
    int64_t d = t - g_vclock_base_ns;
    uint64_t i = g_vclock_base_index + (d > 0 ? (uint64_t)(d / vclock_period()) : 0);
    return g_vclock_last = std::max(i, g_vclock_last);  // re-phasing never moves it back
}
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
struct PendingFlip { uint64_t vsync, swap; uint8_t waitedRender = 0, waitedGpu = 0; };
// pacing diagnostics: why flips missed the vsync they could have had (logged every 300 flips)
static uint64_t g_pace_flips, g_pace_on_time, g_pace_game_late, g_pace_render_late, g_pace_gpu_late;
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

static uint64_t vsync_index() {
    std::lock_guard<std::mutex> lk(g_vclock_mutex);
    return vclock_index(steady_ns());
}
// when the vsync after the current one happens
static std::chrono::steady_clock::time_point next_vsync_time() {
    std::lock_guard<std::mutex> lk(g_vclock_mutex);
    uint64_t next = vclock_index(steady_ns()) + 1;
    int64_t t = g_vclock_base_ns + (int64_t)(next - g_vclock_base_index) * vclock_period();
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(t));
}

// a display vsync (CLOCK_MONOTONIC ns) and the display's refresh period
void gx2_display_vsync(int64_t vsync_ns, int64_t display_period) {
    static const bool off = getenv("WWHD_NO_VSYNC_LOCK") != nullptr;
    if (off || display_period <= 0) return;
    // frame interpolation at 120/240 fps and 40 fps (interp.cpp): the clock runs 2/4 times as fast
    // (40 fps: at 120 Hz, a frame takes 3 of its vsyncs)
    const int64_t target = kVsyncPeriod / std::max(1, interp::vsync_rate());
    int64_t n = std::max<int64_t>(1, (target + display_period / 2) / display_period);
    int64_t period = n * display_period;
    std::lock_guard<std::mutex> lk(g_vclock_mutex);
    if (g_vclock_rate != 1.0) return;  // fast forward: free running until it ends
    if (std::abs(period - target) * 50 > target) {  // e.g. 90 Hz: no multiple near 60 Hz
        if (g_vclock_period != target) {
            int64_t now = steady_ns();
            g_vclock_base_index = vclock_index(now);
            g_vclock_base_ns = now;
            g_vclock_period = target;
        }
        return;
    }
    if (std::abs(period - g_vclock_period) * 200 > period) {  // new rate (beyond 0.5% jitter): continue from the current index
        g_vclock_base_index = vclock_index(steady_ns());
        g_vclock_base_ns = vsync_ns;
        g_vclock_period = period;
        LOG("[gx2] vsync locked to the display: %.2f Hz (%lld x %.2f Hz)", 1e9 / period, (long long)n, 1e9 / display_period);
        return;
    }
    // phase: move our vsyncs onto the nearest display vsync
    int64_t err = (vsync_ns - g_vclock_base_ns) % display_period;
    if (err < 0) err += display_period;
    if (err > display_period / 2) err -= display_period;
    if (std::abs(err) > 250000) g_vclock_base_ns += err;
}

namespace gx2 {
// one virtual vsync (ns): 59.94 Hz, or the display's refresh when the clock is locked to it
int64_t vsync_period_ns() {
    std::lock_guard<std::mutex> lk(g_vclock_mutex);
    return vclock_period();
}
void set_clock_rate(double rate) {
    std::lock_guard<std::mutex> lk(g_vclock_mutex);
    if (rate == g_vclock_rate) return;
    int64_t now = steady_ns();
    g_vclock_base_index = vclock_index(now);
    g_vclock_base_ns = now;
    g_vclock_rate = rate;
}
}  // namespace gx2

// WWHD_LATE_FLIP_WAIT=0: the old fixed grid (a late frame waits for the next vsync)
static bool late_flips() {
    static const bool on = !getenv("WWHD_LATE_FLIP_WAIT") || atoi(getenv("WWHD_LATE_FLIP_WAIT")) != 0;
    return on;
}
static void update_flips() {  // g_flip_mutex held
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        PendingFlip& pf = g_pending_flips.front();
        uint64_t ideal = g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval);
        // `swap interval` vsyncs after the previous flip. A frame swapped after that vsync (it missed
        // it) flips as soon as it is ready instead of on the next vsync (from ZeldaWWHDRecomp, issue
        // #125: a fixed grid made a frame that is a little late cost a whole vsync, so a device that
        // draws ~55 frames a second got 30); one ready on time flips on its vsync as before
        uint64_t at = late_flips() ? ideal : std::max(pf.vsync + 1, ideal);
        if (at > now) break;
        if (gfx::frames_completed() < pf.swap) {  // could flip now, but the frame isn't done
            if (gfx::frames_submitted() < pf.swap) pf.waitedRender = 1;
            else pf.waitedGpu = 1;
            break;
        }
        g_pace_flips++;
        if (pf.vsync + 1 > ideal) g_pace_game_late += pf.vsync + 1 - ideal;  // the swap came after its vsync
        if (now > at) {
            if (pf.waitedRender) g_pace_render_late += now - at;
            else g_pace_gpu_late += now - at;
        } else if (pf.vsync + 1 <= ideal) {
            g_pace_on_time++;
        }
        if (g_pace_flips % 300 == 0) {
            LOG("[pace] last 300 flips: %llu on time; vsyncs lost: game late %llu, render late %llu, GPU late %llu",
                (unsigned long long)g_pace_on_time, (unsigned long long)g_pace_game_late, (unsigned long long)g_pace_render_late,
                (unsigned long long)g_pace_gpu_late);
            g_pace_on_time = g_pace_game_late = g_pace_render_late = g_pace_gpu_late = 0;
        }
        at = now;
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_time = timebase::now();
        g_flip_count++;
    }
}

HLE(gx2, GX2Init) {
    set_default_state();
#ifdef __ANDROID__
    display_vsync::start();
#endif
    LOG("[gx2] initialized (native GX2 -> %s)", gfx::backend_name());
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    if (Recording* r = claim_rec()) {
        r->start = r->pos = arg(c, 0);
        r->end = arg(c, 0) + arg(c, 1);
    }
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = 0;
    if (Recording* r = recording()) {
        size = r->pos - r->start;
        release_rec(r);
    }
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    Recording* r = recording();
    if (arg(c, 0)) st32(arg(c, 0), r ? r->start : 0);
    if (arg(c, 1)) st32(arg(c, 1), r ? r->end - r->start : 0);
    ret(c, r != nullptr);
}
HLE(gx2, GX2CallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }
HLE(gx2, GX2DirectCallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(std::vector<uint32>& p, uint32 addr, uint32 words) {
    size_t at = p.size();
    p.resize(at + words);
    if (addr) memcpy(&p[at], mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p.push_back(fbits(farg(c, i)));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), arg(c, 1) & 0xFF, arg(c, 2)});
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    put_struct(p, arg(c, 1), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)), fbits(farg(c, 4)),
                       arg(c, 2) & 0xFF, arg(c, 3)});
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
HLE(gx2, GX2CopySurface) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kSurfaceWords);
    p.insert(p.end(), {arg(c, 1), arg(c, 2)});
    put_struct(p, arg(c, 3), kSurfaceWords);
    p.insert(p.end(), {arg(c, 4), arg(c, 5)});
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // The copy is complete when GX2CopySurface returns: the game uses the result (and frees the
    // surfaces) right away. agl's tile-mode conversion (027B5EEC) copies into a temporary surface,
    // OSBlockMoves it back and frees it at once; executed later on the render thread, the copy wrote
    // into the freed memory after the heap had reused it (intermittent boot crash; original project
    // 5070881). A linear destination is also read by the CPU right away: the Picto Box JPEG-encodes
    // its picture from one (issue #22; the render thread writes it back, vk_surfaces.cpp). Not for
    // display lists: they run when called.
    if (!recording()) {
        BlockingScope b;
        render_sync();
    }
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    p.push_back(arg(c, 1));
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) {
    // the CPU bit flushes the range on the console (mem_writes.h); "everything" carries no information
    if ((arg(c, 0) & 0x40) && arg(c, 2) < 0x10000000) memw::mark(arg(c, 1), arg(c, 2));
    emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)});
}

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { emit_host(OP_FLUSH, {}); }
// GX2DrawDone calls and the time the game spent in them, for the periodic frame report
static std::atomic<uint64_t> g_drawdone_calls{0}, g_drawdone_us{0};
HLE(gx2, GX2DrawDone) {
    BlockingScope b;
    auto t0 = std::chrono::steady_clock::now();
    emit_host(OP_DRAW_DONE, {});
    int mode = g_drawdone_mode.load(std::memory_order_relaxed);
    // mode 0: waits only while commands that read game memory are pending (upstream v0.9;
    // WWHD_LAZY_DRAWDONE=0 always waits); 1 and 2: the experiments above
    if (mode == 0 || !g_render_thread) {
        static const bool lazy = !getenv("WWHD_LAZY_DRAWDONE") || atoi(getenv("WWHD_LAZY_DRAWDONE")) != 0;
        bool wait = true;
        uint32 dbgOps = 0;
        if (lazy && g_render_thread) {
            std::lock_guard<std::mutex> lk(g_q_mutex);
            wait = g_pending_reads;
            dbgOps = g_pending_ops;
            g_pending_ops = 0;
        }
        if (wait) render_sync();
        // debug: WWHD_TRACE_DRAWDONE=n logs the guest call chain and the wait of n calls every 600
        static const int trace = getenv("WWHD_TRACE_DRAWDONE") ? atoi(getenv("WWHD_TRACE_DRAWDONE")) : 0;
        static uint64_t calls = 0;
        if (trace && calls++ % 600 < (uint64_t)trace) {
            char buf[256];
            int n = snprintf(buf, sizeof buf, "[gx2] DrawDone %s %.2f ms (ops %X), from lr=%08X", wait ? "waited" : "no wait",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), dbgOps, c->lr);
            uint32_t sp = c->r[1];
            for (int i = 0; i < 8 && sp; i++) {
                uint32_t prev = ld32(sp);
                if (!prev || prev <= sp) break;
                n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
                sp = prev;
            }
            LOG("%s", buf);
        }
    } else {
        static uint64_t previous = 0;
        uint64_t id = issue_fence();
        if (mode == 1 && previous) wait_fence(previous);
        previous = id;
    }
    g_drawdone_calls++;
    g_drawdone_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    ret(c, 1);
}
HLE(gx2, GX2SwapScanBuffers) {
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    emit_host(OP_SWAP, {gx2::fbits(a)});
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        g_swap_count++;
        g_pending_flips.push_back({vsync_index(), g_swap_count});
    }
#ifdef __ANDROID__
    fps60::on_swap();
    {
        static uint32_t swaps = 0;
        if (swaps++ % 30 == 0) platform::apply_thread_cores(false);  // the game thread follows the core setting
    }
    perf_hint::on_swap(interp::effective_swap_interval(g_swap_interval));
#endif
    if (g_swap_count % 300 == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        int64_t gameNs = 0, renderNs = 0, recordNs = 0;
#ifdef __ANDROID__
        perf_hint::recent_work(gameNs, renderNs, recordNs);
#endif
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u; GX2DrawDone %.1f/frame, %.1f ms/frame; CPU per frame: game %.1f ms, render %.1f ms, record %.1f ms",
            (unsigned long long)g_swap_count, 300 / s, g_swap_interval, g_drawdone_calls.exchange(0) / 300.0,
            g_drawdone_us.exchange(0) / 300.0 / 1000.0, gameNs / 1e6, renderNs / 1e6, recordNs / 1e6);
#ifdef __ANDROID__
        int64_t game_ns, render_ns, record_ns;
        perf_hint::recent_work(game_ns, render_ns, record_ns);
        LOG("[gx2] CPU per frame: game thread %.1f ms, render thread %.1f ms, record thread %.1f ms",
            game_ns / 1e6, render_ns / 1e6, record_ns / 1e6);
#endif
        if (getenv("WWHD_SCHED_STATS")) threads::report_sched();
        if (g_gen_stats) {
            std::vector<std::pair<uint64_t, uint32>> top;
            for (auto& [r, c] : g_gen_by_reg) top.push_back({c, r});
            std::sort(top.rbegin(), top.rend());
            std::string t;
            for (size_t k = 0; k < top.size() && k < 8; k++) {
                char b[32];
                snprintf(b, sizeof b, " %04X:%.1f", top[k].second, top[k].first / 300.0);
                t += b;
            }
            LOG("[gx2] per frame: set_context %.1f (loads %.1f), shader-relevant register bumps %.1f; by register%s", g_gen_ctx_calls / 300.0,
                g_gen_ctx / 300.0, g_gen_regs / 300.0, t.c_str());
            g_gen_ctx = g_gen_ctx_calls = g_gen_regs = 0;
            g_gen_by_reg.clear();
            LOG("[gx2] per frame: register commands %.0f (%.0f words; uniform constants %.0f, unchanged %.0f, adjacent to the previous %.0f; "
                "by size 1:%.0f 2:%.0f 3-4:%.0f 5-8:%.0f more:%.0f)", g_rs_calls / 300.0, g_rs_words / 300.0, g_rs_alu / 300.0,
                g_rs_unchanged / 300.0, g_rs_adjacent / 300.0, g_rs_small[0] / 300.0, g_rs_small[1] / 300.0, g_rs_small[2] / 300.0,
                g_rs_small[3] / 300.0, g_rs_small[4] / 300.0);
            g_rs_calls = g_rs_words = g_rs_alu = g_rs_unchanged = g_rs_adjacent = 0;
            for (auto& k : g_rs_small) k = 0;
        }
    }
}
// One frame of buffering in the 60 fps modes. The game starts a pass only once every frame it
// swapped has been flipped (f_0274C874: wait for a vsync, then until flips == swaps) and each pass
// starts on a vsync. Logic passes there take 13-17 ms and the passes in between 3-6 ms: a logic
// pass that runs past its vsync cost a whole vsync of waiting, and the late logic step made the
// interpolation drop in-between frames for a while (30 fps runs of ~200 ms, 46-53 fps in Outset on
// an S20 FE). With buffering, a frame still waiting for its flip doesn't hold the game back (one
// at most: the flip count it sees is one short at most), and after a pass that ran past its vsync
// the next one starts at once instead of on the vsync after: the short pass after a long one makes
// up the time. Costs one frame (16.7 ms) of latency while a frame is queued.
// WWHD_PACE_BUFFER=0 turns it off.
static bool pace_buffered() {
    static const bool on = !getenv("WWHD_PACE_BUFFER") || atoi(getenv("WWHD_PACE_BUFFER")) != 0;
    return on && interp::effective_swap_interval(g_swap_interval) == 1 && !interp::mode40();
}
static uint64_t g_pass_vsync = 0;  // vsync index when the game's last vsync wait returned (its pass began)
static bool g_pass_borrowed = false;

HLE(gx2, GX2GetSwapStatus) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    uint64_t flips = g_flip_count;
    if (pace_buffered() && g_swap_count == g_flip_count + 1) flips = g_swap_count;  // one queued frame counts as flipped
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(flips + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
// A flip whose vsync has come but whose frame isn't ready yet: GX2WaitForVsync waits for that frame
// in short steps instead of sleeping to the next vsync, and returns once the flip has executed
// (ZeldaWWHDRecomp, issue #125). False: no such flip (or the deadline came first).
static bool wait_for_late_flip(std::chrono::steady_clock::time_point deadline) {
    auto overdue = [] {  // g_flip_mutex held
        return !g_pending_flips.empty() && g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval) <= vsync_index() &&
               gfx::frames_completed() < g_pending_flips.front().swap;
    };
    uint64_t flips;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        flips = g_flip_count;
        update_flips();
        if (g_flip_count != flips) return true;
        if (!overdue()) return false;
    }
    constexpr auto kStep = std::chrono::microseconds(250);
    for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
        threads::park_sleep_until(std::min(deadline, now + kStep));
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        if (g_flip_count != flips) return true;
        if (!overdue()) return false;
    }
    return false;
}

HLE(gx2, GX2WaitForVsync) {
    if (late_flips() && !pace_buffered() && wait_for_late_flip(next_vsync_time())) {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        g_pass_vsync = vsync_index();
        g_pass_borrowed = false;
        return;
    }
    if (pace_buffered()) {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        const uint64_t now = vsync_index();
        // the pass that began on vsync g_pass_vsync ran past the next one: start the next pass now
        // (not twice in a row, and with at most one frame waiting for its flip)
        if (!g_pass_borrowed && g_pass_vsync && now > g_pass_vsync && g_swap_count <= g_flip_count + 1) {
            g_pass_borrowed = true;
            g_pass_vsync = now;
            return;
        }
    }
    threads::park_sleep_until(next_vsync_time(), true);  // precise: the frame starts on its vsync
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    g_pass_vsync = vsync_index();
    g_pass_borrowed = false;
    update_flips();
    static uint64_t calls = 0;
    if (getenv("WWHD_LOG_VSYNC") && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    LOG("[gx2] TV buffer format %X, mode %u (1-2 480p, 3 720p, 5-7 1080p), %u buffers", arg(c, 3), arg(c, 2), arg(c, 4));
    gfx::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { gfx::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states
#include "../savestate.h"
namespace gfx { void ss_reset_surfaces(); }

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {});
    render_sync();
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    w.u32(kNumRegs);
    w.bytes(g_regs, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : g_contexts) {
        keys.push_back(k);
        if (g_shadow == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)g_contexts[k].size());
        w.bytes(g_contexts[k].data(), g_contexts[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    r.bytes(g_regs, sizeof g_regs);
    g_shader_bulk_gen++;
    touch_all_regs();  // a loaded state may use any register
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_loaded_context = nullptr;
    g_shader_state_gen++;
    g_draw_state_gen++;
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    gfx::ss_reset_surfaces();
    aspect::ss_reset();  // layouts recompute their matrices for the current aspect
}
