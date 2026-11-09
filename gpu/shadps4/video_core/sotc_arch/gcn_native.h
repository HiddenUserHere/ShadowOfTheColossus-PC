// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: ARCH-GCN-NATIVE stages S1-S5 (docs/re-stutters/native/ARCH-GCN-NATIVE.md; every design decision and its reason
// in docs/re-stutters/native/GCN-NATIVE-DECISIONS.md). The shaders resolve their own resources, as GCN does: buffers
// through the guest page table (SotcArch::AddrTable), textures and samplers through a GPU T# table, freshness and
// barriers by event instead of per op.
//
// This header is the contract the parts are written against: change it only together with every implementation.
// GPU-visible layouts: shader_recompiler/gcn_layout.h.
//
// Switches (SotcArch::SwitchMode: unset or 0 off = today's behaviour bit for bit, 1 on, 2 verify = the old path runs as
// well and the differences are counted). Each one is refused (one log line, mode 0 for the whole run) when what it
// needs is missing:
//   SOTC_GCN_MEM      S1 guest page table for every buffer kind, filled by events      needs int64 + BDA
//   SOTC_GCN_BUF      S2 every guest buffer by address (plain, formatted, written)       needs GCN_MEM, SOTC_GENERIC_BUFFERS=2
//   SOTC_GCN_TEX      S3 GPU T#/S# table with miss feedback                              needs SOTC_HEAP on, layouts unified
//   SOTC_GCN_SYNC     S4 freshness per submit (dirty pages and images in one batch)      needs GCN_MEM
//   SOTC_GCN_BARRIER  S5 static barriers, GENERAL layouts, no per-op transitions          needs layouts unified active
//   SOTC_GCN_COMMIT   S6 thin draw thread: dispatches prepared ahead, commit lane            needs SOTC_PARALLEL_RECORD=1
#pragma once

#include <cstdint>
#include <span>

#include "common/types.h"
#include "shader_recompiler/gcn_layout.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h" // VideoCore::ImageId

namespace Vulkan {
class Instance;
class Scheduler;
class Pipeline;
} // namespace Vulkan

namespace VideoCore {
class BufferCache;
class TextureCache;
struct Buffer;
struct Image;
struct ImageView;
class Sampler;
} // namespace VideoCore

namespace Shader {
struct Info;
struct ImageResource;
struct SamplerResource;
} // namespace Shader

