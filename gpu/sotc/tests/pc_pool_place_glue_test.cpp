// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-place-glue-test. Unit tests of gpu/sotc/pc_pool_place_glue.cpp: the switches, the observe and verify
// counters, write demotion, the apply publish list and ChooseWindow, and the recycler gates through the glue.
//
// Build and run (scratch directory, not the repo):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_place_glue_test.cpp gpu/sotc/pc_pool_place_glue.cpp gpu/sotc/pc_pool_place.cpp -o pc_pool_place_glue_test.exe
//   ./pc_pool_place_glue_test.exe   (prints PASS or FAIL; exit code 0 on PASS)

#include "../pc_pool_place_glue.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace SotcPcPlaceGlue;
namespace PP = SotcPcPlace;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

constexpr uint64_t kMiBytes = 0x100000;
constexpr uint64_t kGiBytes = 0x40000000;
// Two adjacent 1 GiB windows inside the pool VA: CPU_ONLY first, GPU_DYNAMIC after it.
constexpr uint64_t kCpuWin = 0x1040300000;
constexpr uint64_t kGpuWin = 0x1080300000;
constexpr const char* kWindows = "0x1040300000:0x40000000:CPU_ONLY;0x1080300000:0x40000000:GPU_DYNAMIC";

// Owner objects (the names and kinds are the provisional priors of the glue).
PP::OwnerKey CpuOwner() { return PP::OwnerKey{0x2dd9f80, "CPUMemory", 1}; }
PP::OwnerKey GpuOwner() { return PP::OwnerKey{0x2dd9f84, "GPUMemory", 2}; }
PP::OwnerKey PageOwner() { return PP::OwnerKey{0x2dd9b00, "PageAllocator", 1}; }

// The flush check of OnFlushDone: every pending run is confirmed (ok) or refused.
std::function<bool(uint64_t, uint64_t)> Unmapped(bool ok) {
    return [ok](uint64_t, uint64_t) { return ok; };
}

