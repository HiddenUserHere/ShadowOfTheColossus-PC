// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_clear_pass implementation: the colour strip 0x93690 draws in
// the engine's strip helpers, replayed as a rendering scope over the bound targets (see the
// header). The target decode is the RT binder's own (guest_bridge's descriptor offsets + Rt decode
// helpers), only without the binder's viewer-wide extent sanity test.
#include "native/native_clear_pass.h"

#include "native/guest_bridge.h"
#include "native/native_draw.h"
#include "native/native_renderer.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

// The gnmdriver-owned presenter singleton (declared the same way in native_pass.cpp).
extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Native {

namespace TargetDescOff {
constexpr u64 Type = 0x62;           // type byte: (b | 2) == 3 -> colour target (types 1/3)
constexpr u64 ColorBlock = 0xe4;     // 8-qword CB_COLORn block (== AmdGpu::ColorBuffer, N2-7)
constexpr u64 ColorBlockAlt = 0x124; // the alternate block (selector bit0 = 1)
constexpr u64 DepthBlock = 0x164;    // 13-dword DB block
constexpr u64 Width = 0x198;         // u32 surface width (0x96f50)
constexpr u64 Height = 0x19c;        // u32 surface height (0x96f60)
} // namespace TargetDescOff

bool DecodeTargets(const u64* targets, TargetSet& set) {
    using namespace Guest;
    set = {};
    if (targets == nullptr) {
        return false;
    }
    const u32 selector = static_cast<u32>(targets[9]) & 1;
    const u64 color_off = selector != 0 ? TargetDescOff::ColorBlockAlt : TargetDescOff::ColorBlock;
    u64 first_desc = 0;
    for (u32 i = 0; i < 8; ++i) {
        const u64 desc = targets[i];
        // Type gate (00092140.c:20 etc.): (type | 2) != 3 -> the slot resolves to NULL.
        if (desc == 0 || ((*reinterpret_cast<const u8*>(desc + TargetDescOff::Type) | 2) != 3)) {
            continue;
        }
        if (first_desc == 0) {
            first_desc = desc;
        }
        AmdGpu::ColorBuffer buffer{};
        u32 extent = 0;
        if (!Rt::DecodeColour(reinterpret_cast<const u64*>(desc + color_off), buffer, extent)) {
            continue; // invalid format words: the guest disables the slot (CB_COLORn_INFO = 0)
        }
        set.color_buffer[i] = buffer;
        set.color_extent_hint[i] = extent;
        set.color[i].format = Rt::ColourVkFormat(buffer);
        set.color[i].active = set.color[i].format != vk::Format::eUndefined;
        set.color_count = i + 1;
    }
    const u64 depth_desc = targets[8];
    if (depth_desc != 0) {
        AmdGpu::DepthBuffer db{};
        AmdGpu::DepthView view{};
        VAddr htile = 0;
        u32 extent = 0;
        if (Rt::DecodeDepth(reinterpret_cast<const u64*>(depth_desc + TargetDescOff::DepthBlock), db,
                            view, htile, extent)) {
            set.depth_buffer = db;
            set.depth_view = view;
            set.depth_htile = htile;
            set.depth_extent_hint = extent;
            set.depth.format = Rt::DepthVkFormat(db);
            set.depth_read_only = view.z_read_only != 0 && view.stencil_read_only != 0;
            set.depth.active = set.depth.format != vk::Format::eUndefined;
        }
        if (first_desc == 0) {
            first_desc = depth_desc;
        }
    }
    if (first_desc == 0) {
        return false;
    }
    set.selector = selector;
    set.width = *reinterpret_cast<const u32*>(first_desc + TargetDescOff::Width);
    set.height = *reinterpret_cast<const u32*>(first_desc + TargetDescOff::Height);
    return set.width != 0 && set.height != 0;
}

namespace {

/// The scope attachments of a resolved set: the colour slots and, when it is a writable depth
/// surface, the read-only depth range the strip's clear shader draws at (0.5). A read-only view is
/// left out so the strip's draws never have to write it.
PassAttachments ScopeAttachments(const TargetSet& set) {
    PassAttachments atts;
    atts.color_count = set.color_count;
    for (u32 i = 0; i < set.color_count; ++i) {
        atts.color[i] = set.color[i].view;
    }
    if (set.depth.active && !set.depth_read_only) {
        atts.depth = set.depth.view;
        atts.depth_load = vk::AttachmentLoadOp::eLoad;
    }
    atts.width = set.width;
    atts.height = set.height;
    atts.color_load = vk::AttachmentLoadOp::eLoad; // the strip blends over what is under it
    atts.color_store = vk::AttachmentStoreOp::eStore;
    atts.depth_store = vk::AttachmentStoreOp::eStore;
    return atts;
}

} // namespace

bool ReplayTargets(const TargetSet& set, const Vulkan::Pipeline* pipeline) {
    Renderer* renderer = Current();
    if (renderer == nullptr || presenter == nullptr) {
        return false;
    }
    TargetSet resolved = set;
    if (!renderer->Passes().ResolveViews(resolved)) {
        return false;
    }
    const PassAttachments atts = ScopeAttachments(resolved);
    if (atts.color_count == 0 && atts.depth == vk::ImageView{}) {
        return false;
    }
    const Pass& pass = renderer->Passes();
    const Draw& draw = renderer->Draws();
    // The strip is recorded between the draws the frame already queued, like every native draw:
    // the hold makes it the stream's next operation.
    renderer->Scheduler().HoldRecorder([&pass, &draw, atts, pipeline, renderer]() {
        const vk::CommandBuffer cmdbuf = renderer->Scheduler().CommandBuffer();
        pass.Begin(cmdbuf, atts);
        pass.SetPassViewportScissor(cmdbuf, atts.width, atts.height);
        if (pipeline != nullptr) {
            draw.BindPipeline(cmdbuf, *pipeline);
            draw.FullscreenRect(cmdbuf);
        }
        pass.End(cmdbuf);
    });
    return true;
}

} // namespace Native
