// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: pc-pool-place-test. Unit tests of gpu/sotc/pc_pool_place.cpp (the placement decision core): the class decision
// of a request, the VA windows, the block ledger and the VA recycler's safety rules.
//
// Build and run (scratch directory, not the repo):
//   C:/msys64/clang64/bin/clang++.exe -std=c++20 -Wall -Wextra -g -I gpu/sotc gpu/sotc/tests/pc_pool_place_test.cpp gpu/sotc/pc_pool_place.cpp -o pc_pool_place_test.exe
//   ./pc_pool_place_test.exe   (prints PASS or FAIL; exit code 0 on PASS)

#include "../pc_pool_place.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace SotcPcPlace;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

constexpr uint64_t kMiBytes = 0x100000;
constexpr uint64_t kGiBytes = 0x40000000;
constexpr uint64_t kPage = 0x10000;
constexpr uint64_t kBase = 0x1040300000; // the Garlic set's VA (reports/04): 1 MiB aligned, inside the pool range

// The owners the probe names (reports3/06 section 6, PC-MEMORY-REWRITE.md section 6).
constexpr uint64_t kOwnerCpuMemory = 0x2dd9f80;   // family object, kind 1
constexpr uint64_t kOwnerGpuMemory = 0x2dd9f84;   // family object, kind 2
constexpr uint64_t kOwnerTaggedGpu = 0x2dd9a00;   // Default TaggedArena, kind 2
constexpr uint64_t kOwnerPageCpu = 0x2dd9b00;     // PageAllocator, kind 1

ClassTable DefaultTable() {
    ClassTable t;
    std::string why;
    // Owner rules (name and kind; the names repeat across kinds).
    CHECK(t.Add(Rule{Rule::Match::NameKind, 0, "CPUMemory", 1, -1, Class::CpuOnly, "CPUMemory"}, &why));
    CHECK(t.Add(Rule{Rule::Match::NameKind, 0, "GPUMemory", 2, -1, Class::GpuDynamic, "GPUMemory"}, &why));
    CHECK(t.Add(Rule{Rule::Match::NameKind, 0, "Default TaggedArena", 2, -1, Class::GpuDynamic, "TaggedArena GPU"}, &why));
    CHECK(t.Add(Rule{Rule::Match::NameKind, 0, "RTAllocator", 2, -1, Class::GpuWritten, "RTAllocator"}, &why));
    // Heap rules (memtype slots; the values are per run, these are the examples of reports3/05).
    CHECK(t.Add(Rule{Rule::Match::Heap, 0, "", 0, 16, Class::GpuDynamic, "RenderSingleFrameGPU"}, &why));
    CHECK(t.Add(Rule{Rule::Match::Heap, 0, "", 0, 17, Class::GpuWritten, "RenderSingleFrameCPU"}, &why));
    CHECK(t.Add(Rule{Rule::Match::Heap, 0, "", 0, 1, Class::CpuOnly, "GameSingleFrame"}, &why));
    return t;
}

void TestParseMode() {
    bool bad = false;
    CHECK(ParseMode("0", &bad) == Mode::Off && !bad);
    CHECK(ParseMode("off", &bad) == Mode::Off && !bad);
    CHECK(ParseMode("1", &bad) == Mode::Observe && !bad);
    CHECK(ParseMode("verify", &bad) == Mode::Verify && !bad);
    CHECK(ParseMode("3", &bad) == Mode::Apply && !bad);
    CHECK(ParseMode("", &bad) == Mode::Off && !bad);
    CHECK(ParseMode("2x", &bad) == Mode::Off && bad);
    CHECK(ParseMode(nullptr, nullptr) == Mode::Off);
}

