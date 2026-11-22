// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_state: the register state machine. It holds the GPU state
// the game's pass/draw writers would set, fed from the game's own context fields
// (gpu/native/guest_bridge.h) instead of PM4 SET_*_REG packets. The field set and the guest->state
// mapping are the emitter table of docs/engine/native-render-map.md §6.
//
// This class only tracks state and computes a change mask; the Vulkan calls that consume it live
// in native_draw / native_pass. Keeping it separate is what lets the divergence checker
// (native/shadow.h) compare "state the emulated path would have" against "state we issued".
#pragma once

#include "common/types.h"
#include "native/native_device.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Native {

/// The dynamic GPU state of one draw. Bit i of `dirty` means field i changed since the last bind.
class State {
public:
    enum Dirty : u32 {
        DirtyColorMask = 1u << 0,   // CB_TARGET_MASK      (0x869990, reg 0x8e)
        DirtyBlend = 1u << 1,       // CB_BLENDn_CONTROL   (0x869a50, reg 0x1e0+i)
        DirtyColorControl = 1u << 2,// CB_COLOR_CONTROL    (0x869ce0, reg 0x202)
        DirtyDepthControl = 1u << 3,// DB_DEPTH_CONTROL    (0x869d60, reg 0x200)
        DirtyStencilControl = 1u << 4, // DB_STENCIL_CONTROL (0x869dc0, reg 0x10b)
        DirtyStencilRef = 1u << 5,  // DB_STENCILREFMASK   (0x869bb0, reg 0x10c/0x10d)
        DirtyScMode = 1u << 6,      // PA_SU_SC_MODE_CNTL  (0x86e1a0, reg 0x205)
        DirtyDepthBias = 1u << 7,   // PA_SU_POLY_OFFSET   (0x86e410/0x86e480, reg 0x2e0/0x2e2)
        DirtyViewport = 1u << 8,    // PA_SC_VPORT         (0x86d7b0)
        DirtyScissor = 1u << 9,     // PA_SC_SCREEN_SCISSOR (0x86d950)
        DirtyPrimitive = 1u << 10,  // VGT_PRIMITIVE_TYPE  (0x86a900, reg 0x242)
        DirtyIndexType = 1u << 11,  // INDEX_TYPE          (0x86bed0, reg 0x2a)
        DirtyDepthClear = 1u << 12, // DB_DEPTH_CLEAR      (0x869f50, reg 0xb)
        DirtyTargets = 1u << 13,    // render-target set   (0x92140 / 0x85cbc0)
        DirtyPipeline = 1u << 14,   // material/shader bind (0x85fc10)
        DirtyUserData = 1u << 15,   // user SGPR block     (0x86d4c0/0x86d5e0)
        DirtyPredication = 1u << 16,// SET_PREDICATION     (0x86e770/0x86e800)
    };

    void Reset() {
        dirty = ~0u;
    }

    u32 DirtyMask() const noexcept {
        return dirty;
    }
    void ClearDirty(u32 mask = ~0u) noexcept {
        dirty &= ~mask;
    }
    void MarkDirty(u32 mask) noexcept {
        dirty |= mask;
    }

    // --- Register values (mirrors of the guest's SH/CX/UC register state) ---
    struct Registers {
        u32 cb_target_mask{0xffffffff};        // reg 0x8e
        u32 cb_blend[8]{};                     // reg 0x1e0+i
        u32 cb_color_control{0xcc};            // reg 0x202
        u32 db_depth_control{};                // reg 0x200
        u32 db_stencil_control{};              // reg 0x10b
        u32 db_stencil_ref_front{};            // reg 0x10c
        u32 db_stencil_ref_back{};             // reg 0x10d
        u32 pa_su_sc_mode_cntl{};              // reg 0x205
        float poly_offset_scale{};             // reg 0x2e0
        float poly_offset_offset{};            // reg 0x2e2
        float db_depth_clear{1.0f};            // reg 0xb
        struct {
            float x, y, w, h, min_depth{0.0f}, max_depth{1.0f};
        } viewport{};
        struct {
            s32 x, y; u32 w, h;
        } scissor{};
        u32 primitive_type{0x11};              // VGT_PRIMITIVE_TYPE (0x11 = RectList)
        u32 index_type{0};                     // INDEX_TYPE
        u32 draw_kind{0};                      // cb+0xc548
    };

    Registers regs;
    u32 draw_kind() const noexcept {
        return regs.draw_kind;
    }

private:
    u32 dirty{~0u};
};

} // namespace Native
