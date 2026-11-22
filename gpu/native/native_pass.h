// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_pass: render-pass structure (colour/depth attachments,
// load/store, viewport/scissor). Maps the pass functions 0x93960 / 0x94600 / 0x93ef0
// (docs/engine/native-render-map.md §5) to Vulkan dynamic rendering.
//
// Phase 2.5 (native-rt) adds the render-target *bind* layer: the guest's 0x92140 render-target
// set binder resolves 9 pass-target descriptors to the embedded CB/DB register blocks and emits
// PM4 CB_COLORn / DB register runs. The native mirror decodes those blocks into attachments
// (formats via the port's own LiverpoolToVK decoder, views via the port's texture cache) and
// derives the load/store ops — the "attachment binding" of docs/engine/native-render-map-bind.md
// §3/§3.5. The decode helpers are proven against out/re/decomp/0009/00092140.c, 0085/0085cbc0.c,
// 0086/0086a1a0.c and 0086/0086a290.c (see native-render-gaps.md N2-7).
//
// Phase 2.6 (native-pass) adds the EOP/WAIT bridge: the pass functions end with
// 0x86d210 (inline-data label slot) + 0x86eb60 (EVENT_WRITE_EOP, packet 0xC0044700) +
// 0x86edc0 (WAIT_REG_MEM function=5 "not equal" on that label) — the guest's GPU fence between
// render phases. The native mirror (Eop) writes the label through the port's own EOP path
// (Rasterizer::OnFence + DeferPriorityOperation(WriteFenceValue) + Flush), matching
// liverpool.cpp:1856-1929, and updates the label mirror the WAIT polls (the native path's
// wait always finds the fence signalled, as the sequential native recording guarantees).
#pragma once

#include "common/types.h"
#include "native/native_device.h"
#include "native/native_state.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Native {

/// One render pass: up to 8 colour attachments plus an optional depth-stencil attachment.
struct PassAttachments {
    u32 color_count{0};
    vk::ImageView color[8]{};
    vk::ImageView depth{};
    u32 width{0};
    u32 height{0};
    vk::AttachmentLoadOp color_load{vk::AttachmentLoadOp::eLoad};
    vk::AttachmentLoadOp depth_load{vk::AttachmentLoadOp::eLoad};
    vk::AttachmentStoreOp color_store{vk::AttachmentStoreOp::eStore};
    vk::AttachmentStoreOp depth_store{vk::AttachmentStoreOp::eStore};
    bool depth_read_only{false};
    float clear_color[8][4]{};
    float clear_depth{1.0f};
    u32 clear_stencil{0};
};

/// One resolved attachment of a 0x92140 render-target set.
struct TargetAttachment {
    bool active{false};    ///< descriptor present (type byte 1/3), block format words valid
    vk::ImageView view{};  ///< resolved via the port's texture cache (commit path only)
    vk::Format format{vk::Format::eUndefined};
    u32 width{0};
    u32 height{0};
};

/// The native mirror of one 0x92140 render-target set (bind-map §3.1). Everything here is derived
/// from the guest descriptors without touching guest or cache state; the guest-visible dirty
/// caches are only written on the native commit path (see Native::RtSetNative).
struct TargetSet {
    TargetAttachment color[8];
    TargetAttachment depth;
    /// Decoded register blocks, kept for ResolveViews (which rebuilds the port's ImageDesc/ImageView
    /// from the exact register image Rasterizer::PrepareRenderState consumes).
    AmdGpu::ColorBuffer color_buffer[8];
    AmdGpu::DepthBuffer depth_buffer;
    AmdGpu::DepthView depth_view;
    VAddr depth_htile{0};
    /// Trailing NOP-hint dwords (the port's CbDbExtent), per slot / depth.
    u32 color_extent_hint[8]{};
    u32 depth_extent_hint{0};
    u32 color_count{0};    ///< highest active colour slot + 1
    u32 width{0};          ///< first non-null descriptor +0x198 (0x96f50)
    u32 height{0};         ///< first non-null descriptor +0x19c (0x96f60)
    u32 selector{0};       ///< descriptor qword [9] bit0: +0xe4 vs +0x124 block
    u32 rt_config_code{0}; ///< encoded into pass+0x8 bits 3-4 (0x92140 step 5)
    /// Bit i: the guest would (re)emit colour slot i (dirty bit i of pass+0x10, or its cached
    /// block pointer at pass+0x48+8i changed). Bit 8: depth rebind (pass+0x11 bit0 aliased).
    u32 rebind_mask{0};
    /// The cached CB_TARGET_MASK (pass+0x1ac, written by 0x93960:38). Not read by 0x92140; kept
    /// for the trace and for the Phase 2.6 pass functions (bind-map §5: the "+0xac" in the task
    /// brief is a typo for +0x1ac).
    u32 target_mask_cache{0};
    /// Load/store ops of the binder itself. The binder never clears (bind-map §3.5): clears are
    /// the callers' explicit fullscreen draws (0x93690/0x932a0, Phase 2.6) and the guest always
    /// writes through, so the RT-set bind maps to load = eLoad / store = eStore for every
    /// attachment. Depth read-only comes from DB_DEPTH_VIEW (depth block dword 8, bits 22/23).
    vk::AttachmentLoadOp load{vk::AttachmentLoadOp::eLoad};
    vk::AttachmentStoreOp store{vk::AttachmentStoreOp::eStore};
    bool depth_read_only{false}; ///< DB_DEPTH_VIEW z_read_only|stencil_read_only (dword 8 bits 22/23)
    float clear_depth{1.0f}; ///< the DB_DEPTH_CLEAR value the guest stages at bind (0.0 co-emitted)
    u32 clear_stencil{0};
};