void TestTableValidation() {
    ClassTable t;
    std::string why;
    CHECK(!t.Add(Rule{Rule::Match::Addr, 0, "", 0, -1, Class::CpuOnly, ""}, &why));
    CHECK(!t.Add(Rule{Rule::Match::NameKind, 0, "", 1, -1, Class::CpuOnly, ""}, &why));
    CHECK(!t.Add(Rule{Rule::Match::Heap, 0, "", 0, -1, Class::CpuOnly, ""}, &why));
    CHECK(!t.Add(Rule{Rule::Match::Heap, 0, "", 0, 3, static_cast<Class>(9), ""}, &why));
    CHECK(t.size() == 0);
    CHECK(t.Add(Rule{Rule::Match::Addr, kOwnerCpuMemory, "", 0, -1, Class::CpuOnly, ""}, &why));
    CHECK(t.size() == 1);
}

void TestDecideNoRule() {
    ClassTable t;
    Request r;
    r.owner = OwnerKey{0, "", 0};
    Decision d = Decide(r, t);
    CHECK(d.place == Class::Unknown && d.publish == Class::Unknown);
    CHECK(!d.owner_matched && !d.heap_matched);
    CHECK(std::string(d.reason) == kReasonNoRule);
}

void TestDecideOwnerKind() {
    ClassTable t = DefaultTable();
    // The same name in two kinds: kind decides.
    Request cpu;
    cpu.owner = OwnerKey{kOwnerCpuMemory, "CPUMemory", 1};
    Decision d = Decide(cpu, t);
    CHECK(d.place == Class::CpuOnly && d.publish == Class::CpuOnly);
    CHECK(std::string(d.reason) == kReasonOwnerOnly);

    Request wrongkind;
    wrongkind.owner = OwnerKey{0, "CPUMemory", 2};
    d = Decide(wrongkind, t);
    CHECK(d.place == Class::Unknown && d.publish == Class::Unknown);
    CHECK(!d.owner_matched);

    Request tagged;
    tagged.owner = OwnerKey{kOwnerTaggedGpu, "Default TaggedArena", 2};
    d = Decide(tagged, t);
    CHECK(d.place == Class::GpuDynamic && d.publish == Class::GpuDynamic);
    CHECK(std::string(d.reason) == kReasonOwnerOnly);
}

void TestDecideOwnerAddrBeatsName() {
    ClassTable t = DefaultTable();
    std::string why;
    // An exact owner rule overrides the name rule of the same name and kind.
    CHECK(t.Add(Rule{Rule::Match::Addr, kOwnerPageCpu, "", 0, -1, Class::GpuWritten, "PageAllocator (GPU reads)"}, &why));
    Request r;
    r.owner = OwnerKey{kOwnerPageCpu, "PageAllocator", 1};
    Decision d = Decide(r, t);
    CHECK(d.owner_class == Class::GpuWritten && d.place == Class::GpuWritten);
    // The address of an owner that is not in the table falls back to nothing (no name rule for PageAllocator).
    r.owner = OwnerKey{kOwnerGpuMemory + 0x1000, "PageAllocator", 1};
    d = Decide(r, t);
    CHECK(d.place == Class::Unknown && !d.owner_matched);
}

void TestDecideHeapOverridesOwner() {
    ClassTable t = DefaultTable();
    // RenderSingleFrameCPU (slot 17) lives in the CPUMemory owner, and the GPU writes it (reports3/05 section 3).
    Request r;
    r.owner = OwnerKey{kOwnerCpuMemory, "CPUMemory", 1};
    r.heap_slot = 17;
    Decision d = Decide(r, t);
    CHECK(d.place == Class::GpuWritten);
    CHECK(d.publish == Class::Unknown);
    CHECK(std::string(d.reason) == kReasonDisagree);

    // A CPU heap inside the CPU owner agrees, and is CPU_ONLY.
    r.heap_slot = 1;
    d = Decide(r, t);
    CHECK(d.place == Class::CpuOnly && d.publish == Class::CpuOnly);
    CHECK(std::string(d.reason) == kReasonAgree);

    // A slot with no heap rule falls back to the owner.
    r.heap_slot = 22;
    d = Decide(r, t);
    CHECK(!d.heap_matched && d.place == Class::CpuOnly && d.publish == Class::CpuOnly);

    // The sentinel -1 means no heap context.
    r.heap_slot = -1;
    d = Decide(r, t);
    CHECK(!d.heap_matched && d.publish == Class::CpuOnly);
}

