// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PRELOAD_UPLOAD, see preupload.h.

#include "preupload.h"

#include "guest_mem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SotcPreupload {
namespace {

constexpr uint64_t kPiece = uint64_t{1} << 20;
constexpr uint64_t kMinRange = uint64_t{4} << 10; // smaller copies are not worth a range
constexpr size_t kMaxRanges = 1u << 20;

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct Loaded {
    uint64_t end;
    uint32_t fourcc;
    uint64_t t_ns;
    uint64_t source; // creator call offset << 32 | kind
};

struct Pending {
    uint64_t addr;
    uint64_t end;
    uint64_t t_ns; // when it was filled (uploaded once kMinAgeNs old)
};

// SOTC_PRELOAD_UPLOAD_AGE_MS (default 100): the age a range needs before the idle upload takes it (a writer still
// filling it has had time to finish). An early upload is only wasted when the CPU writes the range again: page
// tracking marks it and its bind copies it once more.
const uint64_t kMinAgeNs = [] {
    const char* v = std::getenv("SOTC_PRELOAD_UPLOAD_AGE_MS");
    return (v && v[0] ? std::strtoull(v, nullptr, 10) : 100ull) * 1'000'000ull;
}();

std::mutex g_mutex;
std::map<uint64_t, Loaded> g_loaded; // start -> range (non-overlapping: a newer load replaces what it covers)
std::deque<Pending> g_queue;
std::atomic<bool> g_has_work{false};

// Counters of the 5 s window (under g_mutex).
struct KindStat {
    uint64_t loads{}, load_bytes{}, copied{}, lead_ms_sum{}, lead_n{};
};
std::unordered_map<uint32_t, KindStat> g_kinds;
std::unordered_map<uint64_t, KindStat> g_sources; // whole session (not reset by the report)

// Pools whose base is not known yet (the creator writes obj+0x10 after its entry).
struct PendingPool {
    uint64_t obj;
    uint64_t size;
    uint32_t source;
    uint32_t kind;
    uint64_t t_ns;
};
std::vector<PendingPool> g_pending_pools;
uint64_t g_bad_pool_bases = 0; // sotc: pools whose resolved base was not mapped guest memory (dropped)
constexpr uint64_t kPoolMin = uint64_t{1} << 20;
constexpr uint64_t kPoolResolveNs = 20'000'000; // the creator has returned by then
constexpr uint64_t kObjBaseOffset = 0x10;

void ResolvePools(uint64_t now);
uint64_t g_bind_copied{}, g_bind_copied_loaded{}, g_uploaded{}, g_drain_calls{};

// Per-frame cap of the idle upload: renewed at each presented frame and every 33 ms without one (the loading
// screens flip rarely, and the data must still go up then).
uint64_t g_frame{~uint64_t{0}};
uint64_t g_frame_bytes{};
uint64_t g_frame_t_ns{};
constexpr uint64_t kBudgetPeriodNs = 33'000'000;

const uint64_t kFrameCap = [] {
    const char* v = std::getenv("SOTC_PRELOAD_UPLOAD_MB");
    const long long mb = v ? std::atoll(v) : 10;
    return static_cast<uint64_t>(std::clamp<long long>(mb, 1, 256)) << 20;
}();

std::vector<uint32_t> Sources() {
    std::vector<uint32_t> out;
    const char* v = std::getenv("SOTC_PRELOAD_UPLOAD_SOURCES");
    if (!v) {
        return out;
    }
    const char* p = v;
    while (*p) {
        char* end = nullptr;
        const unsigned long long x = std::strtoull(p, &end, 16);
        if (end == p) {
            break;
        }
        out.push_back(static_cast<uint32_t>(x));
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

bool SourceQueued(uint32_t source) {
    static const std::vector<uint32_t> sources = Sources();
    return sources.empty() || std::find(sources.begin(), sources.end(), source) != sources.end();
}

std::string Name(uint32_t fourcc) {
    char c[5]{};
    std::memcpy(c, &fourcc, 4);
    for (char& ch : c) {
        if (ch && (ch < 32 || ch > 126)) {
            ch = '?';
        }
    }
    return c;
}

} // namespace

int Mode() noexcept {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PRELOAD_UPLOAD");
        return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
    }();
    return mode;
}

void NotePool(uint64_t obj, uint64_t size, uint32_t source, uint32_t kind) noexcept {
    if (!On() || obj == 0 || size < kPoolMin || size > (uint64_t{1} << 30)) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (g_pending_pools.size() < 65536) {
        g_pending_pools.push_back({obj, size, source, kind, NowNs()});
    }
}

namespace {
// Under g_mutex. The pools whose creator has returned: their base from obj+0x10, then a range like a memcpy one.
void ResolvePools(uint64_t now) {
    if (g_pending_pools.empty()) {
        return;
    }
    size_t kept = 0;
    std::vector<PendingPool> ready;
    for (auto& p : g_pending_pools) {
        if (now - p.t_ns < kPoolResolveNs) {
            g_pending_pools[kept++] = p;
        } else {
            ready.push_back(p);
        }
    }
    g_pending_pools.resize(kept);
    for (const auto& p : ready) {
        uint64_t base = 0;
        if (!SotcGuest::Read(p.obj + kObjBaseOffset, &base, sizeof base) || base == 0) {
            continue;
        }
        base &= 0xFFFFFFFFFFFFull;
        // sotc: the pool object can be freed and reused before it is resolved (kPoolResolveNs later): its
        // +0x10 then holds unrelated data (0x870122068701, 0x6805a8052005 after a colossus jump, which
        // walked the memory tracker out of bounds). Keep only a base whose first and last bytes are mapped
        // guest memory inside the tracked 40-bit range.
        unsigned char probe = 0;
        if (base >= (uint64_t{1} << 40) || p.size > (uint64_t{1} << 40) - base ||
            !SotcGuest::Read(base, &probe, 1) || !SotcGuest::Read(base + p.size - 1, &probe, 1)) {
            ++g_bad_pool_bases;
            continue;
        }
        const uint64_t end = base + p.size;
        const uint64_t src = (uint64_t{p.source} << 32) | p.kind;
        auto& k = g_kinds[kPool];
        ++k.loads;
        k.load_bytes += p.size;
        auto& sk = g_sources[src];
        ++sk.loads;
        sk.load_bytes += p.size;
        auto it = g_loaded.lower_bound(base);
        if (it != g_loaded.begin() && std::prev(it)->second.end > base) {
            --it;
        }
        while (it != g_loaded.end() && it->first < end) {
            it = g_loaded.erase(it);
        }
        g_loaded.emplace(base, Loaded{end, kPool, p.t_ns, src});
        if (Mode() == 2 && SourceQueued(p.source)) {
            g_queue.push_back({base, end, p.t_ns});
            g_has_work.store(true, std::memory_order_relaxed);
        }
    }
}
} // namespace

void NoteLoaded(uint64_t addr, uint64_t size, uint32_t fourcc, uint32_t source, uint32_t kind) noexcept {
    if (!On() || size < kMinRange || addr == 0) {
        return;
    }
    const uint64_t end = addr + size;
    const uint64_t now = NowNs();
    std::scoped_lock lock{g_mutex};
    auto& k = g_kinds[fourcc];
    ++k.loads;
    k.load_bytes += size;
    const uint64_t src = (uint64_t{source} << 32) | kind;
    auto& sk = g_sources[src];
    ++sk.loads;
    sk.load_bytes += size;
    // A newer load of the same memory replaces the older ranges it overlaps.
    auto it = g_loaded.lower_bound(addr);
    if (it != g_loaded.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > addr) {
            it = prev;
        }
    }
    while (it != g_loaded.end() && it->first < end) {
        it = g_loaded.erase(it);
    }
    if (g_loaded.size() >= kMaxRanges) {
        g_loaded.erase(g_loaded.begin());
    }
    g_loaded.emplace(addr, Loaded{end, fourcc, now, src});
    // The creator's memcpy ranges (meshes: most are other LODs and areas, drawn later or never) only on request.
    static const bool resc_queued = [] {
        const char* v = std::getenv("SOTC_PRELOAD_UPLOAD_RESC");
        return v && v[0] == '1';
    }();
    if (Mode() == 2 && fourcc == kResCopy && resc_queued && SourceQueued(source)) {
        g_queue.push_back({addr, end, now});
        g_has_work.store(true, std::memory_order_relaxed);
    }
}

