// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GPU_CHECKPOINTS (sotc_checkpoints.h).

#include "video_core/sotc_checkpoints.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"

namespace SotcCheckpoints {
namespace {

constexpr uint32_t kRingBits = 16; // 65,536 commands: several frames of the game's ~5,000 draws
constexpr uint64_t kRingMask = (uint64_t{1} << kRingBits) - 1;
constexpr uint32_t kHashes = 6; // Fragment, TessCtrl, TessEval, Vertex, Geometry, Compute (Shader::SwStage)

struct Entry {
    std::atomic<uint64_t> seq{0}; // written last: the entry is whole when seq matches
    Kind kind{};
    uint64_t hashes[kHashes]{};
    uint16_t perms[kHashes]{};
    uint64_t a{}, b{}, c{};
};

std::array<Entry, kRingMask + 1>& Ring() {
    static auto* ring = new std::array<Entry, kRingMask + 1>{}; // never freed (read in the crash path)
    return *ring;
}

std::atomic<uint64_t> g_next{1};
std::atomic<bool> g_available{false};

// The bound resources of the last 2,048 commands (allocated at the first use: ~7 MiB, only with the switch on).
constexpr uint64_t kResMask = (uint64_t{1} << 11) - 1;
constexpr uint32_t kResPerEntry = 64;
struct ResEntry {
    std::atomic<uint64_t> seq{0};
    uint32_t count{};
    Resource res[kResPerEntry]{};
};
std::atomic<ResEntry*> g_res{nullptr};

ResEntry* ResRing() {
    ResEntry* ring = g_res.load(std::memory_order_acquire);
    if (!ring) {
        auto* fresh = new ResEntry[kResMask + 1]; // never freed (read in the crash path)
        if (g_res.compare_exchange_strong(ring, fresh, std::memory_order_acq_rel)) {
            ring = fresh;
        } else {
            delete[] fresh;
        }
    }
    return ring;
}

// The last 16,384 destroyed handles, with the last command noted when each went.
constexpr uint64_t kFreedMask = (uint64_t{1} << 14) - 1;
struct Freed {
    std::atomic<uint64_t> handle{0};
    uint64_t seq{};
    uint8_t type{};
};
std::array<Freed, kFreedMask + 1>& FreedRing() {
    static auto* ring = new std::array<Freed, kFreedMask + 1>{};
    return *ring;
}
std::atomic<uint64_t> g_freed_next{0};

// Report: the device fault addresses (VK_EXT_device_fault) a bound buffer's device range is matched against.
const uint64_t* g_fault_addrs = nullptr;
uint32_t g_fault_count = 0;

// The command noted last when `handle` was destroyed (0: not among the recent frees).
uint64_t FreedAt(uint64_t handle) {
    if (handle == 0) {
        return 0;
    }
    uint64_t best = 0;
    for (const auto& f : FreedRing()) {
        if (f.handle.load(std::memory_order_acquire) == handle) {
            best = std::max(best, f.seq == 0 ? uint64_t{1} : f.seq);
        }
    }
    return best;
}

struct QueueSlot {
    VkQueue queue;
    const char* name;
};
std::mutex g_queues_mutex;
std::vector<QueueSlot> g_queues;

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Draw:
        return "draw";
    case Kind::DrawIndexed:
        return "draw indexed";
    case Kind::DrawIndirect:
        return "draw indirect";
    case Kind::DrawIndexedIndirect:
        return "draw indexed indirect";
    case Kind::DrawIndirectCount:
        return "draw indirect count";
    case Kind::DrawIndexedIndirectCount:
        return "draw indexed indirect count";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::DispatchIndirect:
        return "dispatch indirect";
    case Kind::DrawRun:
        return "draw run (SOTC_DRAW_MDI)";
    }
    return "?";
}

// What a, b, c hold for each kind.
const char* ArgNames(Kind kind) {
    switch (kind) {
    case Kind::Draw:
    case Kind::DrawIndexed:
        return "count %llu, instances %llu, first instance %llu";
    case Kind::DrawIndirect:
    case Kind::DrawIndexedIndirect:
        return "args guest %#llx, draws %llu, stride %llu";
    case Kind::DrawIndirectCount:
    case Kind::DrawIndexedIndirectCount:
        return "args guest %#llx, max draws %llu, count guest %#llx";
    case Kind::Dispatch:
        return "groups %llu x %llu x %llu";
    case Kind::DispatchIndirect:
        return "args guest %#llx, offset %llu, %llu";
    case Kind::DrawRun:
        return "draws %llu, indexed %llu, %llu";
    }
    return "%llu %llu %llu";
}

