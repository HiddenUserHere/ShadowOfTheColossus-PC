// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "common/inline_function.h" // sotc: SOTC_INLINE_FUNC
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/op_footprint.h" // sotc: SOTC_OP_OVERLAP
#include "video_core/amdgpu/regs.h"
#include "video_core/sotc_stats.h" // sotc: SOTC_DROP_FAST counters (CmdRanges::AddNested)
#include "video_core/renderer_vulkan/vk_frame_graph.h" // sotc: SOTC_N5_GRAPH (pass / hazard model)
#include "../../../sotc/label_honest.h" // sotc: SOTC_LABELS_HONEST (label kinds, counters)
#include "../../../sotc/cmd_counters.h" // sotc: SOTC_CMD_COUNTERS (frame count at SubmitDone)

namespace Vulkan {
class Rasterizer;
class Pipeline;
struct PreparedDraw; // sotc: SOTC_PARALLEL_RECORD (draw_prep.h)
}

namespace Shader {
struct Info;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct PM4CmdWaitRegMem; // sotc: pm4_cmds.h (EopShadowSatisfies)

// sotc: SOTC_CP_LABEL_SKIP report line (liverpool.cpp), appended every 5 s while the switch is on:
// waits that did not pass at once, skippable ones, skipped ones (mode 2 counts only).
void ReportCpLabelSkip(std::string& out, double seconds);

// sotc: SOTC_EOP_TICK statistics (liverpool.cpp), printed under SOTC_FPS.
extern std::atomic<u64> g_eop_written, g_eop_latency_ns, g_eop_max_latency_ns, g_eop_flushes;
// sotc: SOTC_RB_EXACT label side (liverpool.cpp). Labels deferred behind a hot GPU write, the sum
// and the maximum of their deferral latency (ns, queue to landing), labels written in total, and
// deferred labels that landed on a thread other than the draw thread.
extern std::atomic<u64> g_rbx_labels_deferred, g_rbx_labels_total, g_rbx_defer_latency_ns,
    g_rbx_defer_max_latency_ns, g_rbx_offthread;
// sotc: SOTC_RB_EXACT adaptive label policy: addresses currently deferred (gauge), mode switches,
// early re-probes of deferred addresses, hot labels written early, waits charged / not charged to
// an early label, global EWMAs (ns) of deferred landing latency (a) and charged wait (b).
extern std::atomic<u64> g_rbx_addr_deferred, g_rbx_mode_switches, g_rbx_probes,
    g_rbx_labels_early_hot, g_rbx_waits_charged, g_rbx_waits_unattributed, g_rbx_ewma_a_ns,
    g_rbx_ewma_b_ns;

// sotc: SOTC_FUTEX_WAIT counters of the command processor and draw thread waits (liverpool.cpp).
// A wait counts once in "waits"; it ends either in the spin ("spins") or in WaitOnAddress
// ("sleeps"). "safety_met" counts timed-out sleeps whose condition already held: wakes that would
// have been lost without the timeout. Read and reset by the 5 s report (driver.cpp).
struct SotcFutexCpCounters {
    std::atomic<u64> waits{};
    std::atomic<u64> spins{};
    std::atomic<u64> sleeps{};
    std::atomic<u64> sleep_ns{};
    std::atomic<u64> sleep_max_ns{};
    std::atomic<u64> wakes{};
    std::atomic<u64> safety_met{};
};
inline SotcFutexCpCounters g_futex_cp;

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};