bool Has(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

Config Cfg(const char* place, const char* recycle, const char* frames = nullptr, const char* windows = nullptr) {
    return MakeConfig(place, recycle, frames, windows);
}

PP::Rule HeapRule(int32_t slot, PP::Class cls, const char* label) {
    PP::Rule r;
    r.match = PP::Rule::Match::Heap;
    r.heap_slot = slot;
    r.cls = cls;
    r.label = label;
    return r;
}

void TestConfig() {
    Config d = MakeConfig(nullptr, nullptr, nullptr, nullptr);
    CHECK(d.place == PP::Mode::Off);
    CHECK(d.recycle == PP::VaRecycler::Mode::Off);
    CHECK(d.quarantine_frames == 120);
    CHECK(d.warnings.empty());
    CHECK(!d.windows_auto || d.windows_spec.empty()); // unset windows: auto, with no explicit spec
    CHECK(d.windows_auto);
    CHECK(!Glue(d).enabled());
    Config off = MakeConfig("0", "0", nullptr, "none");
    CHECK(!off.windows_auto && off.windows_spec.empty());
    Config explicit_win = MakeConfig("0", "0", nullptr, kWindows);
    CHECK(!explicit_win.windows_auto && explicit_win.windows_spec == kWindows);

    Config on = Cfg("3", "2", "500", kWindows);
    CHECK(on.place == PP::Mode::Apply);
    CHECK(on.recycle == PP::VaRecycler::Mode::Recycle);
    CHECK(on.quarantine_frames == 500);
    CHECK(on.warnings.empty());
    CHECK(on.windows_spec == kWindows);

    Config names = Cfg("observe", "1");
    CHECK(names.place == PP::Mode::Observe);
    CHECK(names.recycle == PP::VaRecycler::Mode::Quarantine);

    Config bad = Cfg("9", "7", "abc");
    CHECK(bad.place == PP::Mode::Off);
    CHECK(bad.recycle == PP::VaRecycler::Mode::Off);
    CHECK(bad.quarantine_frames == 120);
    CHECK(!bad.warnings.empty());
    CHECK(Has(bad.warnings, "SOTC_PC_PLACE"));
    CHECK(Has(bad.warnings, "SOTC_PC_RECYCLE '7'"));
    CHECK(Has(bad.warnings, "SOTC_PC_RECYCLE_FRAMES"));
}

void TestWindowSpec() {
    std::vector<PP::VaWindow> w;
    std::string why;
    CHECK(ParseWindowSpec(kWindows, &w, &why));
    CHECK(w.size() == 2);
    CHECK(w.size() == 2 && w[0].base == kCpuWin && w[0].size == kGiBytes && w[0].cls == PP::Class::CpuOnly);
    CHECK(w.size() == 2 && w[1].base == kGpuWin && w[1].cls == PP::Class::GpuDynamic);

    CHECK(ParseWindowSpec("1048576:1048576:gpu_written;", &w, &why)); // decimal, lower case, trailing ';'
    CHECK(w.size() == 1 && w[0].cls == PP::Class::GpuWritten);

    CHECK(ParseWindowSpec("", &w, &why) && w.empty());
    CHECK(!ParseWindowSpec("0x1000:0x100000", &w, &why));     // two fields
    CHECK(Has(why, "base:size:CLASS"));
    CHECK(!ParseWindowSpec("0x1000:0x100000:UNKNOWN", &w, &why)); // Unknown is not a window class
    CHECK(Has(why, "unknown class"));
    CHECK(!ParseWindowSpec("zz:0x100000:CPU_ONLY", &w, &why));
    CHECK(Has(why, "bad base"));
}

void TestOffIsInert() {
    Glue g(Cfg("0", "0"));
    CHECK(!g.enabled());
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnGpuBind(kCpuWin, kMiBytes);
    g.OnWriteFault(kCpuWin);
    CHECK(g.blocks() == 0);
    std::string out;
    g.Report(out, 5.0);
    CHECK(out.empty());
    uint64_t v = 7;
    CHECK(g.PublishList(&v).empty() && v == 0);
}

void TestAddRule() {
    Glue g(Cfg("1", "0"));
    std::string why;
    PP::Rule bad;
    bad.match = PP::Rule::Match::Addr;
    bad.addr = 0;
    CHECK(!g.AddRule(bad, &why));
    PP::Rule nameless;
    nameless.match = PP::Rule::Match::NameKind;
    CHECK(!g.AddRule(nameless, &why));
    CHECK(g.AddRule(HeapRule(1, PP::Class::CpuOnly, "GameSingleFrame"), &why));
}

void TestObserveClassAndBind() {
    Glue g(Cfg("1", "0"));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    CHECK(g.blocks() == 1);
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "CPU_ONLY 1 blocks"));
    CHECK(Has(out, "no heap context 1"));
    CHECK(Has(out, "SOTC PC PLACE mode observe"));

    // A bind inside the block: the block is no longer CPU_ONLY, and the event is counted once.
    g.OnGpuBind(kCpuWin + 0x1000, 0x40);
    g.OnGpuBind(kCpuWin, kMiBytes);
    out.clear();
    g.Report(out, 5.0);
    CHECK(Has(out, "CPU_ONLY 0 blocks"));
    CHECK(Has(out, "CPU_ONLY bound 1 "));
    CHECK(Has(out, "blocks bound 1"));

    // A bind on no block is counted as untracked, and a bind on the block's end is not an overlap.
    g.OnGpuBind(kCpuWin + kMiBytes, 0x100);
    g.OnGpuBind(UINT64_MAX - 4, 100); // wraps: clamped, must not crash
    out.clear();
    g.Report(out, 5.0);
    CHECK(Has(out, "untracked 2"));
}

void TestHeapContextAndDisagreement() {
    Glue g(Cfg("1", "0"));
    std::string why;
    CHECK(g.AddRule(HeapRule(16, PP::Class::GpuDynamic, "RenderSingleFrameGPU"), &why));
    CHECK(g.AddRule(HeapRule(17, PP::Class::GpuWritten, "RenderSingleFrameCPU"), &why));
    {
        HeapScope scope(16);
        CHECK(CurrentHeapSlot() == 16);
        g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1); // heap from the scope, owner CPU
    }
    CHECK(CurrentHeapSlot() == -1);
    std::string out;
    g.Report(out, 5.0);
    // Heap GPU_DYNAMIC inside a CPU owner: the block publishes Unknown, and it is not counted as a heap-less block.
    CHECK(Has(out, "CPU_ONLY 0 blocks"));
    CHECK(Has(out, "UNKNOWN 1 blocks"));
    CHECK(Has(out, "no heap context 0"));
}

