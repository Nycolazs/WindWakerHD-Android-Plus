#include <atomic>
// Per-thread ring buffer of guest function entries, for debugging.
// Enable with WWHD_TRACE_FUNCS=1; dump with `kill -USR1 <pid>` or at fatal errors.
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime.h"

int g_ppc_trace = 0;

namespace {
constexpr uint32_t kRing = 1 << 16;
struct Ring {
    uint32_t buf[kRing];
    uint32_t pos = 0;
    char name[64] = {};
};
std::mutex g_rings_mutex;
std::vector<Ring*> g_rings;
thread_local Ring* t_ring = nullptr;
}  // namespace

// WWHD_WATCH=<hex addr>: log arguments whenever that guest function is entered
static uint32_t g_watch = 0;
static int g_watch_count = 0, g_watch_limit = 200;
static int g_watch_reg = -1;         // WWHD_WATCH_IF=reg=value: only log when r<reg> == value
static uint32_t g_watch_val = 0;

// WWHD_WATCH_R3=<hex>: log every function entered with r3 == value (object/list tracing), with thread
static uint32_t g_watch_r3 = 0;
static std::atomic<int> g_watch_r3_count{0};

void true60_nan_probe(uint32_t addr);  // true60.cpp (WWHD_NAN_PROBE)
// WWHD_COVERAGE=<file>: every game function entered is noted (one bit per instruction address) and
// the list of their addresses written to <file> every 20 s, merged with what it already holds. For
// tools/recomp/hot_functions.txt: the functions the play reaches, compiled fully on the phone, the
// rest at a lower level (runtime/src/recomp/compile.cpp)
namespace {
constexpr uint32_t kCovBase = 0x02000000, kCovSize = 0x01000000;
std::atomic<uint8_t>* g_cov = nullptr;
std::string g_cov_path;

void coverage_write() {
    std::vector<uint8_t> seen(kCovSize / 4 / 8);
    if (FILE* f = fopen(g_cov_path.c_str(), "r")) {
        char line[64];
        while (fgets(line, sizeof line, f)) {
            uint32_t a = (uint32_t)strtoul(line, nullptr, 16);
            if (a >= kCovBase && a < kCovBase + kCovSize) seen[(a - kCovBase) / 32] |= 1 << ((a - kCovBase) / 4 % 8);
        }
        fclose(f);
    }
    size_t n = 0;
    for (size_t i = 0; i < seen.size(); i++) seen[i] |= g_cov[i].load(std::memory_order_relaxed);
    std::string tmp = g_cov_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    for (size_t i = 0; i < seen.size(); i++)
        for (int b = 0; b < 8; b++)
            if (seen[i] >> b & 1) {
                fprintf(f, "%08X\n", kCovBase + (uint32_t)(i * 8 + b) * 4);
                n++;
            }
    fclose(f);
    rename(tmp.c_str(), g_cov_path.c_str());
    log_msg("[coverage] %zu functions in %s", n, g_cov_path.c_str());
}
}  // namespace

void coverage_init() {
    const char* p = getenv("WWHD_COVERAGE");
    if (!p || !*p || g_cov) return;
    g_cov_path = p;
    g_cov = new std::atomic<uint8_t>[kCovSize / 4 / 8]();
    g_ppc_trace = 1;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            coverage_write();
        }
    }).detach();
}

