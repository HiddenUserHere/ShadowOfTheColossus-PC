// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <chrono>
#include <unordered_map>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/range_stamps.h"
#include "video_core/renderer_vulkan/vk_bda_table.h" // sotc: SOTC_BDA
#include "video_core/renderer_vulkan/vk_bindless.h"  // sotc: SOTC_N5_BINDLESS
#include "video_core/renderer_vulkan/vk_frame_graph.h" // sotc: SOTC_N5_ASYNC_COMPUTE (AsyncBeforeOp)
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_arch/sotc_arch.h" // sotc: SOTC_PACKET (DrawPacket), SOTC_HEAP, SOTC_MIRROR
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/amdgpu/draw_prep.h"
#include "video_core/amdgpu/regs.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace SotcVertexExtent {
struct Draw; // sotc: SOTC_VERTEX_EXTENT (sotc_vertex_extent.h)
}

namespace Vulkan {

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    /// sotc: ARCH-GCN-NATIVE. Draw thread, at each submit start: the miss ring drain and the freshness batch
    /// (SotcGcn::OnSubmitStart). Called by the command processor's submit start (liverpool.cpp); no-op when the GCN
    /// memory, buffer, texture and sync stages are all off.
    void GcnSubmitStart();

    /// resolved/use_resolved: the pipeline the command processor looked up for this draw
    /// (Liverpool::IsBackThreaded), instead of looking it up here.
    /// sotc: prepared: SOTC_PARALLEL_RECORD's inputs for this draw (draw thread), or null.
    void Draw(bool is_indexed, u32 index_offset = 0, const Pipeline* resolved = nullptr,
              bool use_resolved = false, const PreparedDraw* prepared = nullptr);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset,
                      const Pipeline* resolved = nullptr, bool use_resolved = false,
                      const PreparedDraw* prepared = nullptr);

    /// sotc: SOTC_N5_ASYNC_COMPUTE=1. The draw thread calls this before each op, after N5Begin: joins
    /// the async dispatches the op conflicts with, and marks a dispatch that may move (candidate).
    void AsyncBeforeOp(const Vulkan::FrameGraph::Sample& s);

    /// sotc: state of AsyncBeforeOp for the next dispatch (consumed by DispatchDirect/Indirect).
    bool async_pre_seen{};
    bool async_candidate{};
    AmdGpu::OpFootprint async_fp{};

    /// sotc: SOTC_GCN_COMMIT. prepared: the slot PrepareDispatch made for this op (used only with use_resolved).
    void DispatchDirect(const Pipeline* resolved = nullptr, bool use_resolved = false,
                        const PreparedDraw* prepared = nullptr);
    void DispatchIndirect(VAddr address, u32 offset, u32 size, const Pipeline* resolved = nullptr,
                          bool use_resolved = false, const PreparedDraw* prepared = nullptr);

    /// sotc: the pipeline lookups of the draws and dispatches alone (command processor thread).
    const Pipeline* LookupGraphicsPipeline(u16 vertex_sgpr_offset, u16 instance_sgpr_offset);
    /// sotc: the register-only test of FilterDraw: true when it will not record the draw. With
    /// the draw thread the command processor then skips the pipeline lookup (a pipeline for
    /// PrimitiveType::None would not even build).
    bool IsDrawFilteredOut() const;
    const Pipeline* LookupComputePipeline();
    /// sotc: SOTC_ASYNC_IMAGE_CREATE: command processor thread, right after the lookup of the
    /// pipeline of a draw or dispatch (its runtime data in slot 0): queues the creation of the
    /// images its T#s need that do not exist yet (TextureCache::PrefetchTexture).
    /// sotc: SOTC_LOAD_WARM: early bind of a recorded texture (draw thread, idle).
    void WarmTexture(const void* data, size_t size);

    void PrefetchImages(const Pipeline* pipeline) {
        if (pipeline && texture_cache.ImagePrefetchEnabled()) {
            PrefetchImagesImpl(pipeline);
        }
    }
    void PrefetchImagesImpl(const Pipeline* pipeline);
    /// sotc: SOTC_PARALLEL_RECORD. A GpuPrep thread: the register-only inputs of a draw
    /// (draw_prep.h) from that thread's registers and Shader::g_info_slot; and PrefetchImages
    /// for several callers.
    void PrepareDraw(const AmdGpu::Regs& regs, const Pipeline* pipeline, bool is_indexed,
                     bool direct, PreparedDraw& out) const;
    /// sotc: SOTC_GCN_COMMIT. A GpuPrep thread, after the op's stages are set up (Shader::g_info_slot) and map_epoch
    /// is set: the prepared resource inputs of a dispatch (draw_prep.h, is_dispatch). Sets valid when done.
    void PrepareDispatch(const AmdGpu::Regs& regs, const AmdGpu::ComputeProgram& cs, const Pipeline* resolved,
                         PreparedDraw& out) const;
    /// sotc: SOTC_PR_RESOLVE. Called at the end of PrepareDraw on a GpuPrep thread: the
    /// PreparedResources of a graphics draw or a dispatch (draw_prep.h). Resets everything it writes.
    void PrepareResources(const Pipeline* pipeline, PreparedResources& out) const;
    void PrefetchImagesShared(const Pipeline* pipeline);
    /// sotc: SOTC_PARALLEL_RECORD: moves on every MapMemory / UnmapMemory (a prepared draw is
    /// used only while it stands still).
    [[nodiscard]] u64 MapEpoch() const noexcept {
        return map_epoch.load(std::memory_order_acquire);
    }
    bool IsGpuSideThread() const;
    void PrepareGpuSideFault();
    bool RunsGpuWorkElsewhere() const;
    template <typename F>
    void RunOnDrawThread(F&& func) {
        RunOnDrawThreadImpl(Common::UniqueFunction<void>{std::forward<F>(func)});
    }
    void RunOnDrawThreadImpl(Common::UniqueFunction<void>&& func);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    /// sotc: SOTC_SHADER_ADDR, mirror coherence. Call after a GPU write of guest range [addr, addr+size) that `buffer`
    /// holds at `offset` was recorded (FillBuffer, CopyBuffer, HLE copies). Copies it into the mirror.
    void SotcMirrorFromBuffer(const VideoCore::Buffer& buffer, u64 offset, VAddr addr, u64 size);
    /// sotc: SOTC_SHADER_ADDR, mirror coherence. Copies a range the shader wrote through the mirror into the buffer
    /// object that descriptor readers and the CPU use (obtained for writing here, so readbacks see it).
    void SotcMirrorToBuffer(VAddr addr, u64 size);
    /// sotc: SOTC_SHADER_ADDR, mirror coherence. The CPU wrote guest range [addr, addr+size) (a write-through HLE fill):
    /// its resident mirror pages take the guest bytes (SotcArch::Mirror::Upload, before this submit).
    void SotcMirrorFromGuest(VAddr addr, u64 size);
    /// sotc: SOTC_BIND_CACHE_RANGES: a GPU write outside BindResources (shader HLE) drops the
    /// cached binds reading [addr, addr + size). No-op otherwise.
    void BindCacheNoteWrite(VAddr addr, u64 size);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    bool InvalidateMemoryOnGuestFault(VAddr addr, u64 size);
    /// sotc: SOTC_NATIVE=1 (no page tracking): the command processor or draw thread wrote guest memory
    /// [addr, addr + size) itself (WRITE_DATA, constant dumps, DMA on the host, label values). Mode 0 learned
    /// it through the write fault; this tells the buffer cache and the texture cache. allow_exact: bytes over
    /// GPU-written memory are copied to the arena (data the GPU reads); false for labels. No-op in modes 0 and
    /// 2 (the tracker sees the write). Draw thread (or the command processor when there is none).
    void NoteHostWrite(VAddr addr, u64 size, bool allow_exact = true);
    /// sotc: after a guest write fault at addr (BufferCache::MarkWriteAhead).
    void MarkWriteAhead(VAddr addr) {
        buffer_cache.MarkWriteAhead(addr);
    }
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    bool IsMapped(VAddr addr, u64 size);
    /// Command processor thread only, see BufferCache::IsRangeGpuWritten.
    bool IsRangeGpuWritten(VAddr addr, u64 size) const {
        return buffer_cache.IsRangeGpuWritten(addr, size);
    }
    /// Command processor thread only, see BufferCache::ReadClean.
    bool ReadCleanMemory(VAddr addr, void* out, u32 size) {
        return buffer_cache.ReadClean(addr, out, size);
    }
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();
    /// sotc: copies the LOD feedback buffer to the host and clears it (sotc_lod_stats.h)
    void SotcLodFeedbackCycle();
    void OnFence();
    /// sotc: SOTC_BARRIER_GUEST. A guest sync point (SotcArch::GuestBarrier::Sync as u32), draw thread, at its place in
    /// the op stream: records one global memory barrier when a buffer write is pending (mode 1).
    void GuestSyncPoint(u32 kind);

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId();
    bool IsGpuSideThreadId(u32 tid) const;
