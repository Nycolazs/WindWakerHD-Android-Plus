// Host driver for the LLVM recompiler (runtime/src/recomp): cking.rpx -> AArch64 object files.
// Used to validate the device-side recompiler on a PC: the objects replace the clang-compiled
// build/gen/code_*.c in the Android build (CMake -DGEN_OBJECTS=...).
//
// usage: recomp_llvm RPX OPS_BC HOOKS_DIR OUTDIR [functions-per-module] [jobs]
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <fstream>
#include <sstream>

#include <llvm/Support/CommandLine.h>

#include "recomp/compile.h"

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s RPX OPS_BC HOOKS_DIR OUTDIR [functions-per-module] [jobs]\n", argv[0]);
        return 2;
    }
    if (const char* a = getenv("RECOMP_LLVM_ARGS")) {  // e.g. "-time-passes"
        static std::string s = a;
        const char* args[] = {"recomp_llvm", s.c_str()};
        llvm::cl::ParseCommandLineOptions(2, args);
    }
    recomp::CompileOptions opt;
    opt.rpxPath = argv[1];
    opt.opsBitcode = slurp(argv[2]);
    for (const char* f : {"hooks.txt", "hooks_climb.txt", "hooks_mods.txt", "hooks_aspect.txt", "hooks_perf.txt", "hooks_resmgr.txt"}) opt.hooksText += slurp(std::string(argv[3]) + "/" + f) + "\n";
    opt.outDir = argv[4];
    opt.cpu = getenv("RECOMP_HOST_CPU") ? "" : "generic";  // RECOMP_HOST_CPU=1 on a phone: the app's own target
    if (const char* o = getenv("RECOMP_OPT")) opt.optLevel = atoi(o);
    if (const char* o = getenv("RECOMP_COLD_OPT")) opt.coldOptLevel = atoi(o);
    opt.hotText = slurp(std::string(argv[3]) + "/hot_functions.txt");
    opt.coveredText = slurp(std::string(argv[3]) + "/covered_functions.txt");
    if (const char* h = getenv("RECOMP_COVERED")) opt.coveredText = slurp(h);  // another covered_functions.txt
    if (argc > 5) opt.perModule = strtoul(argv[5], nullptr, 10);
    if (argc > 6) opt.jobs = (unsigned)strtoul(argv[6], nullptr, 10);
    recomp::CompileResult res;
    std::string err;
    bool ok = recomp::compile_game(opt, res, err);
    printf("%zu functions, %zu modules, %zu instructions (%zu unhandled) in %.1f s; registers in locals: %zu functions, %zu kept in the "
           "struct; %zu cold%s%s\n", res.functions, res.modules, res.instructions, res.unhandled, res.seconds, res.localized, res.localFallback, res.coldFunctions,
           ok ? "" : ": ", ok ? "" : err.c_str());
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("peak memory %ld MB\n", ru.ru_maxrss / 1024);
    return ok ? 0 : 1;
}
