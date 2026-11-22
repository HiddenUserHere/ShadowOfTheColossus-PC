// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: taken from Pink-shadPS4 0.4.0 (luizgustavs), cp_record_thread; scheduler hooks in vk_scheduler.cpp.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// With cp_record_thread, record Vulkan commands and submit them on a separate thread from the
/// command processor
/// The scheduler gives the producer fake command buffer handles, and dispatcher handlers copy their
/// arguments into an ordered stream
/// The consumer replays that stream on real command buffers, while presenter, ImGui and ahead
/// copies using real handles call the driver directly
///
/// Only one producer owns the scheduler at a time and one consumer reads the stream of 256 KiB
/// chunks
/// Data blocks come before the commands using them, and a command may still point into the
/// preceding chunk
/// Recycle a chunk only after the consumer leaves the following chunk so those references remain
/// valid
/// Each producer write is published before the consumer reads it, keeping the stream in the order
/// used by the command processor
/// Copied arrays stay inside these chunks until their commands have run, so the producer does not
/// have to keep the original argument memory alive


class CommandRecorder;

/// sotc: the one recorder of the process (set in its constructor, cleared in its destructor); a
/// command queued by Run below runs against it. Defined in vk_recorder.cpp.
CommandRecorder*& TheRecorder();

// sotc: N2-94 coverage (owned: vk_recorder.h, vk_recorder.cpp), H09
// item                                            | done? | evidence
// ------------------------------------------------|-------|-----------------------------------------
// W1 untimed consumer wait (SOTC_REC_BLOCK)        | yes   | ConsumerLoop: wait(lk, pred) when RecBlock
// W1 wake protocol re-verified and documented      | yes   | comment block at ConsumerLoop sleep path
// W1 stop_token wakes the untimed wait             | yes   | std::stop_callback notifies wake_cv
// W1 optional spin kept                            | yes   | SpinTime loop unchanged
// W4 PublishBatch runtime (32 / 256 with block)    | yes   | publish_batch set in constructor
// W4 PublishNow() for pass boundaries              | yes   | inline in this header; call site is H10
// Default path (switch off)                        | yes   | timed wait_for and PublishBatch 32 unchanged
// W5 submit thread (SOTC_PASS_RECORDER)            | no    | not in H09 spec for this worker

class CommandRecorder {
public:
    CommandRecorder();
    ~CommandRecorder();

    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;

    /// Install command handlers once after the default dispatcher has loaded the device functions
    /// Copy ordinary arguments with the generic handler and pointer arguments with dedicated deep-
    /// copy handlers
    /// Stop and report the command name if a fake handle reaches a command whose pointer arguments
    /// are unsupported
    static void InstallThunks();

    [[nodiscard]] static bool IsFake(VkCommandBuffer cmdbuf) noexcept;

    /// Give the producer a fake handle for a command buffer that the consumer will allocate later
    /// The queued closure calls Bind to connect it with the real buffer before recording its
    /// commands
    [[nodiscard]] vk::CommandBuffer NewFakeHandle();

    /// Queue func for the recording thread after every operation already in the stream so the
    /// producer can preserve command order
    template <typename F>
    void Run(F&& func) {
        using T = std::decay_t<F>;
        static_assert(alignof(T) <= BlockAlign, "Closure over-aligned for the stream");
        // sotc: a command queued from another thread runs on the recording thread; an op queued
        // there records through this same instance (one recorder per process).
        void* payload = Allocate(sizeof(T), BlockKind::Command,
                                 [](void* p) { TheRecorder()->Invoke<T>(p); });
        new (payload) T(std::forward<F>(func));
        Publish();
    }

    /// Reserve a command block of size bytes for invoke(payload) to run on the recording thread
    /// Write the arguments and any referenced arrays into that block, then call Commit before
    /// making another recorder call
    /// Keeping the arguments with the command lets the caller reuse its original memory after
    /// publication
    [[nodiscard]] void* AllocateCommand(size_t size, void (*invoke)(void* payload)) {
        return Allocate(size, BlockKind::Command, invoke);
    }

    /// Publish the block reserved by AllocateCommand so the consumer can run it in the same ordered
    /// stream used by Run
    void Commit() {
        Publish();
    }

    /// Copy count elements into the stream for the next queued command so the producer can release
    /// the original array, preserving null pointers
    template <typename T>
    [[nodiscard]] T* Copy(const T* src, size_t count = 1) {
        if (src == nullptr || count == 0) {
            return nullptr;
        }
        static_assert(std::is_trivially_copyable_v<T>);
        void* dst = Allocate(sizeof(T) * count, BlockKind::Data, nullptr);
        std::memcpy(dst, src, sizeof(T) * count);
        return static_cast<T*>(dst);
    }

    /// Reserve enough room in the current chunk for the next data and command blocks so their bytes
    /// stay together until the consumer finishes
    void Reserve(size_t bytes);

    /// Publish all queued work and wake the recording thread if it is sleeping at this submit point
    /// Between kicks, publish smaller batches for the consumer to pick up while spinning or after
    /// its short timed wait
    /// A Kick makes every command already queued visible before the submit or drain can continue
    void Kick();