/// Decode helpers for the pass-target descriptor blocks (bind-map §3.2-3.3, gap N2-7). Layouts
/// proven: the 8-qword colour block at descriptor +0xe4/+0x124 IS the port's AmdGpu::ColorBuffer
/// (16 words; the guest's 0x86a1a0 emits words 0..13 verbatim as CB_COLORn_BASE.. and word 15 as
/// the trailing NOP-hint dword = the port's CbDbExtent), and the 13-dword depth block at +0x164
/// maps to AmdGpu::DepthBuffer words (DB_Z_INFO..DB_DEPTH_SLICE) + DB_DEPTH_VIEW (dword 8) +
/// DB_HTILE_DATA_BASE (dword 9), with dword 12 as the DB NOP-hint extent.
namespace Rt {

/// Copy a guest 8-qword colour block into the port's ColorBuffer decode. Returns false (leaving
/// `out` zeroed) when the block words are invalid the way the guest's 0x87eda0 validity check is
/// (no address / no format) — the guest then emits CB_COLORn_INFO = 0, i.e. the slot is disabled.
bool DecodeColour(const u64* block, AmdGpu::ColorBuffer& out, u32& extent_hint);

/// Copy a guest depth block (13 dwords) into the port's DepthBuffer/DepthView decode.
/// `htile` receives the full htile address (word 9 << 8, as liverpool's GetAddress() yields).
bool DecodeDepth(const u64* block, AmdGpu::DepthBuffer& out, AmdGpu::DepthView& view,
                 VAddr& htile, u32& extent_hint);

/// CB_COLORn_INFO (format + number_type + comp_swap) -> VkFormat, fully sourced from the port's
/// own decoder (ColorBuffer::GetDataFmt/GetNumberFmt -> LiverpoolToVK::SurfaceFormat). Returns
/// eUndefined for a format the port cannot name (caller disables the slot / chains to guest).
vk::Format ColourVkFormat(const AmdGpu::ColorBuffer& buffer);

/// DB_Z_INFO format class (2 bits: 0 invalid, 1 Z16, 3 Z32F) + stencil -> VkFormat via the port's
/// LiverpoolToVK::DepthFormat.
vk::Format DepthVkFormat(const AmdGpu::DepthBuffer& buffer);

/// The EOP/WAIT bridge of the pass functions (Phase 2.6). The guest pattern
/// (out/re/decomp/0086/0086d210.c + 0086eb60.c + 0086edc0.c, proven):
///   slot = 0x86d210(cb, size, 3)        -> an aligned inline-data block appended to the PM4
///                                          stream, its first dword set to 0 (the guest writes
///                                          the marker dword into its own command buffer area);
///   0x86eb60(cb, event, sub, slot, ...) -> EVENT_WRITE_EOP (header 0xC0044700): the CP writes
///                                          data_sel=1 (Data32Low) = 0 to slot when everything
///                                          before it completed;
///   0x86edc0(cb, slot, 0xffffffff, 5, 1)-> WAIT_REG_MEM (header 0xC0053c00, function 5 =
///                                          NotEqual): spin until the label dword != 0xffffffff.
/// The native mirror keeps the guest's label mirror (a 4-byte guest-memory word) coherent and
/// routes the "GPU finished everything before this point" event through the port's own EOP path.
namespace Eop {

/// One pending label of the native pass: the guest address the label dword lives at and the
/// value the WAIT polls against (0xffffffff = "not signalled"). The slot the guest reserved
/// with 0x86d210 sits inside the guest's own command-buffer allocation, so the address is a
/// plain guest (host) address.
struct Label {
    u64 address{0};   ///< guest VA of the 4-byte label dword (the 0x86d210 slot)
    u32 wait_mask{0}; ///< the 0x86edc0 reference value (0xffffffff in both pass functions)
};

/// Native replacement of the 0x86d210 label allocation: asks the guest's own allocator (the GNM
/// image reached through GuestImageBase, engine-side as in the native cull loop) for a label of
/// `size_dwords` dwords in `cb`'s command buffer. Returns 0 when the image is not mapped.
u64 ReserveLabel(u64 cb, u32 size_dwords);

/// Native replacement of the 0x86d210 + 0x86eb60 pair: reserves nothing in the guest stream
/// (the native path owns the command recording) but still zeroes the guest's label dword —
/// the guest body writes `*slot = 0` unconditionally, and the slot address is what the
/// 0x86edc0 WAIT would poll. Must run below the commit point (a guest-memory write).
void SignalLabel(const Label& label);

/// Native replacement of the 0x86edc0 WAIT on `label`: marks the port's GPU-fence point the
/// same way the emulated EOP does (liverpool.cpp:1856-1929: OnFence, then the deferred
/// WriteFenceValue, then Flush) and then completes the fence immediately — the sequential
/// native recording means everything the label ordered is already ordered for the host queue.
/// Returns false when the port's rasterizer is unreachable (caller chains to the guest body).
/// `cb`: as for Pass::ClearDepth, the fence is then placed in the stream after the operations
/// queued before it instead of touching the scheduler from the calling thread.
bool WaitLabel(const Label& label, u64 cb = 0);

} // namespace Eop

} // namespace Rt

