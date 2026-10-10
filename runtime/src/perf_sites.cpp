// Busy waits of the game (tools/recomp/hooks_perf.txt; from ZeldaWWHDRecomp's Android work). On the Wii U each guest thread has a core of
// its own; here the spinning thread shares the host's few fast cores with the threads it waits
// for (phones: two fast cores for the game, render and worker threads). After a short spin the
// waiting thread yields, then sleeps in short steps, so the threads it waits for get the CPU and
// the spin does not burn power (phones slow down when they get hot).
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#define XXH_NAMESPACE WWHD_PERF_
#define XXH_INLINE_ALL
#include "../third_party/zstd/lib/common/xxhash.h"
#include <thread>

#include "runtime.h"
#include <sched.h>

namespace {
struct Spin {
    std::chrono::steady_clock::time_point last{};
    uint32_t count = 0;
};
thread_local Spin t_spin;

void cooperative_spin() {
    auto now = std::chrono::steady_clock::now();
    // a new wait when the previous iteration was long ago
    if (now - t_spin.last > std::chrono::microseconds(500)) t_spin.count = 0;
    uint32_t n = ++t_spin.count;
    if (n > 256) std::this_thread::sleep_for(std::chrono::microseconds(30));
    else if (n > 16) std::this_thread::yield();
    t_spin.last = std::chrono::steady_clock::now();
}
}  // namespace

// WorkerMgr join (f_02759564): loops until every worker of the job reports idle
extern "C" void site_027596DC(Cpu* c) {
    (void)c;
    cooperative_spin();
}

// WorkerMgr join (f_02759564), first loop: sums the job's done counters and compares them with its total
// until they match (the workers it waits for share the spinning thread's fast core: title-screen and
// gameplay hitches of 100-300 ms with the game thread running and the workers runnable but not running)
extern "C" void site_02759634(Cpu* c) {
    (void)c;
    static const bool on = !getenv("WWHD_SPIN_02759634") || atoi(getenv("WWHD_SPIN_02759634")) != 0;
    if (on) cooperative_spin();
}

// f_027F9994: waits for a flag (+0x388) set by another thread by unlocking and relocking its mutex in a
// loop (Wind Temple and other heavy places: the game thread ran 60-110 ms straight there, mostly mutex
// calls, while the thread that sets the flag shares its fast core). The site is between the unlock and
// the relock: it must never sleep while the mutex is held. WWHD_SPIN_027F99E0=0 turns it off.
extern "C" void site_027F99E0(Cpu* c) {
    (void)c;
    static const bool on = !getenv("WWHD_SPIN_027F99E0") || atoi(getenv("WWHD_SPIN_027F99E0")) != 0;
    if (on) cooperative_spin();
}

