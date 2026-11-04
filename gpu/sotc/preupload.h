// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PRELOAD_UPLOAD (off by default). The first frames that draw a newly loaded area copy ~100 MiB of guest
// buffers on the draw thread (each bind syncs the bytes nobody uploaded yet), on top of their other first-time work.
// The engine builds those buffers at load: the resource creator 0x8be60 allocates each vertex, index or texel buffer
// in its GPU heap and memcpys the data into it from the loader's stream (the stream itself, RES_LOADED, is a
// temporary CPU buffer freed after the build). With SOTC_NATIVE that memcpy is announced like every guest copy
// (a PerFrame page mark, so the first bind uploads it); its return address tells it apart (native_hooks.cpp
// GuestWriteCode), and this module keeps its ranges:
//   =1  measure only: each draw-thread bind copy is attributed to the built ranges it overlaps, with the time from
//       the build to the copy, so the report says how much of the burst an early upload could take;
//   =2  also uploads the pools (kPool: buffers created without initial data, 1 MiB or more, read whole by the
//       game's compute passes; the memcpy ranges too with SOTC_PRELOAD_UPLOAD_RESC=1) in the draw thread's idle gaps,
//       1 MiB at a time and at most SOTC_PRELOAD_UPLOAD_MB (default 10) per presented frame or per 33 ms without one,
//       once a range is at least 100 ms old (a writer still filling it has had time to finish), so the binds of the
//       area find them clean.
// Correctness is that of an early sync (BufferCache::PreuploadRange): a later CPU write is announced (SOTC_NATIVE) or
// faults (page tracking) and the bind copies it again; a range the engine frees (SotcNative::Release) leaves the
// queue; a range the GPU wrote is never uploaded from guest memory.
#pragma once

#include <cstdint>
#include <string>

namespace SotcPreupload {

/// SOTC_PRELOAD_UPLOAD: 0 off, 1 measure, 2 measure and upload.
int Mode() noexcept;
inline bool On() noexcept {
    return Mode() != 0;
}

/// The tag of a range the resource creator filled (NoteLoaded `fourcc`).
constexpr uint32_t kResCopy = 0x43534552u; // "RESC"
/// The tag of a pool: a buffer the creator allocated without initial data (the CPU fills it later), 1 MiB or more.
constexpr uint32_t kPool = 0x4c4f4f50u; // "POOL"

/// Any thread (the creator's entry, NATIVE_ZC_CREATE): the resource object `obj` will hold a buffer of `size` bytes
/// without initial data. Its base is written by the creator (obj+0x10): it is read later (ResolvePools), and the range
/// joins the queue as kPool.
void NotePool(uint64_t obj, uint64_t size, uint32_t source, uint32_t kind) noexcept;

/// Any thread: [addr, addr + size) was filled (tag kResCopy: the resource creator's memcpy; kPool: a pool). Only
/// these ranges are queued for the upload.
/// source: the image offset of the creator call's return address; kind: its resource kind argument. With
/// SOTC_PRELOAD_UPLOAD_SOURCES (comma-separated hex offsets) only those sources are queued; the report lists the
/// sources with the bytes they built and the bytes the binds then copied from them.
void NoteLoaded(uint64_t addr, uint64_t size, uint32_t fourcc, uint32_t source = 0, uint32_t kind = 0) noexcept;

/// Draw thread (SOTC_DRAW_GATE): [addr, addr + size) is needed by a deferred draw. It goes to the front of the queue,
/// with no minimum age (the draw reads it as it is now), unless SOTC_PRELOAD_UPLOAD is not 2.
void QueueUrgent(uint64_t addr, uint64_t size) noexcept;

/// Any thread: the engine freed [addr, addr + size): its ranges leave the queue and the attribution.
void Release(uint64_t addr, uint64_t size) noexcept;

/// Draw thread: a bind's own sync of [addr, addr + size) copied `bytes`.
void NoteBindCopy(uint64_t addr, uint64_t size, uint64_t bytes) noexcept;

/// True while ranges wait for the idle upload (cheap, relaxed).
bool HasWork() noexcept;

/// Draw thread, idle. Calls drain(ctx, addr, size) on 1 MiB pieces of the queued ranges, in load order, for up to
/// budget_ns and at most the per-frame cap of uploads per presented frame (`frame`). Returns the calls made (0:
/// nothing to do now). drain returns the bytes it uploaded.
using DrainFn = uint64_t (*)(void* ctx, uint64_t addr, uint64_t size);
using StopFn = bool (*)(void* ctx);
unsigned Idle(uint64_t budget_ns, uint64_t frame, DrainFn drain, StopFn stop, void* ctx);

/// The 5 s report line (driver.cpp); nothing when the switch is off.
void Report(std::string& out, double seconds);

} // namespace SotcPreupload
