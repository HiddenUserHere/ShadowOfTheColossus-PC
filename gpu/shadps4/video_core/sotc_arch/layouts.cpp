// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A1 unified image layouts (SOTC_LAYOUT_UNIFIED, ARCH-DRAW.md part 3). An image's real layout is GENERAL from
// its first use on, when the mode is Active(): VK_KHR_unified_image_layouts with unifiedImageLayouts present, and the
// switch is on (=1, or the default auto). The texture cache still tracks the layout each use asked for
// (Image::State::layout): the barrier decisions and the render state memo compare those values as before. Pick() gives
// the layout a command, attachment, descriptor or barrier must name for an image. Every raw non-GENERAL layout of our
// own images goes through Pick too, so one image is never named two ways (a barrier's old layout must be the real one).
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "video_core/sotc_arch/sotc_arch.h"
#include "video_core/texture_cache/image.h"

namespace SotcArch::Layouts {

namespace {
// sotc: set once by the instance (NoteSupported); false until then.
std::atomic<bool> g_supported{false};
// sotc: why the instance set g_supported (a string literal), printed by LogDecision.
std::atomic<const char*> g_reason{nullptr};
std::atomic<bool> g_logged{false};
// sotc: counters, cumulative; Report prints the change since its last call.
std::atomic<u64> g_kept{};
std::atomic<u64> g_kept_write{};   // sotc: kept barriers with a write on either side (ordering, never dropped)
std::atomic<u64> g_skipped{};      // layout-only: the same access on both sides, no write, no change of access
std::atomic<u64> g_skipped_read{}; // read to read: another read access, no write on either side
std::atomic<u64> g_verify_skips{};
std::array<std::atomic<u64>, static_cast<size_t>(Residual::Count)> g_residual{};

// sotc: the request. 0 forced off, 1 on, 2 verify, 3 auto (unset, empty or any other value).
int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_LAYOUT_UNIFIED");
        if (!v || !v[0]) {
            return 3;
        }
        switch (v[0]) {
        case '0': return 0;
        case '1': return 1;
        case '2': return 2;
        default: return 3;
        }
    }();
    return mode;
}
} // namespace

bool Requested() {
    return Mode() != 0;
}

bool On() {
    return Mode() == 1 || Mode() == 3;
}

bool Supported() {
    return g_supported.load(std::memory_order_relaxed);
}

void NoteSupported(bool supported, const char* reason) {
    g_supported.store(supported, std::memory_order_relaxed);
    g_reason.store(reason, std::memory_order_relaxed);
}

bool Active() {
    return On() && Supported();
}

bool Verify() {
    // sotc: verify counts what the unified path would drop; without the feature that is meaningless.
    return Mode() == 2 && Supported();
}

void LogDecision() {
    if (g_logged.exchange(true)) {
        return;
    }
    const char* why = g_reason.load(std::memory_order_relaxed);
    if (!why) {
        why = "no probe";
    }
    const int mode = Mode();
    if (mode == 0) {
        std::fprintf(stderr, "SOTC LAYOUT UNIFIED: off (SOTC_LAYOUT_UNIFIED=0), layouts unchanged\n");
    } else if (Active()) {
        std::fprintf(stderr, "SOTC LAYOUT UNIFIED: on (%s): every texture-cache image in GENERAL; %s\n",
                     mode == 1 ? "SOTC_LAYOUT_UNIFIED=1" : "auto", why);
    } else if (mode == 2) {
        std::fprintf(stderr, "SOTC LAYOUT UNIFIED: verify (SOTC_LAYOUT_UNIFIED=2): old layouts used, drops "
                             "counted only; %s\n", why);
    } else {
        std::fprintf(stderr, "SOTC LAYOUT UNIFIED: off (%s, feature missing or refused): layouts unchanged; %s\n",
                     mode == 1 ? "SOTC_LAYOUT_UNIFIED=1 requested" : "auto", why);
    }
}

vk::ImageLayout Pick(vk::ImageLayout layout) {
    if (!Active() || layout == vk::ImageLayout::eUndefined ||
        layout == vk::ImageLayout::ePresentSrcKHR || layout == vk::ImageLayout::eSharedPresentKHR) {
        return layout;
    }
    return vk::ImageLayout::eGeneral;
}