namespace SotcGcn {

enum class Stage : u8 { Mem, Buf, Tex, Sync, Barrier, Commit, Count };

/// The effective mode of a stage (0, 1, 2) after its requirements (Refuse sets it to 0). Read once, then constant,
/// except that Refuse may lower it before the first frame. Cheap (one relaxed load).
int Mode(Stage stage);
/// Refuses a stage for the run: its mode becomes 0 and one "SOTC GCN: <stage> refused: <reason>" line is logged.
/// Every stage that needs it is refused as well (Buf and Sync need Mem). Call before the first op.
void Refuse(Stage stage, const char* reason);
/// One startup line per stage with its mode (rasterizer constructor, after every Init).
void LogDecisions();
/// One 5 s report line with each stage's counters (counters reset); empty when every stage is off.
void Report(char* out, u32 size, double seconds);

// ---------------------------------------------------------------------------------------------------------------
// The globals buffer (gcn_layout.h): page-table pointer, miss ring, seen bitmap, T# table. gcn_globals.cpp.
namespace Globals {
/// Creates the buffer when Buf or Tex is on (rasterizer constructor, after AddrTable::Init). Refuses Buf and Tex when
/// the device has no buffer device address or no host-visible memory for it.
void Init(const Vulkan::Instance& instance);
/// Device address of the buffer (0 when not created). Goes into DrawPacket::arena_guest_base for gcn pipelines.
vk::DeviceAddress Address();
/// Host pointer of the mapping (nullptr when not created). Tex writes its entries through it.
u8* Mapped();
/// Kind-specific consumers of the drained records.
struct MissSink {
    /// kMissPage. sotc: flags (Shader::Gcn::kMissPage*), base and size: the access's V# (gcn_layout.h).
    void (*page)(void* user, VAddr page_addr, u32 flags, VAddr base, u32 size) = nullptr;
    void (*image)(void* user, const u32 (&tsharp)[8], u32 view_class) = nullptr;   ///< kMissImage
    void (*sampler)(void* user, const u32 (&ssharp)[4], u32 sampler_class) = nullptr; ///< kMissSampler
    void* user = nullptr;
};
/// Draw thread, once per submit (SotcGcn::OnSubmitStart): hands every complete record since the last drain to sink,
/// counts overflows, clears the seen bitmap. Records from work still in flight are taken by a later drain.
void DrainMisses(const MissSink& sink);
void Report(char* out, u32 size, double seconds);
} // namespace Globals

// ---------------------------------------------------------------------------------------------------------------
// S1. Guest page table (SotcArch::AddrTable, addr_table.h) for every buffer kind. gcn_mem.cpp.
namespace Mem {
bool On();     ///< Mode(Stage::Mem) != 0
bool Verify(); ///< Mode(Stage::Mem) == 2
/// Classic BindBuffers, after ObtainBuffer answered (buffer, offset) for the guest range [addr, addr + size): mode 2
/// compares the table's answer (AddrTable::Translate of the first and of the last byte) with
/// buffer->BufferDeviceAddress() + offset (and + size - 1). Counts match, mismatch, not in table; logs the first 16
/// mismatches. Draw thread. Mode 1: no-op.
void VerifyBinding(VAddr addr, u64 size, const VideoCore::Buffer* buffer, u64 offset, bool is_written);
/// Lock-free: every table page of [addr, addr + size) is placed (has a non-zero entry). Any thread.
bool Placed(VAddr addr, u64 size);
/// sotc_guest_memory.cpp: a ring-8 imported run [addr, addr + size) is now served at device address device_addr
/// (owner: the imported VkBuffer's VideoCore::Buffer, or nullptr when it has none). Forwards to AddrTable::OnPlace.
void OnImport(VAddr addr, u64 size, vk::DeviceAddress device_addr, const VideoCore::Buffer* owner);
/// sotc_guest_memory.cpp: the imported run is released. Forwards to AddrTable::OnRemove.
void OnRelease(VAddr addr, u64 size);
void Report(char* out, u32 size, double seconds);
} // namespace Mem

// ---------------------------------------------------------------------------------------------------------------
// S2. Every guest buffer by address. gcn_buf.cpp (host); the recompiler side reads Shader::Profile::gcn_buf.
namespace Buf {
bool On();
bool Verify();
/// Shader::Profile::gcn_buf: 0 off, 1 on, 2 verify. 0 unless On() and the profile already has shader_addr in table
/// mode (3 or 4): the gcn path extends the table path, it does not replace it.
u8 ProfileValue();
/// Draw thread, BindBuffers, per shader-addressed buffer of a gcn pipeline, instead of AddrTable::Touch. When every
/// page of the range is placed (Mem::Placed) and Sync is on (freshness is the submit batch's job), returns true with
/// no further work (the fast path: two loads per page run). Otherwise runs AddrTable::Touch (residency + sync) and
/// returns its answer. Mode 2 runs Touch always and counts the cases the fast path would have skipped while Touch
/// found work to do.
bool BindAddressed(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr addr, u64 size,
                   bool is_written, bool is_texel = false);
/// MissSink::page target: makes the page resident through AddrTable::Touch (counted). Takes no lock itself.
/// sotc: a formatted or written access (flags) touches its whole V# range [base, base + size) with its own is_texel /
/// is_written, as the bind does; a plain read touches the page only, and not at all when the page is placed by then
/// (its entry is valid: a miss Touch must not rewrite it).
void OnPageMiss(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr page_addr, u32 flags, VAddr base,
                u32 size);
void Report(char* out, u32 size, double seconds);
} // namespace Buf

// ---------------------------------------------------------------------------------------------------------------
// S3. GPU T#/S# table with miss feedback. gcn_tex.cpp (host); the recompiler side reads Shader::Profile::gcn_tex.
namespace Tex {
bool On();
bool Verify();
/// Shader::Profile::gcn_tex: 0 off, 1 on, 2 verify (the shader still uses the TableMemo index table; the host checks
/// the table on the CPU). 0 unless On() and the profile has heap_images.
u8 ProfileValue();
/// The view class of a binding (gcn_layout.h ViewClass with the bindless key of BindlessImageKeyOf).
u32 ViewClassOf(const Shader::Info& info, const Shader::ImageResource& desc);
/// Inserts or replaces the entry of (tsharp, view_class) -> slot (the view's slot in its key's array). Host thread
/// holding the texture cache lock. Records the reverse link slot -> entry for OnSlotReleased. False when the probe
/// window (kTexMaxProbes) is full (counted; the shader then misses and falls back to the null slot).
bool Insert(const u32 (&tsharp)[8], u32 view_class, u32 slot);
bool InsertSampler(const u32 (&ssharp)[4], u32 sampler_class, u32 slot);
/// The sampler class of a binding (gcn_layout.h SamplerClass); single_lod is the run-time T# fact of its post op
/// (false unless post_op is DisableAnisoIfSingleLod).
u32 SamplerClassOf(const Shader::SamplerResource& desc, bool single_lod);
/// The table's answer on the CPU (the mapping is the source of truth): the slot, or ~0u when absent.
u32 Lookup(const u32 (&tsharp)[8], u32 view_class);
/// Heap (vk_bindless.cpp Release / Evict): slot `slot` of array `key` no longer holds its view. Every entry naming it
/// becomes a tombstone before the slot can be reused (the caller's deferral after the frame fence still applies).
void OnSlotReleased(u32 key, u32 slot);
/// Heap: sampler slot released.
void OnSamplerSlotReleased(u32 slot);
/// MissSink::image target: resolves the T# through the texture cache exactly as the classic heap bind does for one
/// binding with that view class (FindTexture with ImageViewInfo(tsharp, desc built from the class), Heap::IndexOf
/// in GENERAL), then Insert. Draw thread; takes the texture cache lock.
void OnImageMiss(VideoCore::TextureCache& texture_cache, const u32 (&tsharp)[8], u32 view_class);
void OnSamplerMiss(VideoCore::TextureCache& texture_cache, const u32 (&ssharp)[4], u32 sampler_class);
/// Mode 2 and the first-use prefill: after TableMemo::Resolve answered `slot` for binding i of a heap stage, compares
/// Lookup with it (mode 2: counted match / mismatch / absent) and inserts it when absent (both modes: the CPU saw this
/// T# first, so the GPU will not miss it). Draw thread, texture cache lock held.
/// image: the image the resolution bound (recorded for ForEachReferencedImage and for OnImageEvent).
void NoteResolved(const u32 (&tsharp)[8], u32 view_class, u32 slot, VideoCore::ImageId image);
/// The sampler counterpart of NoteResolved (mode 2 compare, insert when absent).
void NoteResolvedSampler(const u32 (&ssharp)[4], u32 sampler_class, u32 slot);
/// An image event (SotcArch::BindSet::Cause, bindset_events.h) for texture cache image index image_index, posted from
/// any thread whether or not SOTC_BINDSET is armed. Every table entry whose image has that index becomes a tombstone,
/// so the next use misses and resolves again through the texture cache (which picks the image that now owns the T#).
/// ImageDirty tombstones only when Sync is off (with Sync on the submit batch refreshes the image in place).
void OnImageEvent(u32 image_index, u8 cause);
/// The images the table references, for the per-submit freshness pass (Sync): calls fn for each live entry's image
/// at most once per call. Draw thread.
/// Once per submit (SotcGcn::OnSubmitStart): stamps the heap use tick of every slot a live entry names, so the
/// bindless LRU eviction never takes a slot the table still serves (with SOTC_GCN_TEX_SKIP_RESOLVE no per-op Resolve
/// stamps them). Draw thread.
void StampLiveSlots();
/// SOTC_GCN_TEX_SKIP_RESOLVE=1 (read once; gcn_core.cpp): gcn_tex == 1 heap stages whose bindings are all in the
/// table skip the per-op TableMemo::Resolve (needs Barrier on). Measurement until the classic decline cases are events.
bool SkipResolve();
void ForEachReferencedImage(VideoCore::TextureCache& texture_cache, void (*fn)(void* user, VideoCore::Image&),
                            void* user);
void Report(char* out, u32 size, double seconds);
} // namespace Tex

// ---------------------------------------------------------------------------------------------------------------
// S4. Freshness per submit. gcn_sync.cpp.
namespace Sync {
bool On();
bool Verify();
/// True when per-op freshness work for table-addressed buffers may be skipped (On() and not Verify()).
bool SkipOpSync();
/// The per-submit batch (called by SotcGcn::OnSubmitStart): (1) every placed table page whose clean bit is clear is
/// synchronized through the buffer cache (one SynchronizeMemory per merged run, on the upload command buffer, which
/// executes before this submit's commands); (2) every image the T# table references whose guest memory the CPU
/// changed is refreshed (texture cache RefreshImage / UpdateImage path). Counted: runs, bytes, images.
void SubmitBatch(VideoCore::BufferCache& cache, VideoCore::TextureCache& texture_cache, Vulkan::Scheduler& scheduler);
/// Mode 2: the per-op path still syncs; it reports here whether it found stale data the batch should have taken.
void NoteOpSync(VAddr addr, u64 size, bool found_stale);
void Report(char* out, u32 size, double seconds);
} // namespace Sync

// ---------------------------------------------------------------------------------------------------------------
// S5. Static barriers and GENERAL layouts. gcn_barrier.cpp.
namespace Barrier {
bool On();
bool Verify();
/// True when a texture-cache transition of an image to `layout` for a shader read or write can be dropped: layouts
/// unified are active (the image is and stays GENERAL) and the hazard is covered by AfterOp / OnPassBoundary. Mode 2
/// returns false and counts the transitions it would have dropped.
bool DropTransition(vk::ImageLayout old_layout, vk::ImageLayout new_layout);
/// After an op whose shaders write memory (Info::writes_memory of any stage, or a storage image): the next op starts
/// with one global memory barrier (shader writes -> shader, vertex, index, indirect reads and writes, transfer). The
/// barrier is recorded lazily by BeforeOp so consecutive writers are not separated by more than one barrier each.
void AfterOp(bool writes_memory);
/// Before an op's commands: records the pending global barrier if one is due. Outside a render pass only; inside one
/// the pending barrier waits for OnPassBoundary.
void BeforeOp(Vulkan::Scheduler& scheduler, bool in_render_pass);
/// At a render pass end: one global barrier for attachment writes -> shader reads, plus any pending one.
void OnPassBoundary(Vulkan::Scheduler& scheduler);
void Report(char* out, u32 size, double seconds);
} // namespace Barrier

// ---------------------------------------------------------------------------------------------------------------
// S6. Thin draw thread. gcn_commit.cpp (switch, eligibility, counters); the prep and draw paths in liverpool.cpp and
// vk_rasterizer.cpp. Two parts:
//   a. Dispatches are prepared on the GpuPrep threads like draws (Rasterizer::PrepareDispatch into the op's
//      PreparedDraw slot; the draw thread takes it with Liverpool::TakePrepared and hands it to DispatchDirect /
//      DispatchIndirect, which pass it to BindResources). Mode 2: the slot is computed and compared with the inline
//      values (counted), never used.
//   b. Commit lane: an op whose pipeline is commit-eligible (Eligible) has nothing left for the bind caches to save
//      (its buffers are table-addressed with S4 freshness, its textures come from the T# table), so BindResources
//      skips the fast bind, the bind cache and the frame bind cache for it (lookup and store) and binds directly.
//      Mode 2: the caches run as before; eligible ops are only counted.
namespace Commit {
bool On();     ///< Mode(Stage::Commit) != 0 (refused without SOTC_PARALLEL_RECORD=1)
bool Verify(); ///< Mode(Stage::Commit) == 2
/// Whether the commit lane may take ops of this pipeline: Buf on with the pipeline's profile gcn_buf != 0 and every
/// guest buffer of every stage shader-addressed (Shader::ShaderAddrBufferAt on the pipeline's profile) except the
/// port's special buffers; Sync::SkipOpSync(); Barrier::On(); every image and sampler binding of the pipeline served by
/// the T# table (a heap stage with profile gcn_tex == 1 and Shader::Gcn::TableStage, with SOTC_GCN_TEX_SKIP_RESOLVE=1),
/// (and heap_images == 1), or no images and samplers at all. The pipeline part is cached on the pipeline object
/// (Pipeline::sotc_gcn_commit); the global terms (Buf, Sync, Barrier on) are read on every call, since a stage can be
/// refused after the first use. Draw thread (BindResources).
bool Eligible(const Vulkan::Pipeline& pipeline);
/// Counters: an op went through the commit lane (bypass = it skipped the caches; false in mode 2 = would have).
void NoteOp(bool eligible, bool is_dispatch);
void NoteBypass();
/// A dispatch reached the draw thread with a prepared slot (taken) or without one (not ready, ineligible, epoch).
void NoteDispatchPrepared(bool taken);
/// Mode 2 and SOTC_VERIFY: a prepared dispatch compared with the inline computation.
void NoteDispatchVerify(bool match);
void Report(char* out, u32 size, double seconds);
} // namespace Commit

// ---------------------------------------------------------------------------------------------------------------
// Draw thread, at each submit start (PcRingCopy::OnSubmitStart's caller): drains the miss ring (Buf / Tex sinks),
// then Sync::SubmitBatch. No-op when every stage is off.
void OnSubmitStart(VideoCore::BufferCache& cache, VideoCore::TextureCache& texture_cache,
                   Vulkan::Scheduler& scheduler);

} // namespace SotcGcn
