// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's memory for the GPU in place, the "PC memory model" (docs/sotc/MODELO-MEMORIA-PC.md).
//
// The backing view of the game's memory (src/runtime_memory.c: the pagefile section's second view,
// which bypasses the guest's page protection) is imported into Vulkan with
// VK_EXT_external_memory_host, in 64 MiB chunks the first time one is asked for: importing pins
// the pages in RAM, so only the chunks in use are. Each chunk is a buffer the GPU can copy to and
// from: what it writes there is the game's memory at once, with no staging buffer and no memcpy.
// SOTC_PC_MEMORY=1 turns it on (the device extension is asked for only then).
//
// Ring import (SOTC_PC_RING=3 or 4, the extension is asked for then too): the caller imports the
// guest ranges it copies from (ImportGuestRange) as transfer-source buffers, one per contiguous
// backing run, and the GPU copies from them at each submit (LookupGuest gives buffer and offset).

#pragma once

#include <cstdint>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}
namespace VideoCore {
struct Buffer;
}

namespace Vulkan::SotcGuestMemory {

/// After the device exists (Rasterizer): reads SOTC_PC_MEMORY and SOTC_PC_RING; the extension
/// (when enabled) and the backing view found later.
void Init(const Instance& instance);

/// At each submission: looks for the backing view until the game has mapped memory, then
/// checks the import once (the first chunk). True when the game's memory can be used in place.
bool Ready();

/// True when the game's memory can be used in place (no lookup).
bool Enabled();

struct Span {
    vk::Buffer buffer; ///< null: not importable (off, outside the backing view, across two chunks)
    u64 offset{};      ///< of the first byte in buffer
};

/// The imported chunk holding [host, host + size) of the backing view.
Span Get(const u8* host, u64 size);

/// Ring import (SOTC_PC_RING=3 or 4): true when the extension is enabled and the device reports
/// a host-visible, coherent memory type for the view whose buffers can be transfer sources.
/// Decided once, with one log line ("import" or "unavailable: <reason>"). False, undecided, while
/// the game has not mapped memory yet: the caller asks again on the next submission.
/// Thread-safe.
bool Available();

/// Ring import: imports the host pages behind the guest range [guest_addr, guest_addr + size) as
/// transfer-source buffers, one per contiguous backing run, each starting at a host address
/// aligned to minImportedHostPointerAlignment (the buffer may begin a little before the run;
/// LookupGuest accounts for it). Idempotent: a range already imported with the same backing
/// costs two backing lookups. Any thread may call it (under one mutex); false when the range
/// cannot be imported, in which case the caller copies on the CPU. Verifies the backing of the
/// range it is given, so a remapped range is imported again; LookupGuest does not re-verify.
bool ImportGuestRange(uint64_t guest_addr, uint64_t size);

/// Ring import: the imported buffer and the offset of guest_addr in it, when one run holds the
/// whole span [guest_addr, guest_addr + size). False when not imported or when the span crosses
/// runs (the caller then splits it at the run boundaries and looks each piece up).
/// Thread-safe.
bool LookupGuest(uint64_t guest_addr, uint64_t size, vk::Buffer* buffer, uint64_t* offset);

/// sotc: SOTC_PC_RING=8. The imported run holding a bind, for BufferCache's host binds.
struct GuestView {
    vk::Buffer buffer{};   ///< the imported buffer (owned by this unit; never destroyed by callers)
    u64 offset{};         ///< of the bind's first byte in buffer
    u64 buffer_size{};    ///< size of buffer
    u64 cpu_base{};       ///< guest address of byte 0 of buffer (guest_addr - offset)
    /// sotc: SOTC_BDA / SOTC_N5_BDA: device address of byte 0 of buffer, 0 when the run was
    /// imported without one (then the buffer must not be used through the block table).
    u64 device_address{};
};

/// sotc: SOTC_PC_RING=8. As LookupGuest, with the buffer's size and base. Thread-safe.
bool LookupGuestView(uint64_t guest_addr, uint64_t size, GuestView* out);

/// The same chunk as a buffer of the buffer cache's kind (for copies to images and the detiler);
/// null where Get is. *offset receives the offset of host in it. Never imports on the caller's
/// thread (an import pins 64 MiB and costs ~18 ms): a chunk not imported yet is handed to the
/// importer thread and null is returned until it is ready.
const VideoCore::Buffer* GetBuffer(const u8* host, u64 size, u64* offset);

} // namespace Vulkan::SotcGuestMemory
