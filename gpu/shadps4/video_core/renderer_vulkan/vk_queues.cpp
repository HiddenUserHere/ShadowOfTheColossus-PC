// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: N5 queue plan, see vk_queues.h.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "video_core/renderer_vulkan/vk_queues.h"
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE mode (AsyncMode)

namespace SotcQueues {

namespace {

constexpr uint32_t VendorNvidia = 0x10DE;
constexpr uint32_t VendorAmd = 0x1002;
constexpr uint32_t VendorIntel = 0x8086;

std::atomic<uint64_t> routed_count{0};
std::atomic<uint64_t> routed_bytes{0};
std::atomic<uint64_t> kept_count{0};
std::atomic<uint64_t> kept_bytes{0};
std::atomic<uint32_t> fallback_logged{0}; // bit per Fallback value: logged once

} // namespace

int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("SOTC_N5_QUEUES");
        if (!env || env[0] == '\0' || env[0] == '0') {
            return 0;
        }
        return env[0] == '2' ? 2 : 1;
    }();
    return mode;
}

int AsyncMode() {
    static const int mode = [] {
        if (!SotcNative::On()) {
            return 0;
        }
        const char* env = std::getenv("SOTC_N5_ASYNC_COMPUTE");
        if (!env || env[0] == '\0' || env[0] == '0') {
            return 0;
        }
        return env[0] == '2' ? 2 : 1;
    }();
    return mode;
}

AsyncPick PickAsyncQueue(std::span<const FamilyInfo> families, uint32_t xfer_family) {
    for (uint32_t i = 0; i < families.size(); i++) {
        const FamilyInfo& f = families[i];
        if (i != xfer_family && f.compute && !f.graphics && f.queue_count >= 1) {
            return AsyncPick{.family = i, .queue_index = 0, .shares_xfer = false};
        }
    }
    // No other compute-only family: the upload family's second queue, when it has one. Many
    // drivers expose one C+T family, which the upload path would otherwise also use.
    if (xfer_family < families.size()) {
        const FamilyInfo& f = families[xfer_family];
        if (f.compute && !f.graphics && f.queue_count >= 2) {
            return AsyncPick{.family = xfer_family, .queue_index = 1, .shares_xfer = true};
        }
    }
    return AsyncPick{};
}

void LogAsyncPick(const AsyncPick& pick, uint32_t family_count) {
    static std::atomic<bool> logged{false};
    if (AsyncMode() == 0 || logged.exchange(true)) {
        return;
    }
    if (pick.family == NoFamily) {
        std::fprintf(stderr, "SOTC N5 ASYNC: no compute-only queue family (of %u); compute stays on "
                             "the graphics queue\n", family_count);
        NoteFallback(Fallback::NoAsyncFamily);
        return;
    }
    std::fprintf(stderr, "SOTC N5 ASYNC: mode %d, async compute queue family %u index %u%s "
                         "(independent dispatches are recorded there, see impl-n5-asynccomp.md)\n",
                 AsyncMode(), pick.family, pick.queue_index,
                 pick.shares_xfer ? " (shared with the upload family)" : "");
}

Probe ProbeFamilies(std::span<const FamilyInfo> families) {
    Probe probe;
    probe.family_count = static_cast<uint32_t>(families.size());
    for (uint32_t i = 0; i < probe.family_count; i++) {
        const FamilyInfo& f = families[i];
        if (f.queue_count == 0) {
            continue;
        }
        if (f.graphics && probe.graphics_family == NoFamily) {
            probe.graphics_family = i;
        }
        if (f.transfer && !f.graphics) {
            // Without compute preferred: a family with compute is often the DMA engine shared
            // with async compute work, and a copy-only family is the cleanest transfer path.
            if (probe.transfer_only_family == NoFamily ||
                (families[probe.transfer_only_family].compute && !f.compute)) {
                probe.transfer_only_family = i;
            }
        }
        if (f.compute && !f.graphics && probe.async_compute_family == NoFamily) {
            probe.async_compute_family = i;
        }
    }
    return probe;
}

const char* VendorName(uint32_t vendor_id) {
    switch (vendor_id) {
    case VendorNvidia:
        return "NVIDIA";
    case VendorAmd:
        return "AMD";
    case VendorIntel:
        return "Intel";
    default:
        return "other";
    }
}

void LogProbe(uint32_t vendor_id, std::span<const FamilyInfo> families, const Probe& probe) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) {
        return;
    }
    std::fprintf(stderr, "SOTC N5 QUEUES: probe vendor %s (0x%04x), %u families:",
                 VendorName(vendor_id), vendor_id, probe.family_count);
    for (std::size_t i = 0; i < families.size(); i++) {
        const FamilyInfo& f = families[i];
        std::fprintf(stderr, " [%zu: x%u%s%s%s]", i, f.queue_count, f.graphics ? " G" : "",
                     f.compute ? " C" : "", f.transfer ? " T" : "");
    }
    std::fprintf(stderr, "; graphics %d, transfer-only %d, async compute %d\n",
                 probe.graphics_family == NoFamily ? -1 : static_cast<int>(probe.graphics_family),
                 probe.transfer_only_family == NoFamily
                     ? -1
                     : static_cast<int>(probe.transfer_only_family),
                 probe.async_compute_family == NoFamily
                     ? -1
                     : static_cast<int>(probe.async_compute_family));
}

void NoteFallback(Fallback reason) {
    if (Mode() == 0 && AsyncMode() == 0) {
        return;
    }
    const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(reason);
    if (fallback_logged.fetch_or(bit) & bit) {
        return;
    }
    switch (reason) {
    case Fallback::NoTransferFamily:
        std::fprintf(stderr, "SOTC N5 QUEUES: no transfer-only queue family; uploads stay on the "
                             "graphics queue\n");
        break;
    case Fallback::TransferQueueRefused:
        std::fprintf(stderr, "SOTC N5 QUEUES: device refused the transfer queue; uploads stay on "
                             "the graphics queue\n");
        break;
    case Fallback::NoAsyncFamily:
        // Logged by LogAsyncPick with the family count; nothing more to add here.
        break;
    case Fallback::AsyncQueueRefused:
        std::fprintf(stderr, "SOTC N5 ASYNC: device refused the async compute queue; compute "
                             "stays on the graphics queue\n");
        break;
    }
}

void NoteUploadRouted(uint64_t bytes) {
    routed_count.fetch_add(1, std::memory_order_relaxed);
    routed_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteUploadKept(uint64_t bytes) {
    kept_count.fetch_add(1, std::memory_order_relaxed);
    kept_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    if (Mode() == 0) {
        return;
    }
    const uint64_t rc = routed_count.exchange(0, std::memory_order_relaxed);
    const uint64_t rb = routed_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t kc = kept_count.exchange(0, std::memory_order_relaxed);
    const uint64_t kb = kept_bytes.exchange(0, std::memory_order_relaxed);
    const double mb = 1.0 / (1024.0 * 1024.0);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "SOTC N5 QUEUES mode %d: xfer uploads %llu (%.1f MB, %.1f MB/s) kept %llu "
                  "(%.1f MB)\n",
                  Mode(), static_cast<unsigned long long>(rc), static_cast<double>(rb) * mb,
                  seconds > 0.0 ? static_cast<double>(rb) * mb / seconds : 0.0,
                  static_cast<unsigned long long>(kc), static_cast<double>(kb) * mb);
    out += line;
}

} // namespace SotcQueues
