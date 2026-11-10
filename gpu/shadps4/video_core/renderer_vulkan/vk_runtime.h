// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;

class Runtime {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime() = default;

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();

    /// sotc: SOTC_XFER_QUEUE. Copies a buffer range on the transfer queue when the current
    /// graphics work does not touch it (see XferQueue). False: the caller uses CopyBuffer.
    bool TryCopyBufferXfer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                           std::span<const vk::BufferCopy> copies);

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value);

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);

    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {});

    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);

    /// sotc: SOTC_HOST_IMAGE_COPY: writes a whole linear image from guest memory with the host
    /// (vkCopyMemoryToImageEXT). guest_base is the image's guest address; the copies' bufferOffset
    /// are relative to it. Returns false, with the image left as it was, when the path does not
    /// apply; gpu_idle says whether the image has no GPU work in flight and none recorded.
    bool TryHostImageCopy(VideoCore::Image* dst, const u8* guest_base,
                          std::span<const vk::BufferImageCopy> copies, bool gpu_idle);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);

    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false);

    void FlushBarriers();

    /// sotc: SOTC_SHADER_ADDR. A shader-addressed write (no buffer record): it becomes part of the pending global
    /// memory barrier, which FlushBarriers emits and clears.
    void NoteAddressedWrite();
    /// sotc: SOTC_SHADER_ADDR. True when a memory barrier is pending (a buffer write since the last FlushBarriers).
    bool MemoryBarrierPending() const noexcept;
    /// sotc: SOTC_SHADER_ADDR. True when a shader-addressed write is pending (cleared by FlushBarriers).
    bool AddressedWritePending() const noexcept;
    /// sotc: SOTC_SHADER_ADDR: counts the barriers FlushBarriers recorded (the rasterizer's guest write ranges are
    /// valid for the barrier interval they were recorded in).
    u64 FlushGeneration() const noexcept {
        return flush_gen;
    }
    /// sotc: image barriers waiting for FlushBarriers (SOTC_DEPTHFAST_VERIFY compares it).
    [[nodiscard]] size_t PendingImageBarriers() const {
        return static_cast<size_t>(image_barriers.size());
    }

private:
    void MakeCurrent(const VideoCore::Buffer* handle);

private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    struct BufferBarriers {
        const VideoCore::Buffer* handle;
        using AccessList = IntervalList<Interval>;
        AccessList read_ranges;
        AccessList write_ranges;
    };
    BufferBarriers* resource{};
    std::vector<BufferBarriers> resources;
    // sotc: entries [0, active_resources) are live. The rest keep their list capacity across
    // barriers (no malloc for the first ranges after every FlushBarriers).
    std::size_t active_resources{};
    // sotc: BbToggle::DrawBindMemo. The accesses applied since the last FlushBarriers (the epoch),
    // direct mapped: applying the same one again changes neither range list nor barrier masks, so
    // a draw that binds what the previous one bound skips them (AccessBuffer).
    struct AccessMemo {
        const VideoCore::Buffer* handle{};
        u64 offset{};
        u64 size{};
        u64 stage{};
        u64 access{};
        u64 epoch{};
    };
    static constexpr std::size_t NumAccessMemos = 64;
    std::array<AccessMemo, NumAccessMemos> access_memos{};
    u64 access_epoch{1};
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
    bool addressed_write_pending{}; // sotc: SOTC_SHADER_ADDR, see NoteAddressedWrite
    u64 flush_gen{};                // sotc: SOTC_SHADER_ADDR, see FlushGeneration
};

} // namespace Vulkan