    /// sotc: N2-94 W1/W4. Publish the pending batch, if any, at a pass boundary (EndRendering, submit).
    /// Costs nothing when nothing is pending, unlike Kick, which always stores and fences.
    void PublishNow() {
        if (unpublished != 0 || commands_counted != 0) {
            Kick();
        }
    }

    /// Count this deferred Vulkan call for performance statistics and report the accumulated calls
    /// at the next Kick
    void CountCommand() noexcept {
        ++commands_counted;
    }

    /// Wait until the recording thread has run every operation queued so far before the producer
    /// continues with work needing those results
    void Sync();

    /// Make the calling thread the producer when shutdown moves recording to another thread
    /// The previous producer must already have stopped, and any other unexpected producer change
    /// stops the recorder
    void ClaimProducer();

    /// sotc: lets a thread that records for the producer issue the commands itself (a native draw
    /// whose hooks run on the command processor, with the stream recorded on its own thread): the
    /// commands are the stream's last operations and must run on the recording thread after them,
    /// so the stream is handed over for this batch. Only threads other than the recording thread
    /// do this; the previous producer must have stopped recording (HoldRecorder ensures it). The
    /// stream goes back to the producer at the batch's next Publish.
    void BeginBatch();

    /// sotc: sub-batches recorded inside a batch keep their parent's ownership.
    bool InBatch() const noexcept {
        return in_batch;
    }

    /// Connect a fake command buffer handle to the real buffer allocated by the consumer before
    /// queued commands use it
    void Bind(VkCommandBuffer fake, VkCommandBuffer real) noexcept;

    /// Look up the real command buffer belonging to this fake handle when the consumer replays its
    /// queued commands
    [[nodiscard]] VkCommandBuffer Real(VkCommandBuffer fake) const noexcept;

    /// Release count fake handle sessions after the consumer has submitted them so the producer can
    /// reuse those slots later
    void Retire(u32 count) noexcept {
        handles_retired.fetch_add(count, std::memory_order_release);
    }

    [[nodiscard]] bool IsRecordingThread() const noexcept {
        return std::this_thread::get_id() == thread.get_id();
    }

    static constexpr size_t BlockAlign = 16;

    static constexpr u32 NumFakeHandles = 1U << 16;

private:
    enum class BlockKind : u32 { Data, Command, End };

    struct BlockHeader {
        u32 size; ///< Total block size including its header, rounded to a multiple of BlockAlign for the next block
        BlockKind kind;
        void (*invoke)(void* payload);
    };
    static_assert(sizeof(BlockHeader) == BlockAlign);

    struct Chunk {
        static constexpr size_t Size = 256_KB;
        std::atomic<u32> committed{0};
        std::atomic<Chunk*> next{nullptr};
        alignas(64) u8 data[Size];
    };

    template <typename T>
    void Invoke(void* payload) {
        T& func = *static_cast<T*>(payload);
        // sotc: an op that records its own commands (RunAfterRecorded) may own the stream already;
        // its nested commands must not end that ownership.
        const bool outer = thread_command;
        thread_command = true;
        func();
        thread_command = outer;
        func.~T();
    }

    void* Allocate(size_t size, BlockKind kind, void (*invoke)(void*));
    void Publish();
    void PublishAll();
    void NextChunk();
    Chunk* AcquireChunk();
    void RecycleChunk(Chunk* chunk);
    void WakeConsumer();
    void WakeIfSleeping();
    void CheckProducer();
    void ConsumerLoop(std::stop_token stoken);

    static constexpr size_t MaxChunksInFlight = 256; ///< Allow up to 64 MiB of queued commands before the producer waits for the recording thread to
///< catch up
    /// sotc: N2-94 W4. Commands published per batch: 32 by default, 256 with SOTC_REC_BLOCK (the
    /// consumer then sleeps untimed, so a larger batch costs fewer wake-ups).
    static constexpr u32 PublishBatchDefault = 32;
    static constexpr u32 PublishBatchBlock = 256;
    u32 publish_batch{PublishBatchDefault};

    // State written by the producer while it adds commands and publishes batches to the recording
    // thread
    Chunk* write_chunk{};
    u32 write_pos{};
    u64 handles_issued{};
    u64 sync_requested{};
    u64 commands_counted{};
    u32 unpublished{};
    u32 producer_tid{};
    u32 producer_switches{};
    /// sotc: a thread other than the recording thread is between BeginBatch and the batch's end
    /// (only that thread adds commands then, so a plain flag is enough).
    bool in_batch{};
    /// sotc: true while a command queued with Run is running on the recording thread: its nested
    /// commands are that op's, which may already own the stream.
    bool thread_command{};

    // State written by the consumer while it runs queued commands and advances through the stream
    // chunks
    std::array<VkCommandBuffer, NumFakeHandles> real_handles{};

    // State shared by both threads to track queued work, sleeping and the recorder lifetime
    std::atomic<u64> handles_retired{0};
    std::atomic<u64> sync_done{0};
    std::atomic<u32> chunks_in_flight{0};
    std::atomic<bool> consumer_sleeping{false};
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wake_flag{};
    std::mutex free_mutex;
    std::vector<Chunk*> free_chunks;
    Chunk* first_chunk{};
    std::jthread thread;
};

} // namespace Vulkan
