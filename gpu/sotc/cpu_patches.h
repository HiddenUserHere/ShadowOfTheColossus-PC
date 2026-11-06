// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"

// ============================================================================
// Windows static guest red-zone protection
// ============================================================================

enum class WindowsGuestRedZoneProtectionMode : u32 {
    Disabled,
    StaticPatching,
};

namespace Core::WindowsGuestRedZoneProtection {

void SetActiveMode(WindowsGuestRedZoneProtectionMode mode) noexcept;
WindowsGuestRedZoneProtectionMode GetActiveMode() noexcept;
bool IsStaticPatchingEnabled() noexcept;

} // namespace Core::WindowsGuestRedZoneProtection

// ============================================================================
// End Windows static guest red-zone protection
// ============================================================================

namespace Core {

// Windows static guest red-zone protection
struct RedZonePatchResult {
    u64 function_count{};
    u64 instruction_count{};
    u64 red_zone_function_count{};
    u64 memory_instruction_count{};
    u64 short_memory_instruction_count{};
    u64 patched_memory_instruction_count{};
    u64 stack_dependent_memory_instruction_count{};
    u64 control_flow_memory_instruction_count{};
    u64 unrelocatable_memory_instruction_count{};
    u64 indirect_red_zone_function_count{};
    u64 cpu_patch_instruction_count{};
    u64 patched_cpu_patch_instruction_count{};
    u64 unsupported_cpu_patch_instruction_count{};
};

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

// Windows static guest red-zone protection
/// Keeps Windows exception dispatch outside live guest red zones at faultable memory accesses.
/// When selected_addresses is not empty only functions containing one of them are patched.
RedZonePatchResult PatchRedZoneMemoryInstructions(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts,
                                                  std::span<const uintptr_t> selected_addresses = {});

/// Returns true when the host CPU lacks SSE4a and EXTRQ/INSERTQ must be emulated.
bool NeedsSse4aEmulation();

/// Relocates 4-byte EXTRQ/INSERTQ (too short for a near jump) together with the following
/// instructions into trampolines, so they never reach the illegal-instruction handler. On Windows
/// exception dispatch writes below RSP and clobbers the guest red zone, which corrupts leaf
/// functions that keep temporaries there. Returns the number of instructions patched.
u64 PatchShortSse4aInstructions(u64 segment_addr, u64 segment_size,
                                std::span<const uintptr_t> function_starts);

/// sotc: points every rdtsc at `clock` (a host function returning the TSC replacement), for
/// systems whose cores disagree on the TSC. Returns the number of sites patched; `unpatched`
/// counts the sites that could not be (they keep reading the host TSC). Windows only.
u64 PatchRdtscInstructions(u64 segment_addr, u64 segment_size,
                           std::span<const uintptr_t> function_starts, void* clock,
                           u64* unpatched);

/// sotc: a detour from guest code into the host hook dispatcher (gpu/sotc/hooks.h,
/// docs/HOOK-DESIGN.md). The stub follows the rdtsc clock call (GenerateClockCall): it steps over
/// the 128-byte red zone, saves flags, every GPR (as a GuestRegs) and xmm0-7, aligns the stack,
/// reserves the Win64 shadow space and calls sotc_hook_dispatch(regs, hook_id, phase). A pre
/// handler may return HookAction::CallGuest in any mode: the stub then calls the guest function
/// set by SetGuestCall (hooks.h) on the guest stack, before the restore and the original.
enum class DetourMode : u32 {
    /// At a function entry. The pre-handler may return HookAction::Return: the stub then returns
    /// to the guest caller with regs.rax/rdx/xmm0 as the result, skipping the original.
    Entry,
    /// At a function entry, pre and post. The original runs as a call from the stub (one extra
    /// frame on the guest stack); the post-handler sees and may change the return values. Only
    /// for functions without stack arguments and without varargs.
    Wrap,
    /// At one instruction inside a function. The handler may edit the registers, which are
    /// written back before the displaced instructions run.
    Site,
    /// sotc: At a function entry, timed (SOTC_TRACE_FUNCS, docs/HOOK-DESIGN.md). No GuestRegs and
    /// no dispatch: the stub records the entry time and a shadow frame (sotc_trace_funcs_enter),
    /// and the return goes through one shared trampoline that records the span
    /// (sotc_trace_funcs_leave). Same refusals as Wrap, plus a return at a non-zero stack depth.
    Time,
};

struct DetourRequest {
    u64 address{};                  ///< host address of the hooked instruction (image + offset)
    DetourMode mode{DetourMode::Entry};
    u32 hook_id{};                  ///< passed to sotc_hook_dispatch
    std::span<const u8> pattern;    ///< bytes expected at address after all other patches
    std::span<const u8> mask;       ///< same length: 0xff compare, 0x00 wildcard (empty: all 0xff)
    /// sotc: SOTC_NATIVE_TRACK_EMU=2 (Site only). Set: the stub calls this handler (GuestRegs*, hook_id, 0) instead of
    /// sotc_hook_dispatch, and when it returns kStoreHookDone the first displaced instruction (the store the handler
    /// did) is skipped.
    const void* store_handler{};
    /// sotc: other threads run: the jump is written with them paused, and refused while one is inside the span.
    bool live{};
    /// sotc: live only: also refused while this says a thread will resume inside [lo, hi) (a thread inside a fault
    /// handler, whose context is not its current RIP; the calling thread itself).
    bool (*live_busy)(u64 lo, u64 hi){};
};

struct DetourResult {
    bool installed{};
    const char* reason{""};         ///< why not installed (static string)
    u64 displaced_bytes{};          ///< guest bytes moved into the trampoline
    u64 stub{};                     ///< sotc: host address of the detour stub (diagnostic)
};

/// Installs one detour into the module registered with RegisterPatchModule. Refuses (installed =
/// false) when the pattern does not match, the displaced span (>= 5 bytes) holds a branch target
/// after its first instruction or control flow before its end, overlaps another CPU patch, lies in
/// a red-zone protected function, or the trampoline area is full. Nothing is written on refusal.
bool InstallGuestDetour(const DetourRequest& request, DetourResult* result);

/// Trampoline bytes used so far by every CPU patch of the registered module.
u64 TrampolineBytesUsed();

/// sotc: SOTC_NATIVE_TRACK_EMU=2, live store hooks (gpu/sotc/native/fault_emu.cpp).
/// The handler's return value that means "the store was done": the hooked instruction is skipped.
constexpr u32 kStoreHookDone = 0x53544f52;
/// Called for each store a function relocation instruments: the site id its stub passes to the handler, or ~0u to
/// leave the instruction as it is.
using StoreSiteRegister = u32 (*)(u64 address, const u8* bytes, u32 length);
enum class StoreHookKind : u32 {
    Refused = 0,
    Detour = 1,           ///< a Site detour at the store
    Relocated = 2,        ///< the function was copied with its stores instrumented, its entry jumps to the copy
    AlreadyRelocated = 3, ///< the function had been relocated before (the store is instrumented in the copy)
};
/// Hooks the store at site: a Site detour (store_handler) when the installer accepts it, else the containing function
/// is relocated (every store of it that register_site accepts is instrumented; the original stays as it was, but for
/// its entry, for any path that still reaches it). live: other threads run (paused around each code write).
StoreHookKind InstallStoreHook(u64 site, u32 site_id, const void* handler, StoreSiteRegister register_site,
                               bool live, const char** reason, bool (*live_busy)(u64 lo, u64 hi) = nullptr);

/// sotc: gives the registered module its function starts (host addresses) for the guest detours,
/// without patching anything. The other passes merge theirs too, but they may not run.
void RegisterFunctionStarts(std::span<const uintptr_t> function_starts);

} // namespace Core