void TestDecideHeapOnlyNeverCpuPublish() {
    ClassTable t = DefaultTable();
    // Heap rule CPU, owner unknown: placed CPU, never published as CPU_ONLY.
    Request r;
    r.owner = OwnerKey{0, "", 0};
    r.heap_slot = 1;
    Decision d = Decide(r, t);
    CHECK(d.place == Class::CpuOnly);
    CHECK(d.publish == Class::Unknown);
    // Heap rule GPU, owner unknown: publish keeps the GPU class (placement and publication agree).
    r.heap_slot = 16;
    d = Decide(r, t);
    CHECK(d.place == Class::GpuDynamic && d.publish == Class::GpuDynamic);
}

void TestDecideUnknownRule() {
    ClassTable t;
    std::string why;
    CHECK(t.Add(Rule{Rule::Match::Addr, kOwnerCpuMemory, "", 0, -1, Class::Unknown, "explicit unknown"}, &why));
    Request r;
    r.owner = OwnerKey{kOwnerCpuMemory, "CPUMemory", 1};
    Decision d = Decide(r, t);
    CHECK(d.place == Class::Unknown && d.publish == Class::Unknown);
}

void TestWindowsCarve() {
    std::string why;
    auto parts = CarveWindows(kBase, 4 * kGiBytes,
                              {{Class::CpuOnly, kGiBytes}, {Class::GpuStatic, kGiBytes},
                               {Class::GpuDynamic, 512 * kMiBytes}, {Class::GpuWritten, kGiBytes}},
                              &why);
    CHECK(parts.size() == 4);
    CHECK(why.empty());
    CHECK(parts[1].base == kBase + kGiBytes && parts[1].size == kGiBytes);
    WindowMap m;
    CHECK(m.Set(parts, &why));
    CHECK(m.ClassOf(kBase) == Class::CpuOnly);
    CHECK(m.ClassOf(kBase + kGiBytes - 1) == Class::CpuOnly);
    CHECK(m.ClassOf(kBase + kGiBytes) == Class::GpuStatic);
    CHECK(m.ClassOf(kBase + 2 * kGiBytes + 512 * kMiBytes) == Class::GpuWritten);
    // The last byte of the GpuWritten window, then the unwindowed tail of the 4 GiB range (the parts sum to 3.5 GiB).
    CHECK(m.ClassOf(kBase + 3 * kGiBytes + 512 * kMiBytes - 1) == Class::GpuWritten);
    CHECK(m.ClassOf(kBase + 3 * kGiBytes + 512 * kMiBytes) == Class::Unknown);
    CHECK(m.ClassOf(kBase + 4 * kGiBytes - 1) == Class::Unknown);
    CHECK(m.ClassOf(kBase - 1) == Class::Unknown);
    CHECK(m.ClassOf(0) == Class::Unknown);
}

void TestWindowsCarveRejects() {
    std::string why;
    CHECK(CarveWindows(kBase + 0x10000, kGiBytes, {{Class::CpuOnly, kMiBytes}}, &why).empty());
    CHECK(CarveWindows(kBase, kGiBytes, {{Class::CpuOnly, 0}}, &why).empty());
    CHECK(CarveWindows(kBase, kGiBytes, {{Class::CpuOnly, kMiBytes + 1}}, &why).empty());
    CHECK(CarveWindows(kBase, kMiBytes, {{Class::CpuOnly, kMiBytes}, {Class::GpuStatic, kMiBytes}}, &why).empty());
    CHECK(CarveWindows(kBase, kMiBytes, {{Class::Unknown, kMiBytes}}, &why).empty());
    CHECK(!why.empty());
}

