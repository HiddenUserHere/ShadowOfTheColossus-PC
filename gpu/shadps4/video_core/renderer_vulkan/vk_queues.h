// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: N5 queue plan (docs/re-stutters/native/PLAN.md N5, "Async queues"). SOTC_N5_QUEUES:
//   0 (default) off: every upload and compute dispatch stays on the graphics queue, as before.
//   1 on: announced buffer uploads go to the transfer-only queue family when the device has one
//         (Instance::IsXferRoutingEnabled); the family probe is logged and counted.
//   2 verify: as 1, plus the probe and fallback counters are reported every 5 s line.
// Queue family ownership: buffers that can be copied on the transfer queue are created CONCURRENT
// over {graphics, transfer} (buffer.cpp), so no release/acquire barriers are needed for them.
// Image uploads are not routed (exclusive images would need ownership transfers): see the report.
// The async compute family is only probed here; no dispatch is moved to it yet.
// sotc: N5 async compute (docs/re-stutters/native/impl-n5-asynccomp.md). SOTC_N5_ASYNC_COMPUTE, with
// SOTC_NATIVE on: 0 off; 1 the device creates one compute queue (PickAsyncQueue) and logs the pick;
// 2 verify: 1 + the classifier in vk_frame_graph.cpp ("N5 async" line). Mode 1 moves NO dispatch yet.
//
// Thread safety: Mode() is read once. Counters are atomics; Report resets its window.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace SotcQueues {

/// SOTC_N5_QUEUES: 0 off, 1 on, 2 on + verify counters.
int Mode();
inline bool On() {
    return Mode() != 0;
}

/// Queue flags of one family, as the probe needs them (no Vulkan header here).
struct FamilyInfo {
    uint32_t queue_count{};
    bool graphics{};
    bool compute{};
    bool transfer{};
};

inline constexpr uint32_t NoFamily = ~uint32_t{0};

/// Result of the probe of the device's queue families (vk_instance.cpp, after the family scan).
struct Probe {
    uint32_t graphics_family{NoFamily};      ///< first family with graphics (the one the port uses)
    uint32_t transfer_only_family{NoFamily}; ///< transfer, no graphics; without compute preferred
    uint32_t async_compute_family{NoFamily}; ///< compute, no graphics, >= 1 queue (not used yet)
    uint32_t family_count{};
};

/// Fills the probe from the device's families. Pure: no logging, no state.
Probe ProbeFamilies(std::span<const FamilyInfo> families);

/// Vendor name for logs: "NVIDIA", "AMD", "Intel", or "vendor 0x...".
const char* VendorName(uint32_t vendor_id);

/// Logs the probe once (SOTC N5 QUEUES line with vendor and families).
void LogProbe(uint32_t vendor_id, std::span<const FamilyInfo> families, const Probe& probe);

/// Fallback reasons, each logged once when it first happens.
enum class Fallback : uint8_t {
    NoTransferFamily,     ///< N5 on but no transfer-only family: uploads stay on graphics
    TransferQueueRefused, ///< device creation refused the transfer queue: graphics only
    NoAsyncFamily,        ///< N5 async on but no compute-only family: compute stays on graphics
    AsyncQueueRefused,    ///< device creation refused the async compute queue: graphics only
};
void NoteFallback(Fallback reason);

/// SOTC_N5_ASYNC_COMPUTE: 0 off, 1 queue created (plumbing), 2 verify (classifier). Needs SOTC_NATIVE.
int AsyncMode();
inline bool AsyncOn() {
    return AsyncMode() != 0;
}

/// The queue the device creates for async compute. family == NoFamily: none (graphics queue).
struct AsyncPick {
    uint32_t family{NoFamily};
    uint32_t queue_index{}; ///< index inside the family (1 when shares_xfer)
    bool shares_xfer{};     ///< the family is the upload family: its queue 0 is the upload queue
};

/// Picks the async compute queue: the first family with COMPUTE, no GRAPHICS, >= 1 queue, other
/// than the upload family (xfer_family, NoFamily when none). Only when no other family exists, the
/// upload family's queue 1 is used, and only if that family has >= 2 queues. Pure.
AsyncPick PickAsyncQueue(std::span<const FamilyInfo> families, uint32_t xfer_family);

/// Logs the pick once (SOTC N5 ASYNC line). A NoFamily pick notes the NoAsyncFamily fallback.
void LogAsyncPick(const AsyncPick& pick, uint32_t family_count);

/// Counters of routed uploads (vk_runtime.cpp TryCopyBufferXfer, snippet in the report).
void NoteUploadRouted(uint64_t bytes);
void NoteUploadKept(uint64_t bytes); ///< a copy kept on graphics (overlap or no queue)

/// Appends "SOTC N5 QUEUES ..." to out (5 s report) and resets the window counters. Empty when off.
void Report(std::string& out, double seconds);

} // namespace SotcQueues
