// sotc: gpu-region-test (gpu/CMakeLists.txt, EXCLUDE_FROM_ALL). Unit test of the GPU region allocator
// (gpu/sotc/gpu_region.cpp) with fake host calls: no game, no GPU, no runtime. The fake reservation returns
// an address that is not mapped in this process, so any byte the allocator wrote into the region would crash
// the test; the commit records prove that every handed-out byte lies in a committed granule.
// Prints PASS or FAIL; the exit code is 0 on PASS.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gpu_region.h"

// The default host of gpu_region.cpp references the runtime exports. The test installs its own host before
// every use, so these are never called.
extern "C" int32_t runtime_memory_reserve_range_export(void** inout, uint64_t size, int flags, uint64_t alignment) {
    (void)inout; (void)size; (void)flags; (void)alignment;
    std::abort();
}
extern "C" int32_t runtime_memory_port_flexible_export(uint64_t start, uint64_t size, int prot) {
    (void)start; (void)size; (void)prot;
    std::abort();
}
extern "C" int32_t runtime_memory_batch_map_export(void* entries, int count, int* processed) {
    (void)entries; (void)count; (void)processed;
    std::abort();
}

namespace {

using SotcGpuRegion::kGranule;

constexpr uint64_t kFakeBase = 0x1010000000ull;  // inside [kRangeLo, kRangeHi), never mapped here
constexpr uint64_t kReserve = 4ull << 30;

int g_failures = 0;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

struct Commit {
    uint64_t start;
    uint64_t length;
    uint8_t prot;
    uint32_t op;
};

struct FakeState {
    uint64_t reserve_result = kFakeBase;
    int32_t reserve_rc = 0;
    uint64_t reserve_calls = 0;
    uint64_t reserve_size = 0;
    int reserve_flags = -1;
    uint64_t reserve_align = 0;
    std::vector<Commit> commits;
    int64_t entry_limit = -1;  // entries beyond this total fail (-1: never)
    int32_t batch_calls = 0;
    std::vector<std::pair<uint64_t, uint64_t>> hook_calls;  // (addr, size) of the release hook
};

FakeState g;

// Layout of the port's BatchEntry, as the fake reads it.
struct FakeEntry {
    uint64_t start, offset, length;
    uint8_t prot, type;
    uint16_t reserved;
    uint32_t operation;
};

int32_t FakeReserve(void** inout, uint64_t size, int flags, uint64_t alignment) {
    ++g.reserve_calls;
    g.reserve_size = size;
    g.reserve_flags = flags;
    g.reserve_align = alignment;
    *inout = reinterpret_cast<void*>(static_cast<uintptr_t>(g.reserve_result));
    return g.reserve_rc;
}

int32_t FakeBatch(void* entries, int count, int* processed) {
    ++g.batch_calls;
    const FakeEntry* e = static_cast<const FakeEntry*>(entries);
    int done = 0;
    int32_t rc = 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t total = g.commits.size();
        if (g.entry_limit >= 0 && static_cast<int64_t>(total) >= g.entry_limit) {
            rc = 1;
            break;
        }
        g.commits.push_back(Commit{e[i].start, e[i].length, e[i].prot, e[i].operation});
        ++done;
    }
    *processed = done;
    return rc;
}

void FakeHook(void* ctx, uint64_t addr, uint64_t size) {
    (void)ctx;
    g.hook_calls.emplace_back(addr, size);
}

void ResetFake() {
    g = FakeState{};
    SotcGpuRegion::ResetForTest();
    SotcGpuRegion::Host host{&FakeReserve, &FakeBatch};
    SotcGpuRegion::SetHostForTest(host);
}

void InitDefault() {
    ResetFake();
    EXPECT(SotcGpuRegion::Init(kReserve));
}

