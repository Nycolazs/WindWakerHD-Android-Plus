// LLVM compile pipeline for the recompiled game (see compile.h).
#include "compile.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../release.h"
#include "emit.h"
#include "locals.h"
#include "program.h"

namespace recomp {

// Code for the CPU we run on: generic AArch64 plus only the extensions the kernel reports (a CPU
// model would imply some the firmware disables, e.g. SVE on Snapdragons); the model itself is used
// for scheduling only ("tune-cpu"). SVE/SVE2 are off: code compiled with them never ran on a
// tested device, and a crash on a Dimensity 8350 points at it (issue #24); WWHD_SVE=1 uses them where
// the kernel enables them (MediaTek Dimensity, Google Tensor; the code stays vector-length
// agnostic). SME stays off: streaming mode isn't something compiled game code can use.
std::string host_features() {
    std::string f;
#if defined(__aarch64__) && defined(__linux__)
    unsigned long hw = getauxval(AT_HWCAP);
    if (hw & (1UL << 8)) f += "+lse,";        // HWCAP_ATOMICS
    if (hw & (1UL << 9)) f += "+fullfp16,";   // HWCAP_FPHP
    if (hw & (1UL << 20)) f += "+dotprod,";   // HWCAP_ASIMDDP
    if (hw & (1UL << 15)) f += "+rcpc,";      // HWCAP_LRCPC
    if (hw & (1UL << 7)) f += "+rdm,";        // HWCAP_ASIMDRDM
    unsigned long hw2 = getauxval(AT_HWCAP2);
    static const bool sveOn = getenv("WWHD_SVE") && atoi(getenv("WWHD_SVE")) != 0;
    const bool sve = sveOn && (hw & (1UL << 22));  // HWCAP_SVE
    const bool sve2 = sve && (hw2 & (1UL << 1));  // HWCAP2_SVE2
    f += sve ? "+sve," : "-sve,";
    f += sve2 ? "+sve2," : "-sve2,";
    f += "-sme";
#else
    f += "-sve,-sve2,-sme";
#endif
    return f;
}

size_t module_count(size_t functions, size_t perModule) { return (functions + perModule - 1) / perModule; }

bool compile_game(const CompileOptions& opt, CompileResult& res, std::string& err) {
    auto t0 = std::chrono::steady_clock::now();
    Program prog;
    if (!prog.load(opt.rpxPath, err)) return false;
    if (!release::select(prog.entry)) {  // before the hooks: their USA addresses are translated
        err = "this game version is not supported";
        return false;
    }
    Hooks hooks;
    hooks.parse(opt.hooksText);

    static std::once_flag init;
    std::call_once(init, [] {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
    });
    const std::string triple = "aarch64-unknown-linux-android30";
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) return false;
    std::string cpu = opt.cpu.empty() ? "generic" : opt.cpu;
    std::string features = opt.cpu.empty() ? host_features() : "";
    std::string tune = opt.cpu.empty() ? llvm::sys::getHostCPUName().str() : "";

