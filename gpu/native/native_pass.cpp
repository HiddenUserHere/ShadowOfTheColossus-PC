// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_pass implementation (dynamic rendering) + the Phase 2.5
// render-target bind decode (docs/engine/native-render-map-bind.md §3, gap N2-7) + the Phase 2.6
// EOP/WAIT bridge and pass viewport/scissor.
#include "native/native_pass.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "native/guest_bridge.h"
#include "native/native_stream_ops.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

// The gnmdriver-owned presenter singleton (gnmdriver.cpp:28, at file scope before
// namespace Libraries::GnmDriver) — declared here so ResolveViews binds to the real global.
extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Native {

// -- Descriptor-block decode (N2-7) -----------------------------------------------------------

bool Rt::DecodeColour(const u64* block, AmdGpu::ColorBuffer& out, u32& extent_hint) {
    out = {};
    extent_hint = 0;
    if (block == nullptr) {
        return false;
    }
    // The guest's 8-qword CB block: 15 words == AmdGpu::ColorBuffer (60 bytes), the 16th word is
    // the trailing NOP-hint dword (the port's CbDbExtent, liverpool.cpp:1488-1494). static_assert
    // below pins the decode; a future ColorBuffer growth must update this path.
    static_assert(sizeof(AmdGpu::ColorBuffer) == 60,
                  "the guest's CB block words 0..14 must match AmdGpu::ColorBuffer");
    std::memcpy(&out, block, sizeof(AmdGpu::ColorBuffer));
    // The guest's validity predicate (0x87eda0 folded into 0x86a1a0): no address or no format
    // means "slot disabled" (CB_COLORn_INFO = 0 emitted). The port's operator bool()
    // (regs_color.h:199) encodes the same test (base_address && info.format).
    extent_hint = static_cast<u32>(block[7] >> 32); // trailing NOP-packet dword = CbDbExtent hint
    return static_cast<bool>(out);
}

bool Rt::DecodeDepth(const u64* block, AmdGpu::DepthBuffer& out, AmdGpu::DepthView& view,
                     VAddr& htile, u32& extent_hint) {
    out = {};
    view = {};
    htile = 0;
    extent_hint = 0;
    if (block == nullptr) {
        return false;
    }
    const u32* w = reinterpret_cast<const u32*>(block);
    // Guest DB block (13 dwords at descriptor +0x164): the DepthBuffer register image
    // (DB_Z_INFO..DB_DEPTH_SLICE, words 0..7 == regs.h DepthBuffer layout) + DB_DEPTH_VIEW
    // (word 8) + DB_HTILE_DATA_BASE (word 9), then two words the emulated path does not decode
    // (0x86a290 sends them to context regs 0x2af/0xf) and the trailing NOP-hint extent (word 12,
    // liverpool.cpp DbZInfo case: payload[21] = last_db_extent).
    static_assert(sizeof(AmdGpu::DepthBuffer) == 32,
                  "the guest's DB block words 0..7 must match AmdGpu::DepthBuffer");
    std::memcpy(&out, w, sizeof(AmdGpu::DepthBuffer));
    std::memcpy(&view, w + 8, sizeof(AmdGpu::DepthView));
    htile = u64(w[9]) << 8; // DB_HTILE_DATA_BASE: word offset << 8, as Address::GetAddress()
    extent_hint = w[12];
    return out.DepthValid() || out.StencilValid();
}

vk::Format Rt::ColourVkFormat(const AmdGpu::ColorBuffer& buffer) {
    // The port's own decoder, end to end: ColorBuffer::GetDataFmt/GetNumberFmt (regs_color.h:250,
    // with the RemapDataFormat/RemapNumberFormat CB-vs-T# fixes of pixel_format.h) ->
    // Vulkan::LiverpoolToVK::SurfaceFormat (the surface_format_table, liverpool_to_vk.cpp:786).
    // N2-7 closure: the guest's decode family reads exactly the fields the port's Color0Info
    // bitfields decode (FUN_00866710 args = info>>2&0x1f | info>>8&7 | info>>0xb&3 = format |
    // number_type | comp_swap), so the mapping table is reused, not re-derived.
    return Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
}

vk::Format Rt::DepthVkFormat(const AmdGpu::DepthBuffer& buffer) {
    // DB_Z_INFO format class: 2 bits, 0 = invalid, 1 = Z16, 3 = Z32F (the guest's `*puVar9 & 3`
    // poly-offset format selector feeding 0x86e3a0; regs_depth.h ZFormat). Stencil 1 bit.
    // Callers decode blocks that may be stale or not depth at all (the read-only pass derivation):
    // z_format 2 reached LiverpoolToVK::DepthFormat's assertion and stopped the game (run
    // probe-passonly-29, gap N2-62), so anything but Z16/Z32F is "no depth format" here.
    const auto z = buffer.z_info.format;
    if (z != AmdGpu::DepthBuffer::ZFormat::Z16 && z != AmdGpu::DepthBuffer::ZFormat::Z32Float) {
        return vk::Format::eUndefined;
    }
    return Vulkan::LiverpoolToVK::DepthFormat(buffer.z_info.format, buffer.stencil_info.format);
}

// -- EOP/WAIT bridge (Phase 2.6) ----------------------------------------------------------------

u64 Rt::Eop::ReserveLabel(u64 cb, u32 size_dwords) {
    // 0x86d210(cb, size_dwords, 3): the guest allocates the label inline in its own command
    // buffer and returns its address there. Reached through the image with the guest's SysV
    // convention (engine code, not the 0x85/0x86 GNM library the native backend bypasses: this is
    // the allocation the guest body would perform, so the label lives where the guest expects).
    const u64 base = Guest::GuestImageBase();
    if (base == 0 || cb == 0) {
        return 0;
    }
    using GuestLabelAllocFn = u64(PS4_SYSV_ABI*)(u64, u64, int);
    return reinterpret_cast<GuestLabelAllocFn>(base + 0x86d210)(cb, size_dwords, 3);
}

void Rt::Eop::SignalLabel(const Label& label) {
    if (label.address == 0) {
        return;
    }
    // The guest body writes `*slot = 0` unconditionally after 0x86d210 (00093960.c:97,
    // 00094600.c:34, 00093ef0.c:107); the native commit mirrors exactly that store. It runs
    // below the commit point, like every guest-visible write of these callbacks.
    u32 zero = 0;
    Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(label.address), &zero,
                                              sizeof(zero));
}