// Every byte of [addr, addr+size) must lie in the committed granules, and the commits must be contiguous from
// the base in 64 KiB units with the GPU-accessible protection.
bool CommitsCover(uint64_t addr, uint64_t size) {
    uint64_t committed_end = kFakeBase;
    for (const Commit& c : g.commits) {
        if (c.start != committed_end || c.length != kGranule || c.prot != 0x33 || c.op != 3) {
            return false;
        }
        committed_end += c.length;
    }
    return addr >= kFakeBase && addr + size <= committed_end;
}

void TestInitArgs() {
    ResetFake();
    EXPECT(SotcGpuRegion::Init(kReserve));
    EXPECT(g.reserve_calls == 1);
    EXPECT(g.reserve_size == kReserve);
    EXPECT(g.reserve_flags == 0);
    EXPECT(g.reserve_align == kGranule);
    EXPECT(SotcGpuRegion::Contains(kFakeBase));
    EXPECT(SotcGpuRegion::Contains(kFakeBase + kReserve - 1));
    EXPECT(!SotcGpuRegion::Contains(kFakeBase - 1));
    EXPECT(!SotcGpuRegion::Contains(kFakeBase + kReserve));
    EXPECT(SotcGpuRegion::Init(kReserve));  // idempotent
    EXPECT(g.reserve_calls == 1);
}

void TestInitRejects() {
    ResetFake();
    g.reserve_result = 0x10000;  // below kRangeLo
    EXPECT(!SotcGpuRegion::Init(kReserve));
    EXPECT(!SotcGpuRegion::Contains(0x10000));

    ResetFake();
    g.reserve_result = 0xfc00000000ull - kReserve + kGranule;  // ends past kRangeHi
    EXPECT(!SotcGpuRegion::Init(kReserve));

    ResetFake();
    g.reserve_result = kFakeBase + 0x100;  // not granule-aligned
    EXPECT(!SotcGpuRegion::Init(kReserve));

    ResetFake();
    g.reserve_rc = -12;
    EXPECT(!SotcGpuRegion::Init(kReserve));
    EXPECT(SotcGpuRegion::Alloc(16, 0, 0, 0) == 0);

    ResetFake();  // a rejected Init can be retried
    EXPECT(SotcGpuRegion::Init(kReserve));
}

void TestAlignmentAndSizes() {
    InitDefault();
    uint64_t a = SotcGpuRegion::Alloc(1, 0, 1, 0);
    EXPECT(a != 0 && a % 16 == 0);
    EXPECT(SotcGpuRegion::SizeOf(a) == 16);

    uint64_t b = SotcGpuRegion::Alloc(100, 64, 1, 0);
    EXPECT(b != 0 && b % 64 == 0);
    EXPECT(SotcGpuRegion::SizeOf(b) == 128);

    uint64_t c = SotcGpuRegion::Alloc(10, 1024, 1, 0);
    EXPECT(c != 0 && c % 1024 == 0);
    EXPECT(SotcGpuRegion::SizeOf(c) == 1024);

    uint64_t d = SotcGpuRegion::Alloc(100, 48, 1, 0);  // 48 raised to 64
    EXPECT(d != 0 && d % 64 == 0);

    uint64_t e = SotcGpuRegion::Alloc(5000, 0, 1, 0);  // large: page-rounded
    EXPECT(e != 0 && e % 4096 == 0);
    EXPECT(SotcGpuRegion::SizeOf(e) == 8192);

    uint64_t f = SotcGpuRegion::Alloc(3000, 8192, 1, 0);  // large because of the alignment
    EXPECT(f != 0 && f % 8192 == 0);
    EXPECT(SotcGpuRegion::SizeOf(f) == 4096);

    uint64_t h = SotcGpuRegion::Alloc(300, 0, 1, 0);
    EXPECT(h != 0 && SotcGpuRegion::SizeOf(h) == 512);

    uint64_t i = SotcGpuRegion::Alloc(4096, 0, 1, 0);
    EXPECT(i != 0 && SotcGpuRegion::SizeOf(i) == 4096 && i % 4096 == 0);

    // Distinct live blocks never overlap.
    const uint64_t starts[] = {a, b, c, d, e, f, h, i};
    uint64_t sizes[8];
    for (int x = 0; x < 8; ++x) {
        sizes[x] = SotcGpuRegion::SizeOf(starts[x]);
        EXPECT(sizes[x] != 0);
    }
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            if (x != y) {
                EXPECT(!(starts[x] < starts[y] + sizes[y] && starts[y] < starts[x] + sizes[x]));
            }
        }
    }
    EXPECT(SotcGpuRegion::Alloc(0, 0, 0, 2) == 0);  // unknown kind
}

