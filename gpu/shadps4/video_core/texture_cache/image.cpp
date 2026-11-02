// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <optional>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <ranges>
#include <unordered_map>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_prefetch.h"
#include "video_core/sotc_stats.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

// sotc: images created by this module (counted at creation, not at the live count).
static std::atomic<u64> images_created{};
static std::atomic<u64> images_destroyed{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used. The exception here is for multisample images when storage is not
            // supported, where even with ExtendedUsage we may get only one supported sample back.
            if (info.num_samples == 1 || instance.IsMultisampleStorageImageSupported())
                usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    Destroy();
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, static_cast<VkImage>(image), allocation);
        image = vk::Image{};
        allocation = {};
        images_destroyed.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_IMG_POOLED report
        SotcStats::image_bytes.fetch_sub(size_bytes, std::memory_order_relaxed);
        SotcStats::image_count.fetch_sub(1, std::memory_order_relaxed);
        size_bytes = 0;
    }
}

// sotc: SOTC_IMG_POOLED=1 (off by default): the image pool is on at 256 MiB, and an image the
// full pool cannot take gets a block of the pool, not a dedicated
// allocation. VMA never makes a dedicated allocation for a custom pool with an explicit block size,
// while the default path does so for every image the driver prefers to have dedicated (render
// targets on the AMD driver).
static bool ImgPooledEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_IMG_POOLED");
        return v && v[0] == '1';
    }();
    return enabled;
}

// sotc: SOTC_IMAGE_POOL_MB=<n> (off by default). A VMA pool of 64 MiB blocks for images, n MiB of
// it allocated when the first image is created (the boot loading screen): images that fit are
// suballocated from it, so a new texture no longer waits for vkAllocateMemory on the draw thread
// while an area loads. The pool's memory type is the one VMA picks for the first image; an image
// that cannot use it, or larger than a block, is allocated as before.
static VmaPool ImagePool(VmaAllocator allocator, const VkImageCreateInfo& image_ci,
                         const VmaAllocationCreateInfo& alloc_ci) {
    // sotc: SOTC_IMG_POOLED=1 turns the pool on at 256 MiB unless SOTC_IMAGE_POOL_MB sets a size.
    static const u64 pool_mb = [] {
        const char* v = std::getenv("SOTC_IMAGE_POOL_MB");
        if (v) {
            return std::strtoull(v, nullptr, 10);
        }
        return ImgPooledEnabled() ? 256ULL : 0ULL;
    }();
    if (pool_mb == 0) {
        return VK_NULL_HANDLE;
    }
    static std::once_flag once;
    static VmaPool pool = VK_NULL_HANDLE;
    std::call_once(once, [&] {
        u32 type_index = 0;
        if (vmaFindMemoryTypeIndexForImageInfo(allocator, &image_ci, &alloc_ci, &type_index) !=
            VK_SUCCESS) {
            return;
        }
        constexpr u64 BlockSize = 64_MB;
        const VmaPoolCreateInfo pool_ci = {
            .memoryTypeIndex = type_index,
            .flags = 0,
            .blockSize = BlockSize,
            .minBlockCount = static_cast<size_t>(std::max<u64>(pool_mb * 1_MB / BlockSize, 1)),
            .maxBlockCount = 0,
        };
        if (vmaCreatePool(allocator, &pool_ci, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            LOG_WARNING(Render_Vulkan, "SOTC_IMAGE_POOL_MB: the image pool could not be created");
        }
    });
    return pool;
}

// sotc: the image pool (SOTC_IMAGE_POOL_MB / SOTC_IMG_POOLED) for one image.
// True when the image was created in it. First in the blocks the pool has (no allocation on this
// thread). With SOTC_IMG_POOLED, a full pool then allocates a block of its own, so the image
// still avoids a dedicated allocation. False: the caller uses the default allocator.
static bool CreateInImagePool(VmaAllocator allocator, const VkImageCreateInfo& image_ci_unsafe,
                              const VmaAllocationCreateInfo& alloc_ci, VkImage& unsafe_image,
                              VmaAllocation& allocation, VmaAllocationInfo& alloc_info) {
    const VmaPool pool = ImagePool(allocator, image_ci_unsafe, alloc_ci);
    if (!pool) {
        return false;
    }
    VmaAllocationCreateInfo pool_ci = alloc_ci;
    pool_ci.pool = pool;
    pool_ci.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT; // only the blocks it has
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &pool_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result != VK_SUCCESS && ImgPooledEnabled()) {
        pool_ci.flags &= ~VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT; // sotc: a block of its own
        result = vmaCreateImage(allocator, &image_ci_unsafe, &pool_ci, &unsafe_image, &allocation,
                                &alloc_info);
    }
    if (result != VK_SUCCESS) {
        return false;
    }
    SotcStats::image_pool_allocs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    const SotcStats::ScopedTimer alloc_timer{SotcStats::image_alloc_ns};
    VkResult result = VK_SUCCESS;
    if (!CreateInImagePool(allocator, image_ci_unsafe, alloc_ci, unsafe_image, allocation,
                           alloc_info)) {
        result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image, &allocation,
                                &alloc_info);
    }
    images_created.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_IMG_POOLED report
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // Over the video memory budget (loading screens peak before the cache collects old
        // images). Going over budget lets the driver page memory out instead of aborting.
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Video memory budget exceeded, allocating images past it");
        }
        VmaAllocationCreateInfo over_budget_ci = alloc_ci;
        over_budget_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &image_ci_unsafe, &over_budget_ci, &unsafe_image,
                                &allocation, &alloc_info);
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
    SotcStats::image_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    SotcStats::image_count.fetch_add(1, std::memory_order_relaxed);
    VmaAllocationInfo2 alloc_info2{};
    vmaGetAllocationInfo2(allocator, allocation, &alloc_info2);
    if (alloc_info2.dedicatedMemory) {
        SotcStats::image_dedicated.fetch_add(1, std::memory_order_relaxed);
    }
}