// Yaz0 decompression (f_0275EBC0: r3 = destination, r4 = compressed data with its "Yaz0" header; returns
// the decompressed size). The game decompresses room archives in a WorkerMgr job that the frame joins:
// walking into another part of an island stopped the game thread for ~200 ms (the recompiled loop keeps
// the PPC registers and flags in memory per byte). The same steps natively: two phases as the game's
// code (the first zeroes each next cache line ahead with dcbz while more than 306 bytes are left, the
// second stops at a back reference that would run past the end, without copying it).
// WWHD_YAZ0_NATIVE=0 runs the game's code; WWHD_YAZ0_CHECK=1 runs both and compares (logs mismatches).
extern "C" void f_0275EBC0_orig(Cpu* c);
namespace {
// the dcbz of the game's first phase only zeroes lines ahead that the decompression writes afterwards
// (more than 306 bytes are left there, and a back reference reads only bytes behind): the final bytes
// are the same without it, so both phases run as one loop with bulk copies (WWHD_YAZ0_CHECK compares)
uint32_t yaz0_native(uint32_t dst, uint32_t src, uint32_t* consumed = nullptr) {
    const uint32_t size = ld32(src + 4);
    uint8_t* const base = ppc_ptr(0);
    uint8_t* out = base + dst;
    uint8_t* const end = out + size;
    const uint8_t* in = base + src + 0x10;
    while (out < end) {
        uint32_t code = *in++;
        for (int bit = 0; bit < 8 && out < end; bit++, code <<= 1) {
            if (code & 0x80) { *out++ = *in++; continue; }
            const uint32_t b = in[0], b2 = in[1];
            in += 2;
            uint32_t n = b >> 4;
            if (!n) n = uint32_t(*in++) + 0x10;
            n += 2;
            const size_t dist = (((b & 0xF) << 8) | b2) + 1;
            // near the end (the game's second phase) a reference running past the end is not copied
            if (n > size_t(end - out)) {
                if (size_t(end - out) <= 306) return size;
                n = uint32_t(end - out);  // not reached: the first phase never overruns (n <= 273 < 306)
            }
            const uint8_t* from = out - dist;
            if (dist >= n) { std::memcpy(out, from, n); out += n; }
            else if (dist == 1) { std::memset(out, *from, n); out += n; }
            else { for (uint32_t i = 0; i < n; i++) out[i] = from[i]; out += n; }
        }
    }
    if (consumed) *consumed = uint32_t(in - (base + src));
    return size;
}
// Decompressed copies of large files (>= 1 MiB, at most 64 MiB, least recently used dropped): walking
// back and forth over an area boundary made the game decompress the same files (up to 19 MB, 45-77 ms
// each) again every time. Looked up by size and a hash of the first 4 KiB of compressed data, then
// checked with a hash over the whole compressed length found the first time. WWHD_YAZ0_CACHE=0: off.
struct Yaz0Cached { uint32_t size, compLen; uint64_t fullHash, lastUse; std::vector<uint8_t> data; };
std::mutex yaz0Mutex;
std::unordered_multimap<uint64_t, Yaz0Cached> yaz0Cache;
size_t yaz0Bytes = 0;
uint64_t yaz0Clock = 0;
constexpr uint32_t kYaz0CacheMin = 1u << 20;
constexpr size_t kYaz0CacheBudget = 64u << 20;
uint32_t yaz0_cached(uint32_t dst, uint32_t src) {
    static const bool on = !getenv("WWHD_YAZ0_CACHE") || strcmp(getenv("WWHD_YAZ0_CACHE"), "0") != 0;
    const uint32_t size = ld32(src + 4);
    if (!on || size < kYaz0CacheMin) return yaz0_native(dst, src);
    const uint8_t* comp = ppc_ptr(src);
    const uint64_t key = XXH64(comp, 4096, 0) ^ size;
    {
        std::lock_guard<std::mutex> lk(yaz0Mutex);
        auto range = yaz0Cache.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) {
            Yaz0Cached& e = it->second;
            if (e.size != size || XXH64(comp, e.compLen, 0) != e.fullHash) continue;
            std::memcpy(ppc_ptr(dst), e.data.data(), size);
            e.lastUse = ++yaz0Clock;
            return size;
        }
    }
    uint32_t compLen = 0;
    yaz0_native(dst, src, &compLen);
    Yaz0Cached e{size, compLen, XXH64(comp, compLen, 0), 0, std::vector<uint8_t>(ppc_ptr(dst), ppc_ptr(dst) + size)};
    std::lock_guard<std::mutex> lk(yaz0Mutex);
    e.lastUse = ++yaz0Clock;
    yaz0Bytes += size;
    yaz0Cache.emplace(key, std::move(e));
    while (yaz0Bytes > kYaz0CacheBudget) {
        auto oldest = yaz0Cache.begin();
        for (auto it = yaz0Cache.begin(); it != yaz0Cache.end(); ++it)
            if (it->second.lastUse < oldest->second.lastUse) oldest = it;
        yaz0Bytes -= oldest->second.size;
        yaz0Cache.erase(oldest);
    }
    return size;
}
}  // namespace
extern "C" void hook_0275EBC0(Cpu* c) {
    static const bool native = !getenv("WWHD_YAZ0_NATIVE") || strcmp(getenv("WWHD_YAZ0_NATIVE"), "0") != 0;
    static const bool check = getenv("WWHD_YAZ0_CHECK") && strcmp(getenv("WWHD_YAZ0_CHECK"), "0") != 0;
    if (!native) { f_0275EBC0_orig(c); return; }
    const uint32_t dst = c->r[3], src = c->r[4], size = ld32(src + 4);
    if (!check) {
        // WWHD_YAZ0_LOG=ms: log decompressions slower than that (size, time, host CPU)
        static const double logMs = getenv("WWHD_YAZ0_LOG") ? atof(getenv("WWHD_YAZ0_LOG")) : 0;
        const auto t0 = std::chrono::steady_clock::now();
        c->r[3] = yaz0_cached(dst, src);
        if (logMs > 0) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > logMs) LOG("[yaz0] %u bytes in %.1f ms (%.0f MB/s) on cpu %d", size, ms, size / 1e3 / ms, sched_getcpu());
        }
        return;
    }
    // check: the game's code first, keep its output, then native over it and compare (+64 bytes after)
    const uint32_t span = size + 64;
    f_0275EBC0_orig(c);
    const uint32_t ret = c->r[3];
    std::vector<uint8_t> want(ppc_ptr(dst), ppc_ptr(dst) + span);
    std::memset(ppc_ptr(dst), 0xCD, size);  // native must write every byte again
    std::memcpy(ppc_ptr(dst + size), want.data() + size, 64);
    const uint32_t got = yaz0_native(dst, src);
    size_t diff = 0;
    for (uint32_t i = 0; i < span; i++) diff += ppc_ptr(dst)[i] != want[i];
    LOG("[yaz0 check] %u bytes: %s (return %u/%u)", size, diff ? "MISMATCH" : "same", got, ret);
    std::memcpy(ppc_ptr(dst), want.data(), span);
    c->r[3] = ret;
}
