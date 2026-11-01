// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_STALE_CHECK (stale_check.h). A sampled detector for the bug where a vertex or index
// buffer reaches the GPU with bytes the guest has since rewritten.
//
// Why a stale page can only mean a missed CPU write. The arena holds a copy of guest buffer
// memory. For a page the tracker reports CPU-clean and GPU-clean, the port assumes the arena
// bytes equal the guest bytes that the last upload of that page copied. RecordUpload hashes those
// bytes right after the CPU copy into staging; the page was write-protected before that copy
// (ForEachUploadRange, protect batch), so a guest write can only land after the copy, and such a
// write faults and sets the CPU bit. Therefore: at bind, a page that is still CPU-clean and
// GPU-clean, and whose guest bytes hash differently from the stored hash, was changed without its
// CPU bit being set. The arena copy is stale. Nothing else can produce that combination, except
// the false positives listed below, which are bookkeeping gaps and not guest writes.
//
// What can give a false positive (a page reported stale while the arena is right):
//  - A port-side write of guest memory that clears the GPU bits (the readback apply writes GPU
//    data into guest memory). The guest bytes then equal what the arena already holds, but not the
//    stored hash. Every UnmarkRegionAsGpuModified site in buffer_cache.cpp calls Forget, so the
//    hash is gone before the bit is clear.
//  - Clean bits lifted without an upload (MarkRingClean, PcRingMarkClean): Forget in that path.
//    Ring declares mark pages CPU-modified, which the bind check skips.
//  - A write racing the check: the guest wrote, its fault has not set the CPU bit yet when the
//    bind looks. The tracker is queried again after hashing, which narrows the window but cannot
//    close it. A page that does become CPU-modified is never counted.
//  - Uploads that do not record hashes (host import, async copy, granule drop, the hash-small
//    hit): Forget runs at the start of the upload for every copied range, so no old hash survives.
//  - Partial pages at the ends of a range: only whole pages are recorded or compared.
// What the check cannot see: a guest write through a mapping the tracker never protects and that
// also never shows up in any port-side path. Those writes are invisible to the check as to the
// tracker, so they are the real target of this switch and would show up as STALE.
//
// Data structures: a sharded map (64 shards, each with its own mutex) from page index (address
// >> 12) to {XXH3-64 hash, upload generation}. A generation is one RecordUpload call; the report
// shows age = current generation - entry generation. Shards are only locked around map operations,
// never while the tracker or guest memory is read, so the fault handler's Forget cannot deadlock
// with a bind check.
//
// Cost: off = one static load per hook. On: RecordUpload hashes every uploaded byte once (XXH3,
// roughly memory bandwidth on a fast core, plus one tracker query per page); Forget is one lock
// per page of every write the hooks see; a sampled bind hashes its clean pages (about 4 KiB per
// page, one in N binds).

#include "stale_check.h"

#include <xxhash.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "mem_classes.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (same symbol as elsewhere).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace SotcStale {
namespace {

constexpr uint64_t kPage = 4096;

// sotc: page sampling. Hashing every uploaded page made each frame take 600-750 ms (first run, 1 Nov),
// slow enough that the Lab never started its route. Only 1 page in SOTC_STALE_PAGES (default 64),
// picked by a hash of the page index (always the same pages), is recorded and checked: the cost falls
// by that factor and a stale page among them is still caught.
bool SampledPage(uint64_t page) {
    static const uint64_t every = [] {
        const char* v = std::getenv("SOTC_STALE_PAGES");
        const uint64_t n = v ? std::strtoull(v, nullptr, 10) : 64;
        return n == 0 ? uint64_t{1} : n;
    }();
    return every == 1 || ((page * 0x9E3779B97F4A7C15ull) >> 32) % every == 0;
}
constexpr uint32_t kShards = 64;
constexpr uint32_t kFirstLog = 32;
constexpr uint64_t kNoStale = ~uint64_t{0};

struct Entry {
    uint64_t hash;
    uint64_t gen;
};

struct Shard {
    std::mutex mu;
    std::unordered_map<uint64_t, Entry> pages; // page index -> entry
};

Shard g_shards[kShards];

std::atomic<uint64_t> g_upload_gen{0}; // RecordUpload calls
std::atomic<uint64_t> g_bind_seq{0};   // SampleBind calls
std::atomic<uint64_t> g_binds{0};      // bind checks run
std::atomic<uint64_t> g_pages{0};      // pages compared (hashed at bind)
std::atomic<uint64_t> g_stale{0};      // STALE pages
std::atomic<uint64_t> g_first_stale{kNoStale};
std::atomic<uint32_t> g_logged{0};

// Report state: only Report touches these (the reporting thread).
uint64_t rep_binds = 0;
uint64_t rep_pages = 0;
uint64_t rep_stale = 0;

uint32_t ParseN() {
    const char* v = std::getenv("SOTC_STALE_CHECK");
    if (!v || !*v) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    if (end == v) {
        return 16; // not a number: on, one bind in 16
    }
    return static_cast<uint32_t>(n);
}

Shard& ShardFor(uint64_t page) {
    return g_shards[(page * 0x9E3779B97F4A7C15ull) >> 58]; // top 6 bits: 0..63
}

const unsigned char* PageBytes(uint64_t page) {
    return runtime_memory_backing_pointer(static_cast<uintptr_t>(page << 12));
}

// Names the block of the mem-class owner table that holds addr (SOTC_CLASS_TRACK=2 fills it).
void NameOwner(uint64_t addr, char* buf, size_t cap) {
    uint64_t version = 0;
    for (const auto& r : SotcMemClasses::SnapshotOwners(version)) {
        if (addr >= r.addr && addr - r.addr < r.size) {
            std::snprintf(buf, cap, "owner %u block 0x%llx+%llu", r.owner,
                          static_cast<unsigned long long>(r.addr),
                          static_cast<unsigned long long>(r.size));
            return;
        }
    }
    std::snprintf(buf, cap, "owner not named");
}

} // namespace

