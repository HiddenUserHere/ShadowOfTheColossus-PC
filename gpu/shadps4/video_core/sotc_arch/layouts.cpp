// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A1 unified image layouts (SOTC_LAYOUT_UNIFIED, ARCH-DRAW.md part 3). An image's real layout is GENERAL from
// its first use on. The texture cache still tracks the layout each use asked for (Image::State::layout): the
// barrier decisions and the render state memo compare those values as before. Pick() gives the layout a command
// or a descriptor must name for an image.
#include <atomic>
#include <cstdio>

#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/texture_cache/image.h"

namespace SotcArch::Layouts {

namespace {
// sotc: set once by the image code from the instance (NoteSupported); false until then.
std::atomic<bool> g_supported{false};
// sotc: barrier counters, cumulative; Report prints the change since its last call.
std::atomic<u64> g_kept{};
std::atomic<u64> g_skipped{};
std::atomic<u64> g_verify_skips{};
} // namespace

bool On() {
    static const bool on = SwitchMode("SOTC_LAYOUT_UNIFIED") == 1;
    return on;
}

bool Verify() {
    static const bool verify = SwitchMode("SOTC_LAYOUT_UNIFIED") == 2;
    return verify;
}

bool Supported() {
    return g_supported.load(std::memory_order_relaxed);
}

void NoteSupported(bool supported) {
    g_supported.store(supported, std::memory_order_relaxed);
}

vk::ImageLayout Pick(vk::ImageLayout layout) {
    if (!On() || layout == vk::ImageLayout::eUndefined) {
        return layout;
    }
    // sotc: a feedback loop keeps its own layout without the unified feature: GENERAL for a feedback loop
    // needs VK_KHR_unified_image_layouts.
    if (layout == vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT &&
        !g_supported.load(std::memory_order_relaxed)) {
        return layout;
    }
    return vk::ImageLayout::eGeneral;
}

void NoteKept() {
    g_kept.fetch_add(1, std::memory_order_relaxed);
}

void NoteSkipped() {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteVerifySkip() {
    g_verify_skips.fetch_add(1, std::memory_order_relaxed);
}

void Report(char* out, u32 size, double seconds) {
    out[0] = '\0';
    if (!On() && !Verify()) {
        return;
    }
    static u64 last_kept = 0, last_skipped = 0, last_verify = 0;
    const u64 kept = g_kept.load(std::memory_order_relaxed);
    const u64 skipped = g_skipped.load(std::memory_order_relaxed);
    const u64 verify_skips = g_verify_skips.load(std::memory_order_relaxed);
    const u64 d_kept = kept - last_kept;
    const u64 d_skipped = skipped - last_skipped;
    const u64 d_verify = verify_skips - last_verify;
    last_kept = kept;
    last_skipped = skipped;
    last_verify = verify_skips;
    const double per_s = seconds > 0.0 ? 1.0 / seconds : 0.0;
    if (On()) {
        std::snprintf(out, size,
                      "SOTC LAYOUT 5s: mode on, feature %s (GENERAL everywhere), barriers kept %llu "
                      "(%.0f/s), transitions skipped %llu (%.0f/s)",
                      Supported() ? "present" : "missing: GENERAL without the extension",
                      static_cast<unsigned long long>(d_kept), d_kept * per_s,
                      static_cast<unsigned long long>(d_skipped), d_skipped * per_s);
    } else {
        std::snprintf(out, size,
                      "SOTC LAYOUT 5s: mode verify (old layouts used), feature %s, would skip %llu "
                      "(%.0f/s); the skipped barriers carry no write before them by rule",
                      Supported() ? "present" : "missing",
                      static_cast<unsigned long long>(d_verify), d_verify * per_s);
    }
}

} // namespace SotcArch::Layouts
