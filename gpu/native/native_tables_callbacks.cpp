// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.10 (native-tables): the entry
// callbacks of the table / block writers.
//   0x95320  root-table builder (per draw: 0x11d3d0/0x11d5b0 and 8 more runners)
//   0xae150  user-data (SGPR) table binder (per draw)
//   0x123cc0 instance/object block thunk (-> 0x123810)
//   0x123810 instance/object block writer proper (0x123cc0's target)
//   0xca6d0  per-view draw-table double-buffer copy (frame N -> N+1, before MainCamera)
//
// Every one of these helpers MUTATES guest state (0x95320 bump-allocates the packed table, 0x123810
// writes the instance-V#/stencil caches, 0xca6d0 copies the draw tables and bumps ctx+0xf4) or is
// the only path that performs staging the emulated draws depend on (0xae150 -> 0x862490 SET_SH_REG
// of the root-table pointer into every active stage). None can be served from its derivation yet:
// each callback derives the plan READ-ONLY, traces it under SOTC_RENDER_VERBOSE=1 (budgeted) and
// chains to the guest body, which performs the real work. The plans are what the native commit
// paths will consume once the draw/bind emission gates open (N2-2 fullscreen pipeline, N2-10
// pipeline fill).
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"
#include "native/native_tables.h"
#include "sotc/hooks.h"

namespace Native {

namespace {

void* Original(u64 va) {
    return Core::SotcHooks::OriginalEntry(va);
}

bool Verbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}

// Per-entry budgets: 0x95320/0xae150/0x123cc0 run once per draw record, so a small budget keeps the
// log useful without flooding; 0x123810 (the same calls through the thunk) and 0xca6d0 (once per
// frame) are tiny.
std::atomic<u64> g_budget_95320{32};
std::atomic<u64> g_budget_ae150{32};
std::atomic<u64> g_budget_123cc0{32};
std::atomic<u64> g_budget_123810{16};
std::atomic<u64> g_budget_ca6d0{8};

bool Allowed(std::atomic<u64>& budget) {
    return Verbose() && budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

} // namespace

// -- SysV signatures (from the decomp; the register mapping is what the hook's ABI depends on) ---

// void f(u32 mask, u64* src, u32 tag): edi = mask (u32), rsi = src, edx = tag (00095320.c:3).
typedef u64 (*GuestRootTableFn)(u32, u64*, u32) __attribute__((sysv_abi));

// void f(long* binder, u64 table): rdi = the PASS context, rsi = the root-table address
// (000ae150.c:3; 0x11d3d0:53 passes `puVar2` = *param_3 = P).
typedef void (*GuestUserDataFn)(u64*, u64) __attribute__((sysv_abi));

// void f(long rec2, long* ctx, int index, char flag): rdi = renderable, rsi = destination object,
// edx = index, ecx = flag (000123cc0.c: the thunk forwards all four unchanged).
typedef void (*GuestInstThunkFn)(u64, u64*, s32, s8) __attribute__((sysv_abi));

// void f(long renderable, long* dest, int index, char flag, uint* inst): rdi/rsi/edx/ecx/r8
// (00123810.c:3). 0x123cc0 passes r9 = renderable+0x90 (raw: `lea r9,[rdi+0x90]`), r8 =
// renderable+0x68, and the 5th stack argument = renderable+0xB8 (raw: `lea rax,[rdi+0xB8]; mov
// [rsp],rax`), i.e. index/flag come through rdx/rcx exactly as in the thunk.
typedef void (*GuestInstWriteFn)(u64, u64*, s32, s8, const u32*) __attribute__((sysv_abi));

// void f(long R, long F, long* ctx): rdi/rsi/rdx (000ca6d0.c:3).
typedef void (*GuestDrawTablesFn)(u64, u64, u64*) __attribute__((sysv_abi));

} // namespace Native

