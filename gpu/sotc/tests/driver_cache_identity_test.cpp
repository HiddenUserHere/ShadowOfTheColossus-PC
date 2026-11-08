// SPDX-License-Identifier: GPL-2.0-or-later
#include "driver_cache_identity.h"

#include <cassert>
#include <vector>

int main() {
    std::array<uint8_t, 16> uuid{};
    uuid[7] = 0xA5;
    std::vector<uint8_t> blob(64);
    blob[0] = 32;
    blob[4] = 1;
    blob[8] = 0xDE;
    blob[9] = 0x10; // NVIDIA 0x10DE, little endian
    blob[12] = 0x34;
    blob[13] = 0x12;
    std::copy(uuid.begin(), uuid.end(), blob.begin() + 16);
    assert(!SotcCache::HeaderMismatch(blob, 0x10DE, 0x1234, uuid));
    for (size_t length = 0; length < 32; ++length) {
        assert(SotcCache::HeaderMismatch(std::span{blob}.first(length), 0x10DE, 0x1234, uuid));
    }
    assert(SotcCache::HeaderMismatch(blob, 0x1002, 0x1234, uuid));
    assert(SotcCache::HeaderMismatch(blob, 0x10DE, 0x5678, uuid));
    auto other = uuid;
    other[0] ^= 1;
    assert(SotcCache::HeaderMismatch(blob, 0x10DE, 0x1234, other));
    blob[4] = 2;
    assert(SotcCache::HeaderMismatch(blob, 0x10DE, 0x1234, uuid));
    blob[4] = 1;
    blob[0] = 16;
    assert(SotcCache::HeaderMismatch(blob, 0x10DE, 0x1234, uuid));
}
