// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: validate the Vulkan version-one cache header before trusting a saved GPL list.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace SotcCache {
// VkPipelineCacheHeaderVersionOne is a 32-byte, little-endian stream, even on big-endian hosts.
inline const char* HeaderMismatch(std::span<const uint8_t> bytes, uint32_t vendor, uint32_t device,
                                  std::span<const uint8_t, 16> uuid) {
    if (bytes.size() < 32) {
        return "truncated header";
    }
    const auto word = [&](size_t offset) {
        return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8) |
               (uint32_t{bytes[offset + 2]} << 16) | (uint32_t{bytes[offset + 3]} << 24);
    };
    if (word(0) != 32 || word(4) != 1) {
        return "unsupported header size/version";
    }
    if (word(8) != vendor || word(12) != device) {
        return "different GPU";
    }
    if (!std::equal(uuid.begin(), uuid.end(), bytes.begin() + 16)) {
        return "different pipeline cache UUID";
    }
    return nullptr; // compatible header, not proof of a particular pipeline cache hit
}
} // namespace SotcCache