void TestHeapScopeNesting() {
    HeapScope outer(3);
    CHECK(CurrentHeapSlot() == 3);
    {
        HeapScope inner(5);
        CHECK(CurrentHeapSlot() == 5);
    }
    CHECK(CurrentHeapSlot() == 3);
}

void TestLedger() {
    Glue g(Cfg("1", "0"));
    std::string why;
    CHECK(g.AddRule(HeapRule(1, PP::Class::CpuOnly, "GameSingleFrame"), &why));
    CHECK(g.AddRule(HeapRule(16, PP::Class::GpuDynamic, "RenderSingleFrameGPU"), &why));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnHeapCarve(kCpuWin, 1);      // same class: no conflict
    g.OnAllocObserved(CpuOwner(), kCpuWin + kMiBytes, kMiBytes, 1, -1);
    g.OnHeapCarve(kCpuWin + kMiBytes, 1);
    g.OnHeapCarve(kCpuWin + kMiBytes, 16); // a GPU heap carved from the CPU block: mixed, Unknown for good
    g.OnHeapCarve(kCpuWin + 2 * kMiBytes, 99); // no rule for this heap: counted as unruled
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "mixed (ledger conflict) 1 blocks"));
    CHECK(Has(out, "carves 4 (unruled 1, untracked 1)"));
    CHECK(Has(out, "CPU_ONLY 1 blocks"));
}

void TestVerifyWindows() {
    Glue g(Cfg("2", "0", nullptr, kWindows));
    CHECK(g.warnings().empty());
    CHECK(g.enabled());
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);         // placed in its window
    g.OnAllocObserved(CpuOwner(), kGpuWin, kMiBytes, 1, -1);         // CPU_ONLY placement, GPU window: mismatch
    g.OnAllocObserved(CpuOwner(), kCpuWin + kGiBytes - kMiBytes, kMiBytes, 1, -1); // last MiB of the CPU window
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "CPU_ONLY 1/3"));       // mismatch 1 of 3 CPU_ONLY placements
    CHECK(Has(out, "ignoring window 3 blocks"));
    CHECK(Has(out, "with window 2 blocks"));
    CHECK(Has(out, "lost to the window 1"));
}

void TestVerifyNoWindows() {
    Glue g(Cfg("2", "0", nullptr, "off"));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "NO WINDOWS"));
    CHECK(Has(out, "ignoring window 1 blocks"));
    CHECK(Has(out, "with window 0 blocks"));
}

void TestWriteDemotes() {
    Glue g(Cfg("2", "0", nullptr, kWindows));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnWriteFault(kCpuWin + 5);     // a write to the predicted CPU_ONLY block: demoted
    g.OnWriteFault(kCpuWin + 9);     // already demoted: counted as a write, not as a second demotion
    g.OnWriteFault(kCpuWin + 2 * kMiBytes); // outside every block
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "writes 3"));
    CHECK(Has(out, "untracked 1), CPU_ONLY written 1"));
    CHECK(Has(out, "CPU_ONLY 0 blocks"));
}

void TestApplyPublishAndChoose() {
    Glue g(Cfg("3", "0", nullptr, kWindows));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnAllocObserved(CpuOwner(), kCpuWin + kMiBytes, kMiBytes, 1, -1);
    g.OnAllocObserved(CpuOwner(), kGpuWin, kMiBytes, 1, -1); // placed outside its window: not published
    uint64_t version = 0;
    std::vector<PublishRange> pub = g.PublishList(&version);
    CHECK(pub.size() == 2);
    CHECK(version == 1);
    CHECK(pub.size() == 2 && pub[0].addr == kCpuWin && pub[1].addr == kCpuWin + kMiBytes);

    pub = g.PublishList(&version);
    CHECK(version == 1); // same list, same version

    g.OnGpuBind(kCpuWin + kMiBytes, 16); // the second block turns out GPU-bound: it drops out of the list
    pub = g.PublishList(&version);
    CHECK(pub.size() == 1);
    CHECK(version == 2);

    // Window fit for the native writer: CPU owner, heap none (owner rule CPU_ONLY), aligned to 1 MiB.
    PP::VaWindow w;
    CHECK(g.ChooseWindow(CpuOwner(), -1, 2 * kMiBytes, kMiBytes, &w));
    CHECK(w.cls == PP::Class::CpuOnly && w.base == kCpuWin && w.size == kGiBytes);
    CHECK(!g.ChooseWindow(CpuOwner(), -1, 2 * kGiBytes, kMiBytes, &w)); // larger than the window
    CHECK(!g.ChooseWindow(CpuOwner(), -1, kMiBytes, 3 * kMiBytes, &w)); // alignment not a power of two
    CHECK(!g.ChooseWindow(PageOwner(), -1, kMiBytes, kMiBytes, &w));    // no rule: Unknown, no window
    CHECK(g.ChooseWindow(GpuOwner(), -1, kMiBytes, kMiBytes, &w) && w.cls == PP::Class::GpuDynamic);

    // Verify and observe never choose a window.
    Glue v(Cfg("2", "0", nullptr, kWindows));
    CHECK(!v.ChooseWindow(CpuOwner(), -1, kMiBytes, kMiBytes, &w));
    uint64_t vv = 0;
    CHECK(v.PublishList(&vv).empty());
}

