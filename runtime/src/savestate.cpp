// Save states: snapshot the whole running game into one of 5 slots and restore it, also in a new
// process.
//
// The recompiled game runs on host threads with native call stacks, so a snapshot can only be taken
// (and restored) when every guest thread is at a point whose complete state is guest memory, its Cpu
// registers and the HLE objects:
//   - the game's main thread at the top of the per-frame function (0203593C; interp.cpp calls
//     service() there), which requests the save/load;
//   - every other guest thread parked in an HLE wait (OSReceiveMessage, OSWaitEvent, OSLockMutex,
//     OSSleepThread, OSSleepTicks, GX2WaitForVsync, ...: park_wait in threads.cpp). A parked thread
//     re-checks its condition after waking and only then consumes anything, so the HLE objects
//     decide where it continues;
//   - service threads (alarms, AX frame) idle between their callbacks.
// threads::quiesce() freezes the game in that state (other threads keep running until they park).
//
// Restoring into a running process requires each thread to be parked at the same place it was
// saved at (same stack pointer, return address and guest back chain => same host call stack); the
// main thread always is. Worker threads normally idle at the same wait. If not, the load is retried
// on the following frames and finally refused with a message; nothing is changed then.
//
// Slot file (version 2): header, the sections from each module (threads + HLE sync objects + alarms
// + guest clock, heaps, open files, AX voices, GX2 registers, runtime allocations) zstd-compressed,
// then guest memory (MEM2, runtime objects, fixed slots, foreground bucket, MEM1) as 64 KiB chunks
// compressed one by one (all-zero chunks are left out). Chunks are compressed straight from guest
// memory while the game is frozen and stay compressed in memory until they're restored, so neither
// saving nor loading holds the raw ~580 MB (that got the game killed for low memory on 6 GB phones).
// Version 1 (read only): zlib blocks (LZ4 on macOS) of one payload holding sections and raw memory.
#include "savestate.h"

#ifdef __APPLE__
#include <compression.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#else
#include <elf.h>
#include <link.h>
#include <zlib.h>  // blocks are deflated instead of LZ4 (states don't move between platforms)
#endif
#include <zstd.h>
// chunk_hash (its own copy of the inlined functions: perf_sites.cpp has one too, under WWHD_PERF_)
#define XXH_NAMESPACE WWHD_SS_
#define XXH_INLINE_ALL
#include "../third_party/zstd/lib/common/xxhash.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "runtime.h"
#include "platform.h"
#include "input.h"
#include "crash_info.h"
#include "interp.h"
#include "mem_writes.h"
#include "mods/mods.h"
#include "release.h"

// module sections
bool threads_ss_save(ss::Writer& w, std::string& why);
bool threads_ss_check(ss::Reader r, std::string& why);
void threads_ss_load(ss::Reader& r);
void threads_ss_targets(ss::Reader r);
void mem_ss_save(ss::Writer& w);
bool mem_ss_check(ss::Reader r, std::string& why);
void mem_ss_load(ss::Reader& r);
void fs_ss_save(ss::Writer& w);
void fs_ss_load(ss::Reader& r);
void ax_ss_save(ss::Writer& w);
bool ax_ss_check(ss::Reader r, std::string& why);
void ax_ss_load(ss::Reader& r);
void gx2_ss_drain();
void gx2_ss_save(ss::Writer& w);
bool gx2_ss_check(ss::Reader r, std::string& why);
void gx2_ss_load(ss::Reader& r);
namespace interp { void ss_reset(); }
namespace mods {  // what the mods remember of the moment (a flight, a climb, a door event): over
void cheats_ss_reset();
void climb_ss_reset();
void turbo_ss_reset();
}
namespace gfx {
uint64_t frame_count();
void request_tv_dump(const std::string& path, int frames_ahead);
}
namespace dispatch { std::vector<std::pair<uint32_t, std::string>> host_functions(); }

namespace ss {
namespace {

constexpr char kMagic[8] = {'W', 'W', 'H', 'D', 'S', 'T', 'A', 'T'};
constexpr uint32_t kVersion = 2;  // written; 1 is still read
constexpr int kZstdLevel = 1;
constexpr uint32_t kChunk = 0x10000;
constexpr uint32_t kBlock = 8 << 20;  // compression block (raw bytes)

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t created;       // unix time
    char area[32];          // stage name
    uint8_t build[16];      // LC_UUID of the executable that wrote it (informational)
    uint64_t game_id;       // hash of cking.rpx
    uint32_t cpu_size;      // sizeof(Cpu)
    uint32_t blocks;        // compressed blocks that follow
    uint64_t raw_size;      // payload bytes
    // added later (files written before end at header_size == kHeaderV1Size and read these as 0)
    uint32_t controller;    // the controls at the time: 0 unknown, 1 GamePad, 2 Pro Controller
    uint32_t reserved;
};
constexpr uint32_t kHeaderV1Size = offsetof(Header, controller);

enum : uint32_t {
    kSecThreads = 'THRD',
    kSecHeaps = 'HEAP',
    kSecFiles = 'FILE',
    kSecAudio = 'AX  ',
    kSecGx2 = 'GX2 ',
    kSecAllocs = 'RALC',
    kSecDispatch = 'DSPT',
    kSecMemory = 'MEM ',
};

struct Region { uint32_t base, size; };

std::vector<Region> regions() {
    uint32_t top = (mem::runtime_top() + kChunk - 1) & ~(kChunk - 1);
    return {{mem::kMem2Start, mem::kMem2End - mem::kMem2Start},
            {mem::kRuntimeStart, top - mem::kRuntimeStart},
            {mem::kFixedStart, mem::kFixedSize},
            {mem::kFgBucket, mem::kFgBucketSize},
            {mem::kMem1, mem::kMem1Size}};
}

// a memory chunk of a snapshot: 64 KiB as they are (comp == 0) or zstd-compressed (comp bytes)
struct ChunkRef {
    const uint8_t* p = nullptr;
    uint32_t comp = 0;
};
bool chunk_data(const ChunkRef& c, uint8_t* dst) {
    if (!c.comp) {
        memcpy(dst, c.p, kChunk);
        return true;
    }
    return ZSTD_decompress(dst, kChunk, c.p, c.comp) == kChunk;
}

// A snapshot in memory: header + payload (sections; version 1 also the raw memory) + the memory
// chunks (version 2: compressed, in blob).
struct Snapshot {
    Header h{};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> blob;
    // version 2: the slot file mapped read-only, the chunks point into it. File pages, not the app's
    // own memory: Android can drop and re-read them under pressure instead of ending the game (the
    // ~235 MB read into a buffer got the game killed for low memory on 6 GB phones)
    void* map = nullptr;
    size_t mapSize = 0;
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    ~Snapshot() {
        if (map) munmap(map, mapSize);
    }
    std::map<uint32_t, std::pair<size_t, size_t>> sections;  // tag -> (offset, size)
    std::unordered_map<uint32_t, ChunkRef> chunks;            // chunk address -> data (absent = zero)
    std::vector<Region> regs;
    int slot = 0;
    bool converted = false;  // read from a version 1 file (request_load writes it back as version 2)

