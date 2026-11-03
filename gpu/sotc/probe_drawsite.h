// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DRAW_SITES=1 (off by default). Counts the draw and dispatch imports per guest call chain, so the
// GPU ops of a frame can be split by engine system (docs/re-backend). Observe only.
#pragma once

#include <cstdint>
#include <string>

namespace SotcProbes {

bool DrawSitesOn();

/// Records one draw (dispatch false) or dispatch import call. ret_slot is the address of the host function's
/// return address on the guest stack (__builtin_frame_address(0) + 8 in a frame-pointer build).
void NoteDrawSite(const void* ret_slot, bool dispatch);

/// One header line plus the top call chains of the window, per frame. Resets the window.
void ReportDrawSites(std::string& out, double seconds);

} // namespace SotcProbes

// Expand at the top of a draw or dispatch import's host function.
#define SOTC_NOTE_DRAW_SITE(dispatch)                                                                    \
    do {                                                                                                 \
        if (::SotcProbes::DrawSitesOn()) {                                                               \
            ::SotcProbes::NoteDrawSite(static_cast<const char*>(__builtin_frame_address(0)) + 8, dispatch); \
        }                                                                                                \
    } while (0)
