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
#include <sys/mman.h>
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
#include "input.h"
#include "mem_writes.h"
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
    std::map<uint32_t, std::pair<size_t, size_t>> sections;  // tag -> (offset, size)
    std::unordered_map<uint32_t, ChunkRef> chunks;            // chunk address -> data (absent = zero)
    std::vector<Region> regs;
    int slot = 0;

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

std::mutex g_mu;  // messages, pending requests
std::mutex g_io;  // one slot file operation at a time
std::string g_message;
std::chrono::steady_clock::time_point g_message_time;
std::atomic<int> g_save_req{0};
std::shared_ptr<Snapshot> g_load_ready;   // decompressed, waiting for the frame boundary
std::atomic<bool> g_loading{false};       // a slot file is being read
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

std::string state_dir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = getenv("WWHD_STATE_DIR")) d = e;
#ifdef __APPLE__
        else d = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Library/Application Support/wwhd/states";
#else
        else d = config::save_dir.substr(0, config::save_dir.find_last_of('/')) + "/states";  // next to save/
#endif
        for (size_t i = 1; i <= d.size(); i++)
            if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
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

// mincore() takes char* on macOS, unsigned char* on Linux
int page_residency(void* p, size_t n, char* vec) {
#ifdef __APPLE__
    return mincore(p, n, vec);
#else
    return mincore(p, n, (unsigned char*)vec);
#endif
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
// guest memory, captured while the game is frozen: the chunks that aren't all zero, compressed on
// every core straight from guest memory
struct Captured {
    struct Piece {
        uint32_t thread, offset, comp;  // comp 0: stored as it is
    };
    std::vector<Region> regs;
    std::vector<uint32_t> addrs;  // chunk addresses, in order
    std::vector<Piece> pieces;    // by chunk
    std::vector<std::vector<uint8_t>> out;  // by compressing thread
    size_t bytes() const {
        size_t n = 0;
        for (auto& o : out) n += o.size();
        return n;
    }
};

std::shared_ptr<Captured> capture_memory() {
    auto c = std::make_shared<Captured>();
    c->regs = regions();
    std::vector<char> vec(kChunk / getpagesize() + 1);
    for (auto& g : c->regs) {
        for (uint32_t off = 0; off < g.size; off += kChunk) {
            uint8_t* p = mem::ptr(g.base + off);
            // untouched pages are zero: skip them without reading (reading would commit them)
            if (page_residency(p, kChunk, vec.data()) == 0) {
                bool touched = false;
                for (size_t i = 0; i < kChunk / (size_t)getpagesize(); i++) touched |= vec[i] != 0;
                if (!touched) continue;
            }
            const uint64_t* q = (const uint64_t*)p;
            bool zero = true;
            for (size_t i = 0; i < kChunk / 8 && zero; i++) zero = q[i] == 0;
            if (!zero) c->addrs.push_back(g.base + off);
        }
    }
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    c->out.resize(nt);
    c->pieces.resize(c->addrs.size());
    std::atomic<size_t> next{0};
    auto work = [&](unsigned t) {
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        std::vector<uint8_t> tmp(ZSTD_compressBound(kChunk));
        std::vector<uint8_t>& o = c->out[t];
        o.reserve((c->addrs.size() / nt + 16) * (kChunk / 2));  // address space; grows if needed
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < c->addrs.size();) {
            const uint8_t* src = mem::ptr(c->addrs[i]);
            size_t n = cx ? ZSTD_compressCCtx(cx, tmp.data(), tmp.size(), src, kChunk, kZstdLevel) : 0;
            Captured::Piece& pc = c->pieces[i];
            pc.thread = t;
            pc.offset = (uint32_t)o.size();
            if (!cx || ZSTD_isError(n) || n >= kChunk) {
                o.insert(o.end(), src, src + kChunk);
                pc.comp = 0;
            } else {
                o.insert(o.end(), tmp.data(), tmp.data() + n);
                pc.comp = (uint32_t)n;
            }
        }
        ZSTD_freeCCtx(cx);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();
    return c;
}

void restore_memory(const Snapshot& s) {
    // the saved chunks, unpacked on every core straight into guest memory (read_slot checked them)
    std::vector<std::pair<uint32_t, ChunkRef>> list(s.chunks.begin(), s.chunks.end());
    std::atomic<size_t> next{0};
    std::atomic<int> bad{0};
    auto work = [&] {
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < list.size();)
            if (!chunk_data(list[i].second, mem::ptr(list[i].first))) bad++;
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (bad) LOG("[savestate] %d memory chunks could not be unpacked", bad.load());
    std::vector<char> vec(kChunk / getpagesize() + 1);
    for (auto& g : s.regs) {
        for (uint32_t off = 0; off < g.size; off += kChunk) {
            uint32_t a = g.base + off;
            uint8_t* p = mem::ptr(a);
            if (s.chunks.count(a)) continue;
            if (page_residency(p, kChunk, vec.data()) == 0) {
                bool touched = false;
                for (size_t i = 0; i < kChunk / (size_t)getpagesize(); i++) touched |= vec[i] != 0;
                if (!touched) continue;
            }
            const uint64_t* q = (const uint64_t*)p;
            for (size_t i = 0; i < kChunk / 8; i++)
                if (q[i]) { memset(p, 0, kChunk); break; }
        }
    }
}

// ---------------------------------------------------------------- slot files
// version 2: header, u64 size + zstd sections, u32 count + regions, then per chunk u32 address,
// u32 compressed size (0: 64 KiB as they are) and its bytes
bool write_slot(int slot, const Header& h0, const std::vector<uint8_t>& sections, const Captured& c) {
    Header h = h0;
    h.blocks = (uint32_t)c.addrs.size();
    h.raw_size = sections.size();
    std::vector<uint8_t> sc(ZSTD_compressBound(sections.size()));
    size_t sn = ZSTD_compress(sc.data(), sc.size(), sections.data(), sections.size(), kZstdLevel);
    if (ZSTD_isError(sn)) return false;
    std::string tmp = slot_path(slot, "tmp");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint64_t sn64 = sn;
    uint32_t nr = (uint32_t)c.regs.size();
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&sn64, 8, 1, f) == 1 && fwrite(sc.data(), 1, sn, f) == sn &&
              fwrite(&nr, 4, 1, f) == 1 && fwrite(c.regs.data(), sizeof(Region), nr, f) == nr;
    for (size_t i = 0; i < c.addrs.size() && ok; i++) {
        const Captured::Piece& pc = c.pieces[i];
        uint32_t rec[2] = {c.addrs[i], pc.comp};
        size_t n = pc.comp ? pc.comp : kChunk;
        ok = fwrite(rec, 8, 1, f) == 1 && fwrite(c.out[pc.thread].data() + pc.offset, 1, n, f) == n;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), slot_path(slot).c_str()) == 0;
    else unlink(tmp.c_str());
    return ok;
}

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
    return true;
}