void TestRecycleQuarantine() {
    Glue g(Cfg("0", "1", "10", kWindows));
    g.OnFrameBegin(0);
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnFreeObserved(kCpuWin, kMiBytes);
    g.OnFlushDone(Unmapped(true));
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "retired 1 runs"));
    CHECK(Has(out, "held now 1 runs"));

    g.OnFrameBegin(100);
    g.OnSubmitFence(100);
    g.OnCachesInvalidated(kCpuWin, kMiBytes);
    uint64_t va = 0;
    std::string why;
    CHECK(!g.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why)); // quarantine mode never takes
    CHECK(Has(why, "would reuse"));
    out.clear();
    g.Report(out, 5.0);
    CHECK(Has(out, "would reuse 1"));
    CHECK(Has(out, "taken 0"));
}

void TestRecycleGates() {
    Glue g(Cfg("0", "2", "10", kWindows));
    g.OnFrameBegin(20);
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnFreeObserved(kCpuWin, kMiBytes);
    g.OnFlushDone(Unmapped(true)); // retired at frame 20
    uint64_t va = 0;
    std::string why;

    g.OnFrameBegin(25); // quarantine: 25 - 20 < 10
    CHECK(!g.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why));
    CHECK(Has(why, "quarantine"));

    g.OnFrameBegin(40); // GPU fence: no submit finished past frame 20
    CHECK(!g.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why));
    CHECK(Has(why, "gpu"));

    g.OnSubmitFence(30); // the GPU is past the retire frame, but the caches have not dropped the run
    CHECK(!g.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why));
    CHECK(Has(why, "invalid"));

    g.OnCachesInvalidated(kCpuWin, kMiBytes);
    CHECK(g.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why));
    CHECK(va == kCpuWin);
    // The run is consumed: a second take of the same class has nothing to hand out.
    CHECK(!g.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why));
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "taken 1 (1.0 MiB)"));
    CHECK(Has(out, "takes 5 hit 1"));
    CHECK(Has(out, "held now 0 runs"));
}

void TestRecycleRefusals() {
    Glue g(Cfg("1", "1", "0", kWindows)); // observe, so the frees line is printed next to the recycle lines
    g.OnFrameBegin(0);
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnFreeObserved(kCpuWin, 2 * kMiBytes);  // size differs from the recorded block: never retired
    g.OnFreeObserved(kCpuWin, kMiBytes);            // block already gone from the record: unobserved
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnFreeObserved(kCpuWin, kMiBytes);            // flush does not confirm the unmap: refused, not retired
    g.OnFlushDone(Unmapped(false));
    g.OnAllocObserved(GpuOwner(), kGpuWin + kMiBytes, kMiBytes, 1, -1);
    g.OnFreeObserved(kGpuWin + kMiBytes, kMiBytes);       // retired into GPU_DYNAMIC (window class)
    g.OnFlushDone(Unmapped(true));
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "size mismatch frees 1"));
    CHECK(Has(out, "retired 1 runs"));
    CHECK(Has(out, "retire 1, quarantine 0"));
    CHECK(Has(out, "frees 4 (unobserved 1"));

    // Recycler off: frees never retire, and TakeRecycled says so.
    Glue off(Cfg("1", "0", nullptr, kWindows));
    off.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    off.OnFreeObserved(kCpuWin, kMiBytes);
    off.OnFlushDone(Unmapped(true));
    uint64_t va = 0;
    std::string why;
    CHECK(!off.TakeRecycled(PP::Class::CpuOnly, kMiBytes, 0, &va, &why));
    CHECK(Has(why, "recycler off"));
}