public:
    explicit Liverpool();
    ~Liverpool();

    /// sotc: SOTC_FRAME_METRICS: ops the command processor has queued (back_head; the command
    /// processor's own thread reads it, the sequence numbers the draw loop runs by).
    [[nodiscard]] u64 QueuedOps() const {
        return back_head.load(std::memory_order_relaxed);
    }

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        SotcCmdCounters::NoteFrame(); // sotc: SOTC_CMD_COUNTERS (frame end: submit index, frame count)
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        if (frame_tickets) {
            PushFrameMark(); // sotc: SOTC_CP_AHEAD=2
        }
        submit_done_seq.fetch_add(1, std::memory_order_release); // sotc: SOTC_PC_RING8_FLIPWAIT=2
        submit_done = true;
        submit_cv.notify_one();
        NotifyCpWait();
    }

    /// sotc: SOTC_CP_IDLE_US. Called after port code changed something a command buffer task of
    /// the command processor may be waiting on (a label or fence written, a flip label reset, a
    /// new submit or command): wakes the command processor if it sleeps in CpIdleSleep. Nothing
    /// (not even the atomic) when the switch is off. Any thread.
    void NotifyCpWait() noexcept {
        if (!cp_idle_ns && !cp_round_sleep) { // sotc: SOTC_CP_LABEL_SLEEP uses the epoch too
            return;
        }
        // seq_cst on both sides (CpIdleSleep): either this sees cp_sleeping or the command
        // processor sees the new epoch before it blocks.
        cp_wake_epoch.fetch_add(1, std::memory_order_seq_cst);
        if (cp_sleeping.load(std::memory_order_seq_cst)) [[unlikely]] {
            WakeCp();
        }
        // sotc: SOTC_CP_LABEL_SLEEP: the same handshake with CpRoundSleep (WaitOnAddress on the epoch).
        if (cp_round_sleeping.load(std::memory_order_seq_cst)) [[unlikely]] {
            WakeCpRound();
        }
    }

    void WaitGpuIdle() noexcept {
        {
            std::unique_lock lk{submit_mutex};
            submit_cv.wait(lk, [this] { return num_submits == 0; });
        }
        // sotc: and until the draw thread ran what the command processor queued.
        while (back_tail.load(std::memory_order_acquire) != back_head.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    /// sotc: waits only until the command processor parsed every submitted command buffer; the
    /// draw thread may still run what it queued (SOTC_CP_AHEAD, see gnmdriver.cpp WaitSubmitGate).
    void WaitCommandsConsumed() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    /// sotc: the SOTC_CP_AHEAD value (0 off, the default; 1 parse gate; 2 frame tickets; other
    /// non-zero values mean 1, as before mode 2 existed). See gnmdriver.cpp WaitSubmitGate.
    static int CpAheadMode() noexcept;
    /// sotc: SOTC_OPSTAT: sizeof(BackOp) (the type is private).
    static std::size_t BackOpSize() noexcept;
    /// sotc: SOTC_CP_AHEAD=2: frames closed by SubmitDone so far (any thread).
    u64 FramesMarked() const noexcept {
        return frames_marked.load(std::memory_order_acquire);
    }
    /// sotc: SOTC_CP_AHEAD=2: frames whose every operation the draw thread has run (any thread).
    /// sotc: SOTC_PC_RING8_FLIPWAIT=2. SubmitDone calls so far (the guest's frame ends).
    u64 SubmitDoneSeq() const noexcept {
        return submit_done_seq.load(std::memory_order_acquire);
    }
    u64 FramesRecorded() const noexcept {
        return frames_recorded.load(std::memory_order_acquire);
    }
    /// sotc: SOTC_CP_AHEAD=2: blocks until at least `target` frames are recorded (guest threads).
    void WaitFramesRecorded(u64 target) noexcept;
    /// sotc: SOTC_ASC_RPTR (0, the default: the compute ring read pointer moves past a packet when
    /// the command processor has finished it, an indirect buffer included; 1: past an indirect
    /// buffer packet when the command processor fetches it, before the buffer runs; 2: the
    /// doorbell copies the submitted ring dwords and moves the read pointer to the write pointer
    /// at once). See ProcessCompute and gnmdriver.cpp sceGnmDingDong.
    static int AscRptrMode() noexcept;
    /// sotc: SOTC_FPS compute ring accounting, from sceGnmDingDong (guest thread, under the
    /// submit lock): the guest wrote the ring up to next_offs_dw with a doorbell of
    /// submitted_dw dwords.
    void NoteAscDoorbell(u32 vqid, u32 next_offs_dw, u32 submitted_dw) noexcept;

    bool IsGpuIdle() const {
        return num_submits == 0 &&
               back_tail.load(std::memory_order_acquire) == back_head.load(std::memory_order_acquire);
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        // sotc: from the command processor itself (it forwards the queue to the draw thread).
        if (back_threaded && std::this_thread::get_id() == gpu_id) {
            QueueBackFunc(Common::UniqueFunction<void>{std::move(func)}, true);
            if constexpr (wait_done) {
                DrainBack();
            }
            return;
        }
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([&sem, &func] {
                    func();
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            WakeBack();
            NotifyCpWait(); // sotc: SOTC_CP_IDLE_US (without the draw thread the CP runs it)
            sem.acquire();
        } else {
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace(std::move(func));
                ++num_commands;
                submit_cv.notify_one();
            }
            WakeBack();
            NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
        }
    }

    /// sotc: a command that must run at the command processor's point of the command stream,
    /// after the work it queued so far (SendCommand runs before that work with the draw thread:
    /// readbacks for guest threads do not wait for the queue).
    void SendOrderedCommand(Common::UniqueFunction<void>&& func) {
        if (!back_threaded) {
            SendCommand(std::move(func));
            return;
        }
        {
            std::scoped_lock lk{submit_mutex};
            ordered_queue.emplace(std::move(func));
            ++num_ordered;
            submit_cv.notify_one();
        }
        NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

    std::thread::id GetGpuCommandProcessorThread() {
        return gpu_id;
    }

    /// sotc: the draw thread (SOTC_CP_THREADS, on by default). The command processor parses the
    /// command buffers and resolves the pipeline of every draw and dispatch, and the draw thread
    /// binds and records them, in the same order: the pipeline lookup (~20% of the command
    /// processor time) runs while the previous draws are bound. Every other operation that
    /// touches the GPU state or writes guest memory for the GPU is queued to the draw thread as
    /// well, so it keeps the command stream order. The draw thread owns copies of the registers
    /// (updated from the chunks the command processor changed), of the compute registers and of
    /// the per-draw shader data (Shader::InfoRuntime, slot 1).
    bool IsBackThreaded() const noexcept {
        return back_threaded;
    }
    bool OnBackThread() const noexcept {
        return back_threaded && std::this_thread::get_id() == back_id;
    }
    /// sotc: SOTC_FLIP_SERVICE: runs the queued commands of the game's threads, when called on
    /// the draw thread between two operations' worth of state (Vulkan::Presenter::wait_service).
    void ServiceCommandsOnBackThread() {
        if (OnBackThread() && num_commands.load(std::memory_order_acquire)) {
            ProcessCommands();
        }
        RbxWaitService(); // sotc: SOTC_RB_EXACT (no-op when off)
    }
    /// The command processor or the draw thread: both work on the GPU state.
    bool IsGpuSideThread() const noexcept {
        const auto id = std::this_thread::get_id();
        return id == gpu_id || (back_threaded && id == back_id);
    }
    /// A fault on the command processor thread: the draw thread must be idle before the fault
    /// handler touches the GPU state (it may also produce what the faulting read wants).
    void PrepareGpuSideFault() {
        if (back_threaded && std::this_thread::get_id() == gpu_id) {
            DrainBack();
        }
    }
    /// Registers of the draw or dispatch being bound (the draw thread's copy on that thread).
    const Regs& DrawRegs() const noexcept {
        return OnBackThread() ? back_regs : regs;
    }
    const ComputeProgram& DrawCsRegs() noexcept {
        return OnBackThread() ? back_cs : GetCsRegs();
    }
    const std::array<CbDbExtent, NUM_COLOR_BUFFERS>& DrawCbExtent() const noexcept {
        return OnBackThread() ? back_cb_extent : last_cb_extent;
    }
    const CbDbExtent& DrawDbExtent() const noexcept {
        return OnBackThread() ? back_db_extent : last_db_extent;
    }
    /// Command processor: waits until the draw thread ran everything queued.
    void DrainBack();
    /// sotc: frees a finished nested command buffer task's frame once the draw thread has run
    /// the work queued until now (PR #219 freed it at once: garbage textures, heap corruption).
    void FreeTaskFrame(std::coroutine_handle<> frame);
    /// True on the command processor thread while the draw thread does the GPU work.
    bool RunsGpuWorkElsewhere() const noexcept {
        return back_threaded && std::this_thread::get_id() == gpu_id;
    }
    /// Command processor: runs func on the draw thread after what is queued, and waits for it.
    void RunOnBackThread(Common::UniqueFunction<void>&& func) {
        QueueBackFunc(std::move(func), true);
        DrainBack();
    }

#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId() {
        return gpu_tid;
    }
    /// sotc: IsGpuSideThread by Linux thread id (the userfaultfd handler thread asks for the
    /// faulting thread): a draw-thread write fault handled as a guest one would wait for the
    /// draw thread itself.
    bool IsGpuSideThreadId(u32 tid) const noexcept {
        return tid == gpu_tid || (back_threaded && tid == back_tid);
    }
#endif

    /// sotc: SOTC_DROP_FAST guard. True when [addr, addr + size) overlaps a command buffer the
    /// command processor has not finished reading (CmdRanges below). Any thread.
    bool CmdRangeInUse(VAddr addr, u64 size) const {
        // A submit whose walk hit a cap has incomplete ranges: fail closed while one is in flight.
        return cmd_overflow_live.load(std::memory_order_acquire) != 0 ||
               cmd_ranges.Intersects(addr, size);
    }

private:
    /// sotc: SOTC_DROP_FAST guard (CmdRangeInUse). The guest ranges the command processor still
    /// reads: each submitted command buffer until its task finishes (SubmitGfx/SubmitAsc to the
    /// pop in Process), and each nested indirect buffer while it is parsed (CmdRangeGuard). An
    /// unmap that skips the wait for the command processor must not free one of them: the parser
    /// would read unmapped (zero) memory and stop on "Unimplemented PM4 type 0". A handful of
    /// entries at a time; one small mutex, taken after GpuQueue::m_access when both are held.
    class CmdRanges {
    public:
        /// Only SOTC_DROP_FAST reads the ranges; with it unset nothing is recorded.
        static bool Enabled() {
            static const bool on = [] {
                const char* v = std::getenv("SOTC_DROP_FAST"); // sotc: off unless =1 (1 Nov)
                return v && v[0] == '1';
            }();
            return on;
        }
        /// A range of a submit (its DCB, CCB or ACB slice, or a nested buffer its walk found). It is
        /// also recorded in `submitted`, so a parser registration that is not in it can be counted.
        void AddSubmitted(const void* data, u64 bytes) {
            if (!Enabled() || data == nullptr || bytes == 0) {
                return;
            }
            const VAddr addr = reinterpret_cast<VAddr>(data);
            std::scoped_lock lock{mutex};
            ranges.emplace_back(addr, bytes);
            submitted.emplace_back(addr, bytes);
        }
        /// Removes one submit range (the pop in Liverpool::Process).
        void RemoveSubmitted(const void* data, u64 bytes) {
            if (!Enabled() || data == nullptr || bytes == 0) {
                return;
            }
            const VAddr addr = reinterpret_cast<VAddr>(data);
            std::scoped_lock lock{mutex};
            EraseOne(ranges, addr, bytes);
            EraseOne(submitted, addr, bytes);
        }
        /// A nested indirect buffer the parser is about to read (CmdRangeGuard). Counted as late
        /// when the submit-time walk did not register it.
        void AddNested(const void* data, u64 bytes) {
            if (!Enabled() || data == nullptr || bytes == 0) {
                return;
            }
            const VAddr addr = reinterpret_cast<VAddr>(data);
            std::scoped_lock lock{mutex};
            bool in_submit = false;
            for (const auto& [lo, n] : submitted) {
                if (lo == addr && n == bytes) {
                    in_submit = true;
                    break;
                }
            }
            if (!in_submit) {
                SotcStats::drop_fast_late_ibs.fetch_add(1, std::memory_order_relaxed);
            }
            ranges.emplace_back(addr, bytes);
        }
        void Remove(const void* data, u64 bytes) {
            if (!Enabled() || data == nullptr || bytes == 0) {
                return;
            }
            const VAddr addr = reinterpret_cast<VAddr>(data);
            std::scoped_lock lock{mutex};
            EraseOne(ranges, addr, bytes);
        }
        bool Intersects(VAddr addr, u64 size) const {
            // Ends saturate at the top of the address space, so a wrapping range overlaps
            // everything above its start instead of nothing.
            const auto end_of = [](VAddr at, u64 n) -> VAddr {
                return n > ~VAddr{0} - at ? ~VAddr{0} : at + n;
            };
            const VAddr end = end_of(addr, size);
            std::scoped_lock lock{mutex};
            for (const auto& [lo, bytes] : ranges) {
                if (addr < end_of(lo, bytes) && lo < end) {
                    return true;
                }
            }
            return false;
        }

    private:
        /// Erases one entry equal to (addr, bytes), if there is one. Caller holds `mutex`.
        static void EraseOne(std::vector<std::pair<VAddr, u64>>& v, VAddr addr, u64 bytes) {
            for (auto it = v.begin(); it != v.end(); ++it) {
                if (it->first == addr && it->second == bytes) {
                    *it = v.back();
                    v.pop_back();
                    return;
                }
            }
        }
        mutable std::mutex mutex;
        std::vector<std::pair<VAddr, u64>> ranges;
        /// The submit-time entries of `ranges` (AddSubmitted), for the late-IB diagnostic only.
        std::vector<std::pair<VAddr, u64>> submitted;
    };
    /// sotc: holds a nested indirect buffer in CmdRanges while its task is parsed (a local of the
    /// parent coroutine, so it lives across the parent's yields).
    struct CmdRangeGuard {
        CmdRangeGuard(CmdRanges& ranges_, const void* data_, u64 bytes_)
            : ranges{ranges_}, data{data_}, bytes{bytes_} {
            ranges.AddNested(data, bytes);
        }
        ~CmdRangeGuard() {
            ranges.Remove(data, bytes);
        }
        CmdRangeGuard(const CmdRangeGuard&) = delete;
        CmdRangeGuard& operator=(const CmdRangeGuard&) = delete;
        CmdRanges& ranges;
        const void* data;
        u64 bytes;
    };
    CmdRanges cmd_ranges;
    /// sotc: submits in flight whose walk hit a cap (SubmitRanges::overflowed). CmdRangeInUse
    /// answers true while this is not zero.
    std::atomic<u32> cmd_overflow_live{0};
    /// The CmdRanges entries of one submit: the DCB and CCB (or the ACB slice), plus every nested
    /// indirect buffer they call, found at submit time (CollectNestedIbs). overflowed: a cap of
    /// the walk was hit, so the list may miss ranges.
    struct SubmitRanges {
        std::vector<std::pair<const void*, u64>> list;
        bool overflowed{};
    };
    /// sotc: SOTC_DROP_FAST guard. Walks a command buffer's packets at submit time and appends
    /// the ranges of the indirect buffers it calls (recursively, capped), so they are guarded from
    /// the submit on and not only once the parser reaches them (reports2/01-cmdbuf-lifetime.md).
    /// Nested buffers whose memory is not mapped are skipped (counted); a repeated one is walked
    /// once per submit.
    static void CollectNestedIbs(std::span<const u32> cb, SubmitRanges& out, u32 depth);

    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            /// sotc: SOTC_TASK_GUARD=1 gives every task frame pages of its own that become
            /// inaccessible when it is freed (and stay so for a while): a later read of a freed
            /// frame faults where it happens (hunting the garbage textures of PR #219).
            static void* operator new(std::size_t size);
            static void operator delete(void* frame, std::size_t size);
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    void ProcessCommands();
    /// Writes an EOP/EOS fence value, keeping GPU caches of that memory coherent.
    void WriteFenceValue(void* address, u64 data, u32 num_bytes);
    /// sotc: SOTC_NATIVE=1: the command processor wrote guest memory itself; tells the caches what the
    /// write fault of mode 0 told them (Rasterizer::NoteHostWrite). A no-op in the other modes.
    void NoteHostWrite(const void* address, u64 size, bool allow_exact = true);
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        std::queue<SubmitRanges> submit_ranges{}; ///< sotc: CmdRanges entries, in step with submits
        ComputeProgram cs_state{};
        // sotc: lock-free hint for Process()'s round-robin so it can skip m_access entirely for
        // queues with nothing queued, instead of locking every queue every pass just to find
        // most of them empty. Set true under the lock on push, re-synced under the lock on pop;
        // read without the lock as a hint only (a stale false just defers that queue to the next
        // pass - num_submits keeps the outer loop going - a stale true costs one wasted lock).
        //
        // A single shared bitmask + bit-scan (std::countr_zero) to jump straight to the next
        // queue with work, instead of this per-queue flag, measured *worse* here: it drains each
        // batch faster, so submit_cv parks and wakes this thread more often for the same work,
        // and each wake is a costlier kernel round-trip on Windows than the iterations it saves
        // (own-code share 23.3%->16%, but ntdll wait share 37%->45%, reproduced over 2 runs).
        // Keep the simpler per-queue hint; see git history for the bitmask version and numbers.
        std::atomic<bool> has_work{false};
        // sotc: SOTC_ASC_RPTR=2: the ring dwords each queued compute submit runs from, one per
        // entry of `submits` (same order, under m_access); freed when its task finishes.
        std::queue<std::vector<u32>> owned{};
    };
    /// sotc: SOTC_ASC_RPTR mode (constructor) and its read pointer update, ProcessCompute.
    int asc_rptr_mode{};
    void NoteAscRptr(u32 vqid, const AscQueueInfo& queue) noexcept;
    /// sotc: SOTC_FPS compute ring accounting per vqid. A ring is "full" once the free space
    /// after a doorbell is smaller than that doorbell's size, so a like-sized next submit would
    /// spin in the game's retry loop (eboot 0x863a50).
    struct AscRingStat {
        std::atomic<u32> wptr_dw{0};   ///< the guest's write offset after its last doorbell
        std::atomic<u32> need_dw{0};   ///< dwords the last doorbell submitted
        std::atomic<u64> full_ns{0};   ///< when the ring became full (0: not full)
        std::atomic<u64> freed_ns{0};  ///< when the command processor last freed a full ring
        std::atomic<u64> window_ns{0}; ///< full_ns of that window
    };
    std::array<AscRingStat, NumComputeRings> asc_ring_stats{};

    // sotc: SOTC_CP_AHEAD=2 frame tickets (see gnmdriver.cpp WaitSubmitGate). A mark holds, per
    // queue, how many submits were enqueued when the guest called SubmitDone; the frame is
    // parsed once every queue completed that many tasks (tasks of one queue finish in order).
    struct CpFrameMark {
        u64 frame{};
        std::array<u64, NumTotalQueues> enqueued{};
    };
    bool frame_tickets{};
    std::array<u64, NumTotalQueues> q_enqueued{};  ///< under submit_mutex
    std::array<u64, NumTotalQueues> q_completed{}; ///< command processor thread only
    u64 q_done_epoch{};                            ///< command processor: tasks finished
    std::deque<CpFrameMark> frame_marks;           ///< under submit_mutex
    std::atomic<u64> frame_mark_epoch{0};          ///< bumped by PushFrameMark
    u64 fm_seen_epoch{}, fm_seen_done{};           ///< command processor: last check
    std::atomic<u64> frames_marked{0};
    std::atomic<u64> submit_done_seq{0}; ///< sotc: SubmitDone calls (SOTC_PC_RING8_FLIPWAIT=2)
    std::atomic<u64> frames_recorded{0};
    std::mutex frames_mutex;
    std::condition_variable frames_cv;
    /// Under submit_mutex (SubmitDone).
    void PushFrameMark();
    /// Command processor: queues the frame end of every mark whose submits are all parsed.
    void ServiceFrameMarks();
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    const bool guest_markers_enabled;
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::queue<Common::UniqueFunction<void>> ordered_queue{}; // sotc: SendOrderedCommand
    std::atomic<u32> num_ordered{};
    std::thread::id gpu_id;
#ifdef __linux__
    u32 gpu_tid;
    u32 back_tid{}; // sotc: the draw thread's (IsGpuSideThreadId)
#endif
    s32 curr_qid{-1};

    // sotc: the draw thread (see IsBackThreaded).
    struct BackStage {
        Shader::Info* info;
        std::array<u32, 16> user_data; // ShaderParams::NumShaderUserData
        u32 num_user_data; // ~0u: no user data bound
        VAddr pgm_base;
        std::vector<u32> flat;
    };
    /// sotc: SOTC_WD_FUSE. Small WRITE_DATA writes (<= WdMaxBytes in all, <= WdMaxSegs packets)
    /// stored inline in an op instead of a Func closure: applied by the draw thread, in order,
    /// right before the op's own work (see Liverpool::FuseWriteData).
    static constexpr u32 WdMaxBytes = 64;
    static constexpr u32 WdMaxSegs = 4;
    struct BackWd {
        struct Seg {
            VAddr address;
            u8 offset;
            u8 length;
        };
        u8 num_segs{};
        u8 used{};
        std::array<Seg, WdMaxSegs> segs;
        alignas(4) std::array<u8, WdMaxBytes> data;
    };
    struct BackOp {
        enum class Kind : u8 { Func, Draw, DrawIndirect, Dispatch, DispatchIndirect };
        Kind kind{};
        u8 func_kind{3}; ///< sotc: SOTC_FPS accounting: 0 write data, 1 EOP/EOS/release, 2 DMA, 3 other
        BackWd wd; ///< sotc: SOTC_WD_FUSE: applied before the op (num_segs == 0: none)
        bool is_indexed{};
        u8 prepass_mark{}; ///< sotc: SOTC_NO_PREPASS=3 (SotcPrepassProbe::CpClassify): 1 drop, 2+ promote
        u16 vertex_sgpr_offset{};
        u16 instance_sgpr_offset{};
        u32 index_offset{};
        u32 offset{};
        u32 stride{};
        u32 max_count{};
        u32 size{};
        VAddr address{};
        VAddr count_address{};
        const Vulkan::Pipeline* pipeline{};
        std::vector<u32> reg_chunks; // chunk index followed by its RegChunkDwords dwords
        std::array<CbDbExtent, NUM_COLOR_BUFFERS> cb_extent{};
        CbDbExtent db_extent{};
        ComputeProgram cs{};
        std::array<BackStage, 5> stages{};
        u32 num_stages{};
        Common::InlineFunction func; // sotc: SOTC_INLINE_FUNC (heap unless on)
        // sotc: SOTC_CULL_EXPRESS (Liverpool::XpPoll). qid: the queue whose task parsed the op
        // (curr_qid). xp_cls/xp_*: what a Func op does to guest memory, set by the parser right
        // before it queues the closure (XpHint); XpUnknown blocks any reorder across the op.
        // xp_done: the express lane already ran the op; BackLoop then only replays its state.
        u8 xp_cls{};
        u8 qid{};
        bool xp_done{};
        VAddr xp_raddr{}, xp_waddr{};
        u64 xp_rsize{}, xp_wsize{};
    };
    static constexpr u32 RegChunkDwords = 64;
    static constexpr u32 NumRegChunks = Regs::NumRegs / RegChunkDwords;
    static constexpr u64 BackRingSize = 512;

    template <typename F>
    void BackOrRun(F&& func, bool writes_memory = true) {
        if (!back_threaded) {
            func();
            return;
        }
        QueueBackFunc(Common::InlineFunction{std::forward<F>(func)}, writes_memory);
    }
    void QueueBackFunc(Common::InlineFunction&& func, bool writes_memory);
    /// sotc: SOTC_BARRIER_GUEST. Queues a guest sync point (SotcArch::GuestBarrier::Sync as u8) in the op stream; the
    /// draw thread records its barrier there. Nothing is queued when the switch is off.
    void QueueGuestSync(u8 kind);

    // sotc: SOTC_WD_FUSE (command processor thread only). WRITE_DATA packets wait here instead of
    // being queued while only packets that neither read nor write guest memory, nor queue an op,
    // follow (FuseTransparent). An indirect draw or dispatch whose argument range contains all of
    // them takes them along (AttachWd); everything else queues them first as one Func-less op
    // (FlushWd), which is what separate Func ops did.
    bool wd_fuse{};
    BackWd wd_pending{};
    u8 func_kind_hint{3}; ///< kind of the packet being parsed (BackOp::func_kind)
    u64 wd_last_seq{};    ///< SOTC_FPS: back_head after the last WRITE_DATA op (pair detection)
    /// Queues a WRITE_DATA, or holds it back for the next indirect draw. False: the caller
    /// queues it as a Func.
    bool FuseWriteData(VAddr address, const u32* data, u32 data_size);
    void FlushWd();
    /// Before an indirect draw or dispatch reading args [begin, begin + span): keeps the pending
    /// writes when they all lie inside, flushes them otherwise.
    void PrepareWdForIndirect(VAddr begin, u64 span, bool allowed);
    void AttachWd(BackOp& op);
    /// SOTC_FPS: counts an indirect draw/dispatch right after a WRITE_DATA (held back or queued).
    void NotePairs();
    void ApplyBackWd(const BackWd& wd);
    void WaitBackWritesSlow();
    void SubmitDraw(bool is_indexed, u32 index_offset);
    /// sotc: prepass_mark >= 0: the SOTC_NO_PREPASS=3 mark of the op, already decided (SubmitIndirectRun).
    void SubmitDrawIndirect(bool is_indexed, VAddr address, u32 offset, u32 stride, u32 max_count,
                            VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset,
                            int prepass_mark = -1);
    /// sotc: SOTC_DRAW_COALESCE. A run of DRAW_INDEX_INDIRECT packets with nothing between them (one state): each
    /// stretch of consecutive records (offset step 0x14, one SOTC_NO_PREPASS=3 mark) is one op of max_count draws.
    void SubmitIndirectRun(VAddr address, std::span<const u32> offsets, u16 vertex_sgpr_offset,
                           u16 instance_sgpr_offset);
    void SubmitDispatch(bool indirect, VAddr address, u32 offset, u32 size);
    /// sotc: SOTC_ASYNC_COMPUTE_XLATE: when the current dispatch's compute shader is new,
    /// queues the new compute shaders of the next dispatches in rest (the packets after it)
    /// for the pipeline workers (see Vulkan::PipelineCache::ComputePrefetchEnabled).
    void PrefetchComputeAhead(std::span<const u32> rest);
    BackOp& BackAlloc();
    /// gpu_write: the op writes guest memory on the GPU even when writes_memory is false (a
    /// pipeline that may write, with SOTC_CP_STRICT off). Feeds back_gpu_write_seq only.
    void BackPublish(bool writes_memory, bool gpu_write = false);
    void CaptureBackOp(BackOp& op, const Vulkan::Pipeline* pipeline);
    /// Before reading guest memory the queued work may write (pipeline lookups read the SRTs):
    /// waits for the last queued op that writes guest memory from the CPU (WRITE_DATA, DMA), or
    /// with SOTC_CP_STRICT=1 also for GPU writes (dispatches, storage writes, fences). GPU-written
    /// memory the command processor reads is read-protected once the draw thread recorded the
    /// writer, and its faults run on the draw thread after that; the SRTs and sharp tables are
    /// written by the CPU.
    void WaitBackWrites() {
        if (back_threaded && back_tail.load(std::memory_order_acquire) < back_write_seq) {
            WaitBackWritesSlow(); // sotc: SOTC_FPS accounting around WaitBackTail
        }
    }
    void WaitBackTail(u64 target);
    void MarkRegs(u32 first_dword, u32 num_dwords) {
        if (!back_threaded || num_dwords == 0) {
            return;
        }
        const u32 last = std::min(first_dword + num_dwords - 1, Regs::NumRegs - 1);
        for (u32 chunk = first_dword / RegChunkDwords; chunk <= last / RegChunkDwords; ++chunk) {
            const u64 bit = u64{1} << (chunk % 64);
            if (!(dirty_chunks[chunk / 64] & bit)) {
                dirty_chunks[chunk / 64] |= bit;
                dirty_list.push_back(static_cast<u16>(chunk));
            }
        }
    }
    void MarkRegs(const void* field, size_t bytes) {
        const auto offset = static_cast<u32>(reinterpret_cast<const u32*>(field) -
                                             regs.reg_array.data());
        MarkRegs(offset, static_cast<u32>((bytes + 3) / 4));
    }
    /// sotc: SOTC_REG_SAME (liverpool.cpp). Copies dwords from src to regs at dst and marks the
    /// chunks dirty; with SOTC_REG_SAME=1 nothing is copied or marked when the words are unchanged.
    void WriteRegsMarked(u32 dst, const void* src, u32 dwords);
    void ExecuteBackOp(BackOp& op);
    void BackLoop(std::stop_token stoken);
    /// sotc: SOTC_EOP_TICK=1. Runs the end-of-pipe labels the GPU has finished, and flushes the
    /// open command buffer when a label has waited too long for it. Draw thread only.
    void EopService();
    /// sotc: SOTC_EOP_TICK=1. Called by the command processor while it waits on memory: queues
    /// EopService on the draw thread once the oldest label has waited too long.
    void EopWaitGuard();
    void WakeBack() {
        if (back_threaded && futex_wait) {
            // sotc: SOTC_FUTEX_WAIT: BackLoop blocks on back_wake (WaitOnAddress); bump it when asleep.
            if (back_sleeping.load(std::memory_order_seq_cst)) {
                WakeBackFutex();
            }
            return;
        }
        if (back_threaded && back_nospin) {
            // sotc: SOTC_WAIT_NOSPIN: BackLoop blocks on back_wake; bump it only when it sleeps.
            if (back_sleeping.load(std::memory_order_seq_cst)) {
                WakeBackAtomic();
            }
            return;
        }
        if (back_threaded) {
            std::scoped_lock lk{back_mutex};
            back_cv.notify_one();
        }
    }
    /// sotc: changes back_wake and wakes BackLoop. Called after the state BackLoop waits on changed.
    void WakeBackAtomic() {
        back_wake.fetch_add(1, std::memory_order_seq_cst);
        back_wake.notify_one();
    }
    /// sotc: SOTC_FUTEX_WAIT: bumps back_wake and wakes the one BackLoop waiter (liverpool.cpp,
    /// Windows; a no-op elsewhere, the switch is Windows-only).
    void WakeBackFutex();

    bool back_threaded{};
    bool back_strict{};
    /// sotc: SOTC_WAIT_NOSPIN=1: BackLoop spins ~20 us, then blocks on back_wake (not back_cv).
    bool back_nospin{};
    /// sotc: SOTC_FUTEX_WAIT=1 (Windows only, off by default): the command processor waits on
    /// back_tail and the draw thread blocks on back_wake with WaitOnAddress (liverpool.cpp).
    /// Overrides back_nospin for the draw thread idle wait; the rest of the switches are unchanged.
    bool futex_wait{};
    // sotc: switches read in the constructor (liverpool.cpp), all OFF by default:
    // SOTC_BACK_HEAD_CACHE (draw thread caches back_head), SOTC_BACK_PF (prefetch of the next
    // op's register chunks), SOTC_REG_SAME (register writes that change nothing are not marked,
    // WriteRegsMarked), SOTC_OPSTAT (counters printed every 5 s).
    bool back_head_cache{};
    bool back_pf{};
    bool reg_same{};
    bool opstat{};
    /// sotc: doorbell BackLoop blocks on under back_nospin; see WakeBackAtomic.
    std::atomic<u32> back_wake{0};
    std::thread::id back_id{};
    std::vector<BackOp> back_ring;
    // sotc: the command processor writes back_head and the draw thread writes back_tail on every
    // op. Each gets its own cache line (layout only, no behaviour change), so the two per-op stores
    // do not bounce one line between the cores. back_sleeping is read per op by the command
    // processor (BackPublish) and stays with back_head; drain_waiting is read per op by the draw
    // thread and stays with back_tail.
    alignas(64) std::atomic<u64> back_head{0};
    std::atomic<bool> back_sleeping{false};
    /// sotc: finished task frames and the back_head they wait for (FreeTaskFrame)
    std::vector<std::pair<std::coroutine_handle<>, u64>> finished_frames;
    alignas(64) std::atomic<u64> back_tail{0};
    std::atomic<bool> drain_waiting{false};
    /// sotc: SOTC_BACK_HEAD_CACHE: the last back_head the draw thread loaded (draw thread only).
    u64 draw_known_head{};
    std::mutex back_mutex;
    std::condition_variable back_cv;
    std::condition_variable drain_cv;
    u64 back_write_seq{0};
    // sotc: SOTC_CP_PRECISE_WAIT (liverpool.cpp; 0 off, 1 precise, 2 precise and verified). Before a
    // pipeline lookup the command processor waits only for the queued ops whose guest writes overlap
    // the ranges the lookup read (CpCollectReads, read after the lookup; a lookup is retried after
    // the wait when it overlapped a pending write). Every publish that writes memory records its
    // range in cp_wr_ring: the range given by NoteBackWriteRange, else unknown (the old wait).
    /// sotc: SOTC_CP_PRECISE_WAIT: why a lookup waits for the back tail (the old wait when not None).
    enum class CpUnknown : u8 {
        None,           // a known range: the wait covers the overlapping write only
        NullPipeline,   // a stage is still translating: its reads are not known (CpCollectReads)
        SrtWalker,      // the SRT walker reads at unbounded places (CpCollectReads)
        TessConsts,     // the tessellation constants are read at a place not recorded (CpCollectReads)
        RangeTableFull, // more read ranges than CpMaxReadRanges (CpCollectReads)
        RingOverflow,   // a pending range was overwritten in cp_wr_ring (CpWaitTarget)
        WriteHeldWd,    // a queued op with held-back WRITE_DATA that may write anything (RecordBackWrite)
        WriteNoRange,   // a queued publish with no NoteBackWriteRange range (RecordBackWrite)
        SrtPlanTooBig,  // the SRT walk plan needs more read ranges than CpMaxReadRanges (CpCollectSrtPlan)
        Count,
    };
    static constexpr u32 CpWriteRingSize = 256;
    static constexpr u32 CpMaxReadRanges = 48;
    static constexpr u32 CpMaxAttempts = 3;
    /// Bound of the fetch shader code a lookup reads (its scan runs to S_SETPC; see the report).
    static constexpr u64 CpFetchShaderBytes = 4096;
    struct CpWriteRange {
        u64 seq{};
        VAddr addr{};
        u64 size{};
        CpUnknown why{}; ///< sotc: CpUnknown::None: a known range; otherwise the write is unknown
    };
    struct CpReadSet {
        std::array<std::pair<VAddr, u64>, CpMaxReadRanges> ranges{};
        u32 count{};
    };
public:
    /// sotc: SOTC_CP_PRECISE_WAIT counters, read and reset by the 5 s report (driver.cpp).
    struct CpPreciseStats {
        static inline std::atomic<u64> avoided{}; ///< lookups that ran while writes were pending
        static inline std::atomic<u64> taken_overlap{}; ///< waits for an overlapping known range
        static inline std::atomic<u64> taken_unknown{}; ///< waits for an unknown range or reads
        static inline std::atomic<u64> retries{};       ///< lookups repeated after a wait
        static inline std::atomic<u64> fallback{};      ///< lookups that gave up, old wait
        static inline std::atomic<u64> verified{};      ///< =2: lookups compared with the old wait
        static inline std::atomic<u64> violations{};    ///< =2: results that differed
        static inline std::atomic<u64> ns_blocked{};    ///< time blocked in the precise waits
        static constexpr u32 kUnknownReasons = u32(CpUnknown::Count); ///< CpUnknown values, None included
        static inline std::array<std::atomic<u64>, kUnknownReasons> unknown_n{}; ///< unknown waits by reason
        static inline std::array<std::atomic<u64>, kUnknownReasons> unknown_ns{}; ///< their blocked time
        /// sotc: SRT walker programs (pgm_hash) that a lookup hit; the report takes and resets them.
        static void NoteSrtWalker(u64 pgm_hash);
        static void TakeSrtWalkers(std::vector<std::pair<u64, u64>>& top, u64& distinct, u64& dropped);
        // sotc: SOTC_CP_SRT_PLAN: lookups that took a walker program's plan, lookups whose plan was too big,
        // and (=2) walker reads outside the plan.
        static inline std::atomic<u64> srt_plan_used{};
        static inline std::atomic<u64> srt_plan_big{};
        static inline std::atomic<u64> srt_plan_misses{};
        /// sotc: SOTC_CP_SRT_PLAN=2: the first programs (pgm_hash) whose walker read outside the plan.
        static void NoteSrtPlanMiss(u64 pgm_hash);
        static void TakeSrtPlanMisses(std::vector<u64>& hashes);
    };

private:
    u8 cp_precise{};
    std::array<CpWriteRange, CpWriteRingSize> cp_wr_ring{};
    u64 cp_wr_count{};
    u64 cp_wr_dropped_seq{}; ///< newest pending range overwritten in the ring (0: none)
    VAddr cp_note_addr{};
    u64 cp_note_size{};
    u64 cp_note_seq{}; ///< back_head + 1 at NoteBackWriteRange: the publish it describes
    /// Command processor, before the op that publishes the writes: the guest range they write
    /// (size 0 or no call: unknown). Only the next publish uses it.
    void NoteBackWriteRange(VAddr addr, u64 size) {
        if (!cp_precise) {
            return;
        }
        cp_note_addr = addr;
        cp_note_size = size;
        cp_note_seq = back_head.load(std::memory_order_relaxed) + 1;
    }
    /// Command processor, before a publish with writes_memory set that writes no guest memory
    /// (the op only touches host state): it adds no range to cp_wr_ring.
    void NoteBackNoGuestWrite() {
        if (!cp_precise) {
            return;
        }
        cp_note_addr = 0;
        cp_note_size = CpNoGuestWrite;
        cp_note_seq = back_head.load(std::memory_order_relaxed) + 1;
    }
    static constexpr u64 CpNoGuestWrite = ~u64{0};
    void RecordBackWrite(u64 head, bool writes_memory);
    void PushWriteRange(u64 seq, VAddr addr, u64 size, CpUnknown why);
    /// False when the lookup's reads are not all bounded (or there is no pipeline to read them).
    bool CpCollectReads(const Vulkan::Pipeline* pipeline, CpReadSet& reads, CpUnknown& why) const;
    /// sotc: SOTC_CP_SRT_PLAN: the read ranges of a walker program from its walk plan (CpCollectReads).
    bool CpCollectSrtPlan(const Shader::Info& info, CpReadSet& reads, CpUnknown& why) const;
    /// The back_tail to wait for before the lookup's result may be used: 0 when no queued write
    /// newer than snap overlaps what the lookup read.
    u64 CpWaitTarget(const Vulkan::Pipeline* pipeline, u64 snap, CpUnknown& why);
    void CpWaitFor(u64 target, CpUnknown why);
    void CpDigest(const Vulkan::Pipeline* pipeline, std::vector<u64>& out) const;
    /// The pipeline lookup of a draw or dispatch, after the waits its guest reads need.
    /// looks_up false: no lookup runs (filtered draw), nothing is read.
    template <typename F>
    const Vulkan::Pipeline* LookupAfterWrites(bool looks_up, F&& lookup) {
        if (!cp_precise) {
            WaitBackWrites();
            return looks_up ? lookup() : nullptr;
        }
        if (!looks_up) {
            return nullptr;
        }
        for (u32 attempt = 0; attempt < CpMaxAttempts; ++attempt) {
            const u64 snap = back_tail.load(std::memory_order_acquire);
            if (snap >= back_write_seq) { // nothing pending: the old wait returns at once too
                return lookup();
            }
            if (attempt != 0) {
                CpPreciseStats::retries.fetch_add(1, std::memory_order_relaxed);
            }
            const Vulkan::Pipeline* pipeline = lookup();
            CpUnknown why = CpUnknown::None;
            const u64 target = CpWaitTarget(pipeline, snap, why);
            if (target == 0) {
                CpPreciseStats::avoided.fetch_add(1, std::memory_order_relaxed);
                if (cp_precise != 2) {
                    return pipeline;
                }
                // =2: the old wait and the lookup again; the result and the state the draw
                // captures from it must be the same. The old result is the one used.
                std::vector<u64> first;
                std::vector<u64> second;
                CpDigest(pipeline, first);
                WaitBackWrites();
                const Vulkan::Pipeline* old_pipeline = lookup();
                CpDigest(old_pipeline, second);
                CpPreciseStats::verified.fetch_add(1, std::memory_order_relaxed);
                if (old_pipeline != pipeline || first != second) {
                    CpPreciseStats::violations.fetch_add(1, std::memory_order_relaxed);
                }
                return old_pipeline;
            }
            CpWaitFor(target, why); // the lookup may have read a write still pending
        }
        CpPreciseStats::fallback.fetch_add(1, std::memory_order_relaxed);
        WaitBackWrites();
        return lookup();
    }
    /// sotc: SOTC_CP_LABEL_SKIP. back_head of the newest publish that may write guest memory on
    /// the GPU or the CPU (every writes_memory publish, every pipeline that may write, and every
    /// non-recorded label). Recorded labels do not count: a skipped wait is for one of them.
    /// Always >= back_write_seq.
    u64 back_gpu_write_seq{0};
    Regs back_regs{};
    ComputeProgram back_cs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> back_cb_extent{};
    CbDbExtent back_db_extent{};
    std::array<u64, (NumRegChunks + 63) / 64> dirty_chunks{};
    std::vector<u16> dirty_list;
    std::jthread back_thread{};
    u64 back_exec_seq{}; ///< sotc: the op ExecuteBackOp runs (draw thread)

    /// sotc: SOTC_PARALLEL_RECORD=1 (default off; needs the draw thread and more than 6
    /// hardware threads). prep_workers GpuPrep threads (SOTC_PARALLEL_RECORD_WORKERS, default
    /// clamp(threads / 4, 1, 2)) follow the back ring: each replays the registers into its own
    /// prep_regs (sotc: design E0, lazily: up to an op it computes, each chunk once, see PrepLoop)
    /// and computes the register-only inputs (draw_prep.h) of the draws with
    /// seq % prep_workers == its index into prep_ring[seq % BackRingSize]. BackAlloc reuses a
    /// slot only when the draw thread and every prep thread are past it (MinRingTail).
    static constexpr u32 MaxPrepWorkers = 2;
    u32 prep_workers{};
    /// the oldest op each still reads (sotc: design E0: its first op not replayed yet)
    std::array<std::atomic<u64>, MaxPrepWorkers> prep_tail{};
    std::atomic<u32> prep_wake{0};     ///< doorbell of sleeping prep threads (BackPublish)
    std::atomic<u32> prep_sleepers{0}; ///< prep threads blocked on prep_wake
    std::unique_ptr<Vulkan::PreparedDraw[]> prep_ring;
    std::vector<std::unique_ptr<Regs>> prep_regs;
    std::vector<std::jthread> prep_threads;
    void PrepLoop(std::stop_token stoken, u32 index);
    /// The oldest op still in use: by the draw thread (back_tail) or a prep thread.
    u64 MinRingTail(std::memory_order order) const {
        u64 tail = back_tail.load(order);
        for (u32 k = 0; k < prep_workers; ++k) {
            tail = std::min(tail, prep_tail[k].load(order));
        }
        return tail;
    }
    /// Draw thread: the prepared inputs of op seq when they are usable, else null (counted).
    /// dispatch: a compute op; its take is not counted in the draw counters (pr_*).
    const Vulkan::PreparedDraw* TakePrepared(u64 seq, bool dispatch = false);
    /// sotc: design E: the SOTC PREP line (draw thread, every 5 s; Vulkan::g_prep_counters).
    void PrepReport();

    /// sotc: SOTC_EOP_TICK=1 (needs back_threaded). End-of-pipe labels wait for the GPU through
    /// the scheduler's deferred operations instead of being written when the command processor
    /// reads them. Off by default.
    bool eop_tick{};
    /// Labels queued and not yet written. Written on the draw thread, read by the CP guard.
    std::atomic<u32> eop_outstanding{0};
    /// Steady clock (ns) at which eop_outstanding went from 0 to 1; 0 when none is outstanding.
    std::atomic<u64> eop_oldest_ns{0};
    /// Scheduler tick of the newest queued label. Equal to CurrentTick() while that label's work
    /// is still in the command buffer being recorded, that is not yet submitted.
    std::atomic<u64> eop_last_tick{0};
    /// sotc: SOTC_EOP_TICK: values queued labels will receive (u32 address -> value), so a
    /// WaitRegMem of the command stream on such a label does not wait for the host GPU: the host
    /// queue runs the stream in order anyway. Guest CPU threads still see the label only once
    /// the GPU has finished (the protection the switch is for).
    std::mutex eop_shadow_mutex;
    std::unordered_map<VAddr, u32> eop_shadow;
    std::atomic<u32> eop_shadow_count{0};
    /// sotc: true when the pending value of a queued label satisfies this wait.
    bool EopShadowSatisfies(const PM4CmdWaitRegMem& wait);

    // sotc: SOTC_CP_LABEL_SKIP=1 (skip) or 2 (count only), off by default. A WAIT_REG_MEM on a
    // label this command processor queued earlier is treated as satisfied while the draw thread
    // has not run that label yet, when the label's value satisfies the wait. Only labels of the
    // same queue count, and only while no other queued op came after them (label_barrier). The
    // CP-side records below are touched by the command processor thread only.
    static constexpr u32 LabelQueues = 8;   ///< 0 graphics, 1 + vqid for compute (vqid < 7)
    static constexpr u32 LabelRecs = 8;     ///< newest queued labels kept per queue
    struct LabelRec {
        VAddr addr{};
        u32 value{};
        u64 seq{}; ///< back_head after the label was published (0: empty slot)
    };
    u8 label_skip{};                                   ///< 0 off, 1 skip, 2 count only
    /// back_head of the newest publish that is not a recorded label of queue q.
    std::array<u64, LabelQueues> label_barrier{};
    std::array<std::array<LabelRec, LabelRecs>, LabelQueues> label_recs{};
    std::array<u32, LabelQueues> label_next{};
    static u32 LabelQueueOf(u32 vqid) {
        return vqid + 1 < LabelQueues ? vqid + 1 : LabelQueues;
    }
    /// Records a label the queue just published (prev_barrier: label_barrier[q] before it).
    /// recordable false: the publish stays a barrier for every label of q.
    void NoteQueuedLabel(u32 q, u64 prev_barrier, bool recordable, VAddr addr, bool dword64,
                         u32 lo, u32 hi);
    /// label_barrier[q] as it is after the FlushWd that QueueBackFunc may run before the label
    /// op (that flush is a publish too). Command processor thread, before BackOrRun.
    u64 LabelPrevBarrier(u32 q);
    /// True when the wait on a not-yet-satisfied label may be treated as satisfied: the queued
    /// ops that may write guest memory (back_gpu_write_seq) are waited for first. Mode 2: counts,
    /// returns false.
    bool LabelWaitSkip(u32 q, const PM4CmdWaitRegMem& wait, bool vo_label);
    /// A EopService call is queued on the draw thread and has not run yet.
    std::atomic<bool> eop_service_requested{false};

    // sotc: SOTC_RB_EXACT=1, label side (rules 5-7 of the design; liverpool.cpp has the long
    // comment). Needs the draw thread. rbx: the switch is on. eop_poll: the deferred label queue
    // is in use (SOTC_EOP_TICK or rbx), so BackLoop polls it and the CP waits use eop_shadow and
    // EopWaitGuard.
    bool rbx{};
    bool eop_poll{};
    /// Hot-write sequence seen at the previous label of each queue (0 gfx, 1 compute); draw
    /// thread only.
    std::array<u64, 2> rbx_last_seq{};
    /// Deferred labels outstanding per 8-byte aligned address (per-address FIFO).
    std::mutex rbx_mutex;
    std::unordered_map<VAddr, u32> rbx_out;
    bool RbxAddrOutstanding(VAddr label); ///< sotc: SOTC_RB_EXACT=publish per-address FIFO
    /// Draw thread, at the label's position in the stream: snapshots HotWriteSeq into snap and
    /// returns true when the label must wait for the GPU.
    bool RbxDecide(u32 queue, VAddr label, u64& snap, bool* hot = nullptr);
    /// Reports a label (or other CPU-visible GPU write) that has just reached guest memory.
    void RbxNoteWritten(u64 snap);
    template <typename F>
    bool RbxTryDefer(u32 queue, VAddr addr, bool shadow32, u32 value, u64& snap, F&& make);
    /// A deferred label: lands (RbxDrain / LandLabel) in queue order once its tick is done.
    struct RbxJob {
        VAddr label{}, key{};
        bool shadow32{}, shadow64{};
        u32 lo{}, hi{}, queue{};
        u64 snap{}, queued_ns{}, tick{};
        std::atomic<bool> ready{false};
        // sotc: SOTC_PC_RING8_LABELS: set when the job lands (LandOnce), so it lands only once even
        // when the watchdog forces it first.
        std::atomic<bool> claimed{false};
        Common::UniqueFunction<void> write;
        // sotc: SOTC_RB_EXACT=publish latency stamps (stat: a CS_DONE label, see EventWriteEop).
        bool stat{};
        u64 t_cp{}, t_draw{}, t_sub{}, t_gpu{};
        // sotc: SOTC_LABELS_HONEST / SOTC_PC_RING8_LABELS: the label kind, for the forced counter.
        // Set where the job is tracked (Ring8Track callers).
        SotcLabels::Kind label_kind{SotcLabels::Kind::Eop};
    };
    /// Queues a label write (write is the whole SignalFence: value and interrupt) as a scheduler
    /// deferred operation. shadow32/shadow64 publish its value for the CP's own waits. t_cp /
    /// t_draw (nonzero t_cp = stat job) are the publish-mode latency stamps.
    std::shared_ptr<RbxJob> QueueDeferredLabel(u32 queue, VAddr label, bool shadow32,
                                               bool shadow64, u32 data_lo, u32 data_hi, u64 snap,
                                               Common::UniqueFunction<void>&& write,
                                               u64 t_cp = 0, u64 t_draw = 0);
    std::deque<std::shared_ptr<RbxJob>> rbx_jobs; ///< FIFO, under rbx_mutex
    std::array<u32, 2> rbx_q_out{};               ///< deferred labels outstanding per queue
    void LandLabel(RbxJob& job);
    void RbxDrain();
    u64 RbxOldestUnsubmittedNs();
    // Adaptive policy per label address (liverpool.cpp, RbxDecide).
    struct RbxAddrStat {
        double a{}, b{}; ///< EWMAs (ns): landing latency, readback wait charged to early labels
        u32 a_samples{}, b_samples{}, hot_count{};
        bool deferred{};
        u64 last_use{};
    };
    struct RbxEarly {
        u64 snap{};
        VAddr key{};
        double cost{};
    };
    static constexpr size_t RbxMaxStats = 1024;
    std::mutex rbx_stat_mutex; ///< policy table and early ring (draw thread and guest threads)
    std::unordered_map<VAddr, RbxAddrStat> rbx_stats;
    u64 rbx_use_clock{};
    std::array<RbxEarly, 128> rbx_early_ring{};
    u32 rbx_early_head{}, rbx_early_count{};
    RbxAddrStat& RbxStat(VAddr key);
    void RbxUpdateMode(RbxAddrStat& st);
    bool RbxPolicyDefer(VAddr key);
    void RbxRecordEarly(u64 snap, VAddr key);
    void RbxFoldLanding(VAddr key, u64 latency_ns);
    static void RbxWaitObserver(u64 writer_seq, u64 wait_ns);
    u64 rbx_flush_tick{}; ///< tick of the last EopService flush (draw thread)

    // sotc: SOTC_PC_RING8_LABELS (default on, 0 disables; liverpool.cpp has the design). ring8_labels: the
    // switch is on (read in the constructor). ring8_jobs: deferred ring8 labels not landed yet,
    // under ring8_mutex; the command processor reads it, the draw thread writes it. A CP wait on
    // such a label queues Ring8Service (ring8_service_requested), which submits the open buffer
    // once when ring8_want_flush is set, lands what the GPU finished, and runs the watchdog.
    bool ring8_labels{};
    // sotc: SOTC_LABELS_HONEST=1 (needs the draw thread). labels_honest: the switch is on and the
    // draw thread runs. label_machinery: the deferral rule is live for this process (ring8_labels
    // and mode 8, or labels_honest). Both read once in the constructor.
    bool labels_honest{};
    bool label_machinery{};
    // Draw thread only (the labels run there). Per queue (0 graphics, 1 compute): HostWriteSeq and
    // SotcLabels::SharedSeq at the queue's previous label.
    std::array<u64, 2> label_ring_seq{};
    std::array<u64, 2> label_shared_seq{};
    struct LabelDecision {
        bool defer{};       ///< write the label on the GPU timeline (QueueDeferredLabel)
        bool pending{};     ///< a rule applied (served write, shared touch, or an outstanding label)
        bool waited{};      ///< pending and waited on (submit lookahead or learned CP wait): not deferred
        bool honest_only{}; ///< deferred only because of the shared-touch rule
    };
    /// Draw thread. The deferral rule for one label of kind on queue q (liverpool.cpp).
    LabelDecision LabelDecide(SotcLabels::Kind kind, u32 q, VAddr label, bool waited);
    std::mutex ring8_mutex;
    std::vector<std::shared_ptr<RbxJob>> ring8_jobs;
    std::atomic<bool> ring8_service_requested{false};
    std::atomic<bool> ring8_want_flush{false};
    void Ring8Track(const std::shared_ptr<RbxJob>& job);
    /// Command processor: true while a deferred ring8 label with this 8-byte key has not landed.
    bool Ring8PendingFor(VAddr addr);
    /// Command processor, inside a WAIT_REG_MEM spin: flushes once per wait (flushed) and queues
    /// Ring8Service while the wait is on a pending ring8 label.
    void Ring8CpWaitStep(VAddr addr, bool& flushed);
    void Ring8Service(); ///< draw thread
    /// Draw thread: lands a ring8 label that has been pending for 500 ms (counted as forced).
    void Ring8Watchdog();
    /// Draw thread: LandLabel unless the job has already landed (scheduler op, RbxDrain, watchdog).
    void LandOnce(RbxJob& job);

    // sotc: SOTC_CULL_EXPRESS=1 (default off). The "express lane" (liverpool.cpp has the long
    // comment with the exactness argument). The command processor recognises the XPR occlusion
    // submit (a gfx segment that ends in a dispatch and a CS_DONE end-of-pipe label) and publishes
    // it as an express segment [start, end) of back ring sequence numbers. The draw thread, at an
    // op boundary, may run that segment BEFORE the older ops [back_tail, start) when none of them
    // can observe it (XpPoll); anything uncertain keeps the FIFO order and is counted.
    /// What a Func op is, for the lane. Everything not listed is XpUnknown and blocks.
    enum XpCls : u8 {
        XpUnknown,
        XpLabel,     ///< EOP/EOS/ReleaseMem fence: writes waddr/wsize (CPU-visible progress signal)
        XpWriteData, ///< WRITE_DATA to memory: writes waddr/wsize
        XpDma,       ///< DMA_DATA between guest memory ranges: reads raddr/rsize, writes waddr/wsize
        XpMark,      ///< frame end (OnSubmit + Flush, or the frames_recorded ticket)
        XpFlip       ///< flip signal
    };
    struct XpHint {
        u8 cls{};
        VAddr raddr{}, waddr{};
        u64 rsize{}, wsize{};
    };
    /// A published express segment: ring sequence numbers [start, end), the last op is the label.
    struct XpSeg {
        u64 start{}, end{}, queued_ns{};
        VAddr label_key{}; ///< the label's 8-byte aligned address
    };
    bool xp_on{};
    bool xp_signals{true};      ///< SOTC_CULL_EXPRESS_SIGNALS=0: skipped labels/flips block
    bool xp_marks{true};        ///< SOTC_CULL_EXPRESS_MARKS=0: skipped frame marks block
    bool xp_pattern{true};      ///< SOTC_CULL_EXPRESS_PATTERN=0: only SOTC_CULL_EXPRESS_ADDR
    VAddr xp_forced_addr{};     ///< SOTC_CULL_EXPRESS_ADDR=<hex>: this label address qualifies
    u32 xp_max_seg{256};        ///< SOTC_CULL_EXPRESS_SEG
    u32 xp_max_skip{480};       ///< SOTC_CULL_EXPRESS_SKIP (rule 5, <= BackRingSize)
    u64 xp_budget_ns{500'000};  ///< SOTC_CULL_EXPRESS_US (rule 5)
    XpHint xp_hint{};           ///< command processor: consumed by QueueBackFunc
    u64 xp_seg_start{};         ///< command processor: first op of the gfx segment being parsed
    u64 xp_last_dispatch{};     ///< command processor: back_head after the last gfx dispatch
    static constexpr u64 XpRing = 8;
    std::array<XpSeg, XpRing> xp_ring{};
    std::atomic<u64> xp_head{0}; ///< published by the command processor (release)
    std::atomic<u64> xp_tail{0}; ///< taken by the draw thread
    u32 xp_gfx_depth{};          ///< command processor: nesting of ProcessGraphics (indirect buffers)
    u32 xp_retries{};            ///< draw thread: re-evaluations of the oldest segment
    double xp_op_ns{2000.0};     ///< draw thread: EWMA of the time per FIFO op
    u64 xp_watch_end{}, xp_watch_ns{}; ///< draw thread: a FIFO segment whose latency is measured
    // Register undo log of the lane (draw thread): the chunks the lane advanced back_regs through.
    std::vector<u32> xp_undo;
    std::array<u64, (NumRegChunks + 63) / 64> xp_undo_mask{};
    std::vector<u16> xp_undo_list;
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> xp_saved_cb{};
    CbDbExtent xp_saved_db{};
    ComputeProgram xp_saved_cs{};
    void XpPublishSeg(const XpSeg& seg);
    /// Prep thread: the shader-bound footprint of an op into its prepared slot.
    void XpFillResources(Vulkan::PreparedDraw& slot, const Vulkan::Pipeline* pipeline);
    /// sotc: SOTC_OP_OVERLAP (7c.0, SotcStats::kOpOverlap). ovl_on is set before the prep threads
    /// start; ovl_prev* are draw-thread only.
    bool ovl_on{};
    bool ovl_prev_known{};
    AmdGpu::OpFootprint ovl_prev;
    /// Draw thread, before the op runs: compare its footprint with the previous op's.
    void NoteOpOverlap(const BackOp& op);
    /// sotc: SOTC_N5_GRAPH (vk_frame_graph.h). n5_on is set before the prep threads start. n5_cur
    /// is draw-thread only: N5Begin runs in ExecuteBackOp before the op, N5End after it.
    bool n5_on{};
    Vulkan::FrameGraph::Sample n5_cur{};
    void N5Begin(const BackOp& op);
    void N5End();
    void XpPoll(u64 tail);
    /// Draw thread: returns true when the segment was consumed (ran ahead or dropped).
    bool XpEvaluate(const XpSeg& seg, u64 tail);
    void XpApplyState(const BackOp& op);
    void XpUndoState();
    void XpReplayState(const BackOp& op); ///< a lane-run op reached in FIFO order
    void XpSetHint(u8 cls, VAddr waddr, u64 wsize, VAddr raddr = 0, u64 rsize = 0) {
        xp_hint = XpHint{cls, raddr, waddr, rsize, wsize};
    }

public:
    /// sotc: SOTC_RB_EXACT: lands the labels that are ready from a long draw thread wait.
    void RbxWaitService();

private:

    /// sotc: what the command buffer task that last yielded waits on (CP thread only, set by the
    /// wait loops right before they yield; SOTC_FPS "SOTC CP spin" line). Label: a memory wait
    /// while ops that write guest memory are still queued for the draw thread (or SOTC_EOP_TICK
    /// labels are outstanding); flip: a VideoOut flip label; reg mem: other guest memory (written
    /// by the game's CPU threads); other: MemSemaphore, Rewind, register waits.
    enum CpWaitKind : u8 { CpWaitRegMem, CpWaitLabel, CpWaitFlip, CpWaitDeCounter, CpWaitOther };
    u8 cp_wait_kind{CpWaitOther};
    u8 MemWaitKind(const PM4CmdWaitRegMem& wait, bool vo_label) const;
    /// sotc: wait loops left (CP thread only). A resume that left one, finished its task or
    /// was the first of its task made progress; any other resume only re-tested its wait.
    u64 cp_wait_exits{0};
    /// sotc: SOTC_CP_IDLE_US=<us> (0/unset: off). After that long without progress the command
    /// processor blocks in CpIdleSleep for at most SOTC_CP_IDLE_TIMEOUT_US (default 200) instead
    /// of yielding; NotifyCpWait wakes it.
    u64 cp_idle_ns{0};
    u64 cp_idle_timeout_ns{200'000};
    std::atomic<u64> cp_wake_epoch{0};
    std::atomic<bool> cp_sleeping{false};
    std::atomic<u64> cp_notify_ns{0}; ///< SOTC_FPS: when WakeCp last woke it (wake latency)
#ifdef _WIN32
    void* cp_wake_event{}; ///< auto-reset event (HANDLE)
#else
    std::mutex cp_idle_mutex;
    std::condition_variable cp_idle_cv;
#endif
    /// Blocks until NotifyCpWait changes cp_wake_epoch from seen_epoch or the timeout passes.
    /// Returns true when woken by a notification (or the epoch had already moved).
    bool CpIdleSleep(u64 seen_epoch);
    void WakeCp() noexcept;
    // sotc: SOTC_CP_LABEL_SLEEP=1 (nonblocking.h; Windows only, read in the constructor). After
    // cp_round_sleep_rounds whole rounds over the queues without progress, the command processor
    // sleeps in CpRoundSleep (WaitOnAddress on cp_wake_epoch, at most cp_round_sleep_ms) instead
    // of yielding; NotifyCpWait (the draw thread after each queued function, new submits and
    // commands, labels and fences written) wakes it. SOTC_CP_LABEL_SLEEP_ROUNDS (default 64) and
    // SOTC_CP_LABEL_SLEEP_MS (default 1) tune it.
    bool cp_round_sleep{};
    u32 cp_round_sleep_rounds{64};
    u32 cp_round_sleep_ms{1};
    std::atomic<bool> cp_round_sleeping{false};
    /// Returns true when the epoch moved from seen_epoch (a notification), false on the timeout.
    bool CpRoundSleep(u64 seen_epoch);
    void WakeCpRound() noexcept;
};

} // namespace AmdGpu