#endif

    /// sotc: frame generation without the HUD. The game composes its UI layer (an R8G8B8A8 sRGB
    /// render target) over the fp16 scene in one draw into the video out image; while frame
    /// generation (FSR 3 or DLSS-G) runs, that draw is repeated into an image of its own with a transparent UI
    /// texture (SOTC_FG_HUDLESS=0 turns it off). Thread of PrepareFrame: that image for the video
    /// out image at `address`, viewed as `format` (layout ShaderReadOnlyOptimal), when it was
    /// drawn since the last call; else a null view.
    vk::ImageView TakeHudless(VAddr address, vk::Format format);

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    /// sotc: SOTC_RS_MEMO (see RsMemo below).
    u32 RsMemoBuildKey(const GraphicsPipeline* pipeline);
    bool RsMemoPrepare(const GraphicsPipeline* pipeline);
    bool RsMemoBegin(RenderState& state);
    bool RsMemoTargetClean(VideoCore::ImageId image_id);
    /// sotc: SOTC_RS_MEMO=2/3 helpers. The read-only checks of a memo hit at BeginRendering
    /// (counts the miss reason), whether cb_descs holds the memo's image in slot and it is still
    /// the same image (uid and backing), and the result check of SOTC_RS_MEMO=3.
    enum class RsMemoHit : u8 { Miss, Same, Cross };
    RsMemoHit RsMemoBeginCheck();
    bool RsMemoSameImage(u32 slot, VideoCore::ImageId image_id);
    void RsMemoVerify(const RenderState& full, RsMemoHit hit);
    void RefreshImageLayouts();
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed,
                            const PreparedDraw* prepared = nullptr) const;
    /// sotc: SOTC_DRAW_MERGE_STATS=1 (vk_rasterizer.cpp): the draw's content key before its bind. True when it joins the
    /// last full bind: the caller then skips BindResources and the descriptor push. Always sets the pending bind.
    bool MergeReuse(const GraphicsPipeline* pipeline, bool is_indexed, bool indirect, bool joinable,
                    const PreparedDraw* prepared, u32 index_offset);
    /// sotc: SOTC_DRAW_MERGE_STATS (sotc_draw_merge.h): the merge key of one recorded draw and its run. Measurement only.
    void DrawMergeNote(const RenderState& state, const GraphicsPipeline* pipeline, bool is_indexed,
                       bool indirect, VAddr arg_address, u32 stride, u32 max_count, VAddr count_address,
                       const PreparedDraw* prepared);
    /// sotc: the former Update*State, split into a computation from the given registers (any
    /// thread; refuse: set instead of asserting on an unsupported restart index) and the
    /// DynamicState setters (draw thread).
    void ComputeDynamic(const AmdGpu::Regs& regs, bool is_indexed, DynamicValues& out,
                        bool* refuse = nullptr) const;
    void ComputeViewportScissor(const AmdGpu::Regs& regs, DynamicValues& out) const;
    void ComputeDepthStencil(const AmdGpu::Regs& regs, DynamicValues& out) const;
    void ComputePrimitive(const AmdGpu::Regs& regs, bool is_indexed, DynamicValues& out,
                          bool* refuse) const;
    void ApplyDynamic(const DynamicValues& values, const GraphicsPipeline* pipeline) const;
    /// sotc: SOTC_PR_VERIFY: the prepared inputs, or null when the inline check disagrees.
    const PreparedDraw* CheckPrepared(const PreparedDraw& prepared,
                                      const GraphicsPipeline* pipeline, bool is_indexed,
                                      bool direct);
    /// sotc: SOTC_GCN_COMMIT. The prepared slot of a dispatch the draw thread may use: null unless SOTC_GCN_COMMIT is on,
    /// the slot is a valid dispatch and use_resolved (the pipeline is the one the slot was prepared for). Mode 2 compares
    /// the slot with the inline values and returns null (never used); SOTC_VERIFY=1 compares it too and returns null on
    /// a difference. Draw thread.
    const PreparedDraw* CheckPreparedDispatch(const PreparedDraw* prepared, bool use_resolved,
                                              const Pipeline* pipeline);

    bool FilterDraw();

    /// sotc: SOTC_SHADOW_REUSE: true when this draw's far sun shadow cascade is reused (moved
    /// by whole texels) instead of drawn this frame (vk_shadow_scroll.h).
    bool ReuseShadowCascade();
    std::unique_ptr<class ShadowScroll> shadow_scroll;

    /// sotc: SOTC_PR_RESOLVE: pr / pr_stage (both or neither) are the prepared entries of this
    /// stage; a descriptor they do not cover is resolved inline.
    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data, u32 set,
                     const PreparedResources* pr = nullptr, const PrStage* pr_stage = nullptr);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding, u32 set,
                      const PreparedResources* pr = nullptr, const PrStage* pr_stage = nullptr);
    /// sotc: SOTC_HEAP real mode, the heap stage of an op. Makes what the classic bind's second pass makes for the
    /// stage's images (bound flags, transitions, bound list, usage) from the index table's images, with no descriptors.
    /// Returns false, having made nothing, when the classic bind must run instead (a render target, a stencil image,
    /// a rebind pending); the reason is counted.
    bool BindHeapImages(const Shader::Info& stage, const SotcArch::TableMemo::TableBinds& binds);

    /// sotc: SOTC_TEX_BLOCK=1 (default off; 2 also runs the full bind and compares). A stage's
    /// texture block (its image descriptors and samplers) is memoized by the raw T#/S# words it
    /// fetches, its flags and the state of the images it bound. A hit replays the image infos,
    /// descriptor writes, bindings and LOD marks; it still runs UpdateImage and Transit per image.
    /// sotc: while SOTC_BIND_CACHE records, a hit also makes the cache records the full bind would.
    static constexpr u32 MaxBlockImages = 16;
    static constexpr u32 MaxBlockSamplers = Shader::NUM_SAMPLERS;
    static constexpr u32 MaxBlockEntries = 32;
    static constexpr u32 MaxBlockKeyWords = 3 + 5 * MaxBlockImages + 3 * MaxBlockSamplers;
    static constexpr size_t NumTexBlockMemos = 512;
    struct TexBlockKey {
        u32 words{};
        u32 slot{};
        u64 hash{};
        // sotc: not zeroed: MakeTextureBlockKey fills the first `words` entries, the only ones read
        // (a value-initialised array cleared 1 KB per stage on every draw).
        std::array<u64, MaxBlockKeyWords> key;
    };
    /// One image binding of a stored block (or a null one: a T# with address 0 or invalid format).
    struct TexBlockEntry {
        bool null{};
        bool redirect{};                         ///< bound is the depth image of found
        VideoCore::ImageId found{};              ///< FindImage's perfect match
        VideoCore::ImageId bound{};              ///< the image bound (found, or its depth image)
        u64 found_uid{};
        u64 bound_uid{};
        VAddr addr{};                            ///< found's guest address
        u64 addr_gen{};                          ///< ImageAddrGeneration(addr) after FindImage
        VideoCore::SubresourceRange range{};     ///< the view range the transition used
        vk::ImageView view{};                    ///< the view of bound (stable for its backing)
        const VideoCore::Image::BackingImage* backing{};
        u32 version{};                           ///< bound's backing state_version after the bind
    };
    struct TexBlockMemo {
        bool valid{};
        u32 key_words{};
        std::array<u64, MaxBlockKeyWords> key{};
        bool candidate{};                        ///< a T# that may become a mip tail (DecideRebase)
        u64 bind_generation{};                   ///< BindGeneration when stored
        u64 gpu_epoch{};                         ///< gpu_modified_epoch when stored
        u32 num_descs{};
        std::array<u8, MaxBlockImages> desc_counts{}; ///< bindings of each image descriptor
        u32 num_entries{};
        std::array<TexBlockEntry, MaxBlockEntries> entries{};
        u32 num_lods{};
        std::array<u32, MaxBlockImages> lod_banks{};
        /// sotc: SOTC_N5_BINDLESS: the heap generation the block was made at (BindlessGeneration),
        /// and per image descriptor its push slot + 1 (0: a plain descriptor) and the heap slot.
        u64 bindless_gen{};
        std::array<u8, MaxBlockImages> bindless_push{};
        std::array<VideoCore::BindlessRef, MaxBlockImages> bindless_block{};
    };
    std::unique_ptr<std::array<TexBlockMemo, NumTexBlockMemos>> tex_block_memos =
        std::make_unique<std::array<TexBlockMemo, NumTexBlockMemos>>();
    static int TexBlockMode();
    /// sotc: SOTC_N5_BINDLESS: the heap generation, 0 without the heap. A stored bind (bind cache,
    /// texture block) is replayed only while it is unchanged (BindlessImages::Generation).
    u64 BindlessGeneration() const noexcept {
        return bindless_images ? bindless_images->Generation() : 0;
    }
    bool MakeTextureBlockKey(const Shader::Info& stage, u32 set, const PreparedResources* pr,
                             const PrStage* pr_stage, TexBlockKey& key);
    static bool TexBlockMatches(const TexBlockMemo& memo, const TexBlockKey& key);
    /// mark_lods: replay the LOD marks (false in verify mode, where the full bind marks them).
    /// sotc: its bind cache records are pushed only after the last check that can return false.
    bool TryTextureBlock(const Shader::Info& stage, const TexBlockKey& key, u32 set,
                         Shader::Backend::Bindings& binding, bool mark_lods = true);
    /// The full bind (the former BindTextures body). rec: the memo slot to store the block in
    /// (null: not storable), filled as the bindings are made.
    void BindTexturesFull(const Shader::Info& stage, Shader::Backend::Bindings& binding, u32 set,
                          const PreparedResources* pr, const PrStage* pr_stage,
                          const TexBlockKey* tex_key, TexBlockMemo* rec);
    void BindSamplers(const Shader::Info& stage, Shader::Backend::Bindings& binding, u32 set,
                      const VideoCore::Image* ui_asset);
    /// sotc: the UI-linear image: the single image of a single-image stage that is drawn 1:1
    /// asset, or null (the point sampler is made bilinear for it).
    const VideoCore::Image* UiLinearAsset(const Shader::Info& stage, size_t num_bindings,
                                          VideoCore::ImageId first);
    bool BindResources(const Pipeline* pipeline, const PreparedDraw* prepared = nullptr);
    bool FastBindEligible(const Pipeline* pipeline) const;
    bool FastBindStatic(const Pipeline* pipeline, const AmdGpu::Regs& regs) const;
    u64 FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data) const;
    u64 FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data,
                     const AmdGpu::Regs& regs) const;
    void CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma);
    /// sotc: SOTC_BIND_CACHE (vk_rasterizer.cpp, see BindCacheEntry below).
    bool BindCacheEligible(const Pipeline* pipeline) const;
    bool BindCacheEligible(const Pipeline* pipeline, const AmdGpu::Regs& regs) const;
    /// sotc: SOTC_BIND_CACHE_FRAME counters: the first failing gate of BindCacheEligible (SotcStats::BindGate).
    u32 BindCacheRefusal(const Pipeline* pipeline, const AmdGpu::Regs& regs) const;
    void BindCacheSync(const Pipeline* pipeline, bool eligible);
    bool TryBindCache(const Pipeline* pipeline, u64 hash, bool& uses_dma);
    void CommitBindCache(const Pipeline* pipeline, u64 hash, bool uses_dma,
                         size_t first_bound_buffer);
    /// `site`: the SotcStats::BindUnsafeSite of an unsafe flush (counted under SOTC_FPS only).
    void BindCacheFlush(u32 reason, u32 site = 0);
    /// sotc: SOTC_BIND_CACHE_FRAME (vk_rasterizer.cpp, see BindCacheEntry below). The table form of
    /// BindCacheSync / TryBindCache / CommitBindCache / BindCacheInvalidate.
    void BindCacheSyncFrame(const Pipeline* pipeline, bool eligible);
    /// sotc: SOTC_BIND_CACHE_HEAP. heap_mask: the heap stages of this op (BindCacheEntry::heap_mask), 0 when the op
    /// resolves its heap eagerly. On a hit the held heap results are checked and replayed into bind_hold_addr / bind_hold_binds.
    bool TryBindCacheFrame(const Pipeline* pipeline, u64 hash, bool& uses_dma, u32 heap_mask);
    /// Returns the entry stored (null when the bind was not stored).
    struct BindCacheEntry;
    BindCacheEntry* CommitBindCacheFrame(const Pipeline* pipeline, u64 hash, bool uses_dma,
                                         size_t first_bound_buffer);
    /// sotc: SOTC_BIND_CACHE_PACKET=2 (PacketVerify, PacketAddrRefresh: declared after BindCacheFindFrame).
    /// sotc: SOTC_BIND_CACHE_PACKET. The heap reason a heap stage's images would take the classic bind (1 target or
    /// forced general, 2 stencil, 3 rebind pending, 4 composite), 0 when BindHeapImages binds them. Caller holds the lock.
    u32 HeapDeclineReason(const Shader::Info& stage, const SotcArch::TableMemo::TableBinds& binds);
    void BindCacheInvalidateFrame(VAddr addr, u64 size, VideoCore::ImageId image);
    void BindCacheNoteLookup(const VideoCore::TextureCache::ImageDesc& desc,
                             VideoCore::ImageId image);
    /// sotc: SOTC_BIND_CACHE_FRAME. The key of a bind by what it binds: the pipeline, the stages'
    /// identity and layout and the T#/V#/S# the stages read, not the constants around them.
    /// false: the pipeline reads resources by something the key cannot name (descriptor arrays,
    /// sharp tables, DMA); the full FastBindHash key is used then.
    bool BindResourceHash(const Pipeline* pipeline, u64& hash) const;
    /// The flat buffer of a stage as the shader must see it, with the live-setting patches (TAA,
    /// tonemap vignette, vegetation, grass) applied; the data pointer is valid until the next call.
    const u32* FlatForBind(const Shader::Info& stage);
    /// sotc: SOTC_BIND_CACHE: a sampled image of the bind being recorded and the transition
    /// BindTextures asked for it (replayed on a hit).
    void BindCacheNoteImage(VideoCore::ImageId image_id, vk::ImageLayout layout,
                            vk::PipelineStageFlags2 stages, vk::AccessFlags2 access,
                            const VideoCore::SubresourceRange& range);
    /// sotc: SOTC_BIND_CACHE_RANGES. Drops the cached binds that read guest memory in
    /// [addr, addr + size) (a bound buffer range or a sampled image overlapping it), or that sample
    /// `image`. Called for every GPU write to guest memory or images (see BindCacheSync).
    void BindCacheInvalidate(VAddr addr, u64 size, VideoCore::ImageId image = {});
    /// sotc: SOTC_BIND_CACHE_RANGES: BindCacheInvalidate for a written image and its guest range.
    void BindCacheInvalidateImage(VideoCore::ImageId image_id);
    /// sotc: SOTC_BIND_CACHE_META. A metadata clear (skipped dispatch) of [addr, addr + size): drops or stamps only the
    /// binds reading that range; flushes nothing else.
    void BindCacheNoteMetaClear(VAddr addr, u64 size);
    /// sotc: the backing of the image an image info names: the one that owns `view` (null when none does).
    const void* ImageInfoBacking(VideoCore::ImageId id, vk::ImageView view);

    /// sotc: records vertex input, vertex and index buffers again when a submit happened since
    /// vertex_tick (the draw moved to a new command buffer).
    void RebindVertexIfSubmitted(const GraphicsPipeline* pipeline, u64& vertex_tick,
                                 bool is_indexed, const std::function<void()>& bind_index,
                                 const SotcVertexExtent::Draw* extent = nullptr); // sotc: SOTC_VERTEX_EXTENT
    /// sotc: extent narrows the sizes to the draw's vertices (SOTC_VERTEX_EXTENT); nullptr keeps the V# sizes.
    void BindVertexBuffers(const GraphicsPipeline* pipeline,
                           const SotcVertexExtent::Draw* extent = nullptr);
    /// sotc: SOTC_INDEX_EXTENT. extent: the indices the draw reads from index_offset (0 = the register window).
    void BindIndexBuffer(u32 index_offset = 0, u32 extent = 0);

    void ResetBindings(bool is_compute);
    void CaptureTextures();
    static bool TaaOff(); ///< sotc: DLSS on (super resolution), vk_rasterizer.cpp
    /// sotc: the composite draws BindTexturesFull acts on, from the stage's bound images in binding order (ids, count)
    /// and the first color buffer. One predicate for two users: BindTexturesFull runs the side effects, and
    /// BindHeapImages sends such an op to the classic bind under SOTC_HEAP_COMPOSITE_OP.
    struct CompositeKinds {
        bool hudless{};  ///< hudless_ui_info is set (SOTC_FG_HUDLESS with frame generation on)
        bool dlss{};     ///< DLSS super resolution: NoteComposite and NoteUiShadow
        bool probe{};    ///< SOTC_DLSS_PROBE logs the fragment
        u32 ui_index{};  ///< which of the two images is the UI layer (for hudless and dlss)
    };
    CompositeKinds CompositeOf(const Shader::Info& stage, const VideoCore::ImageId* ids, u32 count,
                               VideoCore::ImageId cb0);
    u32 CompositeUiIndex(VideoCore::ImageId first); ///< sotc: which of the composite's two images is the UI layer
    void MaybeDumpImages(u64 pgm_hash);
    void FlushPeriodic();
    u64 commands_tick{};
    u64 commands_since_submit{};
    u64 evict_poll{}; ///< sotc: FlushPeriodic commands without periodic submits
    std::chrono::steady_clock::time_point commands_since{};

    /// clear_addr / clear_size (optional): the surface the skipped dispatch clears (sotc: SOTC_BIND_CACHE_META).
    bool IsComputeMetaClear(const Pipeline* pipeline, VAddr* clear_addr = nullptr,
                            u64* clear_size = nullptr);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    // sotc: the finest mip per texture counter bank written by fragment shaders (device local),
    // and the host copies read when the GPU reaches them (one per area in flight).
    static constexpr u32 SotcLodFeedbackBytes = 256 * sizeof(u32); ///< + the gate word
    static constexpr u32 SotcLodFeedbackAreas = 4;
    u32 sotc_lod_cycles{};
    std::unique_ptr<VideoCore::Buffer> sotc_lod_feedback;
    std::unique_ptr<VideoCore::Buffer> sotc_lod_download;
    /// sotc: PC-native GPU result copies (native_copies.h): the host-readable ring.
    std::unique_ptr<VideoCore::Buffer> sotc_result_copies;
    std::array<std::atomic<bool>, SotcLodFeedbackAreas> sotc_lod_pending{};
    u32 sotc_lod_area{};
    std::chrono::steady_clock::time_point sotc_lod_last{};
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    /// sotc: the image of each image_infos entry (null for samplers and empty slots). Ids, not
    /// pointers: FindImage may grow the image slot vector and move every image.
    std::array<VideoCore::ImageId, Shader::NUM_IMAGES> image_info_images{};
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    BdaTable bda_table; ///< sotc: SOTC_BDA, the per-stage block table (vk_bda_table.h)
    /// sotc: hands the dispatch's written binds to the GPU result copies (native_copies.h).
    void CopyGpuResults(vk::CommandBuffer cmdbuf);
    /// sotc: SOTC_DRAW_GATE: true when this draw waits for the staged upload of its buffers (draw_gate.h).
    bool DrawGateDefers(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset);
    /// sotc: SOTC_GCN_CENSUS: what this op binds (gcn_census.h). Measurement only.
    void CensusNote(const Pipeline* pipeline, u8 kind);
    /// sotc: SOTC_GPU_CHECKPOINTS: the bound resources of command `seq` for the device lost report.
    void CheckpointResources(u64 seq);
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
        /// sotc: SOTC_SHADER_ADDR. The guest address of the bound range (0: not guest memory, e.g. GDS). The mirror
        /// copy after the op (ResetBindings, SyncMirrorAfterOp) needs it.
        VAddr addr{};
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;
    /// Vertex/index buffers read by fixed-function stages of the current draw. They are recorded
    /// as reads after the draw so later writes to the same ranges wait for it.
    std::vector<BoundBuffer> fixed_function_reads;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    std::vector<u32> set_write_sets; ///< sotc: descriptor set of each set_writes entry
    Shader::PushData push_data;
    Shader::BindlessSamplerSlots bindless_slots{}; // sotc: SOTC_BINDLESS_SAMP, pushed after push_data
    /// sotc: the pipeline of this op has slot words (Pipeline::HasSlotWords): only then are the slot words filled.
    bool slot_words_op{};
    /// sotc: the slot words are filled for this op: slot_words_op, or the GPU verify of a heap pipeline (which compares
    /// them with the index table; it has no slot words to push, the heap block carries them).
    bool slot_words_fill{};
    /// sotc: SOTC_N5_BINDLESS: the heap of the sampled images (null unless the mode is on).
    std::unique_ptr<VideoCore::BindlessImages> bindless_images;

    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    bool attachment_feedback_loop{};
    /// The image side of the op's barrier (layouts and hazards of its images and render targets, Transit).
    bool needs_barrier{};
    /// sotc: the buffer side of the op's barrier (per-binding overlap checks, addressed-write hazards). Off in
    /// SOTC_BARRIER_STATIC=1, where the static rule decides the buffer side (passes.h).
    bool buffer_barrier_need{};
    /// sotc: SOTC_BARRIER_STATIC. The op's reads and writes (its shader stages write memory, Shader::Info::writes_memory),
    /// recorded by DecideOpBarrier and read by ResetBindings (BarrierStatic::NoteOp).
    bool op_static_reads{};
    bool op_static_writes{};
    /// sotc: SOTC_DEPTH_RO. The depth buffer of this draw is sampled (depth aspect) while its
    /// stencil is written: one layout serves both.
    bool DepthSampledAsAttachment(VideoCore::ImageId image_id, const VideoCore::Image& image,
                                  const VideoCore::TextureCache::ImageDesc& desc,
                                  bool compute) const;

    /// sotc: TakeHudless. BeginHudless starts the redraw of this draw (the final composite) into
    /// its HUD-less image; false when this draw is not the composite.
    bool BeginHudless(const GraphicsPipeline* pipeline, const RenderState& state);
    struct HudlessImage {
        vk::Image image;
        vk::ImageView rt_view;   ///< in the composite's attachment format
        vk::ImageView read_view; ///< in the format TakeHudless asked
        vk::Format rt_format{}, read_format{};
        struct VmaAllocation_T* allocation{};
        u32 width{}, height{};
        bool fresh{};
    };
    void DestroyHudless(HudlessImage& image);
    std::unordered_map<VAddr, HudlessImage> hudless_images; ///< by video out address
    HudlessImage hudless_clear;  ///< 1x1, all zero: the UI layer of the redraw
    s32 hudless_ui_info{-1};     ///< this draw: image_infos index of the UI texture, -1 none
    VAddr hudless_vo{};          ///< this draw: the video out image it renders to, 0 none
    vk::Format hudless_vo_format{};
    u32 hudless_vo_width{}, hudless_vo_height{};
    /// sotc: SOTC_PACKET. A shader-addressed buffer of a packet op: no descriptor, but its range is made resident
    /// by Touch in every bind (BindBuffers). A bind cache entry or a fast bind of a packet op replays these.
    struct PacketAddr {
        VAddr addr{};
        u64 size{};    ///< the clamped size BindBuffers used (0: nothing to touch)
        bool written{}; ///< never replayed: a written buffer is not eligible for the caches
        bool texel{};   ///< a formatted buffer: GcnTouch syncs it from an aliasing image, as ObtainBuffer does
    };
    /// sotc: SOTC_BIND_CACHE_PACKET. Where a PacketAddr of a recorded bind came from: the stage (GetStages index) and
    /// the buffer of its V#, the position of the PacketAddr, of its bind cache range and of its repair (kNoIndex when
    /// the bind pushed none). PacketAddrRefresh recomputes them from the op's own V#.
    static constexpr u32 kNoIndex = ~0u;
    struct PacketAddrSrc {
        u32 stage;
        u32 desc;
        u32 addr_index;
        u32 range_index;
        u32 repair_index;
    };
    /// sotc: descriptor state of the previous graphics draw (SOTC_FAST_BIND). Valid only while
    /// the same command buffer, pipeline, push data and resource generations are current.
    struct FastBindState {
        const Pipeline* pipeline{};
        u64 hash{};
        u64 buffer_generation{};
        u64 texture_generation{};
        u64 stream_generation{};
        u64 mem_epoch{}; ///< SotcStats::mem_epoch: guest memory unchanged since
        u64 tick{};
        bool uses_dma{};
        bool valid{};
        /// sotc: SOTC_FPS measurement: valid was cleared by a dispatch (the descriptor arrays it names may still be
        /// those of this bind). Counts the graphics binds that would have been fast-bind hits without it.
        bool compute_cleared{};
        /// sotc: SOTC_PACKET: the addressed ranges of that bind, touched again by each fast or wide hit.
        std::vector<PacketAddr> addr;
    };
    FastBindState fast_bind{};
    bool fast_bind_hit{};

    /// sotc: SOTC_BIND_CACHE=1 (default off). The results of the last full binds of eligible
    /// graphics draws (read-only resources), keyed by pipeline and FastBindHash (push data, user
    /// data, flat buffers, render target addresses). A draw whose key matches skips the sharp
    /// walks and the buffer/texture cache lookups: the descriptor arrays and push data are copied
    /// back, the image transitions and buffer access checks of the full bind are replayed, and
    /// the caller pushes them with Pipeline::BindResources. Every entry dies together
    /// (BindCacheFlush) when anything they rely on moves: the submit tick, guest memory
    /// (mem_epoch), resident buffers / registered images / samplers / the stream ring, the render
    /// targets, or a bind or DMA that writes GPU memory.
    /// sotc: SOTC_BIND_CACHE_RANGES=1 replaces the guest memory and GPU write flushes by
    /// BindCacheInvalidate on the written ranges (vk_rasterizer.cpp, BindCacheSync).
    static constexpr u32 BindCacheEntries = 32;
    struct BindCacheImage {
        VideoCore::ImageId id;
        vk::ImageLayout layout;
        vk::PipelineStageFlags2 stages;
        vk::AccessFlags2 access;
        VideoCore::SubresourceRange range;
        VAddr guest_lo{}, guest_hi{}; ///< SOTC_BIND_CACHE_RANGES: the image's guest memory
        u64 uid{};                    ///< SOTC_BIND_CACHE_FRAME: Image::image_uid (slots are reused)
    };
    /// sotc: SOTC_BIND_CACHE_XTICK. What a hit needs to synchronize a bound buffer range again
    /// (ObtainBuffer's arguments) and to see that the guest mapping under it is as it was.
    struct BindCacheRepair {
        VAddr addr{};
        u64 vsize{};  ///< the V#'s size, before the clamp to the mapping
        u64 mapped{}; ///< ClampRangeSize(addr, vsize) when the entry was made
        const VideoCore::Buffer* arena{};
        u32 size{};
        bool texel{};
    };
    /// sotc: SOTC_BIND_CACHE_XTICK. A sampler of the bind: touched again in a later tick (its LRU
    /// stamp keeps it from being collected, which would flush the cache) and checked to be the
    /// same object.
    struct BindCacheSampler {
        AmdGpu::Sampler sharp{};
        vk::Sampler handle{};
        u32 slot{}; ///< bindless heap slot (bindless only)
        bool is_depth{};
        bool bindless{};
    };
    /// sotc: SOTC_BIND_CACHE_FRAME. A flat buffer copy in the stream ring that a hit refreshes:
    /// the stage (index in GetStages), the buffer_infos element, the size, and what the entry
    /// last copied there (hash of the data, ring offset, ring wrap count).
    struct BindCacheFlatSlot {
        u32 stage{};
        u32 info{};
        u32 size{};
        u64 hash{};
        u64 offset{};
        u64 gen{~0ULL};
        /// sotc: tick of the copy. The ring keeps a region alive only for the tick that
        /// allocated it (StreamBuffer::AdvanceAndWatch), so a copy is reused only in that tick.
        u64 tick{~0ULL};
        /// sotc: SOTC_FLAT_ARENA. The buffer the copy lives in (the arena or the stream ring) and its device
        /// address base; arena: the copy is an arena entry (valid across ticks while the arena keeps it).
        vk::Buffer buffer{};
        vk::DeviceAddress base{};
        bool arena{};
        /// sotc: SOTC_BIND_DESC_REUSE. The buffer and offset the entry's kept descriptor sets were written with
        /// for this slot (BindPipelineResources). A hit whose copy is in another buffer writes the sets again; one at
        /// another offset of the same buffer binds them with a dynamic offset when the pipeline's flat bindings are
        /// dynamic (Pipeline::FlatDynamic), else writes them again.
        vk::Buffer desc_buffer{};
        u64 desc_offset{~0ULL};
    };
    /// sotc: SOTC_OP_COMPUTE_BIND. One written buffer binding of a bind (BindBuffers, the guest path): the ObtainBuffer
    /// call it made (range, marks, texel is never set), the note calls it made, the narrowing dispatch it was made under
    /// and the binding it answered. A hit replays these calls with its own state (ReplayOwnWrites); mode 2 compares them.
    struct BindCacheWrite {
        VAddr addr{};
        u64 size{};
        u32 mark_offset{};
        u32 mark_size{};
        u32 buf_index{};         ///< the descriptor's index in its stage (NoteMark)
        u64 pgm_hash{};          ///< the stage (the NoteHostWrite tag, NoteMark)
        u64 dispatch_groups{};   ///< t_direct_dispatch of the bind when it names this stage (else 0): the narrowing input
        u64 dispatch_threads{};
        bool note_again{};       ///< the bind ran NoteHostWrite after ObtainBuffer (kBindMarkOnce did not skip it)
        const VideoCore::Buffer* buffer{}; ///< the binding ObtainBuffer answered
        u64 offset{};
    };
    struct BindCacheEntry {
        const Pipeline* pipeline{};
        u64 hash{};
        u64 last_use{};
        bool valid{};
        bool uses_dma{};
        Shader::PushData push_data{};
        Shader::BindlessSamplerSlots bindless_slots{};
        std::vector<vk::WriteDescriptorSet> writes;
        std::vector<u32> write_sets;
        std::vector<u32> write_info; ///< index into buffer_infos (top bit) or image_infos
        std::vector<vk::DescriptorBufferInfo> buffer_infos;
        std::vector<vk::DescriptorImageInfo> image_infos;
        std::vector<VideoCore::ImageId> image_info_images;
        /// sotc: SOTC_BIND_CACHE_PACKET. Per image info, the backing that owns its view when the entry was made (null for
        /// samplers and empty slots). A hit refuses an image whose backing is another one (SetBackingSamples swaps it).
        std::vector<const void*> image_info_backings;
        std::vector<BoundBuffer> buffers;
        std::vector<BindCacheImage> images;
        std::vector<u32> lod_banks; ///< SotcLod::Mark of the full bind, repeated on a hit
        /// Guest ranges of the bound buffers: a hit needs them clean (no upload pending, e.g. a
        /// page left writable by the fault grace, whose writes do not move mem_epoch).
        std::vector<std::pair<VAddr, u64>> ranges;
        /// SOTC_BIND_CACHE_RANGES: [lo, hi) bounds `ranges` and the images' guest memory, so
        /// BindCacheInvalidate rejects most entries with one comparison.
        VAddr lo{}, hi{};
        /// SOTC_BIND_CACHE_XTICK: one per range, the sampler list, the tick of the last replay of
        /// the per-tick stamps (a full bind makes them for its own tick) and the use epoch of the
        /// last block stamping (0: due).
        std::vector<BindCacheRepair> repairs;
        std::vector<BindCacheSampler> samplers;
        /// sotc: SOTC_N5_BINDLESS: the heap slots the bind used and the generation they were in; a
        /// hit needs the generation unchanged, and touches the slots (BindlessImages::Use).
        std::vector<VideoCore::BindlessRef> bindless_refs;
        u64 bindless_gen{};
        u64 made_tick{~0ULL};
        u64 stamp_tick{~0ULL};
        u16 stamp_epoch{};
        /// sotc: SOTC_BIND_CACHE_FRAME. The key (pipeline, hash, render targets), the cache-wide
        /// flush generation, BufferCache::BindVersion and the stream ring wrap count the entry
        /// was made at, the sequence numbers of the stamp tables it was last checked at, the ranges
        /// whose image set it depends on (looked-up T# ranges, the images', texel buffers'), and
        /// the game frame it was made in.
        u64 key{};
        u64 targets_key{};
        u64 flush_gen{};
        u64 buffer_version{};
        u64 stream_gen{};
        u64 checked_tex_seq{};
        u64 checked_write_seq{};
        u64 checked_flag_seq{}; ///< sotc: SOTC_BIND_CACHE_CONTENT: GpuFlagStamps() sequence the entry was last checked at
        u64 map_epoch{};
        u64 made_frame{};
        bool uses_stream{};    ///< holds a flat buffer copy in the stream ring
        /// sotc: SOTC_PACKET. The bind was a packet op: a hit refreshes the packet's flat buffer addresses (from
        /// flat_slots, or the two words below when the entry has none) and replays `addr` (PacketAddr).
        bool packet{};
        std::array<u64, 2> packet_flat{}; ///< flatbuf_address, flatbuf_fragment_address at the commit (no flat_slots)
        std::vector<PacketAddr> addr;
        /// sotc: SOTC_BIND_CACHE_PACKET: where each `addr` came from (PacketAddrRefresh); empty without the switch.
        std::vector<PacketAddrSrc> addr_src;
        bool map_dependent{};  ///< bound a T# the guest mapping rejected (a map can change that)
        /// sotc: the map epoch at which the repairs' mappings were last found unchanged (TryBindCacheFrame's
        /// mapping check is skipped while MapEpoch still reads it: MapMemory / UnmapMemory are the only changes).
        u64 map_ok_epoch{~0ULL};
        /// Bound a mip-tail image: FindImage's tail choice depends on which overlapping images are
        /// GPU-written (VideoCore::gpu_modified_epoch), checked at the hit.
        bool tail_dependent{};
        u64 gpu_epoch{};
        /// Made under the resource identity key: the flat buffer copies and the viewport words of
        /// the push data are refreshed at every hit (flat_slots).
        bool refresh{};
        std::vector<BindCacheFlatSlot> flat_slots;
        /// sotc: SOTC_BIND_DESC_REUSE. The descriptor sets Pipeline::BindResources committed and wrote for this
        /// entry's descriptors (count 0: none kept), and the heap pool serial they belong to (vk_resource_pool.h).
        /// Persistent sets are owned by the entry: freed (DescriptorHeap::RetirePersistent) after their last tick
        /// when the entry writes new ones or its slot is made again.
        Vulkan::DescriptorSetReuse desc_sets{};
        /// sotc: SOTC_SAMPLER_CACHE. Tick of the last per-tick sampler re-resolution of a replay (~0: never).
        u64 sampler_check_tick{~0ULL};
        std::vector<std::pair<VAddr, VAddr>> tex_watch;
        /// sotc: SOTC_BIND_CACHE_HEAP. The heap resolutions of a packet op of a heap pipeline, the held result of
        /// TableMemo::Resolve: bit 0 the fragment/compute heap stage, bit 1 the vertex one. heap[k] is its index table and
        /// images (its ref names the BindSet that keeps it live), heap_stage[k] its stage's position in GetStages.
        u32 heap_mask{};
        std::array<u32, 2> heap_stage{};
        std::array<SotcArch::TableMemo::TableBinds, 2> heap{};
        /// sotc: SOTC_OP_COMPUTE_BIND. The written bindings of the bind, in the order the full bind made them; a hit
        /// replays them (ReplayOwnWrites).
        std::vector<BindCacheWrite> own_writes;
    };
    /// The state every live entry was made in; a change flushes them all.
    struct BindCacheState {
        u64 tick{~0ULL};
        u64 buffer_generation{};
        u64 texture_generation{};
        u64 stream_generation{};
        u64 mem_epoch{};
        u64 write_epoch{};
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> targets{};
        u64 targets_key{}; ///< SOTC_BIND_CACHE_FRAME: the targets and their identities, hashed
    };
    std::array<BindCacheEntry, BindCacheEntries> bind_cache{};
    /// sotc: SOTC_BIND_CACHE_FRAME: an open-addressing table instead of `bind_cache` (allocated on
    /// first use): a key hashes to a slot and is found within FrameProbe slots from it. frame_keys
    /// holds the key of each live slot (0: empty), so a lookup touches one cache line.
    static constexpr u32 BindCacheFrameEntries = 4096;
    static constexpr u32 BindCacheFrameProbe = 8;
    std::vector<BindCacheEntry> frame_cache;
    std::vector<u64> frame_keys;
    /// A cache-wide flush (BindCacheFlush) is one increment: older entries are dropped when found.
    u64 bind_cache_flush_gen{1};
    /// GPU writes to guest ranges and images (BindCacheInvalidate) and image set changes
    /// (TextureCache::ImageChangedSince) since an entry's checkpoints.
    VideoCore::RangeStamps write_stamps;
    static constexpr u32 WriteImageStamps = 4096;
    std::array<std::atomic<u64>, WriteImageStamps> write_image_stamps{};
    /// What the full bind being recorded depends on besides the cache key.
    std::vector<std::pair<VAddr, VAddr>> bind_cache_lookups_rec; ///< T# lookup ranges
    std::vector<std::pair<VAddr, VAddr>> bind_cache_texel_rec;   ///< texel buffer ranges
    std::vector<std::pair<VAddr, VAddr>> bind_cache_tex_watch_rec; ///< CommitBindCacheFrame scratch
    bool bind_cache_stream_rec{};     ///< the bind copied a flat buffer into the stream ring
    bool bind_cache_map_dep_rec{};    ///< the bind rejected a T# for its guest mapping
    u64 bind_rec_tex_seq{};           ///< ImageChangeSeq() when the bind began
    u64 bind_rec_write_seq{};         ///< write_stamps.Seq() when the bind began
    u64 bind_rec_map_epoch{};         ///< MapEpoch() when the bind began
    u64 bind_rec_gpu_epoch{};         ///< VideoCore::gpu_modified_epoch when the bind began
    /// sotc: SOTC_BIND_CACHE_CONTENT. GpuFlagStamps() sequence and the verify state: the lookup's hold (a write today's
    /// rule refuses and the content rule keeps), the entry it holds, and whether the current lookup kept a hit after a write.
    u64 bind_rec_flag_seq{};          ///< GpuFlagStamps().Seq() when the bind began
    bool bind_cache_hold{};           ///< the stamp check held the entry (mode 2): not dropped
    bool bind_cache_verify_pending{}; ///< mode 2: a full bind follows a held lookup of bind_cache_verify_index
    size_t bind_cache_verify_index{};
    bool bind_cache_kept_write{};     ///< the lookup kept an entry across a GPU write (mode 1)
    /// sotc: SOTC_BIND_CACHE_CONTENT=2. The full bind that followed a held lookup is compared with the held entry: counts
    /// the differences by category (SotcStats::BindContentDiff). Changes no state.
    void BindCacheVerify(const BindCacheEntry& old, size_t first_bound_buffer);
    bool bind_cache_tail_rec{};       ///< the bind found a mip-tail image
    std::vector<BindCacheFlatSlot> bind_cache_flat_rec; ///< flat buffer copies of the bind
    /// sotc: SOTC_BIND_DESC_REUSE. The descriptor set record of the op being bound: filled by TryBindCacheFrame from
    /// the hit's entry (reuse set when the sets are valid), handed to Pipeline::BindResources by
    /// BindPipelineResources, which stores what that call committed into desc_reuse_entry (the hit's or the newly
    /// made entry; null when the op has no frame entry).
    Vulkan::DescriptorSetReuse desc_reuse{};
    BindCacheEntry* desc_reuse_entry{};
    /// sotc: Pipeline::BindResources of the op's descriptor state (set_writes, push data, packet, heap block), with
    /// the SOTC_BIND_DESC_REUSE record when the switch is on. Every draw and dispatch path binds through it.
    void BindPipelineResources(const Pipeline* pipeline);
    u32 cur_bind_stage{};             ///< index (in GetStages) of the stage being bound
    bool bind_level2{};               ///< this bind's key is the resource identity
    Shader::PushData bind_input_push{}; ///< the draw's own push data (registers) before the bind
    /// sotc: SOTC_OP_COMPUTE_BIND. The written bindings of the bind being recorded. Their range stamps are noted after the
    /// commit (ApplyOwnWriteStamps), so the commit does not reject the entry for the bind's own writes.
    std::vector<BindCacheWrite> bind_cache_writes_rec;
    /// sotc: SOTC_OP_COMPUTE_BIND. Replays the written bindings of a hit: ObtainBuffer, the texture invalidation and the
    /// host write note each made, then their stamps (the entry is fresh again after them). 0: replayed; 1: a binding
    /// changed (the hit is refused); 2: the entry is stale (reason set).
    int ReplayOwnWrites(BindCacheEntry& entry, u32& reason);
    /// sotc: SOTC_OP_COMPUTE_BIND. The stamps of the recorded written bindings, noted after the commit of the bind
    /// (made: the entry stored by it, or null). Clears the record.
    void ApplyOwnWriteStamps(BindCacheEntry* made);
    /// sotc: SOTC_OP_COMPUTE_BIND=2. The write calls a replay of the entry would make against the ones this full bind made.
    void WriteVerify(const BindCacheEntry& ref);
    /// SOTC_FPS only: keys looked up in this game frame (distinct keys per frame).
    struct KeySeen {
        u64 key{};
        u64 frame{};
    };
    std::array<KeySeen, 16384> bind_key_seen{};
    u64 bind_key_frame{};
    /// The image an image copy / clear dispatch wrote (IsComputeImageCopy / IsComputeImageClear).
    VideoCore::ImageId compute_written_image{};
    /// sotc: SOTC_BIND_CACHE_FRAME: the memo of read-only V# resolutions (BindBuffers): the arena
    /// and offset ObtainBuffer returned for a binding, valid while BufferCache::BindVersion and the
    /// guest mapping under the binding are as they were (the checks of a cached bind's hit).
    struct BufferMemo {
        VAddr addr{};
        u64 vsize{};  ///< the V#'s size, before the clamp to the mapping
        u64 mapped{}; ///< ClampRangeSize(addr, vsize)
        u64 size{};   ///< the size ObtainBuffer was asked for
        u64 offset{};
        u64 version{};
        const VideoCore::Buffer* arena{};
        u16 stamp_epoch{};
        bool valid{};
    };
    static constexpr u32 BufferMemoEntries = 2048;
    std::vector<BufferMemo> buffer_memo;
    bool bind_memo_ok{}; ///< this bind is a read-only graphics bind (BindResources)
    /// sotc: SOTC_PACKET (ARCH-DRAW section 12). The push packet of the op being bound: BindResources fills the
    /// window words and the index table, BindBuffers the flat buffer addresses; the pipelines push it.
    SotcArch::DrawPacket draw_packet{};
    /// sotc: SOTC_HEAP. The heap block of the op being bound (debug buffer address, GPU verify's classic words), filled
    /// by BindResources after the textures are bound and pushed by the pipelines after the packet.
    SotcArch::HeapBlock heap_block{};
    /// sotc: SOTC_SHADER_ADDR. This op binds a shader-addressed buffer (no bound_buffers entry), so it touches
    /// memory: Passes::Decide must see it (BindResources resets it).
    bool shader_addr_touch{};
    /// sotc: SOTC_SHADER_ADDR ordering. Guest ranges written by buffer ops since the last recorded barrier (tagged
    /// with the runtime's FlushGeneration they belong to), the ranges this op writes (merged when its bind ends), and
    /// whether this op touches a range written before it: only then does it need the barrier. A blanket barrier per
    /// addressed op ended the render pass at every draw (35 times more render passes).
    struct GuestWrite {
        VAddr begin;
        VAddr end;
        u64 gen;
    };
    std::vector<GuestWrite> sotc_guest_writes;
    std::vector<std::pair<VAddr, VAddr>> sotc_op_writes;
    bool sotc_addr_hazard{};
    bool GuestWriteOverlap(VAddr addr, u64 size);
    void MergeOpWrites(bool barrier_before_op);
    /// sotc: SOTC_BARRIER_STATIC. The barrier decision before an op (the place of the old Passes::Decide call). Records
    /// the op's static writes for ResetBindings. reads: the op binds or reads memory; writes: a stage writes memory.
    bool DecideOpBarrier(bool reads, bool writes);
    /// sotc: SOTC_BARRIER_GUEST. The safety net's decision for the op being bound (mode 1: the decision; verify: the
    /// counted simulation of it, old_due is today's). Ranges come from bound_buffers, fixed_function_reads, guest_addr_writes.
    bool GuestNetDecide(bool old_due);
    /// sotc: SOTC_BARRIER_GUEST. The addressed writes of the op being bound (MergeOpWrites); ResetBindings marks them.
    std::vector<std::pair<VAddr, VAddr>> guest_addr_writes;
    /// sotc: SOTC_BARRIER_GUEST. Scratch lists of GuestNetDecide (kept to avoid a malloc per op).
    std::vector<std::pair<VAddr, u64>> guest_reads_buf;
    std::vector<std::pair<VAddr, u64>> guest_writes_buf;
    void NoteGuestWrite(VAddr addr, u64 size);
    /// sotc: SOTC_SHADER_ADDR, mirror coherence. Guest ranges the op being bound writes through the mirror (the
    /// shader-addressed written buffers, BindBuffers). ResetBindings copies them back into their buffers.
    struct AddressedWrite {
        VAddr addr;
        u64 size;
    };
    std::vector<AddressedWrite> sotc_addressed_writes;
    /// sotc: SOTC_PACKET. The shader-addressed buffers of the packet op being bound (BindBuffers records them, reset
    /// by BindResources). CommitFastBind and the bind cache store them; a hit replays them (PacketAddrReplay).
    std::vector<PacketAddr> packet_addr_rec;
    std::vector<PacketAddrSrc> packet_addr_src_rec; ///< sotc: SOTC_BIND_CACHE_PACKET, the sources of packet_addr_rec
    /// sotc: SOTC_PACKET. Touches the ranges of a packet op's bind again (a hit, in place of BindBuffers): Touch,
    /// the skipped-buffer counters and shader_addr_touch, as the full bind does.
    void PacketAddrReplay(const std::vector<PacketAddr>& ranges);
    /// sotc: SOTC_SHADER_ADDR, ResetBindings (after the op is recorded): the GPU writes of the op reach the mirror
    /// (bound written buffers) and the addressed writes reach their buffers (see SotcMirrorFromBuffer).
    void SotcMirrorAfterOp();
    /// Whether the entry still depends on nothing that changed since it last checked (the
    /// stamps), updating its checkpoints; `reason` names what moved when not.
    bool BindCacheEntryFresh(BindCacheEntry& entry, u32& reason);
    bool BindCacheStampsFresh(BindCacheEntry& entry, u32& reason);
    void BindCacheDropFrame(BindCacheEntry& entry, u32 reason);
    BindCacheEntry* BindCacheFindFrame(u64 key, const Pipeline* pipeline, u64 hash,
                                       u64 targets_key);
    /// sotc: SOTC_BIND_CACHE_PACKET (declared here, after BindCacheEntry: they take it by reference).
    bool PacketAddrRefresh(BindCacheEntry& entry);
    void PacketVerify(const BindCacheEntry& ref, size_t first_bound_buffer);
    BindCacheState bind_cache_state{};
    /// sotc: SOTC_BIND_CACHE_HEAP. The heap decline check of a held resolution (HeapDeclineReason), under the lock.
    u32 HeapHoldDecline(const Pipeline* pipeline, const BindCacheEntry& entry, u32 k);
    /// sotc: SOTC_BIND_CACHE_HEAP. A hit's replayed heap results (per heap stage: binds, index table, the entry it came
    /// from), and the op's fresh resolution that CommitBindCacheFrame stores (bind_rec_*).
    SotcArch::TableMemo::TableBinds bind_hold_binds[2]{};
    SotcArch::TableMemo::IndexTable bind_hold_addr[2]{};
    const BindCacheEntry* bind_hold_src[2]{};
    SotcArch::TableMemo::TableBinds bind_rec_heap[2]{};
    u32 bind_rec_heap_mask{}; ///< the op's heap stages (0: none to store)
    std::array<u32, 2> bind_rec_heap_stage{};
    u32 bind_cache_live{};
    u64 bind_cache_clock{};
    /// Bumped by GPU-side DMA writes (FillBuffer, CopyBuffer): flushes the cache.
    std::atomic<u64> bind_cache_write_epoch{0};
    bool bind_cache_recording{};   ///< a full bind that may be cached is running
    bool bind_cache_uncacheable{}; ///< ... and did something a hit could not repeat
    std::vector<BindCacheImage> bind_cache_images_rec;
    std::vector<u32> bind_cache_lod_rec;
    std::vector<std::pair<VAddr, u64>> bind_cache_ranges_rec;
    std::vector<BindCacheRepair> bind_cache_repairs_rec;     ///< SOTC_BIND_CACHE_XTICK
    std::vector<BindCacheSampler> bind_cache_samplers_rec;   ///< SOTC_BIND_CACHE_XTICK
    /// sotc: SOTC_N5_BINDLESS: the heap slots of the current bind (BindlessRef), and the heap
    /// generation the bind started at. A stored bind keeps both (BindCacheEntry).
    std::vector<VideoCore::BindlessRef> bind_bindless_rec;
    u64 bind_bindless_gen{};
    /// SOTC_BIND_CACHE_XTICK, SOTC_FPS only: keys of the last two frames that looked the cache up,
    /// direct-mapped (counter: lookups whose key was also seen in the previous frame).
    struct BindSeenSlot {
        u64 hash{};
        u64 frame_last{};
        u64 frame_prev{};
    };
    std::array<BindSeenSlot, 2048> bind_seen{};
    /// The buffer cache's side of the bind validity checks: BindVersion with
    /// SOTC_BIND_CACHE_XTICK, BindGeneration otherwise.
    u64 BufferBindGeneration() const noexcept {
        return SotcStats::kBindCacheXtick ? buffer_cache.BindVersion()
                                          : buffer_cache.BindGeneration();
    }

    /// sotc: SOTC_RS_MEMO=1 (default off). The render state of the last draw whose targets
    /// PrepareRenderState and BeginRendering looked up in full, with the registers it came from.
    /// A draw with the same target registers (and attachment part of the pipeline key), in the
    /// same submit and the render pass that draw left open, with no image registered or
    /// unregistered, no metadata clear bit set since and its targets still clean, tracked and not
    /// sampled, skips the lookups: PrepareRenderState only marks the targets, BeginRendering
    /// repeats the per-draw side effects (TextureCache::ReuseTarget, the transitions, the hudless
    /// / UI shadow tests) on the stored state. Not stored for a pass with a clear or a feedback loop.
    /// sotc: SOTC_RS_MEMO=2 also takes the state in a later submit (RsMemoBeginCheck): then the pass
    /// need not be open, every target must still be the memo's image (uid and backing), and a
    /// transition runs unless it is provably a no-op on the image's current state. =3 runs the full
    /// path on each candidate and compares (RsMemoVerify).
    static constexpr size_t RsMemoKeyWords = 384;
    /// sotc: SOTC_RS_MEMO=2: the memo's targets are slots 0..AmdGpu::NUM_COLOR_BUFFERS-1 (color) and
    /// AmdGpu::NUM_COLOR_BUFFERS (depth). A slot holds the image id the draw used, the image's uid
    /// and its backing (BackingImage lists only grow, so a backing lives as long as its image).
    struct RsMemo {
        std::array<u32, RsMemoKeyWords> key{};
        u32 key_len{};
        bool valid{};
        bool cb0_base{}; ///< color target 0 is mip 0, slice 0 (BeginUiShadow)
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{}; ///< pipeline key's
        u64 tick{};
        u64 texture_generation{};
        u64 meta_epoch{};
        RenderState state{};
        vk::ImageLayout depth_layout{};
        vk::PipelineStageFlags2 depth_stages{};
        vk::AccessFlags2 depth_access{};
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> target_id{};
        std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 1> target_uid{};
        std::array<VideoCore::Image::BackingImage*, AmdGpu::NUM_COLOR_BUFFERS + 1> target_backing{};
    };
    RsMemo rs_memo{};
    std::array<u32, RsMemoKeyWords> rs_key{}; ///< this draw's (RsMemoBuildKey)
    u32 rs_key_len{};
    bool rs_memo_prepared{}; ///< PrepareRenderState took the memo; BeginRendering checks it again
    /// sotc: SOTC_RS_MEMO=3: PrepareRenderState found a memo candidate and ran the full path; the
    /// next BeginRendering checks it (rs_verify_hit) and compares its result (RsMemoVerify).
    bool rs_memo_verify{};
    RsMemoHit rs_verify_hit{RsMemoHit::Miss};
    /// sotc: SOTC_RS_MEMO=3, cross-submit candidates: per target (8 colour, then depth), whether a
    /// replay would have skipped its transition (RsMemoTransitIsNoop) and the backing state before
    /// the full path. A skipped transition is right only if the full path leaves that state as is.
    struct RsVerifyPred {
        bool noop{};
        vk::ImageLayout layout{};
        vk::AccessFlags2 access{};
    };
    std::array<RsVerifyPred, AmdGpu::NUM_COLOR_BUFFERS + 1> rs_verify_pred{};

    /// sotc: SOTC_PARALLEL_RECORD (MapEpoch, PrefetchImagesShared, CheckPrepared).
    std::atomic<u64> map_epoch{0};
    std::mutex prep_prefetch_mutex;
    u64 pr_verify_count{};
    /// sotc: SOTC_PR_RESOLVE + SOTC_PR_VERIFY: the current draw's prepared resources differ from
    /// the inline ones (set by CheckPrepared); BindResources then resolves inline.
    bool pr_verify_off{};

    /// sotc: DLSS super resolution draws the game's UI layer at the output size: each draw into
    /// it (Dlssg::UiTarget) is drawn again into ui_shadow with its viewport and scissor scaled,
    /// and the composite hands that to the presenter (Dlssg::NoteUiShadow). BeginUiShadow
    /// starts the redraw of this draw, EndUiShadow restores the dynamic state.
    bool BeginUiShadow(const GraphicsPipeline* pipeline, const RenderState& state);
    void EndUiShadow();
    void NoteUiShadow(); ///< at the composite: hands the shadow over, starts the next frame's
    HudlessImage ui_shadow; ///< R8G8B8A8: rt_view sRGB, read_view UNORM
    u32 ui_shadow_draws{};  ///< since the last composite
    bool ui_shadow_broken{}; ///< a draw into the UI layer was not redrawn since then
    bool ui_shadow_hit{};    ///< this draw renders into the UI layer
    vk::Format ui_shadow_format{};
    u32 ui_shadow_src_width{}, ui_shadow_src_height{}; ///< this draw: the UI layer size
    u32 ui_shadow_width{}, ui_shadow_height{};         ///< this draw: the output size
    Viewports ui_shadow_viewports;
    Scissors ui_shadow_scissors;
};

} // namespace Vulkan
