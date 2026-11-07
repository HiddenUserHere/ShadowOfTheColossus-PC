// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>
#include <vector>
#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

/// Host helper the walker calls to copy dynamic-index windows (see ReadConstDynamicWindowFlag).
void SrtCopyWindow(u32* dst, u64 src, u64 bytes);
using PFN_SrtCopyWindow = void (*)(u32*, u64, u64);

/// Fast path for clean walker reads (from srt_walker_clean_reads of Pink-shadPS4, luizgustavs):
/// pages where the fault handler served a clean read get a bit in clean_pages, and the walker
/// asks clean_load for loads in them instead of faulting again.
constexpr u64 SrtCleanPageLimit = 1ULL << 40;
struct SrtWalkerContext {
    u64* clean_pages;                                    // offset 0
    bool (*clean_load)(u64 address, u32 size, u64* out); // offset 8
};
const SrtWalkerContext* GetSrtWalkerContext();
void SetSrtCleanLoad(bool (*clean_load)(u64, u32, u64*));
void MarkSrtCleanPages(u64 address, u32 size);
/// sotc: keeps clean_pages equal to the pages currently read-protected (page manager), so
/// walker loads from pages no longer protected stop going through clean_load.
void SetSrtReadProtectedPages(u64 address, u64 size, bool read_protected);

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                            PFN_SrtCopyWindow /*copy_window*/,
                                            const SrtWalkerContext* /*context*/);

/// ReadConst flag marking a load whose dword index is only known at shader run time (e.g. a
/// loop counter). The SRT walker copies a window of the pointed-to memory into the flattened
/// buffer; the low bits hold the window's first dword and the index is clamped to its size.
constexpr u32 ReadConstDynamicWindowFlag = 1u << 31;
constexpr u32 ReadConstDynamicWindowDwords = 512;
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);
/// True when code is inside the generated SRT walkers.
bool IsSrtWalkerCode(const void* code);

// sotc: SOTC_CP_SRT_PLAN=2 (read once): RefreshFlatBuf runs the walker with SrtPlanTraceContext and
// SrtCopyWindowTraced, so every guest read it makes is recorded (Info::sotc_srt_trace). The reads are
// the same as without it; only the context's page test and the window copy are wrapped.
bool SrtPlanTraceEnabled();
const SrtWalkerContext* SrtPlanTraceContext();
void SrtCopyWindowTraced(u32* dst, u64 src, u64 bytes);
void SrtTraceReset();
/// Takes the reads recorded since the reset, without the ones inside [skip_begin, skip_end).
void SrtTraceTake(std::vector<std::pair<u64, u32>>& out, u64 skip_begin, u64 skip_end);

/// sotc: SOTC_CP_SRT_PLAN. One table level of a walker and the guest memory the walker reads from it.
/// The levels are listed parents first (the walk order). Built with the walker (flatten pass) and
/// evaluated by the CP precise wait (Liverpool::CpCollectSrtPlan).
struct SrtWalkNode {
    s32 parent{-1}; ///< the level that holds this level's pointer; -1: a root (user data)
    u32 ptr_dw{};   ///< a root: the user data dword of its pointer; else the dword offset of the pointer in the parent
    u32 extent{};   ///< bytes read from the table at its base (immediate and bounded dynamic offsets)
    u32 window{};   ///< 1: a dynamic window (ReadConstDynamicWindowDwords dwords) is copied from the base
};
constexpr u32 SrtWalkMaxNodes = 48;
struct SrtWalkPlan {
    std::array<SrtWalkNode, SrtWalkMaxNodes> nodes{};
    u32 count{};
    bool complete{true}; ///< false: a read is not bounded, or the plan is too big: there is no plan
};

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs
    SrtWalkPlan walk_plan{};       // sotc: SOTC_CP_SRT_PLAN; plain data (Serialize writes it as is)

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
