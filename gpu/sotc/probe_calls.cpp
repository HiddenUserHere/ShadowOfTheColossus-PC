// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CALL_PROBE=1 (off by default; observe only, nothing is changed). Settles which of the functions with no
// static reference run, and who calls them (docs/re-stutters/native/re-pipelines/indirect-callers.md, topic T6).
// Hooks in scripts/hooks.d/calls.py: mode entry, HookAction::Continue, one hook group each, so one refused hook
// does not stop the others. Each hook is named CALL_<hex eboot address>:
//   Counts the calls. At entry [rsp] is the return address; the first 8 distinct callers are kept and logged. The
//   arguments rdi, rsi, rdx, rcx are logged for the first 3 calls.
// Port side, no hook: NoteStepRateWrites() is called from the SET_CONTEXT_REG handler (liverpool.cpp) and counts
//   every write to VGT_INSTANCE_STEP_RATE_0/1 (context registers 0xA2A8 / 0xA2A9) on any path. The first 8 are logged.
// Output: "SOTC CALL PROBE ..." lines on stderr. Every 5 s, while the window had activity: one line per function
//   with calls (cumulative) and one step-rate line. At most kLineCap lines in total, then only a count.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "probes.h"

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr int kCallers = 8;             // distinct return addresses kept per function
constexpr uint64_t kArgCalls = 3;       // calls whose arguments are logged
constexpr int kValues = 8;              // step-rate writes logged
constexpr uint32_t kStepRate0 = 0xA2A8; // VGT_INSTANCE_STEP_RATE_0, word index (regs.cpp static_assert)
constexpr uint32_t kStepRate1 = 0xA2A9; // VGT_INSTANCE_STEP_RATE_1
constexpr uint64_t kImageSpan = uint64_t{1} << 30; // an address this far above the image base is not in the eboot
constexpr int kLineCap = 400;
constexpr double kReportSeconds = 5.0;

struct Site {
    const char* hook = nullptr; // hook name in scripts/hooks.d/calls.py
    uint64_t calls = 0;
    int ncallers = 0;
    uintptr_t callers[kCallers] = {};
};

// One entry per hook of scripts/hooks.d/calls.py. Each handler gets its own Site through the user pointer.
Site g_sites[] = {
    {"CALL_86BB90"}, {"CALL_86BB30"}, {"CALL_862C50"}, {"CALL_8615F0"}, {"CALL_861730"}, {"CALL_861880"},
    {"CALL_860EA0"}, {"CALL_8613E0"}, {"CALL_869350"}, {"CALL_8694E0"}, {"CALL_869370"}, {"CALL_860C50"},
    {"CALL_8612B0"}, {"CALL_94A80"},  {"CALL_949F0"}, {"CALL_103AA0"},
};

// Shared state, guarded by g_mutex.
std::mutex g_mutex;
int g_lines = 0;
uint64_t g_dropped = 0;
bool g_started = false;
bool g_active = false; // activity since the last report line
std::chrono::steady_clock::time_point g_window_start;
uint64_t g_step_writes = 0;
int g_step_logged = 0;
struct StepValue {
    uint32_t reg;
    uint32_t value;
};
StepValue g_step_values[kValues] = {};

// Prints one line unless the cap is reached. Caller holds g_mutex.
void Emit(const std::string& line) {
    if (g_lines < kLineCap) {
        std::fprintf(stderr, "%s\n", line.c_str());
        ++g_lines;
    } else {
        ++g_dropped;
    }
}

// "eb:0x<offset>" for an address inside the eboot image (the offset is the eboot address), else "abs:0x<address>".
std::string CallerText(uintptr_t ret) {
    const auto base = static_cast<uintptr_t>(SotcHooks::ImageBase());
    char buf[40];
    if (base && ret >= base && static_cast<uint64_t>(ret - base) < kImageSpan) {
        std::snprintf(buf, sizeof buf, "eb:0x%llx", static_cast<unsigned long long>(ret - base));
    } else {
        std::snprintf(buf, sizeof buf, "abs:0x%llx", static_cast<unsigned long long>(ret));
    }
    return buf;
}