bool Rt::Eop::WaitLabel(const Label& label, u64 cb) {
    if (!presenter) {
        return false;
    }
    if (cb != 0 && StreamOps::Enabled()) {
        // Gap N2-65: the fence point is the stream position after the ops queued before it; the
        // command processor marks it on its draw thread (no scheduler access from this thread).
        const u64 address = label.address;
        return StreamOps::Emit(cb, [address] {
            auto& rasterizer = presenter->GetRasterizer();
            rasterizer.OnFence();
            rasterizer.GetScheduler().DeferPriorityOperation([address] {
                u32 value = 0;
                (void)Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(address),
                                                                &value, sizeof(value), 2);
            });
        });
    }
    auto& rasterizer = presenter->GetRasterizer();
    Vulkan::Scheduler& scheduler = rasterizer.GetScheduler();
    // The port's own EOP path (liverpool.cpp:1856-1929): OnFence marks the GPU-fence point the
    // label orders; the write itself is deferred behind the current tick and the flush submits
    // the in-flight work the game may wait on. The EOP packet's data_sel (Data32Low, value 0)
    // and int_sel (None) reproduce the guest's packet shape: only the label dword is written,
    // no GfxEop interrupt is raised from the pass functions (their 0x86eb60 calls pass
    // int_sel=0). The frame fence 0x90e30 does not interrupt either: its third argument to
    // 0x86eb60 is data_sel 2 (Data64), int_sel stays None (REA, N2-94).
    rasterizer.OnFence();
    const u64 address = label.address;
    scheduler.DeferPriorityOperation([address] {
        u32 value = 0;
        if (!Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(address), &value,
                                                       sizeof(value), 2)) {
            LOG_WARNING(Render_Vulkan, "sotc native-pass: EOP label write failed at %#llx",
                        static_cast<unsigned long long>(address));
        }
    });
    // The submit runs where the stream is recorded (the command processor or the recording
    // thread), never on the calling thread while the recorder's producer is another one.
    if (scheduler.HasRecordThread()) {
        scheduler.RunAfterRecorded([] {});
    } else {
        scheduler.Flush();
    }
    // The WAIT (0x86edc0, function 5 NotEqual vs wait_mask) completes immediately: the native
    // recording is sequential, so everything the label ordered is already ordered for the host
    // queue, and the mirror above already carries the EOP value.
    return true;
}

