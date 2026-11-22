// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. shadow: the divergence checker. For every draw, it compares the
// state the native backend is about to issue against the state the emulated (PM4) path would have
// produced, and counts differences. This is the Phase 1/2 acceptance gate (the plan's "shadow state
// equivalence"): zero divergences required.
//
// It is cheap when disabled (a single branch on `active`), so it can stay compiled in.
#pragma once

#include <atomic>

#include "common/types.h"
#include "native/native_state.h"

namespace Native {

/// One draw's worth of comparable state.
struct DrawState {
    u32 draw_kind{0};
    u32 primitive_type{0};
    u32 index_type{0};
    u32 cb_target_mask{0};
    u32 cb_color_control{0};
    u32 db_depth_control{0};
    u32 pa_su_sc_mode_cntl{0};
    u32 index_count{0};
    u32 instance_count{0};
    u32 first_index{0};
    VAddr index_base{0};
    VAddr indirect_args{0};
    bool predicated{false};
    VAddr predicate_addr{0};
};

class Shadow {
public:
    /// Off unless SOTC_RENDER_SHADOW=1.
    static bool Active();

    /// Compare the native draw state against the emulated one; bump the divergence counter and
    /// (throttled) log the first fields that differ.
    static void Compare(const DrawState& native, const DrawState& emulated, u64 draw_index);

    /// Divergences so far (0 required at every gate).
    static u64 Divergences() {
        return divergences.load(std::memory_order_relaxed);
    }
    static u64 Draws() {
        return draws.load(std::memory_order_relaxed);
    }
    static void Reset();

private:
    static std::atomic<u64> divergences;
    static std::atomic<u64> draws;
};

} // namespace Native
