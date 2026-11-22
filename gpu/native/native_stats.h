// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Non-verbose progress counters.
//
// The acceptance metric of the native cutover (plan "100% native in one phase"): how much GPU
// work still arrives as PM4 that the command processor parses, against the native ops that
// replace it. Verbose tracing costs half the frame rate (gap N2-67), so this module only bumps
// relaxed atomics and prints ONE line per second ("SOTC NATIVE STATS"), called next to the
// overlay's "SOTC SCREEN FPS" line. SOTC_NATIVE_STATS=0 turns the line off.
#pragma once

#include "common/types.h"

namespace Native::Stats {

enum Counter : u32 {
    OpsEmitted,    ///< StreamOps::Emit placed a NOP
    OpsEmitFailed, ///< StreamOps::Emit could not write the packet (op dropped)
    OpsRun,        ///< the command processor ran a queued op
    OpsMissing,    ///< a SotcNativeOp NOP whose id was not queued
    Pm4FieldReads, ///< native draw/dispatch read a register range not owned by the native file
    NativeDraws,   ///< draws issued from native state
    NativeDispatches,
    // N2-94 plan (W0): native waits. A wait "starts" at its first poll; the satisfied-by counters
    // add up to the finished waits.
    WaitsStarted,
    WaitPolls,        ///< every predicate evaluation (StreamOps::Poll)
    WaitBlockedUs,    ///< microseconds between a wait's first poll and the poll that passed
    WaitSatFirst,     ///< passed on its first poll
    WaitSatYield,     ///< passed after one or more yields / parks
    WaitSatVirtual,   ///< passed by a pending virtual label (native_labels.h, W2)
    WaitSatEvent,     ///< passed right after a progress-epoch wake-up (W1)
    LabelsRecorded,   ///< native_labels Record calls that created an entry
    LabelsRetired,
    LabelsAtBoundary, ///< entries still pending at sceGnmSubmitDone (invariant violations)
    OpListsBuilt,     ///< submissions run by the native command processor (W3)
    OpListsFallback,  ///< submissions that kept the PM4 parser (not all-native / split / switch)
    kCounterCount,
};

bool Enabled();
void Add(Counter counter, u64 n = 1);

/// W0: records one finished native wait (polls, blocked time, how it passed).
enum class WaitSat : u8 { First, Yield, Virtual, Event };
void WaitFinished(u32 polls, u64 blocked_us, WaitSat how);

/// A PM4 packet the command processor parsed (any opcode except NOP); `compute` for the compute
/// queues. The goal of the cutover is zero.
void Pm4Packet(u32 opcode, bool compute);

/// A named boundary site (a GNM emitter detour or a sceGnm function): returns a slot to pass to
/// Native/Fallback. Thread-safe; call once per site (static local).
u32 RegisterSite(const char* name);
void SiteNative(u32 slot);
void SiteFallback(u32 slot);

/// Prints the per-second line (deltas since the previous call). Called once per second.
void ReportSecond(float seconds_since_start);

} // namespace Native::Stats