    Reader section(uint32_t tag) const {
        auto it = sections.find(tag);
        if (it == sections.end()) return Reader(nullptr, 0);
        return Reader(payload.data() + it->second.first, it->second.second);
    }
    bool parse_sections() {
        Reader r(payload.data(), payload.size());
        while (r.ok && !r.at_end()) {
            uint32_t tag = r.u32();
            uint64_t n = r.u64();
            size_t off = r.p - payload.data();
            if (!r.bytes(nullptr, n)) return false;
            sections[tag] = {off, (size_t)n};
        }
        return r.ok;
    }
    // version 1: the memory is a section of the payload
    bool parse() {
        if (!parse_sections() || !sections.count(kSecMemory)) return false;
        Reader m = section(kSecMemory);
        uint32_t nr = m.u32();
        for (uint32_t i = 0; i < nr && m.ok; i++) {
            Region g{m.u32(), m.u32()};
            regs.push_back(g);
            uint32_t present = m.u32();
            for (uint32_t k = 0; k < present && m.ok; k++) {
                uint32_t idx = m.u32();
                chunks[g.base + idx * kChunk] = ChunkRef{m.p, 0};
                if (!m.bytes(nullptr, kChunk)) return false;
            }
        }
        return m.ok;
    }
};

// a worker thread's body: off the big cores the game thread that starts it may be kept on (a new
// thread inherits that; 8 workers on one core made a 565 MB capture take over a second)
template <class F> auto any_core(F& f) {
    return [&f] {
        platform::set_thread_name("wwhd-state");
        f();
    };
}

std::mutex g_mu;  // messages, pending requests
std::mutex g_io;  // one slot file operation at a time
std::string g_message;
std::chrono::steady_clock::time_point g_message_time;
std::atomic<int> g_save_req{0};
std::shared_ptr<Snapshot> g_load_ready;   // decompressed, waiting for the frame boundary
// A state loads only where every game thread waits where it waited when it was saved (their host call
// stacks can't be saved: the game code runs natively), which is true while a file is being played but
// not at the title or the file select (other threads run there). A load asked for there goes into the
// game first, by itself: A is held in pulses (title, controller choice, the last file played) until a
// file is being played, then the state loads as usual; the screen shows a notice meanwhile.
bool g_entering = false;
std::chrono::steady_clock::time_point g_entering_since;
uint64_t g_play_frames = 0;  // frames a file has been played without a pause (since entering)
std::atomic<bool> g_loading{false};       // a slot file is being read
std::atomic<int> g_autosave_secs{0};      // autosave interval in seconds (0 = off)
std::atomic<bool> g_autosave_now{false};  // the app is going to the background: an autosave first
std::atomic<bool> g_autosave_restart{false};  // a state was loaded: the next autosave a whole interval later
int g_attempts = 0;
const Snapshot* g_check = nullptr;        // snapshot whose memory snap_ld32 reads
uint64_t g_check_serial = 0;              // bumped with each g_check (snap_ld32's unpacked chunk)

void message(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void message(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("[savestate] %s", buf);
    std::lock_guard<std::mutex> lk(g_mu);
    g_message = buf;
    g_message_time = std::chrono::steady_clock::now();
}

void make_dirs(const std::string& d) {
    for (size_t i = 1; i <= d.size(); i++)
        if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
}

std::string state_dir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = getenv("WWHD_STATE_DIR")) d = e;
#ifdef __APPLE__
        else d = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Library/Application Support/wwhd/states";
#else
        else d = config::save_dir.substr(0, config::save_dir.find_last_of('/')) + "/states";  // next to save/
#endif
        make_dirs(d);
        // what a run that ended while writing left behind (slotN.tmp: a slot file being put together,
        // slot6.pre: the autosave's capture ahead): of no use to this one
        for (int slot = 1; slot <= kAutoSlot; slot++) unlink((d + "/slot" + std::to_string(slot) + ".tmp").c_str());
        unlink((d + "/slot" + std::to_string(kAutoSlot) + ".pre").c_str());
        return d;
    }();
    return dir;
}
std::string slot_path(int slot, const char* ext = "bin") { return state_dir() + "/slot" + std::to_string(slot) + "." + ext; }

void build_uuid(uint8_t out[16]) {
    memset(out, 0, 16);
#ifdef __APPLE__
    const auto* h = (const mach_header_64*)&_mh_execute_header;
    const uint8_t* p = (const uint8_t*)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const auto* lc = (const load_command*)p;
        if (lc->cmd == LC_UUID) { memcpy(out, ((const uuid_command*)lc)->uuid, 16); return; }
        p += lc->cmdsize;
    }
#else
    // the GNU build id of the image containing this function
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            uintptr_t self = (uintptr_t)&build_uuid;
            bool mine = false;
            for (int i = 0; i < info->dlpi_phnum; i++) {
                const auto& ph = info->dlpi_phdr[i];
                uintptr_t a = info->dlpi_addr + ph.p_vaddr;
                if (ph.p_type == PT_LOAD && self >= a && self < a + ph.p_memsz) mine = true;
            }
            if (!mine) return 0;
            for (int i = 0; i < info->dlpi_phnum; i++) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                const uint8_t* p = (const uint8_t*)(info->dlpi_addr + ph.p_vaddr);
                const uint8_t* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    const auto* n = (const ElfW(Nhdr)*)p;
                    const uint8_t* desc = p + sizeof *n + ((n->n_namesz + 3) & ~3u);
                    if (n->n_type == NT_GNU_BUILD_ID) {
                        memcpy(data, desc, std::min<size_t>(16, n->n_descsz));
                        return 1;
                    }
                    p = desc + ((n->n_descsz + 3) & ~3u);
                }
            }
            return 1;
        },
        out);
#endif
}

// whether the 64 KiB chunk at p may hold data (its pages were ever touched; untouched pages are zero
// and are skipped without reading them, which would commit them). macOS: mincore() tells, for a page
// paged out too. Linux/Android: mincore() reports residency only, and a page swapped out (zram) still
// holds data, so there every chunk is read (a never touched page maps the shared zero page when read:
// nothing is committed; the zero test of the caller leaves it out)
bool chunk_touched(const uint8_t* p) {
#ifdef __APPLE__
    char vec[kChunk / 4096 + 1];
    if (mincore(p, kChunk, vec) != 0) return true;
    for (size_t i = 0; i < kChunk / (size_t)getpagesize(); i++)
        if (vec[i]) return true;
    return false;
#else
    (void)p;
    return true;
#endif
}

bool chunk_zero(const uint8_t* p) {
    const uint64_t* q = (const uint64_t*)p;
    for (size_t i = 0; i < kChunk / 8; i++)
        if (q[i]) return false;
    return true;
}

// one block of a version 1 slot file
size_t decompress_block(uint8_t* dst, size_t n, const uint8_t* src, size_t srcSize, void* scratch) {
#ifdef __APPLE__
    return compression_decode_buffer(dst, n, src, srcSize, (uint8_t*)scratch, COMPRESSION_LZ4);
#else
    uLongf out = n;
    return uncompress(dst, &out, src, srcSize) == Z_OK ? out : 0;
#endif
}
size_t scratch_size() {
#ifdef __APPLE__
    return compression_decode_scratch_buffer_size(COMPRESSION_LZ4);
#else
    return 1;
#endif
}

uint64_t game_id() {
    static const uint64_t id = [] {
        uint64_t h = 0xcbf29ce484222325ull;
        FILE* f = fopen((config::game_dir + "/code/cking.rpx").c_str(), "rb");
        if (!f) return h;
        std::vector<uint8_t> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
            for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
        fclose(f);
        return h;
    }();
    return id;
}

// current stage (dComIfG_gameInfo.play: the start stage, 8 chars)
const release::Data kStageInfo{0x1046F0B0};
constexpr uint32_t kStageNameOff = 0x5134;  // dStage_startStage_c (next stage at +0x5140)
std::string stage_name() {
    if (const char* e = getenv("WWHD_STATE_STAGE_ADDR")) {
        uint32_t a = (uint32_t)strtoul(e, nullptr, 16);
        return std::string((const char*)mem::ptr(a), strnlen((const char*)mem::ptr(a), 8));
    }
    const char* p = (const char*)mem::ptr(kStageInfo + kStageNameOff);
    size_t n = strnlen(p, 8);
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7E) return "";
    return std::string(p, n);
}

// ---------------------------------------------------------------- memory
// the snapshot's memory into guest memory, on every core: the saved chunks unpacked straight into it
// (read_slot checked them), the chunks of the regions it doesn't hold zeroed where they aren't.
// Returns how many chunks could not be unpacked (the load reports them).
int restore_memory(const Snapshot& s) {
    std::vector<std::pair<uint32_t, ChunkRef>> list(s.chunks.begin(), s.chunks.end());
    std::vector<uint32_t> clear;
    for (auto& g : s.regs)
        for (uint32_t off = 0; off < g.size; off += kChunk)
            if (!s.chunks.count(g.base + off) && chunk_touched(mem::ptr(g.base + off))) clear.push_back(g.base + off);
    std::atomic<size_t> next{0};
    std::atomic<int> bad{0};
    auto work = [&] {
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < list.size() + clear.size();) {
            if (i < list.size()) {
                if (!chunk_data(list[i].second, mem::ptr(list[i].first))) bad++;
                continue;
            }
            uint8_t* p = mem::ptr(clear[i - list.size()]);
            if (!chunk_zero(p)) memset(p, 0, kChunk);
        }
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(any_core(work));
    work();
    for (auto& th : pool) th.join();
    if (bad) LOG("[savestate] %d memory chunks could not be unpacked", bad.load());
    return bad;
}

