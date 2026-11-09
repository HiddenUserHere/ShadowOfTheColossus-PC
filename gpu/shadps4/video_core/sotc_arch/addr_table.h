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
// (the entries on the device are the shadow's, and no guest byte of the page changed since the last sync of the page
// without that sync taking the change). A Touch of a range whose pages are all placed and clean returns at once, with
// no ObtainBuffer, when it is a read, or a written range in native mode that is already GPU-modified. Events that make
// a clean page stale clear its bit (Dirty, OnPlace, OnRemove, and in the tracked modes a written bind); see
// addr_table.cpp for the ordering proof and for why a native GPU write keeps the bits.
#pragma once

#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/sotc_arch/addr_table_hooks.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {
class BufferCache;
struct Buffer;
}

namespace SotcArch::AddrTable {

/// The GPU-visible window: SYSTEM_LOW_MIN, 64 GiB (the mirror's first window size).
inline constexpr VAddr WindowBase = 0x0700000000ULL;
inline constexpr u64 WindowSize = 64ULL << 30;
/// The guest page of the table (see the header comment).
inline constexpr u64 PageShift = 14;
inline constexpr u64 PageSize = 1ULL << PageShift;
inline constexpr u64 NumPages = WindowSize >> PageShift; ///< 4 Mi entries = 32 MiB of device memory

/// sotc: the table is requested: SOTC_ADDR_TABLE=1 and SOTC_SHADER_ADDR (read once), or SOTC_GCN_MEM not refused
/// (SotcGcn::Mode(Stage::Mem), read on every call). Init creates the table or refuses the stage; after a refusal
/// the table is not used, because Mem's mode is 0. Code that needs the table itself checks Translate / Touch.
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
/// A written range takes the fast path only in native mode (SOTC_NATIVE=1), with its pages placed and clean and
/// already GPU-modified (BufferCache::WrittenAlreadyGpuModified); then it runs NoteWrittenFast and no sync.
/// sotc: is_texel (a formatted buffer, DescriptorBinding is_formatted): ObtainBuffer syncs it from an aliasing image as
/// the descriptor path does, and no fast path skips that.
bool Touch(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr addr, u64 size, bool is_written,
           bool is_texel = false);

/// The buffer cache placed [addr, addr+size) at device address device_addr in owner (any thread). Only the shadow
/// changes; the next Touch uploads it.
void OnPlace(VAddr addr, u64 size, vk::DeviceAddress device_addr, const VideoCore::Buffer* owner);

/// The buffer cache released [addr, addr+size) (any thread). Entries become 0.
void OnRemove(VAddr addr, u64 size);

/// The device address of guest byte addr through the table (0 when not resident or outside the window). For the
/// verify paths (ShaderAddr::Expected).
vk::DeviceAddress Translate(VAddr addr);

// sotc: SOTC_GCN_MEM / SOTC_GCN_SYNC (gcn_native.h). The bit arrays are read and written with seq_cst, as the
// Touch fast path does; the scans take no lock.

/// sotc: SOTC_GCN_MEM. True when every table page of [addr, addr + size) is placed (non-zero entry). False for size 0
/// and for a range that is not wholly inside the window. Any thread, lock-free.
bool AllPlaced(VAddr addr, u64 size) noexcept;
/// sotc: true when a page of [addr, addr + size) carries a GPU-modified mark (NoteGpuWrite) that no slow Touch has
/// synchronized since. Buf::BindAddressed's placed-page fast path refuses such a range. Any thread, lock-free.
bool AnyGpuModified(VAddr addr, u64 size) noexcept;
/// sotc: SOTC_GCN_SYNC. Calls fn(user, addr, size) for every maximal run of table pages that are placed and not clean
/// (merged across adjacent pages). Draw thread, once per submit. Lock-free scan of the bit words (64 pages each,
/// all-zero words skipped); no lock is held while fn runs. A page that changes during the scan is either reported or
/// not; the caller's MarkClean decides whether the sync it made still stands.
void ForEachStalePlaced(void (*fn)(void* user, VAddr addr, u64 size), void* user);
/// sotc: SOTC_GCN_SYNC. The sequence token of a clean-state change, read BEFORE the caller synchronizes a run. Any
/// thread, lock-free.
u64 CleanSequence() noexcept;
/// sotc: SOTC_GCN_SYNC. After the caller synchronized every CPU change of [addr, addr + size) (its copy read the guest
/// bytes after `sequence` was taken), sets the clean bits of its pages. Sets nothing and returns false when a clean
/// state change happened since `sequence` (ClearClean bumped the sequence), when an entry of the range is not on the
/// device yet (a pending chunk upload), or when a page is not placed; the pages then stay stale for the next submit.
/// Draw thread.
bool MarkClean(VAddr addr, u64 size, u64 sequence) noexcept;

/// sotc: SOTC_ADDR_VERTEX (1 on, 2 on with verify). Needs On(). The vertex, index and indirect-argument reads of a draw
/// take their buffer from the table (ObtainRead) instead of ObtainBuffer. Pipelines do not change with it yet.
bool VertexOn();
/// sotc: the read paths of SOTC_ADDR_VERTEX, counted apart.
enum class VertexPath : u8 { Vertex, Index, Indirect, IndirectCount, DispatchIndirect, Count };
/// sotc: the buffer and the offset in it of [addr, addr + size), from the page entries and their owners. True only when
/// every page of the range is resident, the pages share one owner, and their device addresses are contiguous (one
/// backing). Draw thread or any thread (takes g_mutex).
bool Backing(VAddr addr, u64 size, const VideoCore::Buffer*& buffer, u64& offset);
/// sotc: SOTC_ADDR_VERTEX. The buffer and offset for a read-only range the draw reads by address: Touch (residency,
/// synchronization, no ObtainBuffer for a clean range), then Backing. Otherwise cache.ObtainBuffer, counted as a
/// fallback. Mode 2 runs ObtainBuffer as well, counts whether the table's answer matches it, and returns its result.
/// Off: exactly cache.ObtainBuffer(addr, size, false). Draw thread.
std::pair<const VideoCore::Buffer*, u64> ObtainRead(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler,
                                                    VAddr addr, u32 size, VertexPath path);

/// One report line (counters reset): entries live, table and shadow memory, updates per second, touches served
/// and failed, uploads.
void Report(char* out, u32 size, double seconds);
/// sotc: SOTC_ADDR_VERTEX, one report line (counters reset): per path, the table answers and the fallbacks, and mode 2's
/// comparison.
void ReportVertex(char* out, u32 size, double seconds);

} // namespace SotcArch::AddrTable
