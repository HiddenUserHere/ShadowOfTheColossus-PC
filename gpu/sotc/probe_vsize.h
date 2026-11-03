// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_VSIZE_PROBE (off by default, measurement only). See probe_vsize.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace Shader {
struct Info;
struct BufferResource;
} // namespace Shader

namespace SotcHooks {
struct GuestRegs; // hooks.h
} // namespace SotcHooks

namespace SotcVsizeProbe {

/// SOTC_VSIZE_PROBE=1, read once.
bool On() noexcept;

/// Draw thread: starts the dirty-byte count of one bind (before ObtainBuffer).
void ResetDirty() noexcept;
/// Draw thread: bytes copied by a sync of the bind in progress (buffer_cache.cpp, where the bind sync is counted).
void NoteDirty(uint64_t bytes) noexcept;
/// Draw thread: the dirty bytes counted since ResetDirty.
uint64_t Dirty() noexcept;

/// Draw thread, after the bind of an unbounded read-only binding (vsize >= 0xFFFF0000) has its buffer.
/// Logs the pair once and feeds the 5 s counters.
void NoteBind(const Shader::Info& info, const Shader::BufferResource& desc, uint32_t buf_index,
              uint64_t vbase, uint32_t vsize, uint64_t size, uint64_t dirty_bytes);

/// Appends the "SOTC VSIZE PROBE" line of the last `seconds` (driver.cpp 5 s report) and resets the window.
void Report(std::string& out, double seconds);

/// Object map (the 0x8be60 and 0x8c280 buffer objects). NoteBpeEntry is called first thing by the
/// NATIVE_ZC_CREATE pre handler (native_hooks.cpp, entry of 0x8be60): it runs only when SOTC_NATIVE=1 also
/// installs that hook, and does nothing without SOTC_VSIZE_PROBE. It counts -1 counts per caller and records
/// the object's interval (size = count * stride, as the callee computes it). Observe only.
void NoteBpeEntry(const SotcHooks::GuestRegs& regs);

/// Registers the VSIZE_VIEW_SITE handler (hooks.d/vsize_probe.py: site 0x8c2e2 in 0x8c280, after the view size
/// store). Called from RegisterAllHandlers (hook_handlers.cpp).
void RegisterVsizeHooks();

} // namespace SotcVsizeProbe
