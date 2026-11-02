// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ADDR_TABLE=1, the universal path of SOTC_SHADER_ADDR (no sparse aliasing, no mirror copy). A device
// buffer holds one u64 per guest page of the GPU-visible window: the device address of the byte where that page's
// data lives today (the buffer cache arena, or a ring-8 imported run), 0 when the page is not resident. Shaders read
// the entry of the page of every access (spirv_emit_context.cpp, TableAddresses) and add the offset in the page.
//
// Granularity (16 KiB, the guest page): a page is always inside one backing. Arenas are linear over their whole
// size (sparse buffers, arena_device + (addr - arena_base)), and their residency is per 64 KiB block, which is a
// multiple of the page. Ring-8 runs are cut at guest pages (sotc_guest_memory.cpp ImportRun; the guest page is
// kPage = 16 KiB, runtime_memory.c PAGE). A 64 KiB table entry would be wrong where a run boundary falls in a block,
// and a 4 KiB one buys nothing: the backing never splits below 16 KiB.
//
// Population: EnsureResident and the arena migration (OnPlace), ReleaseMemory and MaybeEvictCold (OnRemove), and
// Touch, which asks the buffer cache for the range (ObtainBuffer: it places, syncs and serves ring-8 binds) and
// writes the entries of the whole range from the single (buffer, offset) it returns.
//
// Clean state (sotc: SOTC_ADDR_TABLE fast path). Each table page has two bits: placed (its entry is not 0) and clean
// (the entries on the device are the shadow's, and the bytes behind them match guest memory as of the last sync of the
// page). A read Touch of a range whose pages are all placed and clean returns at once, with no ObtainBuffer. Events
// that make a clean page stale clear its bit (Dirty, OnPlace, OnRemove); see addr_table.cpp for the ordering proof.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/sotc_arch/addr_table_hooks.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {
class BufferCache;
}

namespace SotcArch::AddrTable {

/// The GPU-visible window: SYSTEM_LOW_MIN, 64 GiB (the mirror's first window size).
inline constexpr VAddr WindowBase = 0x0700000000ULL;
inline constexpr u64 WindowSize = 64ULL << 30;
/// The guest page of the table (see the header comment).
inline constexpr u64 PageShift = 14;
inline constexpr u64 PageSize = 1ULL << PageShift;
inline constexpr u64 NumPages = WindowSize >> PageShift; ///< 4 Mi entries = 32 MiB of device memory

/// SOTC_ADDR_TABLE=1 and SOTC_SHADER_ADDR on. Read once from the environment.
bool On();

/// Creates the table (rasterizer constructor, when On()). Refuses the run (exits, like ShaderAddr::Init) when the
/// device has no buffer device address.
void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);

/// Device address of entry 0 (the packet's mirror_device_base in table mode). 0 when off.
vk::DeviceAddress Base();

/// Draw thread, BindBuffers (shader-addressed buffers): makes [addr, addr+size) resident through the buffer cache
/// (ObtainBuffer with is_written), writes the table entries of its pages and uploads the dirty table chunks on the
/// upload command buffer, which runs before this submit's commands. False when the range is outside the window, has
/// no device address, or the buffer cache refuses it (counted; the shader then reads zero for those pages).
/// A read of a range whose pages are all placed and clean returns true at once (no ObtainBuffer); SOTC_SHADER_ADDR=2
/// still runs the slow path and only counts the fast path it would have taken.
bool Touch(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr addr, u64 size, bool is_written);

/// The buffer cache placed [addr, addr+size) at device address device_addr (any thread). Only the shadow changes; the
/// next Touch uploads it.
void OnPlace(VAddr addr, u64 size, vk::DeviceAddress device_addr);

/// The buffer cache released [addr, addr+size) (any thread). Entries become 0.
void OnRemove(VAddr addr, u64 size);

/// The device address of guest byte addr through the table (0 when not resident or outside the window). For the
/// verify paths (ShaderAddr::Expected).
vk::DeviceAddress Translate(VAddr addr);

/// One report line (counters reset): entries live, table and shadow memory, updates per second, touches served
/// and failed, uploads.
void Report(char* out, u32 size, double seconds);

} // namespace SotcArch::AddrTable
