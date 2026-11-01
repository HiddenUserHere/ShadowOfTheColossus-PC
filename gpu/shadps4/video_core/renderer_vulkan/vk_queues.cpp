// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: N5 queue plan, see vk_queues.h.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "video_core/renderer_vulkan/vk_queues.h"

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
    if (Mode() == 0) {
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