void NoteBindCopy(uint64_t addr, uint64_t size, uint64_t bytes) noexcept {
    if (!On() || bytes == 0 || size == 0) {
        return;
    }
    const uint64_t end = addr + size;
    const uint64_t now = NowNs();
    std::scoped_lock lock{g_mutex};
    ResolvePools(now);
    g_bind_copied += bytes;
    auto it = g_loaded.lower_bound(addr);
    if (it != g_loaded.begin() && std::prev(it)->second.end > addr) {
        --it;
    }
    // A big copy (4 MiB or more): its creators, once each (the first 64), to find the pools the binds read whole.
    static int big_logged = 0;
    const bool log_big = bytes >= (uint64_t{4} << 20) && big_logged < 64;
    std::string big_line;
    // The copied bytes are spread over the bind's range: each loaded range gets the share it overlaps.
    for (; it != g_loaded.end() && it->first < end; ++it) {
        const uint64_t lo = std::max(addr, it->first);
        const uint64_t hi = std::min(end, it->second.end);
        if (hi <= lo) {
            continue;
        }
        const uint64_t share = static_cast<uint64_t>(static_cast<double>(bytes) * (hi - lo) / size);
        auto& k = g_kinds[it->second.fourcc];
        k.copied += share;
        k.lead_ms_sum += (now - it->second.t_ns) / 1'000'000;
        ++k.lead_n;
        if (log_big && big_line.size() < 600) {
            char b[96];
            std::snprintf(b, sizeof b, " [%#x/k%u %.2f MiB at +%#llx, %llu ms old]",
                          static_cast<unsigned>(it->second.source >> 32),
                          static_cast<unsigned>(it->second.source & 0xFFFFFFFFu),
                          double(it->second.end - it->first) / 1048576.0,
                          static_cast<unsigned long long>(it->first - addr),
                          static_cast<unsigned long long>((now - it->second.t_ns) / 1'000'000));
            big_line += b;
        }
        auto& sk = g_sources[it->second.source];
        sk.copied += share;
        sk.lead_ms_sum += (now - it->second.t_ns) / 1'000'000;
        ++sk.lead_n;
        g_bind_copied_loaded += share;
    }
    if (log_big) {
        ++big_logged;
        std::fprintf(stderr, "SOTC PRELOAD UPLOAD big copy %#llx+%#llx (%.1f MiB copied): %s\n",
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                     double(bytes) / 1048576.0, big_line.empty() ? "no built range" : big_line.c_str());
    }
}

