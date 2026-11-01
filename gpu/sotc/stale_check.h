// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_STALE_CHECK, a sampled detector for missed CPU writes (stale vertex/index data).
// Implementation and the reasoning behind it: stale_check.cpp. Hooks: buffer_cache.cpp, marked
// `// sotc: SOTC_STALE_CHECK`.
#pragma once

#include <cstdint>
#include <string>

namespace SotcStale {

/// SOTC_STALE_CHECK: unset, empty or "0" = off. A number N >= 1 = check one bind in N. A value
/// that is not a number (e.g. "on") = 16. Read once.
bool On() noexcept;
uint32_t SampleN() noexcept;

/// True for one call in SampleN() (and only when On()). Called once per SynchronizeMemory.
bool SampleBind() noexcept;

/// Tracker predicates over guest addresses, supplied by the buffer cache. Neither may be called
/// while a stale-check lock is held (the implementation never does).
struct Oracle {
    void* ctx;
    bool (*cpu_modified)(void* ctx, uint64_t addr, uint64_t size);
    bool (*gpu_modified)(void* ctx, uint64_t addr, uint64_t size);
};

/// Drops the stored hashes of every 4 KiB page that [addr, addr + size) touches. Always safe to
/// call (it only removes checks); call it on every path that may change a page's bytes or its
/// tracker bits without storing new hashes. No-op when off.
void Forget(uint64_t addr, uint64_t size);

/// Upload record: stores the XXH3-64 hash of every whole page of [addr, addr + size) (guest
/// addresses) that the oracle does not report GPU-modified, read through the backing view. The
/// caller has just copied these bytes to staging and their pages are write-protected.
void RecordUpload(uint64_t addr, uint64_t size, const Oracle& oracle);

/// Bind check over [addr, addr + size): every whole page that is CPU-clean and GPU-clean and has
/// a stored hash is hashed again; a different hash is a STALE page (counted, first 32 logged).
void CheckBind(uint64_t addr, uint64_t size, const Oracle& oracle);

/// Appends one line "SOTC STALE CHECK: ..." to out with the counters, and the deltas since the
/// previous call. seconds = time since the previous call (the caller passes 5 s, or the real
/// elapsed time). Appends nothing when off.
void Report(std::string& out, double seconds);

} // namespace SotcStale