void TestSizeOfSlotOf() {
    InitDefault();
    const uint64_t a = SotcGpuRegion::Alloc(40, 0, 42, 0);
    EXPECT(a != 0);
    EXPECT(SotcGpuRegion::SizeOf(a) == 48);
    uint32_t slot = 0;
    EXPECT(SotcGpuRegion::SlotOf(a, &slot) && slot == 42);
    EXPECT(SotcGpuRegion::SizeOf(a + 8) == 0);
    EXPECT(!SotcGpuRegion::SlotOf(a + 8, &slot));
    EXPECT(SotcGpuRegion::Contains(a));
    SotcGpuRegion::Free(a);
    EXPECT(SotcGpuRegion::SizeOf(a) == 0);  // quarantined: not a live block start
    EXPECT(!SotcGpuRegion::SlotOf(a, &slot));
    EXPECT(SotcGpuRegion::Contains(a));
}

void TestClassReuseAfterQuarantine() {
    InitDefault();
    SotcGpuRegion::SetReleaseHook(&FakeHook, nullptr);
    const uint64_t a = SotcGpuRegion::Alloc(32, 0, 1, 0);
    EXPECT(a != 0);
    SotcGpuRegion::Free(a);  // frame 0

    // No reuse before the quarantine expires: released only when more than 3 frames have passed.
    for (uint64_t frame = 1; frame <= 3; ++frame) {
        SotcGpuRegion::OnFrame(frame);
        EXPECT(g.hook_calls.empty());
        const uint64_t b = SotcGpuRegion::Alloc(32, 0, 1, 0);
        EXPECT(b != 0 && b != a);
    }
    SotcGpuRegion::OnFrame(4);
    EXPECT(g.hook_calls.size() == 1);
    EXPECT(g.hook_calls[0].first == a && g.hook_calls[0].second == 32);

    // The released block is the next one of its class.
    const uint64_t c = SotcGpuRegion::Alloc(20, 0, 1, 0);
    EXPECT(c == a);
    EXPECT(SotcGpuRegion::SizeOf(c) == 32);
    SotcGpuRegion::Free(c);
    SotcGpuRegion::OnFrame(5);  // quarantined at 4: still held
    EXPECT(SotcGpuRegion::Alloc(32, 0, 1, 0) != c);
    SotcGpuRegion::OnFrame(8);
    EXPECT(SotcGpuRegion::Alloc(32, 0, 1, 0) == c);
    EXPECT(SotcGpuRegion::Alloc(33, 0, 1, 0) != c);
}

void TestLargeCoalescing() {
    InitDefault();
    g.hook_calls.clear();
    SotcGpuRegion::SetReleaseHook(&FakeHook, nullptr);
    const uint64_t a = SotcGpuRegion::Alloc(8192, 0, 1, 0);
    const uint64_t b = SotcGpuRegion::Alloc(8192, 0, 1, 0);
    const uint64_t c = SotcGpuRegion::Alloc(8192, 0, 1, 0);
    const uint64_t d = SotcGpuRegion::Alloc(8192, 0, 1, 0);  // keeps the bump pointer past c
    EXPECT(a && b == a + 8192 && c == b + 8192 && d == c + 8192);

    SotcGpuRegion::Free(a);
    SotcGpuRegion::Free(c);
    SotcGpuRegion::Free(b);  // joins a and c through the middle block
    SotcGpuRegion::OnFrame(1);
    SotcGpuRegion::OnFrame(4);
    // The three adjacent ranges reach the hook as one call.
    EXPECT(g.hook_calls.size() == 1);
    EXPECT(g.hook_calls[0].first == a && g.hook_calls[0].second == 3 * 8192);

    const uint64_t merged = SotcGpuRegion::Alloc(3 * 8192, 0, 1, 0);
    EXPECT(merged == a);  // first fit takes the coalesced run, not the bump pointer
    EXPECT(SotcGpuRegion::SizeOf(d) == 8192);
}

