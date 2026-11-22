// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Virtual labels (W2, SOTC_NATIVE_VLABELS).
//
// Every GPU queue funnels into the one draw-thread ring (Liverpool BackOrRun), and with default
// settings a label write (EOP/EOS/WRITE_DATA/RELEASE_MEM/flip tail) happens when the draw thread
// reaches it. A native wait whose awaited value is already QUEUED for that ring (parsed by the
// command processor, not yet executed by the draw thread) is therefore satisfied by stream order:
// everything the waiting queue does next is queued after the producer. The table below records
// such pending writes so the wait can pass without polling.
//
// Threads: Record / Satisfies / Exclude / SubmitBoundary run on the command processor thread;
// Retire runs on the draw thread (the real write). The implementation synchronizes them.
//
// Excluded (never recorded, so their waits keep polling the real value): labels written after
// real GPU completion (SOTC_EOP_DEFER, SOTC_VIS_DIRECT / late labels, native-pass
// Rt::Eop::WaitLabel) and labels the game CPU writes (Exclude).
#pragma once

#include "common/types.h"

namespace Native::Labels {

/// A write of `value` (`bytes` = 4 or 8) to guest address `addr` was queued to the draw ring by
/// the op being run. Returns a token for Retire (0 when not recorded: switch off or excluded).
u64 Record(u64 addr, u64 value, u32 bytes);

/// The draw thread performed the write of `token` (from Record). Safe with token 0. Wakes the
/// command processor (Native::Progress::Notify), which re-polls the waits parked on it.
void Retire(u64 token);

/// N2-112: a native wait whose memory test failed asks whether a pending write recorded for `addr`
/// would make `((value & mask) <func> ref)` hold (func: 0 always, 1 <, 2 <=, 3 ==, 4 !=, 5 >=,
/// 6 >). It does NOT pass the wait: the command processor reads guest memory at its own time
/// (pipeline lookup, the flattened user-data copy of every draw), and that memory is only valid
/// once the draw thread has run the producer (its OnFence readbacks). Passing early made objects
/// jump (N2-110). A true answer sets a park hint (TakeParkHint) so the command processor parks the
/// wait without re-polling until the producer's real write (Retire notifies the progress epoch).
bool Satisfies(u64 addr, u32 mask, u32 func, u32 ref);

/// Command processor thread: true once after a Satisfies() that returned true (then cleared).
bool TakeParkHint();

/// The command processor is about to run (resume) the submission `owner` (any stable non-zero
/// identity: the coroutine frame or the native entry). Record tags each pending write with it and
/// Satisfies only accepts a pending write of the SAME submission (gap N2-94 integration): the
/// label-slot arrays are zeroed by the game CPU at build time and reused (0x92540), so a pending
/// write of an earlier submission may belong to an older use of the slot. Within one submission
/// the producer is earlier in the same stream than the wait, which is the redundant case (a).
void SetSubmission(u64 owner);

/// `addr` is written by the game CPU or after real GPU completion: never treat it as pending.
void Exclude(u64 addr);

/// Called at sceGnmSubmitDone (frame boundary). Counts entries still pending there (the
/// invariant of N2-94: the SubmitDone GPU-idle barrier retires every frame's writes first).
void SubmitBoundary();

} // namespace Native::Labels