extern "C" void ppc_trace_enter(uint32_t addr) {
    if (g_cov && addr - kCovBase < kCovSize) {
        const uint32_t i = (addr - kCovBase) / 4;
        if (!(g_cov[i / 8].load(std::memory_order_relaxed) >> (i % 8) & 1)) g_cov[i / 8].fetch_or((uint8_t)(1 << (i % 8)), std::memory_order_relaxed);
    }
    static const bool nan_probe = getenv("WWHD_NAN_PROBE") != nullptr;
    if (nan_probe) true60_nan_probe(addr);
    if (g_watch_r3) {
        Cpu* c = threads::current();
        if (c && c->r[3] == g_watch_r3 && g_watch_r3_count++ < 3000) {
            char name[32] = {};
            pthread_getname_np(pthread_self(), name, sizeof name);
            log_msg("[watch3] %-14s f_%08X lr=%08X r4=%08X r5=%08X", name, addr, c->lr, c->r[4], c->r[5]);
        }
    }
    if (addr == g_watch && g_watch_count < g_watch_limit) {
        Cpu* c = threads::current();
        if (c && (g_watch_reg < 0 || c->r[g_watch_reg] == g_watch_val)) {
            g_watch_count++;
            char tn[32] = {};
            pthread_getname_np(pthread_self(), tn, sizeof tn);
            log_msg("[watch] %s %08X lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X | r28=%08X r29=%08X r30=%08X r31=%08X",
                    tn, addr, c->lr, c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[28], c->r[29], c->r[30], c->r[31]);
            if (const char* e = getenv("WWHD_WATCH_DUMP")) {  // "reg": hex dump 0x100 bytes at r<reg>
                unsigned reg = (unsigned)atoi(e) & 31;
                uint32_t base = c->r[reg];
                for (uint32_t o = 0; o < 0x100; o += 32) {
                    char line[160];
                    int n = snprintf(line, sizeof line, "[watch]   %08X:", base + o);
                    for (uint32_t k = 0; k < 32; k += 4) n += snprintf(line + n, sizeof line - n, " %08X", ld32(base + o + k));
                    log_msg("%s", line);
                }
            }
            if (const char* e = getenv("WWHD_WATCH_EXPR")) {  // "reg+off": print ld32(r<reg> + off)
                unsigned reg = 0, off = 0;
                if (sscanf(e, "%u+%x", &reg, &off) == 2 && reg < 32) log_msg("[watch]   ld32(r%u+%X) = %08X", reg, off, ld32(c->r[reg] + off));
            }
        }
    }
    if (!t_ring) {
        t_ring = new Ring();
        pthread_getname_np(pthread_self(), t_ring->name, sizeof t_ring->name);
        std::lock_guard<std::mutex> lk(g_rings_mutex);
        g_rings.push_back(t_ring);
    }
    t_ring->buf[t_ring->pos++ & (kRing - 1)] = addr;
}

void trace_dump(FILE* f, unsigned last) {
    std::lock_guard<std::mutex> lk(g_rings_mutex);
    for (Ring* r : g_rings) {
        uint32_t end = __atomic_load_n(&r->pos, __ATOMIC_RELAXED);  // other threads keep running
        fprintf(f, "=== thread '%s' (%u calls)\n", r->name, end);
        uint32_t n = std::min(last, std::min(end, kRing));
        for (uint32_t i = end - n; i != end; i++) fprintf(f, "%08X\n", r->buf[i & (kRing - 1)]);
    }
    fflush(f);
}

static void on_usr1(int) {
    FILE* f = fopen("trace_dump.txt", "w");
    if (f) { trace_dump(f, 4000); fclose(f); }
    write(2, "[trace] wrote trace_dump.txt\n", 29);
}

__attribute__((constructor)) static void trace_init() {
    if (const char* w = getenv("WWHD_WATCH_R3")) { g_watch_r3 = (uint32_t)strtoul(w, nullptr, 16); g_ppc_trace = 1; }
    if (const char* w = getenv("WWHD_WATCH")) {
        g_watch = (uint32_t)strtoul(w, nullptr, 16);
        if (const char* l = getenv("WWHD_WATCH_LIMIT")) g_watch_limit = atoi(l);
        if (const char* f = getenv("WWHD_WATCH_IF")) {
            unsigned reg; char val[32];
            if (sscanf(f, "%u=%31s", &reg, val) == 2) { g_watch_reg = (int)(reg & 31); g_watch_val = (uint32_t)strtoul(val, nullptr, 16); }
        }
        g_ppc_trace = 1;
    }
    if (getenv("WWHD_NAN_PROBE")) g_ppc_trace = 1;
    if (getenv("WWHD_TRACE_FUNCS")) {
        g_ppc_trace = 1;
        signal(SIGUSR1, on_usr1);
    }
}
