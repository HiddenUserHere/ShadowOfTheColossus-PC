// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CMD_COUNTERS=1 (off by default; observe only, no behaviour change). Measurement counters for the
// command path and the readbacks, printed by the 5 s report (driver.cpp). See cmd_counters.cpp for the lines.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

namespace SotcCmdCounters {

/// SOTC_CMD_COUNTERS=1, read once at load (like SotcStats::kFpsLog). Every hook below is a no-op when it is off.
inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_CMD_COUNTERS");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}();

/// Wall clock in nanoseconds (steady clock).
uint64_t NowNs() noexcept;

/// Liverpool::frames_recorded, read at the frame-slot waits (native_sync.cpp). Set once by the Liverpool constructor.
void SetRecordedCounter(const std::atomic<uint64_t>* recorded) noexcept;

/// SubmitGfx: one submitted command buffer (the guest segment, read through the span). Hashes it raw and with the
/// guest addresses masked; compares it with the previous frame's buffer at the same submit index.
void NoteDcb(std::span<const uint32_t> dcb) noexcept;

/// SubmitDone: the guest's frame ends. Starts a new submit index and counts the frame.
void NoteFrame() noexcept;

/// One PM4 packet of the command processor's graphics loop (ProcessGraphics). Samples 1 packet in 64 (per thread),
/// keyed by the IT opcode (type 3) or by the packet type. Container packets (indirect buffers) are not timed: their
/// packets are.
class PacketTimer {
public:
    PacketTimer(uint32_t type, uint32_t opcode, bool container) noexcept : key_(type == 3 ? opcode : 256 + type) {
        if (kOn && !container) {
            Begin();
        }
    }
    ~PacketTimer() {
        if (active_) {
            Finish();
        }
    }
    PacketTimer(const PacketTimer&) = delete;
    PacketTimer& operator=(const PacketTimer&) = delete;

private:
    void Begin() noexcept;
    void Finish() noexcept;
    uint32_t key_;
    bool active_ = false;
    uint64_t t0_ = 0;
};

/// The CP's wait loop of one WaitRegMem packet on a label (ProcessGraphics). Counts the waits; a wait that spun
/// (Round() called at least once) adds its rounds and its wall time to the label's entry. The wall time includes
/// the other tasks the fiber ran while it yielded.
class SpinScope {
public:
    explicit SpinScope(uint64_t label) noexcept;
    ~SpinScope();
    SpinScope(const SpinScope&) = delete;
    SpinScope& operator=(const SpinScope&) = delete;
    void Round() noexcept { ++rounds_; }

private:
    uint64_t label_;
    uint64_t rounds_ = 0;
    uint64_t t0_ = 0;
};

/// A mask served as zero by SOTC_NATIVE_MASKS=prev (native_masks.cpp): key is the mask key (0 when unknown).
void NoteZeroMask(uint64_t key, bool unknown) noexcept;

/// Frame-slot waits (native_sync.cpp). RecordedAt returns 1 when the frame is recorded, 0 when not, -1 when there is
/// no recorded counter. NoteFrameWait gets the state read before the wait, the wait's wall time and whether the GPU
/// had finished the frame already (then nothing was waited for).
int RecordedAt(uint64_t frame) noexcept;
void NoteFrameWait(int recorded_state, uint64_t ns, bool completed_at_entry) noexcept;

/// The 5 s report: appends one line per section that had activity in the window, then resets the window.
void Report(std::string& out, double seconds);

} // namespace SotcCmdCounters