// u32 f(u32 mask, u64* src, u32 tag) (00095320.c:3). The decompile declares void, but 0x11d3d0:52
// consumes the returned table address (bind doc §2.7 / §6 item 1: rax survives to ret) — the
// callback returns it so the chained allocation pointer reaches the caller unchanged.
extern "C" __attribute__((sysv_abi)) unsigned long long NativeTables_95320(
    unsigned* src, unsigned mask, unsigned tag) {
    using namespace Native;
    const u32 table_id = mask;
    const u64 root_slot = reinterpret_cast<u64>(src);
    const u64 static_table = tag; // DAT_02ed1f54 in the draw loop (an arena/tag id, not a pointer)
    if (Allowed(g_budget_95320)) {
        RootTablePlan plan;
        const char* gate = DeriveRootTable(table_id, root_slot, static_table, plan);
        if (gate == nullptr) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x95320 mask=%#x slots=%u bytes=%#x src=%#llx "
                         "static=%#llx table=guest-allocation guest chain (derived)\n", table_id,
                         plan.slot_count, plan.bytes, static_cast<unsigned long long>(root_slot),
                         static_cast<unsigned long long>(static_table));
        } else {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x95320 mask=%#x src=%#llx guest chain (%s)\n",
                         table_id, static_cast<unsigned long long>(root_slot), gate);
        }
    }
    return reinterpret_cast<GuestRootTableFn>(Original(0x95320))(table_id,
                                                                 reinterpret_cast<u64*>(root_slot),
                                                                 tag);
}

// void f(long* binder, u64 table) (000ae150.c:3).
extern "C" __attribute__((sysv_abi)) void NativeTables_ae150(void* binder,
                                                             unsigned long long table) {
    using namespace Native;
    const u64 p = reinterpret_cast<u64>(binder);
    const u64 t = static_cast<u64>(table);
    if (Allowed(g_budget_ae150)) {
        UserDataTablePlan plan;
        const char* gate = DeriveUserDataTable(p, t, plan);
        if (gate == nullptr) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0xae150 P=%#llx table=%#llx mask=%#x stages=%u "
                         "written=%u guest chain (derived)\n",
                         static_cast<unsigned long long>(p), static_cast<unsigned long long>(t),
                         plan.enable_mask, plan.stage_count, plan.written_count);
            for (u32 i = 0; i < plan.stage_count; ++i) {
                const UserDataSlot& s = plan.slot[i];
                std::fprintf(stderr,
                             "SOTC RENDER native:   0xae150 bit=%u stage=%u rec=%#llx type=%u "
                             "write=%u sgpr=%u dwords=%u\n",
                             s.bit, s.stage, static_cast<unsigned long long>(s.record),
                             static_cast<unsigned>(s.record_type), s.written ? 1u : 0u, s.sgpr,
                             s.dwords);
            }
        } else {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0xae150 P=%#llx table=%#llx guest chain (%s)\n",
                         static_cast<unsigned long long>(p), static_cast<unsigned long long>(t),
                         gate);
        }
    }
    reinterpret_cast<GuestUserDataFn>(Original(0xae150))(static_cast<u64*>(binder), t);
}

// void f(long rec2, long* ctx, int index, char flag) (via the 0x123cc0 thunk, 000123cc0.c:6).
extern "C" __attribute__((sysv_abi)) void NativeTables_123cc0(void* rec2, void* ctx, int index,
                                                              char flag) {
    using namespace Native;
    const u64 record = reinterpret_cast<u64>(rec2);
    const u64 dest = reinterpret_cast<u64>(ctx);
    if (Allowed(g_budget_123cc0)) {
        InstanceBindPlan plan;
        u64 out_desc = 0;
        u32 stride = 0;
        const char* gate = DeriveInstanceBind(record, dest, static_cast<u32>(index), flag != 0, plan);
        // The thunk's instance pair: renderable+0x90 (raw 0x123cc0 `lea r9,[rdi+0x90]`).
        const char* block_gate =
            DeriveInstanceBlock(record, dest, static_cast<u32>(index), flag != 0, record + 0x90,
                                &out_desc, &stride);
        if (gate == nullptr && block_gate == nullptr) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x123cc0 rec=%#llx dest=%#llx idx=%d flag=%u "
                         "inst=%#llx+0xa8 stride=%#x k=%u stencil=%u guest chain (derived)\n",
                         static_cast<unsigned long long>(record),
                         static_cast<unsigned long long>(dest), index, flag != 0 ? 1u : 0u,
                         static_cast<unsigned long long>(out_desc), stride, plan.stencil_path ? 1u : 0u,
                         plan.stencil_write ? 1u : 0u);
        } else {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x123cc0 rec=%#llx dest=%#llx guest chain (%s)\n",
                         static_cast<unsigned long long>(record),
                         static_cast<unsigned long long>(dest), gate != nullptr ? gate : block_gate);
        }
    }
    reinterpret_cast<GuestInstThunkFn>(Original(0x123cc0))(record, reinterpret_cast<u64*>(dest),
                                                           index, flag);
}