void TestDuplicateAndUnknownCalls() {
    Glue g(Cfg("1", "0"));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1); // same VA again without a free
    CHECK(g.blocks() == 1);
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "dup 1"));
    g.OnAllocObserved(CpuOwner(), kCpuWin + kMiBytes, 0, 1, -1); // zero size: ignored
    CHECK(g.blocks() == 1);
    g.OnFreeObserved(0x1234, kMiBytes);  // never seen: unobserved, no crash
    g.OnSubmitFence(5);
    g.OnFrameBegin(9);
    CHECK(g.blocks() == 1);
}

void TestPublishRangesKeepSet() {
    Glue g(Cfg("3", "0", nullptr, kWindows));
    g.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 2, -1);
    uint64_t version = 0;
    std::vector<PublishRange> pub = g.PublishList(&version);
    CHECK(pub.size() == 1 && pub[0].set == 2 && pub[0].size == kMiBytes);
}

// A free is pending until the batch flush: nothing is retired before OnFlushDone, and an interior carve address
// resolves to the block that holds it.
void TestPendingUntilFlush() {
    Glue g(Cfg("1", "1", "0", kWindows));
    g.OnAllocObserved(CpuOwner(), kCpuWin, 4 * kMiBytes, 1, -1);
    g.OnHeapCarve(kCpuWin + 2 * kMiBytes, 1); // interior address: the block is found, the carve is counted
    g.OnFreeObserved(kCpuWin, 4 * kMiBytes);
    std::string out;
    g.Report(out, 5.0);
    CHECK(Has(out, "retired 0 runs"));
    CHECK(Has(out, "carves 1 (unruled 1, untracked 0)")); // slot 1 has no rule in this glue
    g.OnFlushDone(Unmapped(true));
    out.clear();
    g.Report(out, 5.0);
    CHECK(Has(out, "retired 1 runs"));
}

} // namespace

// ---- auto windows and the stage 3 window fit (added with the auto plan) ----------------------------------

const PP::VaWindow* FindWindow(const std::vector<PP::VaWindow>& ws, uint64_t base) {
    for (const PP::VaWindow& w : ws) {
        if (w.base == base) {
            return &w;
        }
    }
    return nullptr;
}

void TestAutoWindowsCarved() {
    // First observed block (S1, pristine by default) fixes the plan; the expected shares are the kPlans table:
    // S0 406 MiB CPU; S1 1024 MiB: CPU 896, GPU_WRITTEN 128; S2 8124 MiB: GPU_STATIC 5524, GPU_DYNAMIC 1299,
    // GPU_WRITTEN 812, CPU_ONLY 487 (per-mille, rounded down to MiB).
    Glue g(Cfg("2", "0"));
    CHECK(g.warnings().empty());
    g.OnAllocObserved(CpuOwner(), 0x1000300000ull, kMiBytes, 1, -1);
    const std::vector<PP::VaWindow> ws = g.Windows();
    CHECK(ws.size() == 7);
    for (size_t i = 1; i < ws.size(); ++i) {
        CHECK(ws[i - 1].base < ws[i].base); // base order, no overlaps (WindowMap::Set validated them)
    }
    const PP::VaWindow* s0 = FindWindow(ws, 0x123c000000ull);
    CHECK(s0 && s0->cls == PP::Class::CpuOnly && s0->size == 406 * kMiBytes);
    const PP::VaWindow* s1cpu = FindWindow(ws, 0x1000300000ull);
    CHECK(s1cpu && s1cpu->cls == PP::Class::CpuOnly && s1cpu->size == 896 * kMiBytes);
    const PP::VaWindow* s1gw = FindWindow(ws, 0x1000300000ull + 896 * kMiBytes);
    CHECK(s1gw && s1gw->cls == PP::Class::GpuWritten && s1gw->size == 128 * kMiBytes);
    const uint64_t s2 = 0x1040300000ull;
    const PP::VaWindow* gs = FindWindow(ws, s2);
    CHECK(gs && gs->cls == PP::Class::GpuStatic && gs->size == 5524 * kMiBytes);
    const PP::VaWindow* gd = FindWindow(ws, s2 + 5524 * kMiBytes);
    CHECK(gd && gd->cls == PP::Class::GpuDynamic && gd->size == 1299 * kMiBytes);
    const PP::VaWindow* gw = FindWindow(ws, s2 + (5524 + 1299) * kMiBytes);
    CHECK(gw && gw->cls == PP::Class::GpuWritten && gw->size == 812 * kMiBytes);
    const PP::VaWindow* sc = FindWindow(ws, s2 + (5524 + 1299 + 812) * kMiBytes);
    CHECK(sc && sc->cls == PP::Class::CpuOnly && sc->size == 487 * kMiBytes);
    std::string out;
    g.Report(out, 1.0);
    CHECK(Has(out, "SOTC PC PLACE windows (auto): 7"));
    CHECK(Has(out, "SOTC PC PLACE window 0x1000300000-"));
    CHECK(Has(out, "blocks 1 (1.0 MiB live)")); // the one block sits in the S1 CPU window
}

