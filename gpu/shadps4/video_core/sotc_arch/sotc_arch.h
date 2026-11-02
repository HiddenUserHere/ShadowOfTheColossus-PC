// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the interfaces of the draw architecture in docs/re-stutters/native/ARCH-DRAW.md. Every part is off unless
// its switch is set; =2 runs the old path as well and counts differences. This header is the contract the parts
// are written against: change it only together with every implementation.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/amdgpu/regs_texture.h" // AmdGpu::BorderColorBuffer

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {
struct ImageView;
class Sampler;
class TextureCache;
} // namespace VideoCore

namespace Shader {
struct Info;
}

namespace SotcArch {

/// A switch value: 0 off, 1 on, 2 verify. Read once from the environment.
int SwitchMode(const char* name);

// ---------------------------------------------------------------------------------------------------------------
// A0. Per-op cost (switch SOTC_OPCOST). Draw thread and command processor, one 5 s report line each.
namespace OpCost {
enum class Thread : u8 { Draw, CommandProcessor };
enum class Op : u8 { Draw, DrawIndirect, Dispatch, DispatchIndirect, Other, Count };
bool On();
/// Wall time of one op on `thread`, measured by the caller (ns).
void Note(Thread thread, Op op, u64 ns);
void Report(char* out, u32 size, double seconds);
} // namespace OpCost

// ---------------------------------------------------------------------------------------------------------------
// A1. Unified image layouts (switch SOTC_LAYOUT_UNIFIED). With VK_KHR_unified_image_layouts every image lives in
// GENERAL; without it GENERAL is still used (correct, maybe slower on some hardware).
namespace Layouts {
bool On();
/// True when the device reports unifiedImageLayouts (probed by the instance).
bool Supported();
} // namespace Layouts

// ---------------------------------------------------------------------------------------------------------------
// A2. GPU memory mirror (switch SOTC_MIRROR). One sparse VkBuffer per window; its offset o holds guest address
// window.guest_base + o. Pages (64 KiB) get device memory when the guest maps GPU-visible memory.
namespace Mirror {
struct Window {
    VAddr guest_base;
    u64 size;
    vk::Buffer buffer;
    vk::DeviceAddress device_base; ///< device address of guest_base
};
constexpr u64 PageSize = 64 * 1024;

bool On();
/// Creates the windows (instance and scheduler of the rasterizer). Called once by the rasterizer owner.
void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
/// The guest mapped [addr, addr+size) with GPU access (from the runtime, any thread). Pages are bound before the
/// next submit that can reference them (see ReadyForSubmit).
void OnMap(VAddr addr, u64 size);
/// The guest unmapped [addr, addr+size). Pages are released after the frame fence.
void OnUnmap(VAddr addr, u64 size);
/// Draw thread, at submit start: waits (GPU-side, timeline semaphore) for pending sparse binds.
void ReadyForSubmit(Vulkan::Scheduler& scheduler);
/// The window holding [addr, addr+size) with every page resident, or nullptr.
const Window* Find(VAddr addr, u64 size);
/// Draw thread, at bind time: makes every page of [addr, addr+size) resident for this submit. A page that is not
/// yet bound is bound now and filled from guest memory (through the backing view) before this submit's
/// commands. False when a page cannot be made resident (outside the window, not GPU-mapped, over budget).
bool Touch(Vulkan::Scheduler& scheduler, VAddr addr, u64 size);
/// Device address of guest address addr (0 when not resident). For verify paths and CPU-side replicas.
vk::DeviceAddress Translate(VAddr addr);
std::span<const Window> Windows();
/// Draw thread: records a copy of guest bytes [addr, addr+size) into the RESIDENT pages of the mirror (the
/// announce drain keeps resident pages coherent). Never binds: a non-resident page is skipped (counted) and is
/// filled by Touch when a shader first asks for it. When sample is not empty (at most 64 KiB, at most size), it
/// receives the first sample.size() bytes of addr, taken from the same bytes the copy stages for the GPU. True when
/// the sample was taken (the range starts resident); a sampled range is therefore resident.
bool Upload(Vulkan::Scheduler& scheduler, VAddr addr, u64 size, std::span<u8> sample = {});
/// Draw thread, after a recorded op (ResetBindings): the GPU wrote guest range [addr, addr+size) into src at src_offset
/// through a buffer. Copies it into the resident pages of the mirror; a non-resident page is made resident first (its
/// fill from guest memory is overwritten by this copy). The caller ends rendering and makes src's pending writes
/// visible. Records into the scheduler's command buffer, with barriers on both sides.
/// sotc: peer_arena: src is a buffer-cache arena. Its aliased pages are the same memory as the mirror's, so they are
/// skipped (no copy); aliased pages are copied when src is not an arena.
void SyncFromBuffer(Vulkan::Scheduler& scheduler, vk::Buffer src, u64 src_offset, VAddr addr, u64 size,
                    bool peer_arena);
/// Draw thread, after a recorded op: the shader wrote guest range [addr, addr+size) through the mirror. Copies its
/// resident pages into dst at dst_offset (the caller has obtained dst for writing and ends rendering first).
/// sotc: peer_arena as in SyncFromBuffer (dst is an arena: aliased pages are already written).
void SyncToBuffer(Vulkan::Scheduler& scheduler, vk::Buffer dst, u64 dst_offset, VAddr addr, u64 size,
                  bool peer_arena);

// sotc: A2b. Aliases (SOTC_MIRROR, single copy). The buffer cache binds device memory for a guest range into an arena
// (EnsureResident) and unbinds it (UnbindResident). Each such bind is mirrored here: the same VkDeviceMemory and
// memory offset are bound into the mirror window at (guest page - window base), so the page is one memory page seen
// through two sparse buffers (VK_BUFFER_CREATE_SPARSE_ALIASED_BIT, sparseResidencyAliased). The arena blocks are 64 KiB
// pages (block_size equals the sparse page alignment), so guest and memory offsets are both 64 KiB aligned and map
// one to one: no shift, no copy. Alias ops are queued and submitted by the buffer cache in its arena bind submit
// (TakeAliasCalls), so an alias unbind shares the arena unbind's submit, its waits and its frame fence.
using ArenaHold = bool (*)(void* ctx, VAddr addr, u64 size);
/// Registers the buffer cache hook that places [addr, addr+size) in an arena (its placement is aliased at once). The
/// hook runs on the draw thread from Touch. nullptr clears it.
void SetArenaHold(ArenaHold fn, void* ctx);
/// True when the mirror aliases arena memory of this memory type (the window's memoryTypeBits include it).
bool AliasAccepts(u32 memory_type);
/// Queues the alias bind of [addr, addr+size) (whole 64 KiB pages) to memory at memory_offset. False when refused: a
/// page is outside the window, a page is owned (own-page bind, kept: the old sync path then covers it), or the mirror
/// is off. The pages are resident (aliased) from this call on.
bool AliasBind(VAddr addr, u64 size, vk::DeviceMemory memory, u64 memory_offset);
/// Queues the alias unbind of [addr, addr+size). Pages not aliased are skipped. Safe from any thread.
void AliasUnbind(VAddr addr, u64 size);
/// True while alias ops are queued (or any page is aliased: the buffer cache then sends its barriers).
bool AliasActive();
/// Records a global memory barrier between writes and reads through two aliases of the same memory (the mirror and an
/// arena). Recorded after an op that wrote memory, while any page is aliased.
void AliasBarrier(vk::CommandBuffer cmdbuf);
/// One sparse bind call of the mirror buffer. Calls run in order; the last one is the wait the caller adds.
struct AliasCall {
    vk::Buffer buffer;
    std::vector<vk::SparseMemoryBind> binds;
};
/// Takes every queued alias op as calls, split so that no page appears twice in one call.
void TakeAliasCalls(std::vector<AliasCall>& out);
void Report(char* out, u32 size, double seconds);
} // namespace Mirror

// ---------------------------------------------------------------------------------------------------------------
// A3. Shader-computed buffer addresses (switch SOTC_SHADER_ADDR). The shader reads base and size from the V# words
// in its flat buffer and adds the window base from the push packet. This is the CPU replica of that formula.
namespace ShaderAddr {
bool On();
/// The mode as the profile carries it (Shader::Profile::shader_addr): 0 off, 1 addresses, 2 verify.
u8 Mode();
/// SOTC_SHADER_ADDR=2 (verify): creates the debug buffer (host-visible, with a device address) and checks the
/// refusals: SOTC_HEAP is on, or no mirror window exists. Refused runs exit. Called once, after Mirror::Init.
void Init(const Vulkan::Instance& instance);
/// The debug buffer's device address, for DrawPacket::index_table_address (0 unless mode 2). The packet
/// field is free in mode 2 because SOTC_HEAP is refused with it.
vk::DeviceAddress VerifyAddress();
/// What the generated code computes for a V# (base, num_records/stride already decoded): the device address of
/// guest address `base`, or 0 when the range is not in one window.
vk::DeviceAddress Expected(VAddr base, u64 size);
} // namespace ShaderAddr

// ---------------------------------------------------------------------------------------------------------------
// A4. Global descriptor heap (switch SOTC_HEAP) and per-table index memo.
namespace Heap {
bool On();
/// True when VK_EXT_descriptor_heap is used; false: descriptor indexing fallback.
bool UsesDescriptorHeap();
/// The heap is the N5 bindless arrays (vk_bindless.cpp): one runtime array per image key, slot 0 = null view.
/// Needs SOTC_N5_BINDLESS=1 and SOTC_BINDLESS_SAMP=1 (and SOTC_LAYOUT_UNIFIED, see TableMemo).
void Init(const Vulkan::Instance& instance);
u32 NullImageIndex(u32 key);
u32 NullSamplerIndex();
/// The view's slot in the array of `key` for `layout` (acquired when missing; released by the view's destructor
/// after the frame fence). Call with the texture cache lock held.
u32 IndexOf(VideoCore::ImageView& view, u32 key, vk::ImageLayout layout);
u32 IndexOfSampler(VideoCore::Sampler& sampler);
} // namespace Heap

namespace TableMemo {
/// The heap indices of one sharp table's T#/S# entries, in a per-frame ring on the GPU.
struct IndexTable {
    vk::DeviceAddress device_address;
    u32 count;
};
bool On();
/// Creates the per-tick index ring (rasterizer constructor, when On()).
void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
/// The index table of one stage (content-keyed on the T#/S# words the stage reads; a changed table resolves through
/// the texture cache once). Layout: entry i = heap index of stage.images[i], entry images.size()+j = heap index of
/// stage.samplers[j], Heap::NullImageIndex(key) for a null T#. device_address 0 = refused (the draw path binds as today).
/// Valid for the current tick.
IndexTable Resolve(VideoCore::TextureCache& texture_cache, const Shader::Info& stage,
                   AmdGpu::BorderColorBuffer border_color);
void Report(char* out, u32 size, double seconds);
} // namespace TableMemo

// ---------------------------------------------------------------------------------------------------------------
// A5. One pipeline layout and a push-constant packet per op (switch SOTC_PACKET).
/// Byte offset of DrawPacket in the push-constant block: right after Shader::PushData (56 bytes). Packet pipelines
/// carry no bindless slot words. 56 + 72 = 128 bytes, the push constant budget Vulkan guarantees.
inline constexpr u32 PacketPushOffset = 56;
struct DrawPacket {
    u64 flatbuf_address;      ///< flat buffer (user data + sharp tables) of the vertex or compute stage
    u64 index_table_address;  ///< TableMemo::IndexTable::device_address (0: no textures)
    u64 mirror_guest_base;    ///< Mirror window 0
    u64 mirror_device_base;
    u64 arena_guest_base;     ///< ring-8 per-frame arena window (zero-copy host memory), 0 when none
    u64 arena_device_base;
    u64 arena_size;
    u64 flatbuf_fragment_address; ///< flat buffer of the fragment stage (flat buffers are per stage)
    u64 mirror_size;              ///< bytes of mirror window 0 (the shader bounds mirror addresses with it)
};
static_assert(sizeof(DrawPacket) == 72, "56 + 72 = 128: exactly the push constant budget Vulkan guarantees");
namespace Packet {
bool On();
}

// ---------------------------------------------------------------------------------------------------------------
// A6. Passes and parallel recording (switches SOTC_PASSES, SOTC_PASS_RECORD).
namespace Passes {
bool On();
bool ParallelRecord();
} // namespace Passes

} // namespace SotcArch

// C bridge for src/runtime_memory.c (weak there: the loader links without the GPU library in tests).
extern "C" void bbgpu_mirror_on_map(uint64_t addr, uint64_t size);
extern "C" void bbgpu_mirror_on_unmap(uint64_t addr, uint64_t size);