void TestRingExpiry() {
    InitDefault();
    g.hook_calls.clear();
    SotcGpuRegion::SetReleaseHook(&FakeHook, nullptr);
    const uint64_t r = SotcGpuRegion::Alloc(64, 0, 7, 1);  // frame 0
    EXPECT(r != 0);
    uint32_t slot = 0;
    EXPECT(SotcGpuRegion::SlotOf(r, &slot) && slot == 7);

    SotcGpuRegion::OnFrame(4);  // age 4: not older than 4 frames yet
    EXPECT(SotcGpuRegion::SizeOf(r) == 64);

    SotcGpuRegion::OnFrame(5);  // age 5: expires into quarantine
    EXPECT(SotcGpuRegion::SizeOf(r) == 0);
    EXPECT(SotcGpuRegion::Contains(r));
    EXPECT(SotcGpuRegion::GetCounters().ring_expired == 1);
    EXPECT(g.hook_calls.empty());

    SotcGpuRegion::OnFrame(8);  // quarantined at 5: 3 frames is not more than 3
    EXPECT(g.hook_calls.empty());
    SotcGpuRegion::OnFrame(9);
    EXPECT(g.hook_calls.size() == 1 && g.hook_calls[0].first == r && g.hook_calls[0].second == 64);
    EXPECT(SotcGpuRegion::Alloc(64, 0, 7, 0) == r);
}

void TestCommitCoverage() {
    InitDefault();
    // Mixed sizes that cross granule boundaries: every returned byte must sit in a committed granule.
    const uint64_t sizes[] = {16, 4096, 200, 70000, 8192, 3000, 1, 65536, 300000, 48, 4097};
    for (int round = 0; round < 3; ++round) {
        for (uint64_t size : sizes) {
            const uint64_t addr = SotcGpuRegion::Alloc(size, 0, 2, 0);
            EXPECT(addr != 0);
            EXPECT(CommitsCover(addr, SotcGpuRegion::SizeOf(addr)));
        }
    }
    // Commits are lazy: nothing beyond what the bump pointer needed.
    const SotcGpuRegion::Counters c = SotcGpuRegion::GetCounters();
    EXPECT(c.committed_bytes % kGranule == 0);
    EXPECT(c.committed_bytes >= c.bump_bytes);
    EXPECT(c.committed_bytes < c.bump_bytes + 2 * kGranule);
    EXPECT(c.reserved_bytes == kReserve);
}

void TestCommitFailure() {
    ResetFake();
    EXPECT(SotcGpuRegion::Init(kReserve));
    g.entry_limit = 1;  // the second granule fails
    EXPECT(SotcGpuRegion::Alloc(200000, 0, 1, 0) == 0);
    EXPECT(SotcGpuRegion::GetCounters().alloc_failures == 1);
    g.entry_limit = -1;
    EXPECT(SotcGpuRegion::Alloc(16, 0, 1, 0) != 0);  // the first granule was committed; this fits in it
    EXPECT(SotcGpuRegion::Alloc(kReserve, 0, 1, 0) == 0);  // larger than the range
}

void TestExhaustion() {
    ResetFake();
    EXPECT(SotcGpuRegion::Init(2 * kGranule));
    EXPECT(SotcGpuRegion::Alloc(3 * kGranule, 0, 1, 0) == 0);
    EXPECT(SotcGpuRegion::Alloc(kGranule, 0, 1, 0) != 0);
    EXPECT(SotcGpuRegion::Alloc(kGranule, 0, 1, 0) != 0);
    EXPECT(SotcGpuRegion::Alloc(16, 0, 1, 0) == 0);
}

