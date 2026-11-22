// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. shadow implementation.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "native/shadow.h"

namespace Native {

std::atomic<u64> Shadow::divergences{0};
std::atomic<u64> Shadow::draws{0};

bool Shadow::Active() {
    static const bool active = [] {
        const char* v = std::getenv("SOTC_RENDER_SHADOW");
        return v && v[0] == '1';
    }();
    return active;
}

namespace {
template <typename T>
bool FieldDiff(const char* name, const T& a, const T& b, u64 draw_index, bool& any) {
    if (a == b) {
        return false;
    }
    any = true;
    if (draw_index < 8) { // throttled: only the first few draws
        std::fprintf(stderr, "SOTC RENDER DIVERGENCE draw %llu: %s native=%llu emulated=%llu\n",
                     static_cast<unsigned long long>(draw_index), name,
                     static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    }
    return true;
}
} // namespace

void Shadow::Compare(const DrawState& native, const DrawState& emulated, u64 draw_index) {
    if (!Active()) {
        return;
    }
    draws.fetch_add(1, std::memory_order_relaxed);
    bool any = false;
    FieldDiff("draw_kind", native.draw_kind, emulated.draw_kind, draw_index, any);
    FieldDiff("primitive_type", native.primitive_type, emulated.primitive_type, draw_index, any);
    FieldDiff("index_type", native.index_type, emulated.index_type, draw_index, any);
    FieldDiff("cb_target_mask", native.cb_target_mask, emulated.cb_target_mask, draw_index, any);
    FieldDiff("cb_color_control", native.cb_color_control, emulated.cb_color_control, draw_index,
              any);
    FieldDiff("db_depth_control", native.db_depth_control, emulated.db_depth_control, draw_index,
              any);
    FieldDiff("pa_su_sc_mode_cntl", native.pa_su_sc_mode_cntl, emulated.pa_su_sc_mode_cntl,
              draw_index, any);
    FieldDiff("index_count", native.index_count, emulated.index_count, draw_index, any);
    FieldDiff("instance_count", native.instance_count, emulated.instance_count, draw_index, any);
    FieldDiff("first_index", native.first_index, emulated.first_index, draw_index, any);
    FieldDiff("index_base", native.index_base, emulated.index_base, draw_index, any);
    FieldDiff("indirect_args", native.indirect_args, emulated.indirect_args, draw_index, any);
    FieldDiff("predicated", native.predicated, emulated.predicated, draw_index, any);
    FieldDiff("predicate_addr", native.predicate_addr, emulated.predicate_addr, draw_index, any);
    if (any) {
        divergences.fetch_add(1, std::memory_order_relaxed);
    }
}

void Shadow::Reset() {
    divergences.store(0, std::memory_order_relaxed);
    draws.store(0, std::memory_order_relaxed);
}

} // namespace Native