// guest memory captured straight into the slot file while the game is frozen: the chunks that aren't
// all zero, compressed on every core, each thread writing its records out in ~4 MB batches. Only
// those batches are held (holding all ~235 MB of a state until a background thread wrote them got the
// game killed for low memory a few seconds after saving on 6 GB phones); the file's pages are the
// system's to write back. Records are in no particular order (read_slot_v2 doesn't need one).
struct StreamResult {
    bool ok = false;
    size_t chunks = 0, bytes = 0;  // chunks written and their compressed bytes
};

StreamResult capture_to_file(FILE* f, std::mutex& fm) {
    StreamResult res;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Region> regs = regions();
    // the chunks that may hold data; the all-zero ones among them are found by the threads below
    // (on Android that is every chunk: ~1.2 GB looked at, so on every core)
    std::vector<uint32_t> addrs;
    for (auto& g : regs)
        for (uint32_t off = 0; off < g.size; off += kChunk)
            if (chunk_touched(mem::ptr(g.base + off))) addrs.push_back(g.base + off);
    // the regions go first; the count is patched into the header by the caller
    uint32_t nr = (uint32_t)regs.size();
    bool ok = fwrite(&nr, 4, 1, f) == 1 && fwrite(regs.data(), sizeof(Region), nr, f) == nr;
    std::atomic<bool> good{ok};
    std::atomic<size_t> bytes{0}, chunks{0};
    std::atomic<size_t> next{0};
    constexpr size_t kBatch = 4 << 20;
    static const int level = getenv("WWHD_SS_LEVEL") ? atoi(getenv("WWHD_SS_LEVEL")) : kZstdLevel;
    auto work = [&] {
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        std::vector<uint8_t> tmp(ZSTD_compressBound(kChunk));
        std::vector<uint8_t> out;
        out.reserve(kBatch + kChunk + 16);
        auto flush = [&] {
            if (out.empty()) return;
            std::lock_guard<std::mutex> lk(fm);
            if (good && fwrite(out.data(), 1, out.size(), f) != out.size()) good = false;
            bytes += out.size();
            out.clear();
        };
        for (size_t i; good && (i = next.fetch_add(1, std::memory_order_relaxed)) < addrs.size();) {
            const uint8_t* src = mem::ptr(addrs[i]);
            if (chunk_zero(src)) continue;  // left out (restore_memory zeroes what isn't in the state)
            chunks++;
            size_t n = cx ? ZSTD_compressCCtx(cx, tmp.data(), tmp.size(), src, kChunk, level) : 0;
            const bool stored = !cx || ZSTD_isError(n) || n >= kChunk;
            uint32_t rec[2] = {addrs[i], stored ? 0u : (uint32_t)n};
            out.insert(out.end(), (const uint8_t*)rec, (const uint8_t*)rec + 8);
            if (stored) out.insert(out.end(), src, src + kChunk);
            else out.insert(out.end(), tmp.data(), tmp.data() + n);
            if (out.size() >= kBatch) flush();
        }
        flush();
        ZSTD_freeCCtx(cx);
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(any_core(work));
    work();
    for (auto& th : pool) th.join();
    LOG("[savestate] %zu of %zu chunks hold data: compressed and written in %.1f ms (%u threads, level %d)", chunks.load(), addrs.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), nt, level);
    res.ok = good;
    res.chunks = chunks;
    res.bytes = bytes;
    return res;
}


// ---------------------------------------------------------------- the autosave's capture ahead
// Writing ~235 MB with the game frozen held it for 0.4-1.6 s (the page cache taking that much at
// once; compressing is the smaller part). Before an autosave is due, the memory is compressed into
// slotN.pre while the game runs (background threads, copies of the chunks, each one's hash kept);
// at the autosave only the chunks whose hash changed since are compressed while frozen, into memory,
// and the slot file is put together afterwards: header and sections of the frozen moment, the
// regions, the records written ahead, then the changed ones (read_slot_v2: a later record of an
// address replaces an earlier one; a chunk that became zero gets a record of zeros).
// xxh64 over the chunk (~10 GB/s on a big core; a changed chunk that keeps its hash is as good as
// impossible, which the 16-lane xxh32 rounds this replaced didn't quite give)
uint64_t chunk_hash(const uint8_t* p) { return XXH64(p, kChunk, 0); }

// the chunks of `regs` that may hold data (chunk_touched: on Android all of them)
std::vector<uint32_t> touched_chunks(const std::vector<Region>& regs) {
    std::vector<uint32_t> out;
    for (auto& g : regs)
        for (uint32_t off = 0; off < g.size; off += kChunk)
            if (chunk_touched(mem::ptr(g.base + off))) out.push_back(g.base + off);
    return out;
}

void append_record(std::vector<uint8_t>& out, ZSTD_CCtx* cx, std::vector<uint8_t>& tmp, uint32_t addr, const uint8_t* src) {
    size_t n = cx ? ZSTD_compressCCtx(cx, tmp.data(), tmp.size(), src, kChunk, kZstdLevel) : 0;
    const bool stored = !cx || ZSTD_isError(n) || n >= kChunk;
    uint32_t rec[2] = {addr, stored ? 0u : (uint32_t)n};
    out.insert(out.end(), (const uint8_t*)rec, (const uint8_t*)rec + 8);
    if (stored) out.insert(out.end(), src, src + kChunk);
    else out.insert(out.end(), tmp.data(), tmp.data() + n);
}

struct Ahead {
    std::mutex mu;
    // 0 none, 1 being written, 2 ready, 3 an autosave being put together from it
    std::atomic<int> state{0};
    std::vector<Region> regs;
    std::unordered_map<uint32_t, uint64_t> hashes;  // address -> hash of what its record holds
    size_t records = 0, bytes = 0;
    std::chrono::steady_clock::time_point ready;
    std::chrono::steady_clock::time_point failed;  // when the last one failed (ahead_start waits)
} g_ahead;

void ahead_write() {
    platform::set_thread_name("wwhd-autosave");
    setpriority(PRIO_PROCESS, 0, 10);  // this thread (Linux): behind the game's threads
    const auto t0 = std::chrono::steady_clock::now();
    std::string path = slot_path(kAutoSlot, "pre");
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        make_dirs(state_dir());
        f = fopen(path.c_str(), "wb");
    }
    std::vector<Region> regs = regions();
    std::vector<uint32_t> addrs = touched_chunks(regs);
    std::mutex fm, hm;
    std::unordered_map<uint32_t, uint64_t> hashes;
    hashes.reserve(addrs.size());
    std::atomic<size_t> next{0}, records{0}, bytes{0};
    std::atomic<bool> good{f != nullptr};
    auto work = [&] {
        setpriority(PRIO_PROCESS, 0, 10);
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        std::vector<uint8_t> tmp(ZSTD_compressBound(kChunk)), copy(kChunk), out;
        std::vector<std::pair<uint32_t, uint64_t>> mine;
        out.reserve((4 << 20) + kChunk + 16);
        auto flush = [&] {
            if (out.empty()) return;
            std::lock_guard<std::mutex> lk(fm);
            if (good && fwrite(out.data(), 1, out.size(), f) != out.size()) good = false;
            bytes += out.size();
            out.clear();
        };
        for (size_t i; good && (i = next.fetch_add(1, std::memory_order_relaxed)) < addrs.size();) {
            memcpy(copy.data(), mem::ptr(addrs[i]), kChunk);  // the game runs: what is compressed is what is hashed
            if (chunk_zero(copy.data())) continue;
            mine.push_back({addrs[i], chunk_hash(copy.data())});
            append_record(out, cx, tmp, addrs[i], copy.data());
            records++;
            if (out.size() >= (4u << 20)) flush();
        }
        flush();
        ZSTD_freeCCtx(cx);
        std::lock_guard<std::mutex> lk(hm);
        for (auto& [a, h] : mine) hashes[a] = h;
    };
    std::vector<std::thread> pool;
    pool.emplace_back(any_core(work));
    work();
    for (auto& th : pool) th.join();
    if (f && fclose(f) != 0) good = false;
    std::lock_guard<std::mutex> lk(g_ahead.mu);
    if (!good) {
        unlink(path.c_str());
        g_ahead.failed = std::chrono::steady_clock::now();
        g_ahead.state = 0;
        LOG("[savestate] autosave capture ahead failed");
        return;
    }
    g_ahead.regs = regs;
    g_ahead.hashes.swap(hashes);
    g_ahead.records = records;
    g_ahead.bytes = bytes;
    g_ahead.ready = std::chrono::steady_clock::now();
    g_ahead.state = 2;
    LOG("[savestate] autosave captured ahead: %zu chunks as %.1f MB in %.0f ms", (size_t)records, bytes / 1048576.0,
        std::chrono::duration<double, std::milli>(g_ahead.ready - t0).count());
}

