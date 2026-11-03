// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_XPR_GROUPS=1 (off by default; measurement only, nothing is changed). The P0 measurement of the plan to
// coalesce the XPR per-object draws (docs/re-backend/16-draw-path-gaps.md, sections 1, 2 and 7).
//
// At the entry of the XPR draw loop 0x1a6d20 (out/re/decomp/001a/001a6d20.c) the loop is walked on the host from
// the same guest data, in the same order, without drawing:
//   - count = u16 at set+0x64; the visibility mask (rsi) holds (count + 63) / 64 qwords;
//   - plVar5 = *(set+0x50): [0] u16 part boundaries (set+0x60 low u16 of them), [1] u16 mesh-group boundaries
//     (set+0x60 high u16), [4] u32 membership per object, tested with bit (u32 at draw+0x20) & 31;
//   - a set bit whose membership bit is set is a draw (DRAW_INDEX_INDIRECT at args + objIdx * 0x14, the args base
//     is *(set+0x40)); the part and the mesh group are the forward scans of the two boundary arrays;
//   - a group is a run of draws with the same (part, mesh group): one material and one mesh, so its draws differ only
//     in their args record.
// Per group it counts the length and the holes (object indices skipped inside the group's span). The CPU view of
// the args records is read as well: all zero means the records are written by the GPU only (SOTC_NATIVE keeps GPU
// writes out of guest memory), which decides whether a merge can read them on the CPU or must gather them on the
// GPU. XPR_CULL_JOB / XPR_CULL_CASCADE count the callers of 0x11dbe0, the only known writer of the records.
// One line every 5 s: "SOTC XPR GROUPS: ...".

#include "probe_xpr_groups.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "guest_mem.h"
#include "hooks.h"

namespace SotcXprGroups {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint32_t kMaxObjects = 2048; // the 2048-bit mask
constexpr uint64_t kRecordBytes = 0x14;
// Group length buckets: 1, 2-3, 4-7, 8-15, 16-31, 32+.
constexpr int kBuckets = 6;

struct Counters {
    std::atomic<uint64_t> calls{0}, calls_ind{0}, cull_calls{0}, unreadable{0}, host_masks{0}, empty{0};
    std::atomic<uint64_t> draws{0}, groups{0}, holes{0}, span{0};
    std::atomic<uint64_t> max_group{0};
    std::atomic<uint64_t> len_bucket[kBuckets]{};
    std::atomic<uint64_t> kind0{0}, kind6{0}, kind4{0}, kind_other{0};
    std::atomic<uint64_t> rec_read{0}, rec_zero{0}, rec_inst0{0};
    std::atomic<uint64_t> hole_read{0}, hole_zero{0};
    std::atomic<uint64_t> part_changes{0}, mesh_changes{0};
};
Counters g;

int Bucket(uint64_t len) {
    int b = 0;
    while (b + 1 < kBuckets && len >= (uint64_t{2} << b)) {
        ++b;
    }
    return b;
}

void MaxInto(std::atomic<uint64_t>& to, uint64_t value) {
    uint64_t cur = to.load(std::memory_order_relaxed);
    while (value > cur && !to.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

HookAction OnIndexCull(GuestRegs&, void*) {
    g.cull_calls.fetch_add(1, std::memory_order_relaxed);
    return HookAction::Continue;
}

// sotc: SOTC_DRAW_CALLERS=1 (same file, measurement only). Entry hooks on the engine's draw and dispatch wrappers
// (BPGnm_DrawIndexIndirect 0x86b3d0, the DrawIndexOffset pair 0x86b040 / 0x86afd0, BPGnm_DispatchDirect 0x86b9e0)
// count each call by its exact caller, the return address at [rsp] on entry. SOTC_DRAW_SITES scans the stack for
// return addresses instead and picks up stale ones, so its chains name functions that never draw.
enum WrapperKind : uint32_t { kIndirect, kOffset, kOffset2, kDispatch, kWrapperKinds };
constexpr const char* kWrapperNames[kWrapperKinds] = {"indirect", "offset", "offset2", "dispatch"};
constexpr size_t kCallerSlots = 4096;
struct CallerSlot {
    std::atomic<uint64_t> key{0}; // (kind << 32) | (caller image offset + 1); 0 = empty
    std::atomic<uint64_t> calls{0};
};
CallerSlot g_callers[kCallerSlots];
std::atomic<uint64_t> g_caller_overflow{0};
std::atomic<uint64_t> g_wrapper_calls[kWrapperKinds]{};

bool DrawCallersOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DRAW_CALLERS");
        return v && v[0] == '1';
    }();
    return on;
}

HookAction OnWrapper(GuestRegs& regs, void* user) {
    const auto kind = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    g_wrapper_calls[kind].fetch_add(1, std::memory_order_relaxed);
    uint64_t ret = 0;
    const uint64_t base = SotcHooks::ImageBase();
    if (!SotcGuest::Read(regs.rsp, &ret) || base == 0 || ret < base) {
        return HookAction::Continue;
    }
    const uint64_t key = (uint64_t{kind} << 32) | ((ret - base) + 1);
    size_t i = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 52) & (kCallerSlots - 1);
    for (size_t n = 0; n < kCallerSlots; ++n, i = (i + 1) & (kCallerSlots - 1)) {
        uint64_t cur = g_callers[i].key.load(std::memory_order_relaxed);
        if (cur == 0 && g_callers[i].key.compare_exchange_strong(cur, key, std::memory_order_relaxed)) {
            cur = key;
        }
        if (cur == key) {
            g_callers[i].calls.fetch_add(1, std::memory_order_relaxed);
            return HookAction::Continue;
        }
    }
    g_caller_overflow.fetch_add(1, std::memory_order_relaxed);
    return HookAction::Continue;
}

