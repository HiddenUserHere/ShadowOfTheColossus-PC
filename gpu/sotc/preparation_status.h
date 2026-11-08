// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: translation completion alone does not make the boot preparation ready to finalize.
#pragma once

#include <cstddef>
#include <cstdint>

namespace SotcCache {
struct PreparationStatus {
    uint64_t queued{}, completed{};
    uint32_t translating{}, compute_builds{};
    size_t backlog{};
    bool requests_waiting{}, deferred{};
    int64_t last_request_ns{}, now_ns{};

    bool ReadyToFinalize() const {
        // Zero new translations is normal with warm SPIR-V and a missing driver cache.
        // At least one loader request and a quiet interval are still required.
        return completed >= queued && translating == 0 && compute_builds == 0 && backlog == 0 &&
               !requests_waiting && !deferred && last_request_ns > 0 &&
               now_ns >= last_request_ns && now_ns - last_request_ns >= 5'000'000'000LL;
    }
};
} // namespace SotcCache