void TestAutoWindowsPlanIsFixed() {
    // The plan is carved once: a layout set after the first block changes nothing, and nothing is carved when the
    // windows are off or explicit.
    Glue g(Cfg("2", "0"));
    g.OnAllocObserved(CpuOwner(), 0x1000300000ull, kMiBytes, 1, -1);
    const size_t before = g.Windows().size();
    const PoolSetLayout late[3] = {{0x123c000000ull, kMiBytes, true}, {0x1000300000ull, kMiBytes, true}, {0x1040300000ull, kMiBytes, true}};
    g.SetPoolLayout(late);
    CHECK(g.Windows().size() == before && before == 7);

    Glue off(Cfg("2", "0", nullptr, "off"));
    off.OnAllocObserved(CpuOwner(), 0x1000300000ull, kMiBytes, 1, -1);
    CHECK(off.Windows().empty());

    Glue explicit_w(Cfg("2", "0", nullptr, kWindows));
    explicit_w.OnAllocObserved(CpuOwner(), kCpuWin, kMiBytes, 1, -1);
    CHECK(explicit_w.Windows().size() == 2);
}

void TestAutoWindowsNotPristine() {
    // A set that is not pristine gets no windows; the others are carved; the note says why.
    Glue g(Cfg("2", "0"));
    PoolSetLayout sets[3] = {kDefaultPoolSets[0], kDefaultPoolSets[1], kDefaultPoolSets[2]};
    sets[2].pristine = false;
    g.SetPoolLayout(sets);
    g.OnAllocObserved(CpuOwner(), 0x1000300000ull, kMiBytes, 1, -1);
    const std::vector<PP::VaWindow> ws = g.Windows();
    CHECK(ws.size() == 3); // S0 one, S1 two; S2 none
    for (const PP::VaWindow& w : ws) {
        CHECK(!(w.base >= 0x1040300000ull && w.base < 0x123bfe0000ull)); // nothing inside S2
    }
    std::string out;
    g.Report(out, 1.0);
    CHECK(Has(out, "S2 not pristine, no windows"));
}

void TestAutoWindowsWindowsFor() {
    // Apply mode: WindowsFor gives every window of the decided class, in base order; Unknown gives none.
    Glue g(Cfg("3", "0"));
    const std::vector<PP::VaWindow> cpu = g.WindowsFor(CpuOwner(), -1);
    // CPU_ONLY: S1's CPU window, the small CPU spill window in S2, then S0 (the kind 1 search order is not the list order).
    CHECK(cpu.size() == 3 && cpu[0].base == 0x1000300000ull && cpu[2].base == 0x123c000000ull);
    CHECK(cpu.size() == 3 && cpu[1].base == 0x1040300000ull + (5524 + 1299 + 812) * kMiBytes);
    const std::vector<PP::VaWindow> gpu = g.WindowsFor(GpuOwner(), -1);
    CHECK(gpu.size() == 1 && gpu[0].cls == PP::Class::GpuDynamic);
    CHECK(g.WindowsFor(PageOwner(), -1).empty());
    Glue observe(Cfg("1", "0"));
    CHECK(observe.WindowsFor(CpuOwner(), -1).empty()); // not Apply: no windows for the writer
}