// sotc: SOTC_ASYNC_IMAGE_CREATE. As Create, on an image worker: no allocation past the budget
// (a speculative image must not push the cache over it; the image is then made by Create on
// demand, which may go past it) and no assert. Not timed in image_alloc_ns, which is the time
// the thread that needs the image spent.
bool UniqueImage::TryCreateWithinBudget(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };
    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    VkResult result = VK_SUCCESS;
    if (!CreateInImagePool(allocator, image_ci_unsafe, alloc_ci, unsafe_image, allocation,
                           alloc_info)) {
        result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image, &allocation,
                                &alloc_info);
    }
    if (result != VK_SUCCESS) {
        allocation = {};
        return false;
    }
    images_created.fetch_add(1, std::memory_order_relaxed); // sotc: SOTC_IMG_POOLED report
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
    SotcStats::image_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    SotcStats::image_count.fetch_add(1, std::memory_order_relaxed);
    VmaAllocationInfo2 alloc_info2{};
    vmaGetAllocationInfo2(allocator, allocation, &alloc_info2);
    if (alloc_info2.dedicatedMemory) {
        SotcStats::image_dedicated.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

// sotc: SOTC_HOST_IMAGE_COPY=1: VK_IMAGE_USAGE_HOST_TRANSFER_BIT is worth adding when the host can
// write the format (format feature HOST_IMAGE_TRANSFER on optimal tiling) and the driver reports
// optimal device access for this usage (VkHostImageCopyDevicePerformanceQueryEXT). Cached per
// combination, as QueryImageFormat.
static bool HostTransferUsable(const Vulkan::Instance& instance,
                               const vk::PhysicalDeviceImageFormatInfo2& format_info) {
    static std::mutex mutex;
    static std::unordered_map<u64, bool> answers;
    const u64 key = (u64(format_info.format) << 40) ^ (u64(format_info.type) << 36) ^
                    (u64(u32(format_info.flags)) << 20) ^ u64(u32(format_info.usage));
    std::scoped_lock lock{mutex};
    if (const auto it = answers.find(key); it != answers.end()) {
        return it->second;
    }
    const auto physical_device = instance.GetPhysicalDevice();
    const auto format_chain =
        physical_device.getFormatProperties2<vk::FormatProperties2, vk::FormatProperties3>(
            format_info.format);
    bool usable = bool(format_chain.get<vk::FormatProperties3>().optimalTilingFeatures &
                       vk::FormatFeatureFlagBits2::eHostImageTransfer);
    if (usable) {
        const auto query = physical_device.getImageFormatProperties2<
            vk::ImageFormatProperties2, vk::HostImageCopyDevicePerformanceQueryEXT>(format_info);
        usable = query.result == vk::Result::eSuccess &&
                 query.value.get<vk::HostImageCopyDevicePerformanceQueryEXT>().optimalDeviceAccess;
    }
    answers.emplace(key, usable);
    return usable;
}

// sotc: vkGetPhysicalDeviceImageFormatProperties2 takes about 5 ms per call on a Radeon (driver
// 32.0.21045), and the game creates dozens of images in a frame when the camera turns: frames of
// 200-500 ms. The answer only depends on these parameters, so it is asked once per combination
// (and the unsupported-format error is logged once).
struct FormatQuery {
    vk::Result result;
    vk::SampleCountFlags sample_counts;
};
static FormatQuery QueryImageFormat(const Vulkan::Instance& instance,
                                    const vk::PhysicalDeviceImageFormatInfo2& format_info) {
    static std::mutex mutex;
    static std::unordered_map<u64, FormatQuery> answers;
    const u64 key = (u64(format_info.format) << 40) ^ (u64(format_info.type) << 36) ^
                    (u64(u32(format_info.flags)) << 20) ^ u64(u32(format_info.usage));
    std::scoped_lock lock{mutex};
    if (const auto it = answers.find(key); it != answers.end()) {
        return it->second;
    }
    const SotcStats::ScopedTimer query_timer{SotcStats::image_query_ns};
    const auto properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(format_info.format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    const FormatQuery answer{properties.result,
                             properties.result == vk::Result::eSuccess
                                 ? properties.value.imageFormatProperties.sampleCounts
                                 : vk::SampleCountFlags{}};
    answers.emplace(key, answer);
    return answer;
}

ImagePlan PlanImage(const Vulkan::Instance& instance, const ImageInfo& info) {
    ImagePlan plan{};
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    vk::ImageUsageFlags usage_flags = ImageUsageFlags(instance, info);
    vk::FormatFeatureFlags2 format_features = FormatFeatureFlags(usage_flags);

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    // sotc: SOTC_HOST_IMAGE_COPY=1: non-depth images that are not tiled in guest memory may be
    // written by the host (TextureCache::RefreshImage). The usage is fixed at creation, so it is
    // decided here. Colour images always get the attachment usage (ImageUsageFlags), so a render
    // target can carry the flag too; the host upload itself is still gated per upload by
    // Runtime::TryHostImageCopy (GPU idle, not GPU written).
    if (instance.IsHostImageCopyEnabled() && !info.props.is_tiled && !info.props.is_depth) {
        auto with_host = format_info;
        with_host.usage = usage_flags | vk::ImageUsageFlagBits::eHostTransferEXT;
        if (HostTransferUsable(instance, with_host)) {
            usage_flags = with_host.usage;
            format_info.usage = with_host.usage;
        }
    }
    FormatQuery format_query = QueryImageFormat(instance, format_info);
    // sotc: storage usage is speculative (see ImageUsageFlags). AMD's Windows driver refuses it for
    // BC5/BC6H; creating that combination anyway corrupts the texture or loses the device later,
    // so such images are created without storage when the driver accepts them that way (as in
    // gow3_pc).
    if (format_query.result == vk::Result::eErrorFormatNotSupported &&
        (usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        auto without_storage = format_info;
        without_storage.usage = usage_flags & ~vk::ImageUsageFlagBits::eStorage;
        const FormatQuery retry = QueryImageFormat(instance, without_storage);
        if (retry.result == vk::Result::eSuccess) {
            usage_flags = without_storage.usage;
            format_features = FormatFeatureFlags(usage_flags);
            format_query = retry;
        }
    }
    plan.supported_samples = format_query.result == vk::Result::eSuccess
                                 ? format_query.sample_counts
                                 : vk::SampleCountFlagBits::e1;
    plan.usage_flags = usage_flags;
    plan.format_features = format_features;
    plan.format_result = format_query.result;

    plan.image_ci = vk::ImageCreateInfo{
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, plan.supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    return plan;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : runtime{&runtime_}, slot_image_views{&slot_image_views_}, info{info_} {
    // sotc: SOTC_LAYOUT_UNIFIED: the feature the layouts may use (Pick); the switch is part of the accessor.
    SotcArch::Layouts::NoteSupported(instance.SotcUnifiedLayouts());
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameImage};

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    // sotc: the parameters are worked out by PlanImage (shared with SOTC_ASYNC_IMAGE_CREATE).
    const ImagePlan plan = PlanImage(instance, info);
    usage_flags = plan.usage_flags;
    format_features = plan.format_features;
    supported_samples = plan.supported_samples;
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    // sotc: SOTC_ASYNC_IMAGE_CREATE=1: an identical image an image worker already made (or is
    // making: then this waits for it) is taken instead of creating one here.
    if (!TakePrefetchedImage(plan.image_ci, backing->image)) {
        backing->image.Create(plan.image_ci);
    }

    // sotc: the arguments (vk::to_string allocates) are built only when names are kept: with
    // naming off they cost ~3% of the draw thread while textures stream.
    if (Vulkan::object_names_enabled.load(std::memory_order_relaxed)) {
        Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                              info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                              vk::to_string(info.pixel_format), info.guest_address,
                              info.guest_size, info.resources.layers, info.resources.levels,
                              info.num_samples);
    }
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    // sotc: consecutive binds of an image mostly ask for the same view: compare against the
    // view returned last before the linear search. Exact: views are never duplicated or
    // removed from a backing, so the first match of the search is that same entry
    // (SOTC_TEX_BIND_P1=0: always search).
    static const bool last_view_enabled = [] {
        const char* v = std::getenv("SOTC_TEX_BIND_P1");
        return !v || v[0] != '0';
    }();
    const auto& view_infos = backing->image_view_infos;
    if (last_view_enabled) {
        const u32 last = backing->last_view;
        if (last < view_infos.size() && view_infos[last] == view_info) {
            return (*slot_image_views)[backing->image_view_ids[last]];
        }
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        const auto index = std::distance(view_infos.begin(), it);
        backing->last_view = static_cast<u32>(index);
        const auto view_id = backing->image_view_ids[index];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(runtime->GetInstance(), view_info, *this);
    backing->last_view = static_cast<u32>(backing->image_view_infos.size());
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}


// sotc: GCN does not order UAV writes between draws by itself; the game inserts an end-of-pipe label
// where it needs the writes visible. A GENERAL image that was written and is used again with the
// same access therefore only needs the barrier once per fence epoch (the first use after a label),
// not before every draw: each of those barriers ends the dynamic render pass (~20k/s in the Shrine).
// SOTC_GENERAL_SYNC=0 restores a barrier at every use.
u64 g_sotc_fence_epoch = 0;

static bool SkipSameGeneral(Image& image, vk::ImageLayout a, vk::ImageLayout b, vk::AccessFlags2 ma,
                            vk::AccessFlags2 mb) {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_GENERAL_SYNC");
        return !(v && v[0] == '0');
    }();
    if (!on || a != vk::ImageLayout::eGeneral || b != a || ma != mb) {
        return false;
    }
    if (image.general_epoch == g_sotc_fence_epoch) {
        return true;
    }
    image.general_epoch = g_sotc_fence_epoch;
    return false;
}

// sotc: SOTC_LAYOUT_UNIFIED. A barrier that moved only the tracked layout (same access, no write before it) orders
// no hazard: with every image in GENERAL there is no layout change left to order, so it is dropped. Only the
// barriers that carry an access or a write are kept (their layouts become GENERAL, see Layouts::Pick). The first
// use of an image still moves it from UNDEFINED to GENERAL. =2 keeps every barrier and counts the drops.
static bool DropLayoutOnlyBarrier(const Image::State& prev, vk::ImageLayout dst_layout,
                                  vk::AccessFlags2 dst_mask) {
    if (!SotcArch::Layouts::On() && !SotcArch::Layouts::Verify()) {
        return false;
    }
    constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                 vk::AccessFlagBits2::eShaderWrite |
                                 vk::AccessFlagBits2::eMemoryWrite;
    // The real layout must not change either: a feedback loop kept by Pick (no unified feature) is a real change.
    const bool layout_only = prev.layout != dst_layout && prev.access_mask == dst_mask &&
                             prev.layout != vk::ImageLayout::eUndefined &&
                             SotcArch::Layouts::Pick(prev.layout) == SotcArch::Layouts::Pick(dst_layout) &&
                             !static_cast<bool>(prev.access_mask & write_flags);
    if (SotcArch::Layouts::Verify()) {
        if (layout_only) {
            SotcArch::Layouts::NoteVerifySkip();
        }
        return false;
    }
    if (layout_only) {
        SotcArch::Layouts::NoteSkipped();
        return true;
    }
    SotcArch::Layouts::NoteKept();
    return false;
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;
    // sotc: SOTC_TEX_BLOCK: the backing's state version moves when the state changes (at the end).
    const State before = last_state;
    const bool had_subresources = !subresource_states.empty();
    bool subresources_changed = false;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
            subresources_changed = true;
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask ||
                    (is_write && !SkipSameGeneral(*this, state.layout, dst_layout, state.access_mask, dst_mask))) {
                    const bool dropped = DropLayoutOnlyBarrier(state, dst_layout, dst_mask); // sotc: A1
                    if (!dropped) barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = SotcArch::Layouts::Pick(state.layout),
                        .newLayout = SotcArch::Layouts::Pick(dst_layout),
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    subresources_changed |= state.layout != dst_layout ||
                                            state.access_mask != dst_mask ||
                                            state.pl_stage != dst_stage;
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask &&
            (!is_write || SkipSameGeneral(*this, last_state.layout, dst_layout, last_state.access_mask, dst_mask))) {
            return;
        }
        const bool dropped = DropLayoutOnlyBarrier(last_state, dst_layout, dst_mask); // sotc: A1
        if (!dropped) barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = SotcArch::Layouts::Pick(last_state.layout),
            .newLayout = SotcArch::Layouts::Pick(dst_layout),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
    if (subresources_changed || before.layout != dst_layout || before.access_mask != dst_mask ||
        before.pl_stage != dst_stage || had_subresources != !subresource_states.empty()) {
        backing->state_version = NextBackingVersion(); // sotc: SOTC_TEX_BLOCK
    }
}

void ImageChurnReport() {
    if (!ImgPooledEnabled() && !AsyncImageCreateEnabled()) {
        return;
    }
    // Self-throttled: any thread may call it often; one caller per 5 s window wins the CAS.
    static std::atomic<u64> due{};
    static std::atomic<u64> last_created{}, last_destroyed{}, last_pooled{}, last_dedicated{};
    static std::atomic<u64> last_requested{}, last_skipped{}, last_built{}, last_taken{};
    static std::atomic<u64> last_waits{}, last_sync{}, last_taken_back{}, last_expired{};
    const u64 now = SotcStats::NowNs();
    u64 next = due.load(std::memory_order_relaxed);
    if (now < next || !due.compare_exchange_strong(next, now + 5'000'000'000ULL)) {
        return;
    }
    const auto delta = [](const std::atomic<u64>& now_value, std::atomic<u64>& last) {
        const u64 value = now_value.load(std::memory_order_relaxed);
        return value - last.exchange(value, std::memory_order_relaxed);
    };
    const u64 created = delta(images_created, last_created);
    const u64 destroyed = delta(images_destroyed, last_destroyed);
    const u64 pooled = delta(SotcStats::image_pool_allocs, last_pooled);
    const u64 dedicated = delta(SotcStats::image_dedicated, last_dedicated);
    const u64 requested = delta(SotcStats::async_image_requested, last_requested);
    const u64 skipped = delta(SotcStats::async_image_skipped, last_skipped);
    const u64 built = delta(SotcStats::async_image_built, last_built);
    const u64 taken = delta(SotcStats::async_image_taken, last_taken);
    const u64 waits = delta(SotcStats::async_image_waits, last_waits);
    const u64 sync = delta(SotcStats::async_image_sync, last_sync);
    const u64 taken_back = delta(SotcStats::async_image_taken_back, last_taken_back);
    const u64 expired = delta(SotcStats::async_image_expired, last_expired);
    std::fprintf(stderr,
                 "SOTC IMG CREATE 5s: images created %llu (pool %llu, dedicated %llu), freed %llu, "
                 "live %llu | ahead: queued %llu (refused %llu), made %llu, used ready %llu, "
                 "waited %llu, draw made %llu (taken back %llu), wasted %llu\n",
                 static_cast<unsigned long long>(created), static_cast<unsigned long long>(pooled),
                 static_cast<unsigned long long>(dedicated),
                 static_cast<unsigned long long>(destroyed),
                 static_cast<unsigned long long>(SotcStats::image_count.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(requested), static_cast<unsigned long long>(skipped),
                 static_cast<unsigned long long>(built), static_cast<unsigned long long>(taken),
                 static_cast<unsigned long long>(waits), static_cast<unsigned long long>(sync + taken_back),
                 static_cast<unsigned long long>(taken_back), static_cast<unsigned long long>(expired));
}

void NoteMipTailWrite(const Image& image) {
    // The image stays usable: it is GPU-modified now, so it is not freed without a download.
    SotcStats::rebase_violations.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
        LOG_ERROR(Render_Vulkan, "Mip-tail image {:#x} (from mip {}) written by the GPU",
                  image.info.guest_address, image.info.sotc_first_mip);
    }
}

} // namespace VideoCore
