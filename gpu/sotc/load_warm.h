// sotc: SOTC_LOAD_WARM=1 (off by default). Learned working set, warmed in idle time
// (docs/re-stutters/FIRST-FRAME-PEAK.md, phase 1).
//
// The first frame of the world after a load binds ~240 MiB of buffers the buffer cache never saw:
// each new 16 MiB tracking region starts all CPU-dirty, and the binds copy it on the draw thread
// in that one frame (72 ms of copies in a 225 ms frame), while the frames before it leave the draw
// thread idle. This module learns, per event key, which 64 KiB pages the binds of the next
// kWindowNs after the event use, saves them in user/cache/load_warm/<key>.bin, and on the next
// time the same event happens, hands those pages to the draw thread's idle gaps (BackLoop), which
// bind them early (residency and a sync copy, BufferCache::WarmRange). It repeats the pages every
// kRepassNs while the window lasts, so pages the game rewrites after a warm are copied again then.
//
// Event key: "gameload" (the game's "st_gameflow() starting game" line: a save or a new game
// loads). The free-roam areas were keys too until 30 Oct: back in an area, the passes checked
// gigabytes of clean ranges to upload ~36 MiB, using the draw thread's idle gaps mid-game.
//
// Correctness is that of a bind: page tracking stays on, a stale or wrong page costs a copy.
#pragma once

#include <cstddef>
#include <cstdint>

namespace SotcLoadWarm {

/// SOTC_LOAD_WARM=1, read once.
bool Enabled() noexcept;

/// An event: a load or an area change (any thread). Closes the current recording window (its
/// pages join that key's set, saved by the writer thread) and opens a new one for `key`, whose
/// saved set becomes the warm queue.
void OnEvent(const char* key);

/// The draw thread bound [addr, addr + size) (BufferCache::ObtainBuffer). Ignored outside a
/// recording window and during a warm (the warm's own binds are not recorded).
void NoteBind(std::uint64_t addr, std::uint64_t size) noexcept;

/// Draw thread, idle (its op queue empty). Calls warm(addr, size) for queued runs for up to
/// `budget_ns` (checked after each run of at most 1 MiB) or until stop() returns true. Returns the
/// runs warmed (0: nothing to do now, so the caller waits as usual). `warm` returns the bytes it
/// uploaded: once a whole pass uploads under kQuietBytes after the window already uploaded
/// kArrivedBytes (the data arrived, was warmed, and stays clean), the window stops warming.
/// `warm` and `stop` are plain function pointers with a context, so no allocation happens here.
using WarmFn = std::uint64_t (*)(void* ctx, std::uint64_t addr, std::uint64_t size);
using StopFn = bool (*)(void* ctx);
/// `frame`: the presented frame count; at most kFrameCapBytes are uploaded per frame, so warm
/// copies stay a few ms of a gameplay frame once the world is drawn.
unsigned WarmIdle(std::uint64_t budget_ns, std::uint64_t frame, WarmFn warm, StopFn stop, void* ctx);

// SOTC_LOAD_STREAM=1 (off by default; 2 = verify, see BufferCache::DrainRange). Not the learned
// set: the draw thread keeps, for the whole session, the guest ranges it bound (NoteBind), and in
// its idle gaps re-syncs the ones the CPU wrote since their last sync, so the next bind finds them
// clean. The copies of these early syncs go to the copy threads (SOTC_LOAD_STREAM makes
// AsyncUpload's pool exist, see async_upload.h). Independent of SOTC_LOAD_WARM: its own switch,
// read once.
bool StreamEnabled() noexcept;
/// SOTC_LOAD_STREAM=2.
bool StreamVerify() noexcept;

/// Draw thread, idle. Calls drain(addr, size) for the session's bound ranges, round robin, for up
/// to `budget_ns` and at most kStreamFrameCapBytes (SOTC_LOAD_STREAM_MB, default 16) of uploads
/// per presented frame (`frame`), and at most kStreamCallsPerGap calls per idle gap (a clean range
/// costs a tracker look only). Returns the calls made (0: nothing to do now). drain returns the
/// bytes it uploaded.
unsigned StreamIdle(std::uint64_t budget_ns, std::uint64_t frame, WarmFn drain, StopFn stop,
                    void* ctx);
/// Counters of SOTC_LOAD_STREAM, printed with the warm's 5 s report. NoteStreamAsync: bytes an early
/// sync (drain or warm) handed to the copy threads. NoteStreamBind: bytes a real bind copied on the
/// draw thread, and how long that sync took. NoteStreamVerifyBad: a =2 check failed.
void NoteStreamAsync(std::uint64_t bytes) noexcept;
void NoteStreamBind(std::uint64_t bytes, std::uint64_t ns) noexcept;
void NoteStreamVerifyBad() noexcept;

/// True while a warm queue has work (cheap, relaxed): lets the idle branch skip the call.
bool HasWork() noexcept;

/// True on this thread while a warm runs (its uploads stay out of the per-flip upload budget).
bool InWarm() noexcept;

/// RAII marker for the warm's own binds (thread-local), so NoteBind ignores them.
struct WarmScope {
    WarmScope() noexcept;
    ~WarmScope();
};

} // namespace SotcLoadWarm