// `backoff` after one that failed (no space, the folder gone): no new one before it has passed,
// or a failing one would be started on every frame
void ahead_start(std::chrono::seconds backoff) {
    {
        std::lock_guard<std::mutex> lk(g_ahead.mu);
        if (g_ahead.failed != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - g_ahead.failed < backoff) return;
    }
    int idle = 0;
    if (!g_ahead.state.compare_exchange_strong(idle, 1)) return;
    std::thread(ahead_write).detach();
}

// a capture ahead that is ready but of no use (the area changed, most of the memory with it): forgotten
// along with its file, made again for the next autosave
void ahead_discard() {
    std::lock_guard<std::mutex> lk(g_ahead.mu);
    int ready = 2;
    if (!g_ahead.state.compare_exchange_strong(ready, 0)) return;
    g_ahead.hashes.clear();
    unlink(slot_path(kAutoSlot, "pre").c_str());
}

// with the game frozen: the chunks that differ from what slotN.pre holds, compressed into `out`
bool ahead_delta(std::vector<uint8_t>& out, size_t& records) {
    if (g_ahead.state.load() != 2 || regions().size() != g_ahead.regs.size()) return false;
    std::vector<Region> regs = regions();
    for (size_t i = 0; i < regs.size(); i++)
        if (regs[i].base != g_ahead.regs[i].base || regs[i].size != g_ahead.regs[i].size) return false;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t> addrs = touched_chunks(regs);
    const auto t1 = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0}, count{0}, total{0};
    std::atomic<bool> tooMuch{false};
    std::mutex om;
    // held in memory until written: past 64 MB (most of it changed) the usual capture is used
    constexpr size_t kMaxDelta = 64u << 20;
    auto work = [&] {
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        std::vector<uint8_t> tmp(ZSTD_compressBound(kChunk)), mine;
        for (size_t i; !tooMuch && (i = next.fetch_add(1, std::memory_order_relaxed)) < addrs.size();) {
            const uint8_t* p = mem::ptr(addrs[i]);
            auto it = g_ahead.hashes.find(addrs[i]);
            if (it == g_ahead.hashes.end() ? chunk_zero(p) : chunk_hash(p) == it->second) continue;
            const size_t before = mine.size();
            append_record(mine, cx, tmp, addrs[i], p);
            count++;
            if ((total += mine.size() - before) > kMaxDelta) tooMuch = true;
        }
        ZSTD_freeCCtx(cx);
        std::lock_guard<std::mutex> lk(om);
        out.insert(out.end(), mine.begin(), mine.end());
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(any_core(work));
    work();
    for (auto& th : pool) th.join();
    LOG("[savestate] changed chunks: %zu looked at in %.0f ms, compared in %.0f ms", addrs.size(),
        std::chrono::duration<double, std::milli>(t1 - t0).count(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
    if (tooMuch) {
        out.clear();
        out.shrink_to_fit();
        return false;
    }
    records = count;
    return true;
}

// after the thaw: the slot file from the header and sections, the records written ahead and `delta`
void ahead_finish(Header h, std::vector<uint8_t> sections, std::vector<uint8_t> delta, size_t deltaRecords, std::string label) {
    std::thread([h, sections = std::move(sections), delta = std::move(delta), deltaRecords, label]() mutable {
        platform::set_thread_name("wwhd-autosave");
        const auto t0 = std::chrono::steady_clock::now();
        std::string pre = slot_path(kAutoSlot, "pre"), tmp = slot_path(kAutoSlot, "tmp");
        bool ok = false;
        {
            std::lock_guard<std::mutex> io(g_io);
            h.blocks = (uint32_t)(g_ahead.records + deltaRecords);
            FILE* in = fopen(pre.c_str(), "rb");
            FILE* f = in ? fopen(tmp.c_str(), "wb") : nullptr;
            if (f) {
                uint64_t sn = sections.size();
                uint32_t nr = (uint32_t)g_ahead.regs.size();
                ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&sn, 8, 1, f) == 1 && fwrite(sections.data(), 1, sn, f) == sn &&
                     fwrite(&nr, 4, 1, f) == 1 && fwrite(g_ahead.regs.data(), sizeof(Region), nr, f) == nr;
                std::vector<uint8_t> buf(1 << 20);
                size_t copied = 0;
                for (size_t n; ok && (n = fread(buf.data(), 1, buf.size(), in)) > 0;) {
                    ok = fwrite(buf.data(), 1, n, f) == n;
                    copied += n;
                }
                ok = ok && copied == g_ahead.bytes && (delta.empty() || fwrite(delta.data(), 1, delta.size(), f) == delta.size());
                ok = fclose(f) == 0 && ok;
            }
            if (in) fclose(in);
            if (ok) ok = rename(tmp.c_str(), slot_path(kAutoSlot).c_str()) == 0;
            else unlink(tmp.c_str());
            unlink(pre.c_str());
        }
        LOG("[savestate] slot %d: %s from the capture ahead + %zu changed chunks (%.1f MB) in %.0f ms", kAutoSlot,
            ok ? "written" : "WRITE FAILED", deltaRecords, delta.size() / 1048576.0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_message = ok ? "Saved to slot " + std::to_string(kAutoSlot) + label : "Slot " + std::to_string(kAutoSlot) + ": write failed";
            g_message_time = std::chrono::steady_clock::now();
        }
        {
            std::lock_guard<std::mutex> lk(g_ahead.mu);
            g_ahead.hashes.clear();
            g_ahead.state = 0;
        }
    }).detach();
}

// ---------------------------------------------------------------- slot files
// version 2: header, u64 size + zstd sections, u32 count + regions, then per chunk u32 address,
// u32 compressed size (0: 64 KiB as they are) and its bytes (do_save, capture_to_file)
bool read_header(FILE* f, Header& h, std::string& why) {
    memset(&h, 0, sizeof h);
    if (fread(&h, kHeaderV1Size, 1, f) != 1 || memcmp(h.magic, kMagic, 8) != 0) { why = "not a save state"; return false; }
    if (h.version < 1 || h.version > kVersion || h.header_size < kHeaderV1Size || h.header_size > 4096) { why = "saved by another version"; return false; }
    // the fields added since (a newer header's unknown rest is skipped)
    size_t known = std::min<size_t>(h.header_size, sizeof h) - kHeaderV1Size;
    uint32_t headerSize = h.header_size;
    if (known && fread((uint8_t*)&h + kHeaderV1Size, known, 1, f) != 1) { why = "file is truncated"; return false; }
    if (headerSize > sizeof h && fseek(f, headerSize - sizeof h, SEEK_CUR) != 0) { why = "file is truncated"; return false; }
    if (h.cpu_size != sizeof(Cpu)) { why = "saved by an incompatible build"; return false; }
    if (h.game_id != game_id()) { why = "saved with a different game executable"; return false; }
    // (the payload holds the sections, version 1 the raw memory as well: ~1.2 GB at the very most)
    if (h.raw_size > (2ull << 30)) { why = "file is corrupt"; return false; }
    return true;
}

// the memory regions of a slot file as regions() writes them: chunk-aligned, within the 4 GiB
// guest address space, none overlapping (a chunk is only restored into one of them: a damaged
// file can't make restore_memory write anywhere else)
bool regions_valid(const std::vector<Region>& regs) {
    for (size_t i = 0; i < regs.size(); i++) {
        const Region& g = regs[i];
        if (!g.size || (g.base & (kChunk - 1)) || (g.size & (kChunk - 1)) || (uint64_t)g.base + g.size > (1ull << 32)) return false;
        for (size_t k = 0; k < i; k++)
            if (g.base < (uint64_t)regs[k].base + regs[k].size && regs[k].base < (uint64_t)g.base + g.size) return false;
    }
    return true;
}
bool in_regions(const std::vector<Region>& regs, uint32_t chunk) {
    if (chunk & (kChunk - 1)) return false;
    for (auto& g : regs)
        if (chunk - g.base < g.size) return true;
    return false;
}

// the rest of a version 2 file (after the header): sections unpacked, memory chunks kept compressed
// (each one checked here, so restoring can't fail halfway)
bool read_slot_v2(FILE* f, Snapshot& s, std::string& why) {
    why = "file is truncated";
    struct stat st;
    long at = ftell(f);
    if (at < 0 || fstat(fileno(f), &st) != 0 || st.st_size < at) return false;
    const uint64_t end = (uint64_t)st.st_size;
    uint64_t sn = 0;
    // (every size is held against the file's before anything of that size is allocated)
    if (fread(&sn, 8, 1, f) != 1 || sn > end - at || s.h.raw_size > (256u << 20)) return false;
    std::vector<uint8_t> sc(sn);
    if (fread(sc.data(), 1, sn, f) != sn) return false;
    s.payload.resize(s.h.raw_size);
    if (ZSTD_decompress(s.payload.data(), s.payload.size(), sc.data(), sc.size()) != s.payload.size()) { why = "file is corrupt"; return false; }
    uint32_t nr = 0;
    if (fread(&nr, 4, 1, f) != 1 || nr > 64) return false;
    s.regs.resize(nr);
    if (fread(s.regs.data(), sizeof(Region), nr, f) != nr) return false;
    if (!regions_valid(s.regs)) { why = "file is corrupt"; return false; }
    at = ftell(f);
    if (at < 0 || (uint64_t)at > end) return false;
    void* m = end > 0 ? mmap(nullptr, (size_t)end, PROT_READ, MAP_PRIVATE, fileno(f), 0) : MAP_FAILED;
    if (m == MAP_FAILED) {  // read into memory as before
        s.blob.resize((size_t)(end - at));
        if (fread(s.blob.data(), 1, s.blob.size(), f) != s.blob.size()) return false;
    } else {
        s.map = m;
        s.mapSize = (size_t)end;
        madvise(m, (size_t)end, MADV_SEQUENTIAL);
    }
    const uint8_t* data = s.map ? (const uint8_t*)s.map + at : s.blob.data();
    const size_t dataSize = (size_t)(end - at);
    if (s.h.blocks > dataSize / 8) return false;  // a record is at least its two words
    Reader r(data, dataSize);
    std::vector<std::pair<uint32_t, ChunkRef>> list;
    list.reserve(s.h.blocks);
    for (uint32_t i = 0; i < s.h.blocks && r.ok; i++) {
        uint32_t addr = r.u32(), comp = r.u32();
        const uint8_t* p = r.p;
        if (!in_regions(s.regs, addr)) { why = "file is corrupt"; return false; }
        if (comp > kChunk || !r.bytes(nullptr, comp ? comp : kChunk)) return false;
        list.push_back({addr, ChunkRef{p, comp}});
    }
    // every chunk must unpack (into a scratch buffer; the game keeps running meanwhile)
    std::atomic<size_t> next{0};
    std::atomic<bool> bad{false};
    auto work = [&] {
        std::vector<uint8_t> tmp(kChunk);
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < list.size() && !bad;)
            if (!chunk_data(list[i].second, tmp.data())) bad = true;
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(any_core(work));
    work();
    for (auto& th : pool) th.join();
    if (bad || !r.ok || !s.parse_sections()) { why = "file is corrupt"; return false; }
    s.chunks.reserve(list.size());
    for (auto& [a, c] : list) s.chunks[a] = c;
    why.clear();
    return true;
}

// Version 1 file, read into the version 2 form: its blocks are unpacked one by one into a stream, the
// sections go to the payload, and each memory chunk is zstd-compressed as it comes (in batches on
// every core). The raw ~580 MB are never held at once (holding them got the game killed for low
// memory on a 6 GB phone after a few loads of an old state).
struct V1Stream {
    FILE* f;
    uint32_t blocks, next = 0;
    std::vector<uint8_t> raw, comp, scratch = std::vector<uint8_t>(scratch_size());
    size_t pos = 0;
    bool bad = false;
    bool fill() {
        if (next >= blocks) return false;
        uint32_t hdr[2];
        if (fread(hdr, 8, 1, f) != 1 || hdr[0] > kBlock) return bad = true, false;
        comp.resize(hdr[1] ? hdr[1] : hdr[0]);
        if (fread(comp.data(), 1, comp.size(), f) != comp.size()) return bad = true, false;
        raw.resize(hdr[0]);
        if (!hdr[1]) memcpy(raw.data(), comp.data(), hdr[0]);
        else if (decompress_block(raw.data(), hdr[0], comp.data(), hdr[1], scratch.data()) != hdr[0]) return bad = true, false;
        pos = 0;
        next++;
        return true;
    }
    bool read(void* out, size_t n) {
        uint8_t* o = (uint8_t*)out;
        while (n) {
            if (pos == raw.size() && !fill()) return false;
            size_t k = std::min(n, raw.size() - pos);
            if (o) memcpy(o, raw.data() + pos, k), o += k;
            pos += k;
            n -= k;
        }
        return true;
    }
    template <class T> bool get(T& v) { return read(&v, sizeof v); }
};

bool read_slot_v1(FILE* f, Snapshot& s, std::string& why) {
    why = "file is corrupt";
    V1Stream in{f, s.h.blocks};
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr size_t kBatch = 128;  // chunks per round (8 MiB)
    std::vector<uint8_t> batch(kBatch * kChunk);
    std::vector<uint32_t> addrs;
    struct Ref { uint32_t addr; size_t off; uint32_t comp; };
    std::vector<Ref> refs;
    std::vector<std::vector<uint8_t>> outs(nt);
    // compresses the batch's chunks on every core and appends them to the blob
    auto flush = [&](size_t n) {
        std::vector<std::vector<uint8_t>> res(n);
        std::atomic<size_t> next{0};
        auto work = [&] {
            ZSTD_CCtx* cx = ZSTD_createCCtx();
            for (size_t i; (i = next.fetch_add(1)) < n;) {
                std::vector<uint8_t>& o = res[i];
                o.resize(ZSTD_compressBound(kChunk));
                size_t k = cx ? ZSTD_compressCCtx(cx, o.data(), o.size(), batch.data() + i * kChunk, kChunk, kZstdLevel) : 0;
                if (!cx || ZSTD_isError(k) || k >= kChunk) o.clear();  // stored as it is
                else o.resize(k);
            }
            ZSTD_freeCCtx(cx);
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < nt; t++) pool.emplace_back(any_core(work));
        work();
        for (auto& th : pool) th.join();
        for (size_t i = 0; i < n; i++) {
            const bool stored = res[i].empty();
            refs.push_back({addrs[i], s.blob.size(), stored ? 0u : (uint32_t)res[i].size()});
            if (stored) s.blob.insert(s.blob.end(), batch.data() + i * kChunk, batch.data() + (i + 1) * kChunk);
            else s.blob.insert(s.blob.end(), res[i].begin(), res[i].end());
        }
        addrs.clear();
    };
    s.blob.reserve((size_t)s.h.raw_size / 2);  // address space; grows if needed
    bool mem = false;
    while (!mem) {
        uint32_t tag;
        uint64_t n;
        if (!in.get(tag) || !in.get(n)) break;
        if (tag != kSecMemory) {  // a section: kept as it is
            size_t at = s.payload.size();
            s.payload.resize(at + 12 + n);
            memcpy(&s.payload[at], &tag, 4);
            memcpy(&s.payload[at + 4], &n, 8);
            if (!in.read(&s.payload[at + 12], n)) return false;
            continue;
        }
        mem = true;
        uint32_t nr = 0;
        if (!in.get(nr) || nr > 64) return false;
        for (uint32_t r = 0; r < nr; r++) {
            Region g{};
            uint32_t present = 0;
            if (!in.get(g.base) || !in.get(g.size) || !in.get(present)) return false;
            s.regs.push_back(g);
            if (!regions_valid(s.regs) || present > g.size / kChunk) return false;
            for (uint32_t k = 0; k < present; k++) {
                uint32_t idx;
                if (!in.get(idx) || idx >= g.size / kChunk || !in.read(batch.data() + addrs.size() * kChunk, kChunk)) return false;
                addrs.push_back(g.base + idx * kChunk);
                if (addrs.size() == kBatch) flush(kBatch);
            }
        }
        if (!addrs.empty()) flush(addrs.size());
    }
    if (in.bad || !mem || !s.parse_sections()) return false;
    s.blob.shrink_to_fit();
    s.chunks.reserve(refs.size());
    for (auto& r : refs) s.chunks[r.addr] = ChunkRef{s.blob.data() + r.off, r.comp};
    s.h.blocks = (uint32_t)refs.size();
    s.converted = true;
    why.clear();
    return true;
}

// a snapshot read from a version 1 file, written back as version 2 (the next loads need little memory)
bool write_snapshot(int slot, const Snapshot& s) {
    Header h = s.h;
    h.version = kVersion;
    h.header_size = sizeof(Header);
    h.blocks = (uint32_t)s.chunks.size();
    h.raw_size = s.payload.size();
    std::vector<uint8_t> sc(ZSTD_compressBound(s.payload.size()));
    size_t sn = ZSTD_compress(sc.data(), sc.size(), s.payload.data(), s.payload.size(), kZstdLevel);
    if (ZSTD_isError(sn)) return false;
    std::vector<uint32_t> addrs;
    addrs.reserve(s.chunks.size());
    for (auto& [a, c] : s.chunks) addrs.push_back(a);
    std::sort(addrs.begin(), addrs.end());
    std::string tmp = slot_path(slot, "tmp");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint64_t sn64 = sn;
    uint32_t nr = (uint32_t)s.regs.size();
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&sn64, 8, 1, f) == 1 && fwrite(sc.data(), 1, sn, f) == sn &&
              fwrite(&nr, 4, 1, f) == 1 && fwrite(s.regs.data(), sizeof(Region), nr, f) == nr;
    for (size_t i = 0; i < addrs.size() && ok; i++) {
        const ChunkRef& c = s.chunks.at(addrs[i]);
        uint32_t rec[2] = {addrs[i], c.comp};
        size_t n = c.comp ? c.comp : kChunk;
        ok = fwrite(rec, 8, 1, f) == 1 && fwrite(c.p, 1, n, f) == n;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), slot_path(slot).c_str()) == 0;
    else unlink(tmp.c_str());
    return ok;
}

