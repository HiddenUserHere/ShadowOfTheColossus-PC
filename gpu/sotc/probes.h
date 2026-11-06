// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine probes (plan step 0c, docs/HOOK-DESIGN.md). Each probe file registers the
// handlers of its hooks (gpu/sotc/hooks.h) and prints its own report line. Probes only observe:
// they never change guest registers or memory, and every handler returns HookAction::Continue.
//
// Hook names (scripts/hooks.d/*.py) and their switches:
//   probe_xpr.cpp     XPR_CS_DONE_LABEL (0x925c0, entry), XPR_CONSUME_MASK (0x1a6d20, entry)
//                     switch SOTC_HOOK_XPR_PROBE
//   probe_loader.cpp  LOADER_WAIT_ALL (0x1469d0, wrap), LOADER_SYNCPOINT (0x1480f0, wrap)
//                     switch SOTC_HOOK_LOADER_PROBE
//                     LOADER_HAS_PENDING (0x147f40, wrap) is declared for step 5a; no handler yet
//                     (switch SOTC_HOOK_LOADER_NOSTREAM)
//   probe_alloc.cpp   ALLOC_BLOCK (0x479a0, wrap), FREE_BLOCK (0x1b0e0, entry)
//                     switch SOTC_HOOK_ALLOC_PROBE
//                     BPE_HEAP_CODE (0x48a70-0x4a477, range; read by the fault classes)
//                     switch SOTC_HOOK_HEAP_RANGE
//   probe_cvars.cpp   CVAR_MAX_DELTA (range in sub_262a80 with a rip_target derived field: the
//                     cvar's storage), switch SOTC_HOOK_CVAR_PROBE
//   probe_area.cpp    AREA_PRECACHE (0x533100, entry), AREA_LOADER_STATE (the
//                     cp10_setAreaLoaderState user, entry), switch SOTC_AREA_PREWARM
//   probe_pass.cpp    PASS_RESOLVE (0xf8990, entry), MATERIAL_LOADED (0xf6c90, wrap), switch SOTC_PASS_PROBE
#pragma once

#include <cstdint>
#include <string>

namespace SotcProbes {

void RegisterXpr();
void RegisterLoader();
void RegisterAlloc();
// sotc: SOTC_VSIZE_PROBE. The live block (ALLOC_BLOCK feed) that holds an address.
struct BlockInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    int32_t kind = 0;   // 1 CPU, 2 GPU
    char pool[48] = {}; // allocator name
};
bool FindBlock(uint64_t addr, BlockInfo* out); // probe_alloc.cpp; false when no tracked block holds addr

/// sotc: SOTC_BLOCK_EXTENT=1 (off by default). An unbounded read-only binding (V# num_records ~4 G) is bound up to
/// the end of the live allocator block (ALLOC_BLOCK / FREE_BLOCK feed, SOTC_HOOK_ALLOC_PROBE) its base lies in:
/// a game object never crosses its block, and the port bound (and copied) a window of up to 64 MiB around objects of
/// a few hundred bytes (BucketAllocator, 64 KiB blocks). Read once.
bool BlockExtentOn() noexcept;
/// The bytes from addr to the end of the live tracked block holding it (an ordered index, any thread); false when no
/// tracked block holds addr.
bool BlockRemaining(uint64_t addr, uint64_t* remaining) noexcept;
void RegisterCvars();
/// sotc: SOTC_FAULT_OWNERS=1 (probe_memtype.cpp): a write fault on a GPU-written page is charged to the last
/// allocation (heap slot and caller) that touched its 64 KiB granule; the top owners are in the memtype report.
bool FaultOwnersOn();
void NoteFaultOwner(uint64_t addr);
/// sotc: SOTC_FAULT_OWNERS: a GPU write marked [addr, addr + size) GPU-written now (the buffer cache's
/// AddGpuModified). Stamps the time per 4 KiB page, so a later write fault reports the age of the mark.
void NoteGpuWriteStamp(uint64_t addr, uint64_t size);
void RegisterArea();
void RegisterPass(); // sotc: SOTC_PASS_PROBE (probe_pass.cpp), observe only; prints its own 5 s summary
// sotc: SOTC_CALL_PROBE (probe_calls.cpp), observe only; prints its own 5 s summary. RegisterCalls installs the
// CALL_<addr> handlers; NoteStepRateWrites is the port-side count of VGT_INSTANCE_STEP_RATE_0/1 writes (liverpool.cpp).
void RegisterCalls();
// sotc: SOTC_RE_PROBE (probe_re.cpp), observe only; one group per RE_* hook (scripts/hooks.d/re.py). No report line.
void RegisterRe();
// sotc: SOTC_RE_PROBE decal-cb (K6.9 item 6), port side: the draw's CB_COLOR0..7_INFO raw words and the enabled-target
// mask. Logs only while a decal MRT bind is armed (probe_re.cpp).
void NoteDecalDraw(const uint32_t info[8], uint32_t bound_mask);
void NoteStepRateWrites(uint32_t first_reg, const uint32_t* values, uint32_t count);
// sotc: PC memory model, wave B (docs/re-stutters/PC-MEMORY-REWRITE.md section 5).
void RegisterMemtype(); // probe_memtype.cpp, switch SOTC_HOOK_MEMTYPE_PROBE
void RegisterXprMem();  // probe_xprmem.cpp, switch SOTC_HOOK_XPRMEM_PROBE

/// One report line each (or nothing when the probe is off or saw nothing in the window),
/// appended to out by the driver.cpp 5 s report. seconds = window length. Resets the window.
void ReportXpr(std::string& out, double seconds);
void ReportLoader(std::string& out, double seconds);
void ReportAlloc(std::string& out, double seconds);
void ReportCvars(std::string& out, double seconds);
void ReportArea(std::string& out, double seconds);
void ReportMemtype(std::string& out, double seconds);
void ReportXprMem(std::string& out, double seconds);

} // namespace SotcProbes
