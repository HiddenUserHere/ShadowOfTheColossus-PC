// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include <xxhash.h>
#include "common/path_util.h"
#include "../../../sotc/pc_ring.h" // sotc: SOTC_BPE_FIX (HostServed)
#include "video_core/buffer_cache/bpe_heap_guard.h" // sotc: SOTC_BPE_FIX
#include "video_core/renderdoc.h"
#include "video_core/sotc_arch/addr_table.h"   // sotc: SOTC_ADDR_TABLE (the guest page table of SOTC_SHADER_ADDR)
#include "video_core/sotc_arch/shader_addr.h" // sotc: SOTC_SHADER_ADDR, SOTC_MIRROR (sotc_arch.h)
#include "common/debug.h"
#include "common/elf_info.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/sotc_draw_merge.h" // sotc: SOTC_DRAW_MERGE_STATS (counters and report)
#include "video_core/sotc_vertex_extent.h" // sotc: SOTC_VERTEX_EXTENT (vertex binds to the draw's vertices)
#include "video_core/texture_cache/tsharp_slot_cache.h" // sotc: SOTC_TSLOT
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/sotc_guest_memory.h"
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_shadow_scroll.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_live.h"
#include "video_core/sotc_adaptive.h"
#include "video_core/sotc_write_extent.h"
#include "video_core/sotc_read_extent.h"
#include "video_core/sotc_shader_extent.h" // sotc: SOTC_SHADER_EXTENT
#include "../../../sotc/native/native_copies.h" // sotc: PC-native GPU result copies
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE (TracksGuestPages)
#include "video_core/sotc_generation.h" // sotc: SOTC_PR_GEN (per-64 KiB generation sums)
#include "video_core/sotc_op_phases.h" // sotc: SOTC_OP_PHASES
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_noprepass.h" // sotc: SOTC_NO_PREPASS=2 (measurement only)
#include "video_core/sotc_prepass_probe.h" // sotc: SOTC_PREPASS_PROBE (main depth address, observe only)
#include "../../../sotc/native/verify.h" // sotc: SOTC_VERIFY (SotcVerify::VerifyAll)
#include "../../../sotc/native/n1verify.h" // sotc: SOTC_VERIFY=n1
#include "video_core/renderer_vulkan/vk_frame_graph.h" // sotc: SOTC_N5_GRAPH (barrier events)
#include "video_core/sotc_arch/passes.h" // sotc: SOTC_PASSES (pass rule, ARCH-DRAW.md section 5)
#include "video_core/sotc_arch/guest_barrier.h" // sotc: SOTC_BARRIER_GUEST (guest sync points, safety net)
#include "video_core/sotc_arch/gcn_native.h" // sotc: ARCH-GCN-NATIVE (SOTC_GCN_MEM/BUF/TEX/SYNC/BARRIER)
#include "shader_recompiler/gcn_rules.h" // sotc: SOTC_GCN_TEX_SKIP_RESOLVE (TableStage)
#include "shader_recompiler/backend/spirv/spirv_emit_context.h" // sotc: SOTC_HEAP (UsesHeapImages)
#include "../../../sotc/sotc_live.h" // sotc: live switches (SotcLive)
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
// After vk_common.h (via the headers above), which sets the Vulkan header defines first.
#include <vk_mem_alloc.h>
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "../../../sotc/load_warm.h" // sotc: SOTC_LOAD_WARM (texture records)
#include "../../../sotc/region_predrain.h" // sotc: SOTC_BIG_BIND_LOG (bind context)
#include "../../../sotc/probes.h"           // sotc: SOTC_BLOCK_EXTENT
#include "../../../sotc/probe_vsize.h"      // sotc: SOTC_VSIZE_PROBE (unbounded read-only binds)
#include "video_core/sotc_checkpoints.h"    // sotc: SOTC_GPU_CHECKPOINTS
#include "../../../sotc/draw_gate.h"            // sotc: SOTC_DRAW_GATE
#include "../../../sotc/gcn_census.h"           // sotc: SOTC_GCN_CENSUS
#include "../../../sotc/guest_mem.h"            // sotc: SOTC_RB_CHANGED (backing view)

// sotc: SOTC_PR_RESOLVE. Generation of the runtime memory table (src/runtime_memory.c); odd while
// a change is in progress. Same declaration as sotc_bininfo.cpp.
extern "C" uint64_t runtime_memory_generation(void);

namespace Passes = SotcArch::Passes; // sotc: SOTC_PASSES (passes.h)
namespace BarrierStatic = SotcArch::Passes::BarrierStatic; // sotc: SOTC_BARRIER_STATIC (passes.h)

namespace Vulkan {

// sotc: SOTC_GPU_CHECKPOINTS (sotc_checkpoints.h): the ring entry of the next command (its kind, the program hashes of
// the pipeline's stages, a/b/c) and its checkpoint, recorded right before it. One load when off.
// The pipeline of the command GpuCheckpoint marked last (CheckpointResources records its handle).
static thread_local const Pipeline* t_checkpoint_pipeline = nullptr;

static u64 GpuCheckpoint(vk::CommandBuffer cmdbuf, const Pipeline* pipeline, SotcCheckpoints::Kind kind, u64 a,
                         u64 b, u64 c) {
    if (!SotcCheckpoints::On()) {
        return 0;
    }
    t_checkpoint_pipeline = pipeline;
    u64 hashes[Shader::MaxStageTypes]{};
    u16 perms[Shader::MaxStageTypes];
    std::fill(std::begin(perms), std::end(perms), u16{0xFFFF});
    if (pipeline) {
        const auto stages = pipeline->GetStages();
        const u32 first = pipeline->IsCompute() ? u32(Shader::SwStage::Compute) : 0;
        const GraphicsPipelineKey* key =
            pipeline->IsCompute() ? nullptr : &static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        for (u32 i = 0; i < stages.size() && first + i < Shader::MaxStageTypes; ++i) {
            if (stages[i]) {
                hashes[first + i] = stages[i]->pgm_hash;
                // The module's permutation: the key's stage hash is HashCombine(program hash, permutation).
                for (u32 p = 0; key && i < key->stage_hashes.size() && p < 64; ++p) {
                    if (HashCombine(stages[i]->pgm_hash, u64{p}) == key->stage_hashes[i]) {
                        perms[first + i] = static_cast<u16>(p);
                        break;
                    }
                }
            }
        }
    }
    const u64 seq = SotcCheckpoints::Note(kind, hashes, Shader::MaxStageTypes, a, b, c, perms);
    cmdbuf.setCheckpointNV(reinterpret_cast<const void*>(seq));
    return seq;
}

// sotc: SOTC_BARRIER_STATIC. True when a stage of the pipeline writes memory (Shader::Info::writes_memory, a static bit).
static bool StagesWriteMemory(const Pipeline& pipeline) {
    for (const auto* stage : pipeline.GetStages()) {
        if (stage && stage->writes_memory) {
            return true;
        }
    }
    return false;
}

// sotc: ARCH-GCN-NATIVE S5. An op whose shaders write memory: a written buffer or a storage image (the global barrier
// after the op covers both).
static bool GcnOpWrites(const Pipeline& pipeline) {
    for (const auto* stage : pipeline.GetStages()) {
        if (stage && (stage->writes_memory || stage->has_storage_images)) {
            return true;
        }
    }
    return false;
}

// sotc: ARCH-GCN-NATIVE S5. A render pass end. The pass boundary barrier is recorded inside Scheduler::EndRendering
// (only when a pass was open, and for the implicit end in BeginRendering as well).
static void GcnEndRendering(Vulkan::Scheduler& scheduler) {
    scheduler.EndRendering();
}

// sotc: ARCH-GCN-NATIVE S5. Before an op's commands (inside or outside a render pass) and after them.
static void GcnBeforeOp(Vulkan::Scheduler& scheduler) {
    if (SotcGcn::Barrier::On()) { // sotc: SOTC_GCN_BARRIER
        SotcGcn::Barrier::BeforeOp(scheduler, scheduler.IsRendering());
    }
}
static void GcnAfterOp(const Pipeline& pipeline) {
    if (SotcGcn::Barrier::On()) { // sotc: SOTC_GCN_BARRIER
        SotcGcn::Barrier::AfterOp(GcnOpWrites(pipeline));
    }
}

// sotc: ARCH-GCN-NATIVE S2. The residency call of a shader-addressed range: BindAddressed (placed bits, then Touch)
// when the buffer stage is on, else AddrTable::Touch as before. Same arguments and answer.
static bool GcnTouch(VideoCore::BufferCache& cache, Vulkan::Scheduler& scheduler, VAddr addr, u64 size,
                     bool is_written, bool is_texel) {
    if (SotcGcn::Buf::ProfileValue() != 0) { // sotc: SOTC_GCN_BUF
        return SotcGcn::Buf::BindAddressed(cache, scheduler, addr, size, is_written, is_texel);
    }
    return SotcArch::AddrTable::Touch(cache, scheduler, addr, size, is_written, is_texel);
}

// sotc: ARCH-GCN-NATIVE. Draw thread, at each submit start (see the header).
void Rasterizer::GcnSubmitStart() {
    if (SotcGcn::Mem::On() || SotcGcn::Buf::On() || SotcGcn::Tex::On() || SotcGcn::Sync::On()) {
        SotcGcn::OnSubmitStart(buffer_cache, texture_cache, scheduler);
    }
}

namespace {
// sotc: SOTC_LOAD_WARM texture record: what rebuilds a texture bind's ImageDesc (the same
// constructor BindTexturesFull uses), stored raw per area key (load_warm.cpp).
struct SotcTexBlob {
    AmdGpu::Image tsharp;
    Shader::ImageResource resource;
};
static_assert(std::is_trivially_copyable_v<SotcTexBlob>);
const bool kSotcTexBlobSizeSet = [] {
    SotcLoadWarm::SetTextureBlobSize(sizeof(SotcTexBlob));
    return true;
}();
} // namespace

// sotc: SOTC_DEDUP_STATE=0 sends the vertex inputs on every draw again (vertex_input_cache in
// DynamicState, vk_scheduler.h, is then never read or written).
static const bool kDedupState = [] {
    const char* v = std::getenv("SOTC_DEDUP_STATE");
    return !v || v[0] != '0';
}();

// sotc: SOTC_BINDLESS_SAMP: the samplers of this stage come from the global heap, not from set 0.
// The same rule as Shader::UsesBindlessSamplers, whose profile flag is the instance flag.
static bool IsBindlessStage(const Instance& instance, const Shader::Info& stage) {
    // sotc: SOTC_HEAP: a plain stage of a heap pipeline reads its samplers through descriptors (UsesBindlessSamplers).
    return instance.IsBindlessSamplersEnabled() && !stage.samplers.empty() &&
           stage.variant != Shader::StageVariantPlain &&
           stage.samplers.size() <= Shader::GetBindlessSamplerRange(stage.sw_stage).count;
}

// sotc: SOTC_N5_BINDLESS: one bindless image of a draw (sampled or storage): its heap slot, 0 (the
// null slot) when nothing is bound. The view keeps its slot while the heap still holds it in this
// layout (the descriptor of a slot fixes its layout). Otherwise it gives the old slot back (a layout
// change, or an eviction) and takes a new one. The slot is used by the current tick, so no later
// acquire in this tick takes it (BindlessImages::Use). Mode 2 compares the heap mirror with the view.
static VideoCore::BindlessRef BindlessImageIndex(VideoCore::BindlessImages& heap,
                                                 VideoCore::TextureCache& cache,
                                                 VideoCore::ImageId image_id,
                                                 const VideoCore::TextureCache::ImageDesc& desc,
                                                 u32 key, vk::ImageLayout layout) {
    if (!image_id) {
        return {key, heap.NullSlot(key), {}};
    }
    VideoCore::ImageView& view = cache.FindTexture(image_id, desc);
    const vk::ImageView handle = *view.image_view;
    u32& slot = view.bindless_slots[key];
    if (slot == Shader::NO_BINDLESS_IMAGE_SLOT || !heap.Holds(key, slot, handle, layout)) {
        heap.Release(key, slot, handle);
        slot = heap.Acquire(key, handle, layout);
    }
    heap.Use(key, slot);
    if (heap.Mode() == 2 && slot != heap.NullSlot(key)) {
        heap.Verify(key, slot, handle);
    }
    return {key, slot, handle};
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    if (SotcQueues::AsyncMoveOn()) { // sotc: SOTC_N5_ASYNC_COMPUTE=1: the async compute queue object
        scheduler.EnableAsyncCompute();
    }
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    // sotc: SOTC_MIRROR: before SetRasterizer, which replays the live GPU mappings into Mirror::OnMap.
    SotcArch::Mirror::Init(instance, scheduler);
    SotcArch::AddrTable::Init(instance, scheduler); // sotc: SOTC_ADDR_TABLE page table (no-op unless SOTC_ADDR_TABLE=1)
    SotcArch::ShaderAddr::Init(instance); // sotc: SOTC_SHADER_ADDR=2 debug buffer (no-op otherwise; refuses bad combos)
    SotcArch::TableMemo::Init(instance, scheduler); // sotc: SOTC_HEAP (index table ring; no-op when unset)
    SotcArch::Heap::Init(instance);                 // sotc: SOTC_HEAP (logs the mode; no-op when unset)
    // sotc: ARCH-GCN-NATIVE. AddrTable::Init above runs for SOTC_GCN_MEM as well (its On() decides). The globals buffer
    // needs the device; the decisions are logged once every stage has refused what it must.
    SotcGcn::Globals::Init(instance);
    SotcGcn::LogDecisions();
    memory->SetRasterizer(this);
    SotcGuestMemory::Init(instance); // sotc: SOTC_PC_MEMORY=1 (docs/sotc/MODELO-MEMORIA-PC.md)

    // sotc: SOTC_BINDLESS_SAMP: the pipelines bind the global sampler set of the texture cache.
    const auto& bindless = texture_cache.GetBindlessSamplers();
    if (bindless.Enabled()) {
        pipeline_cache.GetDescriptorHeap().SetBindlessSamplers(bindless.Layout(), bindless.Set());
    }
    // sotc: SOTC_N5_BINDLESS: the sampled images of the bindless stages come from the heap arrays
    // of the same set (vk_bindless.h). The mode is read once, the layout bindings are in sampler.cpp.
    if (bindless.Enabled() && Shader::BindlessImagesEnabled() &&
        VideoCore::BindlessImagesMode() != 0) {
        bindless_images = std::make_unique<VideoCore::BindlessImages>(
            instance, scheduler, bindless.Set(), VideoCore::BindlessImagesMode());
    }

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });

    if (Shader::LodFeedbackEnabled()) {
        sotc_lod_feedback = std::make_unique<VideoCore::Buffer>(
            instance, 0, SotcLodFeedbackBytes + sizeof(u32), VideoCore::MemoryType::DeviceLocal,
            "SotC LOD Feedback");
        runtime.FillBuffer(sotc_lod_feedback.get(), SotcLodFeedbackBytes, sizeof(u32), 1u);
        sotc_lod_download = std::make_unique<VideoCore::Buffer>(
            instance, 0, SotcLodFeedbackBytes * SotcLodFeedbackAreas,
            VideoCore::MemoryType::HostCached, "SotC LOD Feedback Download");
        runtime.FillBuffer(sotc_lod_feedback.get(), 0u, SotcLodFeedbackBytes, ~0u);
    }
    // sotc: PC-native GPU result copies (native_copies.h): the host-readable ring.
    if (SotcNativeCopies::Armed()) {
        sotc_result_copies = std::make_unique<VideoCore::Buffer>(
            instance, 0, SotcNativeCopies::kRingBytes, VideoCore::MemoryType::HostCached, "SotC GPU Result Copies");
        SotcNativeCopies::Attach(sotc_result_copies.get());
    }
}

void Rasterizer::SotcLodFeedbackCycle() {
    if (!sotc_lod_feedback) {
        return;
    }
    // Every 8 ms is plenty: the game asks for the statistics about once a frame and the banks
    // stay sampled for a while (SotcLod::Collect).
    const auto now = std::chrono::steady_clock::now();
    if (now - sotc_lod_last < std::chrono::milliseconds{8} ||
        sotc_lod_pending[sotc_lod_area].load(std::memory_order_acquire)) {
        return;
    }
    sotc_lod_last = now;
    const u32 area = sotc_lod_area;
    sotc_lod_area = (sotc_lod_area + 1) % SotcLodFeedbackAreas;
    const vk::Buffer feedback = sotc_lod_feedback->Handle();
    const auto barrier = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::BufferMemoryBarrier2 buffer_barrier = {
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .buffer = feedback,
            .offset = 0,
            .size = SotcLodFeedbackBytes + sizeof(u32),
        };
        Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &buffer_barrier,
        });
    };
    GcnEndRendering(scheduler);
    const auto cmdbuf = scheduler.CommandBuffer();
    barrier(vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = area * SotcLodFeedbackBytes,
        .size = SotcLodFeedbackBytes,
    };
    cmdbuf.copyBuffer(feedback, sotc_lod_download->Handle(), copy);
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.fillBuffer(feedback, 0, SotcLodFeedbackBytes, ~0u);
    // The next cycle measures only when this is the fourth (gate word, DefineLodFeedback).
    constexpr u32 kLodFeedbackEvery = 4u;
    cmdbuf.fillBuffer(feedback, SotcLodFeedbackBytes, sizeof(u32),
                      ++sotc_lod_cycles % kLodFeedbackEvery == 0 ? 1u : 0u);
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    const vk::MemoryBarrier2 host_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_barrier,
    });
    sotc_lod_pending[area].store(true, std::memory_order_release);
    scheduler.DeferOperation([this, area] {
        sotc_lod_download->Invalidate(area * SotcLodFeedbackBytes, SotcLodFeedbackBytes);
        SotcLod::MergeGpu(reinterpret_cast<const u32*>(sotc_lod_download->mapped_data.data() +
                                                       area * SotcLodFeedbackBytes));
        sotc_lod_pending[area].store(false, std::memory_order_release);
    });
}

Rasterizer::~Rasterizer() {
    SotcNativeCopies::Attach(nullptr); // sotc: before sotc_result_copies goes
    for (auto& [address, image] : hudless_images) {
        DestroyHudless(image);
    }
    DestroyHudless(hudless_clear);
    DestroyHudless(ui_shadow);
}

// Games may disable color buffer and enable force depth/stencil dirty and valid to do a copy from
// one depth-stencil surface to another, without a pixel shader: {depth copy, stencil copy}.
static std::pair<bool, bool> DepthStencilOverrideCopy(const auto& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        return {false, false};
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return {depth_copy, stencil_copy};
}

bool Rasterizer::IsDrawFilteredOut() const {
    const auto& regs = liverpool->DrawRegs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return true;
    }
    const auto [depth_copy, stencil_copy] = DepthStencilOverrideCopy(regs);
    return depth_copy || stencil_copy;
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->DrawRegs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const auto [depth_copy, stencil_copy] = DepthStencilOverrideCopy(regs);
    if (depth_copy || stencil_copy) {
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    const SotcOpPhases::Scope ph{SotcOpPhases::PhRenderState}; // sotc: SOTC_OP_PHASES
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the targets
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->DrawRegs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }
    rs_memo_prepared = false;
    rs_memo_verify = false;
    if (SotcStats::kRsMemo && RsMemoPrepare(pipeline)) { // sotc: SOTC_RS_MEMO
        rs_memo_prepared = true;
        return;
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->DrawCbExtent()[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->DrawDbExtent();
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

// sotc: SOTC_RS_MEMO. The memo repeats SOTC_RT_FAST's evaluation (one TestAndTouchMeta per
// target), so it is off with SOTC_RT_FAST=0.
static bool RsMemoEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_RT_FAST");
        return SotcStats::kRsMemo && (!v || v[0] != '0');
    }();
    return enabled;
}

static constexpr size_t RsMemoMaxWords =
    (sizeof(u32) * 2 + 8 + sizeof(AmdGpu::ColorControl) + sizeof(AmdGpu::ColorBufferMask) +
     AmdGpu::NUM_COLOR_BUFFERS * (sizeof(AmdGpu::ColorBuffer) + sizeof(AmdGpu::CbDbExtent)) +
     sizeof(AmdGpu::DepthControl) + sizeof(AmdGpu::DepthBuffer) + sizeof(AmdGpu::DepthView) +
     sizeof(AmdGpu::Address) + sizeof(AmdGpu::DepthRenderControl) + sizeof(u32) + sizeof(float) +
     sizeof(AmdGpu::CbDbExtent) + 3) /
    4;

u32 Rasterizer::RsMemoBuildKey(const GraphicsPipeline* pipeline) {
    // Every input of PrepareRenderState and BeginRendering besides the image state: the
    // attachment part of the pipeline key, the target registers and the extent hints.
    static_assert(Rasterizer::RsMemoKeyWords >= RsMemoMaxWords);
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->DrawRegs();
    u32 n = 0;
    const auto put = [&](const auto& v) {
        static_assert(sizeof(v) % sizeof(u32) == 0);
        std::memcpy(rs_key.data() + n, &v, sizeof(v));
        n += static_cast<u32>(sizeof(v) / sizeof(u32));
    };
    put(key.mrt_mask);
    put(u32{key.num_samples});
    put(key.color_samples);
    put(regs.color_control);
    put(regs.color_target_mask);
    const u32 num_cb =
        std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
    for (u32 cb = 0; cb < num_cb; ++cb) {
        put(regs.color_buffers[cb]);
        put(liverpool->DrawCbExtent()[cb]);
    }
    put(regs.depth_control);
    put(regs.depth_buffer);
    put(regs.depth_view);
    put(regs.depth_htile_data_base);
    put(regs.depth_render_control);
    put(regs.stencil_clear);
    put(regs.depth_clear);
    put(liverpool->DrawDbExtent());
    return n;
}

bool Rasterizer::RsMemoTargetClean(VideoCore::ImageId image_id) {
    // What lets the full path's UpdateImage / RefreshImage / TrackImage and the needs_rebind
    // re-lookup be skipped (the same test as TryBindCache's).
    const auto& image = texture_cache.GetImage(image_id);
    return image.backing && True(image.flags & VideoCore::ImageFlagBits::Registered) &&
           False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
           image.track_addr == image.info.guest_address &&
           image.track_addr_end == image.info.guest_address + image.info.guest_size &&
           !image.binding.needs_rebind;
}

bool Rasterizer::RsMemoPrepare(const GraphicsPipeline* pipeline) {
    // sotc: SOTC_RS_MEMO, under PrepareRenderState's batch lock. A miss drops the memo: the full
    // path rewrites cb_descs / db_desc, which a later hit would read.
    if (!RsMemoEnabled()) {
        return false;
    }
    rs_key_len = RsMemoBuildKey(pipeline);
    if (SotcStats::kFpsLog) {
        SotcStats::rs_memo_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    const auto miss = [&](u32 reason) {
        rs_memo.valid = false;
        if (SotcStats::kFpsLog || SotcOpPhases::kOn) { // sotc: SOTC_OP_PHASES reads the misses too
            SotcStats::rs_memo_misses[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    if (!rs_memo.valid) {
        return miss(SotcStats::RsMissNone);
    }
    const bool same_tick = rs_memo.tick == scheduler.CurrentTick();
    if (!same_tick && !SotcStats::kRsMemoCross) {
        return miss(SotcStats::RsMissTick);
    }
    if (rs_memo.texture_generation != texture_cache.BindGeneration()) {
        return miss(SotcStats::RsMissResources);
    }
    if (rs_memo.key_len != rs_key_len ||
        std::memcmp(rs_memo.key.data(), rs_key.data(), rs_key_len * sizeof(u32)) != 0) {
        return miss(SotcStats::RsMissRegs);
    }
    const u32 num_cb = rs_memo.state.num_color_attachments;
    if (same_tick) {
        if (!scheduler.IsRendering() || !(scheduler.GetRenderState() == rs_memo.state)) {
            return miss(SotcStats::RsMissPass);
        }
        for (u32 cb = 0; cb < num_cb; ++cb) {
            if (const auto id = cb_descs[cb].first; id && !RsMemoTargetClean(id)) {
                return miss(SotcStats::RsMissImage);
            }
        }
        if (const auto id = db_desc.first; id && !RsMemoTargetClean(id)) {
            return miss(SotcStats::RsMissImage);
        }
    } else {
        // sotc: SOTC_RS_MEMO=2, an earlier submit. The memo's own targets are checked (cb_descs may
        // hold another draw's), and the pass need not be open: BeginRendering checks the bindings
        // and opens it.
        if (rs_memo.meta_epoch != texture_cache.MetaEpoch()) {
            return miss(SotcStats::RsMissMeta);
        }
        for (u32 slot = 0; slot < rs_memo.target_id.size(); ++slot) {
            const auto id = rs_memo.target_id[slot];
            if (!RsMemoSameImage(slot, id)) {
                return miss(SotcStats::RsMissIdentity);
            }
            if (id && !RsMemoTargetClean(id)) {
                return miss(SotcStats::RsMissImage);
            }
        }
    }
    if (SotcStats::kRsMemoVerify) { // sotc: SOTC_RS_MEMO=3, the full path runs and is compared
        rs_memo_verify = true;
        return false;
    }
    if (!same_tick) {
        // sotc: the draw's targets are the memo's (checked above). The marks below and the
        // BeginRendering hit read cb_descs / db_desc.
        for (u32 cb = 0; cb < num_cb; ++cb) {
            cb_descs[cb].first = rs_memo.target_id[cb];
        }
        db_desc.first = rs_memo.target_id[AmdGpu::NUM_COLOR_BUFFERS];
    }
    // The targets of this draw, as the full path marks them (ResetBindings cleared them).
    for (u32 cb = 0; cb < num_cb; ++cb) {
        if (const auto id = cb_descs[cb].first) {
            bound_images.emplace_back(id);
            texture_cache.GetImage(id).binding.is_target = 1u;
        }
    }
    if (const auto id = db_desc.first) {
        bound_images.emplace_back(id);
        texture_cache.GetImage(id).binding.is_target = 1u;
    }
    return true;
}

// sotc: SOTC_RS_MEMO=2. True when Image::GetBarriers would emit no barrier and change no state for
// this transition, so a replay of an earlier submit may skip Runtime::Transit. Mirrors its
// full-resource branch (image.cpp): a partial range or per-subresource states take the other
// branch, and a write access is left to the full path (SkipSameGeneral).
static bool RsMemoTransitIsNoop(const VideoCore::Image& image, vk::ImageLayout layout,
                                vk::AccessFlags2 access, const VideoCore::SubresourceRange& range) {
    const auto& backing = *image.backing;
    if (range.base != VideoCore::SubresourceBase{} || range.extent != image.info.resources ||
        !backing.subresource_states.empty()) {
        return false;
    }
    constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                 vk::AccessFlagBits2::eShaderWrite |
                                 vk::AccessFlagBits2::eMemoryWrite;
    return backing.state.layout == layout && backing.state.access_mask == access &&
           !static_cast<bool>(backing.state.access_mask & write_flags);
}

bool Rasterizer::RsMemoSameImage(u32 slot, VideoCore::ImageId image_id) {
    // sotc: SOTC_RS_MEMO=2. The image in a memo slot is still the one the memo drew: same uid
    // (slots are reused) and same backing (its state views and sample count are the memo's).
    if (!image_id) {
        return true;
    }
    const auto& image = texture_cache.GetImage(image_id);
    return image.image_uid == rs_memo.target_uid[slot] &&
           image.backing == rs_memo.target_backing[slot];
}

Rasterizer::RsMemoHit Rasterizer::RsMemoBeginCheck() {
    // sotc: SOTC_RS_MEMO, under BeginRendering's batch lock, after the binds of this draw (they
    // may have submitted, uploaded, ended the pass, registered images or sampled a target).
    // Read-only apart from the miss counters. Same tick: the checks of mode 1, unchanged. Another
    // tick (SOTC_RS_MEMO=2): the pass need not be open, and the targets must be the memo's own.
    const auto miss = [&](u32 reason) {
        if (SotcStats::kFpsLog || SotcOpPhases::kOn) { // sotc: SOTC_OP_PHASES reads the misses too
            SotcStats::rs_memo_misses[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return RsMemoHit::Miss;
    };
    const bool same_tick = rs_memo.tick == scheduler.CurrentTick();
    if (!same_tick && !SotcStats::kRsMemoCross) {
        return miss(SotcStats::RsMissTick);
    }
    if (rs_memo.texture_generation != texture_cache.BindGeneration()) {
        return miss(SotcStats::RsMissResources);
    }
    if (rs_memo.meta_epoch != texture_cache.MetaEpoch()) {
        return miss(SotcStats::RsMissMeta);
    }
    if (same_tick &&
        (!scheduler.IsRendering() || !(scheduler.GetRenderState() == rs_memo.state))) {
        return miss(SotcStats::RsMissPass);
    }
    const u32 num_cb = rs_memo.state.num_color_attachments;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        const auto id = cb_descs[cb].first;
        if (!same_tick && (id != rs_memo.target_id[cb] || !RsMemoSameImage(cb, id))) {
            return miss(SotcStats::RsMissIdentity);
        }
        if (!id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(id);
        // A sampled target (feedback loop) or a backing of another sample count
        // (SetBackingSamples would swap it) takes the full path.
        if (!RsMemoTargetClean(id) || image.binding.is_bound ||
            image.backing->num_samples != rs_memo.color_samples[cb]) {
            return miss(SotcStats::RsMissImage);
        }
    }
    constexpr u32 depth_slot = AmdGpu::NUM_COLOR_BUFFERS;
    const auto db_id = db_desc.first;
    if (!same_tick &&
        (db_id != rs_memo.target_id[depth_slot] || !RsMemoSameImage(depth_slot, db_id))) {
        return miss(SotcStats::RsMissIdentity);
    }
    if (db_id && !RsMemoTargetClean(db_id)) {
        return miss(SotcStats::RsMissImage);
    }
    return same_tick ? RsMemoHit::Same : RsMemoHit::Cross;
}

void Rasterizer::RsMemoVerify(const RenderState& full, RsMemoHit hit) {
    // sotc: SOTC_RS_MEMO=3. The full path's result against what a hit would have replayed: the
    // stored state with the layouts the replay sets (the backings after the full path's
    // transitions), and the memo's image ids. The result of the full path is what is used.
    if (SotcStats::kFpsLog) { // sotc: candidates are counted as checks, not as hits
        SotcStats::rs_memo_verify_checks.fetch_add(1, std::memory_order_relaxed);
    }
    RenderState expect = rs_memo.state;
    bool same = true;
    const u32 num_cb = rs_memo.state.num_color_attachments;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        const auto id = cb_descs[cb].first;
        same &= id == rs_memo.target_id[cb];
        if (id) {
            expect.color_attachments[cb].image_layout =
                texture_cache.GetImage(id).backing->state.layout;
        }
    }
    constexpr u32 depth_slot = AmdGpu::NUM_COLOR_BUFFERS;
    const auto db_id = db_desc.first;
    same &= db_id == rs_memo.target_id[depth_slot];
    if (db_id) {
        expect.depth_stencil_attachment.image_layout =
            texture_cache.GetImage(db_id).backing->state.layout;
    }
    // RenderState has no padding (static_assert in vk_scheduler.h), so memcmp compares values.
    same &= std::memcmp(&expect, &full, sizeof(RenderState)) == 0;
    // A transition a cross replay would skip must be one the full path did not change.
    if (hit == RsMemoHit::Cross) {
        const auto unchanged = [&](u32 slot, VideoCore::ImageId id) {
            const auto& pred = rs_verify_pred[slot];
            if (!id || !pred.noop) {
                return true;
            }
            const auto& state = texture_cache.GetImage(id).backing->state;
            return state.layout == pred.layout && state.access_mask == pred.access;
        };
        for (u32 cb = 0; cb < num_cb; ++cb) {
            same &= unchanged(cb, cb_descs[cb].first);
        }
        same &= unchanged(depth_slot, db_desc.first);
    }
    if (!same) {
        SotcStats::rs_memo_verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Rasterizer::RsMemoBegin(RenderState& state) {
    const RsMemoHit hit = RsMemoBeginCheck();
    if (hit == RsMemoHit::Miss) {
        return false;
    }
    const bool cross = hit == RsMemoHit::Cross;
    const u32 num_cb = rs_memo.state.num_color_attachments;

    state = rs_memo.state;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        texture_cache.ReuseTarget(image_id, false);
        // sotc: a replay of an earlier submit runs the transition unless it is a provable no-op.
        const auto access = vk::AccessFlagBits2::eColorAttachmentWrite |
                            vk::AccessFlagBits2::eColorAttachmentRead;
        if (!cross || !RsMemoTransitIsNoop(*image, vk::ImageLayout::eColorAttachmentOptimal, access,
                                           desc.view_info.range)) {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             access, desc.view_info.range);
        }
        state.color_attachments[cb].image_layout = SotcArch::Layouts::Pick(image->backing->state.layout); // sotc: A1
        if (cb == 0 && image->usage.vo_surface) { // sotc: TakeHudless
            hudless_vo = image->info.guest_address;
            hudless_vo_format = state.color_formats[0];
            hudless_vo_width = image->info.size.width;
            hudless_vo_height = image->info.size.height;
        }
        if (cb == 0 && rs_memo.cb0_base && // sotc: BeginUiShadow
            Dlssg::UiTarget(image->info.guest_address, image->info.size.width,
                            image->info.size.height, ui_shadow_width, ui_shadow_height)) {
            ui_shadow_hit = true;
            ui_shadow_format = state.color_formats[0];
            ui_shadow_src_width = image->info.size.width;
            ui_shadow_src_height = image->info.size.height;
        }
    }
    if (const auto image_id = db_desc.first) {
        auto& image = texture_cache.GetImage(image_id);
        texture_cache.ReuseTarget(image_id, true);
        if (!cross || !RsMemoTransitIsNoop(image, rs_memo.depth_layout, rs_memo.depth_access,
                                           db_desc.second.view_info.range)) {
            needs_barrier |= runtime.Transit(&image, rs_memo.depth_layout, rs_memo.depth_stages,
                                             rs_memo.depth_access, db_desc.second.view_info.range);
        }
        state.depth_stencil_attachment.image_layout = SotcArch::Layouts::Pick(image.backing->state.layout); // sotc: A1
    }
    if (SotcStats::kFpsLog) {
        SotcStats::rs_memo_hits.fetch_add(1, std::memory_order_relaxed);
        if (cross) {
            SotcStats::rs_memo_cross_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (cross) {
        // sotc: the memo belongs to this submit now: its tick, and its state with the layouts the
        // replay set, so the pass the caller opens compares equal to it (same-tick checks).
        rs_memo.tick = scheduler.CurrentTick();
        rs_memo.state = state;
    }
    return true;
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

// sotc: SOTC_VERTEX_EXTENT: what BindVertexBuffers may narrow for this draw (sotc_vertex_extent.h). The offsets are the
// ones the draw is issued with; a value read from user data is not exact (GetDrawOffsets reads it at this point).
static SotcVertexExtent::Draw MakeVertexExtent(const AmdGpu::Regs& regs, const Shader::Info& info,
                                               const Shader::Gcn::FetchShaderData& fetch_shader,
                                               const PreparedDraw* prepared, bool is_indexed,
                                               u32 index_offset, bool allowed) {
    SotcVertexExtent::Draw draw{};
    if (!SotcVertexExtent::kOn || !allowed) {
        return draw;
    }
    const auto [vertex_offset, instance_offset] =
        prepared && prepared->has_offsets
            ? std::pair<u32, u32>{prepared->vertex_offset, prepared->instance_offset}
            : GetDrawOffsets(regs, info, fetch_shader);
    const bool has_fetch = !fetch_shader.Empty();
    draw.enabled = true;
    draw.indexed = is_indexed;
    draw.index_offset = index_offset;
    draw.vertex_offset = vertex_offset;
    // GetDrawOffsets takes the vertex offset from user data when the packet has none (index_offset 0).
    draw.vertex_exact = !(has_fetch && regs.index_offset == 0 && fetch_shader.vertex_offset_sgpr != -1);
    draw.instance_offset = instance_offset;
    draw.instance_exact = !(has_fetch && fetch_shader.instance_offset_sgpr != -1);
    return draw;
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->DrawRegs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->DrawCbExtent()[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    BindCacheInvalidateImage(image_id); // sotc: SOTC_BIND_CACHE_RANGES (no-op otherwise)
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// sotc: SOTC_TRACE (was SOTC_PASS_TRACE): every 300 flips, the render passes of one frame in order (depth
// target, its size, colour target 0, screen and generic scissors, CB mode, draws) on stderr.
// A diagnostic for finding which passes are the sun shadow cascades.
template <typename Regs>
static void TracePass(const Regs& regs) {
    static const bool on = SotcTrace::kOn; // sotc: SOTC_PASS_TRACE is part of SOTC_TRACE
    if (!on) {
        return;
    }
    struct Pass {
        u64 depth, color;
        u32 pitch, height, mode, slice0, slice1;
        s32 sx0, sy0, sx1, sy1, gx0, gy0, gx1, gy1;
        u32 draws;
        u64 mrt_key; // the bound colour targets (address and format of each)
        char mrts[320];
    };
    static std::vector<Pass> passes;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !passes.empty()) {
            std::fprintf(stderr, "SOTC PASSES frame %llu: %zu passes\n",
                         static_cast<unsigned long long>(frame_seen), passes.size());
            for (const auto& p : passes) {
                std::fprintf(stderr,
                             "  depth %#llx %ux%u slices %u-%u color %#llx mode %u screen %d,%d-%d,%d "
                             "generic %d,%d-%d,%d draws %u | mrt %s\n",
                             static_cast<unsigned long long>(p.depth), p.pitch, p.height, p.slice0, p.slice1,
                             static_cast<unsigned long long>(p.color), p.mode, p.sx0, p.sy0,
                             p.sx1, p.sy1, p.gx0, p.gy0, p.gx1, p.gy1, p.draws, p.mrts);
            }
        }
        passes.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    Pass p{
        .depth = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthWriteAddress() : 0,
        .color = regs.color_buffers[0].Address(),
        .pitch = regs.depth_buffer.Pitch(),
        .height = regs.depth_buffer.Height(),
        .mode = static_cast<u32>(regs.color_control.mode),
        .slice0 = regs.depth_view.slice_start,
        .slice1 = regs.depth_view.slice_max,
        .sx0 = regs.screen_scissor.top_left_x,
        .sy0 = regs.screen_scissor.top_left_y,
        .sx1 = regs.screen_scissor.bottom_right_x,
        .sy1 = regs.screen_scissor.bottom_right_y,
        .gx0 = regs.generic_scissor.top_left_x,
        .gy0 = regs.generic_scissor.top_left_y,
        .gx1 = regs.generic_scissor.bottom_right_x,
        .gy1 = regs.generic_scissor.bottom_right_y,
        .draws = 1,
        .mrt_key = 0,
        .mrts = {},
    };
    for (u32 i = 0; i < 8; ++i) {
        const auto& cb = regs.color_buffers[i];
        if (cb) {
            p.mrt_key = p.mrt_key * 0x100000001b3ull ^ cb.Address() ^
                        (u64(cb.info.format) << 56) ^ (u64(i) << 60);
        }
    }
    if (!passes.empty()) {
        auto& last = passes.back();
        if (last.depth == p.depth && last.color == p.color && last.pitch == p.pitch &&
            last.mrt_key == p.mrt_key &&
            last.height == p.height && last.mode == p.mode && last.slice0 == p.slice0 &&
            last.slice1 == p.slice1 && last.sx0 == p.sx0 &&
            last.sy0 == p.sy0 && last.sx1 == p.sx1 && last.sy1 == p.sy1 && last.gx0 == p.gx0 &&
            last.gy0 == p.gy0 && last.gx1 == p.gx1 && last.gy1 == p.gy1) {
            ++last.draws;
            return;
        }
    }
    int at = 0;
    for (u32 i = 0; i < 8 && at < int(sizeof(p.mrts)) - 64; ++i) {
        const auto& cb = regs.color_buffers[i];
        if (cb) {
            at += std::snprintf(p.mrts + at, sizeof(p.mrts) - at, "%u:%#llx d%u n%u %ux%u ", i,
                                static_cast<unsigned long long>(cb.Address()),
                                static_cast<u32>(cb.GetDataFmt()), static_cast<u32>(cb.GetNumberFmt()),
                                cb.Pitch(), cb.Height());
        }
    }
    passes.push_back(p);
}

// sotc: SOTC_PASS_STATS=1 (off by default): draws per frame into the sun shadow map, per slice (square depth targets
// of 512+ texels, as TraceShadowShaders), against all draws. Printed in the 5 s report (ReportPassStats).
namespace PassStats {
bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PASS_STATS");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}
std::atomic<u64> draws{0}, shadow[8]{}, frames_at_start{0};
std::mutex mu;
std::map<std::tuple<u64, u64, u32, u32, u32>, u64> by_target; // (depth, color 0, pitch, height, MRT count) -> draws
template <typename Regs>
void Note(const Regs& regs) {
    if (!On()) {
        return;
    }
    draws.fetch_add(1, std::memory_order_relaxed);
    {
        u32 mrts = 0;
        for (u32 i = 0; i < 8; ++i) {
            mrts += regs.color_buffers[i] ? 1 : 0;
        }
        const bool depth = regs.depth_buffer.DepthValid();
        const auto key = std::make_tuple(depth ? regs.depth_buffer.DepthWriteAddress() : u64{0},
                                         regs.color_buffers[0] ? regs.color_buffers[0].Address() : u64{0},
                                         depth ? regs.depth_buffer.Pitch() : regs.color_buffers[0].Pitch(),
                                         depth ? regs.depth_buffer.Height() : regs.color_buffers[0].Height(), mrts);
        std::lock_guard<std::mutex> lock(mu);
        ++by_target[key];
    }
    if (!regs.depth_buffer.DepthValid()) {
        return;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch == regs.depth_buffer.Height() && pitch >= 512) {
        shadow[std::min<u32>(regs.depth_view.slice_start, 7)].fetch_add(1, std::memory_order_relaxed);
    }
}
// Vertex shader hashes of the depth-only draws into the main depth target (no colour target, not square) and of the
// G-buffer draws (7 colour targets): which engine systems the two big passes hold.
std::map<std::pair<u32, u64>, u64> by_vs; // (0 depth-only, 1 G-buffer), vs hash -> draws
template <typename Regs, typename Pipe>
void NoteShader(const Regs& regs, const Pipe* pipeline) {
    if (!On() || !pipeline || !regs.depth_buffer.DepthValid() ||
        regs.depth_buffer.Pitch() == regs.depth_buffer.Height()) {
        return;
    }
    u32 mrts = 0;
    for (u32 i = 0; i < 8; ++i) {
        mrts += regs.color_buffers[i] ? 1 : 0;
    }
    if (mrts != 0 && mrts != 7) {
        return;
    }
    const u64 vs = pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash;
    std::lock_guard<std::mutex> lock(mu);
    ++by_vs[{mrts == 0 ? 0u : 1u, vs}];
}
} // namespace PassStats

void ReportPassStats(std::string& out, double seconds) {
    if (!PassStats::On()) {
        return;
    }
    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    const u64 frames = flips - PassStats::frames_at_start.exchange(flips);
    if (frames == 0) {
        return;
    }
    const double f = double(frames);
    u64 total_shadow = 0;
    char line[512];
    int at = std::snprintf(line, sizeof line, "SOTC PASS STATS: %.0f draws/frame, sun shadow by slice:",
                           double(PassStats::draws.exchange(0)) / f);
    for (u32 i = 0; i < 8; ++i) {
        const u64 n = PassStats::shadow[i].exchange(0);
        total_shadow += n;
        if (n != 0) {
            at += std::snprintf(line + at, sizeof line - at, " %u=%.0f", i, double(n) / f);
        }
    }
    std::snprintf(line + at, sizeof line - at, " (total %.0f/frame, %llu frames)", double(total_shadow) / f,
                  (unsigned long long)frames);
    out += line;
    out += '\n';
    std::vector<std::pair<u64, std::tuple<u64, u64, u32, u32, u32>>> top;
    {
        std::lock_guard<std::mutex> lock(PassStats::mu);
        for (const auto& [k, n] : PassStats::by_target) {
            top.emplace_back(n, k);
        }
        PassStats::by_target.clear();
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < top.size() && i < 14; ++i) {
        const auto& [n, k] = top[i];
        std::snprintf(line, sizeof line, "  target depth %#llx color0 %#llx %ux%u mrts %u: %.0f draws/frame",
                      (unsigned long long)std::get<0>(k), (unsigned long long)std::get<1>(k), std::get<2>(k),
                      std::get<3>(k), std::get<4>(k), double(n) / f);
        out += line;
        out += '\n';
    }
    std::vector<std::pair<u64, std::pair<u32, u64>>> vs;
    {
        std::lock_guard<std::mutex> lock(PassStats::mu);
        for (const auto& [k, n] : PassStats::by_vs) {
            vs.emplace_back(n, k);
        }
        PassStats::by_vs.clear();
    }
    std::sort(vs.begin(), vs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (u32 pass = 0; pass < 2; ++pass) {
        u32 shown = 0;
        for (const auto& [n, k] : vs) {
            if (k.first == pass && shown++ < 10) {
                std::snprintf(line, sizeof line, "  %s vs %#018llx: %.0f draws/frame", pass == 0 ? "depth-only" : "gbuffer",
                              (unsigned long long)k.second, double(n) / f);
                out += line;
                out += '\n';
            }
        }
    }
}

// sotc: SOTC_TRACE: with the pass list, the shader hashes (all stages, as the dump file
// names) of the G-buffer draws, recognised by their R16G16 float target in MRT 4.
template <typename Regs, typename Pipe>
static void TraceGbufferShaders(const Regs& regs, const Pipe* pipeline) {
    static const bool on = SotcTrace::kOn; // sotc: SOTC_PASS_TRACE is part of SOTC_TRACE
    if (!on || !pipeline) {
        return;
    }
    static std::map<std::array<u64, 6>, u32> counts;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !counts.empty()) {
            std::fprintf(stderr, "SOTC GBUFFER SHADERS frame %llu:\n",
                         static_cast<unsigned long long>(frame_seen));
            for (const auto& [key, draws] : counts) {
                std::fprintf(stderr, "  draws %4u:", draws);
                for (u64 hash : key) {
                    if (hash) {
                        std::fprintf(stderr, " %#018llx", static_cast<unsigned long long>(hash));
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }
        counts.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    const auto& cb4 = regs.color_buffers[4];
    if (!cb4 || cb4.GetDataFmt() != AmdGpu::DataFormat::Format16_16 ||
        cb4.GetNumberFmt() != AmdGpu::NumberFormat::Float) {
        return;
    }
    std::array<u64, 6> key{};
    size_t n = 0;
    for (const auto* info : pipeline->GetStages()) {
        if (info && n < key.size()) {
            key[n++] = info->pgm_hash;
        }
    }
    ++counts[key];
}

// sotc: SOTC_TRACE: the shader hashes of the draws into square depth targets of 512+ texels
// (the sun shadow map's cascades) per slice, every 300 flips.
template <typename Regs, typename Pipe>
static void TraceShadowShaders(const Regs& regs, const Pipe* pipeline) {
    static const bool on = SotcTrace::kOn; // sotc: SOTC_PASS_TRACE is part of SOTC_TRACE
    if (!on || !pipeline || !regs.depth_buffer.DepthValid()) {
        return;
    }
    static std::map<std::pair<u32, u64>, u32> counts;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !counts.empty()) {
            std::fprintf(stderr, "SOTC SHADOW SHADERS frame %llu:\n",
                         static_cast<unsigned long long>(frame_seen));
            for (const auto& [key, draws] : counts) {
                std::fprintf(stderr, "  slice %u vs %#018llx draws %u\n", key.first,
                             static_cast<unsigned long long>(key.second), draws);
            }
        }
        counts.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch != regs.depth_buffer.Height() || pitch < 512) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    ++counts[{regs.depth_view.slice_start, vs.pgm_hash}];
}

// sotc: SOTC_SHADOW_STAGGER=1: the two farthest sun shadow cascades are redrawn on alternate
// frames (slice 2 on odd flips, slice 3 on even ones). A skipped slice starts no render pass, so
// it is not cleared and keeps the previous frame's shadows. The sun shadow map is the square
// depth target that got the most draws in the previous frame (SotC: 1024x1024, 5 slices, ~6500
// draws per frame, 3041 of them in slice 3 and 2049 in slice 2, SOTC_PASS_TRACE). Far shadows
// then lag one frame behind the camera; the two slices cost half as much.
// SOTC_ADAPTIVE_LOAD never uses this skip: a skipped slice holds texels of another light matrix
// at best and, as measured (see SOTC_SHADOW_REUSE below), none at all: missing far shadows.
template <typename Regs>
static bool SkipStaggeredShadow(const Regs& regs) {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_SHADOW_STAGGER");
        return v && v[0] == '1';
    }();
    if (!on) {
        return false;
    }
    static u64 frame_seen = 0, sun_map = 0;
    static std::unordered_map<u64, u32> counts; // draws per square depth target this frame
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        u64 most_addr = 0;
        u32 most = 0;
        for (const auto& [addr, draws] : counts) {
            if (draws > most) {
                most = draws;
                most_addr = addr;
            }
        }
        sun_map = most >= 500 ? most_addr : 0;
        counts.clear();
        frame_seen = frame;
    }
    if (!regs.depth_buffer.DepthValid() ||
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal) {
        return false;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch != regs.depth_buffer.Height() || pitch < 512) {
        return false;
    }
    const u64 addr = regs.depth_buffer.DepthWriteAddress();
    ++counts[addr];
    if (addr != sun_map) {
        return false;
    }
    const u32 slice = regs.depth_view.slice_start;
    return slice == 2 ? (frame & 1) != 0 : slice == 3 ? (frame & 1) == 0 : false;
}

// sotc: SOTC_SHADOW_REUSE (default 0 = off; 1 = on, check = verify once and save the result). Off
// by default: with it, far shadows on the ground blink at 15 Hz with a still camera (the Shrine:
// a rock's shadow there on drawn frames, gone on moved ones; 0.36% of the pixels flicker per
// frame against 0.01% off). SOTC_SHADOW_STAGGER, which only skips, blinks the same, so the game
// does not keep a skipped cascade's texels until the next frame. When on: the far
// sun shadow cascades (slices 2 and 3 of the sun shadow map, the square depth target with the
// most draws in the previous frame; ~2000 draws each) are drawn on alternate frames, and on the
// other frames last frame's cascade is moved instead (vk_shadow_scroll.h). The engine snaps its
// cascades to texels: between two frames only the translation of a cascade's (orthographic)
// light matrix changes, by whole texels in x and y. Moving last frame's texels by that many
// texels, and their depths by the z change, gives the cascade a redraw would (except for things
// that moved since, whose far shadows update every other frame, and the few texels entering at
// the edge). The light matrix is the shadow view's world -> clip at +0x80 of the view
// constants, as for the camera (*(*(VS user data 0..1) + 8), vk_dlssg.cpp). Any other change
// (rotation, scale, viewport, a non-whole-texel or large move): the cascade is drawn. Unlike
// SOTC_SHADOW_STAGGER (texels of another matrix: shadows jumping as the camera moved), a reused
// cascade always matches the matrix the lighting uses.
namespace {
struct ShadowReuseState {
    static constexpr u32 kSlices = 8;
    u64 frame_seen = 0, sun_map = 0;
    std::unordered_map<u64, u32> counts; // draws per square depth target this frame
    std::array<std::array<float, 16>, kSlices> last_vp{};
    std::array<std::array<float, 3>, kSlices> last_scale{};
    std::array<bool, kSlices> have_vp{}, decided{}, skip{}, drawn_prev{}, drawn_now{};
    // The light matrix a slice was decided for this frame, and the image it was drawn into.
    std::array<std::array<float, 16>, kSlices> decided_vp{};
    std::array<VideoCore::ImageId, kSlices> last_image{};
    std::array<u64, kSlices> last_image_uid{};
    u64 prev_key = 0; // target of the previous draw (address * 8 + slice), 0: another kind
    u64 drawn = 0, scrolled = 0, refused = 0, invalid = 0, rendered_again = 0, image_changed = 0;
    s32 check_slice = -1; // verification: the slice scrolled into the scratch image
    bool check_done = false;
};
} // namespace

bool Rasterizer::ReuseShadowCascade() {
    // sotc: SOTC_SHADOW_REUSE=auto (the launcher's default): on with NVIDIA, as in 0.58/0.59, off
    // on AMD and Intel (#178). Unset: off.
    static const int env_mode = [this] {
        const char* v = std::getenv("SOTC_SHADOW_REUSE");
        if (!v) {
            return 0;
        }
        if (v[0] == 'a') {
            constexpr u32 VendorNvidia = 0x10DE;
            return instance.GetVendorID() == VendorNvidia ? 1 : 0;
        }
        return v[0] == '0' ? 0 : (v[0] == 'c' ? 2 : 1);
    }();
    // The in-game menu can switch it while playing (sotc_live.h).
    const int live = SotcLive::shadow_reuse.load(std::memory_order_relaxed);
    int mode = live >= 0 ? live : env_mode;
    // sotc: SOTC_ADAPTIVE_LOAD (sotc_adaptive.h): with the reuse off by the launcher/env (and no
    // choice in the in-game menu, which keeps priority), it is on while the governor is relieved.
    // The governor's state is latched at the first draw of a frame, so a frame never switches
    // between moving and drawing a cascade halfway (a pass begun after a moved part could clear
    // it). An explicit SOTC_SHADOW_REUSE=1/check is left as it is.
    bool adaptive = false;
    if (mode == 0 && live < 0 && SotcAdaptive::kShadows) {
        static u64 latch_frame = ~u64{0};
        static bool latch = false;
        const u64 now_frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
        if (now_frame != latch_frame) {
            latch_frame = now_frame;
            latch = SotcAdaptive::AmortiseShadows();
        }
        adaptive = latch;
        mode = latch ? 1 : 0;
    }
    if (mode == 0) {
        return false;
    }
    static ShadowReuseState s;
    const auto& regs = liverpool->DrawRegs(); // sotc: the draw thread's copy there
    const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    if (frame != s.frame_seen && SotcAdaptive::kEnabled && frame != s.frame_seen + 1) {
        // sotc: SOTC_ADAPTIVE_LOAD: frames went by untracked (the reuse was off): the counts, the
        // matrices and "drawn last frame" are from an older frame, whose texels are gone. Start
        // over: this frame only finds the shadow map, the next draws and records each cascade,
        // and only the one after can move last frame's texels.
        s.counts.clear();
        s.sun_map = 0;
        s.have_vp.fill(false);
        s.drawn_now.fill(false);
    }
    if (frame != s.frame_seen) {
        u64 most_addr = 0;
        u32 most = 0;
        for (const auto& [addr, draws] : s.counts) {
            if (draws > most) {
                most = draws;
                most_addr = addr;
            }
        }
        if ((most >= 500 ? most_addr : 0) != s.sun_map) {
            s.have_vp.fill(false); // another shadow map: nothing to reuse
        }
        s.sun_map = most >= 500 ? most_addr : 0;
        s.counts.clear();
        s.drawn_prev = s.drawn_now;
        s.drawn_now.fill(false);
        s.decided.fill(false);
        s.frame_seen = frame;
        // Every ~60 s in the log (every 300 frames with SOTC_SHADOW_REUSE_STATS).
        static const bool stats = SotcStats::kFpsLog; // sotc: SOTC_SHADOW_REUSE_STATS is part of SOTC_FPS
        if (frame % (stats ? 300 : 1800) == 0) {
            std::fprintf(stderr,
                         "SOTC SHADOW REUSE: far cascades drawn %llu, moved %llu, refused %llu, "
                         "unreadable %llu, drawn twice in a frame %llu, image changed %llu\n",
                         static_cast<unsigned long long>(s.drawn),
                         static_cast<unsigned long long>(s.scrolled),
                         static_cast<unsigned long long>(s.refused),
                         static_cast<unsigned long long>(s.invalid),
                         static_cast<unsigned long long>(s.rendered_again),
                         static_cast<unsigned long long>(s.image_changed));
            s.drawn = s.scrolled = s.refused = s.invalid = s.rendered_again = s.image_changed = 0;
        }
    }
    const bool square_depth =
        regs.depth_buffer.DepthValid() &&
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Normal &&
        regs.depth_buffer.Pitch() == regs.depth_buffer.Height() && regs.depth_buffer.Pitch() >= 512;
    const u64 addr = square_depth ? regs.depth_buffer.DepthWriteAddress() : 0;
    const u32 slice = regs.depth_view.slice_start;
    const u64 key = square_depth ? addr * 8 + (slice & 7) : 0;
    const bool new_run = key != s.prev_key; // first draw after draws into another target
    s.prev_key = key;
    if (!square_depth) {
        return false;
    }
    ++s.counts[addr];
    if (addr != s.sun_map || (slice != 2 && slice != 3)) {
        return false;
    }
    if (s.decided[slice] && !new_run) {
        return s.skip[slice];
    }

    // The cascade's light matrix (rows at +0x80 of the shadow view's constants).
    constexpr u64 kMask48 = 0xFFFFFFFFFFFFull;
    const u64 ud =
        (u64(regs.vs_program.user_data[0]) | (u64(regs.vs_program.user_data[1]) << 32)) & kMask48;
    u64 cb = 0;
    std::array<float, 16> vp{};
    bool valid = ud && memory->TryReadBacking(ud + 8, &cb, sizeof(cb)) && (cb &= kMask48) &&
                 memory->TryReadBacking(cb + 0x80, vp.data(), sizeof(vp));
    if (valid) {
        for (float f : vp) {
            valid &= std::isfinite(f);
        }
        // Orthographic (sun light): the w row is (0, 0, 0, 1).
        valid &= vp[12] == 0.0f && vp[13] == 0.0f && vp[14] == 0.0f && vp[15] == 1.0f;
    }
    const auto& viewport = regs.viewports[0];
    const std::array<float, 3> scale{
        regs.viewport_control.xscale_enable ? viewport.xscale : 1.0f,
        regs.viewport_control.yscale_enable ? viewport.yscale : 1.0f,
        regs.viewport_control.zscale_enable ? viewport.zscale : 1.0f};
    // The image the cascade goes to: a moved cascade needs last frame's texels in the same image.
    VideoCore::ImageId image_id{};
    u64 image_uid = 0;
    if (valid) {
        const auto texture_lock = texture_cache.BatchLock();
        const auto htile = regs.depth_htile_data_base.GetAddress();
        VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                regs.depth_control, htile,
                                                liverpool->DrawDbExtent());
        image_id = texture_cache.FindImage(desc);
        image_uid = texture_cache.GetImage(image_id).image_uid;
    }
    if (s.decided[slice]) {
        // Another run of draws into this slice in the same frame.
        if (valid && vp == s.decided_vp[slice]) {
            return s.skip[slice]; // the same cascade, drawn in several runs
        }
        // Drawn again with another light matrix before the flip (a second scene in one frame, as
        // a slower machine may render): drawn as the game does, and a valid source next frame.
        ++s.rendered_again;
        ++s.drawn;
        s.skip[slice] = false;
        s.decided_vp[slice] = vp;
        s.last_vp[slice] = vp;
        s.last_scale[slice] = scale;
        s.have_vp[slice] = valid;
        s.drawn_now[slice] = true;
        s.last_image[slice] = image_id;
        s.last_image_uid[slice] = image_uid;
        return false;
    }
    s.decided[slice] = true;
    s.skip[slice] = false;
    s.decided_vp[slice] = vp;

    // Verification: the scratch image scrolled last frame against the cascade then drawn.
    if (mode == 2 && s.check_slice >= 0 && !s.check_done && shadow_scroll) {
        const auto texture_lock = texture_cache.BatchLock();
        const auto htile = regs.depth_htile_data_base.GetAddress();
        VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                regs.depth_control, htile,
                                                liverpool->DrawDbExtent());
        auto& image = texture_cache.GetImage(texture_cache.FindImage(desc));
        const char* dump_dir = std::getenv("BB_DUMP_DIR");
        const auto dir = std::filesystem::path(dump_dir ? dump_dir : ".");
        shadow_scroll->SaveCheck(
            image, static_cast<u32>(s.check_slice),
            (dir / ("shadow_check_slice" + std::to_string(s.check_slice))).string());
        s.check_done = true;
    }

    bool moved = false;
    const bool phase = (frame + slice) % 2 == 1; // each far cascade drawn every other frame
    if (!valid) {
        ++s.invalid;
    } else if (s.have_vp[slice] && s.drawn_prev[slice] && phase) {
        const auto& prev = s.last_vp[slice];
        // Last frame's texels are in the image only if it is the same one (not recreated).
        const bool same_image =
            image_id == s.last_image[slice] && image_uid == s.last_image_uid[slice];
        if (!same_image) {
            ++s.image_changed;
        }
        bool same_basis = same_image && s.last_scale[slice] == scale;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                same_basis &= vp[r * 4 + c] == prev[r * 4 + c];
            }
        }
        const double fx = double(scale[0]) * (double(vp[3]) - prev[3]);
        const double fy = double(scale[1]) * (double(vp[7]) - prev[7]);
        const float dz = static_cast<float>(double(scale[2]) * (double(vp[11]) - prev[11]));
        const double rx = std::round(fx), ry = std::round(fy);
        const bool whole = std::abs(fx - rx) < 0.02 && std::abs(fy - ry) < 0.02 &&
                           std::abs(rx) <= 128 && std::abs(ry) <= 128;
        static u32 logged = 0;
        if ((!same_basis || !whole) && SotcStats::kFpsLog && logged < 20) {
            ++logged;
            std::fprintf(stderr,
                         "SOTC SHADOW REUSE refused slice %u: basis %s, move %.4f,%.4f texels, dz "
                         "%g, scale %g %g %g (was %g %g %g)\n",
                         slice, same_basis ? "same" : "changed", fx, fy, dz, scale[0], scale[1],
                         scale[2], s.last_scale[slice][0], s.last_scale[slice][1],
                         s.last_scale[slice][2]);
        }
        if (same_basis && whole) {
            if (!shadow_scroll) {
                shadow_scroll = std::make_unique<ShadowScroll>(instance, scheduler, runtime);
            }
            const auto texture_lock = texture_cache.BatchLock();
            const auto htile = regs.depth_htile_data_base.GetAddress();
            VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                    regs.depth_control, htile,
                                                    liverpool->DrawDbExtent());
            auto& image = texture_cache.GetImage(texture_cache.FindImage(desc));
            const bool check = mode == 2;
            if (check && (s.check_slice >= 0 || (rx == 0 && ry == 0))) {
                // Verification done or pending: draw as usual.
            } else if (shadow_scroll->Scroll(image, slice, static_cast<s32>(rx),
                                             static_cast<s32>(ry), dz, regs.depth_clear, check)) {
                if (check) {
                    s.check_slice = static_cast<s32>(slice);
                    std::fprintf(stderr,
                                 "SOTC SHADOW CHECK slice %u frame %llu shift %d,%d dz %g "
                                 "(scale %g %g %g, clear %g, format %d)\n",
                                 slice, static_cast<unsigned long long>(frame), int(rx), int(ry),
                                 dz, scale[0], scale[1], scale[2], regs.depth_clear,
                                 static_cast<int>(image.info.pixel_format));
                } else {
                    moved = true;
                }
            }
        }
        if (!moved && mode != 2) {
            ++s.refused;
        }
    }
    s.last_vp[slice] = vp;
    s.last_scale[slice] = scale;
    s.have_vp[slice] = valid;
    s.last_image[slice] = image_id;
    s.last_image_uid[slice] = image_uid;
    s.skip[slice] = moved;
    s.drawn_now[slice] = !moved;
    ++(moved ? s.scrolled : s.drawn);
    if (moved && adaptive && SotcStats::kFpsLog) {
        SotcStats::adaptive_amortised.fetch_add(1, std::memory_order_relaxed);
    }
    return moved;
}

// sotc: TakeHudless (vk_rasterizer.h). An image of the video out size, created once per video out
// address in the format of the composite's attachment (mutable: the presenter reads it in the
// video out format).
static bool CreateHudlessImage(const Instance& instance, vk::Format format, u32 width, u32 height,
                               vk::ImageUsageFlags usage, vk::Image& image, vk::ImageView& view,
                               VmaAllocation& allocation) {
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw{};
    if (vmaCreateImage(instance.GetAllocator(), &info, &alloc_info, &raw, &allocation, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    image = vk::Image{raw};
    auto [result, created] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (result != vk::Result::eSuccess) {
        vmaDestroyImage(instance.GetAllocator(), image, allocation);
        image = vk::Image{};
        return false;
    }
    view = created;
    return true;
}

void Rasterizer::DestroyHudless(HudlessImage& hudless) {
    const vk::Device device = instance.GetDevice();
    if (hudless.read_view) {
        device.destroyImageView(hudless.read_view);
    }
    if (hudless.rt_view) {
        device.destroyImageView(hudless.rt_view);
    }
    if (hudless.image) {
        vmaDestroyImage(instance.GetAllocator(), hudless.image, hudless.allocation);
    }
    hudless = {};
}

bool Rasterizer::BeginHudless(const GraphicsPipeline* pipeline, const RenderState& state) {
    // ponytail: one image per video out address, never resized: a video out of another size or
    // format (a resolution change) gets no HUD-less image; recreate it after a Finish if needed.
    auto& target = hudless_images[hudless_vo];
    if (!target.image) {
        if (!CreateHudlessImage(instance, hudless_vo_format, hudless_vo_width, hudless_vo_height,
                                vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eSampled,
                                target.image, target.rt_view, target.allocation)) {
            return false;
        }
        target.rt_format = hudless_vo_format;
        target.width = hudless_vo_width;
        target.height = hudless_vo_height;
    }
    if (target.rt_format != hudless_vo_format || target.width != hudless_vo_width ||
        target.height != hudless_vo_height) {
        return false;
    }
    GcnEndRendering(scheduler);
    GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER (the composite pass reads what a pending writer wrote)
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (!hudless_clear.image) {
        if (!CreateHudlessImage(instance, vk::Format::eR8G8B8A8Unorm, 1, 1,
                                vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferDst,
                                hudless_clear.image, hudless_clear.rt_view,
                                hudless_clear.allocation)) {
            return false;
        }
        const auto to_clear = vk::ImageMemoryBarrier2{
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eTransferDstOptimal), // sotc: A1
            .image = hudless_clear.image,
            .subresourceRange = color_range,
        };
        Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
        cmdbuf.clearColorImage(hudless_clear.image,
                               SotcArch::Layouts::Pick(vk::ImageLayout::eTransferDstOptimal),
                               vk::ClearColorValue{}, color_range);
        const auto to_read = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eTransferDstOptimal), // sotc: A1
            .newLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eShaderReadOnlyOptimal),
            .image = hudless_clear.image,
            .subresourceRange = color_range,
        };
        Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    }
    // The composite writes every pixel: the last frame's content (read by the presenter) can go.
    const auto to_target = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eColorAttachmentOptimal), // sotc: A1
        .image = target.image,
        .subresourceRange = color_range,
    };
    Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
    // sotc: the descriptor arrays of the last bind change here (the UI image becomes the clear
    // image): no later fast-bind reuse of them, wide or not.
    fast_bind.valid = false;
    auto& ui = image_infos[hudless_ui_info];
    ui.imageView = hudless_clear.rt_view;
    ui.imageLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eShaderReadOnlyOptimal); // sotc: A1
    pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots, &draw_packet, &heap_block);
    RenderState hudless_state = state;
    auto& attachment = hudless_state.color_attachments[0];
    attachment.image_view = target.rt_view;
    attachment.image_layout = SotcArch::Layouts::Pick(vk::ImageLayout::eColorAttachmentOptimal); // sotc: A1
    attachment.is_clear = 0;
    hudless_state.color_formats[0] = target.rt_format;
    scheduler.BeginRendering(hudless_state);
    target.fresh = true;
    static bool reported = false;
    if (!std::exchange(reported, true)) {
        std::fprintf(stderr, "SOTC FG HUD-less: final composite into %#llx (%ux%u) redrawn without UI\n",
                     static_cast<unsigned long long>(hudless_vo), target.width, target.height);
    }
    return true;
}

vk::ImageView Rasterizer::TakeHudless(VAddr address, vk::Format format) {
    const auto it = hudless_images.find(address);
    if (it == hudless_images.end() || !std::exchange(it->second.fresh, false)) {
        return {};
    }
    auto& target = it->second;
    if (!target.read_view) {
        // As the presenter's view of the video out image: alpha excluded.
        auto [result, view] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
            .image = target.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .components = {.a = vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result != vk::Result::eSuccess) {
            return {};
        }
        target.read_view = view;
        target.read_format = format;
    }
    if (target.read_format != format) {
        return {}; // ponytail: one read format per video out image (its pixel format is fixed)
    }
    GcnEndRendering(scheduler);
    const auto to_read = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eColorAttachmentOptimal), // sotc: A1
        .newLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eShaderReadOnlyOptimal),
        .image = target.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    return target.read_view;
}

bool Rasterizer::BeginUiShadow(const GraphicsPipeline* pipeline, const RenderState& state) {
    const u32 width = ui_shadow_width;
    const u32 height = ui_shadow_height;
    const float sx = static_cast<float>(width) / static_cast<float>(ui_shadow_src_width);
    const float sy = static_cast<float>(height) / static_cast<float>(ui_shadow_src_height);
    auto& dynamic = scheduler.GetDynamicState();
    ui_shadow_viewports = dynamic.viewports;
    ui_shadow_scissors = dynamic.scissors;
    Viewports viewports = dynamic.viewports;
    bool fits = true;
    for (auto& vp : viewports) {
        vp.x *= sx;
        vp.y *= sy;
        vp.width *= sx;
        vp.height *= sy;
        // Clipping off: the shader maps a 16K window, which scaled may exceed the limits.
        fits &= std::abs(vp.width) <= static_cast<float>(instance.GetMaxViewportWidth()) &&
                std::abs(vp.height) <= static_cast<float>(instance.GetMaxViewportHeight());
    }
    const bool one_sample = pipeline->GetGraphicsKey().color_samples[0] <= 1;
    if (!fits || !one_sample || attachment_feedback_loop || state.num_layers != 1) {
        static u32 reported = 0;
        if (reported++ < 5) {
            std::fprintf(stderr,
                         "SOTC UI: a draw into the UI layer cannot be redrawn (viewport fits %d, "
                         "1 sample %d, feedback loop %d, layers %u)\n",
                         fits, one_sample, attachment_feedback_loop, state.num_layers);
        }
        ui_shadow_broken = true;
        return false;
    }
    if (ui_shadow.width != width || ui_shadow.height != height) {
        if (ui_shadow.image) {
            (void)instance.GetDevice().waitIdle(); // earlier frames may still copy it
            DestroyHudless(ui_shadow);
        }
        ui_shadow_broken = true; // this frame's earlier draws are not in it
        if (!CreateHudlessImage(instance, vk::Format::eR8G8B8A8Srgb, width, height,
                                vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eTransferSrc,
                                ui_shadow.image, ui_shadow.rt_view, ui_shadow.allocation)) {
            return false;
        }
        auto [result, unorm_view] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
            .image = ui_shadow.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result != vk::Result::eSuccess) {
            DestroyHudless(ui_shadow);
            return false;
        }
        ui_shadow.read_view = unorm_view;
        ui_shadow.width = width;
        ui_shadow.height = height;
        std::fprintf(stderr, "SOTC UI: drawing the UI layer (%ux%u) at %ux%u\n",
                     ui_shadow_src_width, ui_shadow_src_height, width, height);
    }
    vk::ImageView view;
    if (ui_shadow_format == vk::Format::eR8G8B8A8Srgb) {
        view = ui_shadow.rt_view;
    } else if (ui_shadow_format == vk::Format::eR8G8B8A8Unorm) {
        view = ui_shadow.read_view;
    } else {
        ui_shadow_broken = true;
        return false;
    }
    GcnEndRendering(scheduler);
    GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER (the composite pass reads what a pending writer wrote)
    // The first draw of a frame starts it (the composite copied the last one); later ones blend
    // over the earlier ones.
    const bool first = ui_shadow_draws++ == 0;
    const auto to_target = vk::ImageMemoryBarrier2{
        .srcStageMask = first ? vk::PipelineStageFlagBits2::eAllCommands
                              : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask =
            first ? vk::AccessFlagBits2::eNone : vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask =
            vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead,
        .oldLayout = first ? vk::ImageLayout::eUndefined : SotcArch::Layouts::Pick(vk::ImageLayout::eColorAttachmentOptimal),
        .newLayout = SotcArch::Layouts::Pick(vk::ImageLayout::eColorAttachmentOptimal),
        .image = ui_shadow.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    Vulkan::FrameGraph::NoteBarrier(); // sotc: SOTC_N5_GRAPH (verify counts barrier events)
    if (to_target.newLayout != vk::ImageLayout::eGeneral ||
        (to_target.oldLayout != vk::ImageLayout::eUndefined && to_target.oldLayout != vk::ImageLayout::eGeneral)) {
        SotcArch::Layouts::NoteResidual(SotcArch::Layouts::Residual::Internal); // sotc: A1, a raw layout is named
    }
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
    RenderState shadow = state;
    auto& color = shadow.color_attachments[0];
    if (first) {
        static u32 reported = 0;
        if (reported++ < 3) {
            const vk::Viewport vp = viewports.empty() ? vk::Viewport{} : viewports[0];
            std::fprintf(stderr,
                         "SOTC UI: first UI draw: clear %u, %u attachments, depth %d, "
                         "viewport %.0f,%.0f %.0fx%.0f\n",
                         color.is_clear, state.num_color_attachments,
                         state.depth_stencil_attachment.has_depth, vp.x, vp.y, vp.width,
                         vp.height);
        }
        if (!color.is_clear) {
            color.is_clear = 1;
            color.clear_value = {};
        }
    }
    color.image_view = view;
    color.image_layout = SotcArch::Layouts::Pick(vk::ImageLayout::eColorAttachmentOptimal);
    shadow.color_formats[0] = ui_shadow_format;
    for (u32 cb = 1; cb < shadow.num_color_attachments; ++cb) {
        shadow.color_attachments[cb].image_view = vk::ImageView{}; // of the render size
        shadow.color_formats[cb] = vk::Format{};
    }
    auto& depth = shadow.depth_stencil_attachment;
    depth.image_view = vk::ImageView{};
    depth.depth_clear = false;
    depth.stencil_clear = false;
    shadow.depth_format = vk::Format{};
    shadow.stencil_format = vk::Format{};
    shadow.samples = vk::SampleCountFlagBits::e1; // the UI target is a single sample image
    shadow.width = static_cast<u16>(width);
    shadow.height = static_cast<u16>(height);
    scheduler.BeginRendering(shadow);

    Scissors scissors = dynamic.scissors;
    for (auto& sc : scissors) {
        const s64 x0 = static_cast<s64>(std::floor(sc.offset.x * sx));
        const s64 y0 = static_cast<s64>(std::floor(sc.offset.y * sy));
        const s64 x1 = static_cast<s64>(std::ceil((sc.offset.x + s64(sc.extent.width)) * sx));
        const s64 y1 = static_cast<s64>(std::ceil((sc.offset.y + s64(sc.extent.height)) * sy));
        sc.offset = vk::Offset2D{static_cast<s32>(x0), static_cast<s32>(y0)};
        sc.extent = vk::Extent2D{static_cast<u32>(std::min<s64>(x1 - x0, 0x7fffffff - x0)),
                                 static_cast<u32>(std::min<s64>(y1 - y0, 0x7fffffff - y0))};
    }
    dynamic.SetViewports(viewports);
    dynamic.SetScissors(scissors);
    dynamic.Commit(instance, scheduler.CommandBuffer());
    return true;
}

void Rasterizer::EndUiShadow() {
    auto& dynamic = scheduler.GetDynamicState();
    dynamic.SetViewports(ui_shadow_viewports);
    dynamic.SetScissors(ui_shadow_scissors);
    dynamic.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::NoteUiShadow() {
    static u32 reported = 0;
    if (ui_shadow_draws && reported++ < 5) {
        std::fprintf(stderr, "SOTC UI: composite after %u UI draws%s\n", ui_shadow_draws,
                     ui_shadow_broken ? ", not all redrawn: render size UI" : "");
    }
    const bool ok = ui_shadow_draws && !ui_shadow_broken;
    Dlssg::NoteUiShadow(ok ? ui_shadow.image : vk::Image{}, ui_shadow.width, ui_shadow.height);
    ui_shadow_draws = 0;
    ui_shadow_broken = false;
}

// sotc: SOTC_MDI_STATS=1 (off by default; a measurement for a possible multi-draw merge). The
// XPR cull emits, inside one batch, consecutive indirect draws whose only difference is the
// argument offset (+stride per section; eboot 0x1a6d20). Two consecutive indirect draws are
// "mergeable" when nothing ran between them (no direct draw or dispatch), the second one hit the
// fast bind (same pipeline, push data, targets and resources), and its arguments directly follow
// the first's in the same buffer, in the same submit. Counts the run lengths; every 5 s one line.
namespace {
// sotc: live (SotcLive): read on every draw.
SotcLive::Int kMdiStats{"SOTC_MDI_STATS", 0};
struct MdiState {
    const void* pipeline = nullptr;
    VAddr base = 0;
    u64 next_offset = 0;
    u32 stride = 0;
    u64 tick = 0;
    bool indexed = false;
    bool open = false;
    u64 run = 0;
    u64 draws = 0, hits = 0, mergeable = 0, gpu_args = 0;
    u64 buckets[7]{}; // run length 1, 2, 3-4, 5-8, 9-16, 17-64, 65+
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
MdiState g_mdi; // the draw thread only
void MdiCloseRun() {
    if (g_mdi.open && g_mdi.run) {
        const u64 r = g_mdi.run;
        const int b = r <= 1 ? 0 : r == 2 ? 1 : r <= 4 ? 2 : r <= 8 ? 3 : r <= 16 ? 4 : r <= 64 ? 5 : 6;
        ++g_mdi.buckets[b];
        g_mdi.mergeable += r - 1;
    }
    g_mdi.open = false;
    g_mdi.run = 0;
}
void MdiBreak() { // a direct draw or a dispatch ends the run
    if (kMdiStats.get()) {
        MdiCloseRun();
    }
}
void MdiNote(const void* pipeline, bool indexed, VAddr base, u64 offset, u32 stride, u32 max_count,
             VAddr count_address, bool fast_hit, bool gpu_args, u64 tick) {
    ++g_mdi.draws;
    g_mdi.hits += fast_hit ? 1 : 0;
    g_mdi.gpu_args += gpu_args ? 1 : 0;
    const bool single = max_count == 1 && count_address == 0;
    const bool joins = g_mdi.open && single && fast_hit && pipeline == g_mdi.pipeline &&
                       indexed == g_mdi.indexed && base == g_mdi.base && stride == g_mdi.stride &&
                       offset == g_mdi.next_offset && tick == g_mdi.tick;
    if (joins) {
        ++g_mdi.run;
    } else {
        MdiCloseRun();
        if (single) {
            g_mdi.open = true;
            g_mdi.run = 1;
        }
    }
    g_mdi.pipeline = pipeline;
    g_mdi.indexed = indexed;
    g_mdi.base = base;
    g_mdi.stride = stride;
    g_mdi.next_offset = offset + stride;
    g_mdi.tick = tick;
    if ((g_mdi.draws & 1023) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_mdi.last >= std::chrono::seconds(5)) {
            g_mdi.last = now;
            const auto& k = g_mdi.buckets;
            std::fprintf(stderr,
                         "SOTC MDI: indirect %llu (GPU args %llu), fast-bind hits %llu, mergeable "
                         "%llu (%.1f%%); runs: 1 %llu, 2 %llu, 3-4 %llu, 5-8 %llu, 9-16 %llu, "
                         "17-64 %llu, 65+ %llu\n",
                         (unsigned long long)g_mdi.draws, (unsigned long long)g_mdi.gpu_args,
                         (unsigned long long)g_mdi.hits, (unsigned long long)g_mdi.mergeable,
                         g_mdi.draws ? 100.0 * double(g_mdi.mergeable) / double(g_mdi.draws) : 0.0,
                         (unsigned long long)k[0], (unsigned long long)k[1],
                         (unsigned long long)k[2], (unsigned long long)k[3],
                         (unsigned long long)k[4], (unsigned long long)k[5],
                         (unsigned long long)k[6]);
            const auto last = g_mdi.last;
            g_mdi = MdiState{};
            g_mdi.last = last;
        }
    }
}

// sotc: SOTC_CS_STATS=1 (off by default; a measurement for a possible compute fast bind). Each
// dispatch's bind state is keyed by the pipeline, the resource sharps it binds and its user data
// and flat buffer. Counts how many dispatches repeat the previous dispatch's key ("same"), and of
// those how many also saw no buffer/texture/memory change in between in the same command buffer
// ("clean": what a fast bind could skip), or repeat any key bound earlier in the same command
// buffer ("seen"); and how much time BindResources takes for each group. One line every 5 s.
// sotc: live (SotcLive): read on every compute dispatch.
SotcLive::Int kCsStats{"SOTC_CS_STATS", 0};
struct CsStatsState {
    u64 prev_key = 0, prev_gen = 0, tick = ~u64{0};
    bool prev_ok = false;
    std::unordered_set<u64> seen;
    u64 dispatches = 0, keyed = 0, writing = 0, same = 0, clean = 0, seen_hits = 0;
    u64 ns_all = 0, ns_same = 0, ns_clean = 0, ns_seen = 0;
    // The same with the resources-only key (BindResourceHash: the sharps, no constants).
    u64 res_prev = 0, res_prev_gen = 0;
    bool res_prev_ok = false;
    std::unordered_set<u64> res_seen;
    u64 res_keyed = 0, res_same = 0, res_clean = 0, res_seen_hits = 0;
    u64 ns_res_same = 0, ns_res_clean = 0, ns_res_seen = 0;
    // The descriptors the bind produced (buffer_infos / image_infos): identical to the previous
    // dispatch's, or the same buffers, ranges and images with only the buffer offsets moved.
    u64 desc_prev = 0, handle_prev = 0;
    bool desc_prev_ok = false;
    u64 desc_keyed = 0, desc_same = 0, offsets_only = 0;
    u64 ns_desc_same = 0, ns_offsets_only = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
CsStatsState g_cs; // the draw thread only

/// The key of a compute bind; false when the pipeline binds by constants (BindResourceHash).
bool CsStatsKey(const Vulkan::Pipeline* pipeline, u64& key, bool& writes) {
    writes = false;
    u64 hash = XXH3_64bits(&pipeline, sizeof(pipeline));
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->uses_dma || !stage->sharp_tables.empty() || stage->UserData().data() == nullptr) {
            return false;
        }
        writes |= stage->has_storage_images;
        for (const auto& desc : stage->buffers) {
            writes |= !desc.IsSpecial() && desc.is_written;
        }
        for (const auto& desc : stage->images) {
            writes |= desc.is_written;
        }
        const auto user_data = stage->UserData();
        hash = XXH3_64bits_withSeed(user_data.data(), user_data.size_bytes(), hash);
        const auto& flat_buf = stage->FlatBuf();
        if (!flat_buf.empty()) {
            hash = XXH3_64bits_withSeed(flat_buf.data(), flat_buf.size() * sizeof(u32), hash);
        }
    }
    key = hash;
    return true;
}

/// The descriptors of a finished bind: with the buffer offsets (desc) and without them (handle).
template <typename BufInfos, typename ImgInfos>
void CsDescKeys(const Vulkan::Pipeline* pipeline, const BufInfos& bufs, const ImgInfos& imgs,
                u64& desc, u64& handle) {
    u64 d = XXH3_64bits(&pipeline, sizeof(pipeline));
    u64 h = d;
    for (const auto& b : bufs) {
        const VkBuffer vb = b.buffer;
        const u64 off = b.offset, range = b.range;
        h = XXH3_64bits_withSeed(&vb, sizeof(vb), h);
        h = XXH3_64bits_withSeed(&range, sizeof(range), h);
        d = XXH3_64bits_withSeed(&vb, sizeof(vb), d);
        d = XXH3_64bits_withSeed(&range, sizeof(range), d);
        d = XXH3_64bits_withSeed(&off, sizeof(off), d);
    }
    for (const auto& i : imgs) {
        const VkImageView v = i.imageView;
        const VkSampler smp = i.sampler;
        const u32 layout = static_cast<u32>(i.imageLayout);
        for (u64* k : {&d, &h}) {
            *k = XXH3_64bits_withSeed(&v, sizeof(v), *k);
            *k = XXH3_64bits_withSeed(&smp, sizeof(smp), *k);
            *k = XXH3_64bits_withSeed(&layout, sizeof(layout), *k);
        }
    }
    desc = d;
    handle = h;
}

void CsStatsNote(bool ok, u64 key, bool res_ok, u64 res_key, bool writes, u64 gen, u64 tick,
                 u64 ns, bool desc_ok, u64 desc_key, u64 handle_key) {
    auto& s = g_cs;
    if (tick != s.tick) {
        s.tick = tick;
        s.seen.clear();
        s.prev_ok = false;
        s.res_seen.clear();
        s.res_prev_ok = false;
        s.desc_prev_ok = false;
    }
    if (desc_ok) {
        ++s.desc_keyed;
        if (s.desc_prev_ok && handle_key == s.handle_prev) {
            if (desc_key == s.desc_prev) {
                ++s.desc_same;
                s.ns_desc_same += ns;
            } else {
                ++s.offsets_only;
                s.ns_offsets_only += ns;
            }
        }
        s.desc_prev = desc_key;
        s.handle_prev = handle_key;
    }
    s.desc_prev_ok = desc_ok;
    if (res_ok) {
        ++s.res_keyed;
        if (s.res_prev_ok && res_key == s.res_prev) {
            ++s.res_same;
            s.ns_res_same += ns;
            if (gen == s.res_prev_gen) {
                ++s.res_clean;
                s.ns_res_clean += ns;
            }
        }
        if (!s.res_seen.insert(res_key).second) {
            ++s.res_seen_hits;
            s.ns_res_seen += ns;
        }
        s.res_prev = res_key;
        s.res_prev_gen = gen;
    }
    s.res_prev_ok = res_ok;
    ++s.dispatches;
    s.ns_all += ns;
    s.writing += writes ? 1 : 0;
    if (ok) {
        ++s.keyed;
        if (s.prev_ok && key == s.prev_key) {
            ++s.same;
            s.ns_same += ns;
            if (gen == s.prev_gen) {
                ++s.clean;
                s.ns_clean += ns;
            }
        }
        if (!s.seen.insert(key).second) {
            ++s.seen_hits;
            s.ns_seen += ns;
        }
        s.prev_key = key;
        s.prev_gen = gen;
    }
    s.prev_ok = ok;
    if ((s.dispatches & 255) == 0) {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - s.last).count();
        if (secs >= 5.0) {
            const auto ms = [&](u64 v) { return double(v) / 1e6 / secs; };
            std::fprintf(stderr,
                         "SOTC CS STATS: dispatches %llu (keyed %llu, writing %llu), same as "
                         "previous %llu (clean %llu), seen in this submit %llu; bind ms/s: all "
                         "%.1f, same %.1f, clean %.1f, seen %.1f\n",
                         (unsigned long long)s.dispatches, (unsigned long long)s.keyed,
                         (unsigned long long)s.writing, (unsigned long long)s.same,
                         (unsigned long long)s.clean, (unsigned long long)s.seen_hits,
                         ms(s.ns_all), ms(s.ns_same), ms(s.ns_clean), ms(s.ns_seen));
            std::fprintf(stderr,
                         "SOTC CS STATS resources only: keyed %llu, same as previous %llu "
                         "(clean %llu), seen in this submit %llu; bind ms/s: same %.1f, clean "
                         "%.1f, seen %.1f\n",
                         (unsigned long long)s.res_keyed, (unsigned long long)s.res_same,
                         (unsigned long long)s.res_clean, (unsigned long long)s.res_seen_hits,
                         ms(s.ns_res_same), ms(s.ns_res_clean), ms(s.ns_res_seen));
            std::fprintf(stderr,
                         "SOTC CS STATS descriptors: bound %llu, identical to previous %llu, "
                         "same buffers with other offsets %llu; bind ms/s: identical %.1f, "
                         "offsets only %.1f\n",
                         (unsigned long long)s.desc_keyed, (unsigned long long)s.desc_same,
                         (unsigned long long)s.offsets_only, ms(s.ns_desc_same),
                         ms(s.ns_offsets_only));
            const auto tick_keep = s.tick;
            s = CsStatsState{};
            s.last = now;
            s.tick = tick_keep;
        }
    }
}
} // namespace

// sotc: SOTC_OPSET_STATS=1 (off by default; a measurement for a possible per-op record, the OpSet). Every op (Draw,
// DrawIndirect, DispatchDirect, DispatchIndirect) gets two keys from the words its stages and attachments read:
//   key B: the pipeline, then for every stage its user data and its flat buffer (user data, sharp tables and SRT walker
//          output, as RefreshFlatBuf wrote them). The draw packet is not in it: BindResources fills it, and its inputs
//          are the flat buffers (the packet's own flat buffer addresses are ring addresses).
//   key A: key B with the attachment registers (the color buffers and the depth buffer of the draw regs; 0 for a dispatch).
// Both keys go to a bounded table (2-way, 64K entries per key kind, each entry with the flip of its last use). A hit is a
// key seen in an earlier frame or earlier in this frame. The report prints the counts of one window. Draw thread writes;
// the report thread reads under the same mutex.
namespace OpSetStats {
bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_OPSET_STATS");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

constexpr std::size_t kTableSize = std::size_t{1} << 16; // entries per table; the partner of index i is i ^ 1
struct Entry {
    u64 key = 0;
    u64 stamp = 0; // flip_count + 1 of the last use; 0 when the entry is empty
};
struct Last { // the previous op of one pipeline
    u64 user = 0, flat = 0, target = 0, key_a = 0;
};
struct State {
    std::mutex mu;
    std::vector<Entry> table_a, table_b, table_c1, table_c2, table_d;
    std::unordered_map<const void*, Last> last;
    std::unordered_set<u64> win_a, win_b; // distinct keys of the window
    u64 ops = 0, hit_a = 0, hit_prev_a = 0, same_hit = 0, hit_b = 0;
    u64 miss_pipe = 0, miss_user = 0, miss_sharp = 0, miss_target = 0, miss_evicted = 0;
    u64 no_write = 0;
    u64 hit_c1 = 0, hit_c2 = 0; // keys without the base words of shader-addressed V#s (C2: also without user data)
    u64 hit_d = 0;              // key of the resource descriptors only (T#, S#, V# without addressed bases)
};

// Only what binding depends on: each stage's decoded T#s, S#s and V#s (the base of a shader-addressed V# zeroed).
// Constants the shader reads from the flat buffer are left out: they are data, not bindings.
u64 DescriptorHash(const Pipeline* pipeline) {
    u64 h = XXH3_64bits(&pipeline, sizeof(pipeline));
    const auto& profile = pipeline->GetProfile();
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (u32 i = 0; i < stage->buffers.size(); ++i) {
            const auto& desc = stage->buffers[i];
            if (desc.IsSpecial()) {
                continue;
            }
            AmdGpu::Buffer vsharp = desc.GetSharp(*stage);
            if (Shader::ShaderAddrBufferAt(profile, *stage, i)) {
                vsharp.base_address = 0;
            }
            h = XXH3_64bits_withSeed(&vsharp, sizeof(vsharp), h);
        }
        for (const auto& desc : stage->images) {
            const AmdGpu::Image tsharp = desc.GetSharp(*stage);
            h = XXH3_64bits_withSeed(&tsharp, sizeof(tsharp), h);
        }
        for (const auto& desc : stage->samplers) {
            const AmdGpu::Sampler ssharp = desc.GetSharp(*stage);
            h = XXH3_64bits_withSeed(&ssharp, sizeof(ssharp), h);
        }
    }
    return h;
}

// The flat buffer with the base address words (dw0, low byte of dw1) of every shader-addressed buffer's V# zeroed:
// the shader reads those itself through the page table, so the host does not need them in an op record's key.
// skip_user also drops the user-data prefix (the bound: every user-data word taken as a pointer).
u64 MaskedFlatHash(const Pipeline* pipeline, bool skip_user) {
    thread_local std::vector<u32> words;
    u64 h = 0;
    const auto& profile = pipeline->GetProfile();
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        const auto& flat = stage->FlatBuf();
        words.assign(flat.begin(), flat.end());
        for (u32 i = 0; i < stage->buffers.size(); ++i) {
            const auto& desc = stage->buffers[i];
            if (desc.IsSpecial() || !Shader::ShaderAddrBufferAt(profile, *stage, i)) {
                continue;
            }
            const auto& fetch = desc.sharp_fetch;
            const auto at = [&](u32 dw) -> int {
                if (fetch.summary == Shader::SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad) {
                    return int(u32{fetch.offsets[0]} + dw);
                }
                return ((fetch.load_mask >> dw) & 1) ? int(u32{fetch.offsets[dw]}) : -1;
            };
            if (const int w0 = at(0); w0 >= 0 && std::size_t(w0) < words.size()) {
                words[w0] = 0;
            }
            if (const int w1 = at(1); w1 >= 0 && std::size_t(w1) < words.size()) {
                words[w1] &= ~0xffu;
            }
        }
        const std::size_t from = skip_user ? std::min(words.size(), stage->UserData().size()) : 0;
        h = XXH3_64bits_withSeed(words.data() + from, (words.size() - from) * sizeof(u32), h);
    }
    return XXH3_64bits_withSeed(&pipeline, sizeof(pipeline), h);
}
State& Get() {
    static State s;
    return s;
}

// 0: miss (the key is now recorded with this stamp), 1: hit in an earlier frame, 2: hit earlier in this frame.
int Probe(std::vector<Entry>& table, u64 key, u64 stamp) {
    const std::size_t first = key & (kTableSize - 1);
    for (const std::size_t i : {first, first ^ std::size_t{1}}) {
        Entry& e = table[i];
        if (e.stamp != 0 && e.key == key) {
            const int kind = e.stamp < stamp ? 1 : 2;
            e.stamp = stamp;
            return kind;
        }
    }
    const std::size_t partner = first ^ std::size_t{1};
    Entry& victim = table[table[first].stamp <= table[partner].stamp ? first : partner];
    victim = Entry{key, stamp};
    return 0;
}

// The attachment words of a draw: the color buffer registers and the depth buffer register.
template <typename Regs>
u64 TargetKey(const Regs& regs) {
    const u64 h = XXH3_64bits(regs.color_buffers, sizeof(regs.color_buffers));
    return XXH3_64bits_withSeed(&regs.depth_buffer, sizeof(regs.depth_buffer), h);
}

// Records one op. `target` is TargetKey of the draw regs (ignored for a dispatch, pass `draw` false).
void Note(const Pipeline* pipeline, bool draw, u64 target) {
    State& s = Get();
    u64 user_h = 0, flat_h = 0;
    bool writes = false;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        writes |= stage->has_storage_images;
        for (const auto& desc : stage->buffers) {
            writes |= !desc.IsSpecial() && desc.is_written;
        }
        for (const auto& desc : stage->images) {
            writes |= desc.is_written;
        }
        const auto user = stage->UserData();
        if (!user.empty()) {
            user_h = XXH3_64bits_withSeed(user.data(), user.size_bytes(), user_h);
        }
        const auto& flat = stage->FlatBuf();
        if (!flat.empty()) {
            flat_h = XXH3_64bits_withSeed(flat.data(), flat.size() * sizeof(u32), flat_h);
        }
    }
    const u64 pipe_h = XXH3_64bits(&pipeline, sizeof(pipeline));
    const u64 key_b = XXH3_64bits_withSeed(&flat_h, sizeof(flat_h),
                                           XXH3_64bits_withSeed(&user_h, sizeof(user_h), pipe_h));
    const u64 key_a = draw ? XXH3_64bits_withSeed(&target, sizeof(target), key_b) : key_b;
    const u64 tgt = draw ? target : 0;
    const u64 stamp = SotcStats::flip_count.load(std::memory_order_relaxed) + 1;
    const u64 key_c1 = XXH3_64bits_withSeed(&tgt, sizeof(tgt), MaskedFlatHash(pipeline, false));
    const u64 key_c2 = XXH3_64bits_withSeed(&tgt, sizeof(tgt), MaskedFlatHash(pipeline, true));
    const u64 key_d = XXH3_64bits_withSeed(&tgt, sizeof(tgt), DescriptorHash(pipeline));

    std::lock_guard<std::mutex> lock(s.mu);
    if (s.table_a.empty()) {
        s.table_a.resize(kTableSize);
        s.table_b.resize(kTableSize);
        s.table_c1.resize(kTableSize);
        s.table_c2.resize(kTableSize);
        s.table_d.resize(kTableSize);
    }
    s.hit_d += Probe(s.table_d, key_d, stamp) != 0 ? 1 : 0;
    s.hit_c1 += Probe(s.table_c1, key_c1, stamp) != 0 ? 1 : 0;
    s.hit_c2 += Probe(s.table_c2, key_c2, stamp) != 0 ? 1 : 0;
    ++s.ops;
    s.no_write += writes ? 0 : 1;
    const int a = Probe(s.table_a, key_a, stamp);
    const int b = Probe(s.table_b, key_b, stamp);
    s.win_a.insert(key_a);
    s.win_b.insert(key_b);
    if (a != 0) {
        ++s.hit_a;
        s.hit_prev_a += a == 1 ? 1 : 0;
    }
    s.hit_b += b != 0 ? 1 : 0;

    const auto it = s.last.find(pipeline);
    const bool have_last = it != s.last.end();
    if (a != 0 && have_last && it->second.key_a == key_a) {
        ++s.same_hit;
    }
    if (a == 0) { // the first differing word of the previous op of this pipeline names the miss
        if (!have_last) {
            ++s.miss_pipe;
        } else if (it->second.user != user_h) {
            ++s.miss_user;
        } else if (it->second.flat != flat_h) {
            ++s.miss_sharp;
        } else if (it->second.target != tgt) {
            ++s.miss_target;
        } else {
            ++s.miss_evicted; // same words as the previous op, so the table lost the key
        }
    }
    s.last[pipeline] = Last{user_h, flat_h, tgt, key_a};
}
} // namespace OpSetStats

// sotc: SOTC_OPSET_STATS: one line per report window (the 5 s report). Prints nothing while the switch is off.
void ReportOpSet(std::string& out, double seconds) {
    if (!OpSetStats::On()) {
        return;
    }
    OpSetStats::State& s = OpSetStats::Get();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.ops == 0) {
        return;
    }
    const double n = double(s.ops);
    const auto pct = [&](u64 v) { return 100.0 * double(v) / n; };
    char line[640];
    std::snprintf(line, sizeof line,
                  "SOTC OPSET: ops %llu (%.0f/s), hit A %.1f%% (prev-frame %.1f%%, same-as-last %.1f%%), "
                  "hit B %.1f%%, misses: pipeline %.1f%%, userdata %.1f%%, sharps %.1f%%, targets %.1f%% "
                  "(evicted %.1f%%); no-write ops %.1f%%; hit C1 (no addressed V# bases) %.1f%%, "
                  "C2 (also no user data) %.1f%%, D (descriptors only) %.1f%%",
                  (unsigned long long)s.ops, seconds > 0.0 ? n / seconds : 0.0, pct(s.hit_a),
                  pct(s.hit_prev_a), pct(s.same_hit), pct(s.hit_b), pct(s.miss_pipe), pct(s.miss_user),
                  pct(s.miss_sharp), pct(s.miss_target), pct(s.miss_evicted), pct(s.no_write),
                  pct(s.hit_c1), pct(s.hit_c2), pct(s.hit_d));
    out += line;
    out += '\n';
    std::snprintf(line, sizeof line, "SOTC OPSET keys: distinct A %llu, distinct B %llu in this window",
                  (unsigned long long)s.win_a.size(), (unsigned long long)s.win_b.size());
    out += line;
    out += '\n';
    s.ops = s.hit_a = s.hit_prev_a = s.same_hit = s.hit_b = 0;
    s.miss_pipe = s.miss_user = s.miss_sharp = s.miss_target = s.miss_evicted = 0;
    s.no_write = 0;
    s.hit_c1 = s.hit_c2 = s.hit_d = 0;
    s.win_a.clear();
    s.win_b.clear();
}

// sotc: SOTC_DRAW_MERGE_STATS (sotc_draw_merge.h). Every recorded draw gets a key: what one multi-draw call would have to
// share with its neighbours. A run is consecutive draws with one key, no barrier recorded between them, the same submit,
// and (indirect draws next to each other) arguments that follow each other with one stride. Draw thread only.
namespace {
using SotcDrawMerge::Break;

struct MergeKey {
    const void* pipeline{};
    u64 resources{};
    u64 submit{};
    u64 barriers{};
    VAddr args{};
    u32 stride{};
    bool indexed{};
    bool indirect{};
    bool joinable{};
    bool feedback{};
    RenderState state{};
    DynamicValues dyn{};
    u64 bound{};     // mode 2: the bound content (BoundContentHash), compared with the run's first draw
    u64 epoch{};     // mode 1: SotcDrawMerge::BindEpoch before the draw
    u64 gens{};      // mode 1: the fast-bind generations and the session serial
};

struct MergeRun {
    bool open{};
    u32 len{};
    u32 direct{};   // the direct draws among the run's members
    MergeKey prev{};
    MergeKey first{}; // mode 2: the run's first draw
};
MergeRun g_merge;

// The first reason the draw cannot join the run ending with p, or Break::None.
Break MergeBreak(const MergeKey& p, const MergeKey& cur) {
    if (cur.submit != p.submit) {
        return Break::Submit;
    }
    if (cur.barriers != p.barriers) {
        return Break::Barrier;
    }
    if (cur.epoch != p.epoch) {
        return Break::Epoch;
    }
    if (cur.pipeline != p.pipeline) {
        return Break::Pipeline;
    }
    if (cur.resources != p.resources) {
        return Break::Resources;
    }
    if (cur.gens != p.gens) {
        return Break::Gens;
    }
    if (!(cur.state == p.state)) {
        return Break::Targets;
    }
    if (cur.feedback != p.feedback || !SameDynamicValues(cur.dyn, p.dyn)) {
        return Break::Dynamic;
    }
    // A multi-draw takes one count of one buffer: a draw with a count buffer or several draws cannot join, and
    // neither can another index type. Indirect draws next to each other follow each other with one stride.
    if (!cur.joinable || cur.indexed != p.indexed) {
        return Break::Args;
    }
    if (cur.indirect && p.indirect && (cur.stride != p.stride || cur.args != p.args + p.stride)) {
        return Break::Args;
    }
    return Break::None;
}

void MergeClose() {
    if (!g_merge.open) {
        return;
    }
    g_merge.open = false;
    if (g_merge.len < 2) {
        return;
    }
    auto& c = SotcDrawMerge::g_counters;
    const u32 len = g_merge.len;
    (len == 2 ? c.runs2 : len <= 4 ? c.runs3_4 : len <= 8 ? c.runs5_8 : c.runs9)
        .fetch_add(1, std::memory_order_relaxed);
    c.saved.fetch_add(len - 1, std::memory_order_relaxed);
    c.direct_in_runs.fetch_add(g_merge.direct, std::memory_order_relaxed);
}

void MergeNote(const MergeKey& cur) {
    auto& c = SotcDrawMerge::g_counters;
    (cur.indirect ? c.indirect : c.direct).fetch_add(1, std::memory_order_relaxed);
    if (g_merge.open) {
        const Break br = MergeBreak(g_merge.prev, cur);
        if (br == Break::None) {
            if (SotcDrawMerge::Mode() == 2) { // sotc: verify, the draw is emitted as before
                c.verify_checks.fetch_add(1, std::memory_order_relaxed);
                if (cur.bound != g_merge.first.bound) {
                    c.verify_mismatch.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ++g_merge.len;
            g_merge.direct += cur.indirect ? 0u : 1u;
            g_merge.prev = cur;
            return;
        }
        MergeClose();
        c.breaks[static_cast<u32>(br)].fetch_add(1, std::memory_order_relaxed);
    }
    // A draw that cannot be a member starts nothing: it is only the previous draw of the next one.
    g_merge.open = cur.joinable;
    g_merge.len = cur.joinable ? 1 : 0;
    g_merge.direct = cur.joinable && !cur.indirect ? 1 : 0;
    g_merge.prev = cur;
    g_merge.first = cur;
}

// sotc: SOTC_DRAW_MERGE_STATS. The content key of one draw (no descriptor handles, no ring addresses): the
// pipeline's descriptors and flat buffer (OpSetStats::DescriptorHash, MaskedFlatHash with the user data kept),
// the attachment registers and the render-state registers. Read after the bind, from the same inputs.
u64 MergeContentKey(const GraphicsPipeline* pipeline, const AmdGpu::Regs& regs) {
    u64 h = OpSetStats::DescriptorHash(pipeline);
    h ^= OpSetStats::MaskedFlatHash(pipeline, false);
    h = XXH3_64bits_withSeed(&pipeline, sizeof(pipeline), h);
    const u64 target = OpSetStats::TargetKey(regs);
    h = XXH3_64bits_withSeed(&target, sizeof(target), h);
    const auto mix = [&h](const auto& field) {
        h = XXH3_64bits_withSeed(&field, sizeof(field), h);
    };
    mix(regs.color_control);
    mix(regs.depth_control);
    mix(regs.stencil_control);
    mix(regs.stencil_ref_front);
    mix(regs.stencil_ref_back);
    mix(regs.blend_control);
    mix(regs.blend_constants);
    mix(regs.clipper_control);
    mix(regs.primitive_restart_index);
    mix(regs.enable_primitive_restart);
    // The viewport words of the push data (the ring offsets are not in MakeUserData; they follow the buffers).
    const Shader::PushData push = MakeUserData(regs);
    h = XXH3_64bits_withSeed(&push, 16, h);
    return h;
}

// sotc: SOTC_DRAW_MERGE_STATS=2 verify. What the bind wrote, as a content hash: buffer handles and range sizes (not
// the ring offsets), image views, samplers and layouts, the viewport words of the push data (not the ring offsets
// of its buffers) and the bindless slots.
u64 BoundContentHash(const std::vector<vk::WriteDescriptorSet>& writes, const Shader::PushData& push,
                     const Shader::BindlessSamplerSlots& slots) {
    u64 h = XXH3_64bits(&push, 16); // xoffset, yoffset, xscale, yscale
    h = XXH3_64bits_withSeed(&slots, sizeof(slots), h);
    for (const auto& w : writes) {
        const u32 meta[4] = {w.dstBinding, w.dstArrayElement, static_cast<u32>(w.descriptorType),
                             w.descriptorCount};
        h = XXH3_64bits_withSeed(meta, sizeof(meta), h);
        for (u32 j = 0; j < w.descriptorCount; ++j) {
            if (w.pBufferInfo) {
                const VkBuffer buffer = w.pBufferInfo[j].buffer;
                const VkDeviceSize range = w.pBufferInfo[j].range;
                h = XXH3_64bits_withSeed(&buffer, sizeof(buffer), h);
                h = XXH3_64bits_withSeed(&range, sizeof(range), h);
            } else if (w.pImageInfo) {
                const VkImageView view = w.pImageInfo[j].imageView;
                const VkSampler sampler = w.pImageInfo[j].sampler;
                const u32 layout = static_cast<u32>(w.pImageInfo[j].imageLayout);
                h = XXH3_64bits_withSeed(&view, sizeof(view), h);
                h = XXH3_64bits_withSeed(&sampler, sizeof(sampler), h);
                h = XXH3_64bits_withSeed(&layout, sizeof(layout), h);
            }
        }
    }
    return h;
}
} // namespace

// sotc: SOTC_DRAW_MERGE_STATS=1 (reuse). The last full bind: the run's first draw, whose descriptors and push constants
// are still recorded in the command buffer while the epoch and the generations hold. g_pending is the draw being decided.
namespace {
struct MergeBind {
    bool valid{};
    bool touch{};     // the bind's barrier bookkeeping (ResetBindings' touches): a reused draw replays it
    u64 idx_hash{};   // the index inputs of the head (MergeIndexHash), compared by SOTC_DRAW_MDI=2
    MergeKey key{};
};
MergeBind g_bind;
MergeKey g_pending{};
bool g_pending_ok{};
u64 g_pending_idx{};
bool g_pending_indexed{};
bool g_pending_ext{};
u32 g_pending_ext_max{};
Scheduler* g_pending_sched{};
bool g_merge_touch{}; // set by MergeReuse for a joined draw, consumed by the next ResetBindings

// sotc: SOTC_DRAW_MDI=1. The open run: the head's bind (g_bind) and the draws joined to it. A joined draw records nothing
// at join time; its draw parameters are kept here and recorded at the flush, after the head.
struct MdiRun {
    bool open{};
    bool indexed{};
    std::thread::id owner{}; // the thread that opened the run (the draw thread)
    bool ext{};     // VK_EXT_multi_draw usable for this run
    u32 ext_max{};  // its maxMultiDrawCount
    Scheduler* scheduler{};
    std::vector<VkDrawIndexedIndirectCommand> idx;
    std::vector<VkDrawIndirectCommand> dir;
};
MdiRun g_run;

// Records the open run into its command buffer: VK_EXT_multi_draw calls for the runs of draws that share their
// instance parameters (and, when indexed, their vertex offset), one draw call each otherwise.
void FlushRun(SotcDrawMerge::Flush why) {
    if (!g_run.open) {
        return;
    }
    g_run.open = false;
    auto& c = SotcDrawMerge::g_counters;
    c.mdi_flush[static_cast<u32>(why)].fetch_add(1, std::memory_order_relaxed);
    const size_t n = g_run.indexed ? g_run.idx.size() : g_run.dir.size();
    if (n == 0) {
        return;
    }
    const SotcDrawMerge::DrawScope scope; // the run's own records: no epoch bump, no re-entry from NoteBind
    const vk::CommandBuffer cmd = g_run.scheduler->CommandBuffer();
    c.mdi_runs.fetch_add(1, std::memory_order_relaxed);
    c.mdi_deferred.fetch_add(n, std::memory_order_relaxed);
    u64 calls = 0;
    u64 ext_calls = 0;
    size_t i = 0;
    GpuCheckpoint(cmd, nullptr, SotcCheckpoints::Kind::DrawRun, n, g_run.indexed ? 1 : 0, 0); // sotc
    while (i < n) {
        // Draws i..j-1 share the instance parameters (and the vertex offset when indexed): one multi-draw call.
        const auto same = [&](size_t k) {
            if (g_run.indexed) {
                const auto& a = g_run.idx[i];
                const auto& b = g_run.idx[k];
                return a.instanceCount == b.instanceCount && a.firstInstance == b.firstInstance &&
                       a.vertexOffset == b.vertexOffset;
            }
            const auto& a = g_run.dir[i];
            const auto& b = g_run.dir[k];
            return a.instanceCount == b.instanceCount && a.firstInstance == b.firstInstance;
        };
        size_t j = i + 1;
        if (g_run.ext) {
            while (j < n && j - i < g_run.ext_max && same(j)) {
                ++j;
            }
        }
        const size_t cnt = j - i;
        if (cnt >= 2) {
            static thread_local std::vector<vk::MultiDrawIndexedInfoEXT> idx_info;
            static thread_local std::vector<vk::MultiDrawInfoEXT> dir_info;
            if (g_run.indexed) {
                idx_info.resize(cnt);
                for (size_t k = 0; k < cnt; ++k) {
                    const auto& a = g_run.idx[i + k];
                    idx_info[k] = vk::MultiDrawIndexedInfoEXT{a.firstIndex, a.indexCount, a.vertexOffset};
                }
                const auto& a = g_run.idx[i];
                const s32 vertex_offset = a.vertexOffset;
                cmd.drawMultiIndexedEXT(static_cast<u32>(cnt), idx_info.data(), a.instanceCount,
                                        a.firstInstance, sizeof(vk::MultiDrawIndexedInfoEXT),
                                        &vertex_offset);
            } else {
                dir_info.resize(cnt);
                for (size_t k = 0; k < cnt; ++k) {
                    const auto& a = g_run.dir[i + k];
                    dir_info[k] = vk::MultiDrawInfoEXT{a.firstVertex, a.vertexCount};
                }
                const auto& a = g_run.dir[i];
                cmd.drawMultiEXT(static_cast<u32>(cnt), dir_info.data(), a.instanceCount, a.firstInstance,
                                 sizeof(vk::MultiDrawInfoEXT));
            }
            ++calls;
            ++ext_calls;
        } else {
            if (g_run.indexed) {
                const auto& a = g_run.idx[i];
                cmd.drawIndexed(a.indexCount, a.instanceCount, a.firstIndex, a.vertexOffset,
                                a.firstInstance);
            } else {
                const auto& a = g_run.dir[i];
                cmd.draw(a.vertexCount, a.instanceCount, a.firstVertex, a.firstInstance);
            }
            ++calls;
        }
        i = j;
    }
    c.mdi_calls.fetch_add(calls, std::memory_order_relaxed);
    c.mdi_ext_calls.fetch_add(ext_calls, std::memory_order_relaxed);
    g_run.idx.clear();
    g_run.dir.clear();
}

// The flush hook of SotcDrawMerge (NoteBind, NoteSubmit): records the open run before the caller's recording.
void MdiFlushHook(SotcDrawMerge::Flush why) {
    // The run belongs to the draw thread: a recording on another thread (presenter, recorder, upload) only bumps the
    // epoch, so no later draw joins; the draw thread records the run at its next flush point.
    if (g_run.owner != std::this_thread::get_id()) {
        return;
    }
    FlushRun(why);
}

// A draw that does not leave the bind as it was (or whose bind is not reusable): no later draw joins it.
void MergeInvalidate() {
    FlushRun(SotcDrawMerge::Flush::Invalid);
    g_bind.valid = false;
}

// After the descriptor push of a full bind: the bind is reusable when the draw was eligible. `touch` is the draw's
// ResetBindings touches (descriptor reads, see ResetBindings).
void MergeBindDone(bool bound, bool touch) {
    if (SotcDrawMerge::Mode() != 1) {
        return;
    }
    g_bind.valid = bound && g_pending_ok;
    g_bind.touch = touch;
    g_bind.idx_hash = g_pending_idx;
    g_bind.key = g_pending;
    // sotc: SOTC_DRAW_MDI=1. A head that may be reused opens the run its joined draws are deferred to.
    if (SotcDrawMerge::MdiMode() == 1 && g_bind.valid) {
        FlushRun(SotcDrawMerge::Flush::NonJoin); // MergeReuse flushed it already; this is the guard
        g_run.open = true;
        g_run.owner = std::this_thread::get_id();
        g_run.indexed = g_pending_indexed;
        g_run.ext = g_pending_ext;
        g_run.ext_max = g_pending_ext_max;
        g_run.scheduler = g_pending_sched;
    }
}

// The index inputs of a draw: the index buffer binding (address and type) and the index offset of the draw packet.
u64 MergeIndexHash(const AmdGpu::Regs& regs, u32 index_offset) {
    u64 h = XXH3_64bits(&regs.index_base_address, sizeof(regs.index_base_address));
    h = XXH3_64bits_withSeed(&regs.index_buffer_type, sizeof(regs.index_buffer_type), h);
    return XXH3_64bits_withSeed(&index_offset, sizeof(index_offset), h);
}

// The SOTC_INDIRECT_DIRECT switch (DrawIndirect reads the arguments on the CPU when it is on): read once, as the draw does.
bool DirectArgsOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_INDIRECT_DIRECT");
        return !v || v[0] != '0';
    }();
    return on;
}

// sotc: SOTC_INDEX_EXTENT=1 (off by default). An indexed indirect draw binds its index buffer over the indices its
// arguments read, not over the whole window of the registers. The bind's base does not move (the draws' firstIndex is
// relative to it); only its size is shortened. Counters: IndexExtentStats, printed by ReportIndexExtent.
bool IndexExtentOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_INDEX_EXTENT");
        return v && v[0] == '1';
    }();
    return on;
}
namespace IndexExtentStats {
std::atomic<u64> binds{};       // index binds of indirect draws whose arguments were readable on the CPU
std::atomic<u64> narrowed{};    // of those, the binds whose size was shortened
std::atomic<u64> bytes_saved{}; // index bytes the shortened binds left out of the register window
} // namespace IndexExtentStats

// sotc: SOTC_INDEX_EXTENT. The end of the indices an indexed indirect draw reads (max of firstIndex + indexCount over its
// draws; the draws of the count buffer when there is one), from the arguments on the CPU. Read under the same condition as
// DrawIndirect's direct arguments: no GPU write pending over the bytes, and a backing read. 0 when unknown: the bind keeps
// the register window.
u32 IndirectIndexExtent(Core::MemoryManager* memory, VideoCore::BufferCache& buffer_cache, VAddr arg_address,
                        u32 stride, u32 max_count, VAddr count_address) {
    if (stride != sizeof(VkDrawIndexedIndirectCommand) || max_count == 0) {
        return 0;
    }
    u32 draws = max_count;
    if (count_address != 0) {
        u32 count = 0;
        if (buffer_cache.IsRegionGpuModified(count_address, sizeof count) ||
            !memory->TryReadBacking(count_address, &count, sizeof count)) {
            return 0;
        }
        draws = std::min(count, max_count);
    }
    if (draws == 0) {
        return 0;
    }
    const u64 bytes = u64{draws} * stride;
    if (buffer_cache.IsRegionGpuModified(arg_address, bytes)) {
        return 0;
    }
    static thread_local std::vector<VkDrawIndexedIndirectCommand> args;
    args.resize(draws);
    if (!memory->TryReadBacking(arg_address, args.data(), bytes)) {
        return 0;
    }
    u64 end = 0;
    for (const auto& a : args) {
        // A draw with no indices or no instances reads no index.
        if (a.indexCount != 0 && a.instanceCount != 0) {
            end = std::max<u64>(end, u64{a.firstIndex} + a.indexCount);
        }
    }
    return static_cast<u32>(std::min<u64>(end, 0xffffffffu));
}

// sotc: SOTC_DRAW_MDI=1. A joined draw's record: the draw's parameters, recorded at the run's flush.
void MdiDefer(bool indexed, u32 count, u32 instances, s32 vertex_offset, u32 first_instance) {
    if (indexed) {
        g_run.idx.push_back(VkDrawIndexedIndirectCommand{count, instances, 0, vertex_offset, first_instance});
    } else {
        g_run.dir.push_back(VkDrawIndirectCommand{count, instances, static_cast<u32>(vertex_offset), first_instance});
    }
}

// Mode 1 key: the bind's own content hash (FastBindHash: pipeline, push data, attachments, user data, flat buffer), then
// the render-state registers. The descriptor handles and the sharps in memory are covered by the fast-bind generations
// (mem_epoch) as the existing fast bind already relies on them.
u64 MergeFastKey(u64 content, const AmdGpu::Regs& regs) {
    u64 h = content;
    const auto mix = [&h](const auto& field) {
        h = XXH3_64bits_withSeed(&field, sizeof(field), h);
    };
    mix(regs.color_control);
    mix(regs.depth_control);
    mix(regs.stencil_control);
    mix(regs.stencil_ref_front);
    mix(regs.stencil_ref_back);
    mix(regs.blend_control);
    mix(regs.blend_constants);
    mix(regs.clipper_control);
    mix(regs.primitive_restart_index);
    mix(regs.enable_primitive_restart);
    return h;
}
} // namespace

// sotc: SOTC_INDEX_EXTENT=1. The 5 s line: index binds of indirect draws with readable arguments, how many were shortened,
// and the index bytes left out of their window.
void ReportIndexExtent(std::string& out, double seconds) {
    if (!IndexExtentOn()) {
        return;
    }
    const u64 binds = IndexExtentStats::binds.exchange(0, std::memory_order_relaxed);
    const u64 narrowed = IndexExtentStats::narrowed.exchange(0, std::memory_order_relaxed);
    const u64 saved = IndexExtentStats::bytes_saved.exchange(0, std::memory_order_relaxed);
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC INDEX EXTENT: %llu binds with readable arguments, %llu narrowed, %.1f MiB of index window "
                  "left unbound (%.1f s)",
                  static_cast<unsigned long long>(binds), static_cast<unsigned long long>(narrowed),
                  double(saved) / (1024.0 * 1024.0), seconds);
    out += line;
    out += '\n';
}

bool Rasterizer::MergeReuse(const GraphicsPipeline* pipeline, bool is_indexed, bool indirect,
                            bool joinable, const PreparedDraw* prepared, u32 index_offset) {
    SotcDrawMerge::g_flush_hook = &MdiFlushHook; // sotc: SOTC_DRAW_MDI (mode 1 only calls it)
    MergeKey key{};
    key.pipeline = pipeline;
    key.indexed = is_indexed;
    key.indirect = indirect;
    key.joinable = joinable;
    key.submit = scheduler.CurrentTick();
    key.barriers = runtime.FlushGeneration();
    key.feedback = attachment_feedback_loop;
    key.epoch = SotcDrawMerge::BindEpoch();
    if (prepared) {
        key.dyn = prepared->dynamic;
    } else {
        ComputeDynamic(liverpool->DrawRegs(), is_indexed, key.dyn);
    }
    // The bind's content hash, computed once: a prepared draw carries it; otherwise it is the fast-bind hash.
    const AmdGpu::Regs& regs = liverpool->DrawRegs();
    const u64 content = prepared ? prepared->fast_hash : FastBindHash(pipeline, MakeUserData(regs), regs);
    key.resources = MergeFastKey(content, regs);
    // The index inputs (SOTC_DRAW_MDI: the joiners share the head's index binding): indexed draws only.
    const u64 idx_hash = is_indexed ? MergeIndexHash(regs, index_offset) : 0;
    if (is_indexed) {
        key.resources = XXH3_64bits_withSeed(&idx_hash, sizeof(idx_hash), key.resources);
    }
    const u64 gens[4] = {
        static_cast<u64>(BufferBindGeneration()), static_cast<u64>(texture_cache.BindGeneration()),
        static_cast<u64>(buffer_cache.StreamGeneration()),
        static_cast<u64>(SotcStats::mem_epoch.load(std::memory_order_relaxed))};
    const u64 serial = scheduler.SessionSerial();
    key.gens = XXH3_64bits_withSeed(&serial, sizeof(serial), XXH3_64bits(gens, sizeof(gens)));
    MergeNote(key); // the run bookkeeping of the 5 s report (DrawMergeNote is off in this mode)

    // The path counters: what the draw runs through (the path is reused like any other, see below).
    auto& c = SotcDrawMerge::g_counters;
    if (prepared) {
        c.seen_prepared.fetch_add(1, std::memory_order_relaxed);
    }
    if (SotcArch::Packet::On()) {
        c.seen_packet.fetch_add(1, std::memory_order_relaxed);
    }
    if (SotcArch::Heap::On()) {
        c.seen_heap.fetch_add(1, std::memory_order_relaxed);
    }
    if (BarrierStatic::On()) {
        c.seen_barrier.fetch_add(1, std::memory_order_relaxed);
    }
    if (SotcArch::GuestBarrier::Active()) {
        c.seen_guest.fetch_add(1, std::memory_order_relaxed);
    }

    // Eligible: a direct-args candidate with no attachment feedback and no storage writes. The packet, heap, prepared and
    // barrier paths are reused: their per-draw pushes are kept from the first draw (their inputs are in the key), and the
    // barrier static touch is replayed (g_merge_touch). Anything else binds in full and ends the reuse.
    bool eligible = true;
    if (!joinable) {
        c.refused[static_cast<u32>(SotcDrawMerge::Refuse::Shape)].fetch_add(1, std::memory_order_relaxed);
        eligible = false;
    } else if (attachment_feedback_loop) {
        c.refused[static_cast<u32>(SotcDrawMerge::Refuse::Feedback)].fetch_add(1, std::memory_order_relaxed);
        eligible = false;
    } else if (StagesWriteMemory(*pipeline)) {
        c.refused[static_cast<u32>(SotcDrawMerge::Refuse::Writes)].fetch_add(1, std::memory_order_relaxed);
        eligible = false;
    } else if (Dlssg::CapturingInputs()) {
        // The DLSS input capture is made per draw (Dlssg::NoteDraw): a draw under it is not reused.
        c.refused[static_cast<u32>(SotcDrawMerge::Refuse::Dlss)].fetch_add(1, std::memory_order_relaxed);
        eligible = false;
    }
    g_pending = key;
    g_pending_ok = eligible;
    g_pending_idx = idx_hash;
    g_pending_indexed = is_indexed;
    g_pending_ext = instance.IsMultiDrawSupported() && instance.MaxMultiDrawCount() > 0 &&
                    !std::getenv("SOTC_DRAW_MDI_NOEXT"); // sotc: diagnostic, consecutive draws instead
    g_pending_ext_max = instance.MaxMultiDrawCount();
    g_pending_sched = &scheduler;
    c.mdi_ext_avail.store(instance.IsMultiDrawSupported() ? 1 : 0, std::memory_order_relaxed);
    bool join = false;
    if (eligible) {
        if (!g_bind.valid) {
            c.refused[static_cast<u32>(SotcDrawMerge::Refuse::NoBind)].fetch_add(1, std::memory_order_relaxed);
        } else if (SotcDrawMerge::MdiMode() == 1 && !g_run.open) {
            // A joined draw is deferred to the run of its head: no open run, no join.
            c.refused[static_cast<u32>(SotcDrawMerge::Refuse::NoBind)].fetch_add(1, std::memory_order_relaxed);
        } else {
            join = MergeBreak(g_bind.key, key) == Break::None;
        }
    }
    g_merge_touch = join && g_bind.touch;
    if (join) {
        c.skipped.fetch_add(1, std::memory_order_relaxed);
        if (SotcDrawMerge::MdiMode() == 2) { // verify: the index inputs of the joiner against the head's
            c.mdi_checks.fetch_add(1, std::memory_order_relaxed);
            if (idx_hash != g_bind.idx_hash) {
                c.mdi_mismatch.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else {
        FlushRun(SotcDrawMerge::Flush::NonJoin); // the run ends: this draw records next
    }
    return join;
}

void Rasterizer::DrawMergeNote(const RenderState& state, const GraphicsPipeline* pipeline,
                               bool is_indexed, bool indirect, VAddr arg_address, u32 stride,
                               u32 max_count, VAddr count_address, const PreparedDraw* prepared) {
    if (SotcDrawMerge::Mode() == 1) { // the run is kept by MergeReuse (its key is taken before the bind)
        return;
    }
    MergeKey key{};
    key.pipeline = pipeline;
    key.indexed = is_indexed;
    key.indirect = indirect;
    key.joinable = !indirect || (max_count == 1 && count_address == 0);
    key.args = arg_address;
    key.stride = stride;
    key.submit = scheduler.CurrentTick();
    key.barriers = runtime.FlushGeneration();
    key.feedback = attachment_feedback_loop;
    key.state = state;
    if (prepared) {
        key.dyn = prepared->dynamic;
    } else {
        ComputeDynamic(liverpool->DrawRegs(), is_indexed, key.dyn);
    }
    // What one draw call shares: the push constants, the packet and heap block when they are in use, and the
    // descriptors as the bind wrote them (the set writes, resized to the bind's count).
    u64 h = XXH3_64bits(&push_data, sizeof(push_data));
    h = XXH3_64bits_withSeed(&bindless_slots, sizeof(bindless_slots), h);
    if (SotcArch::Packet::On()) {
        h = XXH3_64bits_withSeed(&draw_packet, sizeof(draw_packet), h);
    }
    if (SotcArch::Heap::On()) {
        h = XXH3_64bits_withSeed(&heap_block, sizeof(heap_block), h);
    }
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const auto& w = set_writes[i];
        const u32 set_index = i < set_write_sets.size() ? set_write_sets[i] : 0u;
        const u32 meta[5] = {set_index, w.dstBinding, w.dstArrayElement,
                             static_cast<u32>(w.descriptorType), w.descriptorCount};
        h = XXH3_64bits_withSeed(meta, sizeof(meta), h);
        for (u32 j = 0; j < w.descriptorCount; ++j) {
            if (w.pBufferInfo) {
                const VkBuffer buffer = w.pBufferInfo[j].buffer;
                const u64 range[2] = {w.pBufferInfo[j].offset, w.pBufferInfo[j].range};
                h = XXH3_64bits_withSeed(&buffer, sizeof(buffer), h);
                h = XXH3_64bits_withSeed(range, sizeof(range), h);
            } else if (w.pImageInfo) {
                const VkImageView view = w.pImageInfo[j].imageView;
                const VkSampler sampler = w.pImageInfo[j].sampler;
                const u32 layout = static_cast<u32>(w.pImageInfo[j].imageLayout);
                h = XXH3_64bits_withSeed(&view, sizeof(view), h);
                h = XXH3_64bits_withSeed(&sampler, sizeof(sampler), h);
                h = XXH3_64bits_withSeed(&layout, sizeof(layout), h);
            }
        }
    }
    key.resources = h;
    if (SotcDrawMerge::Mode() != 0) { // sotc: content key (sotc_draw_merge.h), indirect args not compared
        key.resources = MergeContentKey(pipeline, liverpool->DrawRegs());
        key.args = 0;
        key.stride = 0;
        if (SotcDrawMerge::Mode() == 2) {
            key.bound = BoundContentHash(set_writes, push_data, bindless_slots);
        }
    }
    MergeNote(key);
}

// sotc: SOTC_PREPASS_PROBE (observe only). After an op's bindings are resolved: does the op read an image at the
// main depth base (bound, not a render target)? Its phase and pipeline key go to SotcPrepassProbe::NoteBind.
template <typename Images>
static void PrepassProbeNoteBind(bool is_draw, const AmdGpu::Regs* regs, const Pipeline* pipeline,
                                 VideoCore::TextureCache& tc, const Images& images) {
    if (!SotcPrepassProbe::On()) {
        return;
    }
    const uint64_t main = SotcPrepassProbe::main_addr.load(std::memory_order_relaxed);
    bool reads = false;
    for (const auto id : images) {
        if (main != 0 && id && !tc.GetImage(id).binding.is_target &&
            tc.GetImage(id).info.guest_address == main) {
            reads = true;
            break;
        }
    }
    uint64_t key = 0;
    if (reads) {
        for (const auto* stage : pipeline->GetStages()) {
            key = stage ? (key * 0x100000001b3ull) ^ stage->pgm_hash : key; // unused stages are null
        }
    }
    SotcPrepassProbe::NoteBind(is_draw, regs, reads, key);
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const Pipeline* resolved,
                      bool use_resolved, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    SotcStats::total_draws.fetch_add(1, std::memory_order_relaxed); // sotc: Lab route load-end detection
    // sotc: SOTC_TEX_PROTECT_BATCH (no-op unless set): texture Tracks of this draw are deferred to
    // the scope's flush, which runs before the next command (see TexProtectScope).
    const VideoCore::TexProtectScope tex_protect_scope;
    MdiBreak(); // sotc: SOTC_MDI_STATS
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDraw}; // sotc: SOTC_OP_PHASES

    scheduler.PopPendingOperations();
    const SotcDrawMerge::DrawScope draw_scope; // sotc: SOTC_DRAW_MERGE_STATS=1, the draw's own recordings do not bump the epoch
    TracePass(liverpool->DrawRegs());
    PassStats::Note(liverpool->DrawRegs()); // sotc: SOTC_PASS_STATS

    if (!FilterDraw()) {
        return;
    }
    if (SkipStaggeredShadow(liverpool->DrawRegs()) || ReuseShadowCascade()) {
        return;
    }

    const auto& regs = liverpool->DrawRegs();
    const GraphicsPipeline* pipeline =
        use_resolved ? static_cast<const GraphicsPipeline*>(resolved)
                     : [&] { // sotc: SOTC_TRACE
                           const SotcTrace::ScopeMin trace_step{"get pipeline"};
                           const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline}; // sotc: SOTC_OP_PHASES
                           return pipeline_cache.GetGraphicsPipeline();
                       }();
    SotcStats::current_program.store(1, std::memory_order_relaxed);
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[0].fetch_add(1, std::memory_order_relaxed);
        if (regs.num_indices == 0 || regs.num_instances.NumInstances() == 0) {
            SotcStats::cmd_kinds[6].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!pipeline) {
        SotcStats::Drop(0, regs.color_buffers[0].Address(), regs.stage_enable.raw);
        return;
    }
    // sotc: SOTC_DRAW_GATE: a draw whose buffers wait for the staged upload is made in a later frame (draw_gate.h).
    // Before any bind, as a draw whose shaders are still translating.
    if (SotcDrawGate::On() && DrawGateDefers(pipeline, is_indexed, index_offset)) {
        return;
    }
    if (SotcGcnCensus::On()) { // sotc: SOTC_GCN_CENSUS (measurement only)
        CensusNote(pipeline, SotcGcnCensus::kDraw);
    }
    TraceGbufferShaders(regs, pipeline);
    TraceShadowShaders(regs, pipeline);
    PassStats::NoteShader(regs, pipeline); // sotc: SOTC_PASS_STATS
    if (prepared) { // sotc: SOTC_PARALLEL_RECORD (SOTC_PR_VERIFY compares it)
        prepared = CheckPrepared(*prepared, pipeline, is_indexed, true);
    }

    // sotc: SOTC_DRAW_MERGE_STATS=1. A draw that joins the last full bind skips its bind; with SOTC_DRAW_MDI=1 it also
    // skips the rest of its per-draw state (identical by key) and only appends its draw to the run (flushed later).
    const bool merged =
        SotcDrawMerge::Mode() == 1 && MergeReuse(pipeline, is_indexed, false, true, prepared, index_offset);
    if (merged && SotcDrawMerge::MdiMode() == 1) {
        const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto& fetch_shader = pipeline->GetFetchShader();
        const auto [vertex_offset, instance_offset] =
            prepared && prepared->has_offsets
                ? std::pair<u32, u32>{prepared->vertex_offset, prepared->instance_offset}
                : GetDrawOffsets(regs, vs_info, fetch_shader);
        MdiDefer(is_indexed, regs.num_indices, regs.num_instances.NumInstances(), s32(vertex_offset),
                 instance_offset);
        ResetBindings(false); // the per-draw lists stay bounded (a joined draw binds nothing; replays the head's touch)
        DebugState.IncDrawCall();
        return;
    }
    {
        const SotcTrace::ScopeMin trace_step{"render state"}; // sotc: SOTC_TRACE
        PrepareRenderState(pipeline);
    }
    hudless_ui_info = -1; // sotc: TakeHudless, set by BindTextures and BeginRendering
    hudless_vo = 0;
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of DLSS (frame generation, SR)
        Dlssg::NoteDraw(regs, pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache, runtime, scheduler);
    }
    bool bound = true; // sotc: SOTC_TRACE
    if (!merged) {
        const SotcTrace::ScopeMin trace_step{"bind resources"};
        bound = BindResources(pipeline, prepared);
    }
    if (!bound) {
        MergeInvalidate();
        SotcStats::Drop(1, regs.color_buffers[0].Address(), 0);
        return;
    }
    PrepassProbeNoteBind(true, &regs, pipeline, texture_cache, bound_images); // sotc: SOTC_PREPASS_PROBE
    const auto state = [&] { // sotc: SOTC_TRACE
        const SotcTrace::ScopeMin trace_step{"begin rendering"};
        return BeginRendering(pipeline);
    }();
    RefreshImageLayouts();

    // sotc: the binds below can submit the command buffer (arena allocation, readbacks,
    // releases); what was recorded before lands in the old one. Re-record vertex state then.
    // sotc: SOTC_VERTEX_EXTENT: the vertex binds below narrow to this draw's vertices. Not under the MDI path
    // (MdiMode 1): a joined draw of a run reuses the head's vertex binds.
    const SotcVertexExtent::Draw vertex_extent =
        MakeVertexExtent(regs, pipeline->GetStage(Shader::SwStage::Vertex), pipeline->GetFetchShader(),
                         prepared, is_indexed, index_offset, SotcDrawMerge::MdiMode() != 1);
    u64 vertex_tick = scheduler.CurrentTick();
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhVertexIndex}; // sotc: SOTC_OP_PHASES
        {
            const SotcTrace::BindScope trace_step{"vertex buffers"}; // sotc: SOTC_TRACE
            BindVertexBuffers(pipeline, &vertex_extent);
        }
        if (is_indexed) {
            BindIndexBuffer(index_offset);
        }
        RebindVertexIfSubmitted(pipeline, vertex_tick, is_indexed, [&] { BindIndexBuffer(index_offset); },
                                &vertex_extent);
    }

    if (DecideOpBarrier(!bound_buffers.empty() || !fixed_function_reads.empty() || shader_addr_touch,
                        StagesWriteMemory(*pipeline))) {
        runtime.FlushBarriers();
    }

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        if (!merged && (!fast_bind_hit || fast_bind.tick != scheduler.CurrentTick())) {
            pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots, &draw_packet, &heap_block);
        }
        if (!merged) { // sotc: SOTC_DRAW_MERGE_STATS=1, this bind is the one later draws may reuse
            MergeBindDone(bound, op_static_reads || !bound_buffers.empty() || shader_addr_touch);
        }
    }
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDynamicState}; // sotc: SOTC_OP_PHASES
        UpdateDynamicState(pipeline, is_indexed, prepared);
    }
    GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER (before the pass: a pending writer barrier goes first)
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] =
        prepared && prepared->has_offsets
            ? std::pair<u32, u32>{prepared->vertex_offset, prepared->instance_offset}
            : GetDrawOffsets(regs, vs_info, fetch_shader);
    if (SotcDrawMerge::On()) { // sotc: SOTC_DRAW_MERGE_STATS (measurement only)
        DrawMergeNote(state, pipeline, is_indexed, false, 0, 0, 1, 0, prepared);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    const u64 cp_seq = GpuCheckpoint(cmdbuf, pipeline,
                                     is_indexed ? SotcCheckpoints::Kind::DrawIndexed : SotcCheckpoints::Kind::Draw,
                                     regs.num_indices, regs.num_instances.NumInstances(), instance_offset); // sotc
    CheckpointResources(cp_seq);
    // sotc: SOTC_SKIP_DRAW_VS=<vertex program hash> (diagnostics): the draws of that vertex program are not recorded.
    static const u64 skip_vs = [] {
        const char* v = std::getenv("SOTC_SKIP_DRAW_VS");
        return v ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    if (skip_vs != 0 && vs_info.pgm_hash == skip_vs) {
        static std::atomic<u32> skipped{0};
        if (skipped.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::fprintf(stderr, "SOTC SKIP DRAW VS: draw of vs %#llx not recorded\n",
                         static_cast<unsigned long long>(skip_vs));
        }
    } else if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    if (cp_seq != 0) { // sotc: SOTC_GPU_CHECKPOINTS: the end of this draw (what follows it is unmarked copies/barriers)
        cmdbuf.setCheckpointNV(reinterpret_cast<const void*>(cp_seq | SotcCheckpoints::kAfterBit));
    }
    GcnAfterOp(*pipeline); // sotc: SOTC_GCN_BARRIER
    // sotc: the composites below record their own pipelines and descriptors: no later draw reuses this bind.
    if ((hudless_ui_info >= 0 && hudless_vo) || ui_shadow_hit) {
        MergeInvalidate();
    }
    // sotc: the final composite again, without the UI layer (TakeHudless).
    if (hudless_ui_info >= 0 && hudless_vo && BeginHudless(pipeline, state)) {
        const auto hudless_cmdbuf = scheduler.CommandBuffer();
        if (is_indexed) {
            hudless_cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                                       s32(vertex_offset), instance_offset);
        } else {
            hudless_cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(),
                                vertex_offset, instance_offset);
        }
    }
    // sotc: a draw into the UI layer again, at the output size (BeginUiShadow).
    if (ui_shadow_hit && BeginUiShadow(pipeline, state)) {
        const auto shadow_cmdbuf = scheduler.CommandBuffer();
        if (is_indexed) {
            shadow_cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                                      s32(vertex_offset), instance_offset);
        } else {
            shadow_cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                               instance_offset);
        }
        EndUiShadow();
    }
    DebugState.IncDrawCall();

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(false);
        FlushPeriodic();
    }
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset, const Pipeline* resolved,
                              bool use_resolved, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    SotcStats::total_draws.fetch_add(1, std::memory_order_relaxed); // sotc: Lab route load-end detection
    const VideoCore::TexProtectScope tex_protect_scope; // sotc: SOTC_TEX_PROTECT_BATCH, see Draw
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDrawIndirect}; // sotc: SOTC_OP_PHASES

    scheduler.PopPendingOperations();
    const SotcDrawMerge::DrawScope draw_scope; // sotc: SOTC_DRAW_MERGE_STATS=1, see Draw
    TracePass(liverpool->DrawRegs());
    PassStats::Note(liverpool->DrawRegs()); // sotc: SOTC_PASS_STATS

    if (!FilterDraw()) {
        return;
    }
    if (SkipStaggeredShadow(liverpool->DrawRegs()) || ReuseShadowCascade()) {
        return;
    }

    if (SotcStats::kCmdStats) { // sotc: the shared SOTC_CMD_STATS switch (sotc_stats.h)
        const u64 args_size = u64{stride} * max_count;
        if (buffer_cache.IsRegionGpuModified(arg_address + offset, args_size) ||
            (count_address && buffer_cache.IsRegionGpuModified(count_address, 4))) {
            SotcStats::cmd_kinds[1].fetch_add(1, std::memory_order_relaxed);
        } else {
            SotcStats::cmd_kinds[2].fetch_add(1, std::memory_order_relaxed);
            u32 first[2]{1, 1};
            if (count_address == 0 && memory->TryReadBacking(arg_address + offset, first, 8) &&
                (first[0] == 0 || first[1] == 0)) {
                SotcStats::cmd_kinds[3].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline =
        use_resolved ? static_cast<const GraphicsPipeline*>(resolved)
                     : [&] { // sotc: SOTC_TRACE
                           const SotcTrace::ScopeMin trace_step{"get pipeline"};
                           const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline}; // sotc: SOTC_OP_PHASES
                           return pipeline_cache.GetGraphicsPipeline(params);
                       }();
    if (!pipeline) {
        SotcStats::Drop(0, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        return;
    }
    if (SotcGcnCensus::On()) { // sotc: SOTC_GCN_CENSUS (measurement only)
        CensusNote(pipeline, SotcGcnCensus::kDrawIndirect);
    }
    TraceShadowShaders(liverpool->DrawRegs(), pipeline);
    PassStats::NoteShader(liverpool->DrawRegs(), pipeline); // sotc: SOTC_PASS_STATS
    SotcPrepassProbe::t_promote = false; // sotc: SOTC_NO_PREPASS=1 (set below for a promoted G-buffer draw)
    if (SotcPrepassProbe::t_cp_mark >= 2) { // sotc: SOTC_NO_PREPASS=3, decided by the command processor
        SotcPrepassProbe::t_promote = true;
        SotcPrepassProbe::t_promote_func = static_cast<AmdGpu::CompareFunc>(SotcPrepassProbe::t_cp_mark - 2);
    }
    if (SotcPrepassProbe::On() || SotcPrepassProbe::SkipMode()) { // sotc: SOTC_PREPASS_PROBE / SOTC_NO_PREPASS=1
        bool discard = false;
        for (const auto* st : pipeline->GetStages()) {
            discard |= st && st->sw_stage == Shader::SwStage::Fragment && st->has_discard;
        }
        SotcPrepassProbe::NoteArgs(liverpool->DrawRegs(), arg_address + offset, discard);
        if (SotcPrepassProbe::SkipPrepassDraw(liverpool->DrawRegs(), arg_address + offset, discard)) {
            return; // an opaque XPR pre-pass draw: its G-buffer draw writes the depth
        }
    }
    if (prepared) { // sotc: SOTC_PARALLEL_RECORD (SOTC_PR_VERIFY compares it)
        prepared = CheckPrepared(*prepared, pipeline, is_indexed, false);
    }

    {
        const SotcTrace::ScopeMin trace_step{"render state"}; // sotc: SOTC_TRACE
        PrepareRenderState(pipeline);
    }
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of DLSS (frame generation, SR)
        FlushRun(SotcDrawMerge::Flush::NonJoin); // sotc: SOTC_DRAW_MDI, its records are made before this draw's
        Dlssg::NoteDraw(liverpool->DrawRegs(), pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache, runtime, scheduler);
    }
    // sotc: SOTC_INDEX_EXTENT=1. The indices the arguments read (0 = keep the register window of the index bind). Not with
    // SOTC_DRAW_MDI=1: a joined draw is recorded with the head's index bind, which would be too short for its indices.
    const u32 index_extent =
        is_indexed && IndexExtentOn() && SotcDrawMerge::MdiMode() != 1
            ? IndirectIndexExtent(memory, buffer_cache, arg_address + offset, stride, max_count, count_address)
            : 0;
    // sotc: SOTC_DRAW_MERGE_STATS=1. A direct-args candidate (one argument record) may join the last full bind. The
    // direct branch below re-checks its own conditions; when it is not taken, the general path binds in full.
    const bool indirect_candidate =
        count_address == 0 && max_count == 1 &&
        stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand));
    bool merged = SotcDrawMerge::Mode() == 1 &&
                  MergeReuse(pipeline, is_indexed, true, indirect_candidate, prepared, 0);
    bool bound = true; // sotc: SOTC_TRACE
    if (!merged) {
        const SotcTrace::ScopeMin trace_step{"bind resources"};
        bound = BindResources(pipeline, prepared);
    }
    if (!bound) {
        MergeInvalidate();
        SotcStats::Drop(1, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        return;
    }
    {
        const auto& probe_regs = liverpool->DrawRegs(); // sotc: SOTC_PREPASS_PROBE
        PrepassProbeNoteBind(true, &probe_regs, pipeline, texture_cache, bound_images);
    }
    // sotc: SOTC_DRAW_MDI=1. A joined direct-args draw appends its arguments to the run: no state, vertex or draw record of
    // its own (identical to the head's by key). Same conditions as the direct branch below; otherwise it binds in full.
    if (merged && SotcDrawMerge::MdiMode() == 1 && count_address == 0 && max_count == 1 &&
        stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand)) &&
        DirectArgsOn() && !buffer_cache.IsRegionGpuModified(arg_address + offset, stride)) {
        if (is_indexed) {
            VkDrawIndexedIndirectCommand a{};
            if (memory->TryReadBacking(arg_address + offset, &a, stride)) {
                g_run.idx.push_back(a);
                ResetBindings(false); // sotc: SOTC_DRAW_MDI, see the direct path
                DebugState.IncDrawCall();
                return;
            }
        } else {
            VkDrawIndirectCommand a{};
            if (memory->TryReadBacking(arg_address + offset, &a, stride)) {
                g_run.dir.push_back(a);
                ResetBindings(false); // sotc: SOTC_DRAW_MDI, see the direct path
                DebugState.IncDrawCall();
                return;
            }
        }
    }
    if (kMdiStats.get()) { // sotc: SOTC_MDI_STATS (fast_bind_hit is this draw's, set by BindResources)
        MdiNote(pipeline, is_indexed, arg_address, offset, stride, max_count, count_address,
                fast_bind_hit, buffer_cache.IsRegionGpuModified(arg_address + offset, stride),
                scheduler.CurrentTick());
    }
    const auto state = [&] { // sotc: SOTC_TRACE
        const SotcTrace::ScopeMin trace_step{"begin rendering"};
        return BeginRendering(pipeline);
    }();
    ui_shadow_broken |= ui_shadow_hit; // sotc: indirect draws are not redrawn (BeginUiShadow)
    RefreshImageLayouts();

    u64 vertex_tick = scheduler.CurrentTick(); // sotc: see the direct draw
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhVertexIndex}; // sotc: SOTC_OP_PHASES
        {
            const SotcTrace::BindScope trace_step{"vertex buffers"}; // sotc: SOTC_TRACE
            BindVertexBuffers(pipeline);
        }
        if (is_indexed) {
            BindIndexBuffer(0, index_extent);
        }
        RebindVertexIfSubmitted(pipeline, vertex_tick, is_indexed,
                                [&] { BindIndexBuffer(0, index_extent); });
    }

    // sotc: SotC writes the arguments of most indirect draws with WRITE_DATA right before them
    // (thousands per frame). With the arguments CPU-side, read them here and draw directly: no
    // upload of the argument range, no barrier against the next WRITE_DATA. The pipeline is the
    // same; the base vertex/instance reach the shader through the draw call in both cases.
    // SOTC_INDIRECT_DIRECT=0 keeps the indirect draws.
    static const bool direct_args = [] {
        const char* v = std::getenv("SOTC_INDIRECT_DIRECT");
        return !v || v[0] != '0';
    }();
    // Read through the backing view: the page may be read-protected for other GPU-written bytes.
    union {
        VkDrawIndexedIndirectCommand indexed;
        VkDrawIndirectCommand plain;
    } args;
    if (direct_args && count_address == 0 && max_count == 1 &&
        stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                              : sizeof(VkDrawIndirectCommand)) &&
        !buffer_cache.IsRegionGpuModified(arg_address + offset, stride) &&
        memory->TryReadBacking(arg_address + offset, &args, stride)) {
        if (DecideOpBarrier(!bound_buffers.empty() || !fixed_function_reads.empty() || shader_addr_touch,
                            StagesWriteMemory(*pipeline))) {
            runtime.FlushBarriers();
        }
        {
            const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
            if (!merged && (!fast_bind_hit || fast_bind.tick != scheduler.CurrentTick())) {
                pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots, &draw_packet, &heap_block);
            }
            if (!merged) { // sotc: SOTC_DRAW_MERGE_STATS=1, this direct draw's bind may be reused
                MergeBindDone(true, op_static_reads || !bound_buffers.empty() || shader_addr_touch);
            }
        }
        {
            const SotcOpPhases::Scope ph{SotcOpPhases::PhDynamicState}; // sotc: SOTC_OP_PHASES
            UpdateDynamicState(pipeline, is_indexed, prepared);
        }
        GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER (before the pass)
        scheduler.BeginRendering(state);
        if (SotcDrawMerge::On()) { // sotc: SOTC_DRAW_MERGE_STATS (measurement only)
            DrawMergeNote(state, pipeline, is_indexed, true, arg_address + offset, stride, max_count,
                          count_address, prepared);
        }
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        if (is_indexed) {
            const auto& a = args.indexed;
            CheckpointResources(GpuCheckpoint(cmdbuf, pipeline, SotcCheckpoints::Kind::DrawIndexed, a.indexCount, a.instanceCount,
                          a.firstInstance)); // sotc
            cmdbuf.drawIndexed(a.indexCount, a.instanceCount, a.firstIndex, a.vertexOffset,
                               a.firstInstance);
        } else {
            const auto& a = args.plain;
            CheckpointResources(GpuCheckpoint(cmdbuf, pipeline, SotcCheckpoints::Kind::Draw, a.vertexCount, a.instanceCount,
                          a.firstInstance)); // sotc
            cmdbuf.draw(a.vertexCount, a.instanceCount, a.firstVertex, a.firstInstance);
        }
        GcnAfterOp(*pipeline); // sotc: SOTC_GCN_BARRIER
        DebugState.IncDrawCall();
        {
            const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
            ResetBindings(false);
            FlushPeriodic();
        }
        return;
    }

    // sotc: SOTC_DRAW_MERGE_STATS=1. This path binds in full (a reused bind is bound here now), and no later draw
    // reuses its bind.
    if (merged) {
        const SotcTrace::ScopeMin trace_step{"bind resources"};
        if (!BindResources(pipeline, prepared)) {
            MergeInvalidate();
            SotcStats::Drop(1, liverpool->DrawRegs().color_buffers[0].Address(), 1);
            return;
        }
    }
    MergeInvalidate();

    // sotc: with SOTC_TRACE on: the indirect draws that keep their GPU arguments (first 80):
    // where the arguments are and what guest memory holds there (missing GPU particles).
    static const bool indirect_trace = SotcTrace::kOn; // sotc: SOTC_INDIRECT_TRACE is part of SOTC_TRACE
    if (indirect_trace) {
        static int shown = 0;
        if (shown++ < 80) {
            u32 first[4]{};
            const bool read = memory->TryReadBacking(arg_address + offset, first, sizeof(first));
            std::fprintf(stderr,
                         "SOTC indirect gpu %d read %d args %#llx+%#x stride %u max %u "
                         "count@%#llx mem %u %u %u %u vs %#llx\n",
                         int(buffer_cache.IsRegionGpuModified(arg_address + offset, stride)),
                         int(read), static_cast<unsigned long long>(arg_address), offset, stride,
                         max_count, static_cast<unsigned long long>(count_address), first[0],
                         first[1], first[2], first[3],
                         static_cast<unsigned long long>(
                             liverpool->DrawRegs().vs_program.Address<uintptr_t>()));
        }
    }
    const auto [buffer, base] = [&] { // sotc: SOTC_TRACE
        const SotcTrace::BindScope trace_step{"indirect args"};
        // sotc: SOTC_ADDR_VERTEX: from the page table when the arguments lie in one backing (ObtainRead).
        return SotcArch::AddrTable::ObtainRead(buffer_cache, scheduler, arg_address + offset,
                                               stride * max_count,
                                               SotcArch::AddrTable::VertexPath::Indirect);
    }();
    if (SotcArch::AddrTable::VertexOn()) { // sotc: SOTC_ADDR_VERTEX, ordering as for shader-addressed reads
        buffer_barrier_need |= GuestWriteOverlap(arg_address + offset, stride * max_count);
    }
    // sotc: SOTC_PASSES=1 and SOTC_BARRIER_STATIC=1 leave the overlap check to their rules (BufferChecks).
    buffer_barrier_need |= Passes::BufferNeed(BarrierStatic::BufferChecks() &&
                                              runtime.IsBufferAccessed(buffer, base, stride * max_count));

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = SotcArch::AddrTable::ObtainRead(
            buffer_cache, scheduler, count_address, 4, SotcArch::AddrTable::VertexPath::IndirectCount);
        if (SotcArch::AddrTable::VertexOn()) {
            buffer_barrier_need |= GuestWriteOverlap(count_address, 4);
        }
        buffer_barrier_need |= Passes::BufferNeed(BarrierStatic::BufferChecks() &&
                                                  runtime.IsBufferAccessed(count_buffer, count_offset, 4));
    }
    // sotc: the argument buffers can submit too.
    RebindVertexIfSubmitted(pipeline, vertex_tick, is_indexed,
                            [&] { BindIndexBuffer(0, index_extent); });

    if (DecideOpBarrier(true, StagesWriteMemory(*pipeline))) { // the indirect arguments are read by the draw
        runtime.FlushBarriers();
    }

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        if (!fast_bind_hit || fast_bind.tick != scheduler.CurrentTick()) {
            pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots, &draw_packet, &heap_block);
        }
    }
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDynamicState}; // sotc: SOTC_OP_PHASES
        UpdateDynamicState(pipeline, is_indexed, prepared);
    }
    GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER (before the pass)
    scheduler.BeginRendering(state);
    if (SotcDrawMerge::On()) { // sotc: SOTC_DRAW_MERGE_STATS (measurement only)
        DrawMergeNote(state, pipeline, is_indexed, true, arg_address + offset, stride, max_count,
                      count_address, prepared);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        CheckpointResources(GpuCheckpoint(cmdbuf, pipeline,
                      count_address != 0 ? SotcCheckpoints::Kind::DrawIndexedIndirectCount
                                         : SotcCheckpoints::Kind::DrawIndexedIndirect,
                      arg_address + offset, max_count, count_address != 0 ? count_address : stride)); // sotc
        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        CheckpointResources(GpuCheckpoint(cmdbuf, pipeline,
                      count_address != 0 ? SotcCheckpoints::Kind::DrawIndirectCount
                                         : SotcCheckpoints::Kind::DrawIndirect,
                      arg_address + offset, max_count, count_address != 0 ? count_address : stride)); // sotc
        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    GcnAfterOp(*pipeline); // sotc: SOTC_GCN_BARRIER
    // Record the indirect argument reads so a later write to the same range (e.g. a WRITE_DATA
    // packet uploading the next draw's arguments) waits for this draw instead of racing it.
    runtime.AccessBuffer(buffer, base, stride * max_count,
                         vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    if (count_address != 0) {
        runtime.AccessBuffer(count_buffer, count_offset, 4,
                             vk::PipelineStageFlagBits2::eDrawIndirect,
                             vk::AccessFlagBits2::eIndirectCommandRead);
    }

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(false);
        FlushPeriodic();
    }
}

// sotc: the two grass compute shaders (grass blades; taller weeds). Their flatbufs hold the blade
// height scale and 7 view distances that thin the grass out in bands.
struct Vegetation {
    u64 hash;
    u32 height_dword;
    u32 bands_dword;
};
constexpr std::array<Vegetation, 2> kVegetation{{{0xfeaef49e, 32, 53}, {0x287f5d44, 30, 35}}};
constexpr u32 kVegetationBands = 7;
// sotc: the grass patch compute picks each patch's blade LOD from floor(k * ln(scale * size / area));
// a smaller scale gives the patches near the camera their full blade count sooner.
constexpr u64 kGrassPatchHash = 0x6172cbb0;
constexpr u32 kGrassPatchLodDword = 30;

// SOTC_GRASS="<grass distance> <grass height> <weed distance> <weed height> <patch LOD scale>"
// multipliers (launcher); the in-game menu overrides them live (SotcLive::grass).
static std::array<float, 5> VegetationScales() {
    static const std::array<float, 5> env = [] {
        std::array<float, 5> s{1.f, 1.f, 1.f, 1.f, 1.f};
        if (const char* v = std::getenv("SOTC_GRASS")) {
            std::sscanf(v, "%f %f %f %f %f", &s[0], &s[1], &s[2], &s[3], &s[4]);
        }
        return s;
    }();
    std::array<float, 5> s = env;
    for (size_t i = 0; i < s.size(); ++i) {
        if (const float live = SotcLive::grass[i].load(std::memory_order_relaxed); live >= 0.f) {
            s[i] = live;
        }
    }
    return s;
}

// sotc: while DLSS takes the jittered frame, the game's TAA resolve runs
// with its history blend off: bit 0 of SRT dword 42. Its other work stays: exposure (the input
// colour times a scalar from a buffer) and the stencil mask in the output's alpha.
constexpr u64 kTaaResolveHash = 0xa402ddac;
constexpr u32 kTaaEnableDword = 42;
// sotc: the tonemap applies a screen space vignette: 1 + strength * (smoothstep(t) - 1),
// t = (scale * |uv - 0.5| - start) / (end - start), from flatbuf dwords 51..54. DLSS would drag
// it along with the scene (dark nested screen borders), so it is applied after upscaling.
constexpr u64 kTonemapHash = 0x291dbcd0;
constexpr u32 kVignetteDword = 51;

bool Rasterizer::TaaOff() {
    // TAA comes back if super resolution is switched or turns itself off
    return Dlssg::SuperResolution();
}

namespace {
// sotc: SOTC_WRITE_EXTENT / SOTC_FPS counter: the size of the direct dispatch whose resources are
// being bound (BindBuffers reads it). groups == 0: not a direct dispatch (graphics, indirect),
// nothing is narrowed there.
struct DirectDispatchScope {
    u64 hash{}, groups{}, threads{};
};
thread_local DirectDispatchScope t_direct_dispatch;
struct DirectDispatchGuard {
    ~DirectDispatchGuard() {
        t_direct_dispatch = {};
    }
};
} // namespace

// sotc: N5 async compute move (SOTC_N5_ASYNC_COMPUTE=1, impl-n5-asynccomp.md section 7).
struct AsyncDispatch {
    bool candidate{};          ///< AsyncBeforeOp said this dispatch may move
    AmdGpu::OpFootprint fp{};  ///< its footprint (unknown when AsyncBeforeOp did not run for it)
};

AsyncDispatch TakeAsync(bool& pre_seen, bool& candidate, AmdGpu::OpFootprint& fp) {
    AsyncDispatch out;
    if (pre_seen) {
        out.candidate = candidate;
        out.fp = std::move(fp);
    }
    pre_seen = false;
    candidate = false;
    fp = {};
    return out;
}

void Rasterizer::AsyncBeforeOp(const Vulkan::FrameGraph::Sample& s) {
    async_pre_seen = s.active && s.cls == Vulkan::FrameGraph::Class::Compute;
    async_candidate = false;
    if (!s.active) {
        return;
    }
    const Vulkan::FrameGraph::AsyncPre pre = Vulkan::FrameGraph::AsyncBefore(s);
    // A frame change joins into the tick being recorded (it ends the frame); other joins go to the
    // tick after the one flushed here, which holds the op.
    if (pre.frame_change) {
        scheduler.AsyncJoinAfter(pre.join);
    } else {
        scheduler.AsyncJoinBefore(pre.join);
    }
    async_candidate = pre.candidate;
    if (s.cls == Vulkan::FrameGraph::Class::Compute) {
        async_fp = s.fp;
        async_fp.known = s.known;
    }
}

void Rasterizer::DispatchDirect(const Pipeline* resolved, bool use_resolved,
                                const PreparedDraw* prepared) {
    RENDERER_TRACE;
    const VideoCore::TexProtectScope tex_protect_scope; // sotc: SOTC_TEX_PROTECT_BATCH, see Draw
    MdiBreak(); // sotc: SOTC_MDI_STATS
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDispatch}; // sotc: SOTC_OP_PHASES
    const AsyncDispatch async = TakeAsync(async_pre_seen, async_candidate, async_fp); // sotc: N5 async

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : [&] { // sotc: SOTC_OP_PHASES
                                                         const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline};
                                                         return pipeline_cache.GetComputePipeline();
                                                     }();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        return;
    }
    if (SotcGcnCensus::On()) { // sotc: SOTC_GCN_CENSUS (measurement only)
        CensusNote(pipeline, SotcGcnCensus::kDispatch);
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    // sotc: the LOD pre-shader variants (7b79db7c stateful, 6eb39c35 snapping) by code address: the CPU mirror
    // (cpu_cull_lod.cpp) tells them apart with it.
    if (cs.pgm_hash == 0x7b79db7cull || cs.pgm_hash == 0x6eb39c35ull) {
        SotcNativeCopies::NoteProgram(cs_program.Address<uintptr_t>(), cs.pgm_hash);
    }
    SotcStats::current_program.store(cs.pgm_hash, std::memory_order_relaxed);
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[4].fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: SOTC_CS_TRACE=<hash>: the buffers of the first 6 dispatches of that compute shader,
    // with the first dwords guest memory holds there (diagnostic for the GPU particle counts).
    static const u64 cs_trace = [] {
        const char* v = std::getenv("SOTC_CS_TRACE");
        return v ? std::strtoull(v, nullptr, 0) : u64{0};
    }();
    // SOTC_CS_WATCH=<n>: also report the GPU writes to that shader's buffer n (buffer_cache.cpp).
    static const int cs_watch = [] {
        const char* v = std::getenv("SOTC_CS_WATCH");
        return v ? std::atoi(v) : -1;
    }();
    if (cs_trace && cs.pgm_hash == cs_trace && cs_watch >= 0 &&
        SotcStats::watch_addr.load(std::memory_order_relaxed) == 0) {
        int n = 0;
        for (const auto& desc : cs.buffers) {
            if (!desc.IsSpecial() && n++ == cs_watch) {
                const VAddr a = desc.GetSharp(cs).base_address;
                SotcStats::watch_addr.store(a, std::memory_order_relaxed);
                std::fprintf(stderr, "SOTC cs watch armed at %#llx\n",
                             static_cast<unsigned long long>(a));
            }
        }
    }
    if (cs_trace && cs.pgm_hash == cs_trace) {
        static int shown = 0;
        if (shown++ < 6) {
            std::fprintf(stderr, "SOTC cs %#llx dispatch %ux%ux%u\n",
                         static_cast<unsigned long long>(cs.pgm_hash), cs_program.dim_x,
                         cs_program.dim_y, cs_program.dim_z);
            for (const auto& desc : cs.buffers) {
                if (desc.IsSpecial()) {
                    continue;
                }
                const auto sharp = desc.GetSharp(cs);
                u32 first[8]{};
                memory->TryReadBacking(sharp.base_address, first, sizeof(first));
                std::fprintf(stderr,
                             "SOTC cs   buf %#llx+%#llx written %d gpu %d mem %u %u %u %u %u %u %u "
                             "%u\n",
                             static_cast<unsigned long long>(sharp.base_address),
                             static_cast<unsigned long long>(sharp.GetSize()),
                             int(desc.is_written),
                             int(buffer_cache.IsRegionGpuModified(sharp.base_address, 32)),
                             first[0], first[1], first[2], first[3], first[4], first[5], first[6],
                             first[7]);
            }
        }
    }
    // SOTC_CMD_STATS=2: the dispatched shaders, every 10 s (command processor thread only).
    static const bool shader_stats = [] {
        const char* v = std::getenv("SOTC_CMD_STATS");
        return v && v[0] == '2';
    }();
    if (shader_stats) {
        static std::unordered_map<u64, u64> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[cs.pgm_hash];
        if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(10)) {
            last = now;
            std::vector<std::pair<u64, u64>> sorted;
            for (const auto& [hash, n] : counts) {
                sorted.emplace_back(n, hash);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::string line = "SOTC dispatches in 10 s:";
            for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
                line += fmt::format(" {:#x}={}", sorted[i].second, sorted[i].first);
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            counts.clear();
        }
    }
    bool hle_done;
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhHle}; // sotc: SOTC_OP_PHASES
        hle_done = ExecuteShaderHLE(cs, liverpool->DrawRegs(), cs_program, *this);
    }
    if (hle_done) {
        // sotc: N5 async: the HLE recorded graphics work, which waits for the moved dispatches it
        // conflicts with (the work is in the current tick: wait, then flush it).
        scheduler.AsyncJoinAfter(Vulkan::FrameGraph::AsyncConflicts(async.fp));
        Vulkan::FrameGraph::AsyncAfter(async.fp, false, 0);
        SotcStats::Drop(5, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    // sotc: dispatch size for BindBuffers (SOTC_WRITE_EXTENT, "SOTC GPU writes by shader"). A
    // non-zero start offset shifts the thread ids: no extent then (groups stays 0), counter only.
    const DirectDispatchGuard dispatch_guard;
    // sotc: the dispatch extent only feeds the write-extent narrowing and the write marks (kWriteMarks).
    if (SotcWriteExtent::kEnabled || SotcStats::kWriteMarks) {
        const u64 groups = u64{cs_program.dim_x} * cs_program.dim_y * cs_program.dim_z;
        const u64 local = u64{std::max<u32>(cs_program.num_thread_x.full, 1)} *
                          std::max<u32>(cs_program.num_thread_y.full, 1) *
                          std::max<u32>(cs_program.num_thread_z.full, 1);
        const bool plain = cs_program.start_x == 0 && cs_program.start_y == 0 &&
                           cs_program.start_z == 0 && groups != 0;
        t_direct_dispatch = {cs.pgm_hash, plain ? groups : 0, groups * local};
    }
    // sotc: SOTC_GCN_COMMIT (S6a). The prepared dispatch inputs, when the prep thread made them for this op.
    const PreparedDraw* dispatch_prepared = CheckPreparedDispatch(prepared, use_resolved, pipeline);
    // sotc: SOTC_CS_STATS (measurement only)
    u64 cs_key = 0;
    bool cs_key_ok = false, cs_writes = false, cs_res_ok = false;
    u64 cs_res_key = 0;
    u64 cs_gen = 0, cs_tick = 0;
    std::chrono::steady_clock::time_point cs_t0{};
    if (kCsStats.get()) {
        cs_key_ok = CsStatsKey(pipeline, cs_key, cs_writes);
        cs_res_ok = BindResourceHash(pipeline, cs_res_key);
        cs_tick = scheduler.CurrentTick();
        const u64 gens[4] = {BufferBindGeneration(), texture_cache.BindGeneration(),
                             buffer_cache.StreamGeneration(),
                             SotcStats::mem_epoch.load(std::memory_order_relaxed)};
        cs_gen = XXH3_64bits(gens, sizeof(gens));
        cs_t0 = std::chrono::steady_clock::now();
    }
    const bool cs_bound = BindResources(pipeline, dispatch_prepared);
    if (cs_bound) {
        PrepassProbeNoteBind(false, nullptr, pipeline, texture_cache, bound_images); // sotc: SOTC_PREPASS_PROBE
    }
    if (kCsStats.get()) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - cs_t0)
                            .count();
        u64 cs_desc = 0, cs_handle = 0;
        if (cs_bound) {
            CsDescKeys(pipeline, buffer_infos, image_infos, cs_desc, cs_handle);
        }
        CsStatsNote(cs_key_ok, cs_key, cs_res_ok, cs_res_key, cs_writes, cs_gen, cs_tick,
                    static_cast<u64>(ns), cs_bound, cs_desc, cs_handle);
    }
    if (!cs_bound) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    // sotc: N5 async move. A dispatch that may move starts the async command buffer here: the graphics
    // work so far is flushed first (its producers), and everything recorded until AsyncEnd, the
    // barriers, the bindings, the dispatch and its result copies, goes to the async queue. Otherwise
    // the recording waits for the moved dispatches it conflicts with.
    const u64 async_value =
        async.candidate && scheduler.HasAsyncQueue() && pipeline->IsReady() &&
                cs.images.empty()
            ? scheduler.AsyncBegin()
            : 0;
    if (async_value == 0) {
        scheduler.AsyncJoinBefore(Vulkan::FrameGraph::AsyncConflicts(async.fp));
    }
    if (DecideOpBarrier(!bound_buffers.empty() || !fixed_function_reads.empty() || shader_addr_touch,
                        StagesWriteMemory(*pipeline))) {
        runtime.FlushBarriers();
    }

    GcnEndRendering(scheduler);
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots, &draw_packet, &heap_block);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    CheckpointResources(GpuCheckpoint(cmdbuf, pipeline, SotcCheckpoints::Kind::Dispatch, cs_program.dim_x, cs_program.dim_y,
                  cs_program.dim_z)); // sotc
    GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER (outside a pass: the pending global barrier goes first)
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    GcnAfterOp(*pipeline); // sotc: SOTC_GCN_BARRIER
    CopyGpuResults(cmdbuf); // sotc: PC-native GPU result copies (native_copies.h)
    if (async_value != 0) {
        scheduler.AsyncEnd();
    }
    Vulkan::FrameGraph::AsyncAfter(async.fp, async_value != 0, async_value);
    DebugState.IncDispatch();
    MaybeDumpImages(cs.pgm_hash);

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(true);
        FlushPeriodic();
    }
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size, const Pipeline* resolved,
                                  bool use_resolved, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    const VideoCore::TexProtectScope tex_protect_scope; // sotc: SOTC_TEX_PROTECT_BATCH, see Draw
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDispatchIndirect}; // sotc: SOTC_OP_PHASES
    const AsyncDispatch async = TakeAsync(async_pre_seen, async_candidate, async_fp); // sotc: N5 async
    MdiBreak(); // sotc: SOTC_MDI_STATS
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[5].fetch_add(1, std::memory_order_relaxed);
    }

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : [&] { // sotc: SOTC_OP_PHASES
                                                         const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline};
                                                         return pipeline_cache.GetComputePipeline();
                                                     }();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        return;
    }
    if (SotcGcnCensus::On()) { // sotc: SOTC_GCN_CENSUS (measurement only)
        CensusNote(pipeline, SotcGcnCensus::kDispatchIndirect);
    }

    // sotc: SOTC_GCN_COMMIT (S6a). The prepared dispatch inputs, when the prep thread made them for this op.
    const PreparedDraw* dispatch_prepared = CheckPreparedDispatch(prepared, use_resolved, pipeline);
    // sotc: SOTC_CS_STATS (measurement only)
    u64 cs_key = 0;
    bool cs_key_ok = false, cs_writes = false, cs_res_ok = false;
    u64 cs_res_key = 0;
    u64 cs_gen = 0, cs_tick = 0;
    std::chrono::steady_clock::time_point cs_t0{};
    if (kCsStats.get()) {
        cs_key_ok = CsStatsKey(pipeline, cs_key, cs_writes);
        cs_res_ok = BindResourceHash(pipeline, cs_res_key);
        cs_tick = scheduler.CurrentTick();
        const u64 gens[4] = {BufferBindGeneration(), texture_cache.BindGeneration(),
                             buffer_cache.StreamGeneration(),
                             SotcStats::mem_epoch.load(std::memory_order_relaxed)};
        cs_gen = XXH3_64bits(gens, sizeof(gens));
        cs_t0 = std::chrono::steady_clock::now();
    }
    const bool cs_bound = BindResources(pipeline, dispatch_prepared);
    if (cs_bound) {
        PrepassProbeNoteBind(false, nullptr, pipeline, texture_cache, bound_images); // sotc: SOTC_PREPASS_PROBE
    }
    if (kCsStats.get()) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - cs_t0)
                            .count();
        u64 cs_desc = 0, cs_handle = 0;
        if (cs_bound) {
            CsDescKeys(pipeline, buffer_infos, image_infos, cs_desc, cs_handle);
        }
        CsStatsNote(cs_key_ok, cs_key, cs_res_ok, cs_res_key, cs_writes, cs_gen, cs_tick,
                    static_cast<u64>(ns), cs_bound, cs_desc, cs_handle);
    }
    if (!cs_bound) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 1);
        return;
    }

    // sotc: SOTC_ADDR_VERTEX: the arguments' buffer from the page table when they lie in one backing (ObtainRead).
    const auto [buffer, base] = SotcArch::AddrTable::ObtainRead(
        buffer_cache, scheduler, address + offset, size, SotcArch::AddrTable::VertexPath::DispatchIndirect);
    if (SotcArch::AddrTable::VertexOn()) { // sotc: SOTC_ADDR_VERTEX, ordering as for shader-addressed reads
        buffer_barrier_need |= GuestWriteOverlap(address + offset, size);
    }
    // sotc: SOTC_PASSES=1 and SOTC_BARRIER_STATIC=1 leave the overlap check to their rules (BufferChecks).
    buffer_barrier_need |=
        Passes::BufferNeed(BarrierStatic::BufferChecks() && runtime.IsBufferAccessed(buffer, base, size));

    // sotc: N5 async move, see DispatchDirect.
    const u64 async_value =
        async.candidate && scheduler.HasAsyncQueue() && pipeline->IsReady() &&
                pipeline->GetStage(Shader::SwStage::Compute).images.empty()
            ? scheduler.AsyncBegin()
            : 0;
    if (async_value == 0) {
        scheduler.AsyncJoinBefore(Vulkan::FrameGraph::AsyncConflicts(async.fp));
    }
    if (DecideOpBarrier(true, StagesWriteMemory(*pipeline))) { // the indirect arguments are read by the dispatch
        runtime.FlushBarriers();
    }

    GcnEndRendering(scheduler);
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots, &draw_packet, &heap_block);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    CheckpointResources(GpuCheckpoint(cmdbuf, pipeline, SotcCheckpoints::Kind::DispatchIndirect, address, offset, 0)); // sotc
    GcnBeforeOp(scheduler); // sotc: SOTC_GCN_BARRIER
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    GcnAfterOp(*pipeline); // sotc: SOTC_GCN_BARRIER
    CopyGpuResults(cmdbuf); // sotc: PC-native GPU result copies (native_copies.h)
    if (async_value != 0) {
        scheduler.AsyncEnd();
    }
    Vulkan::FrameGraph::AsyncAfter(async.fp, async_value != 0, async_value);
    DebugState.IncDispatch();
    runtime.AccessBuffer(buffer, base, size, vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    MaybeDumpImages(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(true);
        FlushPeriodic();
    }
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    SotcGuestMemory::Ready();
    SotcLodFeedbackCycle();
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

void Rasterizer::OnFence() {
    ++VideoCore::g_sotc_fence_epoch;
    texture_cache.ProcessDownloadImages();
}

// sotc: the descriptors take each image's layout when its texture is bound, but BeginRendering may
// move a sampled image to another layout afterwards: a depth buffer both sampled and bound with
// stencil writes goes to DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL while its descriptor still said
// DEPTH_STENCIL_READ_ONLY_OPTIMAL (Vulkan validation; harmless on NVIDIA, wrong on GPUs that
// compress depth by layout). The descriptors are written after this, with the layouts in force.
void Rasterizer::RefreshImageLayouts() {
    for (size_t i = 0; i < image_infos.size(); ++i) {
        if (const VideoCore::ImageId id = image_info_images[i]) {
            image_infos[i].imageLayout =
                SotcArch::Layouts::Pick(texture_cache.GetImage(id).backing->state.layout); // sotc: A1
        }
    }
}

bool Rasterizer::FastBindEligible(const Pipeline* pipeline) const {
    return !attachment_feedback_loop && FastBindStatic(pipeline, liverpool->DrawRegs());
}

bool Rasterizer::FastBindStatic(const Pipeline* pipeline, const AmdGpu::Regs& regs) const {
    // sotc: FastBindEligible without its draw-thread input (attachment_feedback_loop): the
    // registers and the pipeline only, so SOTC_PARALLEL_RECORD's prep threads can run it.
    if (!SotcStats::kFastBind || pipeline->IsCompute()) {
        return false;
    }
    if (regs.clipper_control.user_clip_plane_enable != 0) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // sotc: the bindless slots are set in the push data of the slow path only.
        if (IsBindlessStage(instance, *stage)) {
            return false;
        }
        // sotc: SOTC_N5_BINDLESS: the image slots likewise. A fast bind keeps slots that a later
        // acquire may evict, and it does not mark them used.
        if (Shader::UsesBindlessImages(*stage)) {
            return false;
        }
        if (stage->has_storage_images || !stage->fmasks.empty()) {
            return false;
        }
        for (const auto& desc : stage->buffers) {
            // The stream-buffer bindings are only reusable while the ring has not wrapped;
            // StreamGeneration() in the fast-bind state checks that. Shared memory is compute.
            if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                return false;
            }
            if (!desc.IsSpecial() && desc.is_written) {
                return false;
            }
        }
        for (const auto& desc : stage->images) {
            if (desc.is_written) {
                return false;
            }
        }
    }
    return true;
}

u64 Rasterizer::FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data) const {
    return FastBindHash(pipeline, push_data, liverpool->DrawRegs());
}

u64 Rasterizer::FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data,
                             const AmdGpu::Regs& regs) const {
    // sotc: the user data and flat buffers are this thread's (Shader::g_info_slot).
    u64 hash = XXH3_64bits(&pipeline, sizeof(pipeline));
    hash = XXH3_64bits_withSeed(&push_data, sizeof(push_data), hash);
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const VAddr address = regs.color_buffers[cb].Address();
        hash = XXH3_64bits_withSeed(&address, sizeof(address), hash);
    }
    const VAddr depth_address = regs.depth_buffer.DepthAddress();
    hash = XXH3_64bits_withSeed(&depth_address, sizeof(depth_address), hash);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        const auto user_data = stage->UserData();
        if (!user_data.empty()) {
            hash = XXH3_64bits_withSeed(user_data.data(), user_data.size_bytes(), hash);
        }
        const auto& flat_buf = stage->FlatBuf();
        if (!flat_buf.empty()) {
            hash = XXH3_64bits_withSeed(flat_buf.data(), flat_buf.size() * sizeof(u32), hash);
        }
    }
    return hash;
}

namespace {
/// sotc: SOTC_BIND_CACHE_FRAME. A stage's identity for the bind cache key: its shader and the
/// descriptor layout (kinds and order) a cached entry's writes name.
u64 BindCacheStageIdentity(const Shader::Info& stage) {
    const u64 words[3] = {stage.CacheHash(), // sotc: a variant's layout differs (SOTC_HEAP)
                          (u64{stage.buffers.size()} << 40) | (u64{stage.images.size()} << 20) |
                              u64{stage.samplers.size()},
                          static_cast<u64>(stage.sw_stage)};
    u64 ident = XXH3_64bits(words, sizeof(words));
    u64 layout = 0;
    for (const auto& b : stage.buffers) {
        layout = layout * 0x100000001B3ULL + ((u64{static_cast<u8>(b.buffer_type)} << 2) |
                                              (u64{b.is_written} << 1) | u64{b.is_formatted});
    }
    for (const auto& i : stage.images) {
        layout = layout * 0x100000001B3ULL +
                 (u64{i.is_depth} | (u64{i.is_array} << 1) | (u64{i.is_written} << 2) |
                  (u64{i.array_size} << 3) | (u64{i.is_r128} << 20) |
                  (u64{static_cast<u16>(i.mip_fallback_mode)} << 24));
    }
    for (const auto& smp : stage.samplers) {
        layout = layout * 0x100000001B3ULL + u64{smp.is_depth} + 7;
    }
    return XXH3_64bits_withSeed(&layout, sizeof(layout), ident);
}
} // namespace

bool Rasterizer::BindResourceHash(const Pipeline* pipeline, u64& out) const {
    // sotc: SOTC_BIND_CACHE_FRAME. What the descriptors of a bind are made of: the pipeline, the
    // T#/V#/S# words each stage reads (GetSharp: the flat buffer words located by the stage's
    // sharp fetches, with the post operations, exactly what BindBuffers / BindTextures use) and
    // the stage identity. The rest of the flat buffer (user data registers, SRT pointers and
    // values: constants the shader reads from its stream-ring copy, which a hit re-copies) and
    // the viewport words of the push data are not in the key. A pointer the shader dereferences
    // by itself (DMA), a table or an array it indexes at run time are resources chosen by
    // constants: those pipelines return false and keep the full key.
    u64 hash = 0x4C32525300000001ULL;
    hash = XXH3_64bits_withSeed(&pipeline, sizeof(pipeline), hash);
    // sotc: SOTC_BIND_CACHE_PACKET: the shader-addressed bases are left out of the key of a packet op (see below).
    const bool addr_key = SotcStats::kBindCachePacket != 0 && pipeline->HasPacketRange() &&
                          pipeline->GetProfile().shader_addr == 1;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->uses_dma || !stage->sharp_tables.empty() || !stage->fmasks.empty() ||
            stage->has_storage_images || stage->UserData().data() == nullptr) {
            return false;
        }
        for (const auto& image : stage->images) {
            if (image.array_size != 0) {
                return false;
            }
        }
        const u64 ident = BindCacheStageIdentity(*stage);
        hash = XXH3_64bits_withSeed(&ident, sizeof(ident), hash);
        for (const auto& desc : stage->buffers) {
            if (!desc.IsSpecial()) {
                auto sharp = desc.GetSharp(*stage);
                // sotc: SOTC_BIND_CACHE_PACKET (mode 1/2, packet ops, shader address profile 1). A shader-addressed V#
                // has no descriptor (BindBuffers writes none for it) and the shader reads its base from the flat buffer
                // copy, which a hit re-copies; its range is recomputed by PacketAddrRefresh. So the base is not a key.
                // Soundness: the list in the bind cache notes, the base's uses on the host path.
                if (addr_key && Shader::ShaderAddrBufferAt(pipeline->GetProfile(), *stage,
                                                           static_cast<u32>(&desc - stage->buffers.data()))) {
                    sharp.base_address = 0;
                }
                hash = XXH3_64bits_withSeed(&sharp, sizeof(sharp), hash);
            }
        }
        for (const auto& image : stage->images) {
            const auto sharp = image.GetSharp(*stage);
            hash = XXH3_64bits_withSeed(&sharp, sizeof(sharp), hash);
        }
        for (const auto& sampler : stage->samplers) {
            const auto sharp = sampler.GetSharp(*stage);
            hash = XXH3_64bits_withSeed(&sharp, sizeof(sharp), hash);
        }
    }
    out = hash;
    return true;
}

void Rasterizer::CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma) {
    // sotc: SOTC_DESCRIPTOR_HEAP: a heap draw's bind is not replayed (its ring slots are per draw).
    if (pipeline->UsesDescriptorHeap()) {
        fast_bind.valid = false;
        return;
    }
    fast_bind.addr.assign(packet_addr_rec.begin(), packet_addr_rec.end()); // sotc: SOTC_PACKET (empty otherwise)
    fast_bind.pipeline = pipeline;
    fast_bind.hash = hash;
    fast_bind.buffer_generation = BufferBindGeneration(); // sotc: BindVersion with BIND_CACHE_XTICK
    fast_bind.texture_generation = texture_cache.BindGeneration();
    fast_bind.stream_generation = buffer_cache.StreamGeneration();
    fast_bind.mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed);
    fast_bind.tick = scheduler.CurrentTick();
    fast_bind.uses_dma = uses_dma;
    fast_bind.valid = true;
    fast_bind.compute_cleared = false;
}

// sotc: SOTC_PR_RESOLVE (default off; acts only with SOTC_PARALLEL_RECORD, since the prepared
// data is produced on the GpuPrep threads). Resource inputs of a graphics draw, resolved ahead of
// the draw thread: see PreparedResources (draw_prep.h).
static bool SotcPrResolveOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PR_RESOLVE");
        return v && v[0] == '1';
    }();
    return on;
}

// sotc: SOTC_PR_GEN (default 0; 1 use the prepared values while their generation sums hold, 2 also
// run the inline path and count disagreements). Needs SOTC_PARALLEL_RECORD=1 (the prepared data is
// produced on the GpuPrep threads) and the generation API (sotc_generation.h) on: without it the
// sums are meaningless, so the mode is refused. Replaces the runtime-table generation as the
// per-range validity test; the table generation still gates as well (conservative).
static u32 SotcPrGenMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_PR_GEN");
        const u32 m = v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
        if (m == 0) {
            return 0u;
        }
        if (m > 2 || !SotcGen::On()) {
            std::fprintf(stderr, "SOTC_PR_GEN: off, %s\n",
                         m > 2 ? "the mode is 1 or 2" : "the generation API is not on");
            return 0u;
        }
        return m;
    }();
    return mode;
}

// sotc: SOTC_PR_GEN. A descriptor range longer than this is not prepared (the owner computes it):
// the draw thread's sum check walks every granule of the range.
static constexpr u64 kPrGenMaxSpan = 64ULL << 20;

// sotc: SOTC_PR_RESOLVE. FlatForBind rewrites the flat buffer of these stages (TAA resolve, tonemap
// vignette, vegetation scales, grass patch LOD), so their V#s and T#s are read inline. Keep in sync
// with FlatForBind; SOTC_PR_VERIFY recomputes the prepared values and catches a drift.
static bool FlatMayBePatched(const Shader::Info& s) {
    return s.pgm_hash == kTaaResolveHash || s.pgm_hash == kTonemapHash ||
           s.pgm_hash == kVegetation[0].hash || s.pgm_hash == kVegetation[1].hash ||
           s.pgm_hash == kGrassPatchHash;
}

// sotc: SOTC_PR_RESOLVE. Stages the prepared path leaves to the draw thread: DMA pointers, sharp
// tables, fmasks, storage images, descriptor arrays, and stages FlatForBind may rewrite.
static bool PrStageResolvable(const Shader::Info& s) {
    if (s.uses_dma || !s.sharp_tables.empty() || !s.fmasks.empty() || s.has_storage_images ||
        FlatMayBePatched(s)) {
        return false;
    }
    for (const auto& image : s.images) {
        if (image.array_size != 0) {
            return false;
        }
    }
    return true;
}

// sotc: SOTC_PR_KEYS (default on; acts with SOTC_PARALLEL_RECORD and SOTC_HEAP). The heap table keys of the
// prepared draw are computed for packet pipelines only, as BindResources resolves the heap stages there.
static bool SotcPrKeysOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PR_KEYS");
        return !v || v[0] != '0';
    }();
    return on;
}

// sotc: SOTC_PR_PRED (default 0; 1 BindBuffers takes the shader-addressed predicate from the prepared buffers
// (PrBuffer::shader_addressed) for prepared stages). Acts only with SOTC_PR_RESOLVE, which holds those buffers.
static bool SotcPrPredOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PR_PRED");
        return v && v[0] == '1';
    }();
    return on;
}

// sotc: SOTC_PR_KEYS. The heap stages of a pipeline, as BindResources finds them: out[0] the fragment-kind heap
// stage, out[1] the vertex-kind one (SOTC_HEAP_VS). False when a kind has two stages (the draw is dropped there):
// nothing is precomputed for it. Reads the pipeline and its profile only.
static bool PrHeapStages(const Pipeline& pipeline, const Shader::Info* (&out)[2]) {
    out[0] = out[1] = nullptr;
    for (const auto* stage : pipeline.GetStages()) {
        if (stage && Shader::Backend::SPIRV::UsesHeapImages(*stage, pipeline.GetProfile())) {
            const bool vertex = stage->sw_stage == Shader::SwStage::Vertex;
            const Shader::Info*& kind = out[vertex ? 1 : 0];
            if (kind) {
                return false;
            }
            kind = stage;
        }
    }
    return true;
}

// sotc: SOTC_PR_RESOLVE. The values BindBuffers computes for a guest buffer binding (its clamp to
// the mapping and its unbounded-window limit). The window warning stays with the inline path.
// `profile` is the pipeline's (not the thread's bind profile): the shader-addressed predicate is
// Shader::ShaderAddrBufferAt on it, the same call BindBuffers makes on the draw thread.
static PrBuffer PrResolveBuffer(const Shader::Info& stage, const Shader::BufferResource& desc,
                                Core::MemoryManager* mem, const Shader::Profile& profile) {
    PrBuffer out{};
    // sotc: SOTC_PR_PRED: BindBuffers' shader_addressed lambda (ShaderAddrBufferAt, null profile when
    // shader_addr is 0). Taken before the special check, so a special buffer's bit is the lambda's too.
    if (profile.shader_addr != 0) {
        out.shader_addressed = Shader::ShaderAddrBufferAt(
            profile, stage, static_cast<u32>(&desc - stage.buffers.data()));
    }
    if (desc.IsSpecial()) {
        return out;
    }
    const auto vsharp = desc.GetSharp(stage);
    out.base = vsharp.base_address;
    out.vsize = vsharp.GetSize();
    if (SotcPrGenMode() != 0) {
        // sotc: SOTC_PR_GEN: the sum is read before the mapping read below, so a change that lands
        // after it makes the draw thread's sum differ. Over-long ranges stay with the owner.
        if (out.vsize > kPrGenMaxSpan) {
            return out;
        }
        out.gen_sum = SotcGen::Sum(out.base, out.vsize);
    }
    u64 size = 0;
    if (out.base != 0 && out.vsize != 0) {
        size = mem->ClampRangeSize(out.base, out.vsize);
    }
    out.mapped = size;
    // The same limit as BindBuffers (SHADPS4_UNBOUNDED_BUFFER_MB, 64 MB by default).
    static const u64 unbounded_window = [] {
        const char* v = std::getenv("SHADPS4_UNBOUNDED_BUFFER_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 64ULL) << 20;
    }();
    constexpr u64 MaxSaneBindingSize = 256ULL << 20;
    if (!desc.is_written && (out.vsize >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
        unbounded_window != 0 && size > unbounded_window) {
        size = unbounded_window;
    }
    // sotc: SOTC_READ_EXTENT, as BindBuffers.
    if (SotcReadExtent::kEnabled && !desc.is_written) {
        size = SotcReadExtent::Limit(stage.pgm_hash, static_cast<u32>(&desc - stage.buffers.data()), size);
    }
    // sotc: SOTC_SHADER_EXTENT, as BindBuffers.
    if (SotcShaderExtent::kEnabled) {
        size = SotcShaderExtent::Limit(desc.is_written, desc.sotc_read_extent, out.vsize, size);
    }
    // sotc: SOTC_BLOCK_EXTENT, as BindBuffers.
    if (!desc.is_written && out.vsize >= 0xFFFF0000ULL && SotcProbes::BlockExtentOn()) {
        if (u64 remaining = 0; SotcProbes::BlockRemaining(out.base, &remaining) && remaining < size) {
            size = remaining;
        }
    }
    out.size = size;
    out.computed = true;
    return out;
}

// sotc: SOTC_PR_RESOLVE. The T# and the validity verdict BindTextures makes on it (non-array).
static PrImage PrResolveImage(const Shader::Info& stage, const Shader::ImageResource& image,
                              Core::MemoryManager* mem) {
    PrImage out{};
    const u32 num_bindings = image.NumBindings(stage);
    if (num_bindings > 0xFF) {
        return out; // verdict 0: BindTextures resolves it inline
    }
    out.tsharp = image.GetSharp(stage);
    out.num_bindings = static_cast<u8>(num_bindings);
    const auto data_fmt = out.tsharp.GetDataFmt();
    const auto num_fmt = out.tsharp.GetNumberFmt();
    if (out.tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
        out.verdict = 2;
        return out;
    }
    // sotc: SOTC_PR_GEN: the sum is read before the mapping check (verdicts 1 and 3 use it).
    if (SotcPrGenMode() != 0) {
        out.gen_sum = SotcGen::Sum(out.tsharp.Address(), 1);
    }
    if (!mem->IsValidGpuMapping(out.tsharp.Address(), 0) ||
        !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
        out.verdict = 3;
    } else {
        out.verdict = 1;
    }
    return out;
}

// sotc: SOTC_PR_RESOLVE. Equality of the resolved values (SOTC_PR_VERIFY); runtime_gen is not
// compared.
static bool SamePreparedResources(const PreparedResources& a, const PreparedResources& b) {
    if (a.num_buffers != b.num_buffers || a.num_images != b.num_images) {
        return false;
    }
    for (size_t s = 0; s < a.stages.size(); ++s) {
        if (a.stages[s].ready != b.stages[s].ready ||
            a.stages[s].buf_first != b.stages[s].buf_first ||
            a.stages[s].img_first != b.stages[s].img_first) {
            return false;
        }
    }
    for (u32 i = 0; i < a.num_buffers; ++i) {
        const auto& x = a.buffers[i];
        const auto& y = b.buffers[i];
        if (x.base != y.base || x.vsize != y.vsize || x.mapped != y.mapped || x.size != y.size ||
            x.computed != y.computed || x.shader_addressed != y.shader_addressed) {
            return false;
        }
    }
    for (u32 i = 0; i < a.num_images; ++i) {
        const auto& x = a.images[i];
        const auto& y = b.images[i];
        if (std::memcmp(&x.tsharp, &y.tsharp, sizeof(x.tsharp)) != 0 ||
            x.num_bindings != y.num_bindings || x.verdict != y.verdict) {
            return false;
        }
    }
    return true;
}

// sotc: SOTC_PR_GEN. True when every range the prep thread recorded still has the sum it read, now
// (the same arguments; a descriptor the prep thread did not compute has no sum to check).
static bool PrGenSumsUnchanged(const PreparedResources& pr) {
    for (u32 i = 0; i < pr.num_buffers; ++i) {
        const PrBuffer& b = pr.buffers[i];
        if (b.computed && SotcGen::Sum(b.base, b.vsize) != b.gen_sum) {
            return false;
        }
    }
    for (u32 i = 0; i < pr.num_images; ++i) {
        const PrImage& im = pr.images[i];
        if ((im.verdict == 1 || im.verdict == 3) &&
            SotcGen::Sum(im.tsharp.Address(), 1) != im.gen_sum) {
            return false;
        }
    }
    return true;
}

// sotc: SOTC_PR_GEN counters (draw thread only: BindResources). hits: the prepared values were used.
// needs_owner: the draw resolved inline because a sum moved or the table generation did (table_gen
// counts the second case). mismatches: SOTC_PR_GEN=2 only, the prepared values were valid by sum but
// differ from the owner's computation. Printed every 5 s while the mode is on.
namespace {
struct PrGenState {
    u64 hits{}, needs_owner{}, table_gen{}, mismatches{};
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
PrGenState g_pr_gen;
void PrGenNote(u32 mode, bool hit, bool needs_owner, bool table_gen, bool mismatch) {
    auto& s = g_pr_gen;
    s.hits += hit;
    s.needs_owner += needs_owner;
    s.table_gen += table_gen;
    s.mismatches += mismatch;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last < std::chrono::seconds(5)) {
        return;
    }
    std::fprintf(stderr,
                 "SOTC PR GEN (mode %u): prepared hits %llu, needs-owner %llu (table generation "
                 "%llu), verify mismatches %llu\n",
                 mode, static_cast<unsigned long long>(s.hits),
                 static_cast<unsigned long long>(s.needs_owner),
                 static_cast<unsigned long long>(s.table_gen),
                 static_cast<unsigned long long>(s.mismatches));
    s = PrGenState{};
    s.last = now;
}
} // namespace

// sotc: SOTC_VERIFY turns this on (off by default; a measurement). For every fast-bind-eligible draw that
// misses the fast bind, which of its conditions failed, as a bit mask: the 6-8 most frequent masks
// are printed every 5 s. "epoch" alone means nothing the draw binds changed, only the global
// mem_epoch counter that any write anywhere bumps (memory_tracker.h, NoteHostWrite).
namespace {
// sotc: SOTC_BIND_DIFF is part of SOTC_VERIFY (verify.h): on with SOTC_VERIFY=1.
const bool kBindDiff = SotcVerify::VerifyAll();
enum BindDiffBit : u32 {
    BdInvalid = 1, BdPipeline = 2, BdBufGen = 4, BdTexGen = 8, BdStreamGen = 16, BdEpoch = 32,
    BdHash = 64, BdTick = 128,
};
struct BindDiffState {
    u64 counts[256]{};
    u64 eligible = 0, hits = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
BindDiffState g_bind_diff; // the draw thread only
void BindDiffNote(u32 mask, bool hit) {
    auto& d = g_bind_diff;
    ++d.eligible;
    if (hit) {
        ++d.hits;
    } else {
        ++d.counts[mask & 255];
    }
    if ((d.eligible & 1023) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - d.last < std::chrono::seconds(5)) {
        return;
    }
    d.last = now;
    u64 misses = 0;
    for (u64 c : d.counts) {
        misses += c;
    }
    std::string line = "SOTC BIND DIFF: eligible " + std::to_string(d.eligible) + ", hits " +
                       std::to_string(d.hits) + ", misses " + std::to_string(misses) + ":";
    for (int k = 0; k < 8; ++k) {
        u32 best = 0;
        for (u32 m = 1; m < 256; ++m) {
            if (d.counts[m] > d.counts[best]) {
                best = m;
            }
        }
        if (d.counts[best] == 0) {
            break;
        }
        std::string name;
        static constexpr const char* kNames[8] = {"invalid", "pipeline", "bufgen", "texgen",
                                                  "streamgen", "epoch", "hash", "tick"};
        for (int bit = 0; bit < 8; ++bit) {
            if (best & (1u << bit)) {
                name += (name.empty() ? "" : "+");
                name += kNames[bit];
            }
        }
        char item[96];
        std::snprintf(item, sizeof(item), " %s %.1f%%", name.c_str(),
                      misses ? 100.0 * double(d.counts[best]) / double(misses) : 0.0);
        line += item;
        d.counts[best] = 0;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    const auto last = d.last;
    d = BindDiffState{};
    d.last = last;
}
} // namespace

// sotc: defined with the frame bind cache below (same unnamed namespace).
namespace {
u64 BindCacheFrameKey(const Pipeline* pipeline, u64 hash, u64 targets_key);
}

bool Rasterizer::BindResources(const Pipeline* pipeline, const PreparedDraw* prepared) {
    // sotc: SOTC_SHADER_ADDR / SOTC_HEAP: the pipeline's profile, read by BindBuffers (ShaderAddrBuffer) below.
    SotcArch::ShaderAddr::SetBindProfile(&pipeline->GetProfile());
    if (SotcStats::kFpsLog && SotcStats::kBindCache) { // sotc: SOTC_FPS bind eligibility (sotc_stats.h)
        SotcStats::bind_ops.fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: SOTC_OPSET_STATS: this call is one op of the draw or dispatch (keys from its inputs, see OpSetStats).
    if (OpSetStats::On()) {
        const bool draw = !pipeline->IsCompute();
        OpSetStats::Note(pipeline, draw, draw ? OpSetStats::TargetKey(liverpool->DrawRegs()) : 0);
    }
    // sotc: SOTC_SYNC_PIPE counters: this call is one op (its Protect calls are counted).
    const SotcStats::SyncPipe::OpScope sync_pipe_op;
    const SotcOpPhases::Scope ph{SotcOpPhases::PhBindResources}; // sotc: SOTC_OP_PHASES
    // sotc: writes recorded during this bind follow its command into the tick it lands in.
    buffer_cache.BeginWriteScope();
    struct WriteScopeGuard {
        VideoCore::BufferCache& cache;
        ~WriteScopeGuard() {
            cache.EndWriteScope();
        }
    } write_scope_guard{buffer_cache};
    const SotcStats::SplitTimer bind_split{SotcStats::SplitBind}; // sotc: SOTC_DRAW_SPLIT
    fast_bind_hit = false;
    // sotc: SOTC_DESCRIPTOR_HEAP: a heap draw replaces the descriptor state of the command buffer, so a classic draw
    // after it must not replay an older fast bind.
    if (pipeline->UsesDescriptorHeap()) {
        fast_bind.valid = false;
    }
    bind_bindless_rec.clear();                // sotc: SOTC_N5_BINDLESS (refs of this bind)
    bind_bindless_gen = BindlessGeneration(); // sotc: a stored bind is replayed only while it holds
    // sotc: SOTC_GCN_COMMIT (S6b). A commit-eligible op (its buffers and textures are served by the GCN tables, see
    // SotcGcn::Commit::Eligible) has nothing left for the fast bind and the bind caches to save: it skips them, lookup and
    // store, and binds directly. Mode 2 keeps the caches and only counts the ops the lane would take.
    const bool commit_on = SotcGcn::Commit::On();
    const bool commit_lane = commit_on && SotcGcn::Commit::Eligible(*pipeline);
    const bool commit_bypass = commit_lane && !SotcGcn::Commit::Verify();
    if (commit_on) {
        SotcGcn::Commit::NoteOp(commit_lane, pipeline->IsCompute());
        if (commit_bypass) {
            SotcGcn::Commit::NoteBypass();
        }
    }
    // sotc: the register-only inputs below (push data, hashes, eligibility, heap keys) come from the slot, for a draw
    // (PrepareDraw) and a dispatch (PrepareDispatch, SOTC_GCN_COMMIT) alike. CheckPreparedDispatch passes a dispatch's
    // slot as it is in mode 1 (as a draw's); SOTC_VERIFY=1 passes it only when it matches the inline values.
    const PreparedDraw* reg_prepared = prepared;
    // sotc: SOTC_PACKET (ARCH-DRAW section 12). A pipeline with the packet range reads its window words and its index
    // table from this op's packet, so its binds take no bind cache and no fast bind (their replays would push the
    // addresses of an earlier tick). The arena window is 0: the ring-8 runs are many (SotcGuestMemory keeps a map of
    // runs), one window cannot describe them, so every shader-addressed range goes through the mirror.
    const bool packet_op = pipeline->HasPacketRange();
    // sotc: the slot words are filled (and pushed, Pipeline::BindResources) only for a pipeline that has them.
    slot_words_op = pipeline->HasSlotWords();
    // sotc: SOTC_HEAP_GPU_VERIFY (heap profile 2) fills the slot words of a heap pipeline too: the verify compares them.
    slot_words_fill = slot_words_op || pipeline->GetProfile().heap_images == 2;
    shader_addr_touch = false;
    sotc_addr_hazard = false;
    sotc_op_writes.clear();
    sotc_addressed_writes.clear(); // sotc: SOTC_SHADER_ADDR, mirror coherence: this op's addressed writes only
    packet_addr_rec.clear();       // sotc: SOTC_PACKET: this op's shader-addressed buffers (BindBuffers)
    packet_addr_src_rec.clear();   // sotc: SOTC_BIND_CACHE_PACKET: where those came from (for the bind cache entry)
    // sotc: SOTC_PACKET. The barrier rule of the end of this function (for the replays that return early).
    const auto packet_barrier = [&] {
        if (SotcArch::ShaderAddr::On()) {
            if (sotc_addr_hazard && BarrierStatic::BufferChecks()) {
                buffer_barrier_need = true;
            }
            // sotc: SOTC_BARRIER_STATIC=1 decides the barrier later (DecideOpBarrier): the op's writes are tagged as
            // recorded after it (conservative: a later flush may pass them once more).
            MergeOpWrites(BarrierStatic::On() || needs_barrier || buffer_barrier_need);
        }
    };
    if (packet_op) {
        // sotc: SOTC_ADDR_TABLE: the mirror words name the guest page table (same offsets): its window, its device
        // address and its size (addr_table.h). The shader reads the table through them (TableAddresses).
        if (SotcArch::AddrTable::On()) {
            draw_packet.mirror_guest_base = SotcArch::AddrTable::WindowBase;
            draw_packet.mirror_device_base = SotcArch::AddrTable::Base();
            draw_packet.mirror_size = SotcArch::AddrTable::WindowSize;
        } else {
            const auto windows = SotcArch::Mirror::Windows();
            draw_packet.mirror_guest_base = windows.empty() ? 0 : windows.front().guest_base;
            draw_packet.mirror_device_base = windows.empty() ? 0 : windows.front().device_base;
            draw_packet.mirror_size = windows.empty() ? 0 : windows.front().size;
        }
        // sotc: ARCH-GCN-NATIVE (D6). The globals buffer's device address rides in the arena_guest_base word (push
        // offset 88) of the gcn pipelines; arena_size stays 0 (the arena window is empty, in_arena tests fail). The
        // packet is pushed whole for every packet pipeline, heap stages included (BindResources). Verify (=2) writes it
        // too: its pipelines read the globals as well.
        // sotc: the pipeline's own profile, not the live ProfileValue(): a pipeline compiled with gcn_buf/gcn_tex keeps
        // reading the globals even after a refusal lowered the stage (its shader would otherwise read address 0).
        const bool gcn_packet = pipeline->GetProfile().gcn_buf != 0 || pipeline->GetProfile().gcn_tex != 0;
        draw_packet.arena_guest_base = gcn_packet ? SotcGcn::Globals::Address() : 0;
        if (gcn_packet) { // sotc: diagnostic (stderr, once): the globals address the first gcn packet carries.
            static std::atomic<bool> printed_globals{};
            if (!printed_globals.exchange(true)) {
                std::fprintf(stderr, "SOTC GCN PACKET: word 88 (globals) = %#llx%s\n",
                             static_cast<unsigned long long>(draw_packet.arena_guest_base),
                             draw_packet.arena_guest_base == 0 ? " (no globals: miss stores would go to address 0)" : "");
            }
        }
        draw_packet.arena_device_base = 0;
        draw_packet.arena_size = 0;
    }
    // sotc: SOTC_HEAP. The heap stage of the pipeline (UsesHeapImages) takes its heap indices from this op's index
    // table. The packet has one table address, so a pipeline with two heap stages is refused (counted, logged).
    // A table that the memo refuses (device address 0) is refused too: the op is dropped, not drawn from address 0.
    const Shader::Info* heap_stage = nullptr;
    // sotc: SOTC_HEAP_VS: the vertex heap stage of the pipeline (its table goes to the heap block, not the packet).
    const Shader::Info* heap_vs_stage = nullptr;
    // sotc: SOTC_HEAP real mode (profile 1): the heap stage's images are bound from its index table (BindHeapImages).
    SotcArch::TableMemo::TableBinds heap_binds{};
    SotcArch::TableMemo::TableBinds heap_vs_binds{};
    bool heap_real = false;
    if (packet_op) {
        for (const auto* stage : pipeline->GetStages()) {
            if (stage && Shader::Backend::SPIRV::UsesHeapImages(*stage, pipeline->GetProfile())) {
                // sotc: one heap stage of each kind: the fragment or compute stage, and the vertex stage (SOTC_HEAP_VS).
                const bool vertex = stage->sw_stage == Shader::SwStage::Vertex;
                const Shader::Info*& kind_stage = vertex ? heap_vs_stage : heap_stage;
                if (kind_stage) {
                    static std::atomic<u64> two_heap_stages{};
                    if (two_heap_stages.fetch_add(1, std::memory_order_relaxed) < 8) {
                        LOG_ERROR(Render_Vulkan, "SOTC HEAP: two heap stages of one kind in one pipeline, op dropped");
                    }
                    SotcStats::bind_heap_dropped.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_FPS
                    return false;
                }
                kind_stage = stage;
            }
        }
    }
    // sotc: SOTC_SHADER_ADDR=2: the debug buffer's address (0 otherwise). SOTC_HEAP is refused with mode 2 (Init),
    // so the heap branch below does not overwrite it.
    draw_packet.index_table_address = SotcArch::ShaderAddr::VerifyAddress();
    if (heap_stage) {
        heap_real = pipeline->GetProfile().heap_images == 1;
    }
    // sotc: the heap resolutions of this op: TableMemo::Resolve of each heap stage, as the op binds them. Made here, or
    // deferred past the frame cache's lookup (SOTC_BIND_CACHE_HEAP: a hit replays the held ones, see below).
    u64 heap_vs_address = 0;
    // sotc: SOTC_GCN_TEX (S3). A gcn_tex == 1 heap stage finds its slots in the GPU T# table (no index table read).
    // SOTC_GCN_TEX_SKIP_RESOLVE=1 also skips its per-op Resolve, with the static barriers (S5) on. heap_binds stays empty
    // then, so BindHeapImages runs neither the transitions nor the classic decline cases (render target of the draw,
    // stencil image, rebind, composite op): that mode is for measurement until those cases are events (see
    // GCN-NATIVE-DECISIONS.md). Unset: Resolve runs and prefills the table (NoteResolved), the shader reads the table.
    // sotc: only when every image binding of the heap stages is in the table (gcn_rules.h TableStage). A binding the
    // table cannot express still reads the index table, so its stage keeps Resolve.
    const bool gcn_tex_skip =
        SotcGcn::Tex::SkipResolve() && pipeline->GetProfile().gcn_tex == 1 && SotcGcn::Barrier::On() &&
        (heap_stage || heap_vs_stage) && (!heap_stage || Shader::Gcn::TableStage(*heap_stage)) &&
        (!heap_vs_stage || Shader::Gcn::TableStage(*heap_vs_stage));
    const auto resolve_heap_tables = [&]() -> bool {
        if (gcn_tex_skip) {
            draw_packet.index_table_address = 0;
            return true;
        }
        if (heap_stage) {
            // sotc: SOTC_PR_KEYS: the memo key the prep thread computed for this stage (null: KeyOf here).
            const SotcArch::TableMemo::PreKey* heap_key =
                reg_prepared && SotcPrKeysOn() ? &reg_prepared->table_keys[0] : nullptr;
            const auto table = SotcArch::TableMemo::Resolve(
                texture_cache, *heap_stage, liverpool->DrawRegs().ta_bc_base, BindlessGeneration(),
                heap_real ? &heap_binds : nullptr, heap_key);
            if (table.device_address == 0) {
                static std::atomic<u64> table_refused{};
                if (table_refused.fetch_add(1, std::memory_order_relaxed) < 8) {
                    LOG_ERROR(Render_Vulkan, "SOTC HEAP: no index table for stage (pgm {:#x}), op dropped",
                              heap_stage->pgm_hash);
                }
                SotcStats::bind_heap_dropped.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_FPS
                return false;
            }
            draw_packet.index_table_address = table.device_address;
        }
        // sotc: SOTC_HEAP_VS. The vertex table is resolved like the fragment's; its address goes into the heap block after
        // FillBlock. A vertex heap stage exists only in real mode (VertexOn refuses GPU verify), so its binds are always
        // taken (BindHeapImages below).
        if (heap_vs_stage) {
            ASSERT_MSG(pipeline->GetProfile().heap_images == 1,
                       "SOTC HEAP_VS: a vertex heap stage needs the real heap");
            const SotcArch::TableMemo::PreKey* heap_vs_key =
                reg_prepared && SotcPrKeysOn() ? &reg_prepared->table_keys[1] : nullptr; // sotc: SOTC_PR_KEYS
            const auto table = SotcArch::TableMemo::Resolve(texture_cache, *heap_vs_stage,
                                                            liverpool->DrawRegs().ta_bc_base, BindlessGeneration(),
                                                            &heap_vs_binds, heap_vs_key);
            if (table.device_address == 0) {
                static std::atomic<u64> vs_table_refused{};
                if (vs_table_refused.fetch_add(1, std::memory_order_relaxed) < 8) {
                    LOG_ERROR(Render_Vulkan, "SOTC HEAP_VS: no index table for vertex stage (pgm {:#x}), op dropped",
                              heap_vs_stage->pgm_hash);
                }
                SotcStats::bind_heap_dropped.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_FPS
                return false;
            }
            heap_vs_address = table.device_address;
        }
        return true;
    };
    // sotc: SOTC_BIND_CACHE: flush the cached binds when the state they were made in moved, or
    // when this bind is not a read-only graphics bind (its writes and copies run before them).
    // sotc: SOTC_PARALLEL_RECORD: `prepared` holds the register-only inputs of this bind
    // (eligibility, push data, FastBindHash), computed ahead by a prep thread.
    // sotc: SOTC_PACKET. A packet op takes the frame cache (SOTC_BIND_CACHE_FRAME: its hits replay the packet words and
    // the addressed ranges, TryBindCacheFrame). Not the older cache, and not a heap stage: its index table is per op.
    // The heap check is the profile's flag, not only the stages found: a heap pipeline keeps its index table per op.
    const bool packet_heap_free = heap_stage == nullptr && !pipeline->GetProfile().heap_images;
    // sotc: SOTC_BIND_CACHE_PACKET=1/2. A heap pipeline's packet op (real heap, profile 1) takes the frame cache too. Its
    // heap side is bound on a hit as the full bind binds it (after the lookup, see the hit below); the index table itself
    // was resolved above for this op, and the heap block is filled for it. A heap stage that the classic bind takes
    // (HeapDeclineReason) is never cached.
    const bool packet_heap_cache = packet_op && !packet_heap_free && SotcStats::kBindCachePacket != 0 &&
                                   pipeline->GetProfile().heap_images == 1;
    const bool packet_cacheable =
        !packet_op || (SotcStats::kBindCacheFrame && (packet_heap_free || packet_heap_cache));
    // sotc: SOTC_GCN_COMMIT. cache_sync_eligible is the eligibility the caches' sync always had: a lane op still runs the
    // sync with it, so the other entries are flushed as before. The lane only turns off its own cache use (cache_eligible).
    const bool cache_sync_eligible =
        SotcStats::kBindCache && packet_cacheable &&
        (reg_prepared ? reg_prepared->cache_eligible : BindCacheEligible(pipeline));
    const bool cache_eligible = cache_sync_eligible && !commit_bypass; // sotc: SOTC_GCN_COMMIT (lane: no caches)
    // sotc: SOTC_BIND_CACHE_HEAP. The heap resolutions of a packet op the frame cache may take are made on a miss only:
    // a hit replays the ones its entry holds (TryBindCacheFrame). Any other op resolves them here, as before.
    const u32 heap_mask_op = (heap_stage ? 1u : 0u) | (heap_vs_stage ? 2u : 0u);
    const bool heap_defer = packet_heap_cache && cache_eligible && heap_mask_op != 0 &&
                            SotcArch::TableMemo::HeapHoldMode() != 0;
    bool heap_resolved = false;
    if (!heap_defer) {
        if (!resolve_heap_tables()) {
            return false;
        }
        heap_resolved = true;
    }
    if (SotcStats::kBindCache) {
        if (SotcStats::kBindCacheFrame) {
            BindCacheSyncFrame(pipeline, cache_sync_eligible); // sotc: SOTC_GCN_COMMIT (the sync is not the lane's)
        } else {
            BindCacheSync(pipeline, cache_sync_eligible);
        }
    }
    if (SotcStats::kFpsLog && SotcStats::kBindCache) { // sotc: SOTC_FPS, why a bind is refused (sotc_stats.h)
        if (cache_eligible) {
            SotcStats::bind_eligible.fetch_add(1, std::memory_order_relaxed);
        } else {
            // A packet op is refused by packet_cacheable; packet_heap_free tells the two packet gates apart.
            const u32 gate = !packet_cacheable
                                 ? static_cast<u32>(packet_heap_free ? SotcStats::BgPacket
                                                                     : SotcStats::BgPacketHeap)
                                 : BindCacheRefusal(pipeline, liverpool->DrawRegs());
            SotcStats::bind_gate_refused[gate].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (SotcStats::kFpsLog && SotcStats::kBindCachePacket != 0 && packet_op) { // sotc: SOTC_BIND_CACHE_PACKET
        SotcStats::pk_ops.fetch_add(1, std::memory_order_relaxed);
        if (cache_eligible) {
            SotcStats::pk_eligible.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // sotc: SOTC_BIND_CACHE_FRAME, or SOTC_BUFFER_MEMO alone (the memo without the frame cache)
    bind_memo_ok = (SotcStats::kBindCacheFrame || SotcStats::kBufferMemo) && cache_eligible;
    // sotc: SOTC_OP_COMPUTE_BIND. A dispatch eligible for the frame cache; mode 2 never takes a hit
    // and compares the would-be hit (verify_ref, found before the full bind) with the full bind.
    const bool compute_bind = cache_eligible && pipeline->IsCompute();
    const bool compute_verify = compute_bind && SotcStats::kOpComputeBind == 2;
    if (compute_bind && SotcStats::kFpsLog) {
        SotcStats::op_compute_eligible.fetch_add(1, std::memory_order_relaxed);
    }
    BindCacheEntry* verify_ref = nullptr;
    BindCacheEntry* pk_verify_ref = nullptr; // sotc: SOTC_BIND_CACHE_PACKET=2, the entry a packet hit would take
    compute_written_image = {};
    // sotc: the same evaluation order as the old condition (a copy wins over a metadata clear, which has the side effect
    // of ClearMeta). The surface a metadata clear names is kept for BindCacheNoteMetaClear.
    VAddr meta_clear_addr{};
    u64 meta_clear_size{};
    const bool is_image_copy = IsComputeImageCopy(pipeline);
    const bool is_meta_clear =
        !is_image_copy && IsComputeMetaClear(pipeline, &meta_clear_addr, &meta_clear_size);
    const bool is_image_clear =
        !is_image_copy && !is_meta_clear && IsComputeImageClear(pipeline);
    if (is_image_copy || is_meta_clear || is_image_clear) {
        // sotc: SOTC_BIND_CACHE_RANGES: these replace the dispatch by an image copy / clear or a
        // metadata state change, with no write recorded by range: every cached bind dies.
        // sotc: SOTC_BIND_CACHE_FRAME: an image copy or clear is a GPU write to one image, which
        // is what BindCacheInvalidateImage drops the readers of (as Resolve and DepthStencilCopy
        // do); the metadata state change has no range and still flushes everything.
        // sotc: SOTC_BIND_CACHE_META. A metadata clear writes no memory (the dispatch is skipped below) and no
        // bind reads the clear state (see SotcStats::kBindCacheMeta): only the binds reading its range are dropped.
        if (is_meta_clear && SotcStats::kBindCacheRanges && SotcStats::kBindCacheMeta) {
            BindCacheNoteMetaClear(meta_clear_addr, meta_clear_size);
        } else if (SotcStats::kBindCacheFrame && compute_written_image) {
            BindCacheInvalidateImage(compute_written_image);
            if (SotcStats::kFpsLog) {
                SotcStats::bf_narrowed.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (SotcStats::kBindCacheRanges) {
            BindCacheFlush(SotcStats::BindCacheUnsafe,
                           is_image_copy ? SotcStats::UsCopy
                           : is_meta_clear ? SotcStats::UsMetaClear
                                           : SotcStats::UsImageClear);
        }
        static const bool drop_trace = std::getenv("SOTC_DROP_TRACE") != nullptr;
        if (drop_trace) {
            static int shown = 0;
            if (shown++ < 12) {
                const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
                for (const auto& desc : info.buffers) {
                    if (desc.IsSpecial()) {
                        continue;
                    }
                    const auto sharp = desc.GetSharp(info);
                    std::fprintf(stderr,
                                 "SOTC drop-detail cs %#llx buf %#llx+%#llx written %d fmt %d "
                                 "meta %d xor %d images %zu\n",
                                 static_cast<unsigned long long>(info.pgm_hash),
                                 static_cast<unsigned long long>(sharp.base_address),
                                 static_cast<unsigned long long>(sharp.GetSize()),
                                 int(desc.is_written), int(desc.is_formatted),
                                 int(texture_cache.IsMeta(sharp.base_address).has_value()),
                                 int(info.has_bitwise_xor), info.images.size());
                }
            }
        }
        if (SotcStats::kFpsLog && SotcStats::kBindCache) { // sotc: SOTC_FPS
            SotcStats::bind_post_copy.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    bool uses_dma = false;

    // A stage whose user data was never attached (e.g. a program restored from the pipeline
    // cache that was not refreshed for this draw) would read its SGPRs from a null span.
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->UserData().data() == nullptr &&
            stage->srt_info.flattened_bufsize_dw != 0) {
            LOG_ERROR(Render_Vulkan,
                      "Skipping draw: shader {:#x} (sw stage {}, hw stage {}, info {}) has no "
                      "user data bound",
                      stage->pgm_hash, u32(stage->sw_stage), u32(stage->hw_stage),
                      fmt::ptr(stage));
            fast_bind.valid = false;
            if (SotcStats::kFpsLog && SotcStats::kBindCache) { // sotc: SOTC_FPS
                SotcStats::bind_post_nodata.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
    }

    // sotc: the previous draw left these descriptors and push constants bound. Skip rebuilding
    // them when this draw needs exactly the same pipeline, push data, render targets and
    // resource handles. The generation bumps catch reallocated buffers/images and dead samplers.
    // sotc: SOTC_PACKET: a packet op takes the fast bind unless it has a heap stage (its index table is per op). Its
    // hits replay the addressed ranges (PacketAddrReplay) and the barrier rule (packet_barrier).
    const bool fast_eligible = (!packet_op || packet_heap_free) && !commit_bypass && // sotc: SOTC_GCN_COMMIT (lane)
                               (reg_prepared ? reg_prepared->fast_static && !attachment_feedback_loop
                                             : FastBindEligible(pipeline));
    u64 fast_hash = 0;
    if (fast_eligible) {
        SotcStats::fast_bind_eligible.fetch_add(1, std::memory_order_relaxed);
        const auto input_push = reg_prepared ? reg_prepared->push_data : MakeUserData(liverpool->DrawRegs());
        fast_hash = reg_prepared ? reg_prepared->fast_hash : FastBindHash(pipeline, input_push);
        // Same state as the last full bind, ignoring the submit tick (SOTC_FASTBIND_WIDE needs it).
        bool fast_state_same =
            fast_bind.valid && fast_bind.pipeline == pipeline &&
            fast_bind.buffer_generation == BufferBindGeneration() &&
            fast_bind.texture_generation == texture_cache.BindGeneration() &&
            fast_bind.stream_generation == buffer_cache.StreamGeneration() &&
            fast_bind.mem_epoch == SotcStats::mem_epoch.load(std::memory_order_relaxed) &&
            fast_bind.hash == fast_hash;
        if (kBindDiff) { // sotc: SOTC_BIND_DIFF (SOTC_VERIFY): which conditions failed
            u32 mask = 0;
            if (!fast_bind.valid) {
                mask = BdInvalid;
            } else {
                mask |= fast_bind.pipeline != pipeline ? BdPipeline : 0u;
                mask |= fast_bind.buffer_generation != BufferBindGeneration() ? BdBufGen : 0u;
                mask |= fast_bind.texture_generation != texture_cache.BindGeneration() ? BdTexGen
                                                                                        : 0u;
                mask |= fast_bind.stream_generation != buffer_cache.StreamGeneration() ? BdStreamGen
                                                                                       : 0u;
                mask |= fast_bind.mem_epoch != SotcStats::mem_epoch.load(std::memory_order_relaxed)
                            ? BdEpoch
                            : 0u;
                mask |= fast_bind.hash != fast_hash ? BdHash : 0u;
                mask |= fast_bind.tick != scheduler.CurrentTick() ? BdTick : 0u;
            }
            BindDiffNote(mask, mask == 0);
        }
        // sotc: SOTC_PACKET. A packet op's fast or wide hit touches its addressed ranges again, as its full bind did
        // (Touch may place or evict buffers: a generation it moves ends the hit, and the full bind follows).
        if (packet_op && fast_state_same &&
            (fast_bind.tick == scheduler.CurrentTick() || SotcStats::kFastBindWide)) {
            PacketAddrReplay(fast_bind.addr);
            fast_state_same = fast_bind.buffer_generation == BufferBindGeneration() &&
                              fast_bind.stream_generation == buffer_cache.StreamGeneration();
        }
        if (fast_state_same && fast_bind.tick == scheduler.CurrentTick()) {
            fast_bind_hit = true;
            SotcStats::fast_bind_hits.fetch_add(1, std::memory_order_relaxed);
            if (SotcStats::kFpsLog && SotcStats::kBindCache) { // sotc: SOTC_FPS, served before the cache
                (cache_eligible ? SotcStats::bind_fast_eligible : SotcStats::bind_fast_refused)
                    .fetch_add(1, std::memory_order_relaxed);
            }
            if (packet_op) {
                packet_barrier();
                if (SotcStats::kFpsLog) {
                    SotcStats::packet_fast_hits.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (fast_bind.uses_dma) {
                const SotcStats::SplitTimer dma_split{SotcStats::SplitBindDma};
                buffer_cache.SynchronizeDmaBuffers();
            }
            return true;
        }
        if (fast_state_same && SotcStats::kFastBindWide) {
            // sotc: SOTC_FASTBIND_WIDE: the same state in a later submit. The new command buffer
            // holds none of the push constants or descriptors, so the caller still runs
            // Pipeline::BindResources (fast_bind_hit stays false) with the descriptor arrays kept
            // from the last full bind. fast_bind.valid guarantees no other BindResources call
            // replaced them since (the others clear it), and the generations above guarantee the
            // handles and residency they name are still current.
            push_data = input_push;
            fast_bind.tick = scheduler.CurrentTick();
            if (SotcStats::kFpsLog && SotcStats::kBindCache) { // sotc: SOTC_FPS, served before the cache
                (cache_eligible ? SotcStats::bind_fast_eligible : SotcStats::bind_fast_refused)
                    .fetch_add(1, std::memory_order_relaxed);
            }
            if (SotcStats::kFpsLog) {
                SotcStats::fast_bind_wide_hits.fetch_add(1, std::memory_order_relaxed);
            }
            if (packet_op) {
                packet_barrier(); // sotc: SOTC_PACKET (the ranges were touched above)
                if (SotcStats::kFpsLog) {
                    SotcStats::packet_fast_hits.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (fast_bind.uses_dma) {
                buffer_cache.SynchronizeDmaBuffers();
            }
            return true;
        }
        if (SotcStats::kFpsLog) {
            SotcStats::fast_bind_misses.fetch_add(1, std::memory_order_relaxed);
            if (fast_state_same) {
                SotcStats::fast_bind_tick_misses.fetch_add(1, std::memory_order_relaxed);
            }
            // sotc: the same test with the dispatch's clear undone: a hit lost only to a dispatch in between.
            if (!fast_bind.valid && fast_bind.compute_cleared && fast_bind.pipeline == pipeline &&
                fast_bind.buffer_generation == BufferBindGeneration() &&
                fast_bind.texture_generation == texture_cache.BindGeneration() &&
                fast_bind.stream_generation == buffer_cache.StreamGeneration() &&
                fast_bind.mem_epoch == SotcStats::mem_epoch.load(std::memory_order_relaxed) &&
                fast_bind.hash == fast_hash) {
                (fast_bind.tick == scheduler.CurrentTick() ? SotcStats::fast_bind_compute_misses
                                                            : SotcStats::fast_bind_compute_tick_misses)
                    .fetch_add(1, std::memory_order_relaxed);
            }
        }
        push_data = input_push;
    } else {
        if (SotcStats::kFpsLog && fast_bind.valid && pipeline->IsCompute()) {
            fast_bind.compute_cleared = true; // sotc: SOTC_FPS, see FastBindState::compute_cleared
        }
        fast_bind.valid = false;
        push_data = reg_prepared ? reg_prepared->push_data : MakeUserData(liverpool->DrawRegs());
    }

    // sotc: SOTC_BIND_CACHE: the same state as an earlier full bind of this submit.
    size_t first_bound_buffer = 0;
    u64 cache_hash = 0;
    if (cache_eligible) {
        if (SotcStats::kFpsLog) { // sotc: SOTC_FPS, the binds the cache is consulted for
            SotcStats::bind_consulted.fetch_add(1, std::memory_order_relaxed);
        }
        // The border colour table of custom-border samplers (GetSampler) is a register too.
        const u64 bc_base = liverpool->DrawRegs().ta_bc_base.Address();
        bind_level2 = false;
        if (SotcStats::kBindCacheFrame) {
            // sotc: SOTC_BIND_CACHE_FRAME: the key is the pipeline and the resources it binds
            // (BindResourceHash); the constants around them are refreshed by the hit. A pipeline
            // that binds by constants keeps the full key below.
            u64 res_hash = 0;
            const bool res_ok = reg_prepared ? reg_prepared->res_ok : BindResourceHash(pipeline, res_hash);
            if (reg_prepared) {
                res_hash = reg_prepared->res_hash;
            }
            if (res_ok) {
                cache_hash = XXH3_64bits_withSeed(&bc_base, sizeof(bc_base), res_hash);
                bind_level2 = true;
            }
        }
        if (!bind_level2) {
            cache_hash = fast_eligible ? fast_hash
                         : reg_prepared ? reg_prepared->fast_hash
                                        : FastBindHash(pipeline, push_data);
            cache_hash = XXH3_64bits_withSeed(&bc_base, sizeof(bc_base), cache_hash);
            if (SotcStats::kBindCacheFrame) {
                // sotc: entries now outlive many frames: a pipeline erased and another made at the
                // same address must not take its entries, so the stages' identity is in the key.
                u64 ident = 0;
                for (const auto* stage : pipeline->GetStages()) {
                    if (stage) {
                        const u64 id = BindCacheStageIdentity(*stage);
                        ident = XXH3_64bits_withSeed(&id, sizeof(id), ident);
                    }
                }
                cache_hash = XXH3_64bits_withSeed(&ident, sizeof(ident), cache_hash);
            }
        }
        bind_input_push = push_data; // the draw's own viewport words (refreshed on a hit)
        if (compute_verify) {
            // sotc: SOTC_OP_COMPUTE_BIND=2: the entry a hit would take, if it is still usable.
            const u64 targets_key = bind_cache_state.targets_key;
            BindCacheEntry* found =
                BindCacheFindFrame(BindCacheFrameKey(pipeline, cache_hash, targets_key), pipeline,
                                   cache_hash, targets_key);
            if (found && found->valid && found->flush_gen == bind_cache_flush_gen &&
                found->buffer_version == buffer_cache.BindVersion()) {
                verify_ref = found;
            }
        }
        // sotc: SOTC_BIND_CACHE_PACKET. A heap op whose images the classic bind takes is not cached: decided under the lock,
        // before a hit binds anything. Mode 2 never takes a packet hit: the entry a hit would take is looked up here
        // (its addressed ranges refreshed) and compared with the full bind after it (PacketVerify).
        // sotc: SOTC_BIND_CACHE_HEAP. A deferred op's decline is decided by the held resolution (TryBindCacheFrame) or,
        // on a miss, after its resolution is made (below).
        bool pk_heap_declined = !heap_defer && packet_heap_cache && [&] {
            const auto texture_lock = texture_cache.BatchLock();
            return (heap_stage && HeapDeclineReason(*heap_stage, heap_binds) != 0) ||
                   (heap_vs_stage && HeapDeclineReason(*heap_vs_stage, heap_vs_binds) != 0);
        }();
        if (pk_heap_declined && SotcStats::kFpsLog) {
            SotcStats::pk_miss[SotcStats::PkHeapDeclined].fetch_add(1, std::memory_order_relaxed);
        }
        if (SotcStats::kBindCachePacket == 2 && packet_op && !pk_heap_declined) {
            const u64 targets_key = bind_cache_state.targets_key;
            BindCacheEntry* found =
                BindCacheFindFrame(BindCacheFrameKey(pipeline, cache_hash, targets_key), pipeline,
                                   cache_hash, targets_key);
            if (found && found->valid && found->packet && found->flush_gen == bind_cache_flush_gen &&
                found->buffer_version == buffer_cache.BindVersion()) {
                if (PacketAddrRefresh(*found)) {
                    pk_verify_ref = found;
                } else if (SotcStats::kFpsLog) {
                    SotcStats::pk_miss[SotcStats::PkAddrShape].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        const bool pk_take_hit = !pk_heap_declined && !(SotcStats::kBindCachePacket == 2 && packet_op);
        if (bool hit_dma = false; !compute_verify && pk_take_hit &&
                                  (SotcStats::kBindCacheFrame
                                       ? TryBindCacheFrame(pipeline, cache_hash, hit_dma,
                                                           heap_defer ? heap_mask_op : 0u)
                                       : TryBindCache(pipeline, cache_hash, hit_dma))) {
            if (compute_bind && SotcStats::kFpsLog) {
                SotcStats::op_compute_hits.fetch_add(1, std::memory_order_relaxed);
            }
            // sotc: SOTC_BIND_CACHE_PACKET. The heap side of the op, bound as the full bind binds it: the images of the
            // index table (BindHeapImages cannot decline: checked above, under the same state), then the heap block.
            // The table itself was resolved for this op before the lookup.
            if (packet_heap_cache) {
                // sotc: SOTC_BIND_CACHE_HEAP. A deferred op binds the held resolutions TryBindCacheFrame replayed (the index
                // tables and binds of its entry); otherwise the op's own ones.
                SotcArch::TableMemo::TableBinds& hb0 = heap_defer ? bind_hold_binds[0] : heap_binds;
                SotcArch::TableMemo::TableBinds& hb1 = heap_defer ? bind_hold_binds[1] : heap_vs_binds;
                if (heap_defer) {
                    if (heap_stage) {
                        draw_packet.index_table_address = bind_hold_addr[0].device_address;
                    }
                    if (heap_vs_stage) {
                        heap_vs_address = bind_hold_addr[1].device_address;
                    }
                }
                // The decision was taken before the replay; a decline now would leave the images unbound, so it is logged.
                const bool heap_bound = (!heap_stage || BindHeapImages(*heap_stage, hb0)) &&
                                        (!heap_vs_stage || BindHeapImages(*heap_vs_stage, hb1));
                if (!heap_bound) {
                    static std::atomic<u32> declined_after_hit{};
                    if (declined_after_hit.fetch_add(1, std::memory_order_relaxed) < 8) {
                        LOG_ERROR(Render_Vulkan, "SOTC BIND PACKET: heap images declined after a cache hit");
                    }
                }
                // sotc: SOTC_BIND_CACHE_HEAP=2 (verify). The held resolution of each heap stage against a fresh TableMemo::Resolve
                // of the stage, made as the miss path would make it (its indices and images must be the stored ones).
                if (heap_defer && SotcArch::TableMemo::HeapHoldMode() == 2) {
                    const std::array<const Shader::Info*, 2> vstages{heap_stage, heap_vs_stage};
                    for (u32 k = 0; k < 2; ++k) {
                        if (!vstages[k] || !bind_hold_src[k]) {
                            continue;
                        }
                        SotcArch::TableMemo::TableBinds fresh{};
                        const auto table = SotcArch::TableMemo::Resolve(
                            texture_cache, *vstages[k], liverpool->DrawRegs().ta_bc_base,
                            BindlessGeneration(), &fresh);
                        SotcArch::TableMemo::NoteHeapVerify(
                            table.device_address != 0 &&
                            SotcArch::TableMemo::SameBinds(fresh, bind_hold_src[k]->heap[k]));
                    }
                }
                heap_block = {};
                SotcArch::Heap::FillBlock(heap_block, heap_stage, bindless_slots);
                if (heap_vs_stage) {
                    heap_block.vs_table_address = heap_vs_address;
                }
            }
            // The arrays now hold this state: the next identical draw may take the fast bind.
            if (packet_op) { // sotc: SOTC_PACKET (TryBindCacheFrame replayed the packet's ranges)
                packet_barrier();
                if (SotcStats::kFpsLog) {
                    SotcStats::packet_cache_hits.fetch_add(1, std::memory_order_relaxed);
                    if (SotcStats::kBindCachePacket) {
                        SotcStats::pk_hits.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else if (SotcArch::ShaderAddr::On()) {
                // sotc: SOTC_OP_COMPUTE_BIND. The end of the full bind's ordering (the barrier rule and the op's own writes
                // for the later ops, as the full bind does at its end). ReplayOwnWrites made the hazard checks of the ranges.
                if (sotc_addr_hazard && BarrierStatic::BufferChecks()) {
                    buffer_barrier_need = true;
                }
                MergeOpWrites(BarrierStatic::On() || needs_barrier || buffer_barrier_need);
            }
            if (fast_eligible) {
                CommitFastBind(pipeline, fast_hash, hit_dma);
            }
            return true;
        }
        // sotc: SOTC_BIND_CACHE_HEAP. The miss makes the op's heap resolutions (deferred above), decides the decline on them,
        // and keeps them for CommitBindCacheFrame, which stores them with the entry.
        bind_rec_heap_mask = 0;
        if (heap_defer && !heap_resolved) {
            if (!resolve_heap_tables()) {
                return false;
            }
            heap_resolved = true;
            if (packet_heap_cache) {
                const auto texture_lock = texture_cache.BatchLock();
                pk_heap_declined = (heap_stage && HeapDeclineReason(*heap_stage, heap_binds) != 0) ||
                                   (heap_vs_stage && HeapDeclineReason(*heap_vs_stage, heap_vs_binds) != 0);
                if (pk_heap_declined && SotcStats::kFpsLog) {
                    SotcStats::pk_miss[SotcStats::PkHeapDeclined].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (heap_defer) {
            bind_rec_heap_mask = heap_mask_op;
            bind_rec_heap[0] = heap_binds;
            bind_rec_heap[1] = heap_vs_binds;
            u32 index = 0;
            for (const auto* stage : pipeline->GetStages()) {
                if (stage && stage == heap_stage) {
                    bind_rec_heap_stage[0] = index;
                }
                if (stage && stage == heap_vs_stage) {
                    bind_rec_heap_stage[1] = index;
                }
                ++index;
            }
        }
        bind_cache_recording = true;
        // sotc: SOTC_BIND_CACHE_PACKET: a declined heap bind is not stored (its classic bind is not replayed).
        bind_cache_uncacheable = pk_heap_declined;
        bind_cache_writes_rec.clear(); // sotc: SOTC_OP_COMPUTE_BIND, the written bindings of this bind
        bind_cache_images_rec.clear();
        bind_cache_lod_rec.clear();
        bind_cache_ranges_rec.clear();
        bind_cache_repairs_rec.clear();  // sotc: SOTC_BIND_CACHE_XTICK
        bind_cache_samplers_rec.clear(); // sotc: SOTC_BIND_CACHE_XTICK
        if (SotcStats::kBindCacheFrame) { // sotc: what the entry depends on, from this point on
            bind_cache_lookups_rec.clear();
            bind_cache_texel_rec.clear();
            bind_cache_stream_rec = false;
            bind_cache_map_dep_rec = false;
            bind_rec_tex_seq = texture_cache.ImageChangeSeq();
            bind_rec_write_seq = write_stamps.Seq();
            bind_rec_flag_seq = VideoCore::GpuFlagStamps().Seq(); // sotc: SOTC_BIND_CACHE_CONTENT
            bind_rec_map_epoch = MapEpoch();
            bind_rec_gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
            bind_cache_tail_rec = false;
            bind_cache_flat_rec.clear();
        }
        first_bound_buffer = bound_buffers.size();
    }

    set_write_index = 0;
    // sotc: the heap indices of this draw; each bindless stage fills its own words in BindTextures.
    if (instance.IsBindlessSamplersEnabled()) {
        bindless_slots = {};
    }
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    const bool staged = pipeline->UsesStageSets();
    u32 stage_counter = ~0u;
    // sotc: SOTC_PR_RESOLVE: the prepared resource inputs, when they were computed under the table
    // generation this bind sees. Null: every stage resolves inline, as before.
    const PreparedResources* resolved = nullptr;
    const u32 pr_gen = SotcPrGenMode(); // sotc: SOTC_PR_GEN (0 off)
    if (prepared && (SotcPrResolveOn() || pr_gen != 0) && !pr_verify_off) {
        const u64 gen = runtime_memory_generation();
        const bool table_ok = !(gen & 1) && prepared->resolved.runtime_gen == gen;
        if (pr_gen == 0) {
            if (table_ok) {
                resolved = &prepared->resolved;
            }
        } else if (!table_ok) {
            PrGenNote(pr_gen, false, true, true, false); // sotc: SOTC_PR_GEN: needs-owner
        } else if (!PrGenSumsUnchanged(prepared->resolved)) {
            PrGenNote(pr_gen, false, true, false, false); // sotc: a sum moved: needs-owner
        } else if (pr_gen == 1) {
            resolved = &prepared->resolved;
            PrGenNote(pr_gen, true, false, false, false);
        } else {
            // sotc: SOTC_PR_GEN=2: the owner's own computation is what gets used; the prepared
            // values are compared with it (both computed from the same sums).
            PreparedResources fresh{};
            PrepareResources(pipeline, fresh); // sotc: SOTC_GCN_COMMIT: a dispatch's pipeline is not a graphics one
            const bool same = SamePreparedResources(fresh, prepared->resolved);
            static std::atomic<u32> shown_gen{0};
            if (!same && shown_gen.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::fprintf(stderr, "SOTC PR GEN verify mismatch: the owner resolves inline\n");
            }
            PrGenNote(pr_gen, true, false, false, !same);
        }
    }
    for (const auto* stage : pipeline->GetStages()) {
        ++stage_counter;
        if (!stage) {
            continue;
        }
        cur_bind_stage = stage_counter; // sotc: SOTC_BIND_CACHE_FRAME (flat buffer slots)
        // sotc: SOTC_STAGE_BINDINGS: each stage numbers its descriptors from 0 in its own set. The
        // buffer count runs on across the stages: it indexes the push constant offsets they share.
        const u32 set = staged ? Shader::StageDescriptorSet(stage->sw_stage) : 0u;
        if (staged) {
            binding.unified = 0;
        }
        if (stage->sw_stage == Shader::SwStage::Vertex) { // sotc: SOTC_VS_BINDING_BASE
            binding = Shader::Backend::VsStartBindings(binding, staged);
        }
        // sotc: bindless samplers have no descriptor write (BindTextures sets their push slots).
        const size_t num_sampler_writes =
            IsBindlessStage(instance, *stage) ? 0 : stage->samplers.size();
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          num_sampler_writes);
        set_write_sets.resize(set_writes.size(), 0u);
        // sotc: SOTC_PR_RESOLVE: this stage's prepared entries (stage_counter is its position in
        // GetStages(), as the prep thread numbered it). FlatForBind stages stay inline.
        const PrStage* pr_stage = resolved && resolved->stages[stage_counter].ready &&
                                          !FlatMayBePatched(*stage)
                                      ? &resolved->stages[stage_counter]
                                      : nullptr;
        {
            const SotcTrace::BindScope trace_step{"bind buffers"}; // sotc: a = bytes, b = protects
            const SotcStats::SplitTimer buffers_split{SotcStats::SplitBindBuffers};
            BindBuffers(*stage, binding, push_data, set, pr_stage ? resolved : nullptr, pr_stage);
        }
        {
            const SotcTrace::ScopeMin trace_step{"bind textures", stage->images.size()}; // sotc
            const SotcOpPhases::Scope ph{SotcOpPhases::PhBindTextures}; // sotc: SOTC_OP_PHASES
            const SotcStats::SplitTimer textures_split{SotcStats::SplitBindTextures};
            // sotc: SOTC_HEAP. The heap stage of the op takes its images from the index table in real mode; the classic
            // bind runs when BindHeapImages declines (counted there) and in the GPU verify (counted here).
            bool textures_done = false;
            if (stage == heap_stage) {
                if (heap_real) {
                    textures_done = BindHeapImages(*stage, heap_binds);
                } else {
                    SotcArch::TableMemo::NoteBindClassic(0);
                }
            } else if (stage == heap_vs_stage) {
                // sotc: SOTC_HEAP_VS. Its own fallback: the classic bind runs when BindHeapImages declines (counted there).
                textures_done = BindHeapImages(*stage, heap_vs_binds);
                SotcArch::TableMemo::NoteVertexBind(textures_done);
            }
            // sotc: SOTC_BIND_CACHE_PACKET. The classic bind of a heap stage writes descriptors and records images that a
            // hit does not replay as heap binds: such an op is not stored.
            if (!textures_done && packet_heap_cache && bind_cache_recording &&
                (stage == heap_stage || stage == heap_vs_stage)) {
                bind_cache_uncacheable = true;
            }
            if (!textures_done) {
                BindTextures(*stage, binding, set, pr_stage ? resolved : nullptr, pr_stage);
            }
        }
        uses_dma |= stage->uses_dma;
    }
    // sotc: SOTC_HEAP: the heap block of this op, after every stage's textures are bound (the classic slots of the
    // verify exist only then). The debug buffer address goes in every heap op; the words only in GPU verify.
    heap_block = {};
    if (packet_op && pipeline->GetProfile().heap_images != 0) {
        SotcArch::Heap::FillBlock(heap_block, heap_stage, bindless_slots);
        // sotc: SOTC_HEAP_VS: the vertex table (real mode: the block's words are zero, so this is their only content).
        if (heap_vs_stage) {
            heap_block.vs_table_address = heap_vs_address;
        }
    }
    // sotc: SOTC_N5_BINDLESS: the vector was sized for every image, and the heap images write no
    // descriptor: keep the writes made (the bind cache and the descriptor update count them).
    // sotc: SOTC_SHADER_ADDR: a shader-addressed buffer keeps its slot in the sizing above but writes no descriptor,
    // so the vector ends with zeroed entries (type 0, count 0, binding 0) that Pipeline::BindResources would push.
    if (bindless_images || SotcArch::ShaderAddr::On()) {
        set_writes.resize(set_write_index);
        set_write_sets.resize(set_write_index);
    }
    // sotc: SOTC_SHADER_ADDR ordering. A shader-addressed read or write does not go through ObtainBuffer, so the
    // runtime's per-buffer check never asks for the barrier that orders it after the buffer writes recorded before
    // it (SOTC_PASSES=0 ignores shader_addr_touch). Those writes are pending in the runtime's memory barrier: the
    // op flushes it when it touches memory by address, and any later op flushes it when an address write is pending.
    if (SotcArch::ShaderAddr::On()) {
        if (sotc_addr_hazard && BarrierStatic::BufferChecks()) {
            buffer_barrier_need = true;
        }
        // sotc: SOTC_BARRIER_STATIC=1: see packet_barrier (the flush is decided later).
        MergeOpWrites(BarrierStatic::On() || needs_barrier || buffer_barrier_need);
    }

    if (uses_dma) {
        const SotcStats::SplitTimer dma_split{SotcStats::SplitBindDma};
        buffer_cache.SynchronizeDmaBuffers();
    }

    if (verify_ref) {
        // sotc: SOTC_OP_COMPUTE_BIND=2. The descriptors, their info arrays and the push data of
        // the entry a hit would take, against the ones this full bind built (before the commit
        // below overwrites the entry). Same encoding as CommitBindCacheFrame's write_info.
        const BindCacheEntry& ref = *verify_ref;
        bool same = std::memcmp(&ref.push_data, &push_data, sizeof(push_data)) == 0 &&
                    ref.writes.size() == set_writes.size() &&
                    ref.write_sets.size() == set_write_sets.size() &&
                    ref.write_info.size() == set_writes.size() &&
                    ref.buffer_infos.size() == buffer_infos.size() &&
                    ref.image_infos.size() == image_infos.size();
        for (size_t i = 0; same && i < set_writes.size(); ++i) {
            const auto& a = ref.writes[i];
            const auto& b = set_writes[i];
            same = a.dstBinding == b.dstBinding && a.dstArrayElement == b.dstArrayElement &&
                   a.descriptorCount == b.descriptorCount && a.descriptorType == b.descriptorType &&
                   ref.write_sets[i] == set_write_sets[i];
            const u32 info = b.descriptorType == vk::DescriptorType::eStorageBuffer
                                 ? 0x80000000u | static_cast<u32>(b.pBufferInfo - buffer_infos.data())
                                 : static_cast<u32>(b.pImageInfo - image_infos.data());
            same = same && ref.write_info[i] == info;
        }
        same = same &&
               (buffer_infos.empty() ||
                std::memcmp(ref.buffer_infos.data(), buffer_infos.data(),
                            buffer_infos.size() * sizeof(vk::DescriptorBufferInfo)) == 0) &&
               (image_infos.empty() ||
                std::memcmp(ref.image_infos.data(), image_infos.data(),
                            image_infos.size() * sizeof(vk::DescriptorImageInfo)) == 0);
        if (SotcStats::kFpsLog) {
            SotcStats::op_compute_verify_checks.fetch_add(1, std::memory_order_relaxed);
            if (!same) {
                SotcStats::op_compute_verify_mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // sotc: SOTC_OP_COMPUTE_BIND=2. The written bindings a hit would replay against the ones this full bind made.
    if (verify_ref) {
        WriteVerify(*verify_ref);
    }
    // sotc: SOTC_BIND_CACHE_PACKET=2. The would-be hit compared with this full bind (before the commit below).
    if (pk_verify_ref) {
        PacketVerify(*pk_verify_ref, first_bound_buffer);
    }
    if (fast_eligible) {
        CommitFastBind(pipeline, fast_hash, uses_dma);
    }
    BindCacheEntry* made = nullptr; // sotc: SOTC_OP_COMPUTE_BIND, the entry this bind stored (if any)
    if (bind_cache_recording) {
        bind_cache_recording = false;
        if (SotcStats::kBindCacheFrame) {
            made = CommitBindCacheFrame(pipeline, cache_hash, uses_dma, first_bound_buffer);
        } else {
            CommitBindCache(pipeline, cache_hash, uses_dma, first_bound_buffer);
        }
    }
    ApplyOwnWriteStamps(made);
    if (packet_op && SotcStats::kFpsLog) { // sotc: SOTC_PACKET: a full bind (no cache or fast hit)
        SotcStats::packet_full_binds.fetch_add(1, std::memory_order_relaxed);
    }

    return true;
}

// sotc: SOTC_SHADER_ADDR ordering (see GuestWrite in vk_rasterizer.h). A range is live while its tag is not older
// than the runtime's flush generation: the barrier recorded after it orders it for every later op.
// sotc: SOTC_BIND_CACHE_PACKET. The addressed ranges of a packet entry, recomputed from the op's own V#s (the key has
// no base for them, BindResourceHash). Rewrites the entry's PacketAddr, its bind cache range and repair, and the
// bounds of its ranges, as BindBuffers and CommitBindCacheFrame would have made them for this op. false: the shape
// changed (a range appeared or vanished, or a recorded position is gone); the entry is not used.
bool Rasterizer::PacketAddrRefresh(BindCacheEntry& entry) {
    if (!entry.pipeline || entry.addr_src.size() != entry.addr.size()) {
        return false; // every PacketAddr of the entry must have its source, or some range would stay the old one
    }
    const auto stages = entry.pipeline->GetStages();
    for (const auto& src : entry.addr_src) {
        const Shader::Info* st = src.stage < stages.size() ? stages[src.stage] : nullptr;
        if (!st || src.desc >= st->buffers.size() || src.addr_index >= entry.addr.size()) {
            return false;
        }
        const auto sharp = st->buffers[src.desc].GetSharp(*st);
        const VAddr vbase = sharp.base_address;
        const u32 vsize = sharp.GetSize();
        // As BindBuffers: the clamp to the mapping, 0 when the base or the size is 0.
        const u64 size = vbase != 0 && vsize != 0 ? memory->ClampRangeSize(vbase, vsize) : 0;
        if ((size != 0) != (src.range_index != kNoIndex)) {
            return false;
        }
        if (src.range_index != kNoIndex) {
            if (src.range_index >= entry.ranges.size()) {
                return false;
            }
            entry.ranges[src.range_index] = {vbase, size};
        }
        if (src.repair_index != kNoIndex) {
            if (src.repair_index >= entry.repairs.size()) {
                return false;
            }
            entry.repairs[src.repair_index] = {vbase, vsize, size, nullptr, 0, false};
        }
        entry.addr[src.addr_index] = {vbase, size, false};
    }
    // The bounds of what the entry reads (CommitBindCacheFrame makes them the same way).
    VAddr lo = ~VAddr{0}, hi = 0;
    for (const auto& [addr, len] : entry.ranges) {
        lo = std::min(lo, addr);
        hi = std::max(hi, addr + len);
    }
    for (const auto& img : entry.images) {
        lo = std::min(lo, img.guest_lo);
        hi = std::max(hi, img.guest_hi);
    }
    entry.lo = lo;
    entry.hi = hi;
    return true;
}

// sotc: SOTC_BIND_CACHE_PACKET=2. The full bind of a packet op, compared with the entry a hit would have replayed (its
// addressed ranges already refreshed, PacketAddrRefresh). Called before the commit, which then keeps the full bind's
// result. A category is a mismatch when the hit would have bound something the full bind did not, or the reverse.
void Rasterizer::PacketVerify(const BindCacheEntry& ref, size_t first_bound_buffer) {
    bool bad[SotcStats::PvCategories]{};
    // Descriptor writes, as the compute verify compares them: the info index names the buffer or image info.
    bad[SotcStats::PvWrites] = ref.writes.size() != set_writes.size() ||
                               ref.write_sets.size() != set_write_sets.size() ||
                               ref.write_info.size() != set_writes.size();
    for (size_t i = 0; !bad[SotcStats::PvWrites] && i < set_writes.size(); ++i) {
        const auto& a = ref.writes[i];
        const auto& b = set_writes[i];
        const u32 info = b.descriptorType == vk::DescriptorType::eStorageBuffer
                             ? 0x80000000u | static_cast<u32>(b.pBufferInfo - buffer_infos.data())
                             : static_cast<u32>(b.pImageInfo - image_infos.data());
        bad[SotcStats::PvWrites] = a.dstBinding != b.dstBinding ||
                                   a.dstArrayElement != b.dstArrayElement ||
                                   a.descriptorCount != b.descriptorCount ||
                                   a.descriptorType != b.descriptorType ||
                                   ref.write_sets[i] != set_write_sets[i] ||
                                   ref.write_info[i] != info;
    }
    // Buffer infos: the stream ring copies (flat buffers, LDS, the block table) sit at their own offset in each op.
    const vk::Buffer stream = buffer_cache.GetStreamBuffer().Handle();
    bad[SotcStats::PvBufInfo] = ref.buffer_infos.size() != buffer_infos.size();
    for (size_t i = 0; !bad[SotcStats::PvBufInfo] && i < buffer_infos.size(); ++i) {
        const auto& a = ref.buffer_infos[i];
        const auto& b = buffer_infos[i];
        bad[SotcStats::PvBufInfo] = a.buffer != b.buffer || a.range != b.range ||
                                    (a.buffer != stream && a.offset != b.offset);
    }
    // sotc: classify each differing image info (categories in sotc_stats.h). A backing change is the case a hit now
    // refuses (TryBindCacheFrame), so PvImgInfo counts only the differences a hit would replay.
    bad[SotcStats::PvImgInfo] = ref.image_infos.size() != image_infos.size();
    if (ref.image_infos.size() == image_infos.size()) {
        for (size_t i = 0; i < image_infos.size(); ++i) {
            const auto& a = ref.image_infos[i];
            const auto& b = image_infos[i];
            const VideoCore::ImageId id_a = i < ref.image_info_images.size() ? ref.image_info_images[i] : VideoCore::ImageId{};
            const VideoCore::ImageId id_b = image_info_images[i];
            if (a.imageView == b.imageView && a.sampler == b.sampler && a.imageLayout == b.imageLayout) {
                continue;
            }
            if (id_a != id_b) {
                bad[SotcStats::PvImgImage] = true;
            } else if (a.sampler != b.sampler) {
                bad[SotcStats::PvImgSampler] = true;
            } else if (a.imageView != b.imageView) {
                const void* ref_backing =
                    i < ref.image_info_backings.size() ? ref.image_info_backings[i] : nullptr;
                if (ref_backing != ImageInfoBacking(id_b, b.imageView)) {
                    bad[SotcStats::PvImgBacking] = true;
                } else {
                    bad[SotcStats::PvImgView] = true;
                }
            } else {
                bad[SotcStats::PvImgLayout] = true;
            }
        }
    }
    bad[SotcStats::PvImgInfo] = bad[SotcStats::PvImgInfo] || bad[SotcStats::PvImgImage] ||
                                bad[SotcStats::PvImgSampler] || bad[SotcStats::PvImgView] ||
                                bad[SotcStats::PvImgLayout];
    // Push data, without the viewport words (a hit takes this op's own).
    Shader::PushData ref_push = ref.push_data;
    ref_push.xoffset = push_data.xoffset;
    ref_push.yoffset = push_data.yoffset;
    ref_push.xscale = push_data.xscale;
    ref_push.yscale = push_data.yscale;
    bad[SotcStats::PvPush] = std::memcmp(&ref_push, &push_data, sizeof(ref_push)) != 0;
    // Addressed ranges: the replay touches these.
    bad[SotcStats::PvAddr] = ref.addr.size() != packet_addr_rec.size();
    for (size_t i = 0; !bad[SotcStats::PvAddr] && i < packet_addr_rec.size(); ++i) {
        const auto& a = ref.addr[i];
        const auto& b = packet_addr_rec[i];
        bad[SotcStats::PvAddr] = a.addr != b.addr || a.size != b.size || a.written != b.written;
    }
    // Records: the buffers the bind bound, and the images it looked up.
    const size_t nb = bound_buffers.size() - first_bound_buffer;
    bad[SotcStats::PvBuffers] = ref.buffers.size() != nb;
    for (size_t i = 0; !bad[SotcStats::PvBuffers] && i < nb; ++i) {
        const auto& a = ref.buffers[i];
        const auto& b = bound_buffers[first_bound_buffer + i];
        bad[SotcStats::PvBuffers] = a.buffer != b.buffer || a.offset != b.offset || a.size != b.size ||
                                    a.is_written != b.is_written || a.addr != b.addr;
    }
    bad[SotcStats::PvImages] = ref.images.size() != bind_cache_images_rec.size();
    for (size_t i = 0; !bad[SotcStats::PvImages] && i < bind_cache_images_rec.size(); ++i) {
        const auto& a = ref.images[i];
        const auto& b = bind_cache_images_rec[i];
        bad[SotcStats::PvImages] =
            a.id != b.id || a.layout != b.layout || a.stages != b.stages || a.access != b.access ||
            a.range.base.level != b.range.base.level || a.range.base.layer != b.range.base.layer ||
            a.range.extent.levels != b.range.extent.levels ||
            a.range.extent.layers != b.range.extent.layers || a.guest_lo != b.guest_lo ||
            a.guest_hi != b.guest_hi || a.uid != b.uid;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::pk_verify_checks.fetch_add(1, std::memory_order_relaxed);
        for (u32 c = 0; c < SotcStats::PvCategories; ++c) {
            if (bad[c]) {
                SotcStats::pk_mismatch[c].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

bool Rasterizer::GuestWriteOverlap(VAddr addr, u64 size) {
    const u64 gen = runtime.FlushGeneration();
    const VAddr end = addr + size;
    bool hit = false;
    std::erase_if(sotc_guest_writes, [&](const GuestWrite& w) {
        if (w.gen < gen) {
            return true;
        }
        hit |= w.begin < end && addr < w.end;
        return false;
    });
    return hit;
}

// sotc: the op's own writes become visible to the later ops. When the op records a barrier first (FlushBarriers
// runs after this bind and before its draw), the writes belong to the interval after that barrier.
void Rasterizer::MergeOpWrites(bool barrier_before_op) {
    const u64 gen = runtime.FlushGeneration() + (barrier_before_op ? 1 : 0);
    for (const auto& [begin, end] : sotc_op_writes) {
        sotc_guest_writes.push_back({begin, end, gen});
    }
    // sotc: SOTC_BARRIER_GUEST. The safety net marks them in ResetBindings (end of the op).
    if (SotcArch::GuestBarrier::Active()) {
        guest_addr_writes.insert(guest_addr_writes.end(), sotc_op_writes.begin(), sotc_op_writes.end());
    }
    sotc_op_writes.clear();
}

// sotc: SOTC_BARRIER_GUEST, draw thread. A guest sync point of the op stream (Liverpool::QueueGuestSync). Mode 1 records
// the global barrier when a buffer write is pending; the runtime clears the safety net with it (Runtime::FlushBarriers).
void Rasterizer::GuestSyncPoint(u32 kind) {
    if (SotcArch::GuestBarrier::SyncPoint(static_cast<SotcArch::GuestBarrier::Sync>(kind))) {
        runtime.FlushBarriers();
    }
}

// sotc: SOTC_BARRIER_GUEST. The safety net of the op being bound: its reads and writes against the granules written
// (and read) since the last barrier. Mode 1 returns the decision; verify counts it next to today's (old_due).
bool Rasterizer::GuestNetDecide(bool old_due) {
    guest_reads_buf.clear();
    guest_writes_buf.clear();
    for (const auto& bound : bound_buffers) {
        const VAddr addr = bound.buffer->CpuAddr() + bound.offset;
        guest_reads_buf.emplace_back(addr, bound.size);
        if (bound.is_written) {
            guest_writes_buf.emplace_back(addr, bound.size);
        }
    }
    for (const auto& read : fixed_function_reads) {
        guest_reads_buf.emplace_back(read.buffer->CpuAddr() + read.offset, read.size);
    }
    for (const auto& [begin, end] : guest_addr_writes) {
        guest_writes_buf.emplace_back(begin, end - begin);
    }
    const bool net = SotcArch::GuestBarrier::NetHit(guest_reads_buf, guest_writes_buf, shader_addr_touch);
    return SotcArch::GuestBarrier::Decision(needs_barrier, net, buffer_barrier_need, old_due);
}

// sotc: SOTC_BARRIER_STATIC. The barrier decision before an op: the place of the old Passes::Decide call. Mode 0 is that
// call (image side and buffer side together, as before). The op's reads and writes are kept for ResetBindings.
bool Rasterizer::DecideOpBarrier(bool reads, bool writes) {
    op_static_reads = reads;
    op_static_writes = writes;
    // sotc: SOTC_BARRIER_GUEST=1: the guest sync points and the safety net decide (buffer_barrier_need is not used).
    if (SotcArch::GuestBarrier::On()) {
        return GuestNetDecide(false);
    }
    const bool old_due = BarrierStatic::Decide(needs_barrier, buffer_barrier_need, reads, writes);
    if (SotcArch::GuestBarrier::Verifying()) { // sotc: =2, today's decision is emitted; the new rule is counted next to it
        GuestNetDecide(old_due);
    }
    return old_due;
}

// sotc: a buffer write outside a bind (DMA fill or copy) that a later shader-addressed read must order after.
void Rasterizer::NoteGuestWrite(VAddr addr, u64 size) {
    if (SotcArch::ShaderAddr::On() && size != 0) {
        sotc_guest_writes.push_back({addr, addr + size, runtime.FlushGeneration()});
    }
}

void Rasterizer::PacketAddrReplay(const std::vector<PacketAddr>& ranges) {
    // sotc: SOTC_PACKET. What BindBuffers does for each shader-addressed buffer of a full bind, for a replayed one: no
    // descriptor, the range touched (Touch, or the mirror's Touch in mirror mode), and the skip counted without
    // SOTC_SHADER_ADDR=2. Only read-only ranges get here: BindCacheEligible and FastBindStatic refuse a bind that
    // writes a buffer, so the write side (Passes::NoteWrite, NoteAddressedWrite, the mirror copy back) never replays.
    shader_addr_touch = !ranges.empty();
    for (const auto& r : ranges) {
        if (r.size != 0) {
            sotc_addr_hazard |= GuestWriteOverlap(r.addr, r.size);
        }
        if (!SotcArch::ShaderAddr::Verify()) {
            SotcArch::ShaderAddr::NoteSkip(r.size, r.written);
        }
        const bool touch_ok =
            r.size == 0 ||
            (SotcArch::AddrTable::On()
                 ? GcnTouch(buffer_cache, scheduler, r.addr, r.size, r.written, r.texel)
                 : SotcArch::Mirror::Touch(scheduler, r.addr, r.size));
        if (!touch_ok) {
            static std::atomic<u64> replay_touch_refused{};
            if (replay_touch_refused.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_ERROR(Render_Vulkan, "SOTC SHADER_ADDR: replayed bind cannot hold {:#x}+{:#x}",
                          r.addr, r.size);
            }
        }
    }
}

bool Rasterizer::BindCacheEligible(const Pipeline* pipeline) const {
    return BindCacheEligible(pipeline, liverpool->DrawRegs());
}

bool Rasterizer::BindCacheEligible(const Pipeline* pipeline, const AmdGpu::Regs& regs) const {
    return BindCacheRefusal(pipeline, regs) == SotcStats::BgNone;
}

// sotc: SOTC_OP_COMPUTE_BIND=1/2. A written buffer binding may be recorded by the frame cache: a plain guest buffer (not
// texel, not GDS or another special buffer) that the pipeline does not address from its V# words (those keep their
// ranges and write notes in the shader-address path, which is not recorded). Everything else stays refused.
static bool WrittenBindAdmitted(const Pipeline& pipeline, const Shader::Info& stage,
                                const Shader::BufferResource& desc) {
    if (SotcStats::kOpComputeBind == 0 || desc.IsSpecial() || desc.is_formatted) {
        return false;
    }
    const auto& profile = pipeline.GetProfile();
    if (profile.shader_addr != 0 &&
        Shader::ShaderAddrBufferAt(profile, stage, static_cast<u32>(&desc - stage.buffers.data()))) {
        return false;
    }
    return true;
}

u32 Rasterizer::BindCacheRefusal(const Pipeline* pipeline, const AmdGpu::Regs& regs) const {
    // sotc: SOTC_BIND_CACHE: graphics binds whose resources are all read-only (as FastBindEligible,
    // bindless sampler stages included: their heap slots are kept in the entry). A bind that
    // writes buffers or images is not eligible and flushes the cache, unless the written buffers
    // are admitted by WrittenBindAdmitted (SOTC_OP_COMPUTE_BIND, which replays their side effects).
    // sotc: SOTC_OP_COMPUTE_BIND: a dispatch is eligible under the same rules when it reads only
    // (no DMA pointer, no sharp table; the write checks below apply to it too).
    // Returns the first gate the bind fails (SotcStats::BindGate), BgNone when it is eligible.
    const bool compute = pipeline->IsCompute();
    if (compute && SotcStats::kOpComputeBind == 0) {
        return SotcStats::BgCompute;
    }
    if (regs.clipper_control.user_clip_plane_enable != 0) {
        return SotcStats::BgClip;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->has_storage_images || !stage->fmasks.empty()) {
            return SotcStats::BgStorage;
        }
        if (compute && (stage->uses_dma || !stage->sharp_tables.empty())) {
            return SotcStats::BgDispatchDma;
        }
        for (const auto& desc : stage->buffers) {
            if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                return SotcStats::BgShared;
            }
            if (desc.is_written && (!desc.IsSpecial() ||
                                    desc.buffer_type == Shader::BufferType::GdsBuffer) &&
                !WrittenBindAdmitted(*pipeline, *stage, desc)) {
                return SotcStats::BgWrittenBuf;
            }
        }
        for (const auto& desc : stage->images) {
            if (desc.is_written) {
                return SotcStats::BgWrittenImg;
            }
        }
    }
    return SotcStats::BgNone;
}

void Rasterizer::BindCacheFlush(u32 reason, u32 site) {
    if (bind_cache_live == 0) {
        return;
    }
    if (SotcStats::kFpsLog && reason == SotcStats::BindCacheUnsafe) { // sotc: SOTC_FPS, unsafe by site
        SotcStats::bind_unsafe_sites[site].fetch_add(1, std::memory_order_relaxed);
    }
    if (SotcStats::kBindCacheFrame) {
        // sotc: SOTC_BIND_CACHE_FRAME: older entries are dropped when a lookup finds them.
        ++bind_cache_flush_gen;
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_flushes[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    for (auto& entry : bind_cache) {
        entry.valid = false;
    }
    bind_cache_live = 0;
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_flushes[reason].fetch_add(1, std::memory_order_relaxed);
    }
}

void Rasterizer::BindCacheSync(const Pipeline* pipeline, bool eligible) {
    BindCacheState now{
        .tick = scheduler.CurrentTick(),
        .buffer_generation = BufferBindGeneration(),
        .texture_generation = texture_cache.BindGeneration(),
        .stream_generation = buffer_cache.StreamGeneration(),
        .mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed),
        .write_epoch = bind_cache_write_epoch.load(std::memory_order_relaxed),
    };
    if (!pipeline->IsCompute()) {
        // The targets PrepareRenderState found for this draw (cb_descs past the MRT count are
        // stale, so they are left out).
        const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        const u32 num_cb = std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
        for (u32 cb = 0; cb < num_cb; ++cb) {
            now.targets[cb] = cb_descs[cb].first;
        }
        now.targets[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    } else if (SotcStats::kBindCacheRanges) {
        // A dispatch renders into no target: the draws after it still use the targets the
        // entries were made with when the draws before it did.
        now.targets = bind_cache_state.targets;
    }
    const auto& old = bind_cache_state;
    if (SotcStats::kBindCacheRanges) {
        // sotc: SOTC_BIND_CACHE_RANGES. The tick, resource generation and render target rules
        // stay. Writes no longer flush everything: each one drops the entries reading what it
        // writes (BindCacheInvalidate), and guest memory changes are left to the checks of each
        // hit. Why that covers every writer (TryBindCache refuses a CPU-modified buffer range,
        // and a Dirty, untracked, unregistered or re-bound image):
        //  - guest CPU writes to tracked pages fault: Rasterizer::InvalidateMemory(OnGuestFault)
        //    -> MemoryTracker::InvalidateRegion sets the CPU bit (directly, or after the readback
        //    of a GPU-written page in MarkWrittenByGuest; a page still GPU-pending stays protected
        //    and the write faults again), and TextureCache::InvalidateMemory marks the images
        //    CpuDirty / MaybeCpuDirty or untracks them. Pages left writable (fault grace, hot
        //    pages, write-ahead) keep their CPU bit, as the comment in TryBindCache says.
        //  - command processor writes done on the host (WRITE_DATA, DUMP_CONST_RAM, the CPU paths
        //    of FillBuffer / CopyBuffer, fence and label writes) are CPU writes: they fault the
        //    same way on a protected page, or call InvalidateMemory (labels, video decode).
        //  - unmaps (DropGpuModified, fast or not) mark the range CPU-modified; freed images
        //    move the texture generation (resources flush).
        //  - shader HLE: the replicate fill writes the backing (TryReplaceFromCpu marks it
        //    CPU-modified; TryWriteThroughFromCpu also copies it into the arena) and the copy
        //    shader writes the arena: both call BindCacheNoteWrite (vk_shader_hle.cpp).
        //  - readbacks write guest memory with what the GPU already holds: nothing to drop.
        //  - GPU writes: written buffers and storage images of non-eligible binds (BindBuffers,
        //    BindTextures) and their render targets (below), GPU fills and copies (FillBuffer,
        //    CopyBuffer; GDS is not guest memory and its barrier is replayed by a hit), Resolve,
        //    DepthStencilCopy and EliminateFastClear call BindCacheInvalidate; the compute
        //    image copy / clear / metadata clear paths flush (BindResources).
        // Inputs a hit cannot revalidate are not stored at all (BindBuffers): texel buffers at an
        // image's address (ObtainBuffer re-tiles a GPU-written image into them on every bind)
        // and stream-buffer copies (a snapshot of guest memory).
        // sotc: SOTC_BIND_CACHE_XTICK: the tick is not a flush reason. What it protected is
        // replayed by the first hit of each entry in a new tick (TryBindCache): the image and
        // sampler stamps and the block stamps; descriptors, push data, transitions and buffer
        // access checks are re-issued by every hit anyway.
        if (now.tick != old.tick && !SotcStats::kBindCacheXtick) {
            BindCacheFlush(SotcStats::BindCacheTick);
        } else if (now.buffer_generation != old.buffer_generation ||
                   now.texture_generation != old.texture_generation ||
                   now.stream_generation != old.stream_generation) {
            BindCacheFlush(SotcStats::BindCacheResources);
        } else if (now.targets != old.targets) {
            BindCacheFlush(SotcStats::BindCacheTargets);
        }
        bind_cache_state = now;
        // A draw that writes buffers or images is not eligible: its render targets are written
        // along with the rest (BindBuffers / BindTextures drop the entries for those).
        if (!eligible && !pipeline->IsCompute() && bind_cache_live != 0) {
            const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
            const u32 num_cb =
                std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
            for (u32 cb = 0; cb < num_cb; ++cb) {
                if (const auto id = cb_descs[cb].first) {
                    BindCacheInvalidateImage(id);
                }
            }
            if (const auto id = db_desc.first) {
                BindCacheInvalidateImage(id);
            }
        }
        return;
    }
    if (!eligible || now.write_epoch != old.write_epoch) {
        BindCacheFlush(SotcStats::BindCacheUnsafe, eligible ? SotcStats::UsSyncWriteEpoch
                                                            : SotcStats::UsSyncIneligible);
    } else if (now.tick != old.tick) {
        BindCacheFlush(SotcStats::BindCacheTick);
    } else if (now.mem_epoch != old.mem_epoch) {
        BindCacheFlush(SotcStats::BindCacheMemory);
    } else if (now.buffer_generation != old.buffer_generation ||
               now.texture_generation != old.texture_generation ||
               now.stream_generation != old.stream_generation) {
        BindCacheFlush(SotcStats::BindCacheResources);
    } else if (now.targets != old.targets) {
        BindCacheFlush(SotcStats::BindCacheTargets);
    }
    bind_cache_state = now;
}

void Rasterizer::BindCacheNoteImage(VideoCore::ImageId image_id, vk::ImageLayout layout,
                                    vk::PipelineStageFlags2 stages, vk::AccessFlags2 access,
                                    const VideoCore::SubresourceRange& range) {
    // The depth target of this draw sampled: its layout depends on registers outside the key
    // (stencil enable), so the bind is not stored.
    if (image_id == db_desc.first) {
        bind_cache_uncacheable = true;
        return;
    }
    // Called under BindTextures' batch lock: the image info is stable here.
    const auto& info = texture_cache.GetImage(image_id).info;
    const VAddr guest_lo = info.guest_address - info.sotc_dropped_bytes; // a mip tail: its chain
    const u64 uid = texture_cache.GetImage(image_id).image_uid;
    bind_cache_images_rec.push_back({image_id, layout, stages, access, range, guest_lo,
                                     info.guest_address + info.guest_size, uid});
}

void Rasterizer::BindCacheInvalidate(VAddr addr, u64 size, VideoCore::ImageId image) {
    if (SotcStats::kBindCacheFrame) { // sotc: SOTC_BIND_CACHE_FRAME
        BindCacheInvalidateFrame(addr, size, image);
        return;
    }
    // sotc: SOTC_BIND_CACHE_RANGES. 32 entries: a linear scan, most rejected by the bounds.
    if (bind_cache_live == 0) {
        return;
    }
    const VAddr end = addr + size;
    const auto overlaps = [&](VAddr lo, VAddr hi) { return size != 0 && lo < end && addr < hi; };
    u32 dropped = 0;
    for (auto& entry : bind_cache) {
        if (!entry.valid) {
            continue;
        }
        bool hit = false;
        if (image) {
            for (const auto& img : entry.images) {
                if (img.id == image) {
                    hit = true;
                    break;
                }
            }
        }
        if (!hit && overlaps(entry.lo, entry.hi)) {
            for (const auto& [lo, len] : entry.ranges) {
                if (overlaps(lo, lo + len)) {
                    hit = true;
                    break;
                }
            }
            for (size_t i = 0; !hit && i < entry.images.size(); ++i) {
                hit = overlaps(entry.images[i].guest_lo, entry.images[i].guest_hi);
            }
        }
        if (hit) {
            entry.valid = false;
            --bind_cache_live;
            ++dropped;
        }
    }
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_range_writes.fetch_add(1, std::memory_order_relaxed);
        if (dropped) {
            SotcStats::bind_cache_range_entries.fetch_add(dropped, std::memory_order_relaxed);
        }
    }
}

void Rasterizer::BindCacheInvalidateImage(VideoCore::ImageId image_id) {
    // sotc: SOTC_BIND_CACHE_FRAME notes the write even with an empty cache: a bind in flight stores
    // its entry afterwards and compares it with the writes noted since it began.
    if (!SotcStats::kBindCacheRanges || (bind_cache_live == 0 && !SotcStats::kBindCacheFrame) ||
        !image_id) {
        return;
    }
    const auto& info = texture_cache.GetImage(image_id).info;
    const VAddr lo = info.guest_address - info.sotc_dropped_bytes;
    BindCacheInvalidate(lo, info.guest_address + info.guest_size - lo, image_id);
}

void Rasterizer::BindCacheNoteMetaClear(VAddr addr, u64 size) {
    // sotc: SOTC_BIND_CACHE_META. Why the cache needs nothing more than the entries reading this range: the dispatch
    // that clears the surface is skipped (BindResources returns before any draw or copy), so no memory and no image
    // changes; TextureCache::ClearMeta only sets the clear state of the surface and moves meta_epoch. Cached binds read
    // neither (the clear state is read by the render target paths, TestAndTouchMeta / IsMetaCleared, and by RsMemo,
    // which do not use the bind cache), and IsMeta, which they consult, depends on the registered surfaces only.
    // The range invalidation is the same one a write of this range makes: it is conservative, and it keeps the
    // result right should a clear ever write.
    // sotc: the narrowing statistics walk the whole cache per clear (6% of the draw thread in a lean profile, 7 Nov):
    // only with the FPS timers (not SOTC_FPS_LEAN).
    if (SotcStats::kFpsLog && SotcStats::kFpsTimers) {
        SotcStats::meta_clear_narrowed.fetch_add(1, std::memory_order_relaxed);
        const VAddr end = addr + size;
        const auto reads = [&](const BindCacheEntry& e) {
            if (size == 0) {
                return false;
            }
            for (const auto& [lo, len] : e.ranges) {
                if (lo < end && addr < lo + len) {
                    return true;
                }
            }
            for (const auto& img : e.images) {
                if (img.guest_lo < end && addr < img.guest_hi) {
                    return true;
                }
            }
            return false;
        };
        u64 live = 0, killed = 0;
        if (SotcStats::kBindCacheFrame) {
            for (const auto& e : frame_cache) {
                if (e.valid && e.flush_gen == bind_cache_flush_gen) {
                    ++live;
                    killed += reads(e);
                }
            }
        } else {
            for (const auto& e : bind_cache) {
                if (e.valid) {
                    ++live;
                    killed += reads(e);
                }
            }
        }
        if (live) { // a flush would have emptied a non-empty cache
            SotcStats::meta_clear_flushes_avoided.fetch_add(1, std::memory_order_relaxed);
        }
        SotcStats::meta_clear_entries_killed.fetch_add(killed, std::memory_order_relaxed);
    }
    BindCacheInvalidate(addr, size);
}

const void* Rasterizer::ImageInfoBacking(VideoCore::ImageId id, vk::ImageView view) {
    if (!id) {
        return nullptr;
    }
    auto& image = texture_cache.GetImage(id);
    if (image.backing_images.size() == 1) {
        return image.backing; // the only backing: the view can only be its own
    }
    for (const auto& backing : image.backing_images) {
        for (const auto view_id : backing.image_view_ids) {
            if (*texture_cache.GetImageView(view_id).image_view == view) {
                return &backing;
            }
        }
    }
    return nullptr;
}

void Rasterizer::BindCacheNoteWrite(VAddr addr, u64 size) {
    if (SotcStats::kBindCacheRanges) {
        BindCacheInvalidate(addr, size);
    }
}

bool Rasterizer::TryBindCache(const Pipeline* pipeline, u64 hash, bool& uses_dma) {
    // sotc: SOTC_BIND_CACHE. BindCacheSync has just flushed every entry made in another tick,
    // with other guest memory, other resident buffers / images / samplers / stream ring, other
    // render targets, or before a GPU write; a live entry is the result of a full bind of this
    // very state (same pipeline and FastBindHash inputs) in this command buffer, after which
    // only read-only draws into these same targets ran. With SOTC_BIND_CACHE_RANGES, other
    // binds and DMA may have run too, none of them writing what the entry reads
    // (BindCacheInvalidate), and guest memory may have changed: the checks below catch that.
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_lookups.fetch_add(1, std::memory_order_relaxed);
        if (SotcStats::kBindCacheXtick) {
            // sotc: SOTC_BIND_CACHE_XTICK: was this key looked up in the previous game frame too?
            const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
            auto& seen = bind_seen[(hash ^ (hash >> 29)) % bind_seen.size()];
            bool prev_frame = false;
            if (seen.hash == hash) {
                prev_frame = seen.frame_last == frame ? seen.frame_prev + 1 == frame
                                                      : seen.frame_last + 1 == frame;
                if (seen.frame_last != frame) {
                    seen.frame_prev = seen.frame_last;
                    seen.frame_last = frame;
                }
            } else {
                seen = {hash, frame, 0};
            }
            if (prev_frame) {
                SotcStats::bind_cache_seen_prev_frame.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    BindCacheEntry* entry = nullptr;
    for (auto& e : bind_cache) {
        if (e.valid && e.pipeline == pipeline && e.hash == hash) {
            entry = &e;
            break;
        }
    }
    if (!entry) {
        return false;
    }
    const auto refuse = [] {
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_refused.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    // sotc: SOTC_N5_BINDLESS: the heap slots of the entry are as made (no release or eviction since).
    if (entry->bindless_gen != BindlessGeneration()) {
        entry->valid = false;
        --bind_cache_live;
        return refuse();
    }
    const bool xtick = SotcStats::kBindCacheXtick;
    u64 current_tick = scheduler.CurrentTick();
    // sotc: SOTC_BIND_CACHE_XTICK. What a full bind does once per submit tick (or per use epoch)
    // that a hit would skip, and how it is replayed here, for an entry made in an earlier tick:
    //  - TextureCache::FindImage: tick_prev_accessed / tick_accessed_last stamps and the LRU
    //    touch of each sampled image (the image GC and the host image copy idle test read them):
    //    TextureCache::NoteBoundUse below, once per entry and tick.
    //  - TextureCache::GetSampler: the sampler LRU touch (an untouched sampler is collected,
    //    which moves the texture generation and flushes the cache): GetSampler again, checked
    //    to return the same handle and bindless slot.
    //  - BufferCache::EnsureResident: block_last_use stamps (MaybeEvictCold's idle test): one
    //    stamping per use epoch (seconds), BufferCache::NoteBindUse.
    //  - memory->ClampRangeSize of each V#: the mapped extent the binding was made for (a guest
    //    unmap or a map next to it); checked once per tick and before every repair.
    // Not needed: descriptor and push data (re-issued by every hit), image layout transitions
    // and buffer access barriers (replayed by every hit), SotcLod marks (every hit), the stream
    // buffer (its wrap generation is a flush reason), the resident set (BindVersion is a flush
    // reason: removals and arena replacement; additions move nothing a binding holds).
    bool replay = xtick && entry->stamp_tick != current_tick;
    bool mapping_checked = false;
    const auto mapping_changed = [&] {
        mapping_checked = true;
        for (const auto& r : entry->repairs) {
            if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
                return true;
            }
        }
        return false;
    };
    if (replay && mapping_changed()) {
        entry->valid = false;
        --bind_cache_live;
        return refuse();
    }
    if (xtick && buffer_cache.BindVersion() != bind_cache_state.buffer_generation) {
        BindCacheFlush(SotcStats::BindCacheResources); // an unmap / eviction since BindCacheSync
        return false;
    }
    // ObtainBuffer would upload CPU-modified pages. Pages kept writable after a fault (fault
    // grace, SOTC_HOT_PAGES) take guest writes without moving mem_epoch: checked per range.
    if (!xtick) {
        for (const auto& [addr, size] : entry->ranges) {
            if (buffer_cache.IsRegionCpuModified(addr, size)) {
                return refuse();
            }
        }
    } else {
        // SOTC_BIND_CACHE_XTICK: repair instead of refusing. A CPU-modified range is
        // synchronized as ObtainBuffer does (the same arena and arguments: the cached handle is
        // still the binding, BindVersion), if the guest mapping under it is still the one the
        // entry was made for; the descriptors stay valid. Images and the rest still refuse.
        u32 repaired = 0;
        for (const auto& r : entry->repairs) {
            if (!r.arena || !buffer_cache.IsRegionCpuModified(r.addr, r.size)) {
                continue;
            }
            if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
                return refuse();
            }
            buffer_cache.SynchronizeBinding(r.arena, r.addr, r.size, r.texel);
            ++repaired;
        }
        if (repaired != 0) {
            // The upload may have submitted, evicted or wrapped the stream ring: the entry would
            // be stale. Nothing of this hit has been replayed yet, the full bind follows.
            if (buffer_cache.BindVersion() != bind_cache_state.buffer_generation ||
                buffer_cache.StreamGeneration() != bind_cache_state.stream_generation) {
                BindCacheFlush(SotcStats::BindCacheResources);
                return false;
            }
            // sotc: the synchronization may have dropped this very entry (a BindCacheInvalidate
            // for a range it reads, reached through the upload); a dropped entry is not replayed.
            if (!entry->valid) {
                return false;
            }
            if (SotcStats::kFpsLog) {
                SotcStats::bind_cache_xtick_repaired.fetch_add(1, std::memory_order_relaxed);
            }
            // The synchronization may have submitted: the stamps are for the tick being recorded.
            current_tick = scheduler.CurrentTick();
            replay = entry->stamp_tick != current_tick;
            if (replay && !mapping_checked && mapping_changed()) {
                entry->valid = false;
                --bind_cache_live;
                return refuse();
            }
        }
    }
    if (xtick && replay) {
        for (const auto& s : entry->samplers) {
            u32 slot = 0;
            const vk::Sampler handle = texture_cache.GetSampler(
                s.sharp, liverpool->DrawRegs().ta_bc_base, s.is_depth, &slot); // as BindTextures
            if (handle != s.handle || slot != s.slot) {
                entry->valid = false;
                --bind_cache_live;
                return refuse();
            }
        }
    }
    {
        const auto texture_lock = texture_cache.BatchLock();
        // The guest may unmap (unregister) images from its own threads: checked under the lock.
        if (texture_cache.BindGeneration() != bind_cache_state.texture_generation) {
            BindCacheFlush(SotcStats::BindCacheResources);
            return false;
        }
        // What the full bind would do differently: an image to upload again or re-track
        // (TextureCache::UpdateImage), one replaced since (needs_rebind), or one that became a
        // target or storage image of this draw (general / feedback loop layouts).
        for (const auto& img : entry->images) {
            const auto& image = texture_cache.GetImage(img.id);
            const bool ok =
                True(image.flags & VideoCore::ImageFlagBits::Registered) &&
                False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
                image.track_addr == image.info.guest_address &&
                image.track_addr_end == image.info.guest_address + image.info.guest_size &&
                !image.binding.needs_rebind && !image.binding.force_general &&
                !image.binding.is_target && img.id != db_desc.first;
            if (!ok) {
                return refuse();
            }
        }
        // The side effects of BindTextures' second pass: bound for this draw (BeginRendering's
        // feedback test, ResetBindings), and in the layout the descriptors name, with a barrier
        // when another command changed it since.
        for (const auto& img : entry->images) {
            auto& image = texture_cache.GetImage(img.id);
            image.binding.is_bound = 1u;
            bound_images.emplace_back(img.id);
            needs_barrier |= runtime.Transit(&image, img.layout, img.stages, img.access, img.range);
            image.usage.texture = 1u;
            if (replay) {
                texture_cache.NoteBoundUse(img.id); // sotc: SOTC_BIND_CACHE_XTICK, see above
            }
        }
    }
    if (xtick) {
        entry->stamp_tick = current_tick;
        if (const u16 epoch = buffer_cache.UseEpoch(); entry->stamp_epoch != epoch) {
            for (const auto& r : entry->repairs) {
                if (r.arena) {
                    buffer_cache.NoteBindUse(r.addr, r.size);
                }
            }
            entry->stamp_epoch = epoch;
        }
        if (SotcStats::kFpsLog) {
            if (replay) {
                SotcStats::bind_cache_xtick_replays.fetch_add(1, std::memory_order_relaxed);
            }
            if (entry->made_tick != current_tick) {
                SotcStats::bind_cache_xtick_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // The buffer side of BindBuffers: barrier against earlier writes, and the reads recorded
    // after the draw (ResetBindings).
    for (const auto& buf : entry->buffers) {
        buffer_barrier_need |= Passes::BufferNeed( // sotc: SOTC_PASSES (see BindVertexBuffers)
            BarrierStatic::BufferChecks() &&
            runtime.IsBufferAccessed(buf.buffer, buf.offset, buf.size, buf.is_written));
        bound_buffers.emplace_back(buf);
    }
    for (const u32 bank : entry->lod_banks) {
        SotcLod::Mark(bank);
    }

    buffer_infos.assign(entry->buffer_infos.begin(), entry->buffer_infos.end());
    image_infos.assign(entry->image_infos.begin(), entry->image_infos.end());
    std::copy(entry->image_info_images.begin(), entry->image_info_images.end(),
              image_info_images.begin());
    set_writes.assign(entry->writes.begin(), entry->writes.end());
    set_write_sets.assign(entry->write_sets.begin(), entry->write_sets.end());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const u32 info = entry->write_info[i];
        auto& write = set_writes[i];
        write.dstSet = VK_NULL_HANDLE;
        if (info & 0x80000000u) {
            write.pBufferInfo = buffer_infos.data() + (info & 0x7FFFFFFFu);
        } else {
            write.pImageInfo = image_infos.data() + info;
        }
    }
    set_write_index = static_cast<u32>(set_writes.size());
    push_data = entry->push_data;
    bindless_slots = entry->bindless_slots;
    if (bindless_images) { // sotc: SOTC_N5_BINDLESS: the replayed slots are used by this tick
        for (const auto& ref : entry->bindless_refs) {
            bindless_images->Use(ref.key, ref.slot);
            if (bindless_images->Mode() == 2 && ref.slot != 0) {
                bindless_images->Verify(ref.key, ref.slot, ref.view);
            }
        }
    }
    entry->last_use = ++bind_cache_clock;

    uses_dma = entry->uses_dma;
    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Rasterizer::CommitBindCache(const Pipeline* pipeline, u64 hash, bool uses_dma,
                                 size_t first_bound_buffer) {
    // sotc: SOTC_BIND_CACHE. Not stored when the bind did something a hit would not repeat, or
    // when the state moved while it ran (a submit, a guest write, a new image...): the next
    // BindCacheSync flushes then.
    const auto& state = bind_cache_state;
    const auto vo_target = [&] {
        const auto id = state.targets[0];
        return id && texture_cache.GetImage(id).usage.vo_surface;
    };
    // SOTC_BIND_CACHE_RANGES: a guest write while this bind ran (another thread's fault) leaves
    // its pages CPU-modified or its images dirty, which the next hit refuses: mem_epoch is not
    // needed here either.
    if (bind_cache_uncacheable || vo_target() || scheduler.CurrentTick() != state.tick ||
        (!SotcStats::kBindCacheRanges &&
         SotcStats::mem_epoch.load(std::memory_order_relaxed) != state.mem_epoch) ||
        bind_cache_write_epoch.load(std::memory_order_relaxed) != state.write_epoch ||
        BufferBindGeneration() != state.buffer_generation ||
        texture_cache.BindGeneration() != state.texture_generation ||
        buffer_cache.StreamGeneration() != state.stream_generation ||
        set_writes.size() != set_write_index) {
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_uncacheable.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    BindCacheEntry* entry = nullptr;
    for (auto& e : bind_cache) {
        if (e.valid && e.pipeline == pipeline && e.hash == hash) {
            entry = &e; // a refused match: replaced
            break;
        }
    }
    if (!entry) {
        entry = &bind_cache[0];
        for (auto& e : bind_cache) {
            if (!e.valid) {
                entry = &e;
                break;
            }
            if (e.last_use < entry->last_use) {
                entry = &e;
            }
        }
        if (!entry->valid) {
            ++bind_cache_live;
        }
    }
    entry->pipeline = pipeline;
    entry->hash = hash;
    entry->last_use = ++bind_cache_clock;
    entry->uses_dma = uses_dma;
    entry->push_data = push_data;
    entry->bindless_slots = bindless_slots;
    entry->bindless_refs = bind_bindless_rec;
    entry->bindless_gen = bind_bindless_gen;
    entry->writes.assign(set_writes.begin(), set_writes.end());
    entry->write_sets.assign(set_write_sets.begin(), set_write_sets.end());
    entry->write_info.resize(set_writes.size());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const auto& write = set_writes[i];
        if (write.descriptorType == vk::DescriptorType::eStorageBuffer) {
            entry->write_info[i] =
                0x80000000u | static_cast<u32>(write.pBufferInfo - buffer_infos.data());
        } else {
            entry->write_info[i] = static_cast<u32>(write.pImageInfo - image_infos.data());
        }
    }
    entry->buffer_infos.assign(buffer_infos.begin(), buffer_infos.end());
    entry->image_infos.assign(image_infos.begin(), image_infos.end());
    entry->image_info_images.assign(image_info_images.begin(),
                                    image_info_images.begin() + image_infos.size());
    entry->buffers.assign(bound_buffers.begin() + first_bound_buffer, bound_buffers.end());
    entry->images.assign(bind_cache_images_rec.begin(), bind_cache_images_rec.end());
    entry->lod_banks.assign(bind_cache_lod_rec.begin(), bind_cache_lod_rec.end());
    entry->ranges.assign(bind_cache_ranges_rec.begin(), bind_cache_ranges_rec.end());
    if (SotcStats::kBindCacheXtick) {
        entry->repairs.assign(bind_cache_repairs_rec.begin(), bind_cache_repairs_rec.end());
        entry->samplers.assign(bind_cache_samplers_rec.begin(), bind_cache_samplers_rec.end());
        entry->made_tick = state.tick;
        entry->stamp_tick = state.tick; // the full bind just made this tick's stamps
        entry->stamp_epoch = 0;         // block stamps due at the first hit
    }
    // SOTC_BIND_CACHE_RANGES: the bounds of everything the entry reads (BindCacheInvalidate).
    VAddr lo = ~VAddr{0}, hi = 0;
    for (const auto& [addr, size] : entry->ranges) {
        lo = std::min(lo, addr);
        hi = std::max(hi, addr + size);
    }
    for (const auto& img : entry->images) {
        lo = std::min(lo, img.guest_lo);
        hi = std::max(hi, img.guest_hi);
    }
    entry->lo = lo;
    entry->hi = hi;
    entry->valid = true;
}

// ---------------------------------------------------------------------------------------------
// sotc: SOTC_BIND_CACHE_FRAME. The bind cache that survives the frame.
//
// What changes against the SOTC_BIND_CACHE_XTICK cache above, and why each old flush reason is
// still covered:
//  - Table: 4096 entries, open addressing (FrameProbe slots), keyed by pipeline, FastBindHash
//    (plus the pipeline's stage identity) and the render targets.
//  - Render targets (and their image identities) are part of the key instead of a flush reason.
//    What the targets guarded stays per hit: a sampled image that became a target of this draw
//    (binding.is_target) or is its depth target refuses the hit.
//  - The texture generation (every RegisterImage / UnregisterImage) no longer wipes the cache.
//    FindImage's answer for a T# depends on the registered images overlapping the range it looked
//    up, so the entry keeps those ranges (T# ranges, the found images', texel buffers') and asks
//    TextureCache::ImageChangedSince whether an image was (un)registered over any of them since
//    it last looked. An unrelated registration invalidates nothing. A freed image's slot can be
//    reused: the unregistration is over the image's own range, which the entry watches, and the
//    image uid is compared at the hit as well. The sampler half of the generation (a destroyed
//    sampler leaves stale handles) stays a cache-wide flush.
//  - The buffer generation (BindVersion: unbind, eviction, arena replacement) is compared per
//    entry; an entry made before the change is dropped when found, one made after survives. (A
//    per-range version would drop fewer: see the report.)
//  - The stream ring wrap count is compared for entries that hold a flat buffer copy there.
//  - GPU writes (BindCacheInvalidate) are range stamps checked by the entries that read the
//    range or sample the image, as the scan of the old table did, at the next hit.
//  - Not stored when any of it moved while the bind ran (the old code: texture generation).
//  - A cache-wide flush (BindCacheFlush) is a generation counter.
// Stamps are compared at the hit only when the global sequence number moved since the entry last
// looked, so a hit between two events costs what it did.
// ---------------------------------------------------------------------------------------------

namespace {
u64 BindCacheFrameKey(const Pipeline* pipeline, u64 hash, u64 targets_key) {
    u64 key = hash ^ (reinterpret_cast<uintptr_t>(pipeline) * 0x9E3779B97F4A7C15ULL) ^
              (targets_key * 0xC2B2AE3D27D4EB4FULL);
    key ^= key >> 31;
    return key | 1; // 0 marks an empty slot
}
} // namespace

void Rasterizer::BindCacheSyncFrame(const Pipeline* pipeline, bool eligible) {
    if (frame_cache.empty()) {
        frame_cache.resize(BindCacheFrameEntries);
        frame_keys.assign(BindCacheFrameEntries, 0);
    }
    BindCacheState now{
        .tick = scheduler.CurrentTick(),
        .buffer_generation = buffer_cache.BindVersion(),
        .texture_generation = texture_cache.SamplerGeneration(),
        .stream_generation = buffer_cache.StreamGeneration(),
        .mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed),
        .write_epoch = bind_cache_write_epoch.load(std::memory_order_relaxed),
    };
    if (!pipeline->IsCompute()) {
        // The targets PrepareRenderState found for this draw (cb_descs past the MRT count are
        // stale, so they are left out). Their identities (image uid) are in the key: a slot
        // reused by another image is another target.
        const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        const u32 num_cb = std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
        std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 1> identity{};
        for (u32 cb = 0; cb < num_cb; ++cb) {
            now.targets[cb] = cb_descs[cb].first;
            if (now.targets[cb]) {
                identity[cb] = texture_cache.GetImage(now.targets[cb]).image_uid;
            }
        }
        now.targets[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
        if (db_desc.first) {
            identity[AmdGpu::NUM_COLOR_BUFFERS] = texture_cache.GetImage(db_desc.first).image_uid;
        }
        now.targets_key = XXH3_64bits(identity.data(), identity.size() * sizeof(u64));
    } else {
        // A dispatch renders into no target.
        now.targets = bind_cache_state.targets;
        now.targets_key = bind_cache_state.targets_key;
    }
    const bool samplers_moved = now.texture_generation != bind_cache_state.texture_generation;
    bind_cache_state = now;
    if (samplers_moved) {
        BindCacheFlush(SotcStats::BindCacheResources);
    }
    // A draw that writes buffers or images is not eligible: its render targets are written along
    // with the rest (BindBuffers / BindTextures note the writes of those). sotc: SOTC_OP_COMPUTE_BIND admits a graphics
    // bind that writes a buffer; its render targets are dropped here as an ineligible draw's are.
    if ((!eligible || StagesWriteMemory(*pipeline)) && !pipeline->IsCompute()) {
        const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        const u32 num_cb = std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
        for (u32 cb = 0; cb < num_cb; ++cb) {
            if (const auto id = cb_descs[cb].first) {
                BindCacheInvalidateImage(id);
            }
        }
        if (const auto id = db_desc.first) {
            BindCacheInvalidateImage(id);
        }
    }
}

void Rasterizer::BindCacheInvalidateFrame(VAddr addr, u64 size, VideoCore::ImageId image) {
    // Only the draw thread notes writes (the scan this replaces was not thread safe either).
    write_stamps.Note(addr, addr + std::max<u64>(size, 1),
                      image ? &write_image_stamps[image.index % WriteImageStamps] : nullptr);
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_range_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {
/// Whether the GPU writes and image set changes noted after the checkpoints leave a bind that
/// reads `ranges`, samples `images` and looked up `tex_watch` alone. The checkpoints move up only
/// when it does.
template <typename Ranges, typename Images>
bool BindCacheStampsOk(const VideoCore::RangeStamps& writes,
                       const std::array<std::atomic<u64>, 4096>& image_stamps,
                       const VideoCore::TextureCache& textures, const Ranges& ranges,
                       const Images& images, const std::vector<std::pair<VAddr, VAddr>>& tex_watch,
                       u64& checked_write, u64& checked_tex, u32& reason, int content,
                       u64& checked_flag, bool& kept, bool& hold) {
    // sotc: SOTC_BIND_CACHE_CONTENT. Today's rule (content 0): a GPU write to a range or image the entry reads refuses
    // it. Content 1 and 2: a write refuses only when it changed the GPU-modified state of an image over one of the
    // entry's lookup ranges (VideoCore::GpuFlagStamps: FindImage's mip-tail choice reads it). Any other write keeps the
    // entry; its hit replays the image transitions and buffer barriers as it always did. Content 2 keeps today's
    // decision: a write that today's rule refuses holds the entry (no checkpoint moves) for the full bind's comparison.
    kept = false;
    hold = false;
    // The checkpoint is the sequence number read before the stamps: an event published while
    // looking is newer than it and is found by the next check.
    const u64 write_seq = writes.Seq();
    bool old_write = false; // today's rule: a write over a range or image the entry reads
    if (write_seq != checked_write) {
        for (const auto& [addr, size] : ranges) {
            if (writes.ChangedSince(addr, addr + size, checked_write)) {
                old_write = true;
                break;
            }
        }
        for (const auto& img : images) {
            if (old_write) {
                break;
            }
            if (image_stamps[img.id.index % 4096].load(std::memory_order_relaxed) > checked_write ||
                writes.ChangedSince(img.guest_lo, img.guest_hi, checked_write)) {
                old_write = true;
            }
        }
    }
    if (content == 0 && old_write) {
        reason = SotcStats::BfWrites;
        return false;
    }
    const u64 flag_seq = VideoCore::GpuFlagStamps().Seq();
    if (content != 0 && flag_seq != checked_flag) {
        for (const auto& [lo, hi] : tex_watch) {
            if (VideoCore::GpuFlagStamps().ChangedSince(lo, hi, checked_flag)) {
                if (SotcStats::kFpsLog) {
                    SotcStats::bc_refused[SotcStats::CrFlag].fetch_add(1, std::memory_order_relaxed);
                }
                reason = SotcStats::BfWrites;
                return false;
            }
        }
    }
    const u64 tex_seq = textures.ImageChangeSeq();
    if (tex_seq != checked_tex) {
        for (const auto& [lo, hi] : tex_watch) {
            if (textures.ImageChangedSince(lo, hi, checked_tex)) {
                reason = SotcStats::BfImages;
                return false;
            }
        }
    }
    // Only after the other checks: a held entry is one the content rule would otherwise keep.
    if (old_write) {
        kept = true;
        if (content == 2) {
            hold = true;
            if (SotcStats::kFpsLog) {
                SotcStats::bc_refused[SotcStats::CrHold].fetch_add(1, std::memory_order_relaxed);
            }
            reason = SotcStats::BfWrites;
            return false;
        }
    }
    checked_write = write_seq;
    checked_tex = tex_seq;
    if (content != 0) {
        checked_flag = flag_seq;
    }
    return true;
}
} // namespace

bool Rasterizer::BindCacheStampsFresh(BindCacheEntry& entry, u32& reason) {
    if (entry.map_dependent && entry.map_epoch != MapEpoch()) {
        reason = SotcStats::BfMapping;
        return false;
    }
    // sotc: SOTC_BIND_CACHE_CONTENT. A mip-tail entry's dependence on the GPU-modified state is checked over its lookup
    // ranges (BindCacheStampsOk), not by the epoch that every first GPU write moves.
    const int content = SotcStats::kBindCacheContent;
    if (content == 0 && entry.tail_dependent &&
        entry.gpu_epoch != VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed)) {
        reason = SotcStats::BfImages;
        return false;
    }
    bool kept = false, hold = false;
    const bool ok = BindCacheStampsOk(write_stamps, write_image_stamps, texture_cache, entry.ranges,
                                      entry.images, entry.tex_watch, entry.checked_write_seq,
                                      entry.checked_tex_seq, reason, content,
                                      entry.checked_flag_seq, kept, hold);
    bind_cache_kept_write |= kept;
    bind_cache_hold |= hold;
    return ok;
}

bool Rasterizer::BindCacheEntryFresh(BindCacheEntry& entry, u32& reason) {
    if (entry.flush_gen != bind_cache_flush_gen) {
        reason = SotcStats::BfFlush;
        return false;
    }
    if (entry.buffer_version != buffer_cache.BindVersion()) {
        reason = SotcStats::BfBuffers;
        return false;
    }
    if (entry.uses_stream && entry.stream_gen != buffer_cache.StreamGeneration()) {
        reason = SotcStats::BfStream;
        return false;
    }
    return BindCacheStampsFresh(entry, reason);
}

void Rasterizer::BindCacheDropFrame(BindCacheEntry& entry, u32 reason) {
    const size_t index = static_cast<size_t>(&entry - frame_cache.data());
    if (index < frame_keys.size() && frame_keys[index] != 0) {
        frame_keys[index] = 0;
        --bind_cache_live;
        if (SotcStats::kFpsLog) {
            SotcStats::bf_live.store(bind_cache_live, std::memory_order_relaxed);
            SotcStats::bf_drop[reason].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

Rasterizer::BindCacheEntry* Rasterizer::BindCacheFindFrame(u64 key, const Pipeline* pipeline,
                                                           u64 hash, u64 targets_key) {
    const size_t base = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 52); // 12 bits
    for (u32 probe = 0; probe < BindCacheFrameProbe; ++probe) {
        const size_t index = (base + probe) & (BindCacheFrameEntries - 1);
        if (frame_keys[index] != key) {
            continue;
        }
        auto& entry = frame_cache[index];
        if (entry.pipeline == pipeline && entry.hash == hash && entry.targets_key == targets_key) {
            return &entry;
        }
    }
    return nullptr;
}

void Rasterizer::BindCacheNoteLookup(const VideoCore::TextureCache::ImageDesc& desc,
                                     VideoCore::ImageId image) {
    // sotc: SOTC_BIND_CACHE_FRAME. What FindImage looked at: the T#'s range (with its mip tail
    // alternative), and the image it settled on.
    const auto add = [&](VAddr lo, VAddr hi) {
        if (!bind_cache_lookups_rec.empty() && bind_cache_lookups_rec.back() == std::pair{lo, hi}) {
            return;
        }
        bind_cache_lookups_rec.emplace_back(lo, hi);
    };
    add(desc.info.guest_address - desc.info.sotc_dropped_bytes,
        desc.info.guest_address + desc.info.guest_size);
    if (desc.other_info) {
        add(desc.other_info->guest_address - desc.other_info->sotc_dropped_bytes,
            desc.other_info->guest_address + desc.other_info->guest_size);
    }
    if (image) {
        const auto& info = texture_cache.GetImage(image).info;
        add(info.guest_address - info.sotc_dropped_bytes, info.guest_address + info.guest_size);
        bind_cache_tail_rec |= info.sotc_first_mip != 0;
    }
}

u32 Rasterizer::HeapHoldDecline(const Pipeline* pipeline, const BindCacheEntry& entry, u32 k) {
    // sotc: SOTC_BIND_CACHE_HEAP. The shape was checked by the caller (the stage exists). Caller holds the texture cache lock.
    const Shader::Info* st = pipeline->GetStages()[entry.heap_stage[k]];
    return HeapDeclineReason(*st, entry.heap[k]);
}

// sotc: SOTC_OP_COMPUTE_BIND. A hit's replay of the written bindings of its entry, with the calls the full bind made for
// them (BindBuffers): ObtainBuffer (the binding must be the recorded one: buffer and offset), the texture invalidation,
// the host write note and the op's write for the ordering of later ops. The stamps of those writes are noted last, and the
// entry's checkpoint moves past them: they are this entry's own writes. Returns 0 replayed, 1 the binding changed (the hit
// is refused), 2 the entry is stale (reason set). Also the ordering a full bind makes for the ranges it reads (GuestWriteOverlap).
int Rasterizer::ReplayOwnWrites(BindCacheEntry& entry, u32& reason) {
    if (SotcArch::ShaderAddr::On()) {
        for (const auto& [addr, size] : entry.ranges) {
            if (size != 0 && GuestWriteOverlap(addr, size)) {
                sotc_addr_hazard = true;
            }
        }
    }
    if (entry.own_writes.empty()) {
        return 0;
    }
    // The narrowing dispatch changes the marks (SotcWriteExtent::Narrow): an entry made under another one is refused first.
    const auto& dispatch = t_direct_dispatch;
    for (const auto& w : entry.own_writes) {
        const u64 groups = dispatch.hash == w.pgm_hash ? dispatch.groups : 0;
        const u64 threads = dispatch.hash == w.pgm_hash ? dispatch.threads : 0;
        if (SotcWriteExtent::kEnabled && (groups != w.dispatch_groups || threads != w.dispatch_threads)) {
            return 1;
        }
    }
    for (const auto& w : entry.own_writes) {
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(w.addr, static_cast<u32>(w.size), true, false,
                                                                w.mark_offset, w.mark_size);
        if (buffer != w.buffer || offset != w.offset) {
            return 1;
        }
        texture_cache.InvalidateMemoryFromGPU(w.addr, w.size);
        if (w.note_again) {
            Core::MemoryManager::NoteHostWrite(w.addr, w.size, 7, w.pgm_hash);
        }
        if (SotcStats::kWriteMarks) { // the same statistic as BindBuffers (the write marks)
            const u64 threads = dispatch.hash == w.pgm_hash ? dispatch.threads : 0;
            SotcWriteExtent::NoteMark(w.pgm_hash, w.buf_index, w.addr, w.size,
                                      w.addr + (w.mark_size != ~0u ? w.mark_offset : 0),
                                      w.mark_size != ~0u ? w.mark_size : w.size, threads);
        }
        if (SotcArch::ShaderAddr::On() && w.size != 0) {
            sotc_op_writes.emplace_back(w.addr, w.addr + w.size); // as BindBuffers: ordering against later ops
        }
    }
    // Checked before the own stamps are noted: anything else the writes above noted (the texture cache, the buffer
    // cache) must still leave the entry fresh.
    if (!BindCacheEntryFresh(entry, reason)) {
        return 2;
    }
    if (SotcStats::kBindCacheRanges) {
        for (const auto& w : entry.own_writes) {
            BindCacheInvalidateFrame(w.addr, w.size, VideoCore::ImageId{});
        }
    }
    entry.checked_write_seq = write_stamps.Seq();
    return 0;
}

// sotc: SOTC_OP_COMPUTE_BIND. The stamps of the written bindings of a bind that was recorded. They are noted after its commit
// (the commit checks the stamps without them), so the entry it stored (made) is moved past them: they are its own writes.
// The other entries reading the ranges are dropped by their stamps, as a full bind's inline note dropped them.
void Rasterizer::ApplyOwnWriteStamps(BindCacheEntry* made) {
    if (bind_cache_writes_rec.empty()) {
        return;
    }
    if (SotcStats::kBindCacheRanges) {
        for (const auto& w : bind_cache_writes_rec) {
            BindCacheInvalidateFrame(w.addr, w.size, VideoCore::ImageId{});
        }
    }
    if (made) {
        made->checked_write_seq = write_stamps.Seq();
    }
    bind_cache_writes_rec.clear();
}

// sotc: SOTC_OP_COMPUTE_BIND=2. The calls a replay of the entry would make for the written bindings, against the calls the
// full bind just made (bind_cache_writes_rec). Counted by category; changes no state.
void Rasterizer::WriteVerify(const BindCacheEntry& ref) {
    const auto& cur = bind_cache_writes_rec;
    if (ref.own_writes.empty() && cur.empty()) {
        return;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::wr_verify_checks.fetch_add(1, std::memory_order_relaxed);
    }
    u32 bad = 0;
    const auto mark = [&](u32 category) { bad |= 1u << category; };
    if (ref.own_writes.size() != cur.size()) {
        mark(SotcStats::WvCount);
    } else {
        for (size_t i = 0; i < cur.size(); ++i) {
            const auto& a = ref.own_writes[i];
            const auto& b = cur[i];
            if (a.addr != b.addr || a.size != b.size) {
                mark(SotcStats::WvRange);
            }
            if (a.mark_offset != b.mark_offset || a.mark_size != b.mark_size ||
                (SotcWriteExtent::kEnabled &&
                 (a.dispatch_groups != b.dispatch_groups || a.dispatch_threads != b.dispatch_threads))) {
                mark(SotcStats::WvMark);
            }
            if (a.buffer != b.buffer || a.offset != b.offset) {
                mark(SotcStats::WvBinding);
            }
            if (a.note_again != b.note_again || a.pgm_hash != b.pgm_hash || a.buf_index != b.buf_index) {
                mark(SotcStats::WvNote);
            }
        }
    }
    if (SotcStats::kFpsLog) {
        for (u32 c = 0; c < SotcStats::WvCategories; ++c) {
            if (bad & (1u << c)) {
                SotcStats::wr_verify_mismatch[c].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

bool Rasterizer::TryBindCacheFrame(const Pipeline* pipeline, u64 hash, bool& uses_dma, u32 heap_mask) {
    // sotc: SOTC_BIND_CACHE_FRAME, see the comment above BindCacheSyncFrame. The rest follows
    // TryBindCache (SOTC_BIND_CACHE_XTICK): what a full bind does that a hit would skip is
    // replayed the same way.
    const bool stats = SotcStats::kFpsLog;
    if (stats) {
        SotcStats::bind_cache_lookups.fetch_add(1, std::memory_order_relaxed);
        const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
        auto& seen = bind_seen[(hash ^ (hash >> 29)) % bind_seen.size()];
        bool prev_frame = false;
        if (seen.hash == hash) {
            prev_frame = seen.frame_last == frame ? seen.frame_prev + 1 == frame
                                                  : seen.frame_last + 1 == frame;
            if (seen.frame_last != frame) {
                seen.frame_prev = seen.frame_last;
                seen.frame_last = frame;
            }
        } else {
            seen = {hash, frame, 0};
        }
        if (prev_frame) {
            SotcStats::bind_cache_seen_prev_frame.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const u64 targets_key = bind_cache_state.targets_key;
    const u64 lookup_key = BindCacheFrameKey(pipeline, hash, targets_key);
    if (stats) {
        // distinct keys looked up per game frame (does the table hold a frame's worth?)
        const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
        if (frame != bind_key_frame) {
            bind_key_frame = frame;
            SotcStats::bf_frames.fetch_add(1, std::memory_order_relaxed);
        }
        auto& seen_key = bind_key_seen[(lookup_key >> 20) % bind_key_seen.size()];
        if (seen_key.key != lookup_key || seen_key.frame != frame) {
            seen_key = {lookup_key, frame};
            SotcStats::bf_distinct_keys.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bind_cache_kept_write = false; // sotc: SOTC_BIND_CACHE_CONTENT, per lookup
    bind_cache_hold = false;
    bind_cache_verify_pending = false;
    BindCacheEntry* entry = BindCacheFindFrame(lookup_key, pipeline, hash, targets_key);
    const auto miss = [&](u32 reason) {
        if (stats) {
            SotcStats::bf_miss[reason].fetch_add(1, std::memory_order_relaxed);
            if (SotcStats::kBindCachePacket && pipeline->HasPacketRange()) { // sotc: SOTC_BIND_CACHE_PACKET
                SotcStats::pk_miss[reason].fetch_add(1, std::memory_order_relaxed);
            }
        }
        return false;
    };
    // The entry is found and no longer valid: dropped, the full bind makes a new one.
    const auto stale = [&](u32 reason) {
        // sotc: SOTC_BIND_CACHE_CONTENT=2. Today's rule drops the entry for a write the content rule keeps: it is held
        // instead (the full bind follows and CommitBindCacheFrame compares its result with the held entry).
        if (bind_cache_hold) {
            bind_cache_hold = false;
            bind_cache_verify_pending = true;
            bind_cache_verify_index = static_cast<size_t>(entry - frame_cache.data());
            if (stats) {
                SotcStats::bc_kept.fetch_add(1, std::memory_order_relaxed);
            }
            return miss(reason);
        }
        BindCacheDropFrame(*entry, reason);
        return miss(reason);
    };
    // sotc: site names the check that failed (SotcStats::BindRefuseSite, the SOTC_BIND_CACHE_FRAME report).
    const auto refuse = [&](bool drop, u32 site) {
        if (stats) {
            SotcStats::bind_cache_refused.fetch_add(1, std::memory_order_relaxed);
            SotcStats::bf_refuse[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (drop) {
            BindCacheDropFrame(*entry, SotcStats::BfRefused);
        }
        return miss(SotcStats::BfRefused);
    };
    if (!entry) {
        return miss(SotcStats::BfNoEntry);
    }
    if (u32 reason; !BindCacheEntryFresh(*entry, reason)) {
        return stale(reason);
    }
    // sotc: SOTC_N5_BINDLESS: the heap slots of the entry are as made (no release or eviction since).
    if (entry->bindless_gen != BindlessGeneration()) {
        return refuse(true, SotcStats::RfBindless);
    }
    // sotc: SOTC_BIND_CACHE_HEAP. The heap stages of the entry are the op's (same stages, a stored resolution each).
    if (entry->heap_mask != heap_mask) {
        return refuse(false, SotcStats::RfHeapShape);
    }
    for (u32 k = 0; k < 2; ++k) {
        if (!(heap_mask & (1u << k))) {
            continue;
        }
        const auto stages = pipeline->GetStages();
        if (entry->heap_stage[k] >= stages.size() || !stages[entry->heap_stage[k]]) {
            return refuse(false, SotcStats::RfHeapShape);
        }
    }
    // sotc: an entry made under the resource identity key: this draw's constants. The flat
    // buffer is copied into the stream ring again (patched as BindBuffers would), unless it is
    // the very data the entry copied last and the ring has not wrapped since; the copy's offset
    // goes into the descriptor below. The viewport words of the push data are set below too.
    if (!entry->flat_slots.empty()) {
        const auto stages = pipeline->GetStages();
        const u64 alignment = instance.StorageMinAlignment();
        auto& vk_buffer = buffer_cache.GetStreamBuffer();
        for (auto& slot : entry->flat_slots) {
            const Shader::Info* st = slot.stage < stages.size() ? stages[slot.stage] : nullptr;
            if (!st || st->FlatBuf().size() * sizeof(u32) != slot.size) {
                return refuse(true, SotcStats::RfFlatSize);
            }
            const u32* flat = FlatForBind(*st);
            const u64 hash = XXH3_64bits(flat, slot.size);
            // sotc: only a copy made in this tick may be reused: a later tick reusing it does not
            // extend the ring's watch over it, and a wrap would overwrite it while that tick's
            // draws still read it on the GPU (review of 2026-10-29).
            if (hash == slot.hash && slot.gen == buffer_cache.StreamGeneration() &&
                slot.tick == scheduler.CurrentTick()) {
                if (stats) {
                    SotcStats::bf_flat_reused.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                slot.offset = vk_buffer.Copy(flat, slot.size, alignment);
                slot.hash = hash;
                slot.gen = buffer_cache.StreamGeneration();
                slot.tick = scheduler.CurrentTick();
                if (stats) {
                    SotcStats::bf_flat_copied.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // sotc: SOTC_PACKET. The packet word of this flat buffer, as BindBuffers sets it (by the stage's kind).
            if (entry->packet) {
                const u64 address = vk_buffer.BufferDeviceAddress() + slot.offset;
                if (st->sw_stage == Shader::SwStage::Fragment) {
                    draw_packet.flatbuf_fragment_address = address;
                } else if (st->sw_stage == Shader::SwStage::Vertex ||
                           st->sw_stage == Shader::SwStage::Compute) {
                    draw_packet.flatbuf_address = address;
                }
            }
        }
    }
    // sotc: SOTC_BIND_CACHE_PACKET. The addressed ranges of a packet entry are this op's: the key left their base out, so
    // they are recomputed from the op's V#s before any check or replay below (PacketAddrRefresh).
    if (entry->packet && SotcStats::kBindCachePacket != 0 && !PacketAddrRefresh(*entry)) {
        if (stats) {
            SotcStats::pk_miss[SotcStats::PkAddrShape].fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    u64 current_tick = scheduler.CurrentTick();
    bool replay = entry->stamp_tick != current_tick;
    bool mapping_checked = false;
    const auto mapping_changed = [&] {
        mapping_checked = true;
        // sotc: the cut. The mappings were found unchanged under this map epoch; no MapMemory / UnmapMemory since, so the
        // clamps read the same (the epoch is read before the walk: a map during it leaves the stamp behind).
        const u64 epoch = MapEpoch();
        if (entry->map_ok_epoch == epoch) {
            return false;
        }
        for (const auto& r : entry->repairs) {
            if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
                return true;
            }
        }
        entry->map_ok_epoch = epoch;
        return false;
    };
    if (replay && mapping_changed()) {
        return refuse(true, SotcStats::RfMapReplay);
    }
    // A CPU-modified range is synchronized as ObtainBuffer does (the same arena and arguments:
    // the cached handle is still the binding, BindVersion), if the guest mapping under it is
    // still the one the entry was made for; the descriptors stay valid.
    u32 repaired = 0;
    for (const auto& r : entry->repairs) {
        if (!r.arena || !buffer_cache.IsRegionCpuModified(r.addr, r.size)) {
            continue;
        }
        if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
            return refuse(false, SotcStats::RfMapRepair);
        }
        buffer_cache.SynchronizeBinding(r.arena, r.addr, r.size, r.texel);
        ++repaired;
    }
    if (repaired != 0) {
        // The upload may have submitted, evicted or wrapped the stream ring, or reached a
        // BindCacheInvalidate for a range this entry reads: it must be fresh again, or the full
        // bind follows (nothing of this hit has been replayed yet).
        if (u32 reason; !BindCacheEntryFresh(*entry, reason)) {
            return stale(reason);
        }
        // sotc: the flat copies made or reused above must still be in this lap and tick of the
        // stream ring (the synchronization may have wrapped it or submitted).
        for (const auto& slot : entry->flat_slots) {
            if (slot.gen != buffer_cache.StreamGeneration() ||
                slot.tick != scheduler.CurrentTick()) {
                return refuse(true, SotcStats::RfFlatTick);
            }
        }
        if (stats) {
            SotcStats::bind_cache_xtick_repaired.fetch_add(1, std::memory_order_relaxed);
        }
        // The synchronization may have submitted: the stamps are for the tick being recorded.
        current_tick = scheduler.CurrentTick();
        replay = entry->stamp_tick != current_tick;
        if (replay && !mapping_checked && mapping_changed()) {
            return refuse(true, SotcStats::RfMapAfter);
        }
    }
    if (replay) {
        for (const auto& s : entry->samplers) {
            u32 slot = 0;
            const vk::Sampler handle = texture_cache.GetSampler(
                s.sharp, liverpool->DrawRegs().ta_bc_base, s.is_depth, &slot); // as BindTextures
            if (handle != s.handle || slot != s.slot) {
                return refuse(true, SotcStats::RfSampler);
            }
        }
    }
    // sotc: SOTC_PACKET. The addressed ranges of the packet op, touched as BindBuffers does; Touch may place or evict
    // buffers, so the entry is checked again after it. The packet words that do not come from flat_slots are the
    // ones the entry stored (its flat buffer copy is valid while the stream generation is unchanged, as its buffers).
    if (entry->packet) {
        if (!entry->refresh) {
            draw_packet.flatbuf_address = entry->packet_flat[0];
            draw_packet.flatbuf_fragment_address = entry->packet_flat[1];
        }
        PacketAddrReplay(entry->addr);
        if (u32 reason; !BindCacheEntryFresh(*entry, reason)) {
            return stale(reason);
        }
    }
    // sotc: SOTC_OP_COMPUTE_BIND. The written bindings of the entry and the read ranges' ordering (ReplayOwnWrites).
    {
        u32 replay_reason = 0;
        const int replayed = ReplayOwnWrites(*entry, replay_reason);
        if (replayed == 1) {
            return refuse(true, SotcStats::RfWriteReplay);
        }
        if (replayed == 2) {
            return stale(replay_reason);
        }
    }
    {
        const auto texture_lock = texture_cache.BatchLock();
        // The guest may unmap (unregister) images from its own threads: checked under the lock,
        // before the entry's images are touched (a freed image's slot may hold another image).
        if (texture_cache.SamplerGeneration() != bind_cache_state.texture_generation) {
            BindCacheFlush(SotcStats::BindCacheResources);
            return miss(SotcStats::BfFlush);
        }
        if (u32 reason; !BindCacheStampsFresh(*entry, reason)) {
            return stale(reason);
        }
        // sotc: SOTC_BIND_CACHE_HEAP. The held heap resolutions: each BindSet must still be live (the events are applied
        // first: HeapLive), and the decline rule must still allow the bind of its images. Nothing is replayed before both
        // checks of every stage pass, so a refusal here leaves the bind as it was.
        for (u32 k = 0; k < 2 && heap_mask; ++k) {
            if (!(heap_mask & (1u << k))) {
                continue;
            }
            if (!SotcArch::TableMemo::HeapLive(texture_cache, entry->heap[k].ref)) {
                SotcArch::TableMemo::NoteHeapDead();
                return refuse(true, SotcStats::RfHeapDead);
            }
            if (HeapHoldDecline(pipeline, *entry, k) != 0) {
                SotcArch::TableMemo::NoteHeapDeclined();
                return refuse(false, SotcStats::RfHeapDecline);
            }
        }
        // What the full bind would do differently: an image to upload again or re-track
        // (TextureCache::UpdateImage), one replaced since (needs_rebind), or one that became a
        // target or storage image of this draw (general / feedback loop layouts).
        for (const auto& img : entry->images) {
            const auto& image = texture_cache.GetImage(img.id);
            const bool ok =
                True(image.flags & VideoCore::ImageFlagBits::Registered) &&
                image.image_uid == img.uid &&
                False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
                image.track_addr == image.info.guest_address &&
                image.track_addr_end == image.info.guest_address + image.info.guest_size &&
                !image.binding.needs_rebind && !image.binding.force_general &&
                !image.binding.is_target && img.id != db_desc.first;
            if (!ok) {
                return refuse(false, SotcStats::RfImage);
            }
        }
        // sotc: SOTC_BIND_CACHE_PACKET=2 found image info mismatches of this kind: SetBackingSamples swaps an image's
        // backing without a new uid, and the entry's descriptors name the view of the backing it was made with. A hit
        // would bind that view (and the old memory) where the full bind names the current backing's view: refused.
        if (entry->image_info_backings.size() != entry->image_info_images.size()) {
            return refuse(false, SotcStats::RfImage);
        }
        for (size_t i = 0; i < entry->image_info_images.size(); ++i) {
            const VideoCore::ImageId id = entry->image_info_images[i];
            if (id && static_cast<const void*>(texture_cache.GetImage(id).backing) !=
                          entry->image_info_backings[i]) {
                return refuse(false, SotcStats::RfImage);
            }
        }
        // sotc: SOTC_BIND_CACHE_HEAP. The index tables of the held resolutions, published for this tick and stamped as a
        // BindSet hit stamps them (HeapReplay). Before the transitions below: nothing of the hit is replayed past this point.
        for (u32 k = 0; k < 2 && heap_mask; ++k) {
            if (!(heap_mask & (1u << k))) {
                continue;
            }
            const Shader::Info* st = pipeline->GetStages()[entry->heap_stage[k]];
            const u32 index_count = static_cast<u32>(st->images.size() + st->samplers.size());
            bind_hold_addr[k] = SotcArch::TableMemo::HeapReplay(texture_cache, entry->heap[k].ref, index_count,
                                                                scheduler.CurrentTick(), &bind_hold_binds[k]);
            if (bind_hold_addr[k].device_address == 0) {
                return refuse(true, SotcStats::RfHeapPublish);
            }
            bind_hold_src[k] = entry;
            SotcArch::TableMemo::NoteHeapHit(SotcArch::TableMemo::HeapHoldMode() == 1);
        }
        // The side effects of BindTextures' second pass: bound for this draw (BeginRendering's
        // feedback test, ResetBindings), and in the layout the descriptors name, with a barrier
        // when another command changed it since.
        for (const auto& img : entry->images) {
            auto& image = texture_cache.GetImage(img.id);
            image.binding.is_bound = 1u;
            bound_images.emplace_back(img.id);
            needs_barrier |= runtime.Transit(&image, img.layout, img.stages, img.access, img.range);
            image.usage.texture = 1u;
            if (replay) {
                texture_cache.NoteBoundUse(img.id);
            }
        }
    }
    entry->stamp_tick = current_tick;
    if (const u16 epoch = buffer_cache.UseEpoch(); entry->stamp_epoch != epoch) {
        for (const auto& r : entry->repairs) {
            if (r.arena) {
                buffer_cache.NoteBindUse(r.addr, r.size);
            }
        }
        entry->stamp_epoch = epoch;
    }
    if (stats) {
        if (replay) {
            SotcStats::bind_cache_xtick_replays.fetch_add(1, std::memory_order_relaxed);
        }
        if (entry->made_tick != current_tick) {
            SotcStats::bind_cache_xtick_hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (entry->made_frame != SotcStats::cp_frame.load(std::memory_order_relaxed)) {
            SotcStats::bf_cross_frame_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // The buffer side of BindBuffers: barrier against earlier writes, and the reads recorded
    // after the draw (ResetBindings).
    for (const auto& buf : entry->buffers) {
        buffer_barrier_need |= Passes::BufferNeed( // sotc: SOTC_PASSES (see BindVertexBuffers)
            BarrierStatic::BufferChecks() &&
            runtime.IsBufferAccessed(buf.buffer, buf.offset, buf.size, buf.is_written));
        bound_buffers.emplace_back(buf);
    }
    for (const u32 bank : entry->lod_banks) {
        SotcLod::Mark(bank);
    }

    buffer_infos.assign(entry->buffer_infos.begin(), entry->buffer_infos.end());
    image_infos.assign(entry->image_infos.begin(), entry->image_infos.end());
    std::copy(entry->image_info_images.begin(), entry->image_info_images.end(),
              image_info_images.begin());
    set_writes.assign(entry->writes.begin(), entry->writes.end());
    set_write_sets.assign(entry->write_sets.begin(), entry->write_sets.end());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const u32 info = entry->write_info[i];
        auto& write = set_writes[i];
        write.dstSet = VK_NULL_HANDLE;
        if (info & 0x80000000u) {
            write.pBufferInfo = buffer_infos.data() + (info & 0x7FFFFFFFu);
        } else {
            write.pImageInfo = image_infos.data() + info;
        }
    }
    set_write_index = static_cast<u32>(set_writes.size());
    push_data = entry->push_data;
    // The viewport words are registers of this draw (the buffer offsets stay the entry's).
    push_data.xoffset = bind_input_push.xoffset;
    push_data.yoffset = bind_input_push.yoffset;
    push_data.xscale = bind_input_push.xscale;
    push_data.yscale = bind_input_push.yscale;
    for (const auto& slot : entry->flat_slots) {
        buffer_infos[slot.info].offset = slot.offset;
    }
    bindless_slots = entry->bindless_slots;
    if (bindless_images) { // sotc: SOTC_N5_BINDLESS: the replayed slots are used by this tick
        for (const auto& ref : entry->bindless_refs) {
            bindless_images->Use(ref.key, ref.slot);
            if (bindless_images->Mode() == 2 && ref.slot != 0) {
                bindless_images->Verify(ref.key, ref.slot, ref.view);
            }
        }
    }
    entry->last_use = ++bind_cache_clock;

    uses_dma = entry->uses_dma;
    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }
    if (stats) {
        SotcStats::bind_cache_hits.fetch_add(1, std::memory_order_relaxed);
        if (bind_cache_kept_write) { // sotc: SOTC_BIND_CACHE_CONTENT, a hit kept across a GPU write
            SotcStats::bc_kept.fetch_add(1, std::memory_order_relaxed);
        }
        if (!entry->own_writes.empty()) { // sotc: SOTC_OP_COMPUTE_BIND, a hit that replayed written bindings
            SotcStats::wr_hits.fetch_add(1, std::memory_order_relaxed);
            if (pipeline->IsCompute()) {
                SotcStats::wr_compute_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // sotc: SOTC_PACKET. The hit's addressed ranges are this op's: CommitFastBind keeps them for the fast binds.
    packet_addr_rec.assign(entry->addr.begin(), entry->addr.end());
    return true;
}

Rasterizer::BindCacheEntry* Rasterizer::CommitBindCacheFrame(const Pipeline* pipeline, u64 hash, bool uses_dma,
                                                             size_t first_bound_buffer) {
    // sotc: SOTC_BIND_CACHE_FRAME. Not stored when the bind did something a hit would not repeat,
    // or when what the entry depends on moved while the bind ran (a submit, a new image over a
    // range it looked up, a GPU write to what it reads...).
    const auto& state = bind_cache_state;
    if (SotcStats::kFpsLog && !bind_cache_writes_rec.empty()) { // sotc: SOTC_OP_COMPUTE_BIND, written binds recorded
        SotcStats::wr_recorded.fetch_add(1, std::memory_order_relaxed);
        if (pipeline->IsCompute()) {
            SotcStats::wr_compute_recorded.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto vo_target = [&] {
        const auto id = state.targets[0];
        return id && texture_cache.GetImage(id).usage.vo_surface;
    };
    // Under the resource identity key the flat buffer copies are refreshed by every hit (their ring
    // generation is per slot), so the entry itself does not depend on the ring.
    const bool uses_stream = bind_cache_stream_rec && !bind_level2;
    if (bind_cache_uncacheable || vo_target() || scheduler.CurrentTick() != state.tick ||
        bind_cache_write_epoch.load(std::memory_order_relaxed) != state.write_epoch ||
        buffer_cache.BindVersion() != state.buffer_generation ||
        texture_cache.SamplerGeneration() != state.texture_generation ||
        (uses_stream && buffer_cache.StreamGeneration() != state.stream_generation) ||
        set_writes.size() != set_write_index) {
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_uncacheable.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }
    // sotc: SOTC_BIND_CACHE_HEAP. A heap op's entry holds its resolutions by reference (a BindSet): one without a live
    // reference is not stored (a hit could not check it).
    for (u32 k = 0; k < 2; ++k) {
        if ((bind_rec_heap_mask & (1u << k)) && !bind_rec_heap[k].ref.valid) {
            return nullptr;
        }
    }
    // The ranges whose image set the entry depends on: the T# lookups, the sampled images and
    // the texel buffers (an image registered over one makes ObtainBuffer re-tile it).
    auto& tex_watch = bind_cache_tex_watch_rec;
    tex_watch.assign(bind_cache_lookups_rec.begin(), bind_cache_lookups_rec.end());
    tex_watch.insert(tex_watch.end(), bind_cache_texel_rec.begin(), bind_cache_texel_rec.end());
    for (const auto& img : bind_cache_images_rec) {
        tex_watch.emplace_back(img.guest_lo, img.guest_hi);
    }
    // Checked against the sequence numbers read when the bind began: what moved during it
    // (including what this very bind did, e.g. creating an image) is not trusted.
    u64 checked_write = bind_rec_write_seq;
    u64 checked_tex = bind_rec_tex_seq;
    u64 checked_flag = bind_rec_flag_seq;
    // sotc: SOTC_BIND_CACHE_CONTENT. Mode 2 stores what mode 0 stores (its rule decides here); mode 1 stores by the
    // content rule. The verification below is independent of the store decision.
    const int content = SotcStats::kBindCacheContent;
    const int rule = content == 2 ? 0 : content;
    bool kept = false, hold = false;
    u32 reason = 0;
    bool stored = !(bind_cache_map_dep_rec && bind_rec_map_epoch != MapEpoch());
    if (stored && rule == 0) {
        stored = !(bind_cache_tail_rec &&
                   bind_rec_gpu_epoch != VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed));
    }
    if (stored) {
        stored = BindCacheStampsOk(write_stamps, write_image_stamps, texture_cache,
                                   bind_cache_ranges_rec, bind_cache_images_rec, tex_watch,
                                   checked_write, checked_tex, reason, rule, checked_flag, kept,
                                   hold);
    }
    const u64 key = BindCacheFrameKey(pipeline, hash, state.targets_key);
    const size_t base = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 52);
    size_t chosen = ~size_t{0};
    bool occupied = false;
    // A match is replaced (a refused one); else an empty slot; else a stale one (made before a
    // flush); else the least recently used of the probe window.
    u64 best_score = ~0ULL;
    for (u32 probe = 0; probe < BindCacheFrameProbe; ++probe) {
        const size_t index = (base + probe) & (BindCacheFrameEntries - 1);
        if (frame_keys[index] == 0) {
            if (best_score != 0) {
                chosen = index;
                occupied = false;
                best_score = 0;
            }
            continue;
        }
        const auto& e = frame_cache[index];
        if (frame_keys[index] == key && e.pipeline == pipeline && e.hash == hash &&
            e.targets_key == state.targets_key) {
            chosen = index;
            occupied = true;
            best_score = 0;
            break;
        }
        const u64 score = e.flush_gen != bind_cache_flush_gen ? 1 : 2 + e.last_use;
        if (score < best_score) {
            chosen = index;
            occupied = true;
            best_score = score;
        }
    }
    // sotc: SOTC_BIND_CACHE_CONTENT=2. The full bind that a held lookup led here is compared with the entry that lookup
    // held (same key, same slot), unless the result moved over its tex watch during the bind.
    const bool verify = content == 2 && bind_cache_verify_pending &&
                        chosen == bind_cache_verify_index && frame_keys[chosen] == key;
    bind_cache_verify_pending = false;
    if (verify) {
        bool moved = false;
        for (const auto& [lo, hi] : tex_watch) {
            if (VideoCore::GpuFlagStamps().ChangedSince(lo, hi, bind_rec_flag_seq)) {
                moved = true;
                break;
            }
        }
        if (moved) {
            if (SotcStats::kFpsLog) {
                SotcStats::bc_skipped_moved.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            BindCacheVerify(frame_cache[chosen], first_bound_buffer);
        }
    }
    if (!stored) {
        if (SotcStats::kFpsLog) {
            SotcStats::bf_not_stored_moved.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }
    auto* entry = &frame_cache[chosen];
    if (occupied) {
        if (SotcStats::kFpsLog && frame_keys[chosen] != key) {
            SotcStats::bf_replaced.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        ++bind_cache_live;
    }
    frame_keys[chosen] = 0; // not findable while it is rewritten
    entry->pipeline = pipeline;
    entry->hash = hash;
    entry->last_use = ++bind_cache_clock;
    entry->uses_dma = uses_dma;
    entry->push_data = push_data;
    entry->bindless_slots = bindless_slots;
    entry->bindless_refs = bind_bindless_rec;
    entry->bindless_gen = bind_bindless_gen;
    // sotc: SOTC_BIND_CACHE_HEAP. The heap resolutions of the op, held by reference (the stage positions and the binds).
    entry->heap_mask = bind_rec_heap_mask;
    for (u32 k = 0; k < 2; ++k) {
        if (bind_rec_heap_mask & (1u << k)) {
            entry->heap[k] = bind_rec_heap[k];
            entry->heap_stage[k] = bind_rec_heap_stage[k];
            SotcArch::TableMemo::NoteHeapStored();
        }
    }
    entry->writes.assign(set_writes.begin(), set_writes.end());
    entry->write_sets.assign(set_write_sets.begin(), set_write_sets.end());
    entry->write_info.resize(set_writes.size());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const auto& write = set_writes[i];
        if (write.descriptorType == vk::DescriptorType::eStorageBuffer) {
            entry->write_info[i] =
                0x80000000u | static_cast<u32>(write.pBufferInfo - buffer_infos.data());
        } else {
            entry->write_info[i] = static_cast<u32>(write.pImageInfo - image_infos.data());
        }
    }
    entry->buffer_infos.assign(buffer_infos.begin(), buffer_infos.end());
    entry->image_infos.assign(image_infos.begin(), image_infos.end());
    entry->image_info_images.assign(image_info_images.begin(),
                                    image_info_images.begin() + image_infos.size());
    entry->buffers.assign(bound_buffers.begin() + first_bound_buffer, bound_buffers.end());
    entry->images.assign(bind_cache_images_rec.begin(), bind_cache_images_rec.end());
    entry->lod_banks.assign(bind_cache_lod_rec.begin(), bind_cache_lod_rec.end());
    entry->ranges.assign(bind_cache_ranges_rec.begin(), bind_cache_ranges_rec.end());
    entry->repairs.assign(bind_cache_repairs_rec.begin(), bind_cache_repairs_rec.end());
    entry->samplers.assign(bind_cache_samplers_rec.begin(), bind_cache_samplers_rec.end());
    entry->tex_watch.assign(tex_watch.begin(), tex_watch.end());
    // sotc: SOTC_OP_COMPUTE_BIND. The written bindings of the bind, replayed by a hit (ReplayOwnWrites).
    entry->own_writes.assign(bind_cache_writes_rec.begin(), bind_cache_writes_rec.end());
    // sotc: SOTC_BIND_CACHE_PACKET. The backing each image info's view belongs to (a hit checks it).
    entry->image_info_backings.resize(image_infos.size());
    for (size_t i = 0; i < image_infos.size(); ++i) {
        entry->image_info_backings[i] =
            ImageInfoBacking(image_info_images[i], image_infos[i].imageView);
    }
    // sotc: SOTC_PACKET. A packet op's hit refreshes the packet words (flat_slots, or these two when there are none)
    // and replays its addressed ranges (TryBindCacheFrame).
    entry->packet = pipeline->HasPacketRange();
    entry->packet_flat = {draw_packet.flatbuf_address, draw_packet.flatbuf_fragment_address};
    entry->addr.assign(packet_addr_rec.begin(), packet_addr_rec.end());
    // sotc: SOTC_BIND_CACHE_PACKET: where each of them came from (PacketAddrRefresh on a hit of another base).
    entry->addr_src.assign(packet_addr_src_rec.begin(), packet_addr_src_rec.end());
    entry->made_tick = state.tick;
    entry->stamp_tick = state.tick; // the full bind just made this tick's stamps
    entry->stamp_epoch = 0;         // block stamps due at the first hit
    entry->key = key;
    entry->targets_key = state.targets_key;
    entry->flush_gen = bind_cache_flush_gen;
    entry->buffer_version = state.buffer_generation;
    entry->stream_gen = state.stream_generation;
    entry->uses_stream = uses_stream;
    entry->refresh = bind_level2;
    if (bind_level2) {
        entry->flat_slots.assign(bind_cache_flat_rec.begin(), bind_cache_flat_rec.end());
    } else {
        entry->flat_slots.clear();
    }
    entry->map_dependent = bind_cache_map_dep_rec;
    entry->map_epoch = bind_rec_map_epoch;
    entry->tail_dependent = bind_cache_tail_rec;
    entry->gpu_epoch = bind_rec_gpu_epoch;
    entry->checked_write_seq = checked_write;
    entry->checked_tex_seq = checked_tex;
    entry->checked_flag_seq = checked_flag; // sotc: SOTC_BIND_CACHE_CONTENT
    entry->made_frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    entry->valid = true;
    frame_keys[chosen] = key;
    if (SotcStats::kFpsLog) {
        SotcStats::bf_stored.fetch_add(1, std::memory_order_relaxed);
        SotcStats::bf_live.store(bind_cache_live, std::memory_order_relaxed);
        if (!bind_cache_writes_rec.empty()) { // sotc: SOTC_OP_COMPUTE_BIND
            SotcStats::wr_stored.fetch_add(1, std::memory_order_relaxed);
            if (pipeline->IsCompute()) {
                SotcStats::wr_compute_stored.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return entry;
}

void Rasterizer::BindCacheVerify(const BindCacheEntry& old, size_t first_bound_buffer) {
    // sotc: SOTC_BIND_CACHE_CONTENT=2. The full bind's records and descriptors (still in the members and records the
    // commit reads) against the entry a hit would have replayed: kept by the content rule, held here. Counted by
    // category; nothing is changed.
    u32 diffs = 0;
    const auto diff = [&](SotcStats::BindContentDiff cat) { diffs |= 1u << cat; };
    // Descriptors: the buffer and image infos, the image each one came from, the set writes that name them.
    if (buffer_infos.size() != old.buffer_infos.size() ||
        image_infos.size() != old.image_infos.size() || set_writes.size() != old.writes.size() ||
        old.write_info.size() != old.writes.size() || image_info_images.size() < image_infos.size()) {
        diff(SotcStats::CdDescriptors);
    } else {
        // The stream ring copies (flat buffers, LDS, the block table) sit at their own offset in each op, as in
        // PacketVerify: a hit re-copies them, so only their buffer and range must match.
        const vk::Buffer stream = buffer_cache.GetStreamBuffer().Handle();
        for (size_t i = 0; i < buffer_infos.size(); ++i) {
            const auto& a = buffer_infos[i];
            const auto& b = old.buffer_infos[i];
            if (a.buffer != b.buffer || (a.buffer != stream && a.offset != b.offset) || a.range != b.range) {
                diff(SotcStats::CdDescriptors);
                break;
            }
        }
        for (size_t i = 0; i < image_infos.size(); ++i) {
            const auto& a = image_infos[i];
            const auto& b = old.image_infos[i];
            if (a.sampler != b.sampler || a.imageView != b.imageView ||
                a.imageLayout != b.imageLayout || image_info_images[i] != old.image_info_images[i]) {
                diff(SotcStats::CdDescriptors);
                break;
            }
        }
        for (size_t i = 0; i < set_writes.size(); ++i) {
            const auto& w = set_writes[i];
            const auto& o = old.writes[i];
            const u32 info =
                w.descriptorType == vk::DescriptorType::eStorageBuffer
                    ? 0x80000000u | static_cast<u32>(w.pBufferInfo - buffer_infos.data())
                    : static_cast<u32>(w.pImageInfo - image_infos.data());
            if (w.dstBinding != o.dstBinding || w.descriptorType != o.descriptorType ||
                w.descriptorCount != o.descriptorCount || info != old.write_info[i]) {
                diff(SotcStats::CdDescriptors);
                break;
            }
        }
    }
    if (std::memcmp(&push_data, &old.push_data, sizeof(push_data)) != 0) {
        diff(SotcStats::CdPush);
    }
    // Bound buffer records: the ranges and their barrier flags, as the hit replays them.
    const size_t num_buffers = bound_buffers.size() - first_bound_buffer;
    if (num_buffers != old.buffers.size()) {
        diff(SotcStats::CdBuffers);
    } else {
        for (size_t i = 0; i < num_buffers; ++i) {
            const auto& a = bound_buffers[first_bound_buffer + i];
            const auto& b = old.buffers[i];
            if (a.buffer != b.buffer || a.offset != b.offset || a.size != b.size ||
                a.is_written != b.is_written || a.addr != b.addr) {
                diff(SotcStats::CdBuffers);
                break;
            }
        }
    }
    // Image records: the layout, stages, access and range each image is transited to, and its identity.
    if (bind_cache_images_rec.size() != old.images.size()) {
        diff(SotcStats::CdImages);
    } else {
        for (size_t i = 0; i < old.images.size(); ++i) {
            const auto& a = bind_cache_images_rec[i];
            const auto& b = old.images[i];
            if (a.id != b.id || a.layout != b.layout || a.stages != b.stages ||
                a.access != b.access || a.uid != b.uid || a.guest_lo != b.guest_lo ||
                a.guest_hi != b.guest_hi || std::memcmp(&a.range, &b.range, sizeof(a.range)) != 0) {
                diff(SotcStats::CdImages);
                break;
            }
        }
    }
    // The other records a hit replays: LOD banks, samplers, bindless heap references.
    if (bind_cache_lod_rec != old.lod_banks) {
        diff(SotcStats::CdOther);
    }
    if (bind_cache_samplers_rec.size() != old.samplers.size()) {
        diff(SotcStats::CdOther);
    } else {
        for (size_t i = 0; i < old.samplers.size(); ++i) {
            const auto& a = bind_cache_samplers_rec[i];
            const auto& b = old.samplers[i];
            if (a.handle != b.handle || a.slot != b.slot || a.is_depth != b.is_depth) {
                diff(SotcStats::CdOther);
                break;
            }
        }
    }
    if (bind_bindless_rec.size() != old.bindless_refs.size() ||
        bind_bindless_gen != old.bindless_gen) {
        diff(SotcStats::CdOther);
    }
    // The watched ranges: they decide what later writes refuse, not what the bind is.
    if (bind_cache_ranges_rec.size() != old.ranges.size() ||
        !std::equal(bind_cache_ranges_rec.begin(), bind_cache_ranges_rec.end(), old.ranges.begin()) ||
        bind_cache_tex_watch_rec != old.tex_watch) {
        diff(SotcStats::CdRanges);
    }
    if (SotcStats::kFpsLog) {
        SotcStats::bc_verified.fetch_add(1, std::memory_order_relaxed);
        if (diffs) {
            SotcStats::bc_diff_binds.fetch_add(1, std::memory_order_relaxed);
        }
        for (u32 c = 0; c < SotcStats::CdCats; ++c) {
            if (diffs & (1u << c)) {
                SotcStats::bc_diff[c].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void Rasterizer::RebindVertexIfSubmitted(const GraphicsPipeline* pipeline, u64& vertex_tick,
                                         bool is_indexed,
                                         const std::function<void()>& bind_index,
                                         const SotcVertexExtent::Draw* extent) {
    // sotc: a submit while binding (arena allocation, readback, release) moved recording to a new
    // command buffer, and the vertex input, vertex and index buffers recorded before it stayed in
    // the old one: the draw would run without them (broken vertices; review of 2026-10-27).
    // Record them again; a second submit while doing so is rare, so retry a few times.
    for (int tries = 0; tries < 4 && vertex_tick != scheduler.CurrentTick(); ++tries) {
        vertex_tick = scheduler.CurrentTick();
        BindVertexBuffers(pipeline, extent); // sotc: SOTC_VERTEX_EXTENT, the same draw's extent
        if (is_indexed) {
            bind_index();
        }
    }
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline,
                                   const SotcVertexExtent::Draw* extent) {
    const auto& regs = liverpool->DrawRegs();
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs, unless this command buffer already holds them.
        // sotc: the cache is voided with the generation of DynamicState (SOTC_DEDUP_STATE).
        auto& dynamic = scheduler.GetDynamicState();
        auto& cache = dynamic.vertex_input_cache;
        static_assert(MaxVertexBufferCount <= DynamicState::VertexInputCache::MaxEntries);
        const bool same = kDedupState && cache.generation == dynamic.generation &&
                          std::ranges::equal(cache.attributes, attributes) &&
                          std::ranges::equal(cache.bindings, bindings);
        if (!same) {
            const auto cmdbuf = scheduler.CommandBuffer();
            cmdbuf.setVertexInputEXT(bindings, attributes);
            if (kDedupState) {
                cache.generation = dynamic.generation;
                cache.attributes.assign(attributes.begin(), attributes.end());
                cache.bindings.assign(bindings.begin(), bindings.end());
            }
        }
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    // sotc: SOTC_VERTEX_EXTENT: the size each binding is bound and synced with, the V# size unless the draw's vertices
    // end before it (sotc_vertex_extent.h). The base is never changed, so every fetch address is the same.
    VertexInputs<u64> sizes{};
    for (const auto& buffer : guest_buffers) {
        sizes.push_back(buffer.GetSize());
    }
    if (extent && extent->enabled) {
        auto& counters = SotcVertexExtent::g_counters;
        counters.draws.fetch_add(1, std::memory_order_relaxed);
        // The last VertexIndex the draw fetches; false when it is not known exactly (the size is then kept).
        const auto last_vertex = [&](u64& last) -> bool {
            const u32 count = regs.num_indices;
            if (count == 0 || !extent->vertex_exact) {
                return false;
            }
            if (!extent->indexed) {
                // draw(count, instances, firstVertex = vertex_offset): VertexIndex in [vertex_offset, vertex_offset + count).
                last = u64{extent->vertex_offset} + count - 1;
                return true;
            }
            // drawIndexed(count, ..., vertexOffset): VertexIndex = index + vertexOffset, with the indices BindIndexBuffer
            // binds (index_offset, count). Primitive restart values are not fetched; a draw with restart keeps its size.
            if ((regs.enable_primitive_restart & 1) != 0 || count > SotcVertexExtent::kMaxIndexScan) {
                return false;
            }
            const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
            const u32 index_size = index16 ? 2u : 4u;
            const VAddr index_address =
                regs.index_base_address.Address<VAddr>() + u64{extent->index_offset} * index_size;
            const u64 index_bytes = u64{count} * index_size;
            // The CPU reads guest memory only where the GPU wrote nothing unsynced (as the indirect arguments do).
            if (buffer_cache.IsRegionGpuModified(index_address, index_bytes) ||
                GuestWriteOverlap(index_address, index_bytes)) {
                return false;
            }
            counters.scans.fetch_add(1, std::memory_order_relaxed);
            u32 lowest = ~0u;
            u32 highest = 0;
            alignas(8) u8 raw[1024 * sizeof(u32)];
            for (u32 done = 0; done < count; done += 1024) {
                const u32 n = std::min<u32>(1024, count - done);
                if (!memory->TryReadBacking(index_address + u64{done} * index_size, raw,
                                            u64{n} * index_size)) {
                    return false;
                }
                for (u32 k = 0; k < n; ++k) {
                    u32 index = 0;
                    if (index16) {
                        u16 narrow = 0;
                        std::memcpy(&narrow, raw + k * sizeof(u16), sizeof(u16));
                        index = narrow;
                    } else {
                        std::memcpy(&index, raw + k * sizeof(u32), sizeof(u32));
                    }
                    lowest = std::min(lowest, index);
                    highest = std::max(highest, index);
                }
            }
            const s64 base_vertex = static_cast<s32>(extent->vertex_offset);
            if (s64{lowest} + base_vertex < 0) {
                return false; // a negative VertexIndex fetches before the base: keep the V# size
            }
            last = static_cast<u64>(s64{highest} + base_vertex);
            return true;
        };
        bool vertex_looked = false;
        bool vertex_known = false;
        u64 last_vertex_index = 0;
        for (size_t i = 0; i < guest_buffers.size() && i < bindings.size(); ++i) {
            const auto& buffer = guest_buffers[i];
            const u64 full = buffer.GetSize();
            if (buffer.base_address == 0 || full == 0) {
                continue;
            }
            // Each guest attribute is its own binding at attribute offset 0 (GetVertexInputs), so one fetch of
            // element v reads [v * stride, v * stride + kMaxElemBytes) from the base; the end is the last fetch's.
            const u64 stride = buffer.GetStride();
            u64 end = 0; // 0: not known, the V# size stays
            if (bindings[i].inputRate == vk::VertexInputRate::eVertex) {
                if (!vertex_looked) {
                    vertex_looked = true;
                    vertex_known = last_vertex(last_vertex_index);
                }
                if (vertex_known) {
                    end = SotcVertexExtent::FetchEnd(stride, last_vertex_index);
                }
            } else if (extent->instance_exact && regs.num_instances.NumInstances() != 0 &&
                       bindings[i].divisor != 0) {
                // The fetch index is instance / divisor, with instance over the draw's instances, from firstInstance.
                const u64 last_instance = u64{extent->instance_offset} +
                                          regs.num_instances.NumInstances() - 1;
                end = SotcVertexExtent::FetchEnd(stride, last_instance / bindings[i].divisor);
            }
            if (end != 0) {
                const u64 narrowed = SotcVertexExtent::Narrow(full, end);
                if (narrowed < full) {
                    sizes[i] = narrowed;
                    counters.bindings.fetch_add(1, std::memory_order_relaxed);
                    counters.bytes.fetch_add(full - narrowed, std::memory_order_relaxed);
                }
            }
        }
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (size_t i = 0; i < guest_buffers.size(); ++i) {
        const auto& buffer = guest_buffers[i];
        if (buffer.base_address != 0 && sizes[i] > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + sizes[i]);
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        if (size == 0) {
            // Start is not mapped (garbage vertex sharp): keep a small range instead of GBs.
            size = std::min<u64>(range.GetSize(), 4096);
        }
        SotcVerify::BindKindScope verify_kind{SotcVerify::BindKind::Vertex}; // sotc: SOTC VERIFY BYTES
        // sotc: SOTC_ADDR_VERTEX: the range's buffer from the page table when it lies in one backing (ObtainRead).
        std::tie(range.buffer, range.offset) =
            SotcArch::AddrTable::ObtainRead(buffer_cache, scheduler, range.base_address,
                                            static_cast<u32>(size), SotcArch::AddrTable::VertexPath::Vertex);
        if (SotcArch::AddrTable::VertexOn()) { // sotc: SOTC_ADDR_VERTEX, ordering as for shader-addressed reads
            buffer_barrier_need |= GuestWriteOverlap(range.base_address, size);
        }
        buffer_barrier_need |= Passes::BufferNeed( // sotc: SOTC_PASSES: Passes::Decide covers it when On()
            BarrierStatic::BufferChecks() && runtime.IsBufferAccessed(range.buffer, range.offset, size));
        fixed_function_reads.push_back({range.buffer, range.offset, static_cast<u32>(size), false});
    }

    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (size_t i = 0; i < guest_buffers.size(); ++i) {
        const auto& buffer = guest_buffers[i];
        if (buffer.base_address != 0 && sizes[i] > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(sizes[i]);
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void Rasterizer::BindIndexBuffer(u32 index_offset, u32 extent) {
    const auto& regs = liverpool->DrawRegs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    // sotc: SOTC_INDEX_EXTENT. extent (the indices the draw reads from this base, 0 = unknown) shortens the size of the
    // bind only: the base is the same, and the draws' firstIndex is relative to it. Never longer than the register window.
    u32 index_count = regs.num_indices;
    if (extent != 0) {
        IndexExtentStats::binds.fetch_add(1, std::memory_order_relaxed);
        if (extent < index_count) {
            IndexExtentStats::narrowed.fetch_add(1, std::memory_order_relaxed);
            IndexExtentStats::bytes_saved.fetch_add(u64{index_count - extent} * index_size,
                                                    std::memory_order_relaxed);
            index_count = extent;
        }
    }
    const u32 index_buffer_size = index_count * index_size;
    SotcVerify::BindKindScope verify_kind{SotcVerify::BindKind::Index}; // sotc: SOTC VERIFY BYTES
    // sotc: SOTC_ADDR_VERTEX: the index range's buffer from the page table when it lies in one backing (ObtainRead).
    const auto [buffer, offset] =
        SotcArch::AddrTable::ObtainRead(buffer_cache, scheduler, index_address, index_buffer_size,
                                        SotcArch::AddrTable::VertexPath::Index);
    if (SotcArch::AddrTable::VertexOn()) { // sotc: SOTC_ADDR_VERTEX, ordering as for shader-addressed reads
        buffer_barrier_need |= GuestWriteOverlap(index_address, index_buffer_size);
    }
    buffer_barrier_need |= Passes::BufferNeed( // sotc: SOTC_PASSES: Passes::Decide covers it when On()
        BarrierStatic::BufferChecks() && runtime.IsBufferAccessed(buffer, offset, index_buffer_size));
    fixed_function_reads.push_back({buffer, offset, index_buffer_size, false});
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(buffer->Handle(), offset, index_type);
}

void Rasterizer::FlushPeriodic() {
    // Readbacks drain the GPU, but between them the GPU only gets work when a readback submits:
    // it sits idle while the command processor records. Submitting every N draws/dispatches lets
    // it run the frame during the recording. A submit inside a render pass cuts it, so wait for a
    // command outside one, up to 4 * N. From periodic_flush_commands of Pink-shadPS4
    // (luizgustavs). SOTC_FLUSH_EVERY=N sets it, 0 = off.
    static const u32 every = [this] {
        if (const char* v = std::getenv("SOTC_FLUSH_EVERY")) {
            return static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        // An explicit SOTC_FLUSH_US turns the periodic submit on for any GPU (the launcher's
        // option for AMD and Intel cards, not tested there).
        if (const char* v = std::getenv("SOTC_FLUSH_US"); v && std::strtoull(v, nullptr, 10) != 0) {
            return 256u;
        }
        // SotC: +20% fps in the open world (14-15 -> 18). Other games keep the upstream behavior.
        // Pink-shadPS4 advises turning it off on AMD (black screen); with readbacks ahead on a
        // Radeon RX 6650 XT the image is right and the sanctuary goes from 17.4 to 22.4 fps.
        constexpr u32 VendorNvidia = 0x10DE;
        constexpr u32 VendorAmd = 0x1002;
        return Common::ElfInfo::Instance().IsSotc() &&
                       (instance.GetVendorID() == VendorNvidia ||
                        instance.GetVendorID() == VendorAmd)
                   ? 256u
                   : 0u;
    }();
    // sotc: the command buffer wrote a page the guest reads back: submit it now, the GPU runs
    // it while the guest gets there, and the readback finds it done (or nearly). Within noise
    // when the command processor was slower; on by default since 2026-10-16 (SOTC_RB_EARLY=0
    // off): attract demo 53.6/54.5 -> 55.3/55.7 FPS (open world: 400-490 readbacks/s waited
    // ~10% of the thread), Shrine unchanged (34.2 vs 34.1).
    if (SotcStats::kFpsLog) { // sotc: read only by the SOTC_FPS printer (sotc_stats.h)
        // sotc: written by this thread only: a plain add, no lock prefix (an atomic RMW here
        // waits for the thread's pending stores, ~1.8% of it). The printer takes deltas.
        SotcStats::commands.store(SotcStats::commands.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
    }
    // sotc: SOTC_RB_PREFETCH (off by default): a command boundary outside a render pass, where
    // every write of the commands so far is recorded; the prefetch copies go here, in the tick
    // being recorded, and ride on its submit (the early flush below, or the periodic one).
    if (!scheduler.IsRendering()) {
        buffer_cache.PrefetchIssue();
    }
    static const bool early = [] {
        const char* v = std::getenv("SOTC_RB_EARLY");
        return !v || v[0] != '0';
    }();
    if (early && !scheduler.IsRendering() && buffer_cache.TakeHotWrite()) {
        SotcStats::early_flushes.fetch_add(1, std::memory_order_relaxed);
        scheduler.Flush();
        return;
    }
    if (every == 0) {
        // sotc: without periodic submits, look for cold arena memory every 1024 commands.
        if ((++evict_poll & 1023) == 0 && !scheduler.IsRendering()) {
            buffer_cache.MaybeEvictCold(std::chrono::steady_clock::now());
        }
        return;
    }
    // sotc: submit by recording time instead (SOTC_FLUSH_US, default 1000 where the periodic
    // submit is on and SOTC_FLUSH_EVERY is not set): the GPU gets work at a steady pace whether
    // a scene has few heavy commands or thousands of light ones. Attract demo: +10-15% in most
    // scenes, canyon bridge +2% (count based: 64 helped the first and hurt the second).
    // 1000 instead of 500 (2026-10-15, RTX 3060 laptop, idle shrine ~5800 draws/frame): about +8%
    // in a single unpaired run (250: no gain, 2000: same as 1000): fewer submits, each one
    // cheaper per draw.
    static const u64 flush_us = [] {
        if (const char* v = std::getenv("SOTC_FLUSH_US")) {
            return std::strtoull(v, nullptr, 10);
        }
        return std::getenv("SOTC_FLUSH_EVERY") ? u64{0} : u64{1000};
    }();
    const u64 tick = scheduler.CurrentTick();
    if (tick != commands_tick) {
        commands_tick = tick;
        commands_since_submit = 0;
        commands_since = std::chrono::steady_clock::now();
    }
    ++commands_since_submit;
    if (flush_us != 0) {
        // sotc: the clock is read every 8th command (~40 us at the Shrine against 1000 us),
        // not on every one (~0.5% of the command processor in QueryPerformanceCounter).
        if ((commands_since_submit & 7) != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - commands_since;
        // sotc: SOTC_ADAPTIVE_LOAD: while the governor is relieved, SOTC_ADAPTIVE_FLUSH_US
        // (default 4000) between submits outside a render pass; inside one the longest window
        // stays the larger of that and 4 * SOTC_FLUSH_US. Nothing needs the 1 ms cadence for
        // correctness: readbacks and the hot-write submit above submit on their own, buffers that
        // fill up within a tick wait (submit) as at any tick length, and the GPUs without the
        // periodic submit run whole frames in one tick. Normal: limit and pass_limit as before.
        const auto base = std::chrono::microseconds(flush_us);
        const u64 relieved_us = SotcAdaptive::FlushUs();
        const auto limit = relieved_us > flush_us ? std::chrono::microseconds(relieved_us) : base;
        const auto pass_limit = std::max(limit, base * 4);
        if (elapsed < limit || (scheduler.IsRendering() && elapsed < pass_limit)) {
            return;
        }
        if (SotcStats::kFpsLog && limit != base && !scheduler.IsRendering()) {
            const auto windows = static_cast<u64>(elapsed / base);
            if (windows > 1) {
                SotcStats::adaptive_flushes_saved.fetch_add(windows - 1,
                                                            std::memory_order_relaxed);
            }
        }
        // sotc: cold arena memory goes back at a submit point outside a render pass.
        if (!scheduler.IsRendering() && buffer_cache.MaybeEvictCold(now)) {
            return;
        }
        scheduler.Flush();
        return;
    }
    if (commands_since_submit < every ||
        (scheduler.IsRendering() && commands_since_submit < u64{every} * 4)) {
        return;
    }
    if (!scheduler.IsRendering() &&
        buffer_cache.MaybeEvictCold(std::chrono::steady_clock::now())) {
        return;
    }
    scheduler.Flush();
}

// SOTC_DUMP="hash,hash,..." (hex, default: the SotC volumetric fog passes): after F12 (without
// RenderDoc), the next dispatch of each listed compute shader dumps every image it has bound to
// user/log/dump_N/ (raw mip 0, all layers and slices). The data is what the GPU really computed,
// unlike a RenderDoc replay of a frame that uses the sparse guest-memory buffer.
void Rasterizer::MaybeDumpImages(u64 pgm_hash) {
    // sotc: SOTC_CS_DUMP="hash,hash": after those compute shaders, read the buffer armed by
    // SOTC_CS_WATCH back from the GPU and print its first 24 dwords (first 60 times; slow).
    static const std::vector<u64> dump_after = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("SOTC_CS_DUMP")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 v = std::strtoull(p, &end, 0);
                if (end == p) {
                    break;
                }
                list.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    if (!dump_after.empty() &&
        std::find(dump_after.begin(), dump_after.end(), pgm_hash) != dump_after.end()) {
        static int shown = 0;
        const VAddr watch = SotcStats::watch_addr.load(std::memory_order_relaxed);
        if (watch && shown++ < 60) {
            buffer_cache.ReadMemory(watch, 24 * sizeof(u32));
            u32 v[24]{};
            memory->TryReadBacking(watch, v, sizeof(v));
            std::fprintf(stderr, "SOTC dump after %#llx:", static_cast<unsigned long long>(pgm_hash));
            for (const u32 x : v) {
                std::fprintf(stderr, " %u", x);
            }
            std::fprintf(stderr, "\n");
        }
    }
    static const std::vector<u64> targets = [] {
        std::vector<u64> list;
        const char* env = std::getenv("SOTC_DUMP");
        if (!env) {
            return list;
        }
        std::string spec = env;
        if (spec.empty() || spec == "1") {
            spec = "b33e9db6,91c6c95e,25338711,47c1dc64";
        }
        size_t pos = 0;
        while (pos < spec.size()) {
            const size_t end = std::min(spec.find(',', pos), spec.size());
            list.push_back(std::strtoull(spec.substr(pos, end - pos).c_str(), nullptr, 16));
            pos = end + 1;
        }
        return list;
    }();
    static std::vector<u64> pending;
    static std::filesystem::path dir;
    static u32 session = 0;
    static u32 seq = 0;
    if (targets.empty()) {
        return;
    }
    if (VideoCore::ConsumeImageDumpRequest()) {
        pending = targets;
        dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / fmt::format("dump_{}", ++session);
        std::filesystem::create_directories(dir);
        seq = 0;
    }
    const auto it = std::find(pending.begin(), pending.end(), pgm_hash);
    if (it == pending.end()) {
        return;
    }
    pending.erase(it);
    std::vector<VideoCore::ImageId> seen;
    u32 index = 0;
    for (const auto image_id : bound_images) {
        if (!image_id || std::find(seen.begin(), seen.end(), image_id) != seen.end()) {
            continue;
        }
        seen.push_back(image_id);
        texture_cache.DumpImage(image_id, dir / fmt::format("{:02}_cs_{:08x}_img{}", seq, pgm_hash,
                                                             index++));
    }
    ++seq;
}

// sotc: F5 (SotcStats::tex_capture): every sampled image of the next 30000 draws and dispatches
// is saved once to user/log/texcap_N, named by guest address, size, format, mips and flags, to
// find which texture shows garbage and where it lives.
void Rasterizer::CaptureTextures() {
    static u32 budget = 0, session = 0, count = 0;
    static std::filesystem::path dir;
    static std::set<u64> seen;
    if (SotcStats::tex_capture.exchange(false, std::memory_order_relaxed)) {
        budget = 30000;
        count = 0;
        seen.clear();
        dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
              fmt::format("texcap_{}", ++session);
        std::filesystem::create_directories(dir);
        std::fprintf(stderr, "SOTC TEXCAP %u started\n", session);
    }
    if (!budget) {
        return;
    }
    if (--budget == 0) {
        std::fprintf(stderr, "SOTC TEXCAP %u done: %u textures\n", session, count);
    }
    for (const auto image_id : bound_images) {
        if (!image_id || count >= 600) {
            continue;
        }
        auto& image = texture_cache.GetImage(image_id);
        // At most 64 MiB per image and 1 GiB per capture: each one is downloaded through a
        // host staging buffer, and 600 full chains filled the system heap (out of memory, 26 Oct).
        static u64 captured_bytes = 0;
        if (count == 0) {
            captured_bytes = 0;
        }
        const u64 bytes = image.info.guest_size;
        if (image.info.size.width < 64 || image.info.size.height < 64 || bytes > 64_MB ||
            captured_bytes + bytes > 1_GB) {
            continue;
        }
        captured_bytes += bytes;
        const u64 key = (u64{image.info.guest_address} << 8) ^ u64(image.info.pixel_format);
        if (!seen.insert(key).second) {
            continue;
        }
        ++count;
        texture_cache.DumpImage(image_id,
                                dir / fmt::format("{:03}_{}_m{}f{}_fl{:x}_u{}{}{}", count,
                                                  vk::to_string(image.info.pixel_format),
                                                  image.info.resources.levels,
                                                  image.info.sotc_first_mip, u32(image.flags),
                                                  image.usage.render_target ? "R" : "",
                                                  image.usage.storage ? "S" : "",
                                                  image.usage.depth_target ? "D" : ""));
    }
}

void Rasterizer::ResetBindings(bool is_compute) {
    CaptureTextures();
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    // sotc: SOTC_BARRIER_STATIC. The op's reads and writes become pending for the static rule now, before the mirror
    // copies below (they order against this op's writes). Mode 1 keeps no per-buffer records: the loops after it are
    // the old rule's (mode 0 and the verify mode 2).
    // sotc: SOTC_DRAW_MERGE_STATS=1. A reused draw did not bind: it replays the touch of the bind it reused.
    const bool replay_touch = !is_compute && g_merge_touch;
    g_merge_touch = false;
    const bool touches = op_static_reads || !bound_buffers.empty() || !fixed_function_reads.empty() ||
                         shader_addr_touch || replay_touch;
    BarrierStatic::NoteOp(touches, op_static_writes);
    op_static_writes = false;
    op_static_reads = false;
    if (!BarrierStatic::On()) {
        for (const auto [buffer, offset, size, is_written, bound_addr] : bound_buffers) {
            const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                              : vk::PipelineStageFlagBits2::eAllGraphics;
            const auto write_flag =
                is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
            runtime.AccessBuffer(buffer, offset, size, dst_stage,
                                 vk::AccessFlagBits2::eShaderRead | write_flag);
        }
        for (const auto& read : fixed_function_reads) {
            runtime.AccessBuffer(read.buffer, read.offset, read.size,
                                 vk::PipelineStageFlagBits2::eVertexInput,
                                 vk::AccessFlagBits2::eVertexAttributeRead |
                                     vk::AccessFlagBits2::eIndexRead);
        }
    }
    fixed_function_reads.clear();
    // sotc: SOTC_BARRIER_GUEST. The op's addressed writes join the safety net (its buffer reads and writes joined it in
    // the AccessBuffer loop above, which runs in this mode).
    if (SotcArch::GuestBarrier::Active()) {
        for (const auto& [begin, end] : guest_addr_writes) {
            SotcArch::GuestBarrier::NoteAccess(begin, end - begin, true);
        }
        guest_addr_writes.clear();
    }
    // sotc: SOTC_SHADER_ADDR. Mirror coherence of this op: after it is recorded (the copies follow the writer).
    if (SotcArch::ShaderAddr::On()) {
        SotcMirrorAfterOp();
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
    buffer_barrier_need = false;
}

// sotc: SOTC_SHADER_ADDR, mirror coherence. Two directions, one per kind of write (ARCH-DRAW: the mirror is the GPU's
// view of guest memory, so it must hold every GPU write, and addressed writes must reach the buffers the descriptor
// readers use). Forward: a written buffer binding of this op (bound_buffers, guest address known) is copied into the
// mirror. Reverse: a range the shader wrote through the mirror is copied into its buffer.
void Rasterizer::SotcMirrorAfterOp() {
    bool wrote = false;
    for (const auto& bound : bound_buffers) {
        if (bound.is_written && bound.addr != 0 && bound.size != 0) {
            wrote = true;
            SotcMirrorFromBuffer(*bound.buffer, bound.offset, bound.addr, bound.size);
        }
    }
    for (const auto& write : sotc_addressed_writes) {
        wrote = true;
        SotcMirrorToBuffer(write.addr, write.size);
    }
    sotc_addressed_writes.clear();
    // sotc: A2b (aliases). Between the arena and the window there is no copy to order: a write through one alias is
    // read through the other after a memory barrier, recorded here after the op that wrote.
    if (wrote && SotcArch::Mirror::AliasActive()) {
        GcnEndRendering(scheduler);
        SotcArch::Mirror::AliasBarrier(scheduler.CommandBuffer());
    }
}

void Rasterizer::SotcMirrorFromBuffer(const VideoCore::Buffer& buffer, u64 offset, VAddr addr, u64 size) {
    if (!SotcArch::Mirror::On() || SotcArch::AddrTable::On() || size == 0) { // sotc: not in table mode (addr_table.h)
        return;
    }
    GcnEndRendering(scheduler);
    // The write of the op (or of the copy) is made visible to the transfer read by the runtime's barriers.
    if (runtime.IsBufferAccessed(&buffer, offset, size)) {
        runtime.FlushBarriers();
    }
    SotcArch::Mirror::SyncFromBuffer(scheduler, buffer.Handle(), offset, addr, size,
                                     buffer.mem_type == VideoCore::MemoryType::Sparse);
    runtime.AccessBuffer(&buffer, offset, size, vk::PipelineStageFlagBits2::eCopy,
                         vk::AccessFlagBits2::eTransferRead);
}

void Rasterizer::SotcMirrorFromGuest(VAddr addr, u64 size) {
    if (!SotcArch::Mirror::On() || SotcArch::AddrTable::On() || size == 0) { // sotc: not in table mode (addr_table.h)
        return;
    }
    SotcArch::Mirror::Upload(scheduler, addr, size);
}

void Rasterizer::SotcMirrorToBuffer(VAddr addr, u64 size) {
    if (!SotcArch::Mirror::On() || SotcArch::AddrTable::On() || size == 0) { // sotc: not in table mode (addr_table.h)
        return;
    }
    // Obtained for writing: the range is GPU-modified from here on, so a CPU read downloads it.
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(addr, static_cast<u32>(size), true);
    GcnEndRendering(scheduler);
    if (runtime.IsBufferAccessed(buffer, offset, size, true)) {
        runtime.FlushBarriers();
    }
    SotcArch::Mirror::SyncToBuffer(scheduler, buffer->Handle(), offset, addr, size,
                                   buffer->mem_type == VideoCore::MemoryType::Sparse);
    runtime.AccessBuffer(buffer, offset, size, vk::PipelineStageFlagBits2::eCopy,
                         vk::AccessFlagBits2::eTransferWrite);
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline, VAddr* clear_addr, u64* clear_size) {
    if (!pipeline->IsCompute()) {
        return false;
    }
    // sotc: SOTC_META_CLEAR=0 runs every dispatch the heuristic below would skip (test: a
    // buffer reused from an old image's metadata range makes a real dispatch look like a clear).
    static const bool meta_clear = [] {
        const char* v = std::getenv("SOTC_META_CLEAR");
        return !v || v[0] != '0';
    }();
    if (!meta_clear) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                if (clear_addr && clear_size) { // sotc: SOTC_BIND_CACHE_META, the range the dispatch would have written
                    *clear_addr = address;
                    *clear_size = desc.GetSharp(info).GetSize();
                }
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    // sotc: the destination was found by address, not by a writer lookup: its first GPU write
    // moves the mip-tail epoch (CopyColorAndDepth only passes the flag on).
    VideoCore::MarkGpuModified(dst_image);
    compute_written_image = desc0.is_written ? image0_id : image1_id; // sotc: BindResources
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    compute_written_image = image1_id; // sotc: SOTC_BIND_CACHE_FRAME (BindResources)
    runtime.ClearImage(&image1, range, clear);
    return true;
}

const u32* Rasterizer::FlatForBind(const Shader::Info& stage) {
    // sotc: the live-setting patches of the flat buffer (moved out of BindBuffers so that a hit of
    // the resource identity bind cache, SOTC_BIND_CACHE_FRAME, applies them to the data it
    // re-copies). Every bind-time rewrite of the flat buffer is here: TAA resolve off, the tonemap
    // vignette moved after upscaling, the vegetation distance / height scales and the grass patch
    // LOD scale.
    // sotc: SOTC_PR_RESOLVE: FlatMayBePatched lists the stages these rewrites apply to (their
    // prepared resources are not used); keep the two in sync.
    const u32* flat = stage.FlatBuf().data();
        static std::vector<u32> taa_flat;
        if (stage.pgm_hash == kTaaResolveHash && stage.FlatBuf().size() > kTaaEnableDword &&
            TaaOff()) {
            taa_flat.assign(stage.FlatBuf().begin(), stage.FlatBuf().end());
            taa_flat[kTaaEnableDword] &= ~1u;
            flat = taa_flat.data();
            static bool reported = false;
            if (!std::exchange(reported, true)) {
                std::fprintf(stderr, "SOTC TAA off: resolve without history blend\n");
            }
        }
        static std::vector<u32> tonemap_flat;
        static const bool vignette_after = [] {
            const char* v = std::getenv("SOTC_DLSS_VIGNETTE");
            return !v || v[0] != '0';
        }();
        if (stage.pgm_hash == kTonemapHash && stage.FlatBuf().size() > kVignetteDword + 3 &&
            vignette_after && Dlssg::SuperResolution()) {
            tonemap_flat.assign(stage.FlatBuf().begin(), stage.FlatBuf().end());
            float params[4];
            std::memcpy(params, &tonemap_flat[kVignetteDword], sizeof(params));
            Dlssg::NoteVignette(params);
            tonemap_flat[kVignetteDword + 3] = 0; // strength 0.0f
            flat = tonemap_flat.data();
            static bool reported = false;
            if (!std::exchange(reported, true)) {
                std::fprintf(stderr,
                             "SOTC DLSS: vignette after upscaling (%g %g %g %g)\n",
                             params[0], params[1], params[2], params[3]);
            }
        }
        const auto veg_scales = VegetationScales();
        for (size_t v = 0; v < kVegetation.size(); ++v) {
            const auto& veg = kVegetation[v];
            const float dist = veg_scales[v * 2], height = veg_scales[v * 2 + 1];
            if (stage.pgm_hash != veg.hash || (dist == 1.f && height == 1.f) ||
                stage.FlatBuf().size() < veg.bands_dword + kVegetationBands) {
                continue;
            }
            static std::vector<u32> veg_flat;
            veg_flat.assign(flat, flat + stage.FlatBuf().size());
            const auto scale = [&](u32 dword, float m) {
                veg_flat[dword] = std::bit_cast<u32>(std::bit_cast<float>(veg_flat[dword]) * m);
            };
            scale(veg.height_dword, height);
            for (u32 i = 0; i < kVegetationBands; ++i) {
                scale(veg.bands_dword + i, dist);
            }
            flat = veg_flat.data();
        }
        if (const float lod = veg_scales[4]; stage.pgm_hash == kGrassPatchHash &&
                                                     lod != 1.f &&
                                                     stage.FlatBuf().size() > kGrassPatchLodDword) {
            static std::vector<u32> patch_flat;
            patch_flat.assign(flat, flat + stage.FlatBuf().size());
            patch_flat[kGrassPatchLodDword] =
                std::bit_cast<u32>(std::bit_cast<float>(patch_flat[kGrassPatchLodDword]) * lod);
            flat = patch_flat.data();
        }
    return flat;
}

// sotc: SOTC_BIND_MARK_ONCE=1 (off by default). A written binding's guest write is noted by
// BufferCache::ObtainBuffer for the same range (the same NoteHostWrite call), so BindBuffers skips
// its second call when ObtainBuffer ran for that binding. NoteHostWrite only bumps
// SotcStats::mem_epoch, and every reader of that counter compares it for equality, so one bump
// (made before any reader can run) is as good as two; the stage hash and tag are not used.
static const bool kBindMarkOnce = [] {
    const char* v = std::getenv("SOTC_BIND_MARK_ONCE");
    return v && v[0] == '1';
}();

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32 set, const PreparedResources* pr,
                             const PrStage* pr_stage) {
    const u64 alignment = instance.StorageMinAlignment();
    // sotc: SOTC_BDA: a stage compiled with the block table gets one per draw (vk_bda_table.h).
    const bool bda_stage = stage.HasBdaBlockTable();
    // sotc: SOTC_N5_BDA (vk_bda_table.h): 1 drops the descriptors of the slots the table serves,
    // 2 keeps them and checks each table slot against its descriptor's buffer.
    static const u32 n5_bda = BdaN5Mode();
    // sotc: SOTC_N5_BDA_BUFFERS (vk_bda_table.h): 1 writes no descriptor for the slots the table
    // serves, 2 writes them and counts what 1 would drop. 0 when the device lacks bufferDeviceAddress.
    const u32 n5_buffers = BdaN5BuffersActive(instance);
    // sotc: SOTC_BDA_DIRECT (vk_bda_table.h). A block-table slot whose range lies in an imported ring-8 run is
    // bound by arithmetic: the run's device address plus the offset, with the run's buffer (the one
    // BufferCache::PcHostServe returns for it, kept from its first ObtainBuffer answer), and ObtainBuffer is
    // skipped. Needs SOTC_N5_BDA_BUFFERS (the table is what reads the address), SOTC_PC_RING=8 and the draw
    // thread. Written slots too: ObtainBuffer's served path returns before its written-bind work.
    const u32 bda_direct = n5_buffers != 0 ? BdaDirectMode() : 0;
    const bool direct_ring = bda_direct != 0 && SotcStats::t_draw_thread && SotcPcRing::Mode() == 8;
    // sotc: SOTC_BDA_DIRECT: the switches whose work ObtainBuffer does before the ring path, and PcHostBind's
    // Ring8Verify record. With one of them on, every slot takes the ObtainBuffer path (counted as gate).
    const bool direct_gate = direct_ring &&
                             (SotcN1Verify::On() || SotcMemClasses::On() || SotcPcPlaceGlue::On() ||
                              SotcLoadWarm::Enabled() || SotcLoadWarm::StreamEnabled() ||
                              SotcPcRing::Ring8Verify());
    // The ring maintenance (PcHostBind -> HostMaintain) runs at the first ObtainBuffer of a submit epoch and
    // block-set version. A direct bind is taken only when the last such call saw the current epoch and version
    // (direct_state), so no bind that could depend on the maintenance skips it.
    struct DirectState {
        bool maintained{};
        u32 epoch{};
        u64 version{};
    };
    static thread_local DirectState direct_state{};
    // The buffer ObtainBuffer returned for each run (by its guest base), with the run's buffer handle.
    static thread_local std::unordered_map<VAddr, std::pair<vk::Buffer, const VideoCore::Buffer*>>
        direct_buffers;
    // The arithmetic answer for a block-table slot [vbase, vbase + size) of a ring-8 run: true, with the run's
    // view and buffer, when every condition holds; false otherwise, the reason counted.
    const auto direct_answer = [&](VAddr vbase, u64 size, SotcGuestMemory::GuestView& view,
                                   const VideoCore::Buffer*& run_buffer) -> bool {
        const int relation = SotcPcRing::ArenaRelation(vbase, size);
        if (relation != 1) {
            BdaNoteDirect(relation == 0 ? BdaDirect::FbArena : BdaDirect::FbStraddle);
            return false;
        }
        if (direct_gate) {
            BdaNoteDirect(BdaDirect::FbGate);
            return false;
        }
        if (!direct_state.maintained || direct_state.epoch != SotcPcRing::SubmitEpoch() ||
            direct_state.version != SotcPcRing::BlockVersion()) {
            BdaNoteDirect(BdaDirect::FbMaintain);
            return false;
        }
        if (!SotcGuestMemory::LookupGuestView(vbase, size, &view)) {
            BdaNoteDirect(BdaDirect::FbNotServed);
            return false;
        }
        if (view.device_address == 0) {
            BdaNoteDirect(BdaDirect::FbNoAddress);
            return false;
        }
        const auto known = direct_buffers.find(view.cpu_base);
        if (known == direct_buffers.end() || known->second.first != view.buffer) {
            BdaNoteDirect(BdaDirect::FbNoBuffer);
            return false;
        }
        run_buffer = known->second.second;
        return true;
    };
    // The buffer ObtainBuffer answered for a served range is the run's buffer: remembered for direct binds.
    const auto direct_learn = [&](VAddr vbase, u64 size,
                                  const std::pair<const VideoCore::Buffer*, u64>& got) {
        SotcGuestMemory::GuestView view{};
        if (!got.first || !SotcGuestMemory::LookupGuestView(vbase, size, &view) ||
            view.device_address == 0 || got.second != view.offset ||
            got.first->Handle() != view.buffer || !got.first->HasBufferDeviceAddress() ||
            got.first->BufferDeviceAddress() != view.device_address) {
            return;
        }
        direct_buffers[view.cpu_base] = {view.buffer, got.first};
    };
    // sotc: W1 diagnostic. The BDA table build is logged on the first frame only (flip 0), at most
    // kBdaLogLines lines per process.
    const bool bda_log = bda_stage && SotcStats::flip_count.load(std::memory_order_relaxed) == 0;
    static std::atomic<u32> bda_logged{0};
    constexpr u32 kBdaLogLines = 256;
    const auto bda_log_line = [&] {
        return bda_log && bda_logged.fetch_add(1, std::memory_order_relaxed) < kBdaLogLines;
    };
    if (bda_stage) {
        bda_table.Begin(static_cast<u32>(stage.buffers.size()),
                        buffer_cache.GetStreamBuffer().BufferDeviceAddress());
        bind_cache_uncacheable = true; // the table is per draw
        if (bda_log_line()) {
            LOG_INFO(Render_Vulkan,
                     "SOTC BDA first frame: stage {:#x} table of {} slots, null address {:#x}, "
                     "{} buffer infos before it (static capacity {}), SOTC_N5_BDA {} SOTC_N5_BDA_BUFFERS {}",
                     stage.pgm_hash, stage.buffers.size(),
                     buffer_cache.GetStreamBuffer().BufferDeviceAddress(), buffer_infos.size(),
                     buffer_infos.capacity(), n5_bda, n5_buffers);
        }
    }
    // sotc: SOTC_SHADER_ADDR (A3, sotc_arch/shader_addr.cpp). The predicate is the shader's: Shader::ShaderAddrBuffer
    // on the pipeline's profile (SetBindProfile, BindResources) and this stage's Info, the same call the emitter makes
    // (UsesShaderAddr). It takes no run state: a block-table stage is no exception (the emitter addresses its guest
    // buffers too), and a missing mirror window is not a reason to bind a descriptor the shader never reads (the
    // touch below counts that case). A buffer it addresses from its V# words takes no ObtainBuffer and no descriptor
    // here. Mode 2 runs the old path and verifies each such bind after ObtainBuffer.
    const Shader::Profile* shader_addr_profile = SotcArch::ShaderAddr::BindProfile();
    if (shader_addr_profile && shader_addr_profile->shader_addr == 0) {
        shader_addr_profile = nullptr;
    }
    const auto shader_addressed = [&](const Shader::BufferResource& desc) {
        const u32 index = static_cast<u32>(&desc - stage.buffers.data());
        // sotc: SOTC_PR_PRED=1: the bit the prep thread computed for this buffer (PrepareResources, same
        // ShaderAddrBufferAt on the pipeline's profile). Only for a prepared stage; the rest computes it here.
        if (pr && pr_stage && SotcPrPredOn()) {
            return pr->buffers[pr_stage->buf_first + index].shader_addressed;
        }
        return shader_addr_profile &&
               Shader::ShaderAddrBufferAt(*shader_addr_profile, stage, index);
    };
    for (const auto& desc : stage.buffers) {
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                buffer_barrier_need |= Passes::BufferNeed( // sotc: SOTC_PASSES (see BindVertexBuffers)
                    BarrierStatic::BufferChecks() &&
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written));
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatBuf().size() * sizeof(u32);
                const u32* flat = FlatForBind(stage);
                // sotc: SOTC_BIND_CACHE: flat buffers changed by live settings are not cached.
                if (bind_cache_recording &&
                    (flat != stage.FlatBuf().data() || stage.pgm_hash == kTaaResolveHash ||
                     stage.pgm_hash == kTonemapHash)) {
                    bind_cache_uncacheable = true;
                }
                const u64 offset = vk_buffer.Copy(flat, ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                // sotc: SOTC_PACKET: the flat buffer's device address for the packet (flat buffers are per stage:
                // the fragment one has its own word, the vertex and compute one shares flatbuf_address).
                if (stage.sw_stage == Shader::SwStage::Fragment) {
                    draw_packet.flatbuf_fragment_address = vk_buffer.BufferDeviceAddress() + offset;
                } else if (stage.sw_stage == Shader::SwStage::Vertex ||
                           stage.sw_stage == Shader::SwStage::Compute) {
                    draw_packet.flatbuf_address = vk_buffer.BufferDeviceAddress() + offset;
                }
                bind_cache_stream_rec = true; // sotc: SOTC_BIND_CACHE_FRAME: a copy in the ring
                if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                    bind_cache_flat_rec.push_back({cur_bind_stage,
                                                   static_cast<u32>(buffer_infos.size() - 1),
                                                   ubo_size, XXH3_64bits(flat, ubo_size), offset,
                                                   buffer_cache.StreamGeneration(),
                                                   scheduler.CurrentTick()});
                }
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->DrawRegs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->DrawRegs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::LodFeedback) {
                buffer_infos.emplace_back(sotc_lod_feedback->Handle(), 0,
                                          SotcLodFeedbackBytes + sizeof(u32));
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = liverpool->DrawCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else if (desc.buffer_type == Shader::BufferType::BdaBlockTable) {
                // sotc: SOTC_BDA: the stage's block table, last of its buffers, after every guest slot.
                // Dirty check: when the same words were uploaded earlier in this stream-buffer
                // generation, that copy is still in place and is used again (BdaTable::Lookup).
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 table_bytes = static_cast<u32>(bda_table.Bytes());
                u64 offset{};
                const bool table_reused =
                    bda_table.Lookup(stage.pgm_hash, buffer_cache.StreamGeneration(), &offset);
                if (!table_reused) {
                    offset = vk_buffer.Copy(bda_table.Words(), table_bytes, alignment);
                    // The generation after the copy: the copy may have wrapped the ring.
                    bda_table.Store(stage.pgm_hash, buffer_cache.StreamGeneration(), offset);
                }
                if (bda_log_line()) {
                    LOG_INFO(Render_Vulkan,
                             "SOTC BDA first frame: stage {:#x} table {} B at stream offset {:#x} "
                             "(reused {}), buffer infos {} of {}",
                             stage.pgm_hash, table_bytes, offset, table_reused, buffer_infos.size() + 1,
                             buffer_infos.capacity());
                }
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, table_bytes);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            // sotc: SOTC_OP_PHASES: a binding is clean, dirty (its upload bytes moved) or written.
            const u64 ph_t0 = SotcOpPhases::NowBinding(), ph_upload0 = SotcOpPhases::UploadBytes();
            // sotc: SOTC_PR_RESOLVE: the values the prep thread computed for this descriptor;
            // otherwise the V# is read here.
            const u32 buf_index = static_cast<u32>(&desc - stage.buffers.data());
            // sotc: SOTC_BDA: a slot the table serves is a guest buffer that is neither texel nor
            // coherent (DefineBuffers emits no descriptor for it, so the shader never reads one).
            // sotc: SOTC_SHADER_ADDR: a buffer the shader addresses from its V# words reads no table slot (the emitter's
            // bda flag, ShaderAddrBuffer), so it is no table slot here either.
            const bool bda_slot = bda_stage && !desc.is_formatted && desc.GetSharp(stage).mtype != 3 &&
                                  !shader_addressed(desc);
            // sotc: SOTC_N5_BDA_BUFFERS. DefineBuffers declares no variable for a table slot, so the
            // shader never reads its descriptor: mode 1 writes none (drop_desc: no buffer info, no
            // set write, also for a size-0 bind), mode 2 writes it and counts the drop.
            const bool drop_desc = n5_buffers == 1 && bda_slot;
            if (n5_buffers != 0 && bda_slot) {
                BdaNoteDescriptorSkip(drop_desc);
            }
            const PrBuffer* pb =
                pr && pr_stage && pr->buffers[pr_stage->buf_first + buf_index].computed
                    ? &pr->buffers[pr_stage->buf_first + buf_index]
                    : nullptr;
            AmdGpu::Buffer vsharp{};
            if (!pb) {
                vsharp = desc.GetSharp(stage);
            }
            const VAddr vbase = pb ? pb->base : vsharp.base_address;
            const u32 vsize = pb ? static_cast<u32>(pb->vsize) : vsharp.GetSize();
            // Unmapped addresses show up in descriptors of bindings the shader does not use
            // (garbage left in the guest's descriptor memory); bind nothing instead of aborting.
            // Large ranges get clamped to the mapped memory at their start (0 if unmapped);
            // written bindings are checked as well, so garbage cannot become GPU-modified memory.
            // sotc: the clamp alone is 0 exactly when the base is unmapped or reserved (what the
            // two is-mapped calls checked), so one runtime call per binding instead of three.
            // SOTC_BIND_CLAMP1=0 restores the three calls.
            static const bool clamp1 = [] {
                const char* v = std::getenv("SOTC_BIND_CLAMP1");
                return !v || v[0] != '0';
            }();
            u64 size = 0;
            if (pb) {
                size = pb->size; // sotc: SOTC_PR_RESOLVE: clamped and limited by the prep thread
            } else if (clamp1) {
                if (vbase != 0 && vsize != 0) {
                    size = memory->ClampRangeSize(vbase, vsize);
                }
            } else if (vbase != 0 && vsize != 0 && memory->IsValidMapping(vbase) &&
                       (!desc.is_written || memory->IsMappedAddress(vbase))) {
                size = memory->ClampRangeSize(vbase, vsize);
            }
            // sotc: SOTC_BPE_FIX: in-place written bindings are checked against the BPE heap
            // metadata (=2 counts, =1 clamps the binding before the first metadata byte).
            if (desc.is_written && size != 0) {
                size = VideoCore::BpeNative::Check(vbase, size, VideoCore::BpeNative::Kind::Bind,
                                                   stage.pgm_hash, SotcPcRing::HostServed(vbase, size),
                                                   true);
            }
            // sotc: SOTC_SHADER_ADDR (A3). The shader addresses this buffer itself: no ObtainBuffer, no
            // descriptor, no buffer record for the barriers. What does not need the buffer object stays: the
            // invalidation of images and the write mark of a written range. Mode 2 takes the old path below.
            // sotc: SOTC_SHADER_ADDR. The shader reads a shader-addressed range through the mirror in both modes: =1
            // skips its bind below, =2 runs the old bind beside it. So the range is made resident here in both modes.
            // The arena window is empty (see BindResources), so every such range is in the mirror. A refused Touch
            // means those reads return zero: a coverage bug, counted and logged.
            if (shader_addressed(desc)) {
                const bool in_arena = draw_packet.arena_size != 0 &&
                                      vbase >= draw_packet.arena_guest_base &&
                                      vbase + size <= draw_packet.arena_guest_base + draw_packet.arena_size;
                // sotc: SOTC_ADDR_TABLE: the range is made resident through the buffer cache (ObtainBuffer) and its
                // page entries are written; no mirror (addr_table.h).
                const bool touch_ok =
                    size == 0 || in_arena ||
                    (SotcArch::AddrTable::On()
                         ? GcnTouch(buffer_cache, scheduler, vbase, size, desc.is_written, desc.is_formatted)
                         : SotcArch::Mirror::Touch(scheduler, vbase, size));
                if (!touch_ok) {
                    static std::atomic<u64> touch_refused{};
                    if (touch_refused.fetch_add(1, std::memory_order_relaxed) < 8) {
                        LOG_ERROR(Render_Vulkan, "SOTC SHADER_ADDR: mirror cannot hold {:#x}+{:#x} (pgm {:#x})",
                                  vbase, size, stage.pgm_hash);
                    }
                }
                shader_addr_touch = true; // Passes::Decide: this op touches memory with no bound_buffers entry
                if (size != 0) { // sotc: ordering against the buffer writes before this op (GuestWriteOverlap)
                    sotc_addr_hazard |= GuestWriteOverlap(vbase, size);
                    if (desc.is_written) {
                        sotc_op_writes.emplace_back(vbase, vbase + size);
                    }
                }
                // sotc: SOTC_PACKET. A replayed bind (bind cache, fast bind) touches the same range again (PacketAddrReplay).
                // Only a shader-addressed buffer of a packet op comes here (its profile is the packet's), so this is
                // always a packet op.
                packet_addr_rec.push_back({vbase, size, desc.is_written, desc.is_formatted});
                if (bind_cache_recording && size != 0) {
                    bind_cache_ranges_rec.emplace_back(vbase, size); // what a GPU write to the range invalidates
                }
                if (bind_cache_recording && SotcStats::kBindCacheXtick) {
                    // The mapping under the range is checked at each hit (mapping_changed), as for a descriptor.
                    bind_cache_repairs_rec.push_back({vbase, vsize, size, nullptr, 0, false});
                }
                // sotc: SOTC_BIND_CACHE_PACKET. Where this PacketAddr's range and repair are, so a hit of an entry
                // made with the base out of its key rewrites them (PacketAddrRefresh). A written one is never cached.
                if (bind_cache_recording && SotcStats::kBindCachePacket != 0 && !desc.is_written) {
                    packet_addr_src_rec.push_back(
                        {cur_bind_stage, buf_index, static_cast<u32>(packet_addr_rec.size() - 1),
                         size != 0 ? static_cast<u32>(bind_cache_ranges_rec.size() - 1) : kNoIndex,
                         SotcStats::kBindCacheXtick ? static_cast<u32>(bind_cache_repairs_rec.size() - 1)
                                                    : kNoIndex});
                }
                if (desc.is_written) {
                    Passes::NoteWrite(); // sotc: SOTC_PASSES: a write the pass rule must see
                    // sotc: SOTC_SHADER_ADDR, mirror coherence: the shader's writes went to the mirror; they are copied
                    // back into the buffer after the op (SotcMirrorAfterOp). A range in the arena window is already there.
                    // Mode 2 writes through the descriptor (the shader stores do not use the address): nothing to copy back.
                    // sotc: SOTC_ADDR_TABLE writes the arena itself (the entries point there): nothing to copy back.
                    if (size != 0 && !in_arena && !SotcArch::ShaderAddr::Verify() && !SotcArch::AddrTable::On()) {
                        sotc_addressed_writes.push_back({vbase, size});
                    }
                }
            }
            if (shader_addressed(desc) && !SotcArch::ShaderAddr::Verify()) {
                SotcArch::ShaderAddr::NoteSkip(size, desc.is_written);
                if (desc.is_written && size != 0) {
                    // sotc: InvalidateMemoryFromGPU does nothing without an image over the range, and takes the texture
                    // cache mutex to find that out. The lock-free page check skips it for the usual written range.
                    if (texture_cache.HasImagesExact(vbase, size)) {
                        texture_cache.InvalidateMemoryFromGPU(vbase, size);
                    }
                    Core::MemoryManager::NoteHostWrite(vbase, size, 7, stage.pgm_hash); // sotc: also the mem_epoch bump of a fast written Touch
                    if (SotcStats::kBindCacheRanges) {
                        BindCacheInvalidate(vbase, size);
                    }
                    // sotc: the write has no buffer record (no AccessBuffer): the runtime's pending barrier must
                    // carry it, so the next op that reads or writes memory orders after it (see BindResources).
                    runtime.NoteAddressedWrite();
                }
                // sotc: SOTC_PACKET. A read-only addressed range is replayed by a hit (packet_addr_rec); a written one
                // is not eligible (BindCacheEligible), so this flag only guards that case.
                if (desc.is_written) {
                    bind_cache_uncacheable = true;
                }
                ++binding.unified;
                ++binding.buffer;
                continue;
            }
            // sotc: SOTC_BIND_CACHE_XTICK (BindCacheRepair)
            const u64 mapped_size = pb ? pb->mapped : size;
            if (size == 0) {
                if (!drop_desc) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                }
                if (bind_cache_recording && SotcStats::kBindCacheXtick &&
                    vbase != 0 && vsize != 0) {
                    bind_cache_map_dep_rec = true; // sotc: SOTC_BIND_CACHE_FRAME: a map can fill it
                    // sotc: SOTC_BIND_CACHE_XTICK: a null binding stays null only while the
                    // guest memory under it stays unmapped (checked once per tick).
                    bind_cache_repairs_rec.push_back({vbase, vsize, 0,
                                                      nullptr, 0, false});
                }
            } else {
                // Guest code commonly marks a buffer as unbounded with num_records near 4GB and
                // only touches a small part of it, and descriptors of unused bindings can hold
                // garbage sizes of several GB. Backing such ranges makes the buffer cache keep
                // them resident until video memory runs out, so bindings above a sane size are
                // limited to a window from their base address.
                static const u64 unbounded_window = [] {
                    const char* v = std::getenv("SHADPS4_UNBOUNDED_BUFFER_MB");
                    return (v ? std::strtoull(v, nullptr, 10) : 64ULL) << 20;
                }();
                constexpr u64 MaxSaneBindingSize = 256ULL << 20;
                // Written bindings keep their full range: a shader writing past a shortened
                // binding would hit sparse pages that were never made resident.
                // sotc: SOTC_PR_RESOLVE: a prepared size is limited already (its warning is inline only).
                if (!pb && !desc.is_written &&
                    (vsize >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
                    unbounded_window != 0 && size > unbounded_window) {
                    static std::atomic<u32> reported{0};
                    if (vsize < 0xFFFF0000ULL && reported++ < 16) {
                        LOG_WARNING(Render,
                                    "Limiting {} MB binding at {:#x} of shader {:#x} to {} MB",
                                    size >> 20, vbase, stage.pgm_hash,
                                    unbounded_window >> 20);
                    }
                    size = unbounded_window;
                }
                // sotc: SOTC_READ_EXTENT. A listed read-only binding is bound over the window its rule proves.
                if (SotcReadExtent::kEnabled && !pb && !desc.is_written) {
                    size = SotcReadExtent::Limit(stage.pgm_hash, buf_index, size);
                }
                // sotc: SOTC_SHADER_EXTENT. An unbounded read-only V# built by the shader is bound over the extent
                // its loads were proven to stay in at translation (sotc_shader_extent.h).
                if (SotcShaderExtent::kEnabled && !pb) {
                    size = SotcShaderExtent::Limit(desc.is_written, desc.sotc_read_extent, vsize, size);
                }
                // sotc: SOTC_BLOCK_EXTENT. An unbounded read-only V# stops at the end of the game's allocator block
                // its base is in (a game object never crosses its block; sotc/probes.h BlockRemaining).
                if (!pb && !desc.is_written && vsize >= 0xFFFF0000ULL && SotcProbes::BlockExtentOn()) {
                    if (u64 remaining = 0; SotcProbes::BlockRemaining(vbase, &remaining) && remaining < size) {
                        size = remaining;
                    }
                }
                if (size != vsize) {
                    LOG_DEBUG(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsize, size, stage.pgm_hash);
                }
                // sotc: SOTC_PC_RING=8. A limited read-only bind that starts in an imported arena run
                // ends at the run end and is served from host memory (BufferCache::PcHostClipLimited).
                if (!desc.is_written && size < vsize && SotcPcRing::Mode() == 8) {
                    size = buffer_cache.PcHostClipLimited(vbase, size);
                }
                // sotc: SOTC_WRITE_EXTENT. A written binding of a direct dispatch with a rule in
                // user/write_extent.txt is marked only over the window the rule proves (the
                // binding itself, its barriers and the aliased image / bind cache invalidations
                // below keep the full range). Texel buffers and everything else: full range.
                u32 mark_offset = 0;
                u32 mark_size = ~0u;
                if (SotcWriteExtent::kEnabled && desc.is_written && !desc.is_formatted &&
                    size <= 0xFFFFFFF0ULL && t_direct_dispatch.groups != 0 &&
                    t_direct_dispatch.hash == stage.pgm_hash) {
                    u64 window_off = 0, window_len = 0;
                    if (SotcWriteExtent::Narrow(stage.pgm_hash, buf_index, t_direct_dispatch.groups,
                                                t_direct_dispatch.threads, size, window_off,
                                                window_len)) {
                        mark_offset = static_cast<u32>(window_off);
                        mark_size = static_cast<u32>(window_len);
                    }
                }
                // sotc: SOTC_BIND_CACHE_FRAME. A read-only V# of a read-only graphics bind: the
                // arena and offset ObtainBuffer returned last time are still the answer while
                // BufferCache::BindVersion and the mapping under it are as they were (the checks
                // of a cached bind's hit, see TryBindCacheFrame); a CPU-modified range is
                // synchronized as there, and the block stamps are made once per use epoch.
                // Texel buffers (re-tiled from an aliasing image by every bind) and the stream
                // ring are not memoized.
                // sotc: SOTC_PC_RING=8. A bind across an arena block edge is never memoized: a hit skips
                // PcHostMarkCpu, and the ring does not fault the CPU writes of its arena part.
                const bool memo_on = bind_memo_ok && !desc.is_written && !desc.is_formatted &&
                                     SotcPcRing::ArenaRelation(vbase, size) != 2;
                BufferMemo* memo = nullptr;
                std::pair<const VideoCore::Buffer*, u64> obtained{};
                bool obtain_called = false; // sotc: SOTC_BIND_MARK_ONCE: ObtainBuffer ran here
                // sotc: SOTC_BDA_DIRECT. A slot of a ring-8 run: mode 1 takes the arithmetic answer (no memo, no
                // ObtainBuffer); mode 2 keeps the old path and compares its answer with this one after it.
                // direct_arena and maint_*: set when ObtainBuffer runs for a ring-8 slot (the maintenance state it sees).
                bool direct_arena = false;
                u32 maint_epoch = 0;
                u64 maint_version = 0;
                SotcGuestMemory::GuestView direct_view{};
                const VideoCore::Buffer* direct_run = nullptr;
                const bool direct_answered =
                    direct_ring && bda_slot && direct_answer(vbase, size, direct_view, direct_run);
                if (direct_answered && bda_direct == 1) {
                    obtained = {direct_run, direct_view.offset};
                    BdaNoteDirect(BdaDirect::Bind);
                    SotcPcRing::NoteHostBind(size, desc.is_written); // the counts PcHostServe makes
                    SotcLabels::NoteSharedTouch();                   // and the note it makes
                }
                if (memo_on && !obtained.first) {
                    if (buffer_memo.empty()) {
                        buffer_memo.resize(BufferMemoEntries);
                    }
                    const u64 h = (vbase * 0x9E3779B97F4A7C15ULL) ^
                                  (u64{vsize} * 0xC2B2AE3D27D4EB4FULL) ^ size;
                    memo = &buffer_memo[(h ^ (h >> 29)) % BufferMemoEntries];
                    if (memo->valid && memo->addr == vbase &&
                        memo->vsize == vsize && memo->mapped == mapped_size &&
                        memo->size == size && memo->version == buffer_cache.BindVersion()) {
                        bool usable = true;
                        if (buffer_cache.IsRegionCpuModified(memo->addr, memo->size)) {
                            buffer_cache.SynchronizeBinding(memo->arena, memo->addr,
                                                            static_cast<u32>(memo->size), false);
                            // The upload may have unbound or replaced what the memo names.
                            usable = memo->version == buffer_cache.BindVersion();
                            if (SotcStats::kFpsLog) {
                                SotcStats::bf_memo_repaired.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        if (usable) {
                            if (const u16 epoch = buffer_cache.UseEpoch();
                                memo->stamp_epoch != epoch) {
                                buffer_cache.NoteBindUse(memo->addr, memo->size);
                                memo->stamp_epoch = epoch;
                            }
                            obtained = {memo->arena, memo->offset};
                            if (SotcN1Verify::On() && SotcStats::t_draw_thread) { // sotc: SOTC_VERIFY=n1: a memo hit reads the range
                                SotcN1Verify::NoteBind(memo->addr, memo->size, false, true);
                            }
                            if (SotcStats::kFpsLog) {
                                SotcStats::bf_memo_hits.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                if (!obtained.first) {
                    obtain_called = true;
                    // sotc: SOTC VERIFY BYTES: the bind kind of this shader buffer (texel, storage or constant)
                    SotcVerify::BindKindScope verify_kind{
                        desc.is_formatted ? SotcVerify::BindKind::Texel
                        : desc.is_written ? SotcVerify::BindKind::Storage
                                          : SotcVerify::BindKind::Constant};
                    // sotc: SOTC_TRACE (a = bytes uploaded, b = protect calls)
                    const SotcTrace::BindScope trace_obtain{"obtain buffer"};
                    // sotc: SOTC_BDA_DIRECT: the maintenance state read just before this call (direct_state).
                    direct_arena = direct_ring && bda_slot && SotcPcRing::ArenaRelation(vbase, size) == 1;
                    if (direct_arena) {
                        maint_epoch = SotcPcRing::SubmitEpoch();
                        maint_version = SotcPcRing::BlockVersion();
                    }
                    // sotc: SOTC_BIG_BIND_LOG: the shader and descriptor the bind's log line names (no behaviour change).
                    const bool big_bind_log = SotcRegionPredrain::BigBindLogEnabled();
                    if (big_bind_log) {
                        const auto dsharp = desc.GetSharp(stage);
                        SotcRegionPredrain::SetBindContext(
                            stage.pgm_hash, static_cast<u32>(stage.hw_stage),
                            static_cast<u32>(stage.sw_stage), buf_index, vbase, vsize,
                            dsharp.num_records, dsharp.stride);
                    }
                    if (SotcVsizeProbe::On()) { // sotc: SOTC_VSIZE_PROBE: the dirty bytes of this bind's sync
                        SotcVsizeProbe::ResetDirty();
                    }
                    obtained = buffer_cache.ObtainBuffer(vbase, size, desc.is_written,
                                                         desc.is_formatted, mark_offset, mark_size);
                    if (big_bind_log) {
                        SotcRegionPredrain::ClearBindContext();
                    }
                    if (memo) {
                        // The version after the call: it may have unbound or evicted other ranges.
                        const bool stream =
                            static_cast<const void*>(obtained.first) ==
                            static_cast<const void*>(&buffer_cache.GetStreamBuffer());
                        *memo = BufferMemo{vbase,
                                           vsize,
                                           mapped_size,
                                           size,
                                           obtained.second,
                                           buffer_cache.BindVersion(),
                                           obtained.first,
                                           buffer_cache.UseEpoch(),
                                           !stream};
                        if (SotcStats::kFpsLog) {
                            SotcStats::bf_memo_misses.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                // sotc: SOTC_VSIZE_PROBE (measurement only): an unbounded read-only bind, with the dirty bytes of its
                // own sync (zero when the memo answered).
                if (SotcVsizeProbe::On() && !desc.is_written && vsize >= 0xFFFF0000ULL) {
                    SotcVsizeProbe::NoteBind(stage, desc, buf_index, vbase, vsize, size,
                                             obtain_called ? SotcVsizeProbe::Dirty() : 0);
                }
                // sotc: SOTC_BDA_DIRECT. ObtainBuffer ran for a ring-8 slot: its maintenance state is the one this
                // call saw (direct_state), and the run's buffer is remembered for the next direct binds.
                if (direct_arena) {
                    direct_state = {true, maint_epoch, maint_version};
                    direct_learn(vbase, size, obtained);
                }
                // sotc: SOTC_BDA_DIRECT=2: the arithmetic answer must name the buffer and the address the
                // ObtainBuffer answer (used above) gave, with the block inside the buffer.
                if (direct_answered && bda_direct == 2) {
                    const bool same =
                        obtained.first == direct_run && obtained.first->HasBufferDeviceAddress() &&
                        obtained.first->BufferDeviceAddress() + obtained.second ==
                            direct_view.device_address + direct_view.offset &&
                        obtained.second + size <= obtained.first->SizeBytes();
                    BdaNoteDirect(same ? BdaDirect::Verified : BdaDirect::Mismatch);
                }
                const auto [buffer, offset] = obtained;
                // sotc: ARCH-GCN-NATIVE S1 (SOTC_GCN_MEM=2): the guest page table's answer for this range against the
                // buffer ObtainBuffer just gave (Mem::VerifyBinding; mode 1 does nothing).
                if (SotcGcn::Mem::Verify()) {
                    SotcGcn::Mem::VerifyBinding(vbase, size, buffer, offset, desc.is_written);
                }
                // sotc: SOTC_SHADER_ADDR=2: the address the shader computes for this bind, against the one
                // ObtainBuffer just gave. This run is the old path.
                if (SotcArch::ShaderAddr::Verify() && shader_addressed(desc)) {
                    SotcArch::ShaderAddr::VerifyBind(vbase, vsize, buffer, offset);
                }
                if (SotcStats::kWriteMarks && desc.is_written) { // sotc: kWriteMarks (SOTC_FPS_LEAN)
                    SotcWriteExtent::NoteMark(
                        stage.pgm_hash, buf_index, vbase, size,
                        vbase + (mark_size != ~0u ? mark_offset : 0),
                        mark_size != ~0u ? mark_size : size,
                        t_direct_dispatch.hash == stage.pgm_hash ? t_direct_dispatch.threads : 0);
                }
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                if (bda_stage) { // sotc: SOTC_BDA: exact offset and size, not the aligned ones
                    const bool has_address = buffer->HasBufferDeviceAddress();
                    bda_table.SetBlock(buf_index, has_address ? buffer->BufferDeviceAddress() : 0,
                                       buffer->SizeBytes(), offset, size);
                    if (bda_log_line()) {
                        const bool is_stream = static_cast<const void*>(buffer) ==
                                               static_cast<const void*>(&buffer_cache.GetStreamBuffer());
                        LOG_INFO(Render_Vulkan,
                                 "SOTC BDA first frame: stage {:#x} slot {} vbase {:#x} size {:#x} "
                                 "buffer {} address {:#x} buffer size {:#x} offset {:#x} "
                                 "bda_slot {} drop_desc {}",
                                 stage.pgm_hash, buf_index, vbase, size,
                                 is_stream ? "stream" : "other",
                                 has_address ? buffer->BufferDeviceAddress() : 0ULL,
                                 buffer->SizeBytes(), offset, bda_slot, drop_desc);
                    }
                    if ((n5_bda == 2 || n5_buffers == 2) && bda_slot && has_address) {
                        // Verify: the address the table gives must be the descriptor buffer's device
                        // address plus the offset the descriptor would have started at.
                        const vk::DeviceAddress descriptor_base = instance.GetDevice().getBufferAddress(
                            vk::BufferDeviceAddressInfo{.buffer = buffer->Handle()});
                        bda_table.CheckSlot(buf_index, descriptor_base + offset);
                    }
                }
                if (!drop_desc && n5_bda == 1 && bda_slot) {
                    // sotc: SOTC_N5_BDA=1: the block table serves this slot, so its descriptor is a
                    // null placeholder. The write stays (the layout is unchanged) and the buffer
                    // stays in bound_buffers, so barriers and write marks still cover it.
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else if (!drop_desc) {
                    buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                }
                bound_buffers.emplace_back(buffer, offset, size, desc.is_written);
                if (SotcArch::ShaderAddr::On() && size != 0) {
                    // sotc: a descriptor access after an addressed write of the same bytes: the runtime's per-buffer
                    // check does not see that write, so the guest ranges order it.
                    if (GuestWriteOverlap(vbase, size) && BarrierStatic::BufferChecks()) {
                        buffer_barrier_need = true;
                    }
                    if (desc.is_written) {
                        sotc_op_writes.emplace_back(vbase, vbase + size);
                    }
                }
                // sotc: SOTC_SHADER_ADDR, SotcMirrorAfterOp: a range written through this descriptor is copied into the
                // mirror after the op. In mode 2 the shader-addressed ranges are written through their descriptor too.
                if (!shader_addressed(desc) || SotcArch::ShaderAddr::Verify()) {
                    bound_buffers.back().addr = vbase;
                }
                if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE checks it is still clean
                    bind_cache_ranges_rec.emplace_back(vbase, size);
                    if (SotcStats::kBindCacheFrame && desc.is_formatted) {
                        // sotc: an image registered over a texel buffer changes its binding
                        bind_cache_texel_rec.emplace_back(vbase,
                                                          vbase + size);
                    }
                    if (SotcStats::kBindCacheXtick) {
                        bind_cache_repairs_rec.push_back({vbase, vsize,
                                                          mapped_size, buffer,
                                                          static_cast<u32>(size),
                                                          desc.is_formatted});
                    }
                    // sotc: SOTC_BIND_CACHE_RANGES: what a hit cannot check. A stream-buffer copy
                    // is a snapshot of guest memory; a texel buffer at an image's address is
                    // re-tiled from that image by every full bind once the GPU wrote it
                    // (SynchronizeMemoryFromImage), and HTile is refilled.
                    if (SotcStats::kBindCacheRanges &&
                        (static_cast<const void*>(buffer) ==
                             static_cast<const void*>(&buffer_cache.GetStreamBuffer()) ||
                         (desc.is_formatted && [&] {
                             if (texture_cache.IsMeta(vbase)) {
                                 return true;
                             }
                             // Any image FindImageFromRange could return now or once the GPU
                             // wrote it (it matches the base address only).
                             const auto texture_lock = texture_cache.BatchLock();
                             bool aliased = false;
                             texture_cache.ForEachImageInRegion(
                                 vbase, size,
                                 [&](VideoCore::ImageId, VideoCore::Image& image) {
                                     aliased |= image.info.guest_address == vbase;
                                     return aliased;
                                 });
                             return aliased;
                         }()))) {
                        bind_cache_uncacheable = true;
                    }
                }
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    const SotcTrace::ScopeMin trace_inval{"invalidate images"}; // sotc: SOTC_TRACE
                    texture_cache.InvalidateMemoryFromGPU(vbase, size);
                    // sotc: SOTC_BIND_MARK_ONCE (kBindMarkOnce): ObtainBuffer already noted this
                    // range when it ran for this binding; the second note only bumps mem_epoch.
                    const bool note_again = !(kBindMarkOnce && obtain_called);
                    if (note_again) {
                        Core::MemoryManager::NoteHostWrite(vbase, size, 7,
                                                           stage.pgm_hash);
                    }
                    // sotc: SOTC_OP_COMPUTE_BIND. A bind being recorded keeps the write as a record: the range stamp (which
                    // drops the binds reading it) is noted after the commit (ApplyOwnWriteStamps), and a hit replays the
                    // write from the record (ReplayOwnWrites). An ineligible bind notes the range now, as before.
                    if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                        // SOTC_BPE_FIX=1 clamps the size by heap metadata that a hit does not re-read: not stored.
                        if (size == 0 || VideoCore::BpeNative::Mode() == 1) {
                            bind_cache_uncacheable = true; // no range to replay (or a clamp the key does not hold)
                        } else {
                            BindCacheWrite rec{};
                            rec.addr = vbase;
                            rec.size = size;
                            rec.mark_offset = mark_offset;
                            rec.mark_size = mark_size;
                            rec.buf_index = buf_index;
                            rec.pgm_hash = stage.pgm_hash;
                            const auto& t = t_direct_dispatch;
                            rec.dispatch_groups = t.hash == stage.pgm_hash ? t.groups : 0;
                            rec.dispatch_threads = t.hash == stage.pgm_hash ? t.threads : 0;
                            rec.note_again = note_again;
                            rec.buffer = buffer;
                            rec.offset = offset;
                            bind_cache_writes_rec.push_back(rec);
                        }
                    } else if (SotcStats::kBindCacheRanges) { // sotc: drops the binds reading it
                        BindCacheInvalidate(vbase, size);
                    }
                }
                buffer_barrier_need |= Passes::BufferNeed( // sotc: SOTC_PASSES (see BindVertexBuffers)
                    BarrierStatic::BufferChecks() &&
                    runtime.IsBufferAccessed(buffer, offset, size, desc.is_written));
                SotcOpPhases::ChargeBinding(desc.is_written, ph_upload0, ph_t0); // sotc: SOTC_OP_PHASES
            }
            if (drop_desc) {
                // sotc: SOTC_N5_BDA_BUFFERS=1: no set write for this slot. The layout slots still
                // advance, as the write would have advanced them.
                ++binding.unified;
                ++binding.buffer;
                continue;
            }
        }

        set_write_sets[set_write_index] = set;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

// sotc: the final composite samples the UI layer and the scene: UI first in the base game's
// shader, scene first in the PS4 Pro one (0x9e6767fc). The UI layer is the output-size sRGB image.
u32 Rasterizer::CompositeUiIndex(VideoCore::ImageId first_id) {
    const auto& first = texture_cache.GetImage(first_id);
    return first.info.pixel_format == vk::Format::eR16G16B16A16Sfloat ? 1 : 0;
}

// sotc: the composite predicates of BindTexturesFull (hudless capture, DLSS composite, SOTC_DLSS_PROBE log). The same
// function answers BindHeapImages (SOTC_HEAP_COMPOSITE_OP), so the heap path sends exactly the draws the classic bind
// would act on to that bind. Pure: no state is changed here. The caller holds the texture cache lock.
Rasterizer::CompositeKinds Rasterizer::CompositeOf(const Shader::Info& stage, const VideoCore::ImageId* ids,
                                                   u32 count, VideoCore::ImageId cb0) {
    // sotc: TakeHudless: the final composite samples the UI layer (an sRGB render target) and the
    // fp16 scene, nothing else. Off unless SOTC_FG_HUDLESS=1: with FSR 3 the generated frames of
    // the title screen (faded logo) and of loading screens (the scene left behind the opaque UI
    // layer shows through) came out wrong, the real frame's UI not put back over them.
    static const bool hudless_on = [] {
        const char* v = std::getenv("SOTC_FG_HUDLESS");
        return v && v[0] == '1';
    }();
    // sotc: SOTC_DLSS_PROBE=1 logs each fragment shader that draws into a video out surface once.
    static const bool dlss_probe = std::getenv("SOTC_DLSS_PROBE") != nullptr;
    CompositeKinds k{};
    if (stage.sw_stage != Shader::SwStage::Fragment) {
        return k;
    }
    k.probe = dlss_probe && cb0 && texture_cache.GetImage(cb0).usage.vo_surface;
    if (count != 2 || !ids[0] || !ids[1]) {
        return k;
    }
    k.ui_index = CompositeUiIndex(ids[0]);
    const auto& ui = texture_cache.GetImage(ids[k.ui_index]);
    const auto& scene = texture_cache.GetImage(ids[k.ui_index ^ 1]);
    const bool composite_images = ui.usage.render_target && ui.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
                                  scene.info.pixel_format == vk::Format::eR16G16B16A16Sfloat;
    k.hudless = hudless_on && composite_images && FrameGen::Current() != FrameGen::Mode::Off;
    // sotc: DLSS super resolution: the game's final composite samples the UI layer (an sRGB render
    // target) and the tonemapped fp16 scene, nothing else, into a video out surface.
    k.dlss = Dlssg::SuperResolution() && cb0 && texture_cache.GetImage(cb0).usage.vo_surface &&
             composite_images;
    return k;
}


// sotc: DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL serves the depth target (stencil writes) and the
// sampling of its depth aspect, so both uses ask for the same stages and accesses: the image state
// does not change from one draw to the next and the render pass is not ended for it.
static constexpr vk::PipelineStageFlags2 kDepthReadOnlyStencilStages =
    vk::PipelineStageFlagBits2::eAllCommands;
static constexpr vk::AccessFlags2 kDepthReadOnlyStencilAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
    vk::AccessFlagBits2::eShaderRead;

bool Rasterizer::DepthSampledAsAttachment(VideoCore::ImageId image_id, const VideoCore::Image& image,
                                          const VideoCore::TextureCache::ImageDesc& desc,
                                          bool compute) const {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DEPTH_RO");
        return !v || v[0] != '0';
    }();
    if (!on || compute || !image.info.props.has_stencil || image_id != db_desc.first) {
        return false;
    }
    if (!liverpool->DrawRegs().depth_control.stencil_enable || db_desc.second.view_info.is_storage) {
        return false;
    }
    const vk::Format fmt = desc.view_info.format;
    return Vulkan::LiverpoolToVK::IsFormatDepthCompatible(fmt) &&
           !Vulkan::LiverpoolToVK::IsFormatStencilCompatible(fmt);
}

void Rasterizer::PrefetchImagesImpl(const Pipeline* pipeline) {
    const u64 now = SotcStats::NowNs(); // sotc: one clock read per draw
    // sotc: SOTC_ASYNC_IMAGE_CREATE. Reads the T#s as BindTextures will on the draw thread (from
    // this thread's copy of the per-draw data, Shader::g_info_slot 0) with the same validity
    // checks; descriptor arrays are left out.
    for (const Shader::Info* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& image_desc : stage->images) {
            if (image_desc.array_size != 0) {
                continue;
            }
            const auto tsharp = image_desc.GetSharp(*stage);
            const auto data_fmt = tsharp.GetDataFmt();
            const auto num_fmt = tsharp.GetNumberFmt();
            if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
                !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
                !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
                continue;
            }
            texture_cache.PrefetchTexture(tsharp, image_desc, now);
        }
    }
}

namespace {
// sotc: SOTC_TEX_BLOCK counters, one line on stderr per 5 s (TexBlockReport).
std::atomic<u64> tex_block_lookups{}, tex_block_hits{}, tex_block_stores{};
std::atomic<u64> tex_block_mismatches{};
std::atomic<s64> tex_block_report_due{};

void TexBlockReport() {
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 due = tex_block_report_due.load(std::memory_order_relaxed);
    if (now < due || !tex_block_report_due.compare_exchange_strong(due, now + 5'000'000'000LL)) {
        return;
    }
    std::fprintf(stderr,
                 "SOTC TEX BLOCK: lookups %llu, hits %llu, stores %llu, verify mismatches %llu\n",
                 static_cast<unsigned long long>(tex_block_lookups.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(tex_block_hits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(tex_block_stores.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     tex_block_mismatches.load(std::memory_order_relaxed)));
}
} // namespace

int Rasterizer::TexBlockMode() {
    // sotc: SOTC_TEX_BLOCK, read once. 1: a stage's texture block is replayed from the memo when
    // it can be; 2: also runs the full bind on a would-be hit and compares the two (verify).
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TEX_BLOCK");
        if (!v) {
            return 0;
        }
        return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

bool Rasterizer::MakeTextureBlockKey(const Shader::Info& stage, u32 set,
                                     const PreparedResources* pr, const PrStage* pr_stage,
                                     TexBlockKey& key) {
    // Not keyed: the binds the memo does not answer (storage and fmask stages, descriptor arrays,
    // samplers the full bind rewrites), the composite paths that act on a fragment stage's two
    // images (TakeHudless, DLSS, SOTC_DLSS_PROBE: their side effects are not replayed), and stages
    // without images or samplers. sotc: while the bind cache records, the memo still answers and
    // replays the cache records (see TryTextureBlock), except with SOTC_BIND_CACHE_FRAME (its
    // lookups are not replayed) or a composite probe (its log is not).
    static const bool composite_probe = [] {
        const char* h = std::getenv("SOTC_FG_HUDLESS");
        return (h && h[0] == '1') || std::getenv("SOTC_DLSS_PROBE") != nullptr;
    }();
    if ((bind_cache_recording && (SotcStats::kBindCacheFrame || composite_probe)) ||
        stage.images.size() > MaxBlockImages || stage.samplers.size() > MaxBlockSamplers ||
        (stage.images.empty() && stage.samplers.empty()) || !stage.fmasks.empty()) {
        return false;
    }
    if (stage.sw_stage == Shader::SwStage::Fragment && stage.images.size() <= 2 &&
        (composite_probe || Dlssg::SuperResolution())) {
        return false;
    }
    auto& k = key.key;
    u32 n = 0;
    k[n++] = u64{stage.images.size()} | u64{stage.samplers.size()} << 8 | u64{set} << 16 |
             u64(static_cast<u32>(stage.sw_stage)) << 24;
    k[n++] = stage.CacheHash(); // sotc: a variant's layout differs (SOTC_HEAP)
    const VAddr bc_base = liverpool->DrawRegs().ta_bc_base.Address();
    k[n++] = bc_base;
    for (u32 index = 0; index < stage.images.size(); ++index) {
        const auto& image_desc = stage.images[index];
        if (image_desc.is_written || image_desc.array_size != 0) {
            return false;
        }
        // The T# the full bind reads: the prepared one when its entry is resolved.
        const PrImage* pi = pr && pr_stage ? &pr->images[pr_stage->img_first + index] : nullptr;
        if (pi && pi->verdict == 0) {
            pi = nullptr;
        }
        const AmdGpu::Image tsharp = pi ? pi->tsharp : image_desc.GetSharp(stage);
        k[n++] = u64{image_desc.is_depth} | u64{image_desc.is_atomic} << 1 |
                 u64{image_desc.is_array} << 2 | u64{image_desc.is_r128} << 3 |
                 u64{image_desc.constant_mip_index} << 8 |
                 u64(image_desc.mip_fallback_mode) << 16 | u64(image_desc.post_op) << 24;
        static_assert(sizeof(tsharp) == 4 * sizeof(u64));
        std::memcpy(&k[n], &tsharp, sizeof(tsharp));
        n += 4;
    }
    for (const auto& sampler : stage.samplers) {
        const AmdGpu::Sampler ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                bc_base == 0)) {
            return false; // the full bind logs and rewrites these
        }
        k[n++] = u64{sampler.is_depth};
        static_assert(sizeof(ssharp) == 2 * sizeof(u64));
        std::memcpy(&k[n], &ssharp, sizeof(ssharp));
        n += 2;
    }
    u64 hash = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < n; ++i) {
        hash = (hash ^ k[i]) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= hash >> 29;
    }
    key.words = n;
    key.hash = hash;
    key.slot = static_cast<u32>((hash ^ (hash >> 32)) % NumTexBlockMemos);
    return true;
}

bool Rasterizer::TexBlockMatches(const TexBlockMemo& memo, const TexBlockKey& key) {
    return memo.valid && memo.key_words == key.words &&
           std::memcmp(memo.key.data(), key.key.data(), key.words * sizeof(u64)) == 0;
}

bool Rasterizer::TryTextureBlock(const Shader::Info& stage, const TexBlockKey& key, u32 set,
                                 Shader::Backend::Bindings& binding, bool mark_lods) {
    const TexBlockMemo& memo = (*tex_block_memos)[key.slot];
    if (!TexBlockMatches(memo, key)) {
        return false;
    }
    // sotc: SOTC_N5_BINDLESS: the heap slots the block names are as stored (no release or eviction
    // since moved the generation).
    if (memo.bindless_gen != BindlessGeneration()) {
        return false;
    }
    // A block that may hold a mip tail depends on the whole image set (DecideRebase looks at the
    // images overlapping the tail) and on the GPU-written tails: those two must not have moved.
    if (memo.candidate &&
        (texture_cache.BindGeneration() != memo.bind_generation ||
         VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed) != memo.gpu_epoch)) {
        return false;
    }
    // The bound image is the one stored, and its backing is in the state the bind left it in
    // (state_version), with nothing that makes the bind go another way (needs_rebind, a render
    // target or a feedback loop).
    const auto bound_current = [&](const TexBlockEntry& e) {
        const VideoCore::Image* image = texture_cache.TryGetImage(e.bound, e.bound_uid);
        return image && image->backing == e.backing &&
               image->backing->state_version == e.version && !image->binding.needs_rebind &&
               (image->info.props.is_depth ||
                (!image->binding.force_general && !image->binding.is_target));
    };
    // Checks only, nothing changed yet: the lookup's generation and image, the bound image.
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (!e.null && (texture_cache.ImageAddrGeneration(e.addr) != e.addr_gen ||
                        !texture_cache.TryGetImage(e.found, e.found_uid) || !bound_current(e))) {
            return false;
        }
    }

    // From here the bind's side effects are made in the order of the full bind: the lookups'
    // stamps, the depth links and bound flags, then per image UpdateImage (FindTexture) and the
    // transition. A miss after any of them gives the stamps back: the full bind makes them again.
    const u64 tick = scheduler.CurrentTick();
    std::array<std::pair<u64, u64>, MaxBlockEntries> stamps{}; // (tick_prev, tick_accessed) before
    const auto restore_stamps = [&] {
        for (u32 i = memo.num_entries; i-- > 0;) {
            const TexBlockEntry& e = memo.entries[i];
            if (!e.null) {
                VideoCore::Image& found = texture_cache.GetImage(e.found);
                found.tick_prev_accessed = stamps[i].first;
                found.tick_accessed_last = stamps[i].second;
            }
        }
    };
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (e.null) {
            continue;
        }
        VideoCore::Image& found = texture_cache.GetImage(e.found);
        stamps[i] = {found.tick_prev_accessed, found.tick_accessed_last};
        found.tick_prev_accessed = found.tick_accessed_last;
        found.tick_accessed_last = tick;
    }
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (e.null) {
            continue;
        }
        const VideoCore::ImageId depth =
            texture_cache.GetAssociatedDepth(texture_cache.GetImage(e.found));
        if (depth != (e.redirect ? e.bound : VideoCore::ImageId{})) {
            restore_stamps();
            return false;
        }
        texture_cache.GetImage(e.bound).binding.is_bound = 1u;
    }

    std::array<vk::DescriptorImageInfo, MaxBlockEntries> infos{};
    std::array<VideoCore::ImageId, MaxBlockEntries> ids{};
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (e.null) {
            infos[i] = vk::DescriptorImageInfo(VK_NULL_HANDLE, VK_NULL_HANDLE,
                                               vk::ImageLayout::eGeneral);
            continue;
        }
        // FindTexture's UpdateImage (uploads what the guest wrote), then the second pass's checks
        // on what that changed.
        if (!bound_current(e)) {
            restore_stamps();
            return false;
        }
        texture_cache.UpdateImage(e.bound);
        if (!bound_current(e)) {
            restore_stamps();
            return false;
        }
        VideoCore::Image& image = texture_cache.GetImage(e.bound);
        // As the second pass: no depth-stencil image (TryTextureBlock stores none), so the layout
        // is the sampled one of its kind.
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eAllCommands,
                                         vk::AccessFlagBits2::eShaderRead, e.range);
        image.usage.texture = 1u;
        infos[i] = vk::DescriptorImageInfo(VK_NULL_HANDLE, e.view,
                                           SotcArch::Layouts::Pick(image.backing->state.layout)); // sotc: A1
        ids[i] = e.bound;
    }
    // sotc: SOTC_N5_BINDLESS: UpdateImage above may have destroyed views, and a destroyed view gives
    // its heap slots back. A block that names one is not replayed (as the checks above).
    if (memo.bindless_gen != BindlessGeneration()) {
        restore_stamps();
        return false;
    }

    // sotc: SOTC_BIND_CACHE: the records the full bind makes, in its order (LOD banks in its first
    // pass, sampled images in its second). Nothing below can return false.
    if (bind_cache_recording) {
        for (u32 i = 0; i < memo.num_lods; ++i) {
            bind_cache_lod_rec.push_back(memo.lod_banks[i]);
        }
        for (u32 i = 0; i < memo.num_entries; ++i) {
            const TexBlockEntry& e = memo.entries[i];
            if (e.null) {
                continue;
            }
            // The sampled layout, as the infos loop above.
            const auto new_layout = texture_cache.GetImage(e.bound).info.props.is_depth
                                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                        : vk::ImageLayout::eShaderReadOnlyOptimal;
            BindCacheNoteImage(e.bound, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                               vk::AccessFlagBits2::eShaderRead, e.range);
        }
    }
    // The bindings, as BindTexturesFull makes them: the bound images (each descriptor's infos
    // and one write), then the LOD marks and the samplers.
    for (u32 i = 0; mark_lods && i < memo.num_lods; ++i) {
        SotcLod::Mark(memo.lod_banks[i]);
    }
    u32 entry = 0;
    for (u32 d = 0; d < memo.num_descs; ++d) {
        const u32 count = memo.desc_counts[d];
        const u32 info_index = image_infos.size();
        for (u32 j = 0; j < count; ++j, ++entry) {
            image_infos.push_back(infos[entry]);
            image_info_images[image_infos.size() - 1] = ids[entry];
            if (ids[entry]) {
                bound_images.emplace_back(ids[entry]);
            }
        }
        // sotc: SOTC_N5_BINDLESS: a heap image writes its push word, no descriptor and no binding.
        if (memo.bindless_push[d]) {
            const u32 heap_slot = memo.bindless_push[d] - 1u;
            const VideoCore::BindlessRef& ref = memo.bindless_block[d];
            if (slot_words_fill) { // sotc: a pipeline without slot words (SOTC_PACKET) never pushes them
                const u32 shift = (heap_slot & 1) * 16;
                auto& word = bindless_slots.image_words[heap_slot / 2];
                word = (word & ~(0xFFFFu << shift)) | (ref.slot << shift);
            }
            if (bindless_images && ref.slot != 0) {
                bind_bindless_rec.push_back(ref);
                bindless_images->Use(ref.key, ref.slot);
                if (bindless_images->Mode() == 2) {
                    bindless_images->Verify(ref.key, ref.slot, ref.view);
                }
            }
            continue;
        }
        set_write_sets[set_write_index] = set;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = count;
        set_write.descriptorType = vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[info_index];
        binding.unified += count;
    }
    const VideoCore::ImageId first = memo.num_entries == 1 ? ids[0] : VideoCore::ImageId{};
    const VideoCore::Image* ui_asset = UiLinearAsset(stage, memo.num_entries, first);
    // sotc: SOTC_BIND_CACHE: as BindTexturesFull. The composite probes are refused while recording
    // (MakeTextureBlockKey), so only the UI filter and hudless_ui_info can set it here.
    if (bind_cache_recording && (ui_asset || hudless_ui_info >= 0)) {
        bind_cache_uncacheable = true;
    }
    BindSamplers(stage, binding, set, ui_asset);
    return true;
}

// sotc: SOTC_BINDSET mode 1. True when Image::GetBarriers would make no barrier for a full-range transition to `layout`
// with `access`: the backing is whole (no per-subresource states) and already holds that layout and access. Such a
// transition changes nothing, so Rasterizer::BindHeapImages skips it.
static bool TransitSettled(const VideoCore::Image& image, vk::ImageLayout layout, vk::AccessFlags2 access,
                           const VideoCore::SubresourceRange& range) {
    if (!image.backing || !image.backing->subresource_states.empty()) {
        return false;
    }
    if (range.base != VideoCore::SubresourceBase{} || range.extent != image.info.resources) {
        return false;
    }
    const auto& state = image.backing->state;
    return state.layout == layout && state.access_mask == access;
}

// sotc: SOTC_HEAP real mode. The classic bind's second pass for the images of a heap stage, without descriptors. The
// index table holds the bound image and range each T# resolved to (TableMemo::Resolve ran the first pass's lookups and,
// for a hit, the per-bind UpdateImage). What is left is done here: the bound flag (BeginRendering reads it to see a
// feedback loop, ResetBindings clears it through bound_images), the transition (its barrier), the texture usage bit.
// Not made, as the heap stage has none of it: descriptors, storage images (HeapStaticRefusal), samplers (the table
// holds their heap indices), the bind cache and the texture block records (a heap op takes neither).
// sotc: SOTC_BIND_CACHE_PACKET. The decision of BindHeapImages, separated so that a cache hit can take it before it
// binds anything: 0 the table's images are bound here, else the reason of the classic bind (the same codes as
// NoteBindClassic). Reads the images: the caller holds the texture cache lock.
u32 Rasterizer::HeapDeclineReason(const Shader::Info& stage, const SotcArch::TableMemo::TableBinds& binds) {
    // Decided before anything changes: the cases the classic bind handles with its own transitions.
    for (u32 i = 0; i < binds.count; ++i) {
        if (!binds.image[i]) {
            continue;
        }
        const auto& image = texture_cache.GetImage(binds.image[i]);
        if (image.binding.needs_rebind) {
            return 3;
        }
        // A render target of this draw, or an image forced to general: the feedback loop (classic second pass).
        if (image.binding.is_target || image.binding.force_general) {
            return 1;
        }
        // A stencil image: its transition depends on the draw's depth registers (DepthSampledAsAttachment).
        if (image.info.props.has_stencil) {
            return 2;
        }
    }
    // sotc: SOTC_HEAP_COMPOSITE_OP. The composite draws (hudless capture, DLSS composite, the probe) run their side
    // effects in the classic bind (BindTexturesFull). The heap shader reads its images from the table, so that bind
    // serves it as it serves the other fallbacks; the op is not heap for this bind only. Off: the program was refused.
    if (SotcArch::Heap::CompositeOpOn()) {
        const CompositeKinds composite = CompositeOf(stage, binds.image.data(), binds.count, cb_descs[0].first);
        if (composite.hudless || composite.dlss || composite.probe) {
            return 4;
        }
    }
    return 0;
}

bool Rasterizer::BindHeapImages(const Shader::Info& stage, const SotcArch::TableMemo::TableBinds& binds) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the pass, as BindTextures
    if (const u32 reason = HeapDeclineReason(stage, binds)) {
        SotcArch::TableMemo::NoteBindClassic(reason);
        return false;
    }
    for (u32 i = 0; i < binds.count; ++i) {
        if (!binds.image[i]) {
            continue;
        }
        auto& image = texture_cache.GetImage(binds.image[i]);
        image.binding.is_bound = 1u;
        // The layout is the sampled one of the image's kind, as the classic bind names it (GENERAL under the unified layouts).
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        // sotc: SOTC_BINDSET. Mode 1 skips the transition when the backing is already settled in the read's state (no barrier
        // would be made). Mode 2 makes it as before and counts the barriers the prediction did not make.
        const int bs_mode = SotcArch::TableMemo::BindSetMode();
        const bool settled =
            bs_mode != 0 && TransitSettled(image, new_layout, vk::AccessFlagBits2::eShaderRead, binds.range[i]);
        if (bs_mode == 1 && settled) {
            SotcArch::TableMemo::NoteTransitSkipped();
        } else {
            const bool barrier = runtime.Transit(&image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                                                 vk::AccessFlagBits2::eShaderRead, binds.range[i]);
            needs_barrier |= barrier;
            if (bs_mode == 2 && settled) {
                SotcArch::TableMemo::NoteTransitVerify(barrier);
            }
        }
        image.usage.texture |= 1u;
        bound_images.emplace_back(binds.image[i]);
    }
    SotcArch::TableMemo::NoteBindSkipped();
    return true;
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                              u32 set, const PreparedResources* pr, const PrStage* pr_stage) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the pass
    const int mode = TexBlockMode();
    TexBlockKey key;
    if (mode == 0 || !MakeTextureBlockKey(stage, set, pr, pr_stage, key)) {
        BindTexturesFull(stage, binding, set, pr, pr_stage, nullptr, nullptr);
        return;
    }
    TexBlockMemo& slot = (*tex_block_memos)[key.slot];
    tex_block_lookups.fetch_add(1, std::memory_order_relaxed);
    TexBlockReport();
    if (mode == 1) {
        if (TryTextureBlock(stage, key, set, binding)) {
            tex_block_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        BindTexturesFull(stage, binding, set, pr, pr_stage, &key, &slot);
        return;
    }

    // SOTC_TEX_BLOCK=2 (verify): a would-be hit is replayed, its outputs and the lookups' stamps
    // are taken back, the full bind is made and the two are compared. The full bind's result is
    // the one kept. The replay's UpdateImage and transitions run twice: both are idempotent here.
    // sotc: while SOTC_BIND_CACHE records, the replay's cache records and uncacheable flag are
    // compared with the full bind's the same way.
    const u32 info_base = image_infos.size();
    const u32 write_base = set_write_index;
    const u32 bound_base = bound_images.size();
    const Shader::Backend::Bindings binding_base = binding;
    struct Stamp {
        VideoCore::ImageId id;
        u64 prev;
        u64 accessed;
    };
    std::array<Stamp, MaxBlockEntries> stamps{};
    u32 num_stamps = 0;
    if (TexBlockMatches(slot, key)) {
        for (u32 i = 0; i < slot.num_entries; ++i) {
            const TexBlockEntry& e = slot.entries[i];
            const VideoCore::Image* found =
                e.null ? nullptr : texture_cache.TryGetImage(e.found, e.found_uid);
            if (found) {
                stamps[num_stamps++] = {e.found, found->tick_prev_accessed,
                                        found->tick_accessed_last};
            }
        }
    }
    // sotc: SOTC_BIND_CACHE records of the replay: copied for the comparison below, then taken
    // back with its outputs. The full bind makes its own.
    const size_t lod0 = bind_cache_lod_rec.size();
    const size_t img0 = bind_cache_images_rec.size();
    const size_t smp0 = bind_cache_samplers_rec.size();
    const bool unc0 = bind_cache_uncacheable;
    // sotc: each side starts from a clear flag, so what this stage's replay and full bind set is
    // compared even when an earlier stage already set it (unc0 is put back after both).
    bind_cache_uncacheable = false;
    // sotc: no LOD marks from the replay: the full bind below makes them once, as in mode 0.
    const bool replayed = TryTextureBlock(stage, key, set, binding, false);
    const std::vector<u32> r_lods(bind_cache_lod_rec.begin() + lod0, bind_cache_lod_rec.end());
    const std::vector<BindCacheImage> r_recs(bind_cache_images_rec.begin() + img0,
                                             bind_cache_images_rec.end());
    const std::vector<BindCacheSampler> r_samplers(bind_cache_samplers_rec.begin() + smp0,
                                                   bind_cache_samplers_rec.end());
    const bool r_uncacheable = bind_cache_uncacheable;
    bind_cache_lod_rec.erase(bind_cache_lod_rec.begin() + lod0, bind_cache_lod_rec.end());
    bind_cache_images_rec.erase(bind_cache_images_rec.begin() + img0, bind_cache_images_rec.end());
    bind_cache_samplers_rec.erase(bind_cache_samplers_rec.begin() + smp0,
                                  bind_cache_samplers_rec.end());
    bind_cache_uncacheable = unc0;
    if (!replayed) {
        BindTexturesFull(stage, binding, set, pr, pr_stage, &key, &slot);
        return;
    }
    tex_block_hits.fetch_add(1, std::memory_order_relaxed);
    const std::vector<vk::DescriptorImageInfo> r_infos(image_infos.begin() + info_base,
                                                       image_infos.end());
    const std::vector<VideoCore::ImageId> r_ids(image_info_images.begin() + info_base,
                                                image_info_images.begin() + image_infos.size());
    const std::vector<VideoCore::ImageId> r_bound(bound_images.begin() + bound_base,
                                                  bound_images.end());
    using WriteKey = std::array<s64, 4>; // dstBinding, count, type, index into image_infos
    const auto write_key = [&](const vk::WriteDescriptorSet& w) {
        return WriteKey{s64(w.dstBinding), s64(w.descriptorCount),
                        s64(static_cast<u32>(w.descriptorType)),
                        static_cast<s64>(static_cast<const vk::DescriptorImageInfo*>(w.pImageInfo) -
                                         image_infos.data())};
    };
    std::vector<WriteKey> r_writes;
    for (u32 i = write_base; i < set_write_index; ++i) {
        r_writes.push_back(write_key(set_writes[i]));
    }
    const u32 r_unified = binding.unified;
    // Take the replay's outputs back, and the stamps it made.
    image_infos.resize(info_base);
    set_write_index = write_base;
    bound_images.resize(bound_base);
    binding = binding_base;
    for (u32 i = num_stamps; i-- > 0;) {
        VideoCore::Image& found = texture_cache.GetImage(stamps[i].id);
        found.tick_prev_accessed = stamps[i].prev;
        found.tick_accessed_last = stamps[i].accessed;
    }

    bind_cache_uncacheable = false; // sotc: this stage's own flag (see unc0 above)
    BindTexturesFull(stage, binding, set, pr, pr_stage, &key, &slot);
    const bool f_uncacheable = bind_cache_uncacheable;
    bind_cache_uncacheable = unc0 || f_uncacheable; // the full bind only ever sets it

    bool same = binding.unified == r_unified && image_infos.size() - info_base == r_infos.size() &&
                set_write_index - write_base == r_writes.size() &&
                bound_images.size() - bound_base == r_bound.size();
    for (u32 i = 0; same && i < r_infos.size(); ++i) {
        const auto& a = r_infos[i];
        const auto& b = image_infos[info_base + i];
        same = a.sampler == b.sampler && a.imageView == b.imageView &&
               a.imageLayout == b.imageLayout && r_ids[i] == image_info_images[info_base + i];
    }
    for (u32 i = 0; same && i < r_writes.size(); ++i) {
        same = r_writes[i] == write_key(set_writes[write_base + i]);
    }
    for (u32 i = 0; same && i < r_bound.size(); ++i) {
        same = r_bound[i] == bound_images[bound_base + i];
    }
    // sotc: SOTC_BIND_CACHE records of the full bind against the replay's. Neither adds any
    // while not recording, so the counts match then.
    bool rec_same = bind_cache_lod_rec.size() - lod0 == r_lods.size() &&
                    bind_cache_images_rec.size() - img0 == r_recs.size() &&
                    bind_cache_samplers_rec.size() - smp0 == r_samplers.size();
    for (size_t i = 0; rec_same && i < r_samplers.size(); ++i) {
        const BindCacheSampler& a = r_samplers[i];
        const BindCacheSampler& b = bind_cache_samplers_rec[smp0 + i];
        rec_same = std::memcmp(&a.sharp, &b.sharp, sizeof(a.sharp)) == 0 &&
                   a.handle == b.handle && a.slot == b.slot && a.is_depth == b.is_depth &&
                   a.bindless == b.bindless;
    }
    for (size_t i = 0; rec_same && i < r_lods.size(); ++i) {
        rec_same = r_lods[i] == bind_cache_lod_rec[lod0 + i];
    }
    for (size_t i = 0; rec_same && i < r_recs.size(); ++i) {
        const BindCacheImage& a = r_recs[i];
        const BindCacheImage& b = bind_cache_images_rec[img0 + i];
        rec_same = a.id == b.id && a.layout == b.layout && a.stages == b.stages &&
                   a.access == b.access && a.range.base.level == b.range.base.level &&
                   a.range.base.layer == b.range.base.layer &&
                   a.range.extent.levels == b.range.extent.levels &&
                   a.range.extent.layers == b.range.extent.layers && a.guest_lo == b.guest_lo &&
                   a.guest_hi == b.guest_hi && a.uid == b.uid;
    }
    // Both start from a clear flag. The replay only sets it while recording, so compare then only.
    if (bind_cache_recording) {
        rec_same = rec_same && f_uncacheable == r_uncacheable;
    }
    same = same && rec_same;
    if (!same) {
        tex_block_mismatches.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "SOTC TEX BLOCK mismatch: shader %#llx, infos %zu/%zu, writes %zu/%zu, "
                     "images %zu/%zu, lods %zu/%zu, recs %zu/%zu\n",
                     static_cast<unsigned long long>(stage.pgm_hash), r_infos.size(),
                     static_cast<size_t>(image_infos.size() - info_base), r_writes.size(),
                     static_cast<size_t>(set_write_index - write_base), r_bound.size(),
                     static_cast<size_t>(bound_images.size() - bound_base), r_lods.size(),
                     static_cast<size_t>(bind_cache_lod_rec.size() - lod0), r_recs.size(),
                     static_cast<size_t>(bind_cache_images_rec.size() - img0));
    }
}

// sotc: SOTC_TSLOT: a binding whose ImageDesc the first loop of BindTexturesFull did not build (its
// identity memo entry answered it). The second pass builds the descriptor from these when the image
// needs a rebind. `view_info` is the view info the memo gave.
struct LazyBind {
    u32 index;
    AmdGpu::Image tsharp;
    const Shader::ImageResource* resource;
    VideoCore::ImageViewInfo view_info;
};

// sotc: SOTC_TSLOT: whether ImageDesc::InitMipTail makes a mip-tail candidate of a plain sampled T#
// (texture_cache.cpp), from the T# and the resource's is_depth; `plain` excludes the other inputs
// (is_written, mip fallback). A candidate is never memoized (FindTextureIdentity bypasses it), so its
// identity probe cannot hit and mode 1 does not probe it. Mode 2 checks this against the built ImageDesc.
static bool TailCandidateOf(const AmdGpu::Image& image, bool is_depth) {
    const auto image_type = image.GetType();
    return !(is_depth || image_type == AmdGpu::ImageType::Color3D ||
             image_type == AmdGpu::ImageType::Color2DMsaa ||
             image_type == AmdGpu::ImageType::Color2DMsaaArray || image.base_level == 0 ||
             image.base_level > image.last_level ||
             (VideoCore::MipTailMode() == 1 && !image.lod_hw_cnt_en));
}

void Rasterizer::BindTexturesFull(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                                  u32 set, const PreparedResources* pr, const PrStage* pr_stage,
                                  const TexBlockKey* tex_key, TexBlockMemo* rec) {
    image_bindings.clear();
    // sotc: SOTC_TEX_BLOCK: what the memo records of this bind (rec, see TryTextureBlock).
    // store_ok goes false on anything the replay does not repeat.
    bool store_ok = true;
    if (rec) {
        rec->valid = false;
        rec->key_words = tex_key->words;
        std::copy_n(tex_key->key.begin(), tex_key->words, rec->key.begin());
        rec->candidate = false;
        rec->num_lods = 0;
        rec->bind_generation = texture_cache.BindGeneration();
        rec->gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
        // sotc: SOTC_N5_BINDLESS: the heap generation at the start. A release or an eviction later in
        // this bind moves it, and TryTextureBlock then refuses the block.
        rec->bindless_gen = BindlessGeneration();
        rec->bindless_push.fill(0);
    }
    const u32 first_image_idx = image_infos.size();
    // sotc: SOTC_TSLOT: the switch; the T# of each stage image, read once (the bindless branch uses
    // it); the bindings the first loop left without a descriptor (lazy_binds).
    const int tslot_mode = VideoCore::TsharpSlotCache::Mode();
    boost::container::small_vector<AmdGpu::Image, 8> stage_sharps;
    boost::container::small_vector<LazyBind, 8> lazy_binds;
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    u32 image_index = 0;
    for (const auto& image_desc : stage.images) {
        // sotc: SOTC_PR_RESOLVE: the T# and its verdict as the prep thread computed them (PrImage).
        const PrImage* pi =
            pr && pr_stage ? &pr->images[pr_stage->img_first + image_index] : nullptr;
        if (pi && pi->verdict == 0) {
            pi = nullptr;
        }
        ++image_index;
        const auto tsharp = pi ? pi->tsharp : image_desc.GetSharp(stage);
        stage_sharps.push_back(tsharp);
        // sotc: SOTC_TEX_REPEAT=1 (measurement only): how often a texture bind repeats a T# (and the binding's
        // view-relevant flags) already bound in the same submit (tick) or the same frame, the ceiling of a per-submit
        // memo of resolved texture binds. Draw thread only.
        static const bool tex_repeat = [] {
            const char* v = std::getenv("SOTC_TEX_REPEAT");
            return v && v[0] == '1';
        }();
        if (tex_repeat) {
            thread_local std::unordered_set<u64> seen_tick, seen_frame;
            thread_local u64 tick_of_set = 0, frame_of_set = 0;
            const u64 tick = scheduler.CurrentTick();
            const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
            if (tick != tick_of_set) {
                seen_tick.clear();
                tick_of_set = tick;
                SotcStats::tex_repeat_ticks.fetch_add(1, std::memory_order_relaxed);
            }
            if (frame != frame_of_set) {
                seen_frame.clear();
                frame_of_set = frame;
            }
            u64 key = 0xcbf29ce484222325ull;
            const auto* bytes = reinterpret_cast<const u8*>(&tsharp);
            for (size_t b = 0; b < sizeof(tsharp); ++b) {
                key = (key ^ bytes[b]) * 0x100000001b3ull;
            }
            key ^= (u64{image_desc.is_depth} << 1) ^ (u64{image_desc.is_written} << 2) ^
                   (u64{image_desc.is_array} << 3);
            SotcStats::tex_repeat_binds.fetch_add(1, std::memory_order_relaxed);
            if (!seen_tick.insert(key).second) {
                SotcStats::tex_repeat_tick_hits.fetch_add(1, std::memory_order_relaxed);
            }
            if (!seen_frame.insert(key).second) {
                SotcStats::tex_repeat_frame_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (image_desc.array_size != 0) {
            store_ok = false; // sotc: SOTC_TEX_BLOCK: descriptor arrays are not stored
            // dynamic_tsharp_array_size (from Pink-shadPS4): one binding per table element, then a
            // null one for out-of-range indices. The shader was typed after `tsharp`, the first
            // valid element, so elements of another view type or number class stay null.
            const u32 num_bindings = image_desc.NumBindings(stage);
            const auto view_type = tsharp.GetViewType(image_desc.is_array);
            const bool is_integer = AmdGpu::IsInteger(tsharp.GetNumberFmt());
            for (u32 i = 0; i < num_bindings; ++i) {
                const auto element = i < image_desc.array_size
                                         ? image_desc.GetElementSharp(stage, i)
                                         : AmdGpu::Image::Null(image_desc.is_depth);
                const auto element_dfmt = element.GetDataFmt();
                const auto element_nfmt = element.GetNumberFmt();
                const bool bindable =
                    element && element_dfmt != AmdGpu::DataFormat::FormatInvalid &&
                    magic_enum::enum_contains(element_dfmt) &&
                    magic_enum::enum_contains(element_nfmt) &&
                    memory->IsValidGpuMapping(element.Address(), 0) &&
                    element.GetViewType(image_desc.is_array) == view_type &&
                    AmdGpu::IsInteger(element_nfmt) == is_integer &&
                    element.GetNumberConversion() == tsharp.GetNumberConversion();
                if (!bindable) {
                    // sotc: SOTC_BIND_CACHE_FRAME: a guest map can make the element bindable
                    bind_cache_map_dep_rec |= bind_cache_recording;
                    image_bindings.emplace_back(std::piecewise_construct, std::tuple{},
                                                std::tuple{});
                    continue;
                }
                if (element.lod_hw_cnt_en) {
                    SotcLod::Mark(element.counter_bank_id);
                    if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE repeats it on a hit
                        bind_cache_lod_rec.push_back(element.counter_bank_id);
                    }
                }
                auto& [image_id, desc] = image_bindings.emplace_back(
                    std::piecewise_construct, std::tuple{}, std::tuple{element, image_desc});
                image_id = texture_cache.FindImage(desc);
                if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                    BindCacheNoteLookup(desc, image_id); // sotc: SOTC_BIND_CACHE_FRAME
                }
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                image->binding.is_bound = 1u;
            }
            image_descriptor_array_sizes.push_back(num_bindings);
            continue;
        }
        if (tsharp.lod_hw_cnt_en) {
            SotcLod::Mark(tsharp.counter_bank_id); // GET_LOD_STATS (sotc_lod_stats.h)
            if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE repeats it on a hit
                bind_cache_lod_rec.push_back(tsharp.counter_bank_id);
            }
            if (rec) { // sotc: SOTC_TEX_BLOCK repeats the mark on a hit
                if (rec->num_lods < MaxBlockImages) {
                    rec->lod_banks[rec->num_lods++] = tsharp.counter_bank_id;
                } else {
                    store_ok = false;
                }
            }
        }
        // sotc: with SOTC_TRACE on: mip fields of the large streamed textures (first 120
        // distinct address/field combinations): which mips the game says are resident.
        static const bool tsharp_trace = SotcTrace::kOn; // sotc: SOTC_TSHARP_TRACE is part of SOTC_TRACE
        if (tsharp_trace && tsharp.width + 1 >= 1024 && tsharp.last_level > 0) {
            static std::mutex m;
            static std::set<std::tuple<u64, u32, u32, u32, u32>> seen;
            std::scoped_lock lk{m};
            const auto key = std::make_tuple(u64{tsharp.Address()}, u32(tsharp.base_level),
                                             u32(tsharp.last_level), u32(tsharp.min_lod),
                                             u32(tsharp.min_lod_warn));
            if (seen.size() < 120 && seen.insert(key).second) {
                std::fprintf(stderr,
                             "SOTC tsharp %#llx %ux%u fmt %u base %u last %u min_lod %u (%.2f) "
                             "warn %u hwcnt %u bank %u\n",
                             static_cast<unsigned long long>(tsharp.Address()), tsharp.width + 1,
                             tsharp.height + 1, u32(tsharp.GetDataFmt()), u32(tsharp.base_level),
                             u32(tsharp.last_level), u32(tsharp.min_lod), tsharp.min_lod / 256.f,
                             u32(tsharp.min_lod_warn), u32(tsharp.lod_hw_cnt_en),
                             u32(tsharp.counter_bank_id));
            }
        }
        // sotc: SOTC_POOL_WATCH: the T#s bound at an address whose pages the game freed while the
        // texture was in use (TextureCache::PoolWatch), each distinct mip field set once.
        if (const u32 watched = SotcStats::pool_watch_count.load(std::memory_order_relaxed)) {
            const u64 addr = tsharp.Address();
            for (u32 k = 0; k < std::min<u32>(watched, 64); ++k) {
                if (SotcStats::pool_watch_addr[k].load(std::memory_order_relaxed) != addr) {
                    continue;
                }
                static std::mutex m;
                static std::set<std::tuple<u64, u32, u32, u32>> seen;
                std::scoped_lock lk{m};
                if (seen.size() < 400 &&
                    seen.insert({addr, u32(tsharp.base_level), u32(tsharp.last_level),
                                 u32(tsharp.min_lod)}).second) {
                    std::fprintf(stderr,
                                 "SOTC POOLWATCH tsharp %#llx %ux%u base %u last %u min_lod %.2f "
                                 "bank %u hwcnt %u\n",
                                 static_cast<unsigned long long>(addr), tsharp.width + 1,
                                 tsharp.height + 1, u32(tsharp.base_level), u32(tsharp.last_level),
                                 tsharp.min_lod / 256.f, u32(tsharp.counter_bank_id),
                                 u32(tsharp.lod_hw_cnt_en));
                }
                break;
            }
        }
        // sotc: first few only (per-draw warning; see BufferCache::SynchronizeMemoryFromImage).
        if (static u32 shown = 0; shown < 4 && texture_cache.IsMeta(tsharp.Address())) {
            ++shown;
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (pi ? pi->verdict == 2
               : (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid)) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            if (rec && image_bindings.size() <= MaxBlockEntries) { // sotc: a null binding
                rec->entries[image_bindings.size() - 1] = TexBlockEntry{.null = true};
            }
            continue;
        }

        if (pi ? pi->verdict == 3
               : (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
                  !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt))) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            // sotc: SOTC_BIND_CACHE_FRAME: a guest map can make the T# valid
            bind_cache_map_dep_rec |= bind_cache_recording;
            store_ok = false; // sotc: SOTC_TEX_BLOCK: the memory map decided this one
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = pi ? pi->num_bindings : image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            // sotc: SOTC_GEN_TEX (texture_cache FindTextureIdentity): a sampled texture whose T# words,
            // binding bits, image and guest range generation are as at its last clean bind skips the
            // FindImage lookup, the overlap scan and the refresh.
            static_assert(sizeof(tsharp) == 4 * sizeof(u64));
            VideoCore::TextureCache::TexIdentityKey tkey{};
            std::memcpy(tkey.words.data(), &tsharp, sizeof(tsharp));
            tkey.extra = u64{image_desc.is_depth} | u64{image_desc.is_atomic} << 1 |
                         u64{image_desc.is_array} << 2 | u64{image_desc.is_r128} << 3 |
                         u64{image_desc.constant_mip_index} << 8 |
                         u64(image_desc.mip_fallback_mode) << 16 | u64(image_desc.post_op) << 24;
            // sotc: SOTC_TSLOT: a plain sampled T# (one binding, no mip fallback, not written, no
            // recording of the texture block or the bind cache) whose identity memo entry holds takes
            // its image and view info from the memo, and builds no ImageDesc. Mode 2 takes the full
            // path below too and compares the two answers (the full path's answer is used).
            const bool plain = num_bindings == 1 && !image_desc.is_written &&
                               mip_fallback_mode == Shader::MipStorageFallbackMode::None && !rec &&
                               !bind_cache_recording;
            // sotc: SOTC_TSLOT: a mip-tail candidate is never probed in mode 1 (TailCandidateOf); mode 2
            // probes it, to check that it never hits. A bind that is not plain is not probed; counted.
            const bool tail_candidate = plain && TailCandidateOf(tsharp, image_desc.is_depth);
            VideoCore::TextureCache::TexSkip skip{};
            bool skipped = false;
            if (tslot_mode != 0) {
                using NotPlain = VideoCore::TsharpSlotCache::TexNotPlain;
                if (!plain) {
                    VideoCore::TsharpSlotCache::NoteTexNotPlain(
                        num_bindings != 1                     ? NotPlain::Multi
                        : image_desc.is_written               ? NotPlain::Written
                        : mip_fallback_mode != Shader::MipStorageFallbackMode::None
                            ? NotPlain::MipFallback
                            : NotPlain::Recording);
                } else if (tail_candidate) {
                    VideoCore::TsharpSlotCache::NoteTexNotPlain(NotPlain::Tail);
                }
                if (plain && (!tail_candidate || tslot_mode == 2)) {
                    skipped = texture_cache.ProbeTextureIdentity(tkey, tslot_mode == 2, skip);
                }
            }
            auto& [image_id, desc] = image_bindings.emplace_back();
            if (skipped && tslot_mode == 1) {
                image_id = skip.image_id;
                desc.view_info = skip.view_info;
                lazy_binds.push_back(LazyBind{static_cast<u32>(image_bindings.size() - 1), tsharp,
                                              &image_desc, skip.view_info});
            } else {
                desc = VideoCore::TextureCache::ImageDesc(tsharp, image_desc);
                if (tslot_mode == 2 && plain) { // sotc: SOTC_TSLOT=2: the tail predicate, and no tail hit
                    VideoCore::TsharpSlotCache::NoteTexTail(
                        tail_candidate == (desc.rebase_mip != 0) && !(tail_candidate && skipped));
                }
                if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                    ASSERT(num_bindings == 1);
                    desc.view_info.range.base.level += image_desc.constant_mip_index;
                    desc.view_info.range.extent.levels = 1;
                } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                    desc.view_info.range.base.level += i;
                    desc.view_info.range.extent.levels = 1;
                }
                image_id = desc.type == VideoCore::TextureCache::BindingType::Texture
                               ? texture_cache.FindTextureIdentity(tkey, desc).image_id
                               : texture_cache.FindImage(desc);
                if (skipped) { // sotc: SOTC_TSLOT=2: the skip's answer is compared with this one
                    const bool same =
                        image_id == skip.image_id && desc.view_info == skip.view_info &&
                        *texture_cache.GetImage(image_id).FindView(desc.view_info).image_view ==
                            skip.view;
                    VideoCore::TsharpSlotCache::NoteTexVerify(same);
                }
            }
            // sotc: SOTC_LOAD_WARM: record the plain single-binding sampled textures for the next
            // load of this area (load_warm.h); storage and mip-fallback binds are not replayed.
            if (SotcLoadWarm::TexturesEnabled() && num_bindings == 1 &&
                mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                !image_desc.is_written) {
                SotcTexBlob blob;
                std::memset(&blob, 0, sizeof blob);
                blob.tsharp = tsharp;
                blob.resource = image_desc;
                SotcLoadWarm::NoteTexture(&blob, sizeof blob);
            }
            if (rec) { // sotc: SOTC_TEX_BLOCK: the lookup as TryTextureBlock replays it
                const size_t index = image_bindings.size() - 1;
                if (!texture_cache.IsPerfectFind(image_id, desc.info)) {
                    store_ok = false; // an overlap resolution, not a perfect match
                }
                const VideoCore::Image& found = texture_cache.GetImage(image_id);
                // A mip-tail decision (DecideRebase, MemoLookup) depends on the whole image set and
                // on the GPU-written tails: such a block is checked against both (TryTextureBlock).
                rec->candidate = rec->candidate || found.info.sotc_first_mip != 0 ||
                                 (desc.rebase_mip != 0 && VideoCore::MipTailMode() != 0);
                if (index < MaxBlockEntries) {
                    rec->entries[index] = TexBlockEntry{};
                    rec->entries[index].found = image_id;
                    rec->entries[index].found_uid = found.image_uid;
                    rec->entries[index].addr = found.info.guest_address;
                    rec->entries[index].addr_gen =
                        texture_cache.ImageAddrGeneration(found.info.guest_address);
                } else {
                    store_ok = false;
                }
            }
            if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                BindCacheNoteLookup(desc, image_id); // sotc: SOTC_BIND_CACHE_FRAME
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (size_t entry = 0; entry < image_bindings.size(); ++entry) {
        auto& [image_id, desc] = image_bindings[entry];
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            image_info_images[image_infos.size() - 1] = {};
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                store_ok = false; // sotc: SOTC_TEX_BLOCK: found again, not stored
                // sotc: SOTC_TSLOT: a lazy binding gets the descriptor the first loop did not build,
                // with the view info its memo gave (what the full path's descriptor holds here).
                for (const LazyBind& lazy : lazy_binds) {
                    if (lazy.index == entry) {
                        desc = VideoCore::TextureCache::ImageDesc(lazy.tsharp, *lazy.resource);
                        desc.view_info = lazy.view_info;
                    }
                }
                image_id = texture_cache.FindImage(desc);
                if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                    BindCacheNoteLookup(desc, image_id); // sotc: SOTC_BIND_CACHE_FRAME
                }
            }

            bound_images.emplace_back(image_id);
            if (is_storage && SotcStats::kBindCacheRanges) {
                BindCacheInvalidateImage(image_id); // sotc: drops the binds sampling it
            }

            auto& image = texture_cache.GetImage(image_id);
            // sotc: SOTC_TSLOT: a texture's second pass may skip UpdateImage where it is a no-op.
            const bool second_update = !(tslot_mode != 0 &&
                                         desc.type == VideoCore::TextureCache::BindingType::Texture);
            auto& image_view = texture_cache.FindTexture(image_id, desc, second_update);
            const auto binding = image.binding;

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE: feedback loop / storage
                store_ok = false;              // sotc: SOTC_TEX_BLOCK: a target binding
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    // sotc: the depth buffer the draw writes stencil to is also sampled (depth
                    // aspect only): DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL allows both, so the
                    // image stays in the layout the render target needs. Moving it to
                    // DEPTH_STENCIL_READ_ONLY here and back in BeginRendering ended the render
                    // pass at every such draw (~350 per frame in the Shrine). SOTC_DEPTH_RO=0 off.
                    if (DepthSampledAsAttachment(image_id, image, desc,
                                                 stage.sw_stage == Shader::SwStage::Compute)) {
                        bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE: register-dependent
                        needs_barrier |= runtime.Transit(
                            &image, vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
                            kDepthReadOnlyStencilStages, kDepthReadOnlyStencilAccess,
                            desc.view_info.range);
                    } else {
                        const auto new_layout = image.info.props.is_depth
                                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
                        needs_barrier |= runtime.Transit(
                            &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                            vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                        if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE replays it on a hit
                            BindCacheNoteImage(image_id, new_layout,
                                               vk::PipelineStageFlagBits2::eAllCommands,
                                               vk::AccessFlagBits2::eShaderRead,
                                               desc.view_info.range);
                        }
                    }
                }
            }
            // sotc: SOTC_TEX_BLOCK: the bound image, its view and the backing state the bind left.
            if (image.info.props.has_stencil) {
                store_ok = false; // its transition depends on the draw's depth registers
            }
            if (rec && entry < MaxBlockEntries) {
                TexBlockEntry& e = rec->entries[entry];
                e.bound = image_id;
                e.bound_uid = image.image_uid;
                e.redirect = image_id != e.found;
                e.range = desc.view_info.range;
                e.view = *image_view.image_view;
                e.backing = image.backing;
                e.version = image.backing->state_version;
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            // sotc: SOTC_LAYOUT_UNIFIED: the descriptor names the layout the image really is in.
            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     SotcArch::Layouts::Pick(image.backing->state.layout));
            image_info_images[image_infos.size() - 1] = image_id;
        }
    }

    // sotc: the composite predicates (CompositeOf: TakeHudless, DLSS, the probe), the same ones BindHeapImages asks.
    // (Only a fragment stage can be one: the ids are not gathered for the others.)
    CompositeKinds composite{};
    if (stage.sw_stage == Shader::SwStage::Fragment) {
        boost::container::small_vector<VideoCore::ImageId, 8> bound_ids;
        for (const auto& [id, desc] : image_bindings) {
            bound_ids.push_back(id);
        }
        composite = CompositeOf(stage, bound_ids.data(), static_cast<u32>(bound_ids.size()), cb_descs[0].first);
    }
    // sotc: TakeHudless. Off unless SOTC_FG_HUDLESS=1 (see CompositeOf): the UI texture of the composite is
    // put back over the generated frames.
    if (composite.hudless) {
        hudless_ui_info = static_cast<s32>(first_image_idx + composite.ui_index);
    }

    // sotc: SOTC_DLSS_PROBE=1 logs each fragment shader that draws into a video out surface once:
    // its images (format, size, render target or not), to find the final composite.
    static const bool dlss_probe = std::getenv("SOTC_DLSS_PROBE") != nullptr;
    if (composite.probe) {
        static std::unordered_set<std::string> seen;
        std::string line = fmt::format("{:#x}", stage.pgm_hash);
        {
            for (const auto& [id, desc] : image_bindings) {
                if (!id) {
                    line += " [none]";
                    continue;
                }
                const auto& img = texture_cache.GetImage(id);
                line += fmt::format(" [{} {}x{}{}]", vk::to_string(img.info.pixel_format),
                                    img.info.size.width, img.info.size.height,
                                    img.usage.render_target ? " rt" : "");
            }
            const auto& out = texture_cache.GetImage(cb_descs[0].first);
            if (seen.size() < 60 && seen.insert(line).second)
            std::fprintf(stderr, "SOTC DLSS PROBE: shader %#llx -> %s %ux%u, %zu images:%s\n",
                         static_cast<unsigned long long>(stage.pgm_hash),
                         vk::to_string(out.info.pixel_format).c_str(), out.info.size.width,
                         out.info.size.height, image_bindings.size(), line.c_str());
        }
    }

    // sotc: DLSS super resolution: the game's final composite (CompositeOf) hands its two images to the upscaler.
    if (composite.dlss) {
        const u32 ui_i = composite.ui_index;
        bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE
        Dlssg::NoteComposite(image_bindings[ui_i ^ 1].first, image_bindings[ui_i].first, texture_cache);
        NoteUiShadow();
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            std::fprintf(stderr, "SOTC DLSS: composite shader %#llx\n",
                         static_cast<unsigned long long>(stage.pgm_hash));
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    u32 stage_image = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [image_id, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        const u32 image_index = stage_image++;
        // sotc: SOTC_N5_BINDLESS: a sampled or storage image of the heap takes no descriptor and no
        // binding number (Info::AddBindings skips it). Its push slot gets the heap index of its view.
        // A stored bind replays the slot while the heap generation is unchanged (BindlessRef).
        if (bindless_images && array_size == 1) {
            const u32 heap_slot = Shader::BindlessImageSlotOf(stage, image_index);
            if (heap_slot != Shader::NO_BINDLESS_IMAGE_SLOT) {
                const u32 key = Shader::BindlessImageKeyAt(stage, image_index);
                const vk::ImageLayout layout = image_infos[image_info_idx].imageLayout;
                // sotc: SOTC_TSLOT: a T# whose cached answer still holds (same image, view info, and
                // the heap holds the view in this layout) takes its slot without the view lookup.
                // The T# is read here, not from the prepared resources: it only picks the entry,
                // the image and view info checked against the bind decide. SOTC_TSLOT=2 runs the old
                // resolution too, compares, and uses its answer.
                VideoCore::BindlessRef ref{};
                VideoCore::TsharpSlotCache* tslot = nullptr;
                AmdGpu::Image sharp{};
                u64 image_uid = 0;
                VideoCore::TsharpSlotCache::Entry* hit = nullptr;
                if (image_id && (tslot = VideoCore::TsharpSlotCache::For(*bindless_images))) {
                    sharp = stage_sharps[image_index];
                    image_uid = texture_cache.GetImage(image_id).image_uid;
                    hit = tslot->Find(sharp, key, image_id, image_uid, desc.view_info, layout);
                }
                const bool verify = hit && VideoCore::TsharpSlotCache::Mode() == 2;
                if (hit && !verify) {
                    ref = {key, hit->slot, hit->view};
                    bindless_images->Use(key, hit->slot);
                    if (bindless_images->Mode() == 2) {
                        bindless_images->Verify(key, hit->slot, hit->view);
                    }
                } else {
                    ref = BindlessImageIndex(*bindless_images, texture_cache, image_id, desc, key,
                                             layout);
                    if (tslot) {
                        if (verify) {
                            tslot->NoteVerify(hit->slot == ref.slot && hit->view == ref.view);
                        }
                        if (ref.slot != 0) {
                            tslot->Store(sharp, key, image_id, image_uid, desc.view_info, ref);
                        }
                    }
                }
                if (slot_words_fill) { // sotc: a pipeline without slot words (SOTC_PACKET) never pushes them
                    const u32 shift = (heap_slot & 1) * 16;
                    auto& word = bindless_slots.image_words[heap_slot / 2];
                    word = (word & ~(0xFFFFu << shift)) | (ref.slot << shift);
                }
                if (ref.slot != 0) {
                    bind_bindless_rec.push_back(ref);
                }
                if (rec) { // sotc: SOTC_TEX_BLOCK: the block's heap slot of this descriptor
                    if (image_index < MaxBlockImages) {
                        rec->bindless_push[image_index] = static_cast<u8>(heap_slot + 1);
                        rec->bindless_block[image_index] = ref;
                    } else {
                        store_ok = false;
                    }
                }
                image_info_idx += array_size;
                image_binding_idx += array_size;
                continue;
            }
        }
        set_write_sets[set_write_index] = set;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    const VideoCore::Image* ui_asset = UiLinearAsset(
        stage, image_bindings.size(),
        image_bindings.size() == 1 ? image_bindings[0].first : VideoCore::ImageId{});
    // sotc: SOTC_BIND_CACHE: the composite (TakeHudless, DLSS), the probe and the bilinear UI
    // filter depend on more than the cache key; those binds are not stored.
    if (ui_asset || hudless_ui_info >= 0 || dlss_probe) {
        bind_cache_uncacheable = true;
    }
    BindSamplers(stage, binding, set, ui_asset);

    // sotc: SOTC_TEX_BLOCK: store the block when TryTextureBlock can answer it (TexBlockMemo).
    if (rec) {
        const size_t num_entries = image_bindings.size();
        bool ok = store_ok && num_entries <= MaxBlockEntries &&
                  stage.images.size() <= MaxBlockImages &&
                  image_descriptor_array_sizes.size() == stage.images.size();
        if (ok && rec->candidate) {
            ok = texture_cache.BindGeneration() == rec->bind_generation &&
                 VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed) == rec->gpu_epoch;
        }
        for (size_t i = 0; ok && i < num_entries; ++i) {
            const TexBlockEntry& e = rec->entries[i];
            if (e.null) {
                continue;
            }
            VideoCore::Image* found = texture_cache.TryGetImage(e.found, e.found_uid);
            ok = found && texture_cache.TryGetImage(e.bound, e.bound_uid) &&
                 texture_cache.ImageAddrGeneration(e.addr) == e.addr_gen &&
                 texture_cache.GetAssociatedDepth(*found) ==
                     (e.redirect ? e.bound : VideoCore::ImageId{});
        }
        rec->valid = ok;
        if (ok) {
            rec->num_entries = static_cast<u32>(num_entries);
            rec->num_descs = static_cast<u32>(stage.images.size());
            for (size_t d = 0; d < stage.images.size(); ++d) {
                rec->desc_counts[d] = static_cast<u8>(image_descriptor_array_sizes[d]);
            }
            tex_block_stores.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

const VideoCore::Image* Rasterizer::UiLinearAsset(const Shader::Info& stage, size_t num_bindings,
                                                  VideoCore::ImageId first) {
    // sotc: at a render size above 1080 lines the game stretches its 1080p 2D art (splash logos,
    // UI) itself, with the point sampler meant for 1:1 drawing: hard steps. A draw that samples
    // one asset texture (never a render target, storage or depth image) magnifies bilinearly
    // there. It is on whenever SOTC_RENDER_RES names another size than 1080p (set by the launcher),
    // SOTC_UI_LINEAR_TRACE=1 lists the draws it changes.
    // sotc: the decision is SotcArch::UiLinear's, shared with the heap resolve (SOTC_HEAP_UI_LINEAR).
    if (num_bindings != 1 || !first || !SotcArch::UiLinear::StageEligible(stage)) {
        return nullptr;
    }
    const auto& image = texture_cache.GetImage(first);
    return SotcArch::UiLinear::ImageEligible(image) ? &image : nullptr;
}

void Rasterizer::BindSamplers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                              u32 set, const VideoCore::Image* ui_asset) {
    const bool bindless = IsBindlessStage(instance, stage);
    const auto bindless_range = Shader::GetBindlessSamplerRange(stage.sw_stage);
    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        auto ssharp = sampler.GetSharp(stage);
        if (ui_asset && ssharp.xy_mag_filter.Value() == AmdGpu::Filter::Point &&
            !sampler.is_depth) {
            ssharp.xy_mag_filter.Assign(AmdGpu::Filter::Bilinear);
            static const bool trace = SotcTrace::kOn; // sotc: SOTC_UI_LINEAR_TRACE is part of SOTC_TRACE
            if (trace) {
                static std::mutex m;
                static std::set<u64> seen;
                std::scoped_lock lk{m};
                if (seen.size() < 200 && seen.insert(stage.pgm_hash ^ ui_asset->info.guest_address).second) {
                    std::fprintf(stderr, "SOTC ui-linear pgm %#llx tex %#llx %ux%u\n",
                                 static_cast<unsigned long long>(stage.pgm_hash),
                                 static_cast<unsigned long long>(ui_asset->info.guest_address),
                                 ui_asset->info.size.width, ui_asset->info.size.height);
                }
            }
        }
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                liverpool->DrawRegs().ta_bc_base.Address() == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        liverpool->DrawRegs().ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        u32 heap_slot = 0;
        const auto vk_sampler = texture_cache.GetSampler(
            ssharp, liverpool->DrawRegs().ta_bc_base, sampler.is_depth, &heap_slot);
        if (bind_cache_recording && SotcStats::kBindCacheXtick) {
            // sotc: SOTC_BIND_CACHE_XTICK: touched again by the first hit of each later tick
            bind_cache_samplers_rec.push_back(
                {ssharp, vk_sampler, heap_slot, sampler.is_depth, bindless});
        }
        if (bindless) {
            // sotc: the shader reads the heap index from this slot of the push constants.
            if (slot_words_fill) { // sotc: a pipeline without slot words (SOTC_PACKET) never pushes them
                const u32 slot = bindless_range.base + sampler_index;
                const u32 shift = (slot & 1) * 16;
                auto& word = bindless_slots.words[slot / 2];
                word = (word & ~(0xFFFFu << shift)) | (heap_slot << shift);
            }
            continue;
        }
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        image_info_images[image_infos.size() - 1] = {};
        set_write_sets[set_write_index] = set;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    const SotcOpPhases::Scope ph{SotcOpPhases::PhBeginRendering}; // sotc: SOTC_OP_PHASES
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the attachments
    attachment_feedback_loop = false;
    // sotc: SOTC_RT_FAST (default on) drops redundant attachment work: FindRenderTarget repeats
    // UpdateImage only when the image still needs it, and the CMASK/HTILE clear bit is read and
    // cleared with one lookup. SOTC_RT_FAST=0 restores the original calls and evaluation order.
    static const bool rt_fast = [] {
        const char* v = std::getenv("SOTC_RT_FAST");
        return !v || v[0] != '0';
    }();
    const auto& regs = liverpool->DrawRegs();
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    ui_shadow_hit = false; // sotc: BeginUiShadow
    rs_verify_hit = RsMemoHit::Miss; // sotc: SOTC_RS_MEMO=3
    if (rs_memo_prepared) { // sotc: SOTC_RS_MEMO
        rs_memo_prepared = false;
        if (RsMemoBegin(state)) {
            return state;
        }
    } else if (rs_memo_verify) { // sotc: SOTC_RS_MEMO=3, the full path runs below
        rs_memo_verify = false;
        rs_verify_hit = RsMemoBeginCheck();
        rs_verify_pred = {};
        if (rs_verify_hit == RsMemoHit::Cross) {
            // sotc: the transitions a cross replay would skip, before the full path runs them.
            const auto note = [&](u32 slot, VideoCore::ImageId id, vk::ImageLayout layout,
                                  vk::AccessFlags2 access, const VideoCore::SubresourceRange& range) {
                if (!id) {
                    return;
                }
                const auto& image = texture_cache.GetImage(id);
                auto& pred = rs_verify_pred[slot];
                pred.noop = RsMemoTransitIsNoop(image, layout, access, range);
                pred.layout = image.backing->state.layout;
                pred.access = image.backing->state.access_mask;
            };
            for (u32 cb = 0; cb < rs_memo.state.num_color_attachments; ++cb) {
                note(cb, cb_descs[cb].first, vk::ImageLayout::eColorAttachmentOptimal,
                     vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eColorAttachmentRead,
                     cb_descs[cb].second.view_info.range);
            }
            note(AmdGpu::NUM_COLOR_BUFFERS, db_desc.first, rs_memo.depth_layout,
                 rs_memo.depth_access, db_desc.second.view_info.range);
        }
    }
    bool rs_cb0_base = false;
    vk::ImageLayout rs_depth_layout{};
    vk::PipelineStageFlags2 rs_depth_stages{};
    vk::AccessFlags2 rs_depth_access{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    // Same count as the pipeline's rasterizationSamples (vk_graphics_pipeline.cpp).
    state.samples = LiverpoolToVK::NumSamples(
        key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts());
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc, !rt_fast);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        // sotc: SOTC_RT_FAST: test-and-clear the CMASK bit with one lookup.
        bool is_clear;
        if (rt_fast) {
            is_clear = texture_cache.TestAndTouchMeta(col_buf.CmaskAddress(), slice);
        } else {
            is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
            texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
        }

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
            // sotc: A1, without VK_EXT_attachment_feedback_loop_layout the feedback is declared at BeginRendering.
            if (!instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                state.feedback_mask |= 1u << cb;
            }
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = SotcArch::Layouts::Pick(image->backing->state.layout); // sotc: A1
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;
        state.color_formats[cb] = image_view.info.format;

        image->usage.render_target = 1u;
        if (cb == 0 && image->usage.vo_surface) { // sotc: TakeHudless
            hudless_vo = image->info.guest_address;
            hudless_vo_format = image_view.info.format;
            hudless_vo_width = image->info.size.width;
            hudless_vo_height = image->info.size.height;
        }
        rs_cb0_base |= cb == 0 && mip == 0 && slice == 0; // sotc: SOTC_RS_MEMO
        if (cb == 0 && mip == 0 && slice == 0 && // sotc: BeginUiShadow
            Dlssg::UiTarget(image->info.guest_address, image->info.size.width,
                            image->info.size.height, ui_shadow_width, ui_shadow_height)) {
            ui_shadow_hit = true;
            ui_shadow_format = image_view.info.format;
            ui_shadow_src_width = image->info.size.width;
            ui_shadow_src_height = image->info.size.height;
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool depth_reg_clear = regs.depth_render_control.depth_clear_enable &&
                                     regs.depth_control.depth_enable &&
                                     regs.depth_control.depth_write_enable;
        // sotc: SOTC_RT_FAST: test-and-clear the HTILE bit with one lookup. It is evaluated
        // unconditionally (no short-circuit) because the original TouchMeta always cleared it.
        bool is_depth_clear;
        if (rt_fast) {
            const bool meta_clear = texture_cache.TestAndTouchMeta(htile_address, slice);
            is_depth_clear = depth_reg_clear || meta_clear;
        } else {
            is_depth_clear = depth_reg_clear || texture_cache.IsMetaCleared(htile_address, slice);
        }
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        if (!rt_fast) {
            texture_cache.TouchMeta(htile_address, slice, false);
        }
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);
        // sotc: A1, a depth attachment also sampled by this draw is a feedback attachment (see the color branch).
        if (image.binding.is_bound && !instance.IsAttachmentFeedbackLoopLayoutSupported()) {
            state.feedback_mask |= 1u << 8;
        }

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        static const bool depth_ro = [] {
            const char* v = std::getenv("SOTC_DEPTH_RO");
            return !v || v[0] != '0';
        }();
        // sotc: the stages and access are kept for SOTC_RS_MEMO (same Transit as before).
        const bool depth_ro_layout =
            depth_ro && new_layout == vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
        rs_depth_layout = new_layout;
        rs_depth_stages = depth_ro_layout ? kDepthReadOnlyStencilStages
                                          : vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                                vk::PipelineStageFlagBits2::eLateFragmentTests;
        rs_depth_access = depth_ro_layout ? kDepthReadOnlyStencilAccess
                                          : vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                                vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        needs_barrier |= runtime.Transit(&image, new_layout, rs_depth_stages, rs_depth_access,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = SotcArch::Layouts::Pick(image.backing->state.layout); // sotc: A1
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }
        // Same aspects as the Scheduler's pDepthAttachment / pStencilAttachment.
        state.depth_format = attachment.has_depth ? image_view.info.format : vk::Format{};
        state.stencil_format = attachment.has_stencil ? image_view.info.format : vk::Format{};

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    if (rs_verify_hit != RsMemoHit::Miss) { // sotc: SOTC_RS_MEMO=3, before the memo is replaced
        RsMemoVerify(state, rs_verify_hit);
        rs_verify_hit = RsMemoHit::Miss;
    }
    if (RsMemoEnabled()) { // sotc: SOTC_RS_MEMO: kept unless the pass clears or loops back
        bool storable = !attachment_feedback_loop;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            storable &= !state.color_attachments[cb].is_clear;
        }
        const auto& ds = state.depth_stencil_attachment;
        storable &= !ds.depth_clear && !ds.stencil_clear;
        rs_memo.valid = storable;
        if (storable) {
            rs_memo.key_len = rs_key_len;
            std::memcpy(rs_memo.key.data(), rs_key.data(), rs_key_len * sizeof(u32));
            rs_memo.cb0_base = rs_cb0_base;
            rs_memo.color_samples = key.color_samples;
            rs_memo.tick = scheduler.CurrentTick();
            rs_memo.texture_generation = texture_cache.BindGeneration();
            rs_memo.meta_epoch = texture_cache.MetaEpoch();
            rs_memo.state = state;
            rs_memo.depth_layout = rs_depth_layout;
            rs_memo.depth_stages = rs_depth_stages;
            rs_memo.depth_access = rs_depth_access;
            // sotc: SOTC_RS_MEMO=2, the images of this draw's targets (id, uid, backing).
            for (u32 slot = 0; slot < rs_memo.target_id.size(); ++slot) {
                const bool is_depth = slot == AmdGpu::NUM_COLOR_BUFFERS;
                const auto id = is_depth ? db_desc.first
                                : slot < state.num_color_attachments ? cb_descs[slot].first
                                                                     : VideoCore::ImageId{};
                rs_memo.target_id[slot] = id;
                rs_memo.target_uid[slot] = 0;
                rs_memo.target_backing[slot] = nullptr;
                if (id) {
                    const auto& image = texture_cache.GetImage(id);
                    rs_memo.target_uid[slot] = image.image_uid;
                    rs_memo.target_backing[slot] = image.backing;
                }
            }
        } else if (SotcStats::kFpsLog) {
            SotcStats::rs_memo_unstored.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->DrawCbExtent()[0];
    const auto& mrt1_hint = liverpool->DrawCbExtent()[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->DrawRegs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->DrawRegs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    const auto mrt1_id = texture_cache.FindImage(mrt1_desc, true);
    auto& mrt1_image = texture_cache.GetImage(mrt1_id);

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->DrawRegs().color_buffers[0].Address(),
                                 liverpool->DrawRegs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
    if (SotcStats::kBindCacheRanges) { // sotc: drops the binds reading the written image
        BindCacheInvalidateImage(mrt1_id);
    } else if (SotcStats::kBindCache) { // sotc: a GPU image write a cached texel buffer may alias
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->DrawRegs();
    // sotc: a GPU image write a cached texel buffer may alias (SOTC_BIND_CACHE_RANGES: the
    // written image is dropped below instead).
    if (SotcStats::kBindCache && !SotcStats::kBindCacheRanges) {
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    const auto write_id = texture_cache.FindImage(write_desc);
    auto& write_image = texture_cache.GetImage(write_id);
    BindCacheInvalidateImage(write_id); // sotc: SOTC_BIND_CACHE_RANGES (no-op otherwise)

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->DrawRegs().depth_view.slice_start;
    sub_range.extent.layers = liverpool->DrawRegs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (!is_gds) {
        NoteGuestWrite(address, num_bytes); // sotc: SOTC_SHADER_ADDR ordering
    }
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            // sotc: SOTC_NATIVE=1: this host write reaches no tracker (no page is protected), so the GPU
            // copy of the range and the images over it learn it here, as the write fault did in mode 0.
            if (!SotcNative::TracksGuestPages()) {
                InvalidateMemory(address, num_bytes, true);
                SotcN1Verify::NoteAnnounce(address, num_bytes, SotcN1Verify::kAnnDirect, reinterpret_cast<const void*>(1)); // sotc: SOTC_VERIFY=n1
            }
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    if (!is_gds) {
        texture_cache.InvalidateMemoryFromGPU(address, num_bytes, true);
    }
    if (SotcStats::kBindCacheRanges) {
        // sotc: drops the binds reading the range. GDS is not guest memory, and a hit replays
        // the GDS buffer's access check (barrier) as the full bind does.
        if (!is_gds) {
            BindCacheInvalidate(address, num_bytes);
        }
    } else if (SotcStats::kBindCache) { // sotc: a GPU write: no cached bind survives it
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    runtime.FillBuffer(buffer, offset, num_bytes, value);
    if (!is_gds && SotcArch::ShaderAddr::On()) { // sotc: SOTC_SHADER_ADDR, mirror coherence (SotcMirrorFromBuffer)
        SotcMirrorFromBuffer(*buffer, offset, address, num_bytes);
    }
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds) {
        NoteGuestWrite(dst, num_bytes); // sotc: SOTC_SHADER_ADDR ordering
    }
    // sotc: SOTC_NATIVE_UNIFIED: a unified range is written by the GPU in guest memory, possibly by work not run yet:
    // the copy stays on the GPU, in order.
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes) &&
        !buffer_cache.IsNativeUnified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !buffer_cache.IsNativeUnified(src, num_bytes) && !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            if (!SotcNative::TracksGuestPages()) { // sotc: SOTC_NATIVE=1, see FillBuffer
                InvalidateMemory(dst, num_bytes, true);
                SotcN1Verify::NoteAnnounce(dst, num_bytes, SotcN1Verify::kAnnDirect, reinterpret_cast<const void*>(2)); // sotc: SOTC_VERIFY=n1
            }
            SotcStats::dma_cpu_copies.fetch_add(1, std::memory_order_relaxed);
            SotcStats::dma_cpu_bytes.fetch_add(num_bytes, std::memory_order_relaxed);
            return;
        }
    }
    if (!dst_gds) {
        texture_cache.InvalidateMemoryFromGPU(dst, num_bytes, true);
    }
    if (SotcStats::kBindCacheRanges) {
        if (!dst_gds) { // sotc: drops the binds reading the destination (GDS: see FillBuffer)
            BindCacheInvalidate(dst, num_bytes);
        }
    } else if (SotcStats::kBindCache) { // sotc: a GPU write: no cached bind survives it
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    SotcStats::dma_gpu_copies.fetch_add(1, std::memory_order_relaxed);
    SotcStats::dma_gpu_bytes.fetch_add(num_bytes, std::memory_order_relaxed);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
    if (!dst_gds && SotcArch::ShaderAddr::On()) { // sotc: SOTC_SHADER_ADDR, mirror coherence (SotcMirrorFromBuffer)
        SotcMirrorFromBuffer(*dst_buffer, dst_offset, dst, num_bytes);
    }
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    // sotc: SOTC_NATIVE=1: images the GPU wrote stay (TextureCache::InvalidateMemory keep_gpu_written).
    texture_cache.InvalidateMemory(addr, size, !SotcNative::TracksGuestPages());
    return true;
}

// sotc: SOTC_NATIVE=1, see vk_rasterizer.h. The texture cache part is the announce rule (NativeDrain):
// images the GPU wrote are kept.
void Rasterizer::NoteHostWrite(VAddr addr, u64 size, bool allow_exact) {
    if (SotcNative::TracksGuestPages() || size == 0 || !IsMapped(addr, size)) {
        return;
    }
    buffer_cache.NativeHostWrite(addr, size, allow_exact);
    texture_cache.InvalidateMemory(addr, size, true);
}

// sotc: a guest (not command processor) write fault. With the repeat-fault grace on, the texture
// cache goes first so the page becomes writable last, after any wait on the texture mutex (the GPU
// thread then has less time to protect it again before the guest retries its write).
bool Rasterizer::InvalidateMemoryOnGuestFault(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        return false;
    }
    // sotc: the texture cache part is timed as the fault stage "tc" (SotcStats::FaultStat).
    if (VideoCore::FaultGrace::Pages() != 0) {
        {
            const SotcStats::FaultStat::StageScope tc{SotcStats::FaultStat::kStTc};
            // sotc: SOTC_TC_SINGLE_WRITER: queued for the draw thread (InvalidateMemoryGuest).
            texture_cache.InvalidateMemoryGuest(addr, size);
        }
        buffer_cache.InvalidateMemory(addr, size, false, true);
    } else {
        buffer_cache.InvalidateMemory(addr, size, false);
        {
            const SotcStats::FaultStat::StageScope tc{SotcStats::FaultStat::kStTc};
            texture_cache.InvalidateMemoryGuest(addr, size);
        }
    }
    return true;
}

namespace {
// sotc: SOTC_RB_CHANGED=1, diagnostic: the readbacks that changed the guest bytes (a real GPU result reached the
// CPU), per guest instruction of the read fault, every 5 s. A readback that copies the bytes the guest already holds
// only came from page sharing. Compare two sessions to find the consumer one of them needs.
struct RbChanged {
    std::mutex mu;
    std::unordered_map<u64, std::pair<u64, u64>> rows; // eboot offset -> (changed readbacks, readbacks)
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

bool RbChangedOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_RB_CHANGED");
        return v && v[0] == '1';
    }();
    return on;
}

void NoteRbChanged(u64 rip, bool changed) {
    static RbChanged t;
    const u64 base = SotcTrace::ImageBase();
    const u64 offset = (base && rip >= base && rip - base < SotcTrace::ImageSize()) ? rip - base : ~u64{0};
    std::scoped_lock lock{t.mu};
    auto& row = t.rows[offset];
    row.first += changed ? 1 : 0;
    row.second += 1;
    const auto now = std::chrono::steady_clock::now();
    if (now - t.last < std::chrono::seconds(5)) {
        return;
    }
    t.last = now;
    std::vector<std::pair<u64, std::pair<u64, u64>>> rows(t.rows.begin(), t.rows.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    std::string text;
    for (size_t i = 0; i < rows.size() && i < 20 && rows[i].second.first > 0; ++i) {
        char item[64];
        if (rows[i].first == ~u64{0}) {
            std::snprintf(item, sizeof item, " ?:%llu/%llu", (unsigned long long)rows[i].second.first,
                          (unsigned long long)rows[i].second.second);
        } else {
            std::snprintf(item, sizeof item, " 0x%llx:%llu/%llu", (unsigned long long)rows[i].first,
                          (unsigned long long)rows[i].second.first, (unsigned long long)rows[i].second.second);
        }
        text += item;
    }
    std::fprintf(stderr, "SOTC RB CHANGED (5 s): changed/all readbacks by eboot offset:%s\n", text.c_str());
    t.rows.clear();
}
} // namespace

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    if (RbChangedOn() && size <= (u64{16} << 20)) {
        const u64 rip = VideoCore::SotcFaultRip::current;
        const unsigned char* backing = SotcGuest::Backing(addr, size);
        std::vector<unsigned char> before;
        if (backing) {
            before.assign(backing, backing + size);
        }
        buffer_cache.ReadMemory(addr, size, false, assume_locks);
        if (backing) {
            const bool changed = std::memcmp(before.data(), backing, size) != 0;
            if (rip != 0) {
                NoteRbChanged(rip, changed);
            }
            // sotc: each readback that changed guest bytes, first 80: who read (port thread or guest), where, what.
            if (changed) {
                static std::atomic<u32> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 80) {
                    u64 first = 0, diffs = 0;
                    for (u64 i = 0; i < size; ++i) {
                        if (before[i] != backing[i]) {
                            if (diffs++ == 0) {
                                first = i;
                            }
                        }
                    }
                    const u64 base = SotcTrace::ImageBase();
                    std::fprintf(stderr, "SOTC RB CHANGED DETAIL addr %#llx+%#llx %s rip %#llx: %llu bytes differ, first "
                                 "at +%#llx (%02x -> %02x)\n",
                                 static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                                 IsGpuSideThread() ? "port-thread" : "guest-thread",
                                 static_cast<unsigned long long>(rip && base && rip > base ? rip - base : rip),
                                 static_cast<unsigned long long>(diffs), static_cast<unsigned long long>(first),
                                 before[first], backing[first]);
                }
            }
        }
        return true;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

// sotc: PC-native GPU result copies: the dispatch's written buffers go to native_copies.h, which copies the
// ranges registered by its clients (particle counts, XPR masks). bound_buffers holds the
// written binds of a full bind and of a bind-cache hit alike.
void Rasterizer::CopyGpuResults(vk::CommandBuffer cmdbuf) {
    if (!sotc_result_copies || !SotcNativeCopies::Watching()) {
        return;
    }
    for (const auto& bound : bound_buffers) {
        if (bound.is_written && bound.buffer->CpuAddr() != 0) {
            SotcNativeCopies::NoteWrittenBind(cmdbuf, bound.buffer->Handle(), bound.offset,
                                                 bound.buffer->CpuAddr() + bound.offset, bound.size);
        }
    }
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    map_epoch.fetch_add(1, std::memory_order_acq_rel); // sotc: SOTC_PARALLEL_RECORD
    buffer_cache.OnMappingChanged();
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    SotcGen::Bump(addr, size); // sotc: SOTC_GEN_BIND / SOTC_PR_GEN: a mapping change moves the generation
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    map_epoch.fetch_add(1, std::memory_order_acq_rel); // sotc: SOTC_PARALLEL_RECORD
    buffer_cache.OnMappingChanged();
    // sotc: an unmapped range forgets its GPU-written state instead of downloading it (a 512 KB
    // window around it) into memory that is being given back: with texture streaming that
    // readback broke the game's heap within a minute of loading a save (allocator fault at guest
    // 0x1a371, garbage textures). SOTC_UNMAP_READBACK=1 restores the readback.
    static const bool unmap_readback = [] {
        const char* v = std::getenv("SOTC_UNMAP_READBACK");
        return v && v[0] == '1';
    }();
    if (unmap_readback) {
        buffer_cache.InvalidateMemory(addr, size);
    } else {
        buffer_cache.DropGpuModified(addr, size);
    }
    // Give the arena memory back; it runs on the GPU thread, ordered before later draws.
    // sotc: on by default since 2026-10-16 (SOTC_ARENA_RELEASE=0: never released). It was
    // opt-in because it raced with in-flight GPU work during loads (device lost loading a save,
    // seen before ReleaseMemory finished the GPU work first). Never releasing grew video memory
    // ~90 MB a minute in the attract demo (1.6 -> 6.0 GB in 48 minutes on a 6 GB card) until an
    // arena allocation failed (ASSERT). Released, the 25-minute curve stays ~0.7-0.9 GB lower;
    // no device loss in that run nor in three shrine runs that each load a save.
    static const bool release_enabled = [] {
        const char* v = std::getenv("SOTC_ARENA_RELEASE");
        return !v || v[0] != '0';
    }();
    if (release_enabled) {
        // sotc: ordered at the command processor's point of the stream: with the draw thread a
        // guest-thread command would run ahead of the draws already queued, which may still use
        // the range (review of #99).
        liverpool->SendOrderedCommand(
            [this, addr, size] { buffer_cache.ReleaseMemory(addr, size); });
    }
    // sotc: SOTC_TC_SINGLE_WRITER: the texture part runs on the draw thread, at the command
    // processor's point of the stream (as the arena release above); the guest thread takes no lock.
    if (VideoCore::TextureCache::SingleWriter()) {
        liverpool->SendOrderedCommand(
            [this, addr, size] { texture_cache.UnmapMemory(addr, size); });
    } else {
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    SotcGen::Bump(addr, size); // sotc: SOTC_GEN_BIND / SOTC_PR_GEN: a mapping change moves the generation
}

void Rasterizer::PrepareDraw(const AmdGpu::Regs& regs, const Pipeline* resolved, bool is_indexed,
                             bool direct, PreparedDraw& out) const {
    // sotc: SOTC_PARALLEL_RECORD, on a GpuPrep thread: its own registers (replayed up to this op)
    // and its own Shader::g_info_slot, filled from the op like the draw thread's. Only the
    // register-only inputs the draw thread would compute the same way; the decisions stay there.
    out.valid = false;
    out.is_dispatch = false; // sotc: SOTC_GCN_COMMIT (a dispatch's slot is made by PrepareDispatch)
    out.has_offsets = false;
    // sotc: SOTC_PR_KEYS: no table key from an earlier op in this slot (BindResources checks valid).
    for (auto& table_key : out.table_keys) {
        table_key.valid = false;
    }
    if (!resolved || resolved->IsCompute()) {
        return;
    }
    const auto* pipeline = static_cast<const GraphicsPipeline*>(resolved);
    // BindResources drops a draw with a stage without user data (GetDrawOffsets would read it).
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->UserData().data() == nullptr) {
            return;
        }
    }
    bool refuse = false;
    out.dynamic = DynamicValues{}; // the fields a disabled test leaves alone stay zero (verify)
    SotcNoPrepass::Count(regs); // sotc: SOTC_NO_PREPASS=2, prepared path
    SotcPrepassProbe::NoteDraw(regs); // sotc: SOTC_PREPASS_PROBE, prepared path
    ComputeDynamic(regs, is_indexed, out.dynamic, &refuse);
    if (refuse) {
        return;
    }
    out.is_indexed = is_indexed;
    out.push_data = MakeUserData(regs);
    out.fast_static = FastBindStatic(pipeline, regs);
    out.cache_eligible = BindCacheEligible(pipeline, regs);
    out.fast_hash = FastBindHash(pipeline, out.push_data, regs);
    out.res_ok = SotcStats::kBindCacheFrame && out.cache_eligible &&
                 BindResourceHash(pipeline, out.res_hash);
    if (direct) {
        const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto& fetch_shader = pipeline->GetFetchShader();
        const auto user_data = vs_info.UserData();
        const auto in_range = [&](s32 sgpr) {
            return sgpr == -1 || static_cast<u32>(sgpr) < user_data.size();
        };
        if (fetch_shader.Empty() ||
            (in_range(fetch_shader.vertex_offset_sgpr) &&
             in_range(fetch_shader.instance_offset_sgpr))) {
            std::tie(out.vertex_offset, out.instance_offset) =
                GetDrawOffsets(regs, vs_info, fetch_shader);
            out.has_offsets = true;
        }
    }
    out.valid = true;
    if (SotcPrKeysOn() && SotcArch::TableMemo::On() && pipeline->HasPacketRange()) {
        // sotc: SOTC_PR_KEYS: the heap stages' memo keys (TableMemo::Resolve, BindResources), from this
        // thread's flat copy (setup_stages, PrepLoop) and this op's border color base. Only the
        // registers, the stage and the pipeline's immutable profile are read; nothing is shared.
        const Shader::Info* heap[2]{};
        if (PrHeapStages(*pipeline, heap)) {
            for (u32 k = 0; k < 2; ++k) {
                if (heap[k] &&
                    SotcArch::TableMemo::ComputeKey(*heap[k], regs.ta_bc_base, out.table_keys[k]) &&
                    SotcStats::kFpsLog) {
                    SotcStats::pr_keys_precomputed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    if (SotcPrResolveOn() || SotcPrGenMode() != 0) {
        // sotc: SOTC_PR_RESOLVE / SOTC_PR_GEN: after setup_stages (this op's flat copy) and MapEpoch
        // (PrepLoop).
        PrepareResources(pipeline, out.resolved);
    }
}

void Rasterizer::PrepareDispatch(const AmdGpu::Regs& regs, const AmdGpu::ComputeProgram& cs,
                                 const Pipeline* resolved, PreparedDraw& out) const {
    // sotc: SOTC_GCN_COMMIT (S6a), on a GpuPrep thread after the op's stages are set up (g_info_slot holds the op's user
    // data and flat copy) and after map_epoch. `regs` are the prep thread's replayed registers at this op, equal to the
    // draw thread's DrawRegs(). Fills the register-only inputs BindResources reads for a compute op, each computed as the
    // inline path computes it (the same calls, with regs in place of DrawRegs()): push data, the cache eligibility, the
    // fast-bind static test and hash, the bind resource hash, the compute heap stage's table key, and the resources
    // (PrepareResources, under SOTC_PR_RESOLVE / SOTC_PR_GEN). The program registers are not needed: the resources come
    // from the stage's flat copy, and the dispatch extent is set by DispatchDirect (t_direct_dispatch).
    out.is_dispatch = true;
    out.valid = false;
    out.has_offsets = false; // compute: no vertex offsets
    for (auto& table_key : out.table_keys) {
        table_key.valid = false;
    }
    (void)cs;
    if (!resolved || !resolved->IsCompute()) {
        return;
    }
    out.push_data = MakeUserData(regs);
    out.fast_static = FastBindStatic(resolved, regs); // false for compute (FastBindStatic)
    out.cache_eligible = BindCacheEligible(resolved, regs);
    out.fast_hash = FastBindHash(resolved, out.push_data, regs);
    out.res_ok = SotcStats::kBindCacheFrame && out.cache_eligible && BindResourceHash(resolved, out.res_hash);
    if (SotcPrKeysOn() && SotcArch::TableMemo::On() && resolved->HasPacketRange()) {
        // sotc: SOTC_PR_KEYS: the compute heap stage's memo key (BindResources' resolve_heap_tables, kind 0), with this
        // op's border color base.
        const Shader::Info* heap[2]{};
        if (PrHeapStages(*resolved, heap)) {
            for (u32 k = 0; k < 2; ++k) {
                if (heap[k]) {
                    SotcArch::TableMemo::ComputeKey(*heap[k], regs.ta_bc_base, out.table_keys[k]);
                }
            }
        }
    }
    if (SotcPrResolveOn() || SotcPrGenMode() != 0) {
        PrepareResources(resolved, out.resolved);
    }
    out.valid = true;
}

void Rasterizer::PrepareResources(const Pipeline* pipeline, PreparedResources& out) const {
    // sotc: SOTC_PR_RESOLVE, on a GpuPrep thread (draws and dispatches). Resets everything it writes: the slot keeps the
    // previous op's data until it is overwritten.
    out.num_buffers = 0;
    out.num_images = 0;
    out.stages = {};
    // Read before anything else: an odd value is a change in progress, and BindResources accepts
    // the data only while the generation still equals this one.
    const u64 gen = runtime_memory_generation();
    out.runtime_gen = gen;
    // SOTC_BIND_CLAMP1=0 selects another validity path in BindBuffers: it stays inline.
    static const bool clamp1 = [] {
        const char* v = std::getenv("SOTC_BIND_CLAMP1");
        return !v || v[0] != '0';
    }();
    if ((gen & 1) || !clamp1) {
        return;
    }
    u32 stage_index = 0;
    for (const auto* stage : pipeline->GetStages()) {
        PrStage& ps = out.stages[stage_index++];
        if (!stage || !PrStageResolvable(*stage) ||
            out.num_buffers + stage->buffers.size() > PreparedResources::MaxBuffers ||
            out.num_images + stage->images.size() > PreparedResources::MaxImages) {
            continue;
        }
        ps.ready = true;
        ps.buf_first = out.num_buffers;
        ps.img_first = out.num_images;
        for (const auto& desc : stage->buffers) {
            out.buffers[out.num_buffers++] = PrResolveBuffer(*stage, desc, memory, pipeline->GetProfile());
        }
        for (const auto& image : stage->images) {
            out.images[out.num_images++] = PrResolveImage(*stage, image, memory);
        }
    }
}

void Rasterizer::PrefetchImagesShared(const Pipeline* pipeline) {
    // sotc: SOTC_PARALLEL_RECORD: the prep threads take PrefetchImages off the command processor.
    // TextureCache::PrefetchTexture keeps per-T# state for a single caller, hence the lock.
    if (pipeline && texture_cache.ImagePrefetchEnabled()) {
        std::scoped_lock lock{prep_prefetch_mutex};
        PrefetchImagesImpl(pipeline);
    }
}

const PreparedDraw* Rasterizer::CheckPrepared(const PreparedDraw& prepared,
                                              const GraphicsPipeline* pipeline, bool is_indexed,
                                              bool direct) {
    // sotc: SOTC_VERIFY=1 (verify.h; replaces SOTC_PR_VERIFY=N): every prepared draw is computed
    // inline as well; a difference is logged (first 16), counted, and the inline values are used.
    static const u32 verify_every = SotcVerify::VerifyAll() ? 1u : 0u;
    pr_verify_off = false; // sotc: SOTC_PR_RESOLVE (set below when the verify differs)
    if (prepared.is_indexed != is_indexed) {
        return nullptr; // cannot happen (same op); refused rather than trusted
    }
    if (verify_every == 0 || ++pr_verify_count % verify_every != 0) {
        return &prepared;
    }
    const auto& regs = liverpool->DrawRegs();
    DynamicValues dynamic;
    ComputeDynamic(regs, is_indexed, dynamic);
    const auto push = MakeUserData(regs);
    const bool fast_static = FastBindStatic(pipeline, regs);
    const bool cache_eligible = BindCacheEligible(pipeline, regs);
    const u64 fast_hash = FastBindHash(pipeline, push, regs);
    u32 mismatch = 0;
    mismatch |= SameDynamicValues(dynamic, prepared.dynamic) ? 0u : 1u;
    mismatch |= std::memcmp(&push, &prepared.push_data, sizeof(push)) == 0 ? 0u : 2u;
    mismatch |= fast_static == prepared.fast_static ? 0u : 4u;
    mismatch |= cache_eligible == prepared.cache_eligible ? 0u : 8u;
    mismatch |= fast_hash == prepared.fast_hash ? 0u : 16u;
    // sotc: SOTC_PR_KEYS: each prepared heap key is compared with KeyOf computed here, for the stage it names.
    if (SotcPrKeysOn() && (prepared.table_keys[0].valid || prepared.table_keys[1].valid)) {
        const auto same_key = [](const SotcArch::TableMemo::PreKey& a,
                                 const SotcArch::TableMemo::PreKey& b) {
            return a.stage == b.stage && a.border == b.border && a.hash == b.hash &&
                   a.words == b.words &&
                   std::memcmp(a.key.data(), b.key.data(), a.words * sizeof(u64)) == 0;
        };
        const Shader::Info* heap[2]{};
        const bool stages_ok = PrHeapStages(*pipeline, heap);
        bool keys_bad = false;
        for (u32 k = 0; k < 2; ++k) {
            if (!prepared.table_keys[k].valid) {
                continue;
            }
            SotcArch::TableMemo::PreKey fresh{};
            if (!stages_ok || !heap[k] ||
                !SotcArch::TableMemo::ComputeKey(*heap[k], regs.ta_bc_base, fresh) ||
                !same_key(fresh, prepared.table_keys[k])) {
                keys_bad = true;
            }
        }
        if (keys_bad) {
            mismatch |= 64u;
            SotcStats::pr_keys_mismatch.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (direct && prepared.has_offsets) {
        const auto offsets = GetDrawOffsets(regs, pipeline->GetStage(Shader::SwStage::Vertex),
                                            pipeline->GetFetchShader());
        mismatch |= offsets == std::pair<u32, u32>{prepared.vertex_offset,
                                                   prepared.instance_offset}
                        ? 0u
                        : 32u;
    }
    // sotc: SOTC_PR_RESOLVE: the resource inputs, recomputed here. A difference makes BindResources
    // resolve this draw inline; the return value is unchanged, so the other values fail separately.
    // A table generation that moved since the prep is not compared (BindResources rejects it).
    if (SotcPrResolveOn() && prepared.resolved.runtime_gen == runtime_memory_generation()) {
        PreparedResources fresh{};
        PrepareResources(pipeline, fresh);
        if (!SamePreparedResources(fresh, prepared.resolved)) {
            pr_verify_off = true;
            static std::atomic<u32> shown_resolved{0};
            if (shown_resolved.fetch_add(1, std::memory_order_relaxed) < 20) {
                std::fprintf(stderr,
                             "SOTC PR resolve mismatch (draw %llu): the draw resolves inline\n",
                             static_cast<unsigned long long>(pr_verify_count));
            }
        }
    }
    if (mismatch == 0) {
        return &prepared;
    }
    SotcStats::pr_verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u32> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::fprintf(stderr,
                     "SOTC PR verify mismatch (draw %llu): mask %#x (1 dynamic, 2 push data, 4 fast "
                     "eligible, 8 cache eligible, 16 fast hash, 32 offsets, 64 heap table keys), vs "
                     "%#llx ps %#llx\n",
                     static_cast<unsigned long long>(pr_verify_count), mismatch,
                     static_cast<unsigned long long>(regs.vs_program.Address<uintptr_t>()),
                     static_cast<unsigned long long>(regs.ps_program.Address<uintptr_t>()));
    }
    return nullptr;
}

const PreparedDraw* Rasterizer::CheckPreparedDispatch(const PreparedDraw* prepared, bool use_resolved,
                                                      const Pipeline* pipeline) {
    // sotc: SOTC_GCN_COMMIT (S6a). Draw thread. The slot is used as it is (mode 1); mode 2 and SOTC_VERIFY=1 compare its
    // resource inputs with the inline computation (PrepareResources here, field by field, as CheckPrepared does for
    // draws). Mode 2 never uses the slot; SOTC_VERIFY=1 uses it only when it agrees.
    if (!SotcGcn::Commit::On()) {
        return nullptr;
    }
    pr_verify_off = false; // sotc: SOTC_PR_RESOLVE (BindResources reads this flag for the slot it is given)
    const bool ready = prepared && use_resolved && prepared->valid && prepared->is_dispatch;
    if (!ready) {
        return nullptr; // the availability count is liverpool's (NoteDispatchPrepared)
    }
    const bool verify = SotcGcn::Commit::Verify();
    if (!verify && !SotcVerify::VerifyAll()) {
        return prepared;
    }
    // The inline values of the register inputs, as BindResources computes them for a dispatch without a slot.
    const auto& regs = liverpool->DrawRegs();
    const auto push = MakeUserData(regs);
    const bool fast_static = FastBindStatic(pipeline, regs);
    const bool cache_eligible = BindCacheEligible(pipeline, regs);
    const u64 fast_hash = FastBindHash(pipeline, push, regs);
    u64 res_hash = 0;
    const bool res_ok = SotcStats::kBindCacheFrame && cache_eligible && BindResourceHash(pipeline, res_hash);
    u32 mismatch = 0;
    mismatch |= std::memcmp(&push, &prepared->push_data, sizeof(push)) == 0 ? 0u : 1u;
    mismatch |= fast_static == prepared->fast_static ? 0u : 2u;
    mismatch |= cache_eligible == prepared->cache_eligible ? 0u : 4u;
    mismatch |= fast_hash == prepared->fast_hash ? 0u : 8u;
    mismatch |= res_ok == prepared->res_ok && (!res_ok || res_hash == prepared->res_hash) ? 0u : 16u;
    // The compute heap stage's table key (SOTC_PR_KEYS), recomputed with this op's border color base.
    if (SotcPrKeysOn() && (prepared->table_keys[0].valid || prepared->table_keys[1].valid)) {
        const auto same_key = [](const SotcArch::TableMemo::PreKey& a,
                                 const SotcArch::TableMemo::PreKey& b) {
            return a.stage == b.stage && a.border == b.border && a.hash == b.hash &&
                   a.words == b.words &&
                   std::memcmp(a.key.data(), b.key.data(), a.words * sizeof(u64)) == 0;
        };
        const Shader::Info* heap[2]{};
        const bool stages_ok = PrHeapStages(*pipeline, heap);
        bool keys_bad = false;
        for (u32 k = 0; k < 2; ++k) {
            if (!prepared->table_keys[k].valid) {
                continue;
            }
            SotcArch::TableMemo::PreKey fresh{};
            if (!stages_ok || !heap[k] ||
                !SotcArch::TableMemo::ComputeKey(*heap[k], regs.ta_bc_base, fresh) ||
                !same_key(fresh, prepared->table_keys[k])) {
                keys_bad = true;
            }
        }
        if (keys_bad) {
            mismatch |= 32u;
        }
    }
    // The resources (SOTC_PR_RESOLVE / SOTC_PR_GEN), while the table generation the slot was made under still stands.
    if ((SotcPrResolveOn() || SotcPrGenMode() != 0) &&
        prepared->resolved.runtime_gen == runtime_memory_generation()) {
        PreparedResources fresh{};
        PrepareResources(pipeline, fresh);
        if (!SamePreparedResources(fresh, prepared->resolved)) {
            mismatch |= 64u;
        }
    }
    const bool match = mismatch == 0;
    if (!match) {
        static std::atomic<u32> shown_dispatch{0};
        if (shown_dispatch.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::fprintf(stderr,
                         "SOTC GCN COMMIT dispatch verify mismatch: mask %#x (1 push data, 2 fast static, 4 cache "
                         "eligible, 8 fast hash, 16 bind resource hash, 32 heap table key, 64 resources), the dispatch "
                         "uses the inline values\n",
                         mismatch);
        }
    }
    SotcGcn::Commit::NoteDispatchVerify(match);
    return !verify && match ? prepared : nullptr;
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed,
                                    const PreparedDraw* prepared) const {
    // sotc: the values are computed from the registers alone (ComputeDynamic, also run ahead by
    // SOTC_PARALLEL_RECORD's prep threads) and handed to DynamicState in the order of the former
    // Update*State calls (ApplyDynamic).
    if (prepared) {
        ApplyDynamic(prepared->dynamic, pipeline);
    } else {
        DynamicValues values;
        SotcNoPrepass::Count(liverpool->DrawRegs()); // sotc: SOTC_NO_PREPASS=2, unprepared path
        SotcPrepassProbe::NoteDraw(liverpool->DrawRegs()); // sotc: SOTC_PREPASS_PROBE, unprepared path
        ComputeDynamic(liverpool->DrawRegs(), is_indexed, values);
        ApplyDynamic(values, pipeline);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::ComputeDynamic(const AmdGpu::Regs& regs, const bool is_indexed,
                                DynamicValues& out, bool* refuse) const {
    ComputeViewportScissor(regs, out);
    ComputeDepthStencil(regs, out);
    ComputePrimitive(regs, is_indexed, out, refuse);
    out.line_width = regs.line_control.Width();
    out.blend_constants = regs.blend_constants;
}

void Rasterizer::ApplyDynamic(const DynamicValues& values, const GraphicsPipeline* pipeline) const {
    auto& dynamic_state = scheduler.GetDynamicState();
    // UpdateViewportScissorState
    dynamic_state.SetViewports(values.viewports);
    dynamic_state.SetScissors(values.scissors);
    // UpdateDepthStencilState
    dynamic_state.SetDepthTestEnabled(values.depth_test);
    if (values.depth_test) {
        // sotc: SOTC_NO_PREPASS=1. A G-buffer draw whose pre-pass draw was dropped lays its own depth.
        if (SotcPrepassProbe::t_promote && values.depth_compare == vk::CompareOp::eEqual) {
            // The pre-pass draw's own function in its "or equal" form (Less -> LessOrEqual, Greater -> GreaterOrEqual).
            vk::CompareOp op = LiverpoolToVK::CompareOp(SotcPrepassProbe::t_promote_func);
            if (op == vk::CompareOp::eLess) {
                op = vk::CompareOp::eLessOrEqual;
            } else if (op == vk::CompareOp::eGreater) {
                op = vk::CompareOp::eGreaterOrEqual;
            }
            dynamic_state.SetDepthWriteEnabled(true);
            dynamic_state.SetDepthCompareOp(op);
        } else {
            dynamic_state.SetDepthWriteEnabled(values.depth_write);
            dynamic_state.SetDepthCompareOp(values.depth_compare);
        }
    }
    dynamic_state.SetDepthBoundsTestEnabled(values.depth_bounds_test);
    if (values.depth_bounds_test) {
        dynamic_state.SetDepthBounds(values.depth_bounds_min, values.depth_bounds_max);
    }
    dynamic_state.SetDepthBiasEnabled(values.depth_bias);
    if (values.depth_bias) {
        dynamic_state.SetDepthBias(values.bias_constant, values.bias_clamp, values.bias_slope);
    }
    dynamic_state.SetStencilTestEnabled(values.stencil_test);
    if (values.stencil_test) {
        dynamic_state.SetStencilOps(values.stencil_front, values.stencil_back);
        dynamic_state.SetStencilReferences(values.stencil_ref_front, values.stencil_ref_back);
        dynamic_state.SetStencilWriteMasks(values.stencil_write_front, values.stencil_write_back);
        dynamic_state.SetStencilCompareMasks(values.stencil_compare_front,
                                             values.stencil_compare_back);
    }
    // UpdatePrimitiveState
    dynamic_state.SetPrimitiveRestartEnabled(values.prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(values.rasterizer_discard);
    dynamic_state.SetCullMode(values.cull_mode);
    dynamic_state.SetFrontFace(values.front_face);
    // UpdateRasterizationState
    dynamic_state.SetLineWidth(values.line_width);
    // UpdateColorBlendingState
    dynamic_state.SetBlendConstants(values.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
    // sotc: SOTC_GPL_DYN_STATE: the key's rasterization state (topology, polygon mode, depth clamp/clip, provoking
    // vertex, clip space, samples), made from the same registers when the key was built; recorded where dynamic.
    dynamic_state.SetKeyStatics(pipeline->GetKeyStatics());
}

void Rasterizer::ComputeViewportScissor(const AmdGpu::Regs& regs, DynamicValues& out) const {

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    auto& viewports = out.viewports;
    auto& scissors = out.scissors;
    viewports.clear();
    scissors.clear();

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }
}

void Rasterizer::ComputeDepthStencil(const AmdGpu::Regs& regs, DynamicValues& out) const {
    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    out.depth_test = depth_test_enabled;
    if (depth_test_enabled) {
        out.depth_write = regs.depth_control.depth_write_enable &&
                          !regs.depth_render_control.depth_clear_enable;
        out.depth_compare = LiverpoolToVK::CompareOp(regs.depth_control.depth_func);
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    out.depth_bounds_test = depth_bounds_test_enabled;
    if (depth_bounds_test_enabled) {
        out.depth_bounds_min = regs.depth_bounds_min;
        out.depth_bounds_max = regs.depth_bounds_max;
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    out.depth_bias = depth_bias_enabled;
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        out.bias_constant = front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset;
        out.bias_clamp = regs.poly_offset.depth_bias;
        out.bias_slope =
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f;
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    out.stencil_test = stencil_test_enabled;
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        out.stencil_front = front_ops;
        out.stencil_back = back_ops;

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        out.stencil_ref_front = front_op ? front.stencil_op_val : front.stencil_test_val;
        out.stencil_ref_back = back_op ? back.stencil_op_val : back.stencil_test_val;
        out.stencil_write_front = !stencil_clear ? front.stencil_write_mask : 0U;
        out.stencil_write_back = !stencil_clear ? back.stencil_write_mask : 0U;
        out.stencil_compare_front = front.stencil_mask;
        out.stencil_compare_back = back.stencil_mask;
    }
}

void Rasterizer::ComputePrimitive(const AmdGpu::Regs& regs, const bool is_indexed,
                                  DynamicValues& out, bool* refuse) const {

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    // sotc: a prep thread (SOTC_PARALLEL_RECORD) refuses the draw instead of asserting: the
    // draw thread may still drop it, and asserts inline otherwise.
    const bool restart_supported = !is_indexed || !prim_restart ||
                                   regs.primitive_restart_index == 0xFFFF ||
                                   regs.primitive_restart_index == 0xFFFFFFFF;
    if (refuse && !restart_supported) {
        *refuse = true;
    }
    ASSERT_MSG(refuse || restart_supported,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    out.prim_restart = prim_restart;
    out.rasterizer_discard = regs.clipper_control.dx_rasterization_kill;
    out.cull_mode = cull_mode;
    out.front_face = front_face;
}

bool SameDynamicValues(const DynamicValues& a, const DynamicValues& b) {
    // sotc: SOTC_PR_VERIFY: bit equality of every field (the vectors' live elements only).
    const auto same = [](const auto& x, const auto& y) {
        return std::memcmp(&x, &y, sizeof(x)) == 0;
    };
    if (a.viewports.size() != b.viewports.size() || a.scissors.size() != b.scissors.size()) {
        return false;
    }
    for (size_t i = 0; i < a.viewports.size(); ++i) {
        if (!same(a.viewports[i], b.viewports[i]) || !same(a.scissors[i], b.scissors[i])) {
            return false;
        }
    }
    return a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
           a.depth_bounds_test == b.depth_bounds_test && a.depth_bias == b.depth_bias &&
           a.stencil_test == b.stencil_test && a.prim_restart == b.prim_restart &&
           a.rasterizer_discard == b.rasterizer_discard && a.depth_compare == b.depth_compare &&
           same(a.depth_bounds_min, b.depth_bounds_min) &&
           same(a.depth_bounds_max, b.depth_bounds_max) &&
           same(a.bias_constant, b.bias_constant) && same(a.bias_clamp, b.bias_clamp) &&
           same(a.bias_slope, b.bias_slope) && a.stencil_front == b.stencil_front &&
           a.stencil_back == b.stencil_back && a.stencil_ref_front == b.stencil_ref_front &&
           a.stencil_ref_back == b.stencil_ref_back &&
           a.stencil_write_front == b.stencil_write_front &&
           a.stencil_write_back == b.stencil_write_back &&
           a.stencil_compare_front == b.stencil_compare_front &&
           a.stencil_compare_back == b.stencil_compare_back && a.cull_mode == b.cull_mode &&
           a.front_face == b.front_face && same(a.line_width, b.line_width) &&
           same(a.blend_constants, b.blend_constants);
}


void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

const Pipeline* Rasterizer::LookupGraphicsPipeline(u16 vertex_sgpr_offset,
                                                   u16 instance_sgpr_offset) {
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    return pipeline_cache.GetGraphicsPipeline(params);
}

const Pipeline* Rasterizer::LookupComputePipeline() {
    return pipeline_cache.GetComputePipeline();
}

bool Rasterizer::IsGpuSideThread() const {
    return liverpool->IsGpuSideThread();
}

void Rasterizer::PrepareGpuSideFault() {
    liverpool->PrepareGpuSideFault();
}

bool Rasterizer::RunsGpuWorkElsewhere() const {
    return liverpool->RunsGpuWorkElsewhere();
}

void Rasterizer::RunOnDrawThreadImpl(Common::UniqueFunction<void>&& func) {
    liverpool->RunOnBackThread(std::move(func));
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    return liverpool->IsGpuSideThreadId(tid);
}
#endif

// sotc: SOTC_LOAD_WARM: an early bind of a recorded texture on the draw thread (idle gaps,
// Liverpool::BackLoop): FindImage and FindTexture with the ImageDesc the real bind builds, so the
// image is created, tracked (page protection) and uploaded before the first frame needs it. A record
// whose T# no longer points at mapped GPU memory, or has an invalid format, is skipped.
void Rasterizer::WarmTexture(const void* data, size_t size) {
    if (size != sizeof(SotcTexBlob)) {
        return;
    }
    SotcTexBlob blob;
    std::memcpy(&blob, data, sizeof blob);
    const AmdGpu::Image& tsharp = blob.tsharp;
    const auto dfmt = tsharp.GetDataFmt();
    if (!tsharp || dfmt == AmdGpu::DataFormat::FormatInvalid || !magic_enum::enum_contains(dfmt) ||
        !magic_enum::enum_contains(tsharp.GetNumberFmt()) ||
        !memory->IsValidGpuMapping(tsharp.Address(), 0)) {
        return;
    }
    const auto texture_lock = texture_cache.BatchLock();
    VideoCore::TextureCache::ImageDesc desc(tsharp, blob.resource);
    // One texture is one call: a large one would hold the draw thread past the idle budget.
    if (desc.info.guest_size > 16_MB) {
        return;
    }
    // A record is an old T#: its whole range must be mapped now (the image hashes and uploads read
    // guest memory directly; a range running into unmapped memory crashed the first texture warm,
    // read fault in XXH3 at 0x108e1dc000), and memory the GPU writes is left to the real bind
    // (its pages can be read-protected, and the draw thread cannot take that fault).
    const VAddr base = desc.info.guest_address;
    const u64 bytes = desc.info.guest_size;
    if (bytes == 0 || !memory->IsValidMapping(base, bytes) ||
        buffer_cache.IsRegionGpuModified(base, bytes)) {
        return;
    }
    auto image_id = texture_cache.FindImage(desc);
    if (!image_id) {
        return;
    }
    // As BindTexturesFull: a stencil image with an associated depth image is read through it.
    if (const auto depth_image_id = texture_cache.GetAssociatedDepth(texture_cache.GetImage(image_id))) {
        image_id = depth_image_id;
    }
    (void)texture_cache.FindTexture(image_id, desc);
}

// sotc: SOTC_GPU_CHECKPOINTS: the buffers and images command `seq` binds (bound_buffers, the vertex/index reads,
// bound_images), for the device lost report (sotc_checkpoints.h). seq 0: the switch is off.
void Rasterizer::CheckpointResources(u64 seq) {
    if (seq == 0) {
        return;
    }
    std::array<SotcCheckpoints::Resource, 64> res{};
    u32 n = 0;
    const auto add = [&](u64 addr, u64 size, u64 handle, u8 type, const VideoCore::Buffer* buffer, u64 offset) {
        if (n < res.size()) {
            const u64 base = buffer && buffer->HasBufferDeviceAddress() ? buffer->BufferDeviceAddress() : 0;
            res[n++] = {addr, size, handle, type, base ? base + offset : 0, base ? buffer->SizeBytes() : 0, base};
        }
    };
    for (const auto& b : bound_buffers) {
        add(b.addr != 0 ? b.addr : (b.buffer ? b.buffer->CpuAddr() + b.offset : 0), b.size,
            b.buffer ? reinterpret_cast<u64>(static_cast<VkBuffer>(b.buffer->Handle())) : 0,
            b.is_written ? SotcCheckpoints::kBufferWritten : SotcCheckpoints::kBufferRead, b.buffer, b.offset);
    }
    for (const auto& b : fixed_function_reads) {
        add(b.addr != 0 ? b.addr : (b.buffer ? b.buffer->CpuAddr() + b.offset : 0), b.size,
            b.buffer ? reinterpret_cast<u64>(static_cast<VkBuffer>(b.buffer->Handle())) : 0,
            SotcCheckpoints::kVertexIndex, b.buffer, b.offset);
    }
    for (const auto id : bound_images) {
        if (!id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(id);
        add(image.info.guest_address, image.info.guest_size,
            reinterpret_cast<u64>(static_cast<VkImage>(image.GetImage())), SotcCheckpoints::kImage, nullptr, 0);
    }
    // The pipeline bound for the command (a destroyed one is a use after free on the GPU).
    if (const Pipeline* bound = t_checkpoint_pipeline) {
        add(0, 0, reinterpret_cast<u64>(static_cast<VkPipeline>(bound->Handle())), SotcCheckpoints::kPipeline, nullptr,
            0);
        // The permutation of each graphics stage (key stage hash = HashCombine(program hash, permutation)): as a
        // pipeline row, addr = stage << 32 | permutation (0xFFFF: not found in 0..63), size = the program hash.
        if (!bound->IsCompute()) {
            const auto& key = static_cast<const GraphicsPipeline*>(bound)->GetGraphicsKey();
            const auto stages = bound->GetStages();
            for (u32 s = 0; s < stages.size() && s < key.stage_hashes.size(); ++s) {
                if (!stages[s]) {
                    continue;
                }
                u32 perm = 0xFFFF;
                for (u32 p = 0; p < 64; ++p) {
                    if (HashCombine(stages[s]->pgm_hash, u64{p}) == key.stage_hashes[s]) {
                        perm = p;
                        break;
                    }
                }
                add((u64{s} << 32) | perm, stages[s]->pgm_hash, key.stage_hashes[s], SotcCheckpoints::kPipeline, nullptr,
                    0);
            }
        }
    }
    // The descriptor writes as they go to the set: a binding of the layout missing here was never written.
    for (u32 i = 0; i < set_write_index && i < set_writes.size(); ++i) {
        const auto& w = set_writes[i];
        const u64 key = u64{w.dstBinding} | (u64{static_cast<u32>(w.descriptorType)} << 32);
        if (w.pBufferInfo) {
            add(key, w.pBufferInfo->range, reinterpret_cast<u64>(static_cast<VkBuffer>(w.pBufferInfo->buffer)),
                SotcCheckpoints::kDescriptor, nullptr, 0);
            res[n - 1].gpu = w.pBufferInfo->offset;
        } else if (w.pImageInfo) {
            // A sampler write has no view: its sampler handle goes in gpu (offset column).
            add(key, static_cast<u64>(static_cast<VkImageLayout>(w.pImageInfo->imageLayout)),
                reinterpret_cast<u64>(static_cast<VkImageView>(w.pImageInfo->imageView)),
                SotcCheckpoints::kDescriptor, nullptr, 0);
            res[n - 1].gpu = reinterpret_cast<u64>(static_cast<VkSampler>(w.pImageInfo->sampler));
        } else {
            add(key, 0, 0, SotcCheckpoints::kDescriptor, nullptr, 0);
        }
    }
    SotcCheckpoints::NoteResources(seq, res.data(), n);
}

// sotc: SOTC_DRAW_GATE (draw_gate.h). The guest ranges the draw reads (read-only shader buffers, fetch shader vertex
// buffers, the index buffer), each with the bytes its bind would upload now, and the draw's kind: SotcDrawGate::Decide
// says whether it waits for the staged upload. Ranges are capped at 64 MiB (an unbounded V#).
bool Rasterizer::DrawGateDefers(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset) {
    const auto& regs = liverpool->DrawRegs();
    std::array<SotcDrawGate::Range, 48> ranges{};
    size_t n = 0;
    bool writes = StagesWriteMemory(*pipeline);
    const auto add = [&](VAddr addr, u64 size) {
        if (addr == 0 || size == 0 || n >= ranges.size()) {
            return;
        }
        size = std::min<u64>(size, 64_MB);
        // SOTC_PC_RING=8: a range inside the imported arena is read in host memory (BufferCache::PcHostBind), never
        // uploaded, though its pages stay CPU-modified (no tracking there): nothing to wait for.
        const bool host_served = SotcPcRing::Mode() == 8 && SotcPcRing::ArenaRelation(addr, size) == 1;
        ranges[n++] = {addr, size, host_served ? 0 : buffer_cache.PendingUploadBytes(addr, size)};
    };
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                continue;
            }
            if (desc.is_written) {
                writes = true;
                continue;
            }
            // The range BindBuffers will sync: the V# clamped as there (mapping, unbounded window, read and shader
            // extents, SOTC_BLOCK_EXTENT), so an unbounded V# does not count memory no bind uploads.
            const PrBuffer b = PrResolveBuffer(*stage, desc, memory, pipeline->GetProfile());
            if (b.computed && !b.shader_addressed) {
                add(b.base, b.size);
            }
        }
    }
    if (const auto* vs = pipeline->GetStages()[static_cast<u32>(Shader::SwStage::Vertex)]) {
        for (const auto& attrib : pipeline->GetFetchShader().attributes) {
            const auto sharp = attrib.GetSharp(*vs);
            add(sharp.base_address, sharp.GetSize());
        }
    }
    if (is_indexed) {
        const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        add(regs.index_base_address.Address<VAddr>() + u64{index_offset} * index_size,
            u64{regs.num_indices} * index_size);
    }
    const bool full_screen = regs.num_indices <= 6;
    const bool may_follow = !writes && !full_screen;
    const bool equal_depth =
        regs.depth_control.depth_enable && regs.depth_control.depth_func == AmdGpu::CompareFunc::Equal;
    const bool may_defer = may_follow && !equal_depth;
    return SotcDrawGate::Decide(ranges.data(), n, may_defer, may_follow,
                                SotcStats::flip_count.load(std::memory_order_relaxed));
}

// sotc: SOTC_GCN_CENSUS (gcn_census.h, stage S0 of ARCH-GCN-NATIVE.md). What this op binds, from its stages and the
// sharps as the bind would read them (buffers through PrResolveBuffer, the same clamps as BindBuffers). Measurement only.
void Rasterizer::CensusNote(const Pipeline* pipeline, u8 kind) {
    SotcGcnCensus::Op op{};
    op.kind = kind;
    op.writes_memory = StagesWriteMemory(*pipeline);
    std::array<u64, 64> tsharps{};
    std::array<u64, 32> ssharps{};
    u32 nt = 0, ns = 0;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        op.storage |= stage->has_storage_images;
        op.fmask |= !stage->fmasks.empty();
        op.dma |= stage->uses_dma;
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                ++op.buf_special;
                continue;
            }
            if (desc.is_written) {
                ++op.buf_written;
            } else if (desc.is_formatted) {
                ++op.buf_formatted;
            } else {
                ++op.buf_plain;
            }
            const PrBuffer b = PrResolveBuffer(*stage, desc, memory, pipeline->GetProfile());
            if (!b.computed || b.base == 0 || b.size == 0) {
                continue;
            }
            op.buf_bytes += b.size;
            const u32 bucket = b.size < 4_KB ? 0 : b.size < 64_KB ? 1 : b.size < 1_MB ? 2 : b.size < 16_MB ? 3 : 4;
            ++op.size_hist[bucket];
            if (SotcPcRing::Mode() == 8 && SotcPcRing::ArenaRelation(b.base, b.size) == 1) {
                ++op.buf_host;
            }
        }
        for (const auto& desc : stage->images) {
            const AmdGpu::Image tsharp = desc.GetSharp(*stage);
            (desc.is_written ? op.img_storage : op.img_sampled)++;
            op.img_array += desc.is_array;
            op.img_desc_array += desc.array_size != 0;
            const auto type = tsharp.GetType();
            op.img_3d += type == AmdGpu::ImageType::Color3D;
            op.img_cube += type == AmdGpu::ImageType::Cube;
            op.img_mip += tsharp.base_level != 0;
            if (nt < tsharps.size()) {
                tsharps[nt++] = XXH3_64bits(&tsharp, sizeof(tsharp));
            }
        }
        for (const auto& desc : stage->samplers) {
            const AmdGpu::Sampler ssharp = desc.GetSharp(*stage);
            ++op.samplers;
            if (ns < ssharps.size()) {
                ssharps[ns++] = XXH3_64bits(&ssharp, sizeof(ssharp));
            }
        }
    }
    SotcGcnCensus::Note(op, tsharps.data(), nt, ssharps.data(), ns,
                        SotcStats::flip_count.load(std::memory_order_relaxed));
}

} // namespace Vulkan