std::shared_ptr<Snapshot> read_slot(int slot, std::string& why) {
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) { why = "empty"; return nullptr; }
    auto s = std::make_shared<Snapshot>();
    s->slot = slot;
    if (!read_header(f, s->h, why)) { fclose(f); return nullptr; }
    if (s->h.version >= 2) {
        bool ok = read_slot_v2(f, *s, why);
        fclose(f);
        return ok ? s : nullptr;
    }
    bool ok = read_slot_v1(f, *s, why);
    fclose(f);
    return ok ? s : nullptr;
}

// ---------------------------------------------------------------- save / load at the frame boundary
void put_section(Writer& w, uint32_t tag, const Writer& sec) {
    w.u32(tag);
    w.u64(sec.b.size());
    w.bytes(sec.b.data(), sec.b.size());
}

void save_allocs(Writer& w) {
    auto log = mem::runtime_alloc_log();
    w.u32(mem::runtime_top());
    w.u32((uint32_t)log.size());
    for (auto& a : log) w.pod(a);
}

bool check_allocs(Reader r, std::string& why) {
    uint32_t top = r.u32();
    (void)top;
    uint32_t n = r.u32();
    auto cur = mem::runtime_alloc_log();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        mem::AllocRec a = r.pod<mem::AllocRec>();
        if (i >= cur.size()) break;  // allocated later in the saved session: beyond our top
        const mem::AllocRec& b = cur[i];
        // (the allocating code address, a.tag, changes with every rebuild: only logged)
        if (a.tag != b.tag && i < 4 && g_attempts == 0) LOG("[savestate] runtime object #%u allocated by different code (rebuilt?)", i);
        if (a.addr != b.addr || a.size != b.size) {
            char buf[160];
            snprintf(buf, sizeof buf, "runtime objects are laid out differently (#%u: %08X+%X vs %08X+%X)", i, a.addr, a.size, b.addr,
                     b.size);
            why = buf;
            return false;
        }
    }
    return r.ok;
}