void TestWindowsSetRejects() {
    std::string why;
    WindowMap m;
    CHECK(!m.Set({{kBase + 4096, kMiBytes, Class::CpuOnly}}, &why));           // unaligned base
    CHECK(!m.Set({{kBase, kMiBytes + kPage, Class::CpuOnly}}, &why));          // unaligned size
    CHECK(!m.Set({{kBase, 0, Class::CpuOnly}}, &why));                         // empty
    CHECK(!m.Set({{kVaFloor - kMiBytes, kMiBytes, Class::CpuOnly}}, &why));    // below the pool range
    CHECK(!m.Set({{kVaCeiling - kMiBytes, 2 * kMiBytes, Class::CpuOnly}}, &why)); // crosses the ceiling
    CHECK(!m.Set({{kBase, kMiBytes, Class::Unknown}}, &why));                  // not a placement class
    CHECK(!m.Set({{kBase, 2 * kMiBytes, Class::CpuOnly}, {kBase + kMiBytes, kMiBytes, Class::GpuStatic}}, &why)); // overlap
    // A valid set replaces the empty one; a rejected set keeps the old map.
    CHECK(m.Set({{kBase, kMiBytes, Class::CpuOnly}}, &why));
    CHECK(!m.Set({{kBase, 0, Class::GpuStatic}}, &why));
    CHECK(m.ClassOf(kBase) == Class::CpuOnly);
    // Order of the input does not matter.
    CHECK(m.Set({{kBase + 2 * kMiBytes, kMiBytes, Class::GpuStatic}, {kBase, kMiBytes, Class::CpuOnly}}, &why));
    CHECK(m.ClassOf(kBase + 2 * kMiBytes) == Class::GpuStatic);
    CHECK(m.ClassOf(kBase + kMiBytes) == Class::Unknown);
}

void TestEffective() {
    Decision cpu;
    cpu.publish = Class::CpuOnly;
    cpu.place = Class::CpuOnly;
    CHECK(Effective(cpu, Class::CpuOnly, false) == Class::CpuOnly);
    CHECK(Effective(cpu, Class::GpuWritten, false) == Class::Unknown); // placed outside its window
    CHECK(Effective(cpu, Class::Unknown, false) == Class::Unknown);    // no window at all
    CHECK(Effective(cpu, Class::CpuOnly, true) == Class::Unknown);     // GPU bound: demoted

    Decision gpu;
    gpu.publish = Class::GpuWritten;
    gpu.place = Class::GpuWritten;
    CHECK(Effective(gpu, Class::GpuWritten, true) == Class::GpuWritten); // binding a GPU block is expected
    CHECK(Effective(gpu, Class::GpuStatic, false) == Class::Unknown);

    Decision unk;
    CHECK(Effective(unk, Class::CpuOnly, false) == Class::Unknown);
}

void TestBlockLedger() {
    BlockLedger l;
    l.NoteCarve(kBase, Class::CpuOnly);
    l.NoteCarve(kBase, Class::CpuOnly);
    CHECK(l.Get(kBase) == Class::CpuOnly && !l.Conflicted(kBase));
    l.NoteCarve(kBase, Class::GpuDynamic); // a second heap of another class: sticky
    CHECK(l.Get(kBase) == Class::Unknown && l.Conflicted(kBase));
    l.NoteCarve(kBase, Class::CpuOnly);    // still conflicted
    CHECK(l.Get(kBase) == Class::Unknown && l.Conflicted(kBase));
    l.Forget(kBase);
    CHECK(l.Get(kBase) == Class::Unknown && !l.Conflicted(kBase));
    CHECK(l.size() == 0);

    // A carve by an Unknown heap makes the block Unknown from the start.
    l.NoteCarve(kBase + kMiBytes, Class::Unknown);
    CHECK(l.Get(kBase + kMiBytes) == Class::Unknown && l.Conflicted(kBase + kMiBytes));
    // Blocks are independent.
    l.NoteCarve(kBase + 2 * kMiBytes, Class::GpuWritten);
    CHECK(l.Get(kBase + 2 * kMiBytes) == Class::GpuWritten);
    CHECK(l.Get(kBase + kMiBytes) == Class::Unknown);
}