    // Hot and cold code (as LASRecomp-Android-Plus does): most of the game's ~40000 functions never run
    // in a play, or run too rarely to matter, yet -O3 on them was most of the first start's ~10 minutes
    // on a phone that soon runs hot. The functions of hot_functions.txt (what a play through the game
    // reached) are compiled as before; the others at coldOptLevel, in modules of their own (LASRecomp
    // measured cold functions marked optnone inside -O3 modules 1.8x slower to build than separate ones).
    // On a Galaxy S20 FE, 4 jobs: everything at -O3 363 s, with the list 253 s, everything cold 94 s;
    // a module at -O3 35 s, at the cold level 9 s. The game thread's time in Outset stays the same.
    std::unordered_set<uint32_t> hot;
    for (size_t a = 0; a < opt.hotList.size();) {
        size_t b = opt.hotList.find('\n', a);
        if (b == std::string::npos) b = opt.hotList.size();
        // (USA addresses, as the hook lists: the running release's)
        if (b > a && opt.hotList[a] != '#')
            if (uint32_t e = release::code((uint32_t)strtoul(opt.hotList.substr(a, b - a).c_str(), nullptr, 16))) hot.insert(e);
        a = b + 1;
    }
    // and what they call directly (bl): a play reaches only part of each function's paths, and a
    // fight, an item or an event the profile never saw goes through a hot function into those
    if (!hot.empty()) {
        std::unordered_set<uint32_t> entries(prog.entries.begin(), prog.entries.end()), callees;
        for (uint32_t f : hot) {
            if (!entries.count(f)) continue;
            for (uint32_t a = f, end = prog.funcEnd(f); a < end; a += 4) {
                const uint32_t w = prog.word(a);
                if ((w >> 26) != 18 || (w & 3) != 1) continue;  // bl
                const uint32_t t = a + (uint32_t)(((int32_t)(w << 6)) >> 6 & ~3);
                if (entries.count(t)) callees.insert(t);
            }
        }
        hot.insert(callees.begin(), callees.end());
    }
    std::vector<std::vector<uint32_t>> modules;
    std::vector<bool> coldModule;
    {
        std::vector<uint32_t> hotFns, coldFns;
        for (uint32_t e : prog.entries) (hot.empty() || hot.count(e) ? hotFns : coldFns).push_back(e);
        auto split = [&](const std::vector<uint32_t>& fns, size_t per, bool cold) {
            for (size_t i = 0; i < fns.size(); i += per) {
                modules.emplace_back(fns.begin() + i, fns.begin() + std::min(fns.size(), i + per));
                coldModule.push_back(cold);
            }
        };
        split(hotFns, opt.perModule, false);
        split(coldFns, opt.perModule, true);
        res.coldFunctions = coldFns.size();
    }
    llvm::sys::fs::create_directories(opt.outDir);
    if (opt.modulesTotal) *opt.modulesTotal = modules.size();
    std::atomic<size_t> next{0}, instructions{0}, unhandled{0}, localized{0}, localFallback{0};
    std::atomic<bool> failed{false};
    std::mutex errm;
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(errm);
        if (!failed.exchange(true)) err = why;
    };
    auto worker = [&] {
        for (size_t i; !failed && (i = next++) < modules.size();) {
            const bool cold = coldModule[i];
            const int level = cold ? opt.coldOptLevel : opt.optLevel;
            if (opt.cancel && *opt.cancel) return fail("cancelled");
            llvm::LLVMContext ctx;
            EmitStats st;
            std::string e;
            char name[32];
            snprintf(name, sizeof name, "code_%03zu", i);
            if (opt.keepExisting && llvm::sys::fs::exists(opt.outDir + "/" + name + ".o")) {
                if (opt.modulesDone) ++*opt.modulesDone;
                continue;
            }
            // WWHD_RECOMPILE_TIMES=1: the time of each step (emit, ops inlined, registers in locals, optimization, code)
            static const bool times = getenv("WWHD_RECOMPILE_TIMES") != nullptr;
            auto tp = std::chrono::steady_clock::now();
            double ms[5] = {};
            auto phase = [&](int k) {
                auto now = std::chrono::steady_clock::now();
                ms[k] = std::chrono::duration<double, std::milli>(now - tp).count();
                tp = now;
                if (times && k == 4)
                    fprintf(stderr, "[recomp] %s: emit %.0f, inline %.0f, locals %.0f, optimize %.0f, codegen %.0f ms\n", name, ms[0], ms[1], ms[2], ms[3], ms[4]);
            };
            auto m = emit_module(ctx, prog, hooks, modules[i], opt.opsBitcode, name, st, e);
            if (!m) return fail(std::string(name) + ": " + e);
            phase(0);
            m->setTargetTriple(triple);
            llvm::TargetOptions to;
            to.AllowFPOpFusion = llvm::FPOpFusion::Strict;  // exact FP: no contraction into FMA
            std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
                triple, cpu, features, to, llvm::Reloc::PIC_, std::nullopt,
                level >= 3 ? llvm::CodeGenOptLevel::Aggressive : level == 2 ? llvm::CodeGenOptLevel::Default
                           : level == 1 ? llvm::CodeGenOptLevel::Less : llvm::CodeGenOptLevel::None));
            // (cold levels: 0 SROA + EarlyCSE + SimplifyCFG, -1 SROA alone; the fast code generator)
            m->setDataLayout(tm->createDataLayout());
            for (llvm::Function& f : *m) {
                if (f.isDeclaration()) continue;
                if (!tune.empty() && tune != "generic") f.addFnAttr("tune-cpu", tune);
                // unwind tables (.eh_frame, registered by the loader): native backtraces go through the
                // game code, so crash logs name the game functions of the call chain
                f.setUWTableKind(llvm::UWTableKind::Async);
            }
            std::string verr;
            llvm::raw_string_ostream vs(verr);
            if (llvm::verifyModule(*m, &vs)) return fail(std::string(name) + ": invalid IR: " + verr.substr(0, 500));
            // inlines the ops and folds their decoding
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cgam;
            llvm::ModuleAnalysisManager mam;
            llvm::PassBuilder pb(tm.get());
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);
            {  // the ops inlined and their decoding folded first: the register copy's accesses are then known
                llvm::ModulePassManager mpm;
                mpm.addPass(llvm::AlwaysInlinerPass());
                mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::InstSimplifyPass()));
                mpm.run(*m, mam);
            }
            phase(1);
            LocalsStats ls;
            localize_registers(*m, locals_options(), ls);
            phase(2);
            localized += ls.functions;
            localFallback += ls.fallback;
            if (level <= 0) {
                // cold code: the register copy into SSA values (it lives on the stack otherwise: every
                // register access a load or store), redundancies and empty blocks gone; nothing else
                llvm::ModulePassManager mpm;
                llvm::FunctionPassManager fpm;
                fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
                if (level == 0) {
                    fpm.addPass(llvm::EarlyCSEPass());
                    fpm.addPass(llvm::SimplifyCFGPass());
                }
                mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
                mpm.run(*m, mam);
            } else {
                pb.buildPerModuleDefaultPipeline(level >= 3 ? llvm::OptimizationLevel::O3 : level == 2 ? llvm::OptimizationLevel::O2
                                                                                                       : llvm::OptimizationLevel::O1).run(*m, mam);
            }
            phase(3);
            std::string path = opt.outDir + "/" + name + ".o", tmp = path + ".part";
            {
                std::error_code ec;
                llvm::raw_fd_ostream out(tmp, ec, llvm::sys::fs::OF_None);
                llvm::legacy::PassManager cg;
                if (ec || tm->addPassesToEmitFile(cg, out, nullptr, llvm::CodeGenFileType::ObjectFile))
                    return fail(std::string("cannot write ") + path);
                cg.run(*m);
            }
            phase(4);
            if (llvm::sys::fs::rename(tmp, path)) return fail(std::string("cannot write ") + path);
            instructions += st.instructions;
            unhandled += st.unhandled;
            if (opt.modulesDone) ++*opt.modulesDone;
        }
    };
    unsigned jobs = opt.jobs ? opt.jobs : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned j = 0; j < jobs; j++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    res.functions = prog.entries.size();
    res.modules = modules.size();
    res.instructions = instructions;
    res.unhandled = unhandled;
    res.localized = localized;
    res.localFallback = localFallback;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return !failed;
}

}  // namespace recomp