void ReportCallers(std::string& out, double seconds) {
    if (!DrawCallersOn()) {
        return;
    }
    struct Row {
        uint64_t key, calls;
    };
    std::vector<Row> rows;
    for (auto& slot : g_callers) {
        const uint64_t key = slot.key.load(std::memory_order_relaxed);
        const uint64_t calls = slot.calls.exchange(0, std::memory_order_relaxed);
        if (key != 0 && calls != 0) {
            rows.push_back({key, calls});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.calls > b.calls; });
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC DRAW CALLERS (%.1f s): indirect %.0f/s, offset %.0f/s, offset2 %.0f/s, dispatch %.0f/s, "
                  "%zu callers (table overflow %llu)\n",
                  seconds, g_wrapper_calls[kIndirect].exchange(0) / seconds,
                  g_wrapper_calls[kOffset].exchange(0) / seconds, g_wrapper_calls[kOffset2].exchange(0) / seconds,
                  g_wrapper_calls[kDispatch].exchange(0) / seconds, rows.size(),
                  (unsigned long long)g_caller_overflow.exchange(0));
    out += line;
    for (size_t r = 0; r < rows.size() && r < 30; ++r) {
        const uint32_t kind = static_cast<uint32_t>(rows[r].key >> 32);
        const uint64_t caller = (rows[r].key & 0xffffffffull) - 1;
        std::snprintf(line, sizeof line, "  %-8s %9.0f/s  caller eboot+%#llx\n", kWrapperNames[kind],
                      rows[r].calls / seconds, (unsigned long long)caller);
        out += line;
    }
}

struct Record {
    uint32_t index_count, instance_count, first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
};
static_assert(sizeof(Record) == kRecordBytes);

bool IsZero(const Record& r) {
    return r.index_count == 0 && r.instance_count == 0 && r.first_index == 0 && r.vertex_offset == 0 &&
           r.first_instance == 0;
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_XPR_GROUPS");
        return v && v[0] == '1';
    }();
    return on;
}

void OnConsumeIndirect() noexcept {
    if (On()) {
        g.calls_ind.fetch_add(1, std::memory_order_relaxed);
    }
}