// The window fit (FitInWindow, stage 3). Free runs of S1 with A = 1 GiB aligned:
//   run0 [A, A+3 MiB), run1 [A+8 MiB, A+12 MiB), all backed (backed = size), free VA pages ample.
void TestWindowFit() {
    namespace M = SotcPcPool::Model;
    const uint64_t A = 0x1040000000ull;
    M::Pools pre;
    M::Set& s = pre.set[1];
    s.free_va_pages = 1 << 20;
    s.free_va.push_back(M::Run{A, 3 * kMiBytes, 3 * kMiBytes, 0});
    s.free_va.push_back(M::Run{A + 8 * kMiBytes, 4 * kMiBytes, 4 * kMiBytes, 0});
    WindowFit f;

    // (a) the window is run0 exactly and the request is run0 whole: exact.
    CHECK(FitInWindow(pre, 1, PP::VaWindow{A, 3 * kMiBytes, PP::Class::CpuOnly}, 3 * kMiBytes, kMiBytes, &f));
    CHECK(f.run == 0 && f.start == A && f.exact);

    // (b) a request smaller than the backed run splits it: found, not exact.
    CHECK(FitInWindow(pre, 1, PP::VaWindow{A, 3 * kMiBytes, PP::Class::CpuOnly}, kMiBytes, kMiBytes, &f));
    CHECK(f.run == 0 && f.start == A && !f.exact);

    // (c) the window cuts run0 at its end: the block starts inside the run (a pad), not exact.
    CHECK(FitInWindow(pre, 1, PP::VaWindow{A + 2 * kMiBytes, 2 * kMiBytes, PP::Class::CpuOnly}, kMiBytes, kMiBytes, &f));
    CHECK(f.run == 0 && f.start == A + 2 * kMiBytes && !f.exact);

    // (d) alignment 2 MiB: run0 has only 1 MiB left from the aligned start, so run1 is taken, and it is aligned.
    const PP::VaWindow wide{A + kMiBytes, 12 * kMiBytes, PP::Class::CpuOnly};
    CHECK(FitInWindow(pre, 1, wide, 2 * kMiBytes, 2 * kMiBytes, &f));
    CHECK(f.run == 1 && f.start == A + 8 * kMiBytes && !f.exact);

    // (e) the window misses every run: false.
    CHECK(!FitInWindow(pre, 1, PP::VaWindow{A + 20 * kMiBytes, 4 * kMiBytes, PP::Class::CpuOnly}, kMiBytes, kMiBytes, &f));

    // (f) the window is smaller than the request: false.
    CHECK(!FitInWindow(pre, 1, PP::VaWindow{A, kMiBytes, PP::Class::CpuOnly}, 2 * kMiBytes, kMiBytes, &f));

    // (g) the pool's own page test: not enough free VA pages in the set: false.
    M::Pools few = pre;
    few.set[1].free_va_pages = 1;
    CHECK(!FitInWindow(few, 1, PP::VaWindow{A, 3 * kMiBytes, PP::Class::CpuOnly}, 2 * kMiBytes, kMiBytes, &f));

    // (h) malformed calls: a size that is not 64 KiB, a zero size, an align that is not a power of two, a bad set.
    CHECK(!FitInWindow(pre, 1, wide, 0x18000, kMiBytes, &f));
    CHECK(!FitInWindow(pre, 1, wide, 0, kMiBytes, &f));
    CHECK(!FitInWindow(pre, 1, wide, kMiBytes, 3 * kMiBytes, &f));
    CHECK(!FitInWindow(pre, 3, wide, kMiBytes, kMiBytes, &f));

    // (i) the set of the window matters: the same runs in S2 (set 2 is empty) do not fit.
    CHECK(!FitInWindow(pre, 2, wide, kMiBytes, kMiBytes, &f));
}

int main() {
    TestConfig();
    TestAutoWindowsCarved();
    TestAutoWindowsPlanIsFixed();
    TestAutoWindowsNotPristine();
    TestAutoWindowsWindowsFor();
    TestWindowFit();
    TestWindowSpec();
    TestOffIsInert();
    TestAddRule();
    TestObserveClassAndBind();
    TestHeapContextAndDisagreement();
    TestHeapScopeNesting();
    TestLedger();
    TestVerifyWindows();
    TestVerifyNoWindows();
    TestWriteDemotes();
    TestApplyPublishAndChoose();
    TestRecycleQuarantine();
    TestRecycleGates();
    TestRecycleRefusals();
    TestDuplicateAndUnknownCalls();
    TestPublishRangesKeepSet();
    TestPendingUntilFlush();

    if (g_failures == 0) {
        std::printf("PASS pc_pool_place_glue_test: %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAIL pc_pool_place_glue_test: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
