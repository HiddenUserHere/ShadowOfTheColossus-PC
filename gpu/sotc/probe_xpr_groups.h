// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_XPR_GROUPS (off by default, measurement only). See probe_xpr_groups.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace SotcHooks {
struct GuestRegs; // hooks.h
} // namespace SotcHooks

namespace SotcXprGroups {

/// SOTC_XPR_GROUPS=1, read once.
bool On() noexcept;

/// Guest thread, at the entry of the XPR draw loop 0x1a6d20 (NATIVE_MASK_VIS, after the mask it will read is in
/// regs.rsi): walks the loop's own data without changing anything and feeds the 5 s counters.
void OnConsume(const SotcHooks::GuestRegs& regs) noexcept;
/// Guest thread: one call of the alternate consumer 0x1a7290 (NATIVE_MASK_VIS_IND).
void OnConsumeIndirect() noexcept;

/// Registers XPR_CULL_JOB / XPR_CULL_CASCADE (callers of 0x11dbe0, the GPU cull that writes the args records).
void Register();

/// The 5 s report line (driver.cpp); nothing when the switch is off.
void Report(std::string& out, double seconds);

} // namespace SotcXprGroups
