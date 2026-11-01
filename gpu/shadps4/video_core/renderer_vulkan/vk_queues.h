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
};
void NoteFallback(Fallback reason);

/// Counters of routed uploads (vk_runtime.cpp TryCopyBufferXfer, snippet in the report).
void NoteUploadRouted(uint64_t bytes);
void NoteUploadKept(uint64_t bytes); ///< a copy kept on graphics (overlap or no queue)

/// Appends "SOTC N5 QUEUES ..." to out (5 s report) and resets the window counters. Empty when off.
void Report(std::string& out, double seconds);

} // namespace SotcQueues