void NoteKept() {
    g_kept.fetch_add(1, std::memory_order_relaxed);
}

void NoteKeptWrite() {
    g_kept_write.fetch_add(1, std::memory_order_relaxed);
}

void NoteSkipped() {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteSkippedRead() {
    g_skipped_read.fetch_add(1, std::memory_order_relaxed);
}

void NoteVerifySkip() {
    g_verify_skips.fetch_add(1, std::memory_order_relaxed);
}

void NoteResidual(Residual reason, u64 count) {
    g_residual[static_cast<size_t>(reason)].fetch_add(count, std::memory_order_relaxed);
}

void Report(char* out, u32 size, double seconds) {
    out[0] = '\0';
    if (!On() && !Verify()) {
        return;
    }
    static u64 last_kept = 0, last_kept_write = 0, last_skipped = 0, last_skipped_read = 0, last_verify = 0;
    static std::array<u64, static_cast<size_t>(Residual::Count)> last_residual{};
    const u64 kept = g_kept.load(std::memory_order_relaxed);
    const u64 kept_write = g_kept_write.load(std::memory_order_relaxed);
    const u64 skipped = g_skipped.load(std::memory_order_relaxed);
    const u64 skipped_read = g_skipped_read.load(std::memory_order_relaxed);
    const u64 verify_skips = g_verify_skips.load(std::memory_order_relaxed);
    const u64 d_kept = kept - last_kept;
    const u64 d_kept_write = kept_write - last_kept_write;
    const u64 d_skipped = skipped - last_skipped;
    const u64 d_skipped_read = skipped_read - last_skipped_read;
    const u64 d_verify = verify_skips - last_verify;
    last_kept = kept;
    last_kept_write = kept_write;
    last_skipped = skipped;
    last_skipped_read = skipped_read;
    last_verify = verify_skips;
    std::array<u64, static_cast<size_t>(Residual::Count)> d_residual{};
    for (size_t i = 0; i < d_residual.size(); ++i) {
        const u64 now = g_residual[i].load(std::memory_order_relaxed);
        d_residual[i] = now - last_residual[i];
        last_residual[i] = now;
    }
    const double per_s = seconds > 0.0 ? 1.0 / seconds : 0.0;
    if (On() && Supported()) {
        const u64 d_skip_all = d_skipped + d_skipped_read;
        std::snprintf(out, size,
                      "SOTC LAYOUT 5s: mode %s, feature present (GENERAL everywhere), barriers kept %llu "
                      "(%.0f/s, ordering: a write or a first use %llu), transitions skipped %llu (%.0f/s: layout-only %llu, "
                      "read-to-read %llu), residual non-GENERAL: present %llu, sdk %llu, internal %llu",
                      Mode() == 3 ? "auto on" : "on",
                      static_cast<unsigned long long>(d_kept), d_kept * per_s,
                      static_cast<unsigned long long>(d_kept_write),
                      static_cast<unsigned long long>(d_skip_all), d_skip_all * per_s,
                      static_cast<unsigned long long>(d_skipped),
                      static_cast<unsigned long long>(d_skipped_read),
                      static_cast<unsigned long long>(d_residual[0]),
                      static_cast<unsigned long long>(d_residual[1]),
                      static_cast<unsigned long long>(d_residual[2]));
    } else if (On()) {
        std::snprintf(out, size,
                      "SOTC LAYOUT 5s: mode %s, feature missing: layouts unchanged (no GENERAL attachments "
                      "without unifiedImageLayouts), transitions skipped 0",
                      Mode() == 3 ? "auto (off)" : "on");
    } else {
        std::snprintf(out, size,
                      "SOTC LAYOUT 5s: mode verify (old layouts used), feature present, would skip %llu "
                      "(%.0f/s); the skipped barriers carry no write before them by rule",
                      static_cast<unsigned long long>(d_verify), d_verify * per_s);
    }
}

} // namespace SotcArch::Layouts
