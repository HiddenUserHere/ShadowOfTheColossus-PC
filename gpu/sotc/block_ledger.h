// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BLOCK_LEDGER=1 (off by default; observe only, no behaviour change). Wall time the frame threads spend
// in secondary work done inline: pipeline and shader builds, program loads from the pretranslate pack, cache file
// writes and reads, the driver cache save, GPL library builds, the recorder's compute pipeline waits. Phase P0 of
// docs/re-stutters/native/arch-next/nonblocking-frame.md: the baseline that every later phase must bring to zero on
// the command processor, the draw thread and the recorder.
//
// Each site is timed with a Scope on the thread that runs it; the time goes to that thread's role
// (SotcStats::tl_role: command processor, draw, recorder, other = pipeline workers and the rest). Two outputs:
//   "SOTC BLOCK LEDGER" every 5 s (driver.cpp report), ms and count per role and site;
//   ", ledger cp ... draw ... rec ..." appended to each SOTCSTALL line (vk_presenter.cpp), the frame's own share.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "video_core/sotc_trace.h" // sotc: reuse the per-thread flight recorder

namespace SotcBlockLedger {

/// SOTC_BLOCK_LEDGER=1, read once at load. Scopes are no-ops when both ledger and trace are off.
inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_BLOCK_LEDGER");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}();

enum Site : uint8_t {
    PipeBuild,       ///< a graphics pipeline created inline (GetGraphicsPipeline, not on a worker)
    ComputeBuild,    ///< a compute pipeline created inline (GetComputePipeline)
    Translate,       ///< a shader translated inline (CompileModule without worker pools)
    LazyLoad,        ///< LoadLazyProgram: pack read, cache copies, module creation
    RegisterBinary,  ///< RegisterShaderBinary: the cache compare and save request
    StorageSave,     ///< Storage::DataBase::Save, the caller's part (path, copy, queueing)
    StorageLoad,     ///< Storage::DataBase::Load (file or prefetch read)
    ShaderIndex,     ///< SaveShaderIndex
    DriverCacheSave, ///< SaveDriverCache: vkGetPipelineCacheData and the file write
    DriverCacheGet,  ///< of that, vkGetPipelineCacheData alone (the driver's cache serialization)
    GplBuild,        ///< a GPL library built by GplLibraryCache::GetOrBuild
    GplLockWait,     ///< waiting for the GplLibraryCache mutex
    ComputeWait,     ///< recorder demand, including building a pipeline not claimed by a worker
    LibraryWait,     ///< command processor demand for missing graphics libraries
    StageDemand,     ///< command processor translating a pending graphics stage synchronously
    GplLink,         ///< fast-link request, inclusive of missing-library work
    SiteCount
};

/// Adds ns to the calling thread's role under site.
void Add(Site site, uint64_t ns) noexcept;

/// Steady clock in nanoseconds.
uint64_t NowNs() noexcept;

/// Records a completed site in the flight recorder (literal site name, key and ending flip).
void Record(Site site, uint64_t start, uint64_t end, uint64_t key) noexcept;

/// Times one site on the calling thread. Timings are inclusive when scopes nest.
class Scope {
public:
    explicit Scope(Site site, uint64_t key = 0) noexcept
        : site_(site), start_((kOn || SotcTrace::kOn) ? NowNs() : 0), key_(key) {}
    ~Scope() {
        if (!start_) {
            return;
        }
        const uint64_t end = NowNs();
        if (kOn) {
            Add(site_, end - start_);
        }
        if (SotcTrace::kOn && end - start_ >= SotcTrace::kZoneMinNs) {
            Record(site_, start_, end, key_);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Site site_;
    uint64_t start_;
    uint64_t key_;
};

/// The 5 s report line, inclusive site sums since the previous call; flips = frames in the window.
/// Nested sites overlap: these sums are not exclusive CPU time or a frame critical-path duration.
std::string TakeReport(uint64_t flips);

/// The frame's part (", ledger cp X ms [site ms ...] draw ... rec ..."), since the previous call; empty when nothing
/// was spent on the frame threads. Called once per presented frame.
std::string TakeFrameText();

} // namespace SotcBlockLedger