void TestRecyclerOff() {
    VaRecycler r(VaRecycler::Mode::Off, 4);
    std::string why;
    CHECK(!r.Retire(kBase, kPage, Class::GpuStatic, 1, true, &why));
    uint64_t va = 0;
    CHECK(!r.Take(Class::GpuStatic, kPage, 0, 100, 100, &va, &why));
    CHECK(r.runs() == 0 && r.stats().retired == 0);
}

void TestRecyclerRetireRefusals() {
    VaRecycler r(VaRecycler::Mode::Recycle, 4);
    std::string why;
    CHECK(!r.Retire(kBase, kPage, Class::GpuStatic, 1, false, &why));          // not confirmed unmapped
    CHECK(!r.Retire(kBase + 8, kPage, Class::GpuStatic, 1, true, &why));       // unaligned VA
    CHECK(!r.Retire(kBase, kPage + 8, Class::GpuStatic, 1, true, &why));       // unaligned size
    CHECK(!r.Retire(kBase, 0, Class::GpuStatic, 1, true, &why));               // empty
    CHECK(!r.Retire(kBase, kPage, Class::Unknown, 1, true, &why));             // no window class
    CHECK(r.stats().refused_retire == 5);
    CHECK(r.Retire(kBase, 4 * kPage, Class::GpuStatic, 1, true, &why));
    CHECK(!r.Retire(kBase, kPage, Class::GpuStatic, 1, true, &why));          // double free
    CHECK(!r.Retire(kBase + 3 * kPage, 2 * kPage, Class::GpuStatic, 1, true, &why)); // overlaps the tail
    CHECK(!r.Retire(kBase - kPage, 2 * kPage, Class::GpuStatic, 1, true, &why));    // overlaps the head
    CHECK(r.Retire(kBase + 4 * kPage, kPage, Class::GpuStatic, 1, true, &why));     // adjacent: fine
    CHECK(r.stats().refused_retire == 8);
}

void TestRecyclerSafetyRules() {
    VaRecycler r(VaRecycler::Mode::Recycle, 4);
    std::string why;
    CHECK(r.Retire(kBase, 4 * kPage, Class::GpuDynamic, 10, true, &why));
    uint64_t va = 0;

    // 1. Quarantine: frame 13 is 3 frames after the retire, the quarantine is 4.
    CHECK(!r.Take(Class::GpuDynamic, kPage, 0, 13, 100, &va, &why));
    CHECK(why == "quarantine");
    CHECK(r.stats().refused_quarantine == 1);
    // A clock that went backwards is quarantine too.
    CHECK(!r.Take(Class::GpuDynamic, kPage, 0, 5, 100, &va, &why));

    // 2. GPU fence: the GPU finished only frame 9, before the retire frame 10.
    r.MarkInvalidated(kBase, 4 * kPage);
    CHECK(!r.Take(Class::GpuDynamic, kPage, 0, 20, 9, &va, &why));
    CHECK(why == "gpu");
    CHECK(r.stats().refused_gpu == 1);

    // 3. Once the quarantine is over, the GPU fence passed and the run is invalidated, the run is ready.
    CHECK(r.Take(Class::GpuDynamic, kPage, 0, 20, 10, &va, &why));
    CHECK(va == kBase);
    CHECK(r.stats().taken == 1 && r.stats().taken_bytes == kPage);
}

void TestRecyclerInvalidationPartial() {
    VaRecycler r(VaRecycler::Mode::Recycle, 0);
    std::string why;
    CHECK(r.Retire(kBase, 4 * kPage, Class::GpuStatic, 1, true, &why));
    // A range covering only the first half does not mark the run (the rest is still cached).
    r.MarkInvalidated(kBase, 2 * kPage);
    uint64_t va = 0;
    CHECK(!r.Take(Class::GpuStatic, kPage, 0, 10, 10, &va, &why));
    CHECK(why == "invalid");
    // A range that covers it wholly marks it.
    r.MarkInvalidated(kBase - kPage, 6 * kPage);
    CHECK(r.Take(Class::GpuStatic, kPage, 0, 10, 10, &va, &why));
}

