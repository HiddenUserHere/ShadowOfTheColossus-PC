// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

#include "common/types.h"

// sotc: SOTC_TC_FAST_LOOKUP: the texture-cache lookup fast paths (texture_cache.cpp MemoLookup,
// LockCounted, image.cpp Image::FindView).
//   0 (default): the code as before.
//   1: the fast paths. Same answers: every check is kept, only the order and the data touched change.
//   2: the fast paths, and every memo hit and view lookup is also computed by the reference search;
//      a difference counts in fast_verify_mismatch (the fast answer is still used).
// Read once. Counters are reported by TextureCache::ReportTcFast every window (driver.cpp).
namespace VideoCore::TcFast {

inline u32 Mode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_TC_FAST_LOOKUP");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return mode;
}

/// MemoLookup calls while the fast paths are on.
inline std::atomic<u64> fast_lookups{};
/// MemoLookup calls answered by the memo (a fast hit).
inline std::atomic<u64> fast_hits{};
/// Mode 2: fast answers that differ from the reference search (memo hits and view lookups).
inline std::atomic<u64> fast_verify_mismatch{};
/// Mode 2: memo hits and view lookups checked against the reference search.
inline std::atomic<u64> fast_verify_checks{};

} // namespace VideoCore::TcFast