// the rest of a version 2 file (after the header): sections unpacked, memory chunks kept compressed
// (each one checked here, so restoring can't fail halfway)
bool read_slot_v2(FILE* f, Snapshot& s, std::string& why) {
    why = "file is truncated";
    uint64_t sn = 0;
    if (fread(&sn, 8, 1, f) != 1 || sn > (1u << 30) || s.h.raw_size > (1u << 30)) return false;
    std::vector<uint8_t> sc(sn);
    if (fread(sc.data(), 1, sn, f) != sn) return false;
    s.payload.resize(s.h.raw_size);
    if (ZSTD_decompress(s.payload.data(), s.payload.size(), sc.data(), sc.size()) != s.payload.size()) { why = "file is corrupt"; return false; }
    uint32_t nr = 0;
    if (fread(&nr, 4, 1, f) != 1 || nr > 64) return false;
    s.regs.resize(nr);
    if (fread(s.regs.data(), sizeof(Region), nr, f) != nr) return false;
    long at = ftell(f);
    if (at < 0 || fseek(f, 0, SEEK_END) != 0) return false;
    long end = ftell(f);
    if (end < at || fseek(f, at, SEEK_SET) != 0) return false;
    s.blob.resize((size_t)(end - at));
    if (fread(s.blob.data(), 1, s.blob.size(), f) != s.blob.size()) return false;
    Reader r(s.blob.data(), s.blob.size());
    std::vector<std::pair<uint32_t, ChunkRef>> list;
    list.reserve(s.h.blocks);
    for (uint32_t i = 0; i < s.h.blocks && r.ok; i++) {
        uint32_t addr = r.u32(), comp = r.u32();
        const uint8_t* p = r.p;
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
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (bad || !r.ok || !s.parse_sections()) { why = "file is corrupt"; return false; }
    s.chunks.reserve(list.size());
    for (auto& [a, c] : list) s.chunks[a] = c;
    why.clear();
    return true;
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
    s->payload.resize(s->h.raw_size);
    // a batch of blocks per round (one for each thread): the compressed file isn't held as a whole
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::vector<uint8_t>> blocks(nt), scratch(nt);
    std::vector<uint32_t> raws(nt), comps(nt);
    for (unsigned t = 0; t < nt; t++) scratch[t].resize(scratch_size());
    bool ok = true;
    std::atomic<bool> bad{false};
    for (uint32_t first = 0; first < s->h.blocks && ok && !bad; first += nt) {
        const unsigned batch = std::min<uint32_t>(nt, s->h.blocks - first);
        for (unsigned t = 0; t < batch && ok; t++) {
            uint32_t hdr[2];
            ok = fread(hdr, 8, 1, f) == 1 && hdr[0] <= kBlock;
            if (!ok) break;
            raws[t] = hdr[0];
            comps[t] = hdr[1];
            blocks[t].resize(hdr[1] ? hdr[1] : hdr[0]);
            ok = fread(blocks[t].data(), 1, blocks[t].size(), f) == blocks[t].size();
        }
        if (!ok) break;
        auto work = [&](unsigned t) {
            const size_t i = first + t;
            if (i * kBlock + raws[t] > s->payload.size()) { bad = true; return; }
            uint8_t* dst = s->payload.data() + i * (size_t)kBlock;
            if (!comps[t]) { memcpy(dst, blocks[t].data(), raws[t]); return; }
            if (decompress_block(dst, raws[t], blocks[t].data(), comps[t], scratch[t].data()) != raws[t]) bad = true;
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < batch; t++) pool.emplace_back(work, t);
        work(0);
        for (auto& th : pool) th.join();
    }
    fclose(f);
    if (!ok) { why = "file is truncated"; return nullptr; }
    if (bad || !s->parse()) { why = "file is corrupt"; return nullptr; }
    return s;
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
        if (a.tag != b.tag && i < 4) LOG("[savestate] runtime object #%u allocated by different code (rebuilt?)", i);
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
        if (it == cur.end() || it->second != name) {
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
    gx2_ss_drain();
    Writer threads_w;
    if (!threads_ss_save(threads_w, why)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("Slot %d: not saved (%s)", slot, why.c_str());
        return true;
    }
    auto payload = std::make_shared<Writer>();  // the sections (memory: capture_memory)
    put_section(*payload, kSecThreads, threads_w);
    Writer w;
    mem_ss_save(w);
    put_section(*payload, kSecHeaps, w);
    w = Writer();
    fs_ss_save(w);
    put_section(*payload, kSecFiles, w);
    w = Writer();
    ax_ss_save(w);
    put_section(*payload, kSecAudio, w);
    w = Writer();
    gx2_ss_save(w);
    put_section(*payload, kSecGx2, w);
    w = Writer();
    save_allocs(w);
    put_section(*payload, kSecAllocs, w);
    w = Writer();
    save_dispatch(w);
    put_section(*payload, kSecDispatch, w);
    auto memory = capture_memory();
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
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG("[savestate] slot %d: captured %.1f MB of memory as %.1f MB in %.1f ms (stage %s)", slot,
        memory->addrs.size() * (double)kChunk / 1048576.0, memory->bytes() / 1048576.0, ms, stage.c_str());
    gfx::request_tv_dump(slot_path(slot, "png"), 0);
    // compress and write in the background; the game continues
    std::thread([slot, h, payload, memory, stage] {
        auto t1 = std::chrono::steady_clock::now();
        bool ok;
        {
            std::lock_guard<std::mutex> io(g_io);
            ok = write_slot(slot, h, payload->b, *memory);
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        struct stat st{};
        stat(slot_path(slot).c_str(), &st);
        LOG("[savestate] slot %d: %s (%.1f MB on disk, %.0f ms)", slot, ok ? "written" : "WRITE FAILED", st.st_size / 1048576.0, ms);
        std::lock_guard<std::mutex> lk(g_mu);
        g_message = ok ? "Saved to slot " + std::to_string(slot) + (stage.empty() ? "" : " (" + area_label(stage.c_str()) + ")")
                       : "Slot " + std::to_string(slot) + ": write failed";
        g_message_time = std::chrono::steady_clock::now();
    }).detach();
    return true;
}

bool do_load(const std::shared_ptr<Snapshot>& s) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    threads_ss_targets(s->section(kSecThreads));
    if (!threads::quiesce(250, busy, 2, 0)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
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
        if (layout_ok && ++g_attempts < 30) {
            if (g_attempts == 1) LOG("[savestate] slot %d: waiting for the game threads (%s)", s->slot, why.c_str());
            return false;
        }
        message("Slot %d: cannot load here (%s)", s->slot, why.c_str());
        return true;
    }
    restore_memory(*s);
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
    r = s->section(kSecThreads);
    threads_ss_load(r);  // last: wakes the parked threads (they continue after the thaw)
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string area = s->h.area[0] ? " (" + area_label(s->h.area) + ")" : "";
    // the controls the state was saved with: the game expects input from that controller
    bool pro = s->h.controller == 2;
    if (s->h.controller && pro != input::pro_controller()) {
        input::set_pro_controller(pro);
        message("Loaded slot %d%s; controls act as %s again", s->slot, area.c_str(), pro ? "Wii U Pro Controller" : "Wii U GamePad");
    } else {
        message("Loaded slot %d%s", s->slot, area.c_str());
    }
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
    if (slot < 1 || slot > kSlots) return;
    g_save_req = slot;
}

void request_load(int slot) {
    if (slot < 1 || slot > kSlots) return;
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
        LOG("[savestate] slot %d: read %.1f MB (%zu memory chunks, version %u) in %.0f ms", slot,
            (s->payload.size() + s->blob.size()) / 1048576.0, s->chunks.size(), s->h.version,
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
    if (s && do_load(s)) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_load_ready.reset();
        }
        g_loading = false;
        g_attempts = 0;
        for (int k = 1; k <= dump; k++)
            gfx::request_tv_dump("state_load" + std::to_string(s->slot) + "_" + std::to_string(k) + ".png", k);
    }
}

}  // namespace ss