void TestRecyclerClassPurity() {
    VaRecycler r(VaRecycler::Mode::Recycle, 0);
    std::string why;
    CHECK(r.Retire(kBase, 8 * kPage, Class::CpuOnly, 1, true, &why));
    r.MarkInvalidated(kBase, 8 * kPage);
    uint64_t va = 0;
    CHECK(!r.Take(Class::GpuWritten, kPage, 0, 10, 10, &va, &why));
    CHECK(why == "no retired run of that class is big enough");
    CHECK(!r.Take(Class::GpuStatic, kPage, 0, 10, 10, &va, &why));
    CHECK(r.Take(Class::CpuOnly, kPage, 0, 10, 10, &va, &why));
    CHECK(va == kBase);
}

void TestRecyclerQuarantineMode() {
    VaRecycler r(VaRecycler::Mode::Quarantine, 0);
    std::string why;
    CHECK(r.Retire(kBase, 4 * kPage, Class::GpuStatic, 1, true, &why));
    r.MarkInvalidated(kBase, 4 * kPage);
    uint64_t va = 0;
    CHECK(!r.Take(Class::GpuStatic, kPage, 0, 10, 10, &va, &why));
    CHECK(r.stats().would_reuse == 1 && r.stats().taken == 0);
    // The run is untouched: the same size still fits and nothing was split.
    CHECK(r.runs() == 1);
    CHECK(r.retired_bytes_now() == 4 * kPage);
}

void TestRecyclerAlignmentAndSplit() {
    VaRecycler r(VaRecycler::Mode::Recycle, 0);
    std::string why;
    // A run starting 64 KiB before a 1 MiB boundary.
    const uint64_t start = kBase + 15 * kPage;
    CHECK(r.Retire(start, 32 * kPage, Class::GpuDynamic, 1, true, &why));
    r.MarkInvalidated(start, 32 * kPage);
    uint64_t va = 0;
    // Asking for 1 MiB aligned, 1 MiB long: the aligned start is kBase + 16 * 64 KiB = kBase + 1 MiB.
    CHECK(r.Take(Class::GpuDynamic, kMiBytes, kMiBytes, 10, 10, &va, &why));
    CHECK(va == kBase + kMiBytes);
    // The head [start, va) and the tail [va + 1 MiB, start + 32 pages) stay retired.
    CHECK(r.runs() == 2);
    CHECK(r.retired_bytes_now() == 32 * kPage - kMiBytes);
    CHECK(r.stats().taken_bytes == kMiBytes);

    // Conservation: retired bytes minus taken bytes equals what is still retired.
    CHECK(r.stats().retired_bytes - r.stats().taken_bytes == r.retired_bytes_now());

    // A request that does not fit the head or the tail is refused with the size reason.
    CHECK(!r.Take(Class::GpuDynamic, 2 * kMiBytes, 0, 10, 10, &va, &why));
    // Bad alignments and sizes are rejected without state change.
    CHECK(!r.Take(Class::GpuDynamic, kPage, 3 * kPage, 10, 10, &va, &why));
    CHECK(!r.Take(Class::GpuDynamic, kPage + 1, 0, 10, 10, &va, &why));
    CHECK(r.retired_bytes_now() == 32 * kPage - kMiBytes);
}

