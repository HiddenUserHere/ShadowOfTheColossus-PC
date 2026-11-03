// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the interfaces of the draw architecture in docs/re-stutters/native/ARCH-DRAW.md. Every part is off unless
// its switch is set; =2 runs the old path as well and counts differences. This header is the contract the parts
// are written against: change it only together with every implementation.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/amdgpu/regs_texture.h" // AmdGpu::BorderColorBuffer
#include "video_core/texture_cache/types.h" // sotc: VideoCore::ImageId, SubresourceRange (TableMemo::TableBinds)

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {
struct Image;
struct ImageView;
class Sampler;
class TextureCache;
} // namespace VideoCore

namespace Shader {
struct Info;
struct BindlessSamplerSlots;
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
/// The switch SOTC_LAYOUT_UNIFIED: 0 forces it off, 1 on (needs the feature), 2 verify (the old layouts are used
/// and counted), unset or "auto" (the default): on when the feature is present and nothing refuses it.
/// Requested() is false only for the forced off: the instance probes the feature whenever it may be used.
bool Requested();
/// True when the mode acts once the feature is there: =1 or auto. Supported() still has to hold (see Active).
bool On();
/// True when the device reports unifiedImageLayouts. Set once by the instance (NoteSupported), after the
/// device is created: a refused device reports false again.
bool Supported();
/// sotc: the instance's report of the feature (vk_instance.cpp). false also after a device retry without it.
/// `reason` is a string literal naming why, for the line LogDecision prints.
void NoteSupported(bool supported, const char* reason = nullptr);
/// True when the mode acts: On() and Supported(). Only then Pick returns GENERAL and barriers are dropped.
/// Without the feature every layout is the one it was before (the fallback: no GENERAL attachments).
bool Active();
/// Prints the one startup line with the decision (on, verify or off) and its reason. Call once, after the device exists.
void LogDecision();
/// True for SOTC_LAYOUT_UNIFIED=2 with the feature: the drops are counted and the old barriers are kept.
bool Verify();
/// The layout a command, attachment, descriptor or barrier names for a texture-cache image: GENERAL when Active();
/// the layout itself otherwise, and for present layouts always.
vk::ImageLayout Pick(vk::ImageLayout layout);
/// Non-GENERAL transitions the mode still emits, by the reason they stay (the SOTC LAYOUT report).
enum class Residual : u8 {
    Present,  ///< the swapchain image: PRESENT_SRC is required by the spec
    Sdk,      ///< our copies handed to Streamline (DLSS-G) or FFX (FSR-FG) with declared states
    Internal, ///< our own raw images: presenter frames, FSR/PP passes, ui shadow, shadow scratch
    Count,
};
/// Counts one residual transition (the instrumented sites call it where the barrier is recorded).
void NoteResidual(Residual reason, u64 count = 1);
/// A kept barrier that orders a write or a first use (never dropped). SOTC LAYOUT "ordering" count.
void NoteKeptWrite();
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
/// sotc: the heap push block (Shader::HeapBlockOffset, 64 bytes): the heap debug buffer's device address, and in GPU
/// verify the classic slot words of the heap stage, compacted for the shader (layout: resource.h, HeapVerifyWords).
struct HeapBlock {
    u64 debug_address;
    union {
        /// sotc: SOTC_HEAP_VS: the index table address of the vertex heap stage (Shader::HeapVsTableOffset, byte 136).
        /// Real mode only: the classic words are GPU verify's, and SOTC_HEAP_VS never runs with it.
        u64 vs_table_address;
        u32 words[14];
    };
};
static_assert(sizeof(HeapBlock) == 64, "HeapBlock must fill Shader::HeapPushBytes - Shader::HeapBlockOffset");

namespace Heap {
/// SOTC_HEAP=1 (or 2) and its requirements met (CheckRequirements). Off means the pipelines are built as today.
bool On();
/// sotc: SOTC_HEAP_VS=1 (read once): a vertex stage may be a heap stage as well as the fragment. Needs On(), and is off
/// with GPU verify (SOTC_HEAP_GPU_VERIFY=1), whose verify words take the bytes where the vertex table goes.
bool VertexOn();
/// The profile value (Shader::Profile::heap_images): 0 off, 1 heap, 2 heap with the GPU verify (SOTC_HEAP_GPU_VERIFY=1).
u8 ProfileValue();
/// The heap debug buffer's device address (0 when heap is off or the buffer could not be made).
vk::DeviceAddress DebugAddress();
/// Fills block for one draw: the debug address, and in GPU verify the classic slot words of `stage` (the draw's heap
/// stage, nullptr: none) compacted from this draw's bindless slots (see HeapBlock).
void FillBlock(HeapBlock& block, const Shader::Info* stage, const Shader::BindlessSamplerSlots& slots);
/// One report line into out (the debug buffer's counters and the first distinct records); resets the counters.
void Report(char* out, u32 size, double seconds);
/// True when VK_EXT_descriptor_heap is used; false: descriptor indexing fallback.
bool UsesDescriptorHeap();
/// Called once by the instance, after the bindless flags are set. SOTC_HEAP needs SOTC_N5_BINDLESS=1,
/// SOTC_BINDLESS_SAMP=1 and SOTC_LAYOUT_UNIFIED=1 (heap descriptors are written in GENERAL) and a device with the
/// bindless arrays. When one is missing: one log line, and On() is false for the whole run (no misrendering).
void CheckRequirements(bool bindless_images, bool bindless_samplers);
/// The heap is the N5 bindless arrays (vk_bindless.cpp): one runtime array per image key, slot 0 = null view.
void Init(const Vulkan::Instance& instance);
/// True when a stage cannot be a heap stage: Shader::HeapStaticRefusal, or a host rule (the composite rule of
/// BindTexturesFull, and the UI linear rule unless SOTC_HEAP_UI_LINEAR=1, which the memo answers per op). The pipeline
/// cache calls it before it picks the heap variant.
bool StageRefused(const Shader::Info& stage);
/// sotc: SOTC_HEAP_COMPOSITE_OP=1 (read once). The composite rule of StageRefused is then not applied per program:
/// the heap stage stays, and BindHeapImages sends each op the composite predicates act on (CompositeOf) to the classic
/// bind, which runs the composite side effects. Off: the per-program refusal, as before.
bool CompositeOpOn();
u32 NullImageIndex(u32 key);
u32 NullSamplerIndex();
/// The view's slot in the array of `key` for `layout` (acquired when missing; released by the view's destructor
/// after the frame fence). Call with the texture cache lock held.
u32 IndexOf(VideoCore::ImageView& view, u32 key, vk::ImageLayout layout);
u32 IndexOfSampler(VideoCore::Sampler& sampler);
} // namespace Heap

// ---------------------------------------------------------------------------------------------------------------
// sotc: the UI linear rule (SOTC_RENDER_RES names a size other than 1920x1080). A single-image stage whose one asset
// texture is sampled with a point magnification has the sampler magnified bilinearly instead: the game draws its 1080p
// 2D art with the point sampler, and a larger render size would show hard steps. The classic bind
// (Rasterizer::UiLinearAsset, BindSamplers) and the heap resolve (TableMemo, ResolveFresh) take the decision from these
// functions only, so both make the same one.
namespace UiLinear {
/// SOTC_RENDER_RES names another size than 1920x1080 (read once).
bool RenderResOn();
/// The static part of the rule, decided by the T# words of the stage: RenderResOn, one image (not an array), and a
/// number format that is not an integer one.
bool StageEligible(const Shader::Info& stage);
/// The dynamic part, decided by the bound image: not a render or depth target, not a storage image, a VO surface, a
/// binding target, nor a depth image.
bool ImageEligible(const VideoCore::Image& image);
/// sotc: SOTC_HEAP_UI_LINEAR=1 (read once): the heap resolve decides the rule per op (TableMemo), so the stage stays a
/// heap stage (Heap::StageRefused does not refuse it).
bool HeapOn();
} // namespace UiLinear

namespace TableMemo {
/// The heap indices of one sharp table's T#/S# entries, in a per-frame ring on the GPU.
struct IndexTable {
    vk::DeviceAddress device_address;
    u32 count;
};
/// sotc: the images one stage's index table binds in this op (SOTC_HEAP real mode). Entry i matches stage.images[i]:
/// the bound image (the depth image of a stencil one), its view range as the classic bind's transition uses it, and
/// the null image when the T# is null (image id invalid).
constexpr u32 kMaxBindImages = 16;
/// sotc: SOTC_BIND_CACHE_HEAP. The index entries of one resolution: the images then the samplers (kMaxImages +
/// NUM_SAMPLERS in table_memo.cpp, asserted there).
constexpr u32 kMaxBindIndices = 32;
/// sotc: SOTC_BIND_CACHE_HEAP. A stored resolution (a BindSet, table_memo.cpp): its entry index and the serial it was
/// stored under. Live while that entry is still valid with the same serial (HeapLive); an event that kills the BindSet
/// makes every reference to it dead, with no per-reference bookkeeping.
struct HeapRef {
    u32 entry{};
    u64 serial{};
    bool valid{};
};
struct TableBinds {
    u32 count{};       ///< images (the bound ones are in image / range)
    u32 index_count{}; ///< index entries (images + samplers) in `index`
    std::array<VideoCore::ImageId, kMaxBindImages> image{};
    std::array<VideoCore::SubresourceRange, kMaxBindImages> range{};
    /// sotc: SOTC_BIND_CACHE_HEAP. The indices the resolution wrote, and the BindSet they came from (valid when the
    /// resolution was a stored BindSet; FillBinds sets it).
    std::array<u32, kMaxBindIndices> index{};
    HeapRef ref{};
};
bool On();
/// Creates the per-tick index ring (rasterizer constructor, when On()).
void Init(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
/// The index table of one stage (content-keyed on the T#/S# words the stage reads; a changed table resolves through
/// the texture cache once). Layout: entry i = heap index of stage.images[i], entry images.size()+j = heap index of
/// stage.samplers[j], Heap::NullImageIndex(key) for a null T#. device_address 0 = refused (the draw path binds as today).
/// Valid for the current tick.
/// bindless_gen: Rasterizer::BindlessGeneration of this op. An entry validated under another generation is checked
/// slot by slot (each heap slot it names must still hold its view) before it is replayed.
/// binds: sotc: SOTC_HEAP real mode only (nullptr otherwise). Set to the images of this op, after a hit has been
/// validated and its per-bind UpdateImage run (TouchBoundImage), or after a resolution. The caller then does the
/// transitions and the bound flags of those images (the classic bind's second pass, without descriptors).
IndexTable Resolve(VideoCore::TextureCache& texture_cache, const Shader::Info& stage,
                   AmdGpu::BorderColorBuffer border_color, u64 bindless_gen, TableBinds* binds);

/// sotc: SOTC_PR_KEYS. The memo key of one heap stage (table_memo.cpp KeyOf), computed ahead by a GpuPrep thread
/// (PrepareDraw) from the op's flat buffer and border color base. The record is used by Resolve only for the stage
/// pointer and border base it was made for; the words are the same ones KeyOf writes.
struct PreKey {
    /// KeyWordsOf of the largest stage the memo takes (16 images, 16 samplers); table_memo.cpp asserts it.
    static constexpr u32 kMaxWords = 3 + 5 * 16 + 3 * 16;
    const Shader::Info* stage{}; ///< the pipeline stage the key was computed for
    u64 border{};                ///< border color base it was computed with
    u64 hash{};                  ///< KeyOf's return value
    u32 words{};                 ///< KeyWordsOf(stage); the entries past it are not read
    bool valid{};
    std::array<u64, kMaxWords> key{};
};
/// Pure (no shared state): KeyOf of `stage` into `out`, with `out.stage` and `out.border` set. Reads the stage's flat
/// buffer through the calling thread's Shader::g_info_slot, so a GpuPrep thread must have its slot filled. false
/// (out.valid false) when the key does not fit PreKey::kMaxWords.
bool ComputeKey(const Shader::Info& stage, AmdGpu::BorderColorBuffer border_color, PreKey& out);
/// Resolve with the key precomputed by ComputeKey when `pre` is valid for this stage and border base (SOTC_PR_KEYS);
/// otherwise the key is computed here, as Resolve above does.
IndexTable Resolve(VideoCore::TextureCache& texture_cache, const Shader::Info& stage,
                   AmdGpu::BorderColorBuffer border_color, u64 bindless_gen, TableBinds* binds,
                   const PreKey* pre);
/// sotc: SOTC_HEAP accounting of the stage binds of heap ops: skipped (the table covers the stage), or the classic
/// bind ran (reason: 0 GPU verify, 1 render target or forced general, 2 stencil image, 3 rebind pending,
/// 4 composite draw under SOTC_HEAP_COMPOSITE_OP).
void NoteBindSkipped();
void NoteBindClassic(u32 reason);
/// sotc: SOTC_HEAP_VS. The vertex heap stage of an op: skipped (true) or its classic bind ran (false, the fallback).
void NoteVertexBind(bool skipped);
/// sotc: SOTC_BINDSET (stage 1, table_memo.cpp): the mode, 0 off, 1 replay, 2 verify; 0 when the table memo is not up.
/// A replay (mode 1) replays a stored BindSet without validating it, the events of bindset_events.h keep it valid.
int BindSetMode();
/// sotc: SOTC_BINDSET. Rasterizer::BindHeapImages skipped the transition of an image its backing already holds in the
/// state the read needs (mode 1), or made it and checked the prediction (mode 2: barrier = Transit returned one).
void NoteTransitSkipped();
void NoteTransitVerify(bool barrier);
/// sotc: SOTC_BINDSET. One line for the 5 s report (counters reset on read); empty when the layer is off.
void BindSetReport(char* out, u32 size, double seconds);
/// sotc: SOTC_BIND_CACHE_HEAP. The effective mode: 0 off, 1 a bind cache hit replays the heap resolution it stored
/// (HeapReplay) instead of TableMemo::Resolve, 2 the same plus a fresh Resolve compared with the stored one. Off unless
/// SOTC_BINDSET=1, SOTC_BIND_CACHE_PACKET=1 and the frame bind cache are on too (the events are what keep a hold valid).
int HeapHoldMode();
/// sotc: SOTC_BIND_CACHE_HEAP. The hold is live: the events queued so far are applied first (the caller holds the
/// texture cache lock), then the BindSet is still valid under its serial and not dropped by the time rule. Mutates
/// only what the events and the time rule would mutate anyway.
bool HeapLive(VideoCore::TextureCache& texture_cache, const HeapRef& ref);
/// sotc: SOTC_BIND_CACHE_HEAP. Replays a live hold as a BindSet hit does (stamps, LRU, heap slot use, the bound
/// images' bind evidence) and publishes its index table for this tick. `binds` (optional) receives the images of the
/// resolution. Returns device_address 0 when the hold is not live or the publish overflowed; the caller then refuses.
/// Call only after HeapLive returned true for `ref` under the same lock.
IndexTable HeapReplay(VideoCore::TextureCache& texture_cache, const HeapRef& ref, u32 index_count,
                      u64 tick, TableBinds* binds);
/// sotc: SOTC_BIND_CACHE_HEAP. Whether two resolutions of one stage name the same indices and images (mode 2).
bool SameBinds(const TableBinds& a, const TableBinds& b);
/// sotc: SOTC_BIND_CACHE_HEAP counters: a stage replayed from a hold (resolve_skipped: no Resolve ran for it), a bind
/// cache entry whose hold was dead at a lookup, a hold the decline rule refused, a stored hold, a verify comparison
/// and its mismatch.
void NoteHeapHit(bool resolve_skipped);
void NoteHeapDead();
void NoteHeapDeclined();
void NoteHeapStored();
void NoteHeapVerify(bool match);
/// sotc: SOTC_BIND_CACHE_HEAP. One line for the 5 s report (counters reset on read); empty when the mode is off.
void HeapReport(char* out, u32 size, double seconds);
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