// -- Pass (dynamic rendering) -----------------------------------------------------------------

void Pass::Begin(vk::CommandBuffer cmdbuf, const PassAttachments& attachments) const {
    vk::RenderingAttachmentInfo color[8]{};
    for (u32 i = 0; i < attachments.color_count; ++i) {
        color[i].imageView = attachments.color[i];
        color[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color[i].loadOp = attachments.color_load;
        color[i].storeOp = attachments.color_store;
        color[i].clearValue.color.float32[0] = attachments.clear_color[i][0];
        color[i].clearValue.color.float32[1] = attachments.clear_color[i][1];
        color[i].clearValue.color.float32[2] = attachments.clear_color[i][2];
        color[i].clearValue.color.float32[3] = attachments.clear_color[i][3];
    }
    vk::RenderingAttachmentInfo depth{};
    depth.imageView = attachments.depth;
    depth.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    depth.loadOp = attachments.depth_load;
    depth.storeOp = attachments.depth_store;
    depth.clearValue.depthStencil.depth = attachments.clear_depth;
    depth.clearValue.depthStencil.stencil = attachments.clear_stencil;

    vk::RenderingInfo info{};
    info.renderArea = vk::Rect2D{{0, 0}, {attachments.width, attachments.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = attachments.color_count;
    info.pColorAttachments = color;
    if (attachments.depth) {
        info.pDepthAttachment = &depth;
    }
    cmdbuf.beginRendering(info);
}

void Pass::End(vk::CommandBuffer cmdbuf) const {
    cmdbuf.endRendering();
}

bool Pass::ClearDepth(const TargetSet& set, float value, u64 cb) const {
    if (!set.depth.active) {
        return false;
    }
    if (!presenter) {
        return false;
    }
    if (cb != 0 && StreamOps::Enabled()) {
        // Gap N2-64/N2-65: the texture-cache resolution and the recording both run where the
        // draws are recorded, when the command processor reaches this point of the stream.
        const Pass* self = this;
        return StreamOps::Emit(cb, [self, set, value] {
            if (!self->ClearDepthNow(set, value, /*on_stream=*/true)) {
                std::fprintf(stderr, "SOTC RENDER native: stream depth clear z=%#llx unresolved\n",
                             static_cast<unsigned long long>(set.depth_buffer.DepthAddress()));
            }
        });
    }
    return ClearDepthNow(set, value, /*on_stream=*/false);
}

bool Pass::ClearDepthNow(const TargetSet& set, float value, bool on_stream) const {
    // The exact ImageDesc the depth attachment resolves through (ResolveViews' depth branch),
    // so the clear lands in the image the frame's later draws see as the attachment.
    AmdGpu::CbDbExtent extent{};
    extent.raw = set.depth_extent_hint;
    const AmdGpu::DepthControl ctl{};
    VideoCore::TextureCache::ImageDesc desc{set.depth_buffer, set.depth_view, ctl,
                                            set.depth_htile, extent};
    VideoCore::TextureCache& cache = presenter->GetRasterizer().GetTextureCache();
    const auto texture_lock = cache.BatchLock();
    const auto image_id = cache.FindImage(desc);
    if (!image_id) {
        return false;
    }
    (void)cache.FindDepthTarget(image_id, desc); // registers the view usage like ResolveViews
    auto& image = cache.GetImage(image_id);
    // Gap N2-58: the guest range the clear makes GPU-modified, to match against the guest data
    // corruption (Oodle LZ / UI hashes) and the page-tracking faults of run probe-passonly-23.
    static const bool verbose = [] {
        const char* e = std::getenv("SOTC_RENDER_VERBOSE");
        return e && e[0] == '1';
    }();
    if (verbose) {
        std::fprintf(stderr,
                     "SOTC RENDER native: ClearDepth z_read=%#llx z_write=%#llx htile=%#llx "
                     "image=[%#llx,+%#x) %ux%u\n",
                     static_cast<unsigned long long>(set.depth_buffer.DepthAddress()),
                     static_cast<unsigned long long>(set.depth_buffer.DepthWriteAddress()),
                     static_cast<unsigned long long>(set.depth_htile),
                     static_cast<unsigned long long>(image.info.guest_address),
                     image.info.guest_size, image.info.size.width, image.info.size.height);
    }
    image.binding.is_target = 1u;

    const auto& range = desc.view_info.range;
    const vk::ImageAspectFlags mask =
        image.aspect_mask & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil);
    if (mask == vk::ImageAspectFlags{}) {
        return false;
    }
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = mask,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    vk::ClearDepthStencilValue clear{};
    clear.depth = value;
    clear.stencil = 0; // the guest stages 0 for stencil in both clear paths
    const vk::ImageLayout dst_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    // The clear must sit in frame order between the draws recorded before it and after it (the
    // cascade clears run between the shadow jobs, the touches between the caster and the main
    // scene). With a recording thread the native draws are recorded on the RECORDING thread
    // (Draw::IssueFromRegs -> Scheduler::HoldRecorder -> the recorder stream), so recording the
    // clear through Scheduler::RecordDeferred put it on the producer's own command buffer, out of
    // that order: the image barrier and the clear landed in a different buffer from the draws
    // that share the image, and the driver faulted on the reordered state (run
    // probe-native-fix5.log, nvoglv64.dll+0xe1373a). The clear is handed to the stream through
    // the same hold a native draw uses, becoming the next operation the recording thread runs
    // after everything queued so far.
    Vulkan::Scheduler& scheduler = presenter->GetRasterizer().GetScheduler();
    const vk::Image handle = image.GetImage();
    auto record_clear = [handle, vk_range, clear, dst_layout, &scheduler](vk::CommandBuffer cmdbuf) {
        // The image transition is illegal inside a rendering scope that may use the image, and the
        // scope in flight belongs to whatever drew last. Close it, clear, reopen the same scope
        // (BeginRendering's load ops re-establish the attachments; the scheduler emits no metadata
        // passes, so eLoad layouts are always legal).
        const bool reopen = scheduler.IsRendering();
        const Vulkan::RenderState saved = scheduler.GetRenderState();
        if (reopen) {
            scheduler.EndRendering();
        }
        const vk::ImageMemoryBarrier2 to_transfer = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = handle,
            .subresourceRange = vk_range,
        };
        const vk::ImageMemoryBarrier2 to_attachment = {
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = dst_layout,
            .image = handle,
            .subresourceRange = vk_range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_transfer,
        });
        cmdbuf.clearDepthStencilImage(handle, vk::ImageLayout::eTransferDstOptimal, clear,
                                      vk_range);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_attachment,
        });
        if (reopen) {
            scheduler.BeginRendering(saved);
        }
    };
    // SOTC_NATIVE_CLEAR_RECORD=0 (bisection, gap N2-55): every guest-side effect of the native
    // clear stays (the caller's caches and fence), but no Vulkan command is recorded and the image
    // state is left untouched. The guest-rect fallback (SOTC_NATIVE_CLEAR=0) cannot split this: the
    // guest 0x93690 body crashes when called from the native pass body.
    static const bool record = [] {
        const char* e = std::getenv("SOTC_NATIVE_CLEAR_RECORD");
        return !e || e[0] != '0';
    }();
    if (!record) {
        return true;
    }
    // Inside a hold the stream is the batch already open on this thread, recorded through
    // Scheduler::CommandBuffer(); outside one, the hold opens a batch and Scheduler::CommandBuffer()
    // hands back the same producer view the recorder replaces with the real buffer at replay.
    if (on_stream) {
        // Already on the thread that owns the stream (the command processor's draw thread):
        // record like a draw does.
        record_clear(scheduler.CommandBuffer());
    } else {
        scheduler.HoldRecorder([record_clear, &scheduler]() {
            record_clear(scheduler.CommandBuffer());
        });
    }
    // The layout the clear leaves the image in; the commands above are queued ahead of the next
    // draw into this image, which reads this state when it prepares the attachment.
    image.backing->state.layout = dst_layout;
    // sotc: MarkGpuModified moves the mip-tail epoch on the first write (Runtime::ClearImage).
    VideoCore::MarkGpuModified(image);
    image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Pass::SetViewportScissor(vk::CommandBuffer cmdbuf, const State::Registers& regs) const {
    const auto& vp = regs.viewport;
    vk::Viewport viewport{vp.x, vp.y, vp.w, vp.h, vp.min_depth, vp.max_depth};
    cmdbuf.setViewport(0, viewport);

    const auto& sc = regs.scissor;
    vk::Rect2D scissor{{sc.x, sc.y}, {sc.w, sc.h}};
    cmdbuf.setScissor(0, scissor);
}

