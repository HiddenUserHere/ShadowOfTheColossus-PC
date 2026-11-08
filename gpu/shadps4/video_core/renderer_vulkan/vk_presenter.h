// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "core/libraries/videoout/buffer.h"
#include <imgui.h> // sotc port: ImTextureID only
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    /// sotc: this frame without the HUD (Rasterizer::TakeHudless) for frame generation, or null.
    Frame* hudless{};
    /// sotc: SOTC_FRAME_HASH, the guest flip number this frame is presented as (set by the video
    /// out driver before the present job; 0 for frames it did not flip).
    u64 sotc_flip_index{};

    ImTextureID imgui_texture;
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;
struct SotcFrameHashRing; // sotc: SOTC_FRAME_HASH, defined in vk_presenter.cpp

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    /// sotc: SOTC_RAM_REPORT (gpu/sotc/ram_report.cpp) reads the VMA allocator through this.
    const Instance& GetInstance() const {
        return instance;
    }

    /// sotc: SOTC_RAM_REPORT: the rasterizer (null before it is created) and the runtime, which holds
    /// the staging pool. Read-only telemetry.
    Rasterizer* SotcRasterizer() {
        return rasterizer.get();
    }
    Runtime& SotcRuntime() {
        return runtime;
    }

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    /// sotc: SOTC_FLIP_THREAD=1 (off by default). The draw thread records only the copy of the
    /// game's front buffer into one of the SOTC_FLIP_IMAGES port-owned images, on draw_scheduler at the
    /// flip's position of the stream, and queues the rest of the flip (the frame's wait for a free
    /// presentation frame, FSR 1 / sharpen, the post process pass, the submit) to the flip thread,
    /// which hands the frame to on_frame, in flip order. With SOTC_FLIP_COALESCE, a flip that finds
    /// its flip image still in use joins the newest queued job the flip thread has not started
    /// instead of waiting: on_frame then runs with the job's frame (first flip) or with null
    /// (later flips of the job, which only flip). True when queued: the caller must not
    /// call PrepareFrame, and on_frame runs later on the flip thread. False when the flip takes the
    /// usual path (any DLSS / frame generation / hud-less / shot case, or the switch is off): the
    /// flip thread has then finished every earlier flip, and the caller calls PrepareFrame.
    bool QueueFlip(const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address,
                   std::function<void(Frame*)> on_frame);

    /// sotc: SOTC_FLIP_THREAD. Returns once every flip queued by QueueFlip has been handed to its
    /// on_frame. Call before anything else that presents or takes a frame on the draw thread
    /// (blank frames, the usual path), so that the presentation order is kept. No-op when off.
    void FlipDrain();

    /// sotc: SOTC_FLIP_SERVICE=1 (off by default). Set by the command processor when the draw
    /// thread prepares the frames: PrepareFrame first waits for a free presentation frame,
    /// calling this every 200 us of waiting, so the commands of the game's threads (unmaps,
    /// readbacks) run during that wait instead of after it. The wait comes before anything of
    /// the flip is done, so they run as they would between two operations.
    inline static void (*wait_service)() = nullptr;

    /// sotc: SOTC_FLIP_OFFTHREAD=1 (off by default), the handoff from the present thread. The
    /// present thread (its tick, see PresentThread) moves one presentation frame whose present_done
    /// fence has already signalled from free_queue to ready_queue, without waiting for any fence.
    /// GetRenderFrame takes a ready frame first, so the draw thread's PrepareFrame finds a frame
    /// with no wait (no free-frame wait, no fence wait) in the usual case. Frame order is kept:
    /// the copy is still recorded by the draw thread at the same point of its stream, and the
    /// early VO label release (driver.cpp SubmitFlipInternal) still follows it. A frame is never
    /// dropped: a ready frame goes to the draw thread or the present thread, never back. When no
    /// ready frame exists, the draw thread waits for a free frame as before (counted in SOTC FLIP
    /// WAIT and in SOTC FLIP OFFTHREAD).
    void RefillReadyFrames();

    /// sotc: SOTC_CP_FLIP_PACE=1 (off by default). The presenter the command processor paces its
    /// flips with, null when off. Before it queues a flip for the draw thread, the command
    /// processor waits (PaceFlip) until more presentation frames are free than flips it queued
    /// that the draw thread has not reached yet (FlipReached), so the draw thread finds a free
    /// frame instead of blocking in GetRenderFrame while the game's threads wait for it. The
    /// frame rate is unchanged: the same wait moves to the command processor.
    inline static Presenter* cp_pacer = nullptr;
    void PaceFlip();
    void FlipReached() {
        pending_flips.fetch_sub(1, std::memory_order_acq_rel);
    }

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

    /// sotc: SOTC_LOW_LATENCY. Start of a game frame, called by the presenting thread before the
    /// flip is released (driver.cpp). Does nothing when the switch is off.
    void LowLatencyFrameStart() {
        low_latency.FrameStart(swapchain.GetHandle());
    }

    /// sotc: SOTC_PRESENT_WAIT. Id of the last present tagged for VK_KHR_present_id, 0 while none
    /// (the switch is off, or nothing presented yet). Written by the presenting thread.
    u64 LastPresentId() const {
        return last_present_id.load(std::memory_order_acquire);
    }

    /// sotc: SOTC_PRESENT_WAIT. Waits up to timeout_ns (vkWaitForPresentKHR) until present `id` is
    /// on screen. True when it is, or when there is nothing to wait for (id 0, a present from a
    /// replaced swapchain, or an error, which must not stall the caller); false on timeout, or
    /// when `id` is not presented yet. The host time the wait saw it done is kept, see
    /// PresentDoneNs.
    bool WaitPresented(u64 id, u64 timeout_ns);

    /// sotc: SOTC_PRESENT_WAIT. Host steady-clock time (ns) WaitPresented saw present `id` done,
    /// 0 when it has not (or is too old to be kept: the last 64 presents are).
    u64 PresentDoneNs(u64 id) const;

    /// sotc: display cadence (always on). True when the display waiter thread waits for every
    /// tagged present (VK_KHR_present_wait) and records when it is on screen, so PresentDoneNs is
    /// exact to the waiter's wake-up instead of the vblank thread's next poll. Started by the first
    /// tagged present; false before that and on devices without present wait.
    bool DisplayClockAvailable() const {
        return display_clock_on.load(std::memory_order_acquire);
    }

    /// sotc: the display's vblank period in ns: the primary display's refresh rate, refined from
    /// the intervals between present completions that are within 25% of a whole number of vblanks.
    double DisplayVblankNs() const;

    /// sotc: SOTC_PACER. The display time (host steady-clock ns) the next tagged present is planned
    /// for: TagPresent stores it with that present and clears it, and the SOTC DISPLAY line
    /// measures the phase error when the present is seen on screen. 0 for none.
    void SetNextPresentTarget(u64 target_ns) {
        next_present_target_ns.store(target_ns, std::memory_order_relaxed);
    }

    /// sotc: SOTC_PACER. A game frame superseded by a newer one before it was presented: its GPU
    /// work is waited for on the present queue, as a present would, and it goes back to the free
    /// queue without being shown. Call on the presenting thread.
    void DiscardFrame(Frame* frame);

    /// sotc: SOTC_PACER state for the SOTC DISPLAY line: whether the pacer paces now, and frames it
    /// dropped (superseded before their present).
    void NotePacer(bool active, u32 dropped) {
        pacer_active.store(active, std::memory_order_relaxed);
        if (dropped) {
            pacer_dropped.fetch_add(dropped, std::memory_order_relaxed);
        }
    }