void Release(uint64_t addr, uint64_t size) noexcept {
    if (!On() || size == 0) {
        return;
    }
    const uint64_t end = addr + size;
    std::scoped_lock lock{g_mutex};
    auto it = g_loaded.lower_bound(addr);
    if (it != g_loaded.begin() && std::prev(it)->second.end > addr) {
        --it;
    }
    while (it != g_loaded.end() && it->first < end) {
        it = g_loaded.erase(it);
    }
    std::erase_if(g_queue, [&](const Pending& p) { return p.addr < end && addr < p.end; });
}

void QueueUrgent(uint64_t addr, uint64_t size) noexcept {
    if (Mode() != 2 || size == 0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    const uint64_t end = addr + size;
    // Already at the front for an earlier deferral of this frame: not queued twice.
    for (size_t i = 0; i < std::min<size_t>(g_queue.size(), 64); ++i) {
        if (g_queue[i].addr <= addr && end <= g_queue[i].end && g_queue[i].t_ns == 0) {
            return;
        }
    }
    g_queue.push_front({addr, end, 0}); // t_ns 0: old enough at once
    g_has_work.store(true, std::memory_order_relaxed);
}

bool HasWork() noexcept {
    return g_has_work.load(std::memory_order_relaxed) || (Mode() == 2 && !g_pending_pools.empty());
}

unsigned Idle(uint64_t budget_ns, uint64_t frame, DrainFn drain, StopFn stop, void* ctx) {
    if (Mode() != 2 || !HasWork()) {
        return 0;
    }
    const uint64_t t0 = NowNs();
    unsigned calls = 0;
    for (;;) {
        Pending piece{};
        {
            std::scoped_lock lock{g_mutex};
            if (frame != g_frame || t0 - g_frame_t_ns > kBudgetPeriodNs) {
                g_frame = frame;
                g_frame_bytes = 0;
                g_frame_t_ns = t0;
            }
            ResolvePools(t0);
            if (g_frame_bytes >= kFrameCap || g_queue.empty()) {
                g_has_work.store(!g_queue.empty(), std::memory_order_relaxed);
                return calls;
            }
            Pending& head = g_queue.front();
            if (t0 - head.t_ns < kMinAgeNs) {
                return calls; // queue order is fill order: the rest is younger still
            }
            piece = {head.addr, std::min(head.end, head.addr + kPiece)};
            head.addr = piece.end;
            if (head.addr >= head.end) {
                g_queue.pop_front();
            }
        }
        const uint64_t uploaded = drain(ctx, piece.addr, piece.end - piece.addr);
        ++calls;
        {
            std::scoped_lock lock{g_mutex};
            g_frame_bytes += uploaded;
            g_uploaded += uploaded;
            ++g_drain_calls;
        }
        if (NowNs() - t0 >= budget_ns || stop(ctx)) {
            return calls;
        }
    }
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    const double mib = 1024.0 * 1024.0;
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC PRELOAD UPLOAD (%.1f s): bind copies %.1f MiB, of loaded ranges %.1f MiB; idle uploads %.1f "
                  "MiB in %llu calls, queued %zu; ranges %zu; bad pool bases %llu;",
                  seconds, g_bind_copied / mib, g_bind_copied_loaded / mib, g_uploaded / mib,
                  (unsigned long long)g_drain_calls, g_queue.size(), g_loaded.size(),
                  (unsigned long long)g_bad_pool_bases);
    out += line;
    std::vector<std::pair<uint32_t, KindStat>> kinds(g_kinds.begin(), g_kinds.end());
    std::sort(kinds.begin(), kinds.end(),
              [](const auto& a, const auto& b) { return a.second.load_bytes > b.second.load_bytes; });
    int shown = 0;
    for (const auto& [cc, k] : kinds) {
        if (shown++ >= 8) {
            break;
        }
        std::snprintf(line, sizeof line, " %s loads %llu %.1f MiB copied %.1f MiB lead %llu ms;", Name(cc).c_str(),
                      (unsigned long long)k.loads, k.load_bytes / mib, k.copied / mib,
                      (unsigned long long)(k.lead_n ? k.lead_ms_sum / k.lead_n : 0));
        out += line;
    }
    out += '\n';
    // The sources of the session, by built bytes: which creators' buffers the binds read (copied) and which never.
    std::vector<std::pair<uint64_t, KindStat>> sources(g_sources.begin(), g_sources.end());
    std::sort(sources.begin(), sources.end(),
              [](const auto& a, const auto& b) { return a.second.load_bytes > b.second.load_bytes; });
    out += "SOTC PRELOAD UPLOAD sources (session):";
    shown = 0;
    for (const auto& [src, k] : sources) {
        if (shown++ >= 16) {
            break;
        }
        std::snprintf(line, sizeof line, " %#x/k%u built %llu %.1f MiB copied %.1f MiB;",
                      static_cast<unsigned>(src >> 32), static_cast<unsigned>(src & 0xFFFFFFFFu),
                      (unsigned long long)k.loads, k.load_bytes / mib, k.copied / mib);
        out += line;
    }
    out += '\n';
    g_kinds.clear();
    g_bind_copied = g_bind_copied_loaded = g_uploaded = g_drain_calls = 0;
}

} // namespace SotcPreupload
