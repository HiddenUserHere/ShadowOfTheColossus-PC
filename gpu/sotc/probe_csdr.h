// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CSDR_PROBE (off by default; observe only). See probe_csdr.cpp.
#pragma once

#include <cstdint>

namespace SotcCsdrProbe {

/// SOTC_CSDR_PROBE=1, read once.
bool On() noexcept;

/// Registers RES_DECOMPRESS_DONE (a site after the resource loader's decompress call, 0x14725c).
void Register();

/// SOTC_CSDR_EXTRACT=<input pack> (with SOTC_CSDR_EXTRACT_OUT=<output pack>): decompresses every record of the input
/// pack (tools/csdr/pack_csdr.py; header "CSDRIN01") with the game's own OodleLZ_Decompress, writes the output
/// pack ("CSDROUT1") and returns the process exit code. Runs from the RES_DECOMPRESS_DONE site at the session's first
/// decompression (needs SOTC_CSDR_PROBE=1 for the hook), after the game initialized; image_base is the mapped eboot.
/// Returns -1 when the switch is not set.
int ExtractIfRequested(uintptr_t image_base);

} // namespace SotcCsdrProbe