void save_dispatch(Writer& w) {
    auto v = dispatch::host_functions();
    w.u32((uint32_t)v.size());
    for (auto& [a, n] : v) {
        w.u32(a);
        w.str(n);
    }
}

bool check_dispatch(Reader r, std::string& why) {
    std::unordered_map<uint32_t, std::string> cur;
    for (auto& [a, n] : dispatch::host_functions()) cur[a] = n;
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t a = r.u32();
        std::string name = r.str();
        auto it = cur.find(a);
        if (it != cur.end() && it->second == name) continue;
        // saved by another session or version (other addresses): the same function there too
        if (!dispatch::alias_host(a, name)) {
            why = "host function " + name + " is not registered at the same address";
            return false;
        }
    }
    return r.ok;
}

std::string area_label(const char* stage) {
    static const std::map<std::string, std::string> names = {
        {"sea", "Great Sea"}, {"LinkRM", "Link's House"}, {"MajyuE", "Forsaken Fortress"}, {"M_NewD2", "Dragon Roost Cavern"},
        {"kindan", "Forbidden Woods"}, {"Siren", "Tower of the Gods"}, {"Asoko", "Tetra's Ship"},
    };
    auto it = names.find(stage);
    return it == names.end() ? std::string(stage) : it->second;
}

// true when done (saved or failed for good); false to retry on the next frame
bool do_save(int slot) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    if (!threads::quiesce(250, busy, 1, 30)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("Slot %d: not saved (game busy:%s)", slot, busy.c_str());
        return true;
    }
    const auto tq = std::chrono::steady_clock::now();
    gx2_ss_drain();
    const auto tg = std::chrono::steady_clock::now();
    Writer threads_w;
    if (!threads_ss_save(threads_w, why)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("Slot %d: not saved (%s)", slot, why.c_str());
        return true;
    }
    auto payload = std::make_shared<Writer>();  // the sections (memory: capture_to_file)
    put_section(*payload, kSecThreads, threads_w);
    const auto tt = std::chrono::steady_clock::now();
    Writer w;
    mem_ss_save(w);
    put_section(*payload, kSecHeaps, w);
    const auto th_ = std::chrono::steady_clock::now();
    w = Writer();
    fs_ss_save(w);
    put_section(*payload, kSecFiles, w);
    w = Writer();
    ax_ss_save(w);
    put_section(*payload, kSecAudio, w);
    const auto ta = std::chrono::steady_clock::now();
    w = Writer();
    gx2_ss_save(w);
    put_section(*payload, kSecGx2, w);
    const auto tx = std::chrono::steady_clock::now();
    w = Writer();
    save_allocs(w);
    put_section(*payload, kSecAllocs, w);
    w = Writer();
    save_dispatch(w);
    put_section(*payload, kSecDispatch, w);
    Header h{};
    memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    h.header_size = sizeof(Header);
    h.created = (uint64_t)time(nullptr);
    std::string stage = stage_name();
    snprintf(h.area, sizeof h.area, "%s", stage.c_str());
    build_uuid(h.build);
    h.game_id = game_id();
    h.cpu_size = sizeof(Cpu);
    h.controller = input::pro_controller() ? 2 : 1;
    h.raw_size = payload->b.size();
    // the autosave with its memory captured ahead: only what changed since, while frozen
    if (slot == kAutoSlot && g_ahead.state.load() == 2) {
        std::vector<uint8_t> delta;
        size_t records = 0;
        const auto ts = std::chrono::steady_clock::now();
        {
            auto d = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            LOG("[savestate] sections: drain %.0f, threads %.0f, heaps %.0f, files+audio %.0f, gx2 %.0f, rest %.0f ms (%zu bytes)", d(tq, tg),
                d(tg, tt), d(tt, th_), d(th_, ta), d(ta, tx), d(tx, ts), payload->b.size());
        }
        if (ahead_delta(delta, records)) {
            const auto td = std::chrono::steady_clock::now();
            g_ahead.state = 3;
            threads::thaw();
            std::vector<uint8_t> sc(ZSTD_compressBound(payload->b.size()));
            size_t sn = ZSTD_compress(sc.data(), sc.size(), payload->b.data(), payload->b.size(), kZstdLevel);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            auto d = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            LOG("[savestate] slot %d: %zu changed chunks captured as %.1f MB in %.1f ms (stage %s; quiesce %.0f, sections %.0f, chunks %.0f)",
                slot, records, delta.size() / 1048576.0, ms, stage.c_str(), d(t0, tq), d(tq, ts), d(ts, td));
            if (ZSTD_isError(sn)) {
                std::lock_guard<std::mutex> lk(g_ahead.mu);
                g_ahead.state = 2;
                message("Slot %d: not saved (sections)", slot);
                return true;
            }
            sc.resize(sn);
            // its picture, as for the other slots: the screen copy and the PNG are made off the game
            // thread (an Auto slot without one looked empty)
            gfx::request_tv_dump(slot_path(slot, "png"), 0);
            ahead_finish(h, std::move(sc), std::move(delta), records, stage.empty() ? "" : " (" + area_label(stage.c_str()) + ")");
            return true;
        }
        LOG("[savestate] the capture ahead doesn't fit (memory regions or most of it changed): a full capture");
        ahead_discard();
    }
    // header (its chunk count patched in at the end), sections, then the memory straight from the frozen game
    bool ok = false;
    StreamResult cap{};
    std::string tmp = slot_path(slot, "tmp");
    {
        std::lock_guard<std::mutex> io(g_io);
        std::vector<uint8_t> sc(ZSTD_compressBound(payload->b.size()));
        size_t sn = ZSTD_compress(sc.data(), sc.size(), payload->b.data(), payload->b.size(), kZstdLevel);
        FILE* f = ZSTD_isError(sn) ? nullptr : fopen(tmp.c_str(), "wb");
        if (!f && !ZSTD_isError(sn)) {  // the folder was deleted while the game ran: made again
            make_dirs(state_dir());
            f = fopen(tmp.c_str(), "wb");
        }
        if (f) {
            uint64_t sn64 = sn;
            ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&sn64, 8, 1, f) == 1 && fwrite(sc.data(), 1, sn, f) == sn;
            std::mutex fm;
            if (ok) cap = capture_to_file(f, fm);
            ok = ok && cap.ok;
            h.blocks = (uint32_t)cap.chunks;
            ok = ok && fseek(f, offsetof(Header, blocks), SEEK_SET) == 0 && fwrite(&h.blocks, 4, 1, f) == 1;
            ok = fclose(f) == 0 && ok;
        }
    }
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG("[savestate] slot %d: captured %.1f MB of memory as %.1f MB in %.1f ms (stage %s)", slot,
        cap.chunks * (double)kChunk / 1048576.0, cap.bytes / 1048576.0, ms, stage.c_str());
    {
        std::lock_guard<std::mutex> io(g_io);
        if (ok) ok = rename(tmp.c_str(), slot_path(slot).c_str()) == 0;
        else unlink(tmp.c_str());
    }
    LOG("[savestate] slot %d: %s", slot, ok ? "written" : "WRITE FAILED");
    if (ok) gfx::request_tv_dump(slot_path(slot, "png"), 0);  // the picture is copied and written off the game thread
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_message = ok ? "Saved to slot " + std::to_string(slot) + (stage.empty() ? "" : " (" + area_label(stage.c_str()) + ")")
                       : "Slot " + std::to_string(slot) + ": write failed";
        g_message_time = std::chrono::steady_clock::now();
    }
    return true;
}