bool On() noexcept {
    return SampleN() != 0;
}

uint32_t SampleN() noexcept {
    static const uint32_t n = ParseN();
    return n;
}

bool SampleBind() noexcept {
    const uint32_t n = SampleN();
    if (n == 0) {
        return false;
    }
    return g_bind_seq.fetch_add(1, std::memory_order_relaxed) % n == 0;
}

void Forget(uint64_t addr, uint64_t size) {
    if (!On() || size == 0) {
        return;
    }
    const uint64_t first = addr >> 12;
    const uint64_t last = (addr + size - 1) >> 12;
    for (uint64_t page = first; page <= last; ++page) {
        if (!SampledPage(page)) {
            continue;
        }
        Shard& s = ShardFor(page);
        std::scoped_lock lock{s.mu};
        s.pages.erase(page);
    }
}

void RecordUpload(uint64_t addr, uint64_t size, const Oracle& oracle) {
    if (!On() || size == 0) {
        return;
    }
    const uint64_t gen = g_upload_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t first = (addr + kPage - 1) >> 12; // whole pages only
    const uint64_t end = (addr + size) >> 12;
    for (uint64_t page = first; page < end; ++page) {
        if (!SampledPage(page)) {
            continue;
        }
        const uint64_t a = page << 12;
        // Pages with GPU bits may be changed by the GPU at any time: no hash for them.
        bool store = false;
        uint64_t hash = 0;
        if (!oracle.gpu_modified(oracle.ctx, a, kPage)) {
            if (const unsigned char* bytes = PageBytes(page)) {
                hash = XXH3_64bits(bytes, kPage);
                store = true;
            }
        }
        Shard& s = ShardFor(page);
        std::scoped_lock lock{s.mu};
        if (store) {
            s.pages[page] = Entry{hash, gen};
        } else {
            s.pages.erase(page);
        }
    }
}

void CheckBind(uint64_t addr, uint64_t size, const Oracle& oracle) {
    if (!On() || size == 0) {
        return;
    }
    g_binds.fetch_add(1, std::memory_order_relaxed);
    const uint64_t first = addr >> 12;
    const uint64_t end = (addr + size + kPage - 1) >> 12;
    for (uint64_t page = first; page < end; ++page) {
        if (!SampledPage(page)) {
            continue;
        }
        Entry e{};
        bool have = false;
        Shard& s = ShardFor(page);
        {
            std::scoped_lock lock{s.mu};
            if (const auto it = s.pages.find(page); it != s.pages.end()) {
                e = it->second;
                have = true;
            }
        }
        if (!have) {
            continue;
        }
        const uint64_t a = page << 12;
        // Only pages the tracker considers current: no CPU bit and no GPU bit.
        if (oracle.cpu_modified(oracle.ctx, a, kPage) ||
            oracle.gpu_modified(oracle.ctx, a, kPage)) {
            continue;
        }
        const unsigned char* bytes = PageBytes(page);
        if (!bytes) {
            continue;
        }
        g_pages.fetch_add(1, std::memory_order_relaxed);
        if (XXH3_64bits(bytes, kPage) == e.hash) {
            continue;
        }
        // Looked again after hashing: a guest write that faulted meanwhile is not stale.
        if (oracle.cpu_modified(oracle.ctx, a, kPage) ||
            oracle.gpu_modified(oracle.ctx, a, kPage)) {
            continue;
        }
        g_stale.fetch_add(1, std::memory_order_relaxed);
        uint64_t none = kNoStale;
        g_first_stale.compare_exchange_strong(none, a, std::memory_order_relaxed);
        if (g_logged.fetch_add(1, std::memory_order_relaxed) < kFirstLog) {
            char owner[160];
            NameOwner(a, owner, sizeof owner);
            const uint64_t gen_now = g_upload_gen.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "SOTC STALE CHECK: stale page 0x%llx, hash age %llu upload generations, "
                         "bind 0x%llx+%llu, %s\n",
                         static_cast<unsigned long long>(a),
                         static_cast<unsigned long long>(gen_now - e.gen),
                         static_cast<unsigned long long>(addr),
                         static_cast<unsigned long long>(size), owner);
        }
    }
}

void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    const uint64_t binds = g_binds.load(std::memory_order_relaxed);
    const uint64_t pages = g_pages.load(std::memory_order_relaxed);
    const uint64_t stale = g_stale.load(std::memory_order_relaxed);
    const uint64_t first = g_first_stale.load(std::memory_order_relaxed);
    char first_text[32];
    if (first == kNoStale) {
        std::snprintf(first_text, sizeof first_text, "none");
    } else {
        std::snprintf(first_text, sizeof first_text, "0x%llx",
                      static_cast<unsigned long long>(first));
    }
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC STALE CHECK: binds checked %llu (+%llu in %.1fs), pages compared %llu "
                  "(+%llu), STALE pages %llu (+%llu) (first: %s)\n",
                  static_cast<unsigned long long>(binds),
                  static_cast<unsigned long long>(binds - rep_binds), seconds,
                  static_cast<unsigned long long>(pages),
                  static_cast<unsigned long long>(pages - rep_pages),
                  static_cast<unsigned long long>(stale),
                  static_cast<unsigned long long>(stale - rep_stale), first_text);
    out += line;
    rep_binds = binds;
    rep_pages = pages;
    rep_stale = stale;
}

} // namespace SotcStale
