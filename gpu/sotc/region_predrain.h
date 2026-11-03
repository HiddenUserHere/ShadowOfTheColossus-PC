// sotc: SOTC_REGION_PREDRAIN=1 and SOTC_BIG_BIND_LOG=1 (both off by default). See region_predrain.cpp
// and docs/re-backend/15-burst-stutters.md (fixes 1 and 2).
#pragma once

#include <cstdint>        // std::uint32_t, std::uint64_t
#include <string>

namespace SotcRegionPredrain {

/// SOTC_REGION_PREDRAIN=1, read once: queue new 16 MiB tracking regions and upload them in idle time.
bool Enabled() noexcept;

/// SOTC_BIG_BIND_LOG=1, read once: one log line per (pgm_hash, stage, binding) for a bind sync of 4 MiB or more.
bool BigBindLogEnabled() noexcept;

/// True when either switch is on: the pending-region bits and the bind counters are kept.
bool TrackOn() noexcept;

/// Any thread. A 16 MiB tracking region (index = guest address >> 24) was created by
/// MemoryTracker::CreateRegion. Its pending bit is set (a fresh region: created, not yet synced), and
/// with SOTC_REGION_PREDRAIN it is queued for the idle drain.
void NoteRegionCreated(std::uint64_t region_index) noexcept;

/// Any thread, after the tracker walk of a sync of [addr, addr + size) (BufferCache::SynchronizeMemoryFull).
/// Clears the pending bits of the regions the range covers. Returns how many were pending: the fresh
/// regions this sync touched (a region created by the walk itself counts).
std::uint32_t NoteWindowSynced(std::uint64_t addr, std::uint64_t size) noexcept;

/// True while the queue has regions (cheap, relaxed): lets the idle branch skip the call.
bool HasWork() noexcept;

/// Draw thread, idle. Calls drain(ctx, addr, size) on 1 MiB pieces of the queued regions, in queue
/// order, for up to budget_ns and at most SOTC_REGION_PREDRAIN_MB (default 8) of uploads per presented
/// frame (`frame`). Returns the calls made (0: nothing to do now, so the caller waits as usual).
/// drain returns the bytes it uploaded. A region is finished after its last piece.
using DrainFn = std::uint64_t (*)(void* ctx, std::uint64_t addr, std::uint64_t size);
using StopFn = bool (*)(void* ctx);
unsigned Idle(std::uint64_t budget_ns, std::uint64_t frame, DrainFn drain, StopFn stop, void* ctx);

/// Draw thread. True when [addr, addr + size) overlaps a CPU-only range of SOTC_CLASS_TRACK
/// (mem_classes.h Snapshot, refreshed when the class table version moves). Such ranges are not predrained.
bool InCpuOnlyClass(std::uint64_t addr, std::uint64_t size) noexcept;

/// Draw thread, SOTC_BIG_BIND_LOG only: the shader and descriptor of the bind the next ObtainBuffer
/// serves. Set by the rasterizer before the call and cleared after it.
void SetBindContext(std::uint64_t pgm_hash, std::uint32_t hw_stage, std::uint32_t sw_stage,
                    std::uint32_t binding, std::uint64_t base, std::uint32_t vsize,
                    std::uint32_t num_records, std::uint32_t stride) noexcept;
void ClearBindContext() noexcept;

/// Draw thread, a bind's own sync (not an early one): `bytes` copied on the draw thread and `fresh`
/// regions it touched. Counts the bind when bytes >= 4 MiB, and logs it once per key with
/// SOTC_BIG_BIND_LOG (no behaviour change).
void NoteBindSync(std::uint64_t addr, std::uint64_t size, std::uint64_t bytes,
                  std::uint32_t fresh) noexcept;

/// Appends the SOTC_REGION_PREDRAIN and bind counters of the last `seconds` to `out` (driver.cpp 5 s
/// report). Empty when both switches are off.
void Report(std::string& out, double seconds);

} // namespace SotcRegionPredrain