void PrintEntry(uint64_t seq, const char* tag) {
    const Entry& e = Ring()[seq & kRingMask];
    if (e.seq.load(std::memory_order_acquire) != seq) {
        std::fprintf(stderr, "    #%llu %s (overwritten)\n", static_cast<unsigned long long>(seq), tag);
        return;
    }
    static constexpr const char* kStage[kHashes] = {"ps", "hs", "ds", "vs", "gs", "cs"};
    char hashes[256] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < kHashes; ++i) {
        if (e.hashes[i] != 0 && used < sizeof(hashes) - 40) {
            used += static_cast<size_t>(std::snprintf(hashes + used, sizeof(hashes) - used, " %s %#llx", kStage[i],
                                                      static_cast<unsigned long long>(e.hashes[i])));
            if (e.perms[i] != 0xFFFF && used < sizeof(hashes) - 12) {
                used += static_cast<size_t>(std::snprintf(hashes + used, sizeof(hashes) - used, " (perm %u)",
                                                          static_cast<unsigned>(e.perms[i])));
            }
        }
    }
    char args[160];
    std::snprintf(args, sizeof(args), ArgNames(e.kind), static_cast<unsigned long long>(e.a),
                  static_cast<unsigned long long>(e.b), static_cast<unsigned long long>(e.c));
    std::fprintf(stderr, "    #%llu %s %s:%s; %s\n", static_cast<unsigned long long>(seq), tag, KindName(e.kind),
                 hashes, args);
    // Its bound resources, and those destroyed after it was recorded (the GPU still read them).
    const ResEntry* ring = g_res.load(std::memory_order_acquire);
    if (!ring || std::strcmp(tag, "done") == 0) {
        return;
    }
    const ResEntry& r = ring[seq & kResMask];
    if (r.seq.load(std::memory_order_acquire) != seq) {
        return;
    }
    static constexpr const char* kType[] = {"?", "buffer", "buffer (written)", "vertex/index", "image", "descriptor",
                                            "pipeline"};
    for (uint32_t i = 0; i < r.count; ++i) {
        const Resource& res = r.res[i];
        if (res.type == kDescriptor) {
            std::fprintf(stderr, "        descriptor       binding %u type %u handle %#llx offset %#llx range %#llx\n",
                         static_cast<unsigned>(res.addr & 0xFFFFFFFFu), static_cast<unsigned>(res.addr >> 32),
                         static_cast<unsigned long long>(res.handle), static_cast<unsigned long long>(res.gpu),
                         static_cast<unsigned long long>(res.size));
            continue;
        }
        const uint64_t freed = FreedAt(res.handle);
        char note[160] = "";
        for (uint32_t f = 0; f < g_fault_count && res.gpu_base != 0; ++f) {
            const uint64_t fa = g_fault_addrs[f];
            if (fa >= (res.gpu_base & ~uint64_t{0xFFF}) && fa < res.gpu_base + res.gpu_size) {
                std::snprintf(note, sizeof(note), "  <-- HOLDS THE FAULT ADDRESS %#llx (buffer device range %#llx +%#llx)",
                              static_cast<unsigned long long>(fa), static_cast<unsigned long long>(res.gpu_base),
                              static_cast<unsigned long long>(res.gpu_size));
            }
        }
        if (note[0] != 0) {
            // the fault match says it all
        } else if (freed >= seq) {
            std::snprintf(note, sizeof(note), "  <-- DESTROYED after this command was recorded (at #%llu)",
                          static_cast<unsigned long long>(freed));
        } else if (freed != 0) {
            std::snprintf(note, sizeof(note), "  (a handle destroyed earlier, at #%llu, reused?)",
                          static_cast<unsigned long long>(freed));
        }
        char gpu[64] = "";
        if (res.gpu != 0) {
            std::snprintf(gpu, sizeof(gpu), " gpu %#llx", static_cast<unsigned long long>(res.gpu));
        }
        std::fprintf(stderr, "        %-16s guest %#llx +%#llx handle %#llx%s%s\n", kType[res.type < 7 ? res.type : 0],
                     static_cast<unsigned long long>(res.addr), static_cast<unsigned long long>(res.size),
                     static_cast<unsigned long long>(res.handle), gpu, note);
    }
}

} // namespace

bool Wanted() noexcept {
    static const bool wanted = [] {
        const char* v = std::getenv("SOTC_GPU_CHECKPOINTS");
        return v && v[0] == '1';
    }();
    return wanted;
}

bool On() noexcept {
    return g_available.load(std::memory_order_relaxed);
}

void SetAvailable(bool available) noexcept {
    g_available.store(available && Wanted(), std::memory_order_relaxed);
    if (Wanted()) {
        std::fprintf(stderr, "SOTC GPU CHECKPOINTS: %s\n",
                     available ? "on (VK_NV_device_diagnostic_checkpoints)"
                               : "unavailable (VK_NV_device_diagnostic_checkpoints is NVIDIA only), off");
    }
}

