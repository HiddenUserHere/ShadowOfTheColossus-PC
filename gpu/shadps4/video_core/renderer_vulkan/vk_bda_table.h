// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BDA groundwork (off by default). Host side of the block table that shaders read when
// Profile::bda_buffers is set (Shader::BufferType::BdaBlockTable): one slot per info.buffers entry
// of a stage, four u32 each (device address low, high, size in bytes, unused). The rasterizer
// fills the slots of a stage per draw and uploads the words as that stage's table descriptor.
// Nothing here owns or looks up buffers; the caller passes device addresses.

#pragma once

#include <vector>

#include "common/types.h"

namespace Vulkan {

class Instance;

/// SOTC_BDA: 0 = off (default), 1 = on, 2 = on and verify: a slot whose block does not lie inside
/// its buffer is counted in verify_fail and made null. Read once from the environment; 0 also when
/// the device has no int64 (the emitted addresses need it).
u32 BdaTableMode(const Instance& instance);

class BdaTable {
public:
    /// Starts the table of one stage with `num_slots` slots. Every slot is a null block (size 0)
    /// whose address is `null_address`: it must be readable, since shaders load from it and select
    /// zero. Counts one draw.
    void Begin(u32 num_slots, u64 null_address);

    /// Points `slot` at `size` bytes starting at `offset` of a buffer. `device_address` is the
    /// buffer's device address, or 0 when the buffer has none (then the slot is null and counted in
    /// no_bda). `buffer_bytes` is the buffer's size, used by the verify mode.
    void SetBlock(u32 slot, u64 device_address, u64 buffer_bytes, u64 offset, u64 size);

    /// Makes `slot` a null block (reads zero, writes dropped).
    void SetNull(u32 slot);

    /// Words to upload: four u32 per slot, in slot order.
    const u32* Words() const noexcept {
        return words.data();
    }
    u64 Bytes() const noexcept {
        return words.size() * sizeof(u32);
    }
    u32 NumSlots() const noexcept {
        return static_cast<u32>(words.size() / 4);
    }

private:
    void Write(u32 slot, u64 address, u32 size);

    std::vector<u32> words;
    u64 null_address{};
};

/// Counters since the last call (they reset). One line for a periodic report, for example:
/// "SOTC BDA draws 120 slots 4800 nulls 310 no_bda 0 misaligned 0 verify_fail 0 table 19200 B".
/// `nulls` counts size-0 slots, `no_bda` slots whose buffer had no device address (their reads
/// return zero: a correctness problem while SOTC_BDA is on), `misaligned` blocks whose address is
/// not 4-byte aligned (dword accesses need it).
void BdaTableReport(char* out, u32 out_size);

} // namespace Vulkan
