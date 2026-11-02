// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BDA groundwork (off by default). Host side of the block table that shaders read when
// Profile::bda_buffers is set (Shader::BufferType::BdaBlockTable): one slot per info.buffers entry
// of a stage, four u32 each (device address low, high, size in bytes, unused). The rasterizer
// fills the slots of a stage per draw and uploads the words as that stage's table descriptor.
// Nothing here owns or looks up buffers; the caller passes device addresses.

#pragma once

#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace Vulkan {

class Instance;

/// SOTC_BDA: 0 = off (default), 1 = on, 2 = on and verify: a slot whose block does not lie inside
/// its buffer is counted in verify_fail and made null. Read once from the environment; 0 also when
/// the device has no int64 (the emitted addresses need it).
/// sotc: SOTC_N5_BDA (native mode only, see BdaN5Mode) also turns the table on: the result is the
/// larger of the two switches.
u32 BdaTableMode(const Instance& instance);

/// sotc: SOTC_N5_BDA: 0 = off (default) or SOTC_NATIVE off, 1 = the native buffer path: guest
/// buffers that are not texel or coherent are read through the table and their descriptors are
/// replaced by null ones; 2 = the same plus verify: descriptors stay bound and every table slot is
/// checked against the device address of its descriptor's buffer (base_mismatch). Read once.
u32 BdaN5Mode();

/// sotc: SOTC_N5_BDA_BUFFERS (native mode only, like BdaN5Mode): 0 = off (default), 1 = a guest
/// buffer slot the table serves gets no descriptor write at all (no set write, no buffer info: the
/// shader declares no variable for it, see DefineBuffers), 2 = verify: the descriptors are written
/// as before, every table slot is checked against its descriptor's buffer, and the writes that
/// mode 1 would drop are counted (desc_verify). Read once. Also turns the table on, like the others.
u32 BdaN5BuffersMode();

/// sotc: the device exposes bufferDeviceAddress (Vulkan 1.2 feature), probed once.
bool BdaBufferDeviceAddressSupported(const Instance& instance);

/// sotc: SOTC_N5_BDA_BUFFERS as used by the rasterizer: BdaN5BuffersMode, or 0 when the device has
/// no bufferDeviceAddress (the switch is then ignored, with one log line).
u32 BdaN5BuffersActive(const Instance& instance);

/// sotc: SOTC_N5_BDA_BUFFERS: counts one guest buffer slot whose descriptor write was dropped
/// (dropped = true, mode 1) or would have been dropped (dropped = false, mode 2).
void BdaNoteDescriptorSkip(bool dropped);

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

    /// Verify mode: counts a slot whose address is not `expected_address` (the device address of
    /// the descriptor's buffer plus the block offset). Null slots are not compared.
    void CheckSlot(u32 slot, u64 expected_address);

    /// Dirty check. Lookup: true when the words equal the ones last stored under `key` and that
    /// copy was made in the same stream-buffer generation (so its bytes are still in place); then
    /// `offset` is the copy's offset and the upload can be skipped. Store: remembers the words
    /// just uploaded at `offset`.
    bool Lookup(u64 key, u64 generation, u64* offset);
    void Store(u64 key, u64 generation, u64 offset);

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

    struct Uploaded {
        std::vector<u32> words;
        u64 generation{};
        u64 offset{};
    };

    std::vector<u32> words;
    u64 null_address{};
    std::unordered_map<u64, Uploaded> uploaded; ///< by stage key (pipeline hash)
};

/// Counters since the last call (they reset). One line for a periodic report, for example:
/// "SOTC BDA draws 120 slots 4800 nulls 310 no_bda 0 misaligned 0 verify_fail 0 base_mismatch 0
/// reuse 80 table 19200 B desc_skip 0 desc_verify 0".
/// `nulls` counts size-0 slots, `no_bda` slots whose buffer had no device address (their reads
/// return zero: a correctness problem while the table is on), `misaligned` blocks whose address is
/// not 4-byte aligned (dword accesses need it), `base_mismatch` (verify) slots whose address is not
/// the descriptor buffer's address plus the offset, `reuse` uploads skipped by the dirty check,
/// `desc_skip` (SOTC_N5_BDA_BUFFERS=1) descriptor writes dropped, `desc_verify` (=2) writes that
/// mode 1 would have dropped. Per frame: divide by the frame count of the same report window.
void BdaTableReport(char* out, u32 out_size);

} // namespace Vulkan