void TestRecyclerCoalesce() {
    VaRecycler r(VaRecycler::Mode::Recycle, 0);
    std::string why;
    CHECK(r.Retire(kBase, 4 * kPage, Class::GpuWritten, 1, true, &why));
    CHECK(r.Retire(kBase + 4 * kPage, 4 * kPage, Class::GpuWritten, 2, true, &why));
    CHECK(r.runs() == 1);
    CHECK(r.stats().merged == 1);
    // Coalescing does not join a different class.
    CHECK(r.Retire(kBase + 8 * kPage, 4 * kPage, Class::CpuOnly, 2, true, &why));
    CHECK(r.runs() == 2);
    // The merged run waits for the later retire frame (2) and needs invalidation of the whole merged run.
    r.MarkInvalidated(kBase, 4 * kPage);
    uint64_t va = 0;
    CHECK(!r.Take(Class::GpuWritten, kPage, 0, 10, 10, &va, &why));
    CHECK(why == "invalid");
    r.MarkInvalidated(kBase, 8 * kPage);
    CHECK(r.Take(Class::GpuWritten, 8 * kPage, 0, 10, 10, &va, &why));
    CHECK(va == kBase);
    // Frame 2 retire: a take at frame 5 with quarantine 4 is refused (5 - 2 < 4).
    VaRecycler q(VaRecycler::Mode::Recycle, 4);
    CHECK(q.Retire(kBase, 4 * kPage, Class::GpuWritten, 1, true, &why));
    CHECK(q.Retire(kBase + 4 * kPage, 4 * kPage, Class::GpuWritten, 2, true, &why));
    q.MarkInvalidated(kBase, 8 * kPage);
    CHECK(!q.Take(Class::GpuWritten, kPage, 0, 5, 10, &va, &why));
    CHECK(why == "quarantine");
    CHECK(q.Take(Class::GpuWritten, kPage, 0, 6, 10, &va, &why));
}

void TestRecyclerRunCountNoLeak() {
    // Many retire/take rounds: the bytes the recycler holds never exceed what was retired, and the stats balance.
    VaRecycler r(VaRecycler::Mode::Recycle, 2);
    std::string why;
    uint64_t frame = 0;
    uint64_t va = 0;
    for (int i = 0; i < 64; ++i) {
        const uint64_t at = kBase + static_cast<uint64_t>(i) * 4 * kPage;
        CHECK(r.Retire(at, 4 * kPage, Class::GpuStatic, frame, true, &why));
        ++frame;
    }
    // Coalescing merges the 64 runs into one; it is invalidated as a whole (the merge clears the flag per step).
    CHECK(r.retired_bytes_now() == 64 * 4 * kPage);
    CHECK(r.runs() == 1);
    r.MarkInvalidated(kBase, 64 * 4 * kPage);
    int taken = 0;
    for (int i = 0; i < 32; ++i) {
        if (r.Take(Class::GpuStatic, kMiBytes, kMiBytes, frame + 10, frame + 10, &va, &why)) {
            ++taken;
        }
    }
    CHECK(taken == 16); // 16 MiB of retired VA in 1 MiB blocks
    CHECK(r.retired_bytes_now() == 0);
    CHECK(r.stats().retired_bytes - r.stats().taken_bytes == r.retired_bytes_now());
}

} // namespace

int main() {
    TestParseMode();
    TestTableValidation();
    TestDecideNoRule();
    TestDecideOwnerKind();
    TestDecideOwnerAddrBeatsName();
    TestDecideHeapOverridesOwner();
    TestDecideHeapOnlyNeverCpuPublish();
    TestDecideUnknownRule();
    TestWindowsCarve();
    TestWindowsCarveRejects();
    TestWindowsSetRejects();
    TestEffective();
    TestBlockLedger();
    TestRecyclerOff();
    TestRecyclerRetireRefusals();
    TestRecyclerSafetyRules();
    TestRecyclerInvalidationPartial();
    TestRecyclerClassPurity();
    TestRecyclerQuarantineMode();
    TestRecyclerAlignmentAndSplit();
    TestRecyclerCoalesce();
    TestRecyclerRunCountNoLeak();

    if (g_failures == 0) {
        std::printf("PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("FAIL (%d of %d checks)\n", g_failures, g_checks);
    return 1;
}
