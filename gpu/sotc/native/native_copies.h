// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC-native GPU result copies, the engine of option 3 of docs/re-stutters/native/PLAN.md ("the consumer
// reads the last completed value without waiting"). A client registers a guest range the GPU is about to write;
// the draw thread copies every GPU write of that range into a host-readable ring right after the dispatch; the CPU
// serves the copy once the GPU finished the frame the write belongs to (a poll of the native frame ticket, never a
// wait). Clients: native_particles.cpp (ProcessStats counts), native_masks.cpp (XPR visibility masks).
#pragma once

#include <cstdint>
#include <string>

#include "video_core/renderer_vulkan/vk_common.h" // sotc: vk::CommandBuffer, vk::Buffer

namespace VideoCore {
struct Buffer;
}

namespace SotcNativeCopies {

/// Bytes of the host-readable copy ring the rasterizer creates (Attach).
/// (16 MiB: the LOD mirror registers two tables per pre-shader dispatch, ~50 dispatches per frame, and a
/// registration must survive the 2 to 3 frames the GPU trails the CPU.)
inline constexpr uint64_t kRingBytes = 16ull << 20;
/// The largest range one registration may cover (a table of 8192 models of 32 bytes).
inline constexpr uint64_t kMaxBytes = 256ull << 10;

/// Key namespaces of the clients (the low bits hold a guest address or an object id).
inline constexpr uint64_t kKeyParticles = 1ull << 62;
inline constexpr uint64_t kKeyMasks = 2ull << 62;

/// Draw thread. A dispatched compute program: its code address and shader hash (only the programs a client asks
/// about are noted). Any thread: ProgramAt returns the hash noted for the address, 0 when none.
void NoteProgram(uint64_t code_address, uint64_t hash);
uint64_t ProgramAt(uint64_t code_address);

/// True when a client is armed (the rasterizer creates the ring then). Read once.
bool Armed();

/// The frame a registration made now belongs to (SotcNative::FrameSubmitted() + 1).
uint64_t CurrentFrame();

/// Why CopyOut served nothing: the reason of the newest candidate of the key in the seq range (counted by the
/// clients). kNoKey: no registration of the key in the range.
enum class Miss : uint8_t {
    kNone,        // served
    kNoKey,       // no candidate in the seq range
    kInvalid,     // the ring overwrote the registration
    kFrame,       // the GPU has not finished the registration's frame (or its copies are still in flight)
    kNoWrite,     // the frame finished and the GPU write was never recorded
    kShort,       // the registration is smaller than the requested bytes
    kNotCovered,  // the recorded copies do not cover the whole registration
    kAsync,       // the async queue tick of a copy is not done
    kNoStaging,   // the host-readable ring is not attached
};
inline constexpr int kMissKinds = 9;

/// Any thread. The GPU is about to write [base, base + bytes) for (key, seq). seq is the frame of the registration.
/// `identity` names the object behind the key (0: unknown); a registration whose identity changed and whose size
/// differs from the newest one of the key forgets the key's history. A seq below the newest of the key forgets
/// it too; the same seq (two registrations in one frame) keeps it. `single_write`: the registration takes exactly
/// one GPU write; the writes of its base pair with the registrations of that base in order (oldest first), so a
/// newer registration at the same base never takes the write of an older one. False when the range is too large,
/// the ring has no room or the native mode is off.
bool Register(uint64_t key, uint64_t seq, uint64_t base, uint64_t bytes, uint64_t identity = 0,
              bool single_write = false);

/// Any thread. The key and seq of the newest registration at base. False when none is live.
bool KeyOf(uint64_t base, uint64_t* key, uint64_t* seq, uint64_t* identity = nullptr);

/// Any thread. True when the key has a live registration (the clients' caches drop the keys that are not).
bool KeyLive(uint64_t key);

/// Any thread. Copies `need` bytes of the newest complete registration of key with seq in [min_seq, max_seq] into
/// dst. Returns its seq, or 0 when none is complete and large enough. `uncopied` (optional) is set when the
/// registration of max_seq exists but the GPU write was never seen. `miss` (optional) is the reason when 0 is
/// returned.
uint64_t CopyOut(uint64_t key, uint64_t min_seq, uint64_t max_seq, uint64_t need, void* dst,
                 bool* uncopied = nullptr, Miss* miss = nullptr);

/// Draw thread. The rasterizer's host-readable ring (nullptr detaches it).
void Attach(VideoCore::Buffer* staging);
/// Draw thread, after a dispatch: a buffer range the dispatch wrote. Records the GPU copy of every overlap with
/// a registered range.
void NoteWrittenBind(vk::CommandBuffer cmdbuf, vk::Buffer src, uint64_t src_offset, uint64_t addr, uint64_t size);
/// True while registrations wait (a cheap check before walking the written binds).
bool Watching();

/// "SOTC NATIVE COPIES" 5 s line (counters reset). Nothing when not armed.
/// sotc: SOTC_N5_ASYNC_COMPUTE. The async queue tick the GPU copies recorded now belong to (0: the
/// graphics queue). A copy is complete only once that async submit completed (see CopyOut).
void SetRecordingTick(uint64_t tick);

/// sotc: SOTC_N5_ASYNC_COMPUTE. Completion test of an async queue tick, set by the async compute
/// queue (null when there is none). Non-blocking.
void SetAsyncDone(bool (*done)(uint64_t tick));

/// Window counters of the ring (read and reset by the clients' report).
struct Stats {
    uint64_t paired = 0;           // single-write registrations that took their write
    uint64_t unpaired = 0;         // writes to a registered base with no registration waiting for them
    uint64_t expired = 0;          // registrations whose frame completed without their write
    uint64_t pruned_ring = 0;      // records the ring overwrote
    uint64_t pruned_ring_live = 0; // of those, records whose frame the GPU had not completed yet (still needed)
    uint64_t pruned_count = 0;     // records dropped by the record cap
    uint64_t kept_same_frame = 0;  // registrations of a key already registered this frame (history kept)
    uint64_t identity_changed = 0; // registrations whose object identity differs from the key's newest
    uint64_t forgot = 0;           // keys whose history was forgotten (seq regressed, or object replaced)
    uint64_t key_missing = 0;      // KeyOf: no registration at the base
    uint64_t key_invalid = 0;      // KeyOf: the registration at the base was overwritten by the ring
    uint64_t lag_max = 0;          // CopyOut: max frames the GPU completion trailed the submitted frame
    uint64_t write_lag_max = 0;    // paired writes: max frames between the registration and its write
    uint64_t ring_span_max = 0;    // max bytes between the newest record and the oldest kept one
};
void TakeStats(Stats& out);

void Report(std::string& out, double seconds);

} // namespace SotcNativeCopies