private:
    /// sotc: SOTC_FLIP_THREAD: one of the port-owned images a flip is copied into (SOTC_FLIP_IMAGES).
    /// busy: from the draw thread's copy until the flip thread's submit that reads it; read_tick is
    /// the flip scheduler tick of that submit (0: none yet). Guarded by flip_mutex, except image,
    /// view and the size fields, which only the draw thread changes, and only while !busy.
    struct FlipImage {
        vk::Image image{};
        vk::ImageView view{};
        VmaAllocation allocation{};
        vk::Format format{};      ///< format of the game image it copies
        vk::Format view_format{}; ///< format of the view the passes sample
        u32 width{};
        u32 height{};
        bool busy{};
        u64 read_tick{};
    };

    /// sotc: SOTC_FLIP_THREAD: one queued job, in the order the draw thread queued them. A job is
    /// one presented frame; with SOTC_FLIP_COALESCE it can carry several flips (on_frames). The
    /// job at the front of flip_jobs is the one the flip thread runs; it leaves the deque when done.
    struct FlipJob {
        u32 slot{};
        u64 copy_tick{};          ///< draw_scheduler tick of the (last) copy
        u32 width{};              ///< presentation frame size of the flip
        u32 height{};
        vk::Extent2D source_size{}; ///< size of the game image the copy read
        bool srgb_input{};
        bool started{};           ///< the flip thread runs it (guarded by flip_mutex); no more copies
        bool filling{};           ///< the draw thread is recording a coalesced copy into it; not started yet
        /// One per flip the job presents, in flip order. The first gets the frame; the others get
        /// null, so the driver flips without a second present (SOTC_FLIP_COALESCE).
        std::vector<std::function<void(Frame*)>> on_frames;
    };

    /// sotc: SOTC_FLIP_IMAGES=N (2..8, default 3, read once): the number of flip images (the ring size).
    static u32 FlipImageCount();

    Frame* GetRenderFrame();
    /// sotc: a presentation frame of width x height (GetRenderFrame() uses the expected size).
    Frame* GetRenderFrame(u32 width, u32 height);
    /// sotc: SOTC_FLIP_SERVICE: waits until a presentation frame is free, calling wait_service.
    void WaitFreeFrame();

    /// sotc: SOTC_FLIP_THREAD. The output size of the game image (SOTC_OUTPUT_RES), as PrepareFrame
    /// sets it.
    void ApplyOutputRes(vk::Extent2D image_size);
    /// sotc: the host passes of the usual path (FSR 1 or bilinear, then the post process pass) from
    /// source_view into frame. Records into cmdbuf only; shared by PrepareFrame and the flip thread.
    void RenderPlainPasses(vk::CommandBuffer cmdbuf, vk::ImageView source_view,
                           vk::Extent2D source_size, Frame* frame, bool srgb_input);
    bool CreateFlipImage(FlipImage& flip, vk::Format format, vk::Format view_format,
                         vk::Extent2D size);
    void DestroyFlipImage(FlipImage& flip);
    /// sotc: SOTC_FLIP_THREAD, the flip thread's loop and one flip of it.
    void FlipThreadMain();
    void RunFlipJob(FlipJob& job);

    /// sotc: SOTC_PRESENT_WAIT. A tagged present: its id, the swapchain it went to, and when
    /// WaitPresented saw it done (0 until then).
    struct PresentRecord {
        u64 id{};
        VkSwapchainKHR swapchain{};
        u64 done_ns{};
        u64 queued_ns{}; ///< sotc: host time the id was taken, just before vkQueuePresentKHR
        u64 target_ns{}; ///< sotc: SOTC_PACER planned display time, 0 for none
    };

    /// sotc: the display waiter thread (display cadence log): waits for each tagged present in
    /// order and records when it is on screen.
    void DisplayWaitMain(std::stop_token token);

    /// sotc: takes the id of the present about to be queued. Call under Scheduler::submit_mutex.
    u64 TagPresent();

    /// sotc: false when a frame past ring_default_frames cannot be allocated (the caller drops it).
    bool RecreateFrame(Frame* frame, u32 width, u32 height);

    /// sotc: FSR 3 frame generation: presents the frame between the last game frame and
    /// `frame`. False when there is none.
    bool PresentGenerated(Frame* frame);

    void SetExpectedGameSize(s32 width, s32 height);

    /// sotc: SOTC_FRAME_HASH. Records the downscale and readback of the game image of `frame`
    /// into `cmdbuf` (the present command buffer), right after the blit to the swapchain.
    void RecordFrameHash(const vk::CommandBuffer& cmdbuf, const Frame* frame);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    /// sotc: the same FSR 1 for the frame without the HUD (frame generation), with its own images:
    /// fsr_pass's ring holds the game frame's.
    HostPasses::FsrPass fsr_pass_hudless;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    /// sotc: SOTC_FLIP_THREAD: the flip thread's own scheduler (no recorder thread). Used by the flip
    /// thread, except Wait on the draw thread when a flip image changes size or format.
    Scheduler flip_thread_scheduler;
    Swapchain swapchain;
    LowLatency low_latency{instance}; ///< sotc: SOTC_LOW_LATENCY backend, see vk_low_latency.h
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::vector<Frame> hudless_frames; ///< sotc: Frame::hudless, one per present frame
    /// sotc: SOTC_PRESENT_FRAMES: frames with an id below this are always allocated; the ones past
    /// it may be dropped when VRAM is short.
    u32 ring_default_frames{};
    std::queue<Frame*> free_queue;
    /// sotc: SOTC_FLIP_OFFTHREAD: frames the present thread moved from free_queue, fence signalled.
    /// Guarded by free_mutex, like free_queue; a frame is in exactly one of the two queues or in use.
    std::queue<Frame*> ready_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    /// sotc: SOTC_FLIP_THREAD state, guarded by flip_mutex (see FlipImage, FlipJob).
    std::mutex flip_mutex;
    std::condition_variable_any flip_cv; ///< flip jobs queued, flip images freed, drained
    std::deque<FlipJob> flip_jobs; ///< front: the job the flip thread runs (started), then the queued ones
    u64 flip_queued{};   ///< jobs queued by QueueFlip
    u64 flip_done{};     ///< jobs whose on_frame has run
    bool flip_stop{};    ///< the destructor asks the flip thread to drain and exit
    std::vector<FlipImage> flip_images = std::vector<FlipImage>(FlipImageCount());
    u64 flip_seq{};      ///< draw thread only: the flip image of the next flip is flip_seq % flip_images.size()
    std::jthread flip_thread; ///< started by the first QueueFlip while the switch is on
    std::atomic<s32> pending_flips{0}; ///< sotc: SOTC_CP_FLIP_PACE, see cp_pacer
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    std::atomic<u64> last_present_id{0};         ///< sotc: SOTC_PRESENT_WAIT, see LastPresentId
    mutable std::mutex present_records_mutex;    ///< sotc: guards present_records
    std::array<PresentRecord, 64> present_records{}; ///< sotc: indexed by id % 64
    /// sotc: SOTC_FRAME_HASH, see SotcFrameHashRing. Created by the first game frame recorded;
    /// null while off, or when the GPU cannot blit the swapchain format (sotc_frame_hash_tried).
    std::shared_ptr<SotcFrameHashRing> sotc_frame_hash;
    bool sotc_frame_hash_tried{};
    /// sotc: display cadence log and SOTC_PACER (see DisplayClockAvailable, DisplayWaitMain).
    std::atomic<bool> display_clock_on{false};
    std::atomic<double> display_vblank_ns{0.0};
    std::atomic<u64> next_present_target_ns{0};
    std::atomic<bool> pacer_active{false};
    std::atomic<u32> pacer_dropped{0};
    std::mutex display_wait_mutex;            ///< guards the display_wait_cv predicate
    std::condition_variable_any display_wait_cv; ///< TagPresent wakes the waiter
    std::jthread display_thread;              ///< last member: stopped first (also in ~Presenter)
};

/// sotc: SOTC_FPS (B5). Appends one line per window: the draw thread's flip wait split by cause
/// (no free presentation frame, the present_done fence, submit_mutex on the draw thread), per
/// frame. Resets its counters. Appends nothing while SOTC_FPS is off.
void ReportFlipWait(std::string& out, double seconds);

/// sotc: SOTC_PRESENT_FRAMES (C6) and the present thread's lock. Appends one line per window:
/// the ring size, the GetRenderFrame calls that waited for a free frame, the time the present
/// thread waited for submit_mutex and the time under it. Resets its counters. Appends nothing
/// while SOTC_FPS is off.
void ReportPresent(std::string& out, double seconds);

} // namespace Vulkan
