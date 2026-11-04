// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RES_DUMP=<dir> (off by default; observe only). See probe_dump.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace SotcResDump {

/// SOTC_RES_DUMP set to a directory, read once.
bool On() noexcept;

/// Registers the RES_CREATE and CWKS_BODY handlers (hooks in scripts/hooks.d/dump.py).
void Register();

/// Called from the RES_LOADED handler (probe_csdr.cpp) for every resource the loader hands over. ret is the return
/// address (the loader's call site), data and n the bytes it passes. Writes the file when the resource's type is
/// selected and the cap is not reached.
void NoteResource(uint64_t ret, const unsigned char* data, size_t n) noexcept;

} // namespace SotcResDump