uint64_t Note(Kind kind, const uint64_t* hashes, uint32_t hash_count, uint64_t a, uint64_t b, uint64_t c,
              const uint16_t* perms) noexcept {
    const uint64_t seq = g_next.fetch_add(1, std::memory_order_relaxed);
    Entry& e = Ring()[seq & kRingMask];
    e.seq.store(0, std::memory_order_relaxed);
    e.kind = kind;
    for (uint32_t i = 0; i < kHashes; ++i) {
        e.hashes[i] = i < hash_count && hashes ? hashes[i] : 0;
        e.perms[i] = perms ? perms[i] : 0xFFFF;
    }
    e.a = a;
    e.b = b;
    e.c = c;
    e.seq.store(seq, std::memory_order_release);
    return seq;
}

void NoteResources(uint64_t seq, const Resource* resources, uint32_t count) noexcept {
    ResEntry& r = ResRing()[seq & kResMask];
    r.seq.store(0, std::memory_order_relaxed);
    r.count = std::min(count, kResPerEntry);
    for (uint32_t i = 0; i < r.count; ++i) {
        r.res[i] = resources[i];
    }
    r.seq.store(seq, std::memory_order_release);
}

void NoteFreed(uint64_t handle, uint8_t type) noexcept {
    if (!On() || handle == 0) {
        return;
    }
    Freed& f = FreedRing()[g_freed_next.fetch_add(1, std::memory_order_relaxed) & kFreedMask];
    f.handle.store(0, std::memory_order_relaxed);
    f.seq = g_next.load(std::memory_order_relaxed) - 1;
    f.type = type;
    f.handle.store(handle, std::memory_order_release);
}

void RegisterQueue(vk::Queue queue, const char* name) noexcept {
    if (!queue) {
        return;
    }
    std::scoped_lock lock{g_queues_mutex};
    const VkQueue raw = static_cast<VkQueue>(queue);
    for (const auto& q : g_queues) {
        if (q.queue == raw) {
            return; // the same queue under another name (present = graphics)
        }
    }
    g_queues.push_back({raw, name});
}

void Report(const uint64_t* fault_addresses, uint32_t fault_count) noexcept {
    static std::atomic<bool> reported{false};
    if (!On() || reported.exchange(true)) {
        return;
    }
    g_fault_addrs = fault_addresses;
    g_fault_count = fault_addresses ? fault_count : 0;
    const auto get_data = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV;
    if (!get_data) {
        std::fprintf(stderr, "SOTC GPU CHECKPOINTS: vkGetQueueCheckpointDataNV not loaded, no report\n");
        return;
    }
    const uint64_t last_noted = g_next.load(std::memory_order_relaxed) - 1;
    std::fprintf(stderr, "SOTC GPU CHECKPOINTS: device lost; last command noted #%llu\n",
                 static_cast<unsigned long long>(last_noted));
    std::vector<QueueSlot> queues;
    {
        std::scoped_lock lock{g_queues_mutex};
        queues = g_queues;
    }
    for (const auto& q : queues) {
        uint32_t count = 0;
        get_data(q.queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        if (count) {
            get_data(q.queue, &count, data.data());
        }
        // The driver reports, per pipeline stage, the last checkpoint that stage passed. Started: the highest marker
        // at the top of the pipe; finished: the highest at the bottom. The commands between them were in flight.
        uint64_t top = 0, bottom = 0;
        for (const auto& d : data) {
            const uint64_t raw = reinterpret_cast<uint64_t>(d.pCheckpointMarker);
            if (raw & kAfterBit) {
                std::fprintf(stderr, "  queue %s: stage %#x at the END of #%llu (the GPU went past that command)\n", q.name,
                             static_cast<unsigned>(d.stage), static_cast<unsigned long long>(raw & ~kAfterBit));
            }
            const uint64_t marker = raw & ~kAfterBit;
            if (d.stage & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) {
                top = std::max(top, marker);
            }
            if (d.stage & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) {
                bottom = std::max(bottom, marker);
            }
            std::fprintf(stderr, "  queue %s: stage %#x at #%llu\n", q.name, static_cast<unsigned>(d.stage),
                         static_cast<unsigned long long>(marker));
        }
        if (top == 0 && bottom == 0) {
            std::fprintf(stderr, "  queue %s: no checkpoint reached\n", q.name);
            continue;
        }
        std::fprintf(stderr, "  queue %s: last started #%llu, last finished #%llu\n", q.name,
                     static_cast<unsigned long long>(top), static_cast<unsigned long long>(bottom));
        // Checkpoint s is recorded right before command s. Its marker at the bottom of the pipe: every command before
        // it finished; at the top: command s started. So commands [bottom, top] were in flight (the faulting one among
        // them, or an unmarked copy/barrier recorded between two of them). The few before, the ones in flight (at most
        // 64), and the next few not started.
        const uint64_t from = bottom > 4 ? bottom - 4 : 1;
        const uint64_t in_flight_end = std::min(std::max(top, bottom), from + 4 + 64);
        for (uint64_t s = from; s <= in_flight_end + 3 && s <= last_noted; ++s) {
            const char* tag = s < bottom ? "done" : s <= top ? "IN FLIGHT" : "not started";
            PrintEntry(s, tag);
        }
    }
    std::fflush(stderr);
}

} // namespace SotcCheckpoints