// a load the title or the file select refused: into the game first (g_entering); false if a file is
// already being played (nowhere else to go) or this is that second try
bool start_entering(int slot) {
    if (g_entering || mods::save_loaded(mods::save_addr())) return false;
    g_entering = true;
    g_entering_since = std::chrono::steady_clock::now();
    g_play_frames = 0;
    g_attempts = 0;
    LOG("[savestate] slot %d: not loadable outside a played file: going into the game first", slot);
    message("Loading slot %d: going into the game first...", slot);
    return true;
}

bool do_load(const std::shared_ptr<Snapshot>& s) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    threads_ss_targets(s->section(kSecThreads));
    // (threads waiting somewhere else for a while, a file loading or an event: up to 10 s while a file is
    // played, a frame each; a quiesce that times out costs up to 250 ms, so those keep the 30 tries)
    const int tries = mods::save_loaded(mods::save_addr()) ? 300 : 30;
    if (!threads::quiesce(250, busy, 2, 0)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        if (start_entering(s->slot)) return false;
        message("Slot %d: not loaded (game busy:%s)", s->slot, busy.c_str());
        return true;
    }
    gx2_ss_drain();
    g_check = s.get();
    g_check_serial++;
    bool ok = check_allocs(s->section(kSecAllocs), why) && check_dispatch(s->section(kSecDispatch), why) &&
              mem_ss_check(s->section(kSecHeaps), why) && ax_ss_check(s->section(kSecAudio), why) &&
              gx2_ss_check(s->section(kSecGx2), why);
    bool layout_ok = ok;
    ok = ok && threads_ss_check(s->section(kSecThreads), why);
    g_check = nullptr;
    if (!ok) {
        threads::thaw();
        // threads at other waits right now may be where they were saved on a later frame
        if (layout_ok && ++g_attempts < tries) {
            if (g_attempts == 1) LOG("[savestate] slot %d: waiting for the game threads (%s)", s->slot, why.c_str());
            return false;
        }
        if (start_entering(s->slot)) return false;
        message("Slot %d: cannot load here (%s)", s->slot, why.c_str());
        return true;
    }
    const int damaged = restore_memory(*s);
    Reader r = s->section(kSecAllocs);
    mem::raise_runtime_top(r.u32());
    r = s->section(kSecHeaps);
    mem_ss_load(r);
    r = s->section(kSecFiles);
    fs_ss_load(r);
    r = s->section(kSecAudio);
    ax_ss_load(r);
    r = s->section(kSecGx2);
    gx2_ss_load(r);
    interp::ss_reset();
    mods::cheats_ss_reset();
    mods::climb_ss_reset();
    mods::turbo_ss_reset();
    r = s->section(kSecThreads);
    threads_ss_load(r);  // last: wakes the parked threads (they continue after the thaw)
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string area = s->h.area[0] ? " (" + area_label(s->h.area) + ")" : "";
    // the controls the state was saved with: the game expects input from that controller
    bool pro = s->h.controller == 2;
    const bool switched = s->h.controller && pro != input::pro_controller();
    if (switched) input::set_pro_controller(pro);
    if (damaged) {
        message("Slot %d%s: loaded with %d damaged memory chunks", s->slot, area.c_str(), damaged);
    } else if (switched) {
        message("Loaded slot %d%s; controls act as %s again", s->slot, area.c_str(), pro ? "Wii U Pro Controller" : "Wii U GamePad");
    } else {
        message("Loaded slot %d%s", s->slot, area.c_str());
    }
    g_autosave_restart = true;  // not straight over the Auto slot just loaded
    memw::mark_all();  // guest memory replaced: the renderer copies everything again
    LOG("[savestate] slot %d: restored in %.1f ms", s->slot, ms);
    return true;
}

// test aid: WWHD_STATE_SAVE_AT=frame:slot,...  WWHD_STATE_LOAD_AT=frame:slot,... (TV frames)
struct Timed { uint64_t frame; int slot; };
std::vector<Timed> parse_timed(const char* var) {
    std::vector<Timed> v;
    const char* e = getenv(var);
    unsigned long long f;
    int s, n;
    while (e && sscanf(e, "%llu:%d%n", &f, &s, &n) == 2) {
        v.push_back({f, s});
        e += n;
        if (*e != ',') break;
        e++;
    }
    return v;
}

}  // namespace

uint32_t snap_ld32(uint32_t ea) {
    if (!g_check) return ld32(ea);
    uint32_t base = ea & ~(kChunk - 1);
    auto it = g_check->chunks.find(base);
    if (it == g_check->chunks.end()) {
        for (auto& g : g_check->regs)
            if (ea - g.base < g.size) return 0;
        return ld32(ea);  // not part of the snapshot (code, host-only memory)
    }
    // the chunk unpacked once while it's read (the checks read a few words from each of a few chunks)
    static uint64_t cachedSerial = 0;
    static uint32_t cachedBase = 0;
    static std::vector<uint8_t> buf(kChunk);
    if (cachedSerial != g_check_serial || cachedBase != base) {
        if (!chunk_data(it->second, buf.data())) memset(buf.data(), 0, kChunk);
        cachedSerial = g_check_serial;
        cachedBase = base;
    }
    uint32_t v;
    memcpy(&v, buf.data() + (ea - base), 4);
    return __builtin_bswap32(v);
}