// The 5 s report. Caller holds g_mutex. Nothing is printed for a window without activity.
void MaybeReport(std::chrono::steady_clock::time_point now) {
    if (!g_started) {
        g_started = true;
        g_window_start = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - g_window_start).count();
    if (elapsed < kReportSeconds) {
        return;
    }
    if (g_active) {
        for (const Site& site : g_sites) {
            if (site.calls == 0) {
                continue;
            }
            std::string line = "SOTC CALL PROBE: " + std::string(site.hook) + " calls " +
                               std::to_string(site.calls) + " callers [";
            for (int i = 0; i < site.ncallers; ++i) {
                if (i) {
                    line += ' ';
                }
                line += CallerText(site.callers[i]);
            }
            line += "]";
            Emit(line);
        }
        if (g_step_writes) {
            std::string line = "SOTC CALL PROBE step-rate regs: writes " + std::to_string(g_step_writes) + " values [";
            for (int i = 0; i < g_step_logged; ++i) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%s%x=0x%08x", i ? " " : "", static_cast<unsigned>(g_step_values[i].reg),
                              static_cast<unsigned>(g_step_values[i].value));
                line += buf;
            }
            line += "]";
            Emit(line);
        }
        g_active = false;
    }
    g_window_start = now;
}

// Entry of a hooked function. The caller is the return address at [rsp] (regs.rsp points at it at entry).
HookAction OnCall(GuestRegs& regs, void* user) {
    auto* site = static_cast<Site*>(user);
    uintptr_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof ret);
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    MaybeReport(now);
    g_active = true;
    const uint64_t n = ++site->calls;
    bool stored = false;
    bool known = false;
    for (int i = 0; i < site->ncallers; ++i) {
        if (site->callers[i] == ret) {
            known = true;
            break;
        }
    }
    if (!known && site->ncallers < kCallers) {
        site->callers[site->ncallers++] = ret;
        stored = true;
    }
    if (n <= kArgCalls) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "SOTC CALL PROBE %s call %llu from %s rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx",
                      site->hook, static_cast<unsigned long long>(n), CallerText(ret).c_str(),
                      static_cast<unsigned long long>(regs.rdi), static_cast<unsigned long long>(regs.rsi),
                      static_cast<unsigned long long>(regs.rdx), static_cast<unsigned long long>(regs.rcx));
        Emit(buf);
    } else if (stored) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "SOTC CALL PROBE %s call %llu new caller %s", site->hook,
                      static_cast<unsigned long long>(n), CallerText(ret).c_str());
        Emit(buf);
    }
    return HookAction::Continue;
}

} // namespace

// Port side (liverpool.cpp, SET_CONTEXT_REG, guarded by SOTC_CALL_PROBE). first_reg is the register word index of
// values[0], so values[i] is written to first_reg + i.
void NoteStepRateWrites(uint32_t first_reg, const uint32_t* values, uint32_t count) {
    if (first_reg > kStepRate1 || first_reg + count <= kStepRate0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    MaybeReport(now);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t reg = first_reg + i;
        if (reg != kStepRate0 && reg != kStepRate1) {
            continue;
        }
        ++g_step_writes;
        g_active = true;
        if (g_step_logged < kValues) {
            g_step_values[g_step_logged] = {reg, values[i]};
            ++g_step_logged;
            char buf[120];
            std::snprintf(buf, sizeof buf, "SOTC CALL PROBE step-rate write %llu reg=0x%x value=0x%08x",
                          static_cast<unsigned long long>(g_step_writes), static_cast<unsigned>(reg),
                          static_cast<unsigned>(values[i]));
            Emit(buf);
        }
    }
}

void RegisterCalls() {
    for (Site& site : g_sites) {
        SotcHooks::Handler handler;
        handler.pre = &OnCall;
        handler.user = &site;
        SotcHooks::RegisterHandler(site.hook, handler);
    }
}

} // namespace SotcProbes