void Pass::SetClearValues(const State::Registers& regs) const {
    // Clear values are carried on PassAttachments and consumed by Begin()'s load ops. This helper
    // exists so a caller can stage the depth clear value the guest wrote (DB_DEPTH_CLEAR, 0x869f50)
    // into an attachments struct; see Native::Renderer.
    (void)regs.db_depth_clear;
}

bool Pass::ResolveViews(TargetSet& set) const {
    // The port's texture cache is reached the way the rest of video_core reaches it: the
    // gnmdriver-owned presenter singleton (vk_presenter.h: GetRasterizer -> GetTextureCache).
    if (!presenter) {
        return false;
    }
    VideoCore::TextureCache& cache = presenter->GetRasterizer().GetTextureCache();
    const auto texture_lock = cache.BatchLock(); // one lock for the whole RT set, as PrepareRenderState

    for (u32 i = 0; i < set.color_count; ++i) {
        TargetAttachment& att = set.color[i];
        if (!att.active) {
            continue;
        }
        AmdGpu::CbDbExtent extent{};
        extent.raw = set.color_extent_hint[i];
        // The exact ImageDesc Rasterizer::PrepareRenderState builds for a colour target
        // (vk_rasterizer.cpp: ImageDesc{col_buf, DrawCbExtent()[cb]} -> FindImage ->
        // FindRenderTarget). ImageDesc copies both decode outputs, so the stack buffer is fine.
        VideoCore::TextureCache::ImageDesc desc{set.color_buffer[i], extent};
        const auto image_id = cache.FindImage(desc);
        if (!image_id) {
            return false;
        }
        auto& view = cache.FindRenderTarget(image_id, desc);
        auto& image = cache.GetImage(image_id);
        image.binding.is_target = 1u;
        att.view = *view.image_view;
        att.width = extent.Valid() ? extent.width : set.color_buffer[i].Pitch();
        att.height = extent.Valid() ? extent.height : set.color_buffer[i].Height();
    }

    if (set.depth.active) {
        AmdGpu::CbDbExtent extent{};
        extent.raw = set.depth_extent_hint;
        // DepthControl drives the depth view's storage/read-only split; the RT binder runs before
        // the pass writes DB_DEPTH_CONTROL, so the pass context's cached value (pass+0x11c,
        // written by 0x93960:49) is the closest state available (0 on the first bind = read-only).
        const AmdGpu::DepthControl ctl{};
        VideoCore::TextureCache::ImageDesc desc{set.depth_buffer, set.depth_view, ctl,
                                                set.depth_htile, extent};
        const auto image_id = cache.FindImage(desc);
        if (!image_id) {
            return false;
        }
        auto& view = cache.FindDepthTarget(image_id, desc);
        set.depth.view = *view.image_view;
    }
    return true;
}

void Pass::StageDepthClear(TargetSet& set, float depth) const {
    // 0x94600 co-emits DB_DEPTH_CLEAR = 0.0 when it binds the depth block (00094600.c:68,
    // 0x869f50 reg 0xb) and restores 1.0 after its clear draw (line 90). The native mirror
    // folds the staged clear value into the depth attachment instead: the clear draw itself is
    // the guest's fullscreen rect, so the value lands via the depth load-op, not a PM4 register.
    set.clear_depth = depth;
}

void Pass::SetPassViewportScissor(vk::CommandBuffer cmdbuf, u32 w, u32 h) const {
    // 0x85de40(0x3f000000, 0x3f000000, cb, 0, 0, w, h) as the pass functions call it
    // (00093960.c:59, 00094600.c:54, 00093ef0.c:81): viewport (0,0,w,h) with the depth range
    // [0.5, 0.5] the two 0x3f000000 float arguments carry, scissor (0,0,w,h) (the window
    // scissor 0x86a010 derives from the same dims inside 0x85de40).
    State::Registers regs;
    regs.viewport = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.5f, 0.5f};
    regs.scissor = {0, 0, w, h};
    SetViewportScissor(cmdbuf, regs);
}

} // namespace Native