void OnConsume(const GuestRegs& regs) noexcept {
    if (!On()) {
        return;
    }
    g.calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t set = regs.rdi, mask_addr = regs.rsi, draw = regs.rdx, ctx_pp = regs.rcx;
    uint16_t count = 0;
    uint64_t plv = 0, args = 0;
    uint32_t bounds = 0, shift = 0;
    if (!SotcGuest::Read(set + 0x64, &count) || !SotcGuest::Read(set + 0x50, &plv) ||
        !SotcGuest::Read(set + 0x40, &args) || !SotcGuest::Read(set + 0x60, &bounds) ||
        !SotcGuest::Read(draw + 0x20, &shift)) {
        g.unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (count == 0 || count > kMaxObjects) {
        return;
    }
    uint64_t arrays[5]{};
    if (!SotcGuest::Read(plv, arrays, sizeof arrays)) {
        g.unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t parts = bounds & 0xffff, meshes = bounds >> 16;
    thread_local std::vector<uint64_t> mask;
    thread_local std::vector<uint32_t> member;
    thread_local std::vector<uint16_t> part_bounds, mesh_bounds;
    const uint32_t qwords = (count + 63u) / 64u;
    mask.resize(qwords);
    member.resize(count);
    part_bounds.resize(parts);
    mesh_bounds.resize(meshes);
    // SOTC_NATIVE_MASKS=prev hands the loop a mask in host memory (the native copies ring), outside guest memory:
    // the loop reads it at this address right after the hook, so it is read the same way here.
    if (!SotcGuest::Read(mask_addr, mask.data(), qwords * 8u)) {
        if (mask_addr == 0) {
            g.unreadable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(mask.data(), reinterpret_cast<const void*>(mask_addr), qwords * 8u);
        g.host_masks.fetch_add(1, std::memory_order_relaxed);
    }
    if (!SotcGuest::Read(arrays[4], member.data(), count * 4u) ||
        (parts && !SotcGuest::Read(arrays[0], part_bounds.data(), parts * 2u)) ||
        (meshes && !SotcGuest::Read(arrays[1], mesh_bounds.data(), meshes * 2u))) {
        g.unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The shader kind of the draw call (ctx+0xc548 of *(*rcx)), as 0x1a6d20 reads it before each draw.
    uint64_t ctx = 0;
    int32_t kind = -1;
    if (SotcGuest::Read(ctx_pp, &ctx) && ctx) {
        uint64_t cursor = 0;
        if (SotcGuest::Read(ctx, &cursor) && cursor) {
            SotcGuest::Read(cursor + 0xc548, &kind);
        }
    }
    uint64_t draws = 0, groups = 0, holes = 0, span = 0, part_changes = 0, mesh_changes = 0;
    uint64_t rec_read = 0, rec_zero = 0, rec_inst0 = 0, hole_read = 0, hole_zero = 0;
    uint64_t buckets[kBuckets]{};
    uint64_t max_group = 0;
    uint32_t part = 0, mesh = 0;
    uint32_t cur_part = 0xffffffff, cur_mesh = 0xffffffff;
    uint64_t group_len = 0, group_first = 0, group_last = 0;
    const auto close_group = [&] {
        if (group_len == 0) {
            return;
        }
        ++groups;
        ++buckets[Bucket(group_len)];
        if (group_len > max_group) {
            max_group = group_len;
        }
        span += group_last - group_first + 1;
        group_len = 0;
    };
    for (uint32_t idx = 0; idx < count; ++idx) {
        if (((mask[idx >> 6] >> (idx & 63)) & 1) == 0) {
            continue;
        }
        if (((member[idx] >> (shift & 31)) & 1) == 0) {
            continue;
        }
        while (part < parts && idx >= part_bounds[part]) {
            ++part;
        }
        while (mesh < meshes && idx >= mesh_bounds[mesh]) {
            ++mesh;
        }
        const bool new_part = part != cur_part, new_mesh = mesh != cur_mesh;
        if (new_part || new_mesh) {
            close_group();
            part_changes += new_part;
            mesh_changes += new_mesh;
            cur_part = part;
            cur_mesh = mesh;
            group_first = idx;
        } else if (idx != group_last + 1) {
            holes += idx - group_last - 1;
            // The records the group's span skips over: a draw over the span would read them.
            for (uint64_t h = group_last + 1; h < idx && hole_read < 64; ++h) {
                Record r{};
                if (SotcGuest::Read(args + h * kRecordBytes, &r)) {
                    ++hole_read;
                    hole_zero += IsZero(r) || r.instance_count == 0;
                }
            }
        }
        group_last = idx;
        ++group_len;
        ++draws;
        Record r{};
        if (SotcGuest::Read(args + uint64_t{idx} * kRecordBytes, &r)) {
            ++rec_read;
            rec_zero += IsZero(r);
            rec_inst0 += r.instance_count == 0;
        }
    }
    close_group();
    if (draws == 0) {
        g.empty.fetch_add(1, std::memory_order_relaxed);
    }
    g.draws.fetch_add(draws, std::memory_order_relaxed);
    g.groups.fetch_add(groups, std::memory_order_relaxed);
    g.holes.fetch_add(holes, std::memory_order_relaxed);
    g.span.fetch_add(span, std::memory_order_relaxed);
    g.part_changes.fetch_add(part_changes, std::memory_order_relaxed);
    g.mesh_changes.fetch_add(mesh_changes, std::memory_order_relaxed);
    g.rec_read.fetch_add(rec_read, std::memory_order_relaxed);
    g.rec_zero.fetch_add(rec_zero, std::memory_order_relaxed);
    g.rec_inst0.fetch_add(rec_inst0, std::memory_order_relaxed);
    g.hole_read.fetch_add(hole_read, std::memory_order_relaxed);
    g.hole_zero.fetch_add(hole_zero, std::memory_order_relaxed);
    for (int b = 0; b < kBuckets; ++b) {
        g.len_bucket[b].fetch_add(buckets[b], std::memory_order_relaxed);
    }
    MaxInto(g.max_group, max_group);
    auto& kind_counter = kind == 0 ? g.kind0
                         : (kind == 0x45 || kind == 0xad || kind == 0x145 || kind == 0x1ad) ? g.kind6
                         : kind == 0xb0                                                     ? g.kind4
                                                                                            : g.kind_other;
    kind_counter.fetch_add(draws, std::memory_order_relaxed);
}

void Register() {
    SotcHooks::Handler handler;
    handler.pre = &OnIndexCull;
    SotcHooks::RegisterHandler("XPR_CULL_JOB", handler);
    SotcHooks::RegisterHandler("XPR_CULL_CASCADE", handler);
    constexpr const char* kWrapperHooks[kWrapperKinds] = {"DRAW_WRAP_INDIRECT", "DRAW_WRAP_OFFSET",
                                                          "DRAW_WRAP_OFFSET2", "DRAW_WRAP_DISPATCH"};
    for (uint32_t k = 0; k < kWrapperKinds; ++k) {
        SotcHooks::Handler wrapper;
        wrapper.pre = &OnWrapper;
        wrapper.user = reinterpret_cast<void*>(static_cast<uintptr_t>(k));
        SotcHooks::RegisterHandler(kWrapperHooks[k], wrapper);
    }
}

void Report(std::string& out, double seconds) {
    ReportCallers(out, seconds);
    if (!On()) {
        return;
    }
    const auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const uint64_t calls = take(g.calls), calls_ind = take(g.calls_ind), cull = take(g.cull_calls);
    const uint64_t draws = take(g.draws), groups = take(g.groups), holes = take(g.holes), span = take(g.span);
    const uint64_t parts = take(g.part_changes), meshes = take(g.mesh_changes);
    const uint64_t rec_read = take(g.rec_read), rec_zero = take(g.rec_zero), rec_inst0 = take(g.rec_inst0);
    const uint64_t hole_read = take(g.hole_read), hole_zero = take(g.hole_zero);
    const uint64_t max_group = take(g.max_group), unreadable = take(g.unreadable);
    const uint64_t host_masks = take(g.host_masks), empty = take(g.empty);
    uint64_t b[kBuckets];
    for (int i = 0; i < kBuckets; ++i) {
        b[i] = take(g.len_bucket[i]);
    }
    const uint64_t k0 = take(g.kind0), k6 = take(g.kind6), k4 = take(g.kind4), kx = take(g.kind_other);
    char line[768];
    std::snprintf(line, sizeof line,
                  "SOTC XPR GROUPS (%.1f s): consume calls %llu (alternate 0x1a7290 %llu, index cull jobs %llu, "
                  "unreadable %llu, host masks %llu, no draw %llu); draws %llu in %llu groups (avg %.2f, max %llu; len 1/2-3/4-7/8-15/16-31/32+: "
                  "%llu/%llu/%llu/%llu/%llu/%llu); part changes %llu, mesh changes %llu; holes inside groups %llu "
                  "(members %.1f%% of the spans); kind 0/6/4/other draws %llu/%llu/%llu/%llu; CPU view of args "
                  "records: %llu read, %llu all zero, %llu instanceCount 0; hole records %llu read, %llu zero or no "
                  "instance\n",
                  seconds, (unsigned long long)calls, (unsigned long long)calls_ind, (unsigned long long)cull,
                  (unsigned long long)unreadable, (unsigned long long)host_masks, (unsigned long long)empty,
                  (unsigned long long)draws, (unsigned long long)groups,
                  groups ? double(draws) / double(groups) : 0.0, (unsigned long long)max_group,
                  (unsigned long long)b[0], (unsigned long long)b[1], (unsigned long long)b[2],
                  (unsigned long long)b[3], (unsigned long long)b[4], (unsigned long long)b[5],
                  (unsigned long long)parts, (unsigned long long)meshes, (unsigned long long)holes,
                  span ? 100.0 * double(draws) / double(span) : 0.0, (unsigned long long)k0,
                  (unsigned long long)k6, (unsigned long long)k4, (unsigned long long)kx,
                  (unsigned long long)rec_read, (unsigned long long)rec_zero, (unsigned long long)rec_inst0,
                  (unsigned long long)hole_read, (unsigned long long)hole_zero);
    out += line;
}

} // namespace SotcXprGroups