SlotInfo slot_info(int slot) {
    SlotInfo info;
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) return info;
    Header h;
    std::string why;
    info.used = true;
    info.compatible = read_header(f, h, why);
    fclose(f);
    if (info.compatible || memcmp(h.magic, kMagic, 8) == 0) {
        time_t t = (time_t)h.created;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char buf[64];
        strftime(buf, sizeof buf, "%b %d %H:%M:%S", &tmv);
        info.when = buf;
        h.area[sizeof h.area - 1] = 0;
        if (h.area[0]) info.area = area_label(h.area);
        info.controller = (int)h.controller;
    }
    return info;
}

void request_save(int slot) {
    if (slot != kAutoSlot && (slot < 1 || slot > kSlots)) return;
    g_save_req = slot;
}

void request_load(int slot) {
    if (slot != kAutoSlot && (slot < 1 || slot > kSlots)) return;
    if (g_loading.exchange(true)) return;
    std::thread([slot] {
        auto t0 = std::chrono::steady_clock::now();
        std::string why;
        std::shared_ptr<Snapshot> s;
        {
            std::lock_guard<std::mutex> io(g_io);
            s = read_slot(slot, why);
        }
        if (!s) {
            message("Slot %d: %s", slot, why.c_str());
            g_loading = false;
            return;
        }
        if (s->converted) {  // an old state: from now on in the new form
            std::lock_guard<std::mutex> io(g_io);
            bool ok = write_snapshot(slot, *s);
            LOG("[savestate] slot %d: converted to version %u %s", slot, kVersion, ok ? "and written" : "but NOT written");
        }
        LOG("[savestate] slot %d: read %.1f MB (%zu memory chunks, version %u) in %.0f ms", slot,
            (s->payload.size() + s->blob.size() + s->mapSize) / 1048576.0, s->chunks.size(), s->h.version,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        std::lock_guard<std::mutex> lk(g_mu);
        g_load_ready = s;
    }).detach();
}

std::string last_message() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_message.empty() || std::chrono::steady_clock::now() - g_message_time > std::chrono::seconds(4)) return "";
    return g_message;
}

// ---- autosave ----
void set_autosave_seconds(int secs) { g_autosave_secs = secs < 0 ? 0 : secs; }
int autosave_seconds() { return g_autosave_secs.load(); }
void autosave_now() { if (g_autosave_secs.load() > 0) g_autosave_now = true; }
bool autosave_busy() { return g_autosave_now.load() || g_save_req.load() == kAutoSlot || g_ahead.state.load() == 3; }

void service(Cpu* c) {
    (void)c;
    static const std::vector<Timed> save_at = parse_timed("WWHD_STATE_SAVE_AT"), load_at = parse_timed("WWHD_STATE_LOAD_AT");
    if (!save_at.empty() || !load_at.empty()) {
        static uint64_t last = 0;
        uint64_t f = gfx::frame_count();
        for (auto& t : save_at)
            if (t.frame > last && t.frame <= f) request_save(t.slot);
        for (auto& t : load_at)
            if (t.frame > last && t.frame <= f) request_load(t.slot);
        last = f;
    }
    // autosave: snapshot into kAutoSlot every g_autosave_secs, only while a file is actually being
    // played (not the title / file select) and nothing else is saving or loading. A snapshot holds
    // the game for ~0.5 s: once due it waits for a moment where that isn't felt (a game menu open,
    // a dialogue, or 2-6 s after entering another area or room), at most another interval.
    // autosave_now (the app going to the background, where Android may end it): at once, unless
    // the last one is less than 30 s old.
    {
        using clk = std::chrono::steady_clock;
        static clk::time_point last, areaSince;
        static std::string area;
        static bool saved = false;  // an autosave written in this session
        const int secs = g_autosave_secs.load();
        const auto now = clk::now();
        const bool now_asked = g_autosave_now.exchange(false);
        if (secs <= 0) {
            last = clk::time_point{};
        } else {
            if (last == clk::time_point{} || g_autosave_restart.exchange(false)) last = now;
            const uint32_t addr = mods::save_addr();
            const bool playing = addr >= 0x10000000u && mods::save_loaded(addr);
            std::string here = stage_name() + ":" + std::to_string((int)(int8_t)ld8(kStageInfo + kStageNameOff + 10));
            if (here != area) {
                area = here;
                areaSince = now;
                ahead_discard();  // another area: most of the memory changes, the capture ahead is made again
            }
            // the memory captured ahead some 20 s before the autosave is due (WWHD_AUTOSAVE_AHEAD=0: off)
            static const bool aheadOn = !getenv("WWHD_AUTOSAVE_AHEAD") || atoi(getenv("WWHD_AUTOSAVE_AHEAD")) != 0;
            if (aheadOn && playing && !g_loading.load() && now - last >= std::chrono::seconds(std::max(0, secs - std::min(20, secs / 2))))
                ahead_start(std::chrono::seconds(secs));
            if (now_asked && playing && !g_save_req.load() && !g_loading.load() &&
                (!saved || now - last >= std::chrono::seconds(30))) {
                last = now;
                saved = true;
                request_save(kAutoSlot);
            } else if (playing && !g_save_req.load() && !g_loading.load() && now - last >= std::chrono::seconds(secs)) {
                const auto inArea = now - areaSince;
                const bool calm = interp::menu_open() || mods::event_mode_now() == 1 ||
                                  (inArea >= std::chrono::seconds(2) && inArea < std::chrono::seconds(6));
                // and the capture ahead done (unless it is late)
                const bool aheadDone = !aheadOn || g_ahead.state.load() != 1 || now - last >= std::chrono::seconds(secs + 30);
                if ((calm && aheadDone) || now - last >= std::chrono::seconds(2 * secs)) {
                    last = now;
                    saved = true;
                    request_save(kAutoSlot);
                }
            }
        }
    }
    // test aid: WWHD_STATE_DUMP=n dumps the n TV frames after each save/load (state_<save|load><slot>_<k>.png)
    static const int dump = getenv("WWHD_STATE_DUMP") ? atoi(getenv("WWHD_STATE_DUMP")) : 0;
    if (int slot = g_save_req.load()) {
        if (do_save(slot)) {
            g_save_req = 0;
            g_attempts = 0;
            for (int k = 1; k <= dump; k++)
                gfx::request_tv_dump("state_save" + std::to_string(slot) + "_" + std::to_string(k) + ".png", k);
        }
        return;
    }
    std::shared_ptr<Snapshot> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_load_ready;
    }
    if (s && g_entering) {
        const bool inFile = mods::save_loaded(mods::save_addr());
        g_play_frames = inFile && !interp::menu_open() ? g_play_frames + 1 : 0;
        const bool late = std::chrono::steady_clock::now() - g_entering_since > std::chrono::seconds(120);
        if (late) {
            input::set_auto_buttons(0);
            g_entering = false;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_load_ready.reset();
            }
            g_loading = false;
            g_attempts = 0;
            message("Slot %d: not loaded (the game could not be entered: start a file, then load it)", s->slot);
            return;
        }
        if (!inFile) {  // A in pulses (pressed 4 frames of every 12): title, controller choice, file select
            input::set_auto_buttons(gfx::frame_count() % 12 < 4 ? input::kA : 0);
            return;
        }
        input::set_auto_buttons(0);
        if (g_play_frames < 45) return;  // the place drawn and its threads settled
    }
    if (s && do_load(s)) {
        if (g_entering) {
            g_entering = false;
            input::set_auto_buttons(0);
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_load_ready.reset();
        }
        g_loading = false;
        g_attempts = 0;
        crash_info::set("save state", "slot " + std::to_string(s->slot) + " loaded at game frame " + std::to_string(gfx::frame_count()) +
                                          " (" + std::string(s->h.area) + ", saved " + std::to_string(s->h.created) + ")");
        // a slot without its picture (an autosave of an older version, or one written as Android closed
        // the app): the picture is taken once the game has drawn the loaded place again
        struct stat st;
        if (stat(slot_path(s->slot, "png").c_str(), &st) != 0) gfx::request_tv_dump(slot_path(s->slot, "png"), 15);
        for (int k = 1; k <= dump; k++)
            gfx::request_tv_dump("state_load" + std::to_string(s->slot) + "_" + std::to_string(k) + ".png", k);
    }
}

}  // namespace ss