void TestReportAndCounters() {
    InitDefault();
    const uint64_t a = SotcGpuRegion::Alloc(64, 0, 1, 0);
    SotcGpuRegion::Free(a);
    SotcGpuRegion::Free(a);  // second free: unknown
    SotcGpuRegion::Free(kFakeBase + 0x12345);
    const SotcGpuRegion::Counters c = SotcGpuRegion::GetCounters();
    EXPECT(c.allocs_total == 1);
    EXPECT(c.frees_total == 1);
    EXPECT(c.unknown_frees == 2);
    EXPECT(c.quarantined_blocks == 1 && c.quarantined_bytes == 64);
    EXPECT(c.live_blocks == 0 && c.live_bytes == 0);
    std::string out;
    SotcGpuRegion::Report(out);
    EXPECT(out.find("GPU region: reserved=") != std::string::npos);
    EXPECT(out.find("unknown frees=2") != std::string::npos);
}

void TestThreadSafety() {
    InitDefault();
    SotcGpuRegion::SetReleaseHook(&FakeHook, nullptr);
    constexpr int kThreads = 4;
    constexpr int kIterations = 4000;
    std::atomic<bool> running{true};
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> kept(kThreads);
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &kept]() {
            const uint64_t sizes[] = {16, 48, 200, 256, 500, 1024, 4096, 5000, 12000};
            std::vector<std::pair<uint64_t, uint64_t>> pending;
            for (int i = 0; i < kIterations; ++i) {
                const uint64_t size = sizes[(i * 7 + t) % 9];
                const uint64_t addr = SotcGpuRegion::Alloc(size, 0, static_cast<uint32_t>(t), 0);
                if (addr == 0) {
                    continue;
                }
                pending.emplace_back(addr, SotcGpuRegion::SizeOf(addr));
                if (i % 2 == 1) {
                    SotcGpuRegion::Free(pending.back().first);
                    pending.pop_back();
                }
            }
            kept[t] = pending;
        });
    }
    std::thread ticker([&running]() {
        for (uint64_t frame = 1; running.load(); ++frame) {
            SotcGpuRegion::OnFrame(frame);
            std::this_thread::yield();
        }
    });
    for (auto& w : workers) {
        w.join();
    }
    running = false;
    ticker.join();

    // Blocks still live at the end must not overlap; reuse after release would show up here.
    std::vector<std::pair<uint64_t, uint64_t>> all;
    for (const auto& k : kept) {
        all.insert(all.end(), k.begin(), k.end());
    }
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < all.size(); ++i) {
        EXPECT(SotcGpuRegion::Contains(all[i].first));
        EXPECT(SotcGpuRegion::SizeOf(all[i].first) == all[i].second);
        if (i + 1 < all.size()) {
            EXPECT(all[i].first + all[i].second <= all[i + 1].first);
        }
    }
    const SotcGpuRegion::Counters c = SotcGpuRegion::GetCounters();
    EXPECT(c.live_blocks == all.size());
    EXPECT(c.allocs_total == c.frees_total + c.live_blocks);
    EXPECT(c.alloc_failures == 0);
}

}  // namespace

int main() {
    // The size-class tests below cover the packed layout; page isolation (the default) is off for them.
    _putenv("SOTC_GPU_REGION_PAGE_ISOLATE=0");
    TestInitArgs();
    TestInitRejects();
    TestAlignmentAndSizes();
    TestSizeOfSlotOf();
    TestClassReuseAfterQuarantine();
    TestLargeCoalescing();
    TestRingExpiry();
    TestCommitCoverage();
    TestCommitFailure();
    TestExhaustion();
    TestReportAndCounters();
    TestThreadSafety();
    SotcGpuRegion::ResetForTest();
    if (g_failures == 0) {
        std::printf("PASS gpu-region-test\n");
        return 0;
    }
    std::printf("FAIL gpu-region-test: %d check(s) failed\n", g_failures);
    return 1;
}
