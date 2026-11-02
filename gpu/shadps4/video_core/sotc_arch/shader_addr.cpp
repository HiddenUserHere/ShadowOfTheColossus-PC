// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: A3 (SOTC_SHADER_ADDR, docs/re-stutters/native/ARCH-DRAW.md sections 6 and 8). The CPU replica of the address
// the generated code computes for a guest buffer, and the counters of the bind skip and of the verify run.
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/sotc_guest_memory.h"
#include "video_core/sotc_arch/shader_addr.h"
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::ShaderAddr {

namespace {

// The profile of the pipeline being bound (set by BindResources on the draw thread, read by BindBuffers).
thread_local const Shader::Profile* t_bind_profile = nullptr;

std::atomic<u64> g_skipped{0};
std::atomic<u64> g_skipped_written{0};
std::atomic<u64> g_skipped_bytes{0};
std::atomic<u64> g_match{0};
std::atomic<u64> g_mismatch{0};
std::atomic<u64> g_other_buffer{0};
std::atomic<u64> g_no_address{0};
std::atomic<u64> g_unanswered{0};

// The first unanswered binds of the report window, by guest address (mutex: only the report reads them).
constexpr size_t kListed = 8;
std::mutex g_list_mutex;
std::array<std::pair<VAddr, u64>, kListed> g_listed{};
size_t g_listed_count = 0;

void NoteUnanswered(VAddr base, u64 size) {
    std::scoped_lock lock{g_list_mutex};
    if (g_listed_count < kListed) {
        g_listed[g_listed_count++] = {base, size};
    }
}

} // namespace

bool On() {
    static const bool on = SwitchMode("SOTC_SHADER_ADDR") != 0;
    return on;
}

bool Verify() {
    static const bool verify = SwitchMode("SOTC_SHADER_ADDR") == 2;
    return verify;
}

void SetBindProfile(const Shader::Profile* profile) {
    t_bind_profile = profile;
}

const Shader::Profile* BindProfile() {
    return t_bind_profile;
}

vk::DeviceAddress Expected(VAddr base, u64 size) {
    // The window rule of the shader (ARCH-DRAW section 6, specialist 6's SPIR-V): the arena window when the whole
    // range lies inside it, else the mirror window when it lies inside that, else 0. Unsigned, no wrap.
    const u64 end = base + size;
    if (size == 0 || end < base) {
        return 0;
    }
    // Arena window: the ring-8 imported run holding base (SotcGuestMemory::LookupGuestView). Its buffer byte 0 is
    // cpu_base at device_address, so the window is [cpu_base, cpu_base + buffer_size) at device_address.
    Vulkan::SotcGuestMemory::GuestView view{};
    if (Vulkan::SotcGuestMemory::LookupGuestView(base, 1, &view) && view.device_address != 0 &&
        view.buffer_size != 0) {
        const u64 arena_guest = view.cpu_base;
        const u64 arena_end = arena_guest + view.buffer_size;
        if (base >= arena_guest && end <= arena_end) {
            return view.device_address + (base - arena_guest);
        }
    }
    // Mirror window 0 (the packet carries one mirror window).
    const auto windows = Mirror::Windows();
    if (!windows.empty()) {
        const Mirror::Window& mirror = windows.front();
        if (base >= mirror.guest_base && end <= mirror.guest_base + mirror.size) {
            return mirror.device_base + (base - mirror.guest_base);
        }
    }
    return 0;
}

void NoteSkip(u64 size, bool written) {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    g_skipped_bytes.fetch_add(size, std::memory_order_relaxed);
    if (written) {
        g_skipped_written.fetch_add(1, std::memory_order_relaxed);
    }
}

void VerifyBind(VAddr base, u64 size, const VideoCore::Buffer* buffer, u64 offset) {
    if (!buffer || !buffer->HasBufferDeviceAddress()) {
        g_no_address.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const vk::DeviceAddress begin = buffer->BufferDeviceAddress();
    const vk::DeviceAddress obtained = begin + offset;
    const vk::DeviceAddress expected = Expected(base, size);
    if (expected == 0) {
        g_unanswered.fetch_add(1, std::memory_order_relaxed);
        NoteUnanswered(base, size);
        return;
    }
    if (expected == obtained) {
        g_match.fetch_add(1, std::memory_order_relaxed);
    } else if (expected >= begin && expected < begin + buffer->SizeBytes()) {
        // The same buffer at another offset: the address the shader computes points at other bytes.
        g_mismatch.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Another device memory (the mirror or an arena run, against a classic buffer): the same guest bytes
        // may live there, so equality of addresses says nothing. Byte comparison is the mirror's own verify.
        g_other_buffer.fetch_add(1, std::memory_order_relaxed);
    }
}

void Report(char* out, u32 size, double seconds) {
    if (!out || size == 0) {
        return;
    }
    const u64 skipped = g_skipped.exchange(0, std::memory_order_relaxed);
    const u64 written = g_skipped_written.exchange(0, std::memory_order_relaxed);
    const u64 bytes = g_skipped_bytes.exchange(0, std::memory_order_relaxed);
    const u64 match = g_match.exchange(0, std::memory_order_relaxed);
    const u64 mismatch = g_mismatch.exchange(0, std::memory_order_relaxed);
    const u64 other = g_other_buffer.exchange(0, std::memory_order_relaxed);
    const u64 no_address = g_no_address.exchange(0, std::memory_order_relaxed);
    const u64 unanswered = g_unanswered.exchange(0, std::memory_order_relaxed);
    std::array<std::pair<VAddr, u64>, kListed> listed{};
    size_t listed_count = 0;
    {
        std::scoped_lock lock{g_list_mutex};
        listed_count = g_listed_count;
        listed = g_listed;
        g_listed_count = 0;
    }
    int n = std::snprintf(out, size,
                          "SOTC SHADER_ADDR: %llu binds skipped (%llu written, %.1f MiB) in %.1f s; "
                          "verify: %llu match, %llu mismatch, %llu other buffer, %llu no address, "
                          "%llu unanswered",
                          static_cast<unsigned long long>(skipped),
                          static_cast<unsigned long long>(written),
                          static_cast<double>(bytes) / (1024.0 * 1024.0), seconds,
                          static_cast<unsigned long long>(match),
                          static_cast<unsigned long long>(mismatch),
                          static_cast<unsigned long long>(other),
                          static_cast<unsigned long long>(no_address),
                          static_cast<unsigned long long>(unanswered));
    if (n < 0) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < listed_count && static_cast<u32>(n) < size; ++i) {
        const int m = std::snprintf(out + n, size - n, "%s%#llx+%#llx", i == 0 ? " [unanswered: " : " ",
                                    static_cast<unsigned long long>(listed[i].first),
                                    static_cast<unsigned long long>(listed[i].second));
        if (m < 0) {
            break;
        }
        n += m;
    }
    if (listed_count != 0 && static_cast<u32>(n) < size) {
        std::snprintf(out + n, size - n, "]");
    }
}

} // namespace SotcArch::ShaderAddr
