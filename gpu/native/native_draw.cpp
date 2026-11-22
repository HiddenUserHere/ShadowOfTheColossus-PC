// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_draw implementation.
#include "native/native_draw.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "native/native_renderer.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

// The gnmdriver-owned presenter singleton (gnmdriver.cpp, file scope, global namespace), as
// native_pass.cpp declares it. It must stay outside namespace Native: declaring it inside would
// give the linker a different symbol (Native::presenter) that nothing defines.
extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Native {

void Draw::BindPipeline(vk::CommandBuffer cmdbuf, const Vulkan::Pipeline& pipeline) const {
    cmdbuf.bindPipeline(pipeline.IsCompute() ? vk::PipelineBindPoint::eCompute
                                             : vk::PipelineBindPoint::eGraphics,
                        pipeline.Handle());
}

void Draw::FullscreenRect(vk::CommandBuffer cmdbuf) const {
    // The engine's fullscreen shader is procedural (the guest uses prim 0x11 RectList with a
    // 3-vertex auto draw in 0x93690); a single triangle covers the viewport.
    cmdbuf.draw(3, 1, 0, 0);
}

void Draw::Auto(vk::CommandBuffer cmdbuf, u32 count, u32 instances, u32 first_vertex,
                u32 first_instance) const {
    cmdbuf.draw(count, instances, first_vertex, first_instance);
}

void Draw::IndexedOffset(vk::CommandBuffer cmdbuf, u32 index_count, u32 first_index, u32 instances,
                         s32 vertex_offset) const {
    cmdbuf.drawIndexed(index_count, instances, first_index, vertex_offset, 0);
}

void Draw::Indirect(vk::CommandBuffer cmdbuf, vk::Buffer args, u64 offset, u32 stride) const {
    cmdbuf.drawIndirect(args, offset, 1, stride);
}

void Draw::IndexedIndirect(vk::CommandBuffer cmdbuf, vk::Buffer args, u64 offset,
                           u32 stride) const {
    cmdbuf.drawIndexedIndirect(args, offset, 1, stride);
}

void Draw::Dispatch(vk::CommandBuffer cmdbuf, u32 x, u32 y, u32 z) const {
    cmdbuf.dispatch(x, y, z);
}

namespace {
std::atomic<u64> g_issue_trace_budget{64};
bool IssueTraceAllowed() {
    return g_issue_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}
bool IssueVerbose() {
    static const bool v = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    return v;
}
} // namespace

AmdGpu::Regs& Draw::GuestRegs() const {
    return presenter->GetLiverpool().regs;
}

// Removed: the scratch copy was shared by every guest thread that draws (see native_draw.h); the
// draw objects keep their register image in their own call frame now.

bool Draw::IssueFromRegs(const DrawRequest& request) const {
    // Superseded (plan "100% native in one phase"): this path copied the command processor's LIVE
    // registers at hook time (wrong state, wrong moment) and Rasterizer::DrawNative dropped the
    // draw when the command processor is back-threaded. Native draws now come from the GNM
    // boundary as stream ops (gpu/native/gnm/w09_draws.cpp -> Liverpool::NativeSubmitDraw).
    // Declining keeps every hooked draw object on its guest body until then.
    static const bool legacy = [] {
        const char* e = std::getenv("SOTC_NATIVE_LEGACY_DRAW");
        return e && e[0] == '1';
    }();
    if (!legacy || request.regs == nullptr) {
        return false;
    }
    if (Current() == nullptr || !presenter) {
        return false;
    }
    // The port's rasterizer owns the GPU-state path the draw reuses (targets, descriptors, user
    // data, viewport/scissor, binds): reached through the presenter singleton like native_pass.
    Vulkan::Rasterizer& rasterizer = presenter->GetRasterizer();
    AmdGpu::Liverpool& liverpool = presenter->GetLiverpool();
    // The indirect arguments are guest memory the buffer cache names (the guest's own record
    // state predates the request; the index-buffer type is the pass's cached one).
    AmdGpu::Regs& regs_ref = *request.regs;
    if (request.is_indexed && request.regs->index_buffer_type.index_type !=
                                  AmdGpu::IndexType::Index32) {
        regs_ref.index_buffer_type.index_type =
            static_cast<AmdGpu::IndexType>(regs_ref.index_buffer_type.raw);
    }
    vk::Buffer native_args_buffer{};
    u64 native_args_offset = 0;
    if (request.args != 0) {
        const auto [args_buffer, args_offset] =
            rasterizer.GetBufferCache().ObtainBuffer(request.args, request.args_size, false);
        if (args_buffer == nullptr) {
            return false;
        }
        native_args_buffer = args_buffer->Handle();
        native_args_offset = args_offset;
    }
    // The pipeline: the hook's own content-key seam when it has one (the material bind path);
    // otherwise the port's cache builds it from the registers (the same lookup the emulated draw
    // makes), so the key, the stages and the specializations are the emulator's. The cast is the
    // documented GraphicsPipeline : Pipeline upcast (vk_graphics_pipeline.h:71).
    const Vulkan::GraphicsPipeline* pipeline = request.pipeline != nullptr
                                                    ? static_cast<const Vulkan::GraphicsPipeline*>(
                                                          request.pipeline)
                                                    : rasterizer.GetPipelineCache().GetGraphicsPipeline(
                                                          Vulkan::DrawIndirectParams{
                                                              .vertex_sgpr_offset =
                                                                  request.vertex_sgpr_offset,
                                                              .instance_sgpr_offset =
                                                                  request.instance_sgpr_offset});
    if (pipeline == nullptr) {
        return false; // no pipeline (refused/still building): chain to the guest body
    }
    Vulkan::Rasterizer::NativeDrawRequest native{};
    native.pipeline = pipeline;
    native.regs = std::make_unique<AmdGpu::Regs>(*request.regs);
    native.is_indexed = request.is_indexed;
    if (request.args != 0) {
        native.args = native_args_buffer;
        native.args_offset = native_args_offset;
        native.args_stride = request.args_size;
        native.first_vertex = 0;
    }
    native.first_vertex = request.is_indexed ? 0u : request.index_offset; // auto: vertexOffset
    native.index_count = request.fullscreen ? 3u : request.num_indices;
    native.fullscreen = request.fullscreen;
    native.instances = request.regs->num_instances.NumInstances();
    native.index_type = static_cast<u32>(request.regs->index_buffer_type.index_type);
    // Rasterizer::DrawNative reads the registers the request carries, sets num_indices from the
    // request and binds the index buffer from index_base_address with first_index = 0: the
    // object's own index_offset is folded into the base here (the guest's 0x86b040 passes it as
    // the draw's first index).
    if (request.is_indexed) {
        const u32 size = request.regs->index_buffer_type.index_type == AmdGpu::IndexType::Index16
                             ? 2u
                             : 4u;
        native.index_base = VAddr(request.regs->index_base_address.Address<VAddr>() +
                                  u64(request.index_offset) * size);
    }
    const u32 count = request.num_indices;
    const u32 offset = request.index_offset;
    const bool indexed = request.is_indexed;
    // The native draw's commands are the stream's next operations: with the recording thread
    // they go there -- the stream is flushed and handed over, the commands are recorded on the
    // recording thread after everything queued so far (the registration resets are deletes: they
    // must follow the draws that used the buffers) and the stream goes back to the producer when
    // the draw returns. Without the recording thread the command processor owns the stream and
    // records them right away.
    Vulkan::Scheduler& scheduler = rasterizer.GetScheduler();
    auto* queued = new Vulkan::Rasterizer::NativeDrawRequest(std::move(native));
    Common::UniqueFunction<void> run = [&rasterizer, queued]() {
        rasterizer.DrawNative(*queued);
        delete queued;
    };
    if (scheduler.RecorderHeld()) {
        // A native draw of a nested hook: it belongs to the stream's current batch, on this
        // thread, so it is recorded among them (the outermost holder closes the batch).
        run();
    } else {
        scheduler.HoldRecorder(std::move(run));
    }
    if (IssueVerbose() && IssueTraceAllowed()) {
        std::fprintf(stderr,
                     "SOTC RENDER native: draw count=%u indexed=%u offset=%u issued\n", count,
                     indexed ? 1u : 0u, offset);
    }
    return true;
}

} // namespace Native
