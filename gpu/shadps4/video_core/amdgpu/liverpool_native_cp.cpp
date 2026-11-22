// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native command processor (W3, SOTC_NATIVE_CP, N2-94 plan): runs an all-native submission
// from its OpList without parsing PM4 or using a coroutine. Owner: H03.
//
// Coverage table (N2-94 H03, W3):
//   item                                                        | done? | evidence
//   ------------------------------------------------------------+-------+--------------------------------
//   RunNativeEntry replaces the step 0 stub                      | yes   | RunNativeEntry below
//   first call: prefetch then graphics cblock.Reset (as ProcessGraphics) | yes | started flag, prefetch_shadows
//   first call: prefetch of the top-level shadows when rasterizer| yes   | PrefetchComputeShaders(body+1)
//     and SOTC_COMPUTE_PREFETCH on (cs copy = mapped_queues[vqid])| yes  | prefetch_shadows lambda
//   IB shadows walked when the IB is reached (lazy OpList)       | yes   | Ib item branch
//   Op bit 30 set: Native::StreamOps::Run inline                  | yes   | Op branch
//   Op bit 30 clear: draw-thread run batched per consecutive run | yes   | NativeBackRunIds(ids,n,any bit31)
//   Compute: native_asc_pipe_id = asc_queues[{vqid-1}].pipe_id   | yes   | set before the list runs
//   Wait: Poll; false -> polls/park_start_ns, Parked, cursor kept| yes   | Wait branch, finish_wait
//   Wait true -> WaitFinished(First if 1 poll else Yield)        | yes   | finish_wait
//   WaitCe: CE items from ce_cursor until Poll holds             | yes   | ce_run(has_id=true)
//   WaitCe: CE parked -> Parked                                  | yes   | ce_run returns 1
//   WaitCe: CE list exhausted, Poll false -> Parked (SPEC)       | yes   | ce_run returns 2, WaitCe branch (note 1)
//   CE Op: always inline, bit 30 ignored (deliberate, note 4)    | yes   | ce_run Op branch
//   CE Wait: false parks the CE cursor                            | yes   | ce_run Wait branch
//   CE Ib (IndirectBufferConst is PM4 only): counted fallback   | yes   | ce_run Ib branch (note 2)
//   Ib: IbFrame{BuildOpList} pushed, continues inside it          | yes   | Ib branch
//   Ib: BuildOpList fails -> OpListsFallback + LOG_ERROR once     | yes   | Ib branch
//   Remaining CE drained after the DE list (ProcessGraphics tail)| yes   | drain after DE loop
//   Compute: AdvanceReadPointer at each Parked return (by the    | yes   | N2-97 integration: OpItem::dw_end +
//     dwords already consumed) and the rest at Done              |       | release_consumed (note 3)
//   REA check of the game's ring-wait functions (N2-94 H03 fixer r4) | partial | REA document "target-91d23a4842cd.exe"
//     (same sha256 91d23a48...4dabc92 as out/re/eboot_pe_fde.exe; the file-name selector is rejected);
//     procedure_pseudo_code 0x8636e0 OK (ev_471939be...); 0x863310 and 0x85d380 via decomp files only.
//     Raw bytes at out/eboot.elf VA+0x4000: 0x8636e0 -> 55 48 89 e5 41 57 ... (push rbp; mov rbp,rsp);
//     0x863310 -> 55 48 89 e5 41 57 ...; 0x85d380 -> 55 53 48 89 c8 ... : all three are function entries.
//   Read-pointer write target identical to the PM4 path (step 0 legacy): ProcessCompute calls
//     AdvanceReadPointer(queue.read_addr, next_dw_off, ...) after every packet (liverpool.cpp ~2939), so
//     the native Done write hits the same asc.read_addr field that the shipping path already writes.
//     Whether that field reaches the game's param_1[0x26] is NOT established (no store to [0x26] of the
//     ring struct from the CP side is visible in the decompile; 0x85d380 only calls sceGnmDingDong with
//     param_1[0x18]). This is an open point shared with the legacy path, not introduced here. Status: open.
//   Done only when list, ib_stack and ce_list are exhausted      | yes   | return paths
//   cp_progress = true whenever at least one item ran            | yes   | ran flag, return paths
//   Prefetch gate: local ComputePrefetchOn() duplicates the      | yes   | same logic as static
//     static ComputePrefetchEnabled() in liverpool.cpp (that one  |       | ComputePrefetchEnabled in liverpool.cpp; export
//     is file-static, not reachable from this TU; core request)   |       | it from liverpool.h to share (not owned, not done)
//
// Notes:
//   1. WaitCe follows the SPEC: when the CE list is exhausted and Poll(id) is still false, the entry
//      returns Parked and is re-polled on the next progress. An earlier revision passed the wait
//      here. That was a bug, not a nit: draw-thread ops queued by NativeBackRunIds run asynchronously
//      and may write the label after the CE list has ended, so passing could run past an unsatisfied
//      wait. ProcessGraphics (legacy PM4 path) still exits early; the native path does not.
//   2. The CCB of a graphics submission is built from CE packets replaced by native ops; an
//      IndirectBufferConst is a PM4 packet, never a native IB, so an Ib item in ce_list does not
//      occur. If one appears it is skipped, counted in OpListsFallback, and logged once.
//   3. Compute ring release (N2-94 H03 fixer round 2). Decompile evidence (out/re/decomp/0086/):
//      - 00863310.c lines 81-87 (sceGnmDingDong wrapper) DO read the ring fields: they compute free
//        space as param_1[0x12] - param_1[0x26] and read param_1[0x24] / param_1[0x25] / param_1[10].
//        The earlier claim that 0x863310 "does not read the ring read pointer itself" was WRONG.
//      - 00863af0.c lines 51-55 read param_6[0x24], [0x25], [0x26] and +0x2e4 the same way.
//      - 008636e0.c lines 84-122 (compute submit) compute free space as param_1[0x12] - param_1[0x26]
//        and, when the ring is full, loop on FUN_0085d380 ("ComputeQueue ... is full; waiting for
//        space") until it reports room.
//      Open point: which of these fields is the one AdvanceReadPointer writes (asc.read_addr). The
//      decompile does not establish that mapping. asc.read_addr is the guest pointer the game passed
//      to sceGnmMapComputeQueue (gnmdriver.cpp ~1348, ~1355), so the CP writes the game's read
//      pointer. In 00863310.c / 008636e0.c, [0x26] is used with the ring base [2], the cursor [0x24]
//      and the size [0x12] as a game-side ring cursor. Nothing in the decompile shows it is refreshed
//      from *read_ptr_addr, so it is NOT established that the CP's write reaches [0x26].
//      The REA cross-check (raw bytes at out/eboot.elf VA + 0x4000) was NOT run in round 3 either,
//      so the mapping is unverified.
//      Shipped (N2-97 integration, N2-109): BuildOpList records OpItem::dw_end, and every Parked
//      return at top level calls release_consumed(), which advances asc.read_addr past the items
//      already run; Done advances the rest. REA (N2-109 D3): the game spin 0x863af0 -> 0x85d380 ->
//      0x85d3d0 reads a 32-bit consumer index through [X+0x10] in the same dword unit, so moving
//      the read pointer per consumed item is what lets it out. REA (N2-113): 0x85d300 maps the
//      queue with sceGnmMapComputeQueue (stub 0xf90900) writing the handle to S+0x08 and storing
//      that call's read_ptr_addr at S+0x10; 0x85d380 rings sceGnmDingDong (stub 0xf907a0) with
//      dword [X+0x08], so [X+0x10] is the read_addr of the very queue it submits to. A parked slice still holds
//      its unconsumed tail (inherent: the PM4 parser holds it the same way).
//   4. CE Op routing (deliberate, closes the SPEC ambiguity in the W3 CE clause). The spec says CE
//      ops honour bit 30 like DE ops. They do not: ce_run runs every CE Op inline on the command
//      processor thread via Native::StreamOps::Run, and never sends a bit-30-clear CE op to the draw
//      thread. Reason: the legacy path does the same (ProcessCeUpdate runs SotcNativeOp inline,
//      liverpool.cpp ~1392). Routing a CE op to the draw thread would make it asynchronous, so a CE
//      wait placed after it could be polled before the op wrote its label, which the CE stream does
//      not allow. Run strips the flag bits, so the bit 30 value has no effect on this path.