// void f(long renderable, long* dest, int index, char flag, uint* inst) (00123810.c:3). rdx/rcx are
// index/flag exactly as the thunk passes them; the other pointer arguments are forwarded verbatim.
extern "C" __attribute__((sysv_abi)) void NativeTables_123810(void* renderable, void* dest, int index,
                                                              char flag, const unsigned* inst) {
    using namespace Native;
    const u64 record = reinterpret_cast<u64>(renderable);
    const u64 d = reinterpret_cast<u64>(dest);
    if (Allowed(g_budget_123810)) {
        InstanceBindPlan plan;
        const char* gate = DeriveInstanceBind(record, d, static_cast<u32>(index), flag != 0, plan);
        if (gate == nullptr) {
            const u64 pair = reinterpret_cast<u64>(inst);
            const u32 lo = pair != 0 ? *reinterpret_cast<const u32*>(pair) : 0;
            const u32 hi = pair != 0 ? *reinterpret_cast<const u32*>(pair + 4) : 0;
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x123810 rec=%#llx dest=%#llx idx=%d flag=%u qword=%#llx "
                         "srcs=%u guest chain (derived)\n",
                         static_cast<unsigned long long>(record),
                         static_cast<unsigned long long>(d), index, flag != 0 ? 1u : 0u,
                         static_cast<unsigned long long>(lo | (static_cast<u64>(hi & 0xfff) << 32)),
                         plan.optional_scope ? 1u : 0u);
        } else {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0x123810 rec=%#llx dest=%#llx guest chain (%s)\n",
                         static_cast<unsigned long long>(record),
                         static_cast<unsigned long long>(d), gate);
        }
    }
    reinterpret_cast<GuestInstWriteFn>(Original(0x123810))(
        record, reinterpret_cast<u64*>(d), index, flag, inst);
}

// void f(long R, long F, long* ctx) (000ca6d0.c:3).
extern "C" __attribute__((sysv_abi)) void NativeTables_ca6d0(void* r, void* f, void* ctx) {
    using namespace Native;
    const u64 R = reinterpret_cast<u64>(r);
    const u64 F = reinterpret_cast<u64>(f);
    const u64 c = reinterpret_cast<u64>(ctx);
    if (Allowed(g_budget_ca6d0)) {
        DrawTablesPlan plan;
        const char* gate = DeriveDrawTables(R, F, c, plan);
        if (gate == nullptr) {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0xca6d0 R=%#llx F=%#llx ctx=%#llx copies=%u "
                         "clear8f8=%u epoch=%u guest chain (derived)\n",
                         static_cast<unsigned long long>(R), static_cast<unsigned long long>(F),
                         static_cast<unsigned long long>(c), plan.copy_count,
                         plan.clear_8f8 ? 1u : 0u, plan.epoch_write ? 1u : 0u);
        } else {
            std::fprintf(stderr,
                         "SOTC RENDER native: 0xca6d0 R=%#llx ctx=%#llx guest chain (%s)\n",
                         static_cast<unsigned long long>(R), static_cast<unsigned long long>(c),
                         gate);
        }
    }
    reinterpret_cast<GuestDrawTablesFn>(Original(0xca6d0))(R, F, reinterpret_cast<u64*>(c));
}
