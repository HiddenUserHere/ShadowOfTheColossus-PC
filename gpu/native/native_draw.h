// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_draw: issues draws and dispatches. Each method corresponds to
// one of the game's draw objects, with the mapping from docs/engine/native-render-map.md §5:
//   0x93690  fullscreen rect        -> Draw(3)
//   0x94ec0  DrawIndexAuto           -> DrawIndexed/Draw
//   0x94d00  DrawIndexOffset         -> DrawIndexed
//   0x94dd0  DrawIndexOffset+inst    -> DrawIndexed(instances)
//   0x10add0 DrawIndirect            -> DrawIndirect
//   0xdb620  DrawIndexIndirect       -> DrawIndexedIndirect
//   0x86b9e0 DispatchDirect          -> Dispatch
#pragma once

#include "common/types.h"
#include "native/native_device.h"
#include "native/native_state.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Pipeline;
} // namespace Vulkan

namespace AmdGpu {
union Regs;
} // namespace AmdGpu

namespace Native {

/// sotc: the emission ladder of the native backend (SOTC_NATIVE_EMIT). On (default whenever a
/// native stage is selected) the hooks issue through the port's GPU-state path
/// (Draw::IssueFromRegs) instead of the per-stage recorders; "0" keeps the pre-cutover behaviour
/// the A/B gates compare against.
bool NativeEmitWanted();

class Draw {
public:
    explicit Draw(const Device& device) : device{device} {}

    /// Bind a resolved pipeline (from the port's pipeline cache, keyed by the guest's shader
    /// records). The native backend does not build pipelines itself: it reuses the recompiler's.
    void BindPipeline(vk::CommandBuffer cmdbuf, const Vulkan::Pipeline& pipeline) const;

    /// 0x93690: the fullscreen rect draw (primitive 0x11 RectList). Procedural fullscreen triangle.
    void FullscreenRect(vk::CommandBuffer cmdbuf) const;

    /// 0x94ec0: DrawIndexAuto, count vertices, no index buffer. first_vertex is the start-vertex
    /// constant 0x862510 stages into the shader's SGPR (Vulkan: BaseVertex of a non-indexed draw).
    void Auto(vk::CommandBuffer cmdbuf, u32 count, u32 instances = 1, u32 first_vertex = 0,
              u32 first_instance = 0) const;

    /// 0x94d00 / 0x94dd0: DrawIndexOffset (indexed, first index `first_index`).
    void IndexedOffset(vk::CommandBuffer cmdbuf, u32 index_count, u32 first_index,
                       u32 instances = 1, s32 vertex_offset = 0) const;

    /// 0x10add0: DrawIndirect.
    void Indirect(vk::CommandBuffer cmdbuf, vk::Buffer args, u64 offset, u32 stride = 16) const;

    /// 0xdb620: DrawIndexIndirect.
    void IndexedIndirect(vk::CommandBuffer cmdbuf, vk::Buffer args, u64 offset,
                         u32 stride = 20) const;

    /// 0x86b9e0: DispatchDirect (compute).
    void Dispatch(vk::CommandBuffer cmdbuf, u32 x, u32 y, u32 z) const;

    /// One draw from the guest's own state (the cutover's native issuance): a copy of the
    /// registers the draw object would have emitted plus the draw parameters, resolved here into
    /// a port GraphicsPipeline and issued through the command processor's own GPU-state path
    /// (target images + layout transitions, descriptors, user data, viewport/scissor, vertex and
    /// index binds: Rasterizer::DrawNative). Nothing is recorded when any link is missing; the
    /// caller then chains to the guest body.
        struct DrawRequest {
        /// The draw's register image: a per-call copy the caller fills (GuestRegs() copied, then
        /// the object's own fields). IssueFromRegs keeps it alive until the stream has recorded the
        /// draw, so it must stay on the caller's stack for the duration of the call.
        AmdGpu::Regs* regs{nullptr};
        const void* cb_extent{};     ///< AmdGpu::CbDbExtent[8] the colour targets used
        const void* db_extent{};     ///< AmdGpu::CbDbExtent the depth target used
        const Vulkan::Pipeline* pipeline{}; ///< resolved by a hook's own seam (null: look it up)
        VAddr args{};      ///< indirect: the args table (0 = a plain draw)
        u32 args_size{};   ///< indirect: 16 (DrawIndirect) or 20 (DrawIndexedIndirect)
        u32 num_indices{};
        bool fullscreen{}; ///< 0x93690: a 3-vertex triangle is drawn instead of num_indices
        bool is_indexed{};
        u32 index_offset{};
        u16 vertex_sgpr_offset{};
        u16 instance_sgpr_offset{};
    };
    /// Returns false when the draw cannot be named (no pipeline) or the renderer is down; the
    /// caller chains. Returns true only after the draw was accepted for issuance.
    bool IssueFromRegs(const DrawRequest& request) const;

    /// The register image the guest's frame is building (the draw thread's own copy). A draw
    /// object copies it into its own stack frame (DrawRequest::regs) and mutates that copy: guest
    /// threads share this renderer's Draw object, and a single scratch copy was being written by
    /// two of them at once (the camera pass and the GBuffer pass draw on two guest threads).
    AmdGpu::Regs& GuestRegs() const;

private:
    const Device& device;
};

} // namespace Native
