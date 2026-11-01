// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RAM_REPORT=1 (off by default). Appends a RAM breakdown to the 5 s report: process
// working set and commit, guest pool, VirtualQuery section sweep, Vulkan memory from VMA and the
// OS budget, host import budget. See ram_report.cpp for what each number means.
#pragma once

#include <string>

namespace Vulkan {
class Presenter;
}

namespace SotcRamReport {

/// Appends the RAM lines to out when SOTC_RAM_REPORT is set and not "0". presenter may be null
/// (the Vulkan, host cache and pipeline lines are then skipped; the process, guest, loader and
/// sweep lines still print).
void Report(std::string& out, Vulkan::Presenter* presenter);

} // namespace SotcRamReport