#include "native/native_labels.h"
#include "video_core/amdgpu/liverpool.h"

#include <cstdlib>

#include "common/assert.h"
#include "native/native_stats.h"
#include "native/native_stream_ops.h"
#include "video_core/sotc_stats.h"

namespace AmdGpu {

namespace {

// Same value as Native::StreamOps::kShadowTag (the tag of a native op's prefetch shadow).
constexpr u32 kShadowTag = 0x68750f10u;
// Consecutive draw-thread ops handed to NativeBackRunIds in one call.
constexpr u32 kDrawRunMax = 64;

bool ComputePrefetchOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_COMPUTE_PREFETCH");
        return !v || v[0] != '0';
    }();
    return on;
}

} // namespace

Liverpool::NativeRunResult Liverpool::RunNativeEntry(NativeQueueEntry& entry) {
    const bool is_gfx = !entry.is_compute;
    bool ran = false;

    // The queue this entry runs on: mapped_queues[entry.vqid] (graphics: 0, compute: gnm_vqid,
    // as ProcessCompute reads it), its ASC slot is asc_queues[{vqid - 1}] (as SubmitAsc / ProcessCompute).
    auto& queue_state = mapped_queues[entry.vqid];

    if (entry.is_compute) {
        native_asc_pipe_id = asc_queues[{entry.vqid - 1}].pipe_id;
    }

    // Walks the prefetch shadows of a list (a native op's copies of the compute packets it replaced)
    // and hands the dispatches they name to the translation workers. Same contract as
    // PrefetchComputeShaders on the PM4 stream: a wrong guess costs one translation at worst.
    auto prefetch_shadows = [this](const Native::OpList& list, ComputeProgram& cs) {
        for (const Native::OpItem& item : list.items) {
            if (item.kind != Native::OpKind::Op || item.body == nullptr ||
                item.body_dwords < 1 || item.body[0] != kShadowTag) {
                continue;
            }
            PrefetchComputeShaders({item.body + 1, item.body_dwords - 1}, cs, 0);
        }
    };

    if (!entry.started) {
        entry.started = true;
        if (rasterizer && ComputePrefetchOn()) {
            ComputeProgram cs = queue_state.cs_state;
            prefetch_shadows(entry.list, cs);
        }
        if (is_gfx) {
            cblock.Reset(); // as ProcessGraphics does: prefetch first, reset after
        }
    }

    // The list being run: the innermost IB frame, else the top-level list.
    auto cur_list = [&entry]() -> const Native::OpList& {
        return entry.ib_stack.empty() ? entry.list : entry.ib_stack.back().list;
    };
    auto cur_cursor = [&entry]() -> u32& {
        return entry.ib_stack.empty() ? entry.cursor : entry.ib_stack.back().cursor;
    };

    // Stats for a wait that passed (WaitFinished adds WaitsStarted, WaitPolls, WaitBlockedUs).
    auto finish_wait = [&entry]() {
        const u64 blocked_us =
            entry.polls > 1 ? (SotcStats::NowNs() - entry.park_start_ns) / 1000 : 0;
        Native::Stats::WaitFinished(entry.polls, blocked_us,
                                    entry.label_parked ? Native::Stats::WaitSat::Virtual
                                    : entry.polls <= 1 ? Native::Stats::WaitSat::First
                                                       : Native::Stats::WaitSat::Yield);
        entry.polls = 0;
        entry.park_start_ns = 0;
        entry.label_parked = false;
    };
    // A wait that did not pass: remember when it started on its first visit.
    auto note_park = [&entry]() {
        if (entry.polls == 1) {
            entry.park_start_ns = SotcStats::NowNs();
        }
    };
    // sotc: N2-94 integration. A parked compute slice gives back the ring space of everything it
    // consumed so far, as the PM4 parser does packet by packet (AdvanceReadPointer after each
    // packet): otherwise a game-side "compute queue is full" wait (0x863af0) could wait on a
    // slice that waits on the game. Top level only (nested IBs are not in the ring).
    auto release_consumed = [&] {
        if (!entry.is_compute || !entry.ib_stack.empty() || entry.cursor == 0) {
            return;
        }
        const u32 upto = entry.list.items[entry.cursor - 1].dw_end;
        if (upto > entry.read_advanced) {
            auto& asc = asc_queues[{entry.vqid - 1}];
            AdvanceReadPointer(asc.read_addr, upto - entry.read_advanced, asc.ring_size_dw);
            entry.read_advanced = upto;
        }
    };

    // Runs CE items from entry.ce_cursor. has_id: stop as soon as Poll(id) holds (a DE WaitCe).
    // Returns 0 when Poll(id) holds, 1 when the CE side is parked on one of its waits, 2 when
    // the CE list is exhausted. Sets `ran` for every item it advances.
    auto ce_run = [&](bool has_id, u32 id) -> int {
        while (true) {
            if (has_id && Native::StreamOps::Poll(id)) {
                return 0;
            }
            if (entry.ce_cursor >= entry.ce_list.items.size()) {
                return 2;
            }
            const Native::OpItem& ce = entry.ce_list.items[entry.ce_cursor];
            switch (ce.kind) {
            case Native::OpKind::Op:
                // ProcessCeUpdate runs CE ops inline; Run strips the flag bits itself.
                Native::StreamOps::Run(ce.id);
                break;
            case Native::OpKind::Wait:
                if (!Native::StreamOps::Poll(ce.id)) {
                    return 1; // the CE cursor stays on this wait
                }
                break;
            case Native::OpKind::WaitCe:
                // Not produced in a CCB; a WaitCe on the CE side has no meaning, so it passes.
                break;
            case Native::OpKind::Ib:
                // Not produced in a CCB (IndirectBufferConst is PM4). Counted and skipped, see note 2.
                Native::Stats::Add(Native::Stats::OpListsFallback);
                {
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        LOG_ERROR(Lib_GnmDriver,
                                  "SOTC native CP: native IB in a constant command buffer skipped");
                    }
                }
                break;
            }
            ++entry.ce_cursor;
            ran = true;
        }
    };

    // The DE list (or the innermost IB frame).
    while (true) {
        const Native::OpList& list = cur_list();
        u32& cursor = cur_cursor();
        if (cursor >= list.items.size()) {
            if (entry.ib_stack.empty()) {
                break; // top-level list done
            }
            entry.ib_stack.pop_back(); // references above are stale now: loop refetches them
            continue;
        }

        const Native::OpItem& item = list.items[cursor];
        switch (item.kind) {
        case Native::OpKind::Op: {
            if (item.id & Native::StreamOps::kRunOnCpBit) {
                Native::StreamOps::Run(item.id); // state/submit op: inline, on this thread
                ++cursor;
                ran = true;
                break;
            }
            // Draw-thread ops: the consecutive run is queued as one call, in stream order.
            u32 ids[kDrawRunMax];
            u32 count = 0;
            bool writes_memory = false;
            while (count < kDrawRunMax && cursor < list.items.size()) {
                const Native::OpItem& next = list.items[cursor];
                if (next.kind != Native::OpKind::Op ||
                    (next.id & Native::StreamOps::kRunOnCpBit) != 0) {
                    break;
                }
                ids[count++] = next.id;
                writes_memory = writes_memory || (next.id & Native::StreamOps::kWritesMemoryBit) != 0;
                ++cursor;
            }
            NativeBackRunIds(ids, count, writes_memory);
            ran = true;
            break;
        }
        case Native::OpKind::Wait: {
            // Draw-thread ops queued before this wait were already queued in stream order above.
            // sotc: N2-112: a wait parked on a pending label write is not re-polled until something
            // moved the progress epoch (the producer's Retire notifies it).
            if (entry.label_parked &&
                progress_epoch.load(std::memory_order_seq_cst) == entry.park_epoch) {
                if (ran) {
                    cp_progress = true;
                }
                return NativeRunResult::Parked;
            }
            const u64 epoch_before = progress_epoch.load(std::memory_order_seq_cst);
            (void)Native::Labels::TakeParkHint(); // drop a hint left by a PM4-path wait
            ++entry.polls;
            if (Native::StreamOps::Poll(item.id)) {
                finish_wait();
                ++cursor;
                ran = true;
                break;
            }
            note_park();
            if (Native::Labels::TakeParkHint()) {
                // The epoch was read before the poll: a Retire after it moves it, so no wake is lost.
                entry.label_parked = true;
                entry.park_epoch = epoch_before;
            }
            release_consumed();
            if (ran) {
                cp_progress = true;
            }
            return NativeRunResult::Parked;
        }
        case Native::OpKind::WaitCe: {
            ++entry.polls;
            const u32 id = item.id;
            const int state = ce_run(true, id);
            if (state == 0) {
                // Poll(id) holds: the wait passed.
                finish_wait();
                ++cursor;
                ran = true;
                break;
            }
            // state 1 (CE parked) or 2 (CE list exhausted, predicate still false): Parked, re-polled
            // on the next progress. Draw-thread ops queued by NativeBackRunIds may write the label
            // later, so the wait must not be passed here (note 1).
            note_park();
            release_consumed();
            if (ran) {
                cp_progress = true;
            }
            return NativeRunResult::Parked;
        }
        case Native::OpKind::Ib: {
            const u64 ib_addr = item.ib_addr;
            const u32 ib_size = item.ib_size;
            ++cursor; // the parent resumes after the IB
            ran = true;
            if (ib_size == 0) {
                break;
            }
            Native::OpList child;
            if (!Native::BuildOpList({reinterpret_cast<const u32*>(ib_addr), ib_size}, child)) {
                // Not expected: native IBs carry native buffers. The PM4 path cannot run it here.
                Native::Stats::Add(Native::Stats::OpListsFallback);
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    LOG_ERROR(Lib_GnmDriver, "SOTC native CP: native IB could not be built, skipped");
                }
                break;
            }
            entry.ib_stack.push_back(NativeQueueEntry::IbFrame{std::move(child), 0});
            if (rasterizer && ComputePrefetchOn()) {
                ComputeProgram cs = queue_state.cs_state;
                prefetch_shadows(entry.ib_stack.back().list, cs);
            }
            break;
        }
        }
    }

    // The top-level DE list is done: the CE list still runs to its end (ProcessGraphics drains the
    // CE task the same way). A CE wait that does not hold parks the entry.
    const int ce_state = ce_run(false, 0);
    if (ce_state == 1) {
        // sotc: N2-109: the DE list is consumed; give its ring space back before parking.
        release_consumed();
        if (ran) {
            cp_progress = true;
        }
        return NativeRunResult::Parked;
    }

    // Everything ran. Compute: the ring read pointer moves past the slice once (note 3).
    if (entry.is_compute) {
        auto& asc = asc_queues[{entry.vqid - 1}];
        if (entry.list.dwords > entry.read_advanced) {
            AdvanceReadPointer(asc.read_addr, entry.list.dwords - entry.read_advanced,
                               asc.ring_size_dw);
            entry.read_advanced = entry.list.dwords;
        }
    }
    if (ran) {
        cp_progress = true;
    }
    return NativeRunResult::Done;
}

} // namespace AmdGpu
