// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_LABELS_HONEST (0 off by default, 1 on). The label side of the draw thread: an end-of-pipe,
// ReleaseMem or EventWriteEos label is written by the draw thread only after the GPU has run the work
// recorded before it, when the work since the queue's previous label touched memory that the CPU
// reads early (served host ranges, GPU-to-CPU results). liverpool.cpp has the deferral rule and the
// deadlock argument; this header is the shared API and the counters.
#pragma once

#include <cstdint>
#include <string>

namespace SotcLabels {

/// Label kinds, in the counters' order.
enum class Kind : uint8_t {
    Eop = 0,        ///< PM4 EVENT_WRITE_EOP (graphics queue)
    ReleaseMem = 1, ///< PM4 RELEASE_MEM (compute queue)
    Eos = 2,        ///< PM4 EVENT_WRITE_EOS, SIGNAL_FENCE (graphics queue)
};

/// SOTC_LABELS_HONEST=1 (read once). False when the variable is unset or not "1".
bool On();

/// On() and the deferred-label machinery is live (the draw thread runs, Liverpool set it up).
/// The frame pipelining (gnmdriver.cpp) gates on this.
bool Honest();

/// Liverpool constructor: the machinery is live for this process (draw thread present).
void MarkLive();

/// Draw thread: the GPU work just recorded reads or writes HOST_SHARED or GPU_TO_CPU memory.
void NoteSharedTouch();

/// Count of NoteSharedTouch calls, ever. A label compares it with the value at the previous label of
/// its queue: a change means shared memory was touched since then.
uint64_t SharedSeq();

/// One label seen by the deferral rule. deferred: the rule deferred it. honest_only: deferred because
/// of NoteSharedTouch alone (without the ring8 written-bind rule or an outstanding label), i.e. it
/// would have been written early without SOTC_LABELS_HONEST. This is the cost count.
void NoteLabel(Kind kind, bool deferred, bool honest_only);
/// A label that would be deferred but is not: the submit (or a learned CP wait) waits on it.
void NoteExcluded(Kind kind);
/// A deferred label written by the 500 ms watchdog.
void NoteForced(Kind kind);

/// Appends one line (with a newline) to out and resets the counters; seconds is the window length.
/// Writes nothing when the switch is off.
void ReportLabels(std::string& out, double seconds);

} // namespace SotcLabels