class Pass {
public:
    explicit Pass(const Device& device) : device{device} {}

    /// Begin a pass with the given attachments (VK_KHR_dynamic_rendering). Replaces the RT/depth
    /// bind + clear sequence of 0x92140 / 0x85cbc0 / 0x86a290 / 0x869f50.
    void Begin(vk::CommandBuffer cmdbuf, const PassAttachments& attachments) const;

    /// End the pass (vkCmdEndRendering).
    void End(vk::CommandBuffer cmdbuf) const;

    /// The native depth clear (Phase 2.6/2.8, the cutover's first emission): replaces the two
    /// 0x93690 fullscreen draws whose only GPU effect is a full-surface store of the staged
    /// DB_DEPTH_CLEAR value — the shadow-cascade clears (0x932a0 kind 2, 000cc9b0.c:77) and the
    /// 0x93ef0 depth-touch clears (00093ef0.c:85). `set` is the depth attachment the pass
    /// derived; `value` is the float the guest staged. No pipeline is involved: the clear is a
    /// vkCmdClearDepthStencilImage through the port's own layout transition. Mutates texture-cache
    /// state (FindImage / FindDepthTarget / layout), so it runs at the commit point only.
    /// Returns false when the attachment cannot be resolved (caller chains to the guest).
    ///
    /// `cb` (the guest command buffer object of the pass, pass[0]): when given and
    /// SOTC_NATIVE_STREAM_OPS is on, the clear is queued as a stream op (native_stream_ops.h) and
    /// runs on the command processor's draw thread when the stream reaches this point; the return
    /// value then only says the op was placed (a later resolution failure is traced there).
    bool ClearDepth(const TargetSet& set, float value, u64 cb = 0) const;

private:
    /// The clear itself: texture-cache resolution and recording. `on_stream`: running as a stream
    /// op on the thread that records the draws (record directly), else the old immediate path
    /// (Scheduler::HoldRecorder from the calling thread).
    bool ClearDepthNow(const TargetSet& set, float value, bool on_stream) const;

public:

    /// Dynamic viewport + scissor + depth range. Maps 0x85de40 (PA_SC_VPORT/SCREEN_SCISSOR/VTE/Z).
    void SetViewportScissor(vk::CommandBuffer cmdbuf, const State::Registers& regs) const;

    /// Colour/depth clear as load-op clear values (replaces the fullscreen clear draw of 0x93690
    /// in the clear pass 0x94600 and the depth clear of 0x869f50).
    void SetClearValues(const State::Registers& regs) const;

    /// Resolve the attachment VkImageViews of a derived TargetSet through the port's texture
    /// cache — the exact ImageDesc{ColorBuffer/DepthBuffer} path Rasterizer::PrepareRenderState
    /// uses (vk_rasterizer.cpp: colour ImageDesc{buffer, CbDbExtent} -> FindImage ->
    /// FindRenderTarget; depth -> FindImage -> FindDepthTarget). This mutates cache state
    /// (registers images, marks targets written, tracks pages), so it runs only on the native
    /// commit path. Returns false when the rasterizer/texture cache is not reachable or a target
    /// cannot be resolved (caller chains to the guest body).
    bool ResolveViews(TargetSet& set) const;

    /// Stage the load-op clear values of a pass descriptor (Phase 2.6): 0x94600 (pass B) co-emits
    /// DB_DEPTH_CLEAR = 0.0 before its clear draw and restores 1.0 after (00094600.c:68/90); the
    /// native mirror folds that into the depth attachment's clear value. The colour attachments
    /// never load-op clear (bind-map §3.5) — the guest's colour clear is the fullscreen draw.
    void StageDepthClear(TargetSet& set, float depth) const;

    /// The dynamic viewport/scissor of one pass, from the pass descriptor's +0x120 (pass A) or
    /// +0x194 (pass B / depth object) dims. Maps 0x85de40(z_lo=0.5f, z_hi=0.5f, cb, 0, 0, w, h)
    /// (0x93960:59, 0x94600:54, 0x93ef0:81): viewport (0,0,w,h) with depth range [0.5, 0.5] and
    /// scissor (0,0,w,h). Dynamic state, safe above the commit point (records nothing until the
    /// command buffer records it).
    void SetPassViewportScissor(vk::CommandBuffer cmdbuf, u32 w, u32 h) const;

private:
    const Device& device;
};

} // namespace Native
