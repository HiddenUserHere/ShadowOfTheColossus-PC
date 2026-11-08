// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <string>
#include <span>
#include <unordered_map>

#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_queues.h" // sotc: SOTC_N5_QUEUES family probe

#define TRACY_VK_USE_SYMBOL_TABLE
#include <tracy/TracyVulkan.hpp>

namespace Frontend {
class WindowSDL;
}

VK_DEFINE_HANDLE(VmaAllocator)

namespace Vulkan {

class Instance {
public:
    explicit Instance(bool validation = false, bool crash_diagnostic = false);
    explicit Instance(Frontend::WindowSDL& window, s32 physical_device_index,
                      bool enable_validation = false, bool enable_crash_diagnostic = false);
    ~Instance();

    /// Returns a formatted string for the driver version
    std::string GetDriverVersionName();

    /// Gets a compatibility format if the format is not supported.
    [[nodiscard]] vk::Format GetSupportedFormat(vk::Format format,
                                                vk::FormatFeatureFlags2 flags) const;

    /// Returns the Vulkan instance
    vk::Instance GetInstance() const {
        return *instance;
    }

    /// Returns the current physical device
    vk::PhysicalDevice GetPhysicalDevice() const {
        return physical_device;
    }

    /// Returns the Vulkan device
    vk::Device GetDevice() const {
        return *device;
    }

    /// Returns the VMA allocator handle
    VmaAllocator GetAllocator() const {
        return allocator;
    }

    /// Returns a list of the available physical devices
    std::span<const vk::PhysicalDevice> GetPhysicalDevices() const {
        return physical_devices;
    }

    /// Retrieve queue information
    u32 GetGraphicsQueueFamilyIndex() const {
        return queue_family_index;
    }

    /// sotc: SOTC_XFER_QUEUE=1: true when a transfer-only queue family was found and its queue
    /// created (XferQueue in vk_scheduler.h). Without it, the device has the graphics queue only.
    bool HasXferQueue() const {
        return xfer_queue_family_index != NoXferQueueFamily;
    }

    u32 GetXferQueueFamilyIndex() const {
        return xfer_queue_family_index;
    }

    /// sotc: SOTC_XFER_QUEUE=1: ordinary uploads may be routed to the transfer queue
    /// (Runtime::TryCopyBufferXfer). The queue may also exist for SOTC_HOST_IMPORT alone.
    bool IsXferRoutingEnabled() const {
        return xfer_routing && HasXferQueue();
    }

    /// sotc: SOTC_N5_QUEUES: the queue families found at device creation (vk_queues.h). Only
    /// meaningful with the switch on; the async compute family is recorded, not used yet.
    const SotcQueues::Probe& N5Probe() const {
        return n5_probe;
    }

    /// sotc: SOTC_RB_TICK=1 and the transfer queue exists: guest readbacks may be copied on it
    /// (XferQueue::ReadbackCopy). It creates the queue WITHOUT routing uploads to it. Chosen over
    /// queue family ownership transfers: every Buffer (arenas, staging) is then created
    /// concurrent over {graphics, transfer} (buffer.cpp), so no release/acquire barriers pair up.
    bool IsRbTickEnabled() const {
        return rb_tick && HasXferQueue();
    }

    /// sotc: SOTC_HOST_IMPORT=1, VK_EXT_external_memory_host is enabled and the transfer queue
    /// exists (BufferCache::HostImport).
    bool IsHostImportEnabled() const {
        return host_import_enabled;
    }

    /// sotc: minImportedHostPointerAlignment (SOTC_HOST_IMPORT).
    u64 HostImportAlignment() const {
        return host_import_alignment;
    }

    /// sotc: SOTC_TRACE_GPU (sotc_gpu_trace.h). A calibrated time domain extension is enabled.
    bool IsGpuTraceCalibrated() const {
        return gpu_trace_calibrated;
    }

    /// sotc: SOTC_TRACE_GPU. The enabled extension is VK_KHR_calibrated_timestamps (else EXT).
    bool IsGpuTraceCalibratedKhr() const {
        return gpu_trace_calibrated_khr;
    }

    /// sotc: SOTC_TRACE_GPU. The QueryPerformanceCounter time domain is listed (Windows).
    bool IsGpuTraceHostDomain() const {
        return gpu_trace_host_domain;
    }

    /// sotc: SOTC_TRACE_GPU. timestampValidBits of the graphics queue family (0: no timestamps).
    u32 GetTimestampValidBits() const {
        return timestamp_valid_bits;
    }

    /// sotc: SOTC_TRACE_GPU. limits.timestampPeriod, in ns per tick.
    float GetTimestampPeriodNs() const {
        return properties.limits.timestampPeriod;
    }

    /// sotc: SOTC_N5_ASYNC_COMPUTE=1: the compute-only queue of AsyncCompute (vk_scheduler.h). It is
    /// the queue index async_pick names: a family of its own, or queue 1 of the upload family.
    bool HasAsyncQueue() const {
        return async_pick.family != SotcQueues::NoFamily;
    }

    u32 GetAsyncQueueFamilyIndex() const {
        return async_pick.family;
    }

    vk::Queue GetAsyncQueue() const {
        return async_queue;
    }

    vk::Queue GetXferQueue() const {
        return xfer_queue;
    }

    vk::Queue GetGraphicsQueue() const {
        return graphics_queue;
    }

    vk::Queue GetPresentQueue() const {
        return present_queue;
    }

    TracyVkCtx GetProfilerContext() const {
        return profiler_context;
    }

    /// Returns true if anisotropic filtering is supported
    bool IsAnisotropicFilteringSupported() const {
        return features.samplerAnisotropy;
    }

    /// Returns true if depth bounds testing is supported
    bool IsDepthBoundsSupported() const {
        return features.depthBounds;
    }

    /// Returns true if 16-bit floats are supported in shaders
    bool IsShaderFloat16Supported() const {
        return vk12_features.shaderFloat16;
    }

    /// Returns true if 64-bit floats are supported in shaders
    bool IsShaderFloat64Supported() const {
        return features.shaderFloat64;
    }

    /// Returns true if 64-bit ints are supported in shaders
    bool IsShaderInt64Supported() const {
        return features.shaderInt64;
    }

    /// Returns true if 16-bit ints are supported in shaders
    bool IsShaderInt16Supported() const {
        return features.shaderInt16;
    }

    /// Returns true if 8-bit ints are supported in shaders
    bool IsShaderInt8Supported() const {
        return vk12_features.shaderInt8;
    }

    /// Returns true if VK_KHR_maintenance8 is supported
    bool IsMaintenance8Supported() const {
        return maintenance_8;
    }

    /// sotc: VK_EXT_external_memory_host, enabled with SOTC_PC_MEMORY=1 only
    bool IsExternalMemoryHostEnabled() const {
        return external_memory_host;
    }

    /// Returns true if VK_EXT_attachment_feedback_loop_layout is supported
    bool IsAttachmentFeedbackLoopLayoutSupported() const {
        return attachment_feedback_loop;
    }

    /// Returns true when VK_EXT_custom_border_color is supported
    bool IsCustomBorderColorSupported() const {
        return custom_border_color;
    }

    /// Returns true when VK_EXT_shader_stencil_export is supported
    bool IsShaderStencilExportSupported() const {
        return shader_stencil_export;
    }

    /// Returns true when VK_EXT_depth_clip_control is supported
    bool IsDepthClipControlSupported() const {
        return depth_clip_control;
    }

    /// Returns true when VK_EXT_depth_clip_enable is supported
    bool IsDepthClipEnableSupported() const {
        return depth_clip_enable;
    }

    /// Returns true when VK_EXT_depth_range_unrestricted is supported
    bool IsDepthRangeUnrestrictedSupported() const {
        return depth_range_unrestricted;
    }

    /// Returns true when VK_EXT_extended_dynamic_state3 is supported
    bool IsExtendedDynamicState3Supported() const {
        return dynamic_state_3;
    }

    /// Returns true when the extendedDynamicState3ColorWriteMask feature of
    /// VK_EXT_extended_dynamic_state3 is supported.
    bool IsDynamicColorWriteMaskSupported() const {
        return dynamic_state_3 && dynamic_state_3_features.extendedDynamicState3ColorWriteMask;
    }

    /// sotc: SOTC_GPL_DYN_STATE (sotc/nonblocking.h). States that are dynamic in every graphics pipeline, set per draw
    /// from the pipeline key (DynamicState::SetKeyStatics). Each is false when the switch is off or the device lacks
    /// the feature; the state is then baked in the pipeline (and in its library key) as before.
    struct KeyDynamicStates {
        bool topology{};       ///< primitive topology (core 1.3); the vertex input library keeps the topology class
        bool polygon_mode{};   ///< extendedDynamicState3PolygonMode
        bool depth_clamp{};    ///< extendedDynamicState3DepthClampEnable
        bool depth_clip{};     ///< extendedDynamicState3DepthClipEnable (with VK_EXT_depth_clip_enable)
        bool provoking{};      ///< extendedDynamicState3ProvokingVertexMode (with VK_EXT_provoking_vertex)
        bool clip_negative{};  ///< extendedDynamicState3DepthClipNegativeOneToOne (with VK_EXT_depth_clip_control)
        bool samples{};        ///< extendedDynamicState3RasterizationSamples
    };
    const KeyDynamicStates& KeyDynamic() const {
        return key_dynamic;
    }

    /// Returns true when VK_EXT_vertex_input_dynamic_state is supported.
    bool IsVertexInputDynamicState() const {
        return vertex_input_dynamic_state;
    }

    /// sotc: SOTC_DRAW_MDI. VK_EXT_multi_draw is enabled with its multiDraw feature.
    bool IsMultiDrawSupported() const {
        return multi_draw;
    }

    /// sotc: SOTC_DRAW_MDI. maxMultiDrawCount of VK_EXT_multi_draw (0 when it is off).
    u32 MaxMultiDrawCount() const {
        return max_multi_draw_count;
    }

    /// sotc: true when SOTC_PIPE_GPL=1 and VK_EXT_graphics_pipeline_library (with
    /// graphicsPipelineLibrary) is enabled. Read once in CreateDevice.
    bool IsGraphicsPipelineLibrary() const {
        return graphics_pipeline_library;
    }

    /// Returns true when VK_KHR_fragment_shader_barycentric is supported.
    bool IsFragmentShaderBarycentricSupported() const {
        return fragment_shader_barycentric;
    }

    /// Returns true when VK_AMD_shader_explicit_vertex_parameter is supported.
    bool IsAmdShaderExplicitVertexParameterSupported() const {
        return amd_shader_explicit_vertex_parameter;
    }

    /// Returns true when VK_EXT_primitive_topology_list_restart is supported for regular lists.
    bool IsListRestartSupported() const {
        return list_restart && list_restart_features.primitiveTopologyListRestart;
    }

    /// Returns true when VK_EXT_primitive_topology_list_restart is supported for patch lists.
    bool IsPatchListRestartSupported() const {
        return list_restart && list_restart_features.primitiveTopologyPatchListRestart;
    }

    /// Returns true when VK_EXT_provoking_vertex is supported.
    bool IsProvokingVertexSupported() const {
        return provoking_vertex;
    }

    /// Returns true when VK_AMD_shader_image_load_store_lod is supported.
    bool IsImageLoadStoreLodSupported() const {
        return image_load_store_lod;
    }

    /// Returns true when VK_AMD_gcn_shader is supported.
    bool IsAmdGcnShaderSupported() const {
        return amd_gcn_shader;
    }

    /// Returns true when VK_AMD_shader_trinary_minmax is supported.
    bool IsAmdShaderTrinaryMinMaxSupported() const {
        return amd_shader_trinary_minmax;
    }

    /// Returns true when the shaderBufferFloat32AtomicMinMax feature of
    /// VK_EXT_shader_atomic_float2 is supported.
    bool IsShaderAtomicFloatBuffer32MinMaxSupported() const {
        return shader_atomic_float2 &&
               shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax;
    }

    /// Returns true when the shaderImageFloat32AtomicMinMax feature of
    /// VK_EXT_shader_atomic_float2 is supported.
    bool IsShaderAtomicFloatImage32MinMaxSupported() const {
        return shader_atomic_float2 && shader_atomic_float2_features.shaderImageFloat32AtomicMinMax;
    }

    /// Returns true if 64-bit integer atomic operations can be used on buffers
    bool IsBufferInt64AtomicsSupported() const {
        return vk12_features.shaderBufferInt64Atomics;
    }

    /// Returns true if 64-bit integer atomic operations can be used on shared memory
    bool IsSharedInt64AtomicsSupported() const {
        return vk12_features.shaderSharedInt64Atomics;
    }

    /// sotc: true when SOTC_BINDLESS_SAMP=1 was set and the device has the descriptor indexing
    /// features for a global sampler heap (runtime descriptor array of update-after-bind samplers).
    /// Off by default; the shader cache version depends on it.
    bool IsBindlessSamplersEnabled() const {
        return bindless_samplers;
    }

    /// sotc: upper bound on update-after-bind sampler descriptors per set (device limit).
    u32 GetMaxDescriptorSetUpdateAfterBindSamplers() const {
        return vk12_props.maxDescriptorSetUpdateAfterBindSamplers;
    }

    /// Returns true if the subgroup size can be set to match guest subgroup size in every stage.
    /// sotc: AMD reports 64-wide subgroups but can require a size only for compute
    /// (requiredSubgroupSizeStages = compute); requiring 64 in the graphics stages was invalid and
    /// the compute shaders skipped the wave64 lowering, which gave AMD cards random green/pink
    /// fog and characters (issue #7, raulbezerro-dev). Those cards use the 32-wide path instead.
    bool IsSubgroupSize64Supported() const {
        constexpr auto stages = vk::ShaderStageFlagBits::eVertex |
                                vk::ShaderStageFlagBits::eFragment |
                                vk::ShaderStageFlagBits::eCompute;
        return vk13_features.subgroupSizeControl && vk13_props.maxSubgroupSize >= 64 &&
               (vk13_props.requiredSubgroupSizeStages & stages) == stages;
    }

    /// sotc: true when compute shaders must be pinned to 32-wide subgroups: the device could run
    /// them wider, but the recompiler lowers wave64 code for 32 lanes (the NVIDIA path).
    bool IsComputeSubgroupSize32Required() const {
        return !IsSubgroupSize64Supported() && vk13_features.subgroupSizeControl &&
               vk13_props.minSubgroupSize <= 32 && vk13_props.maxSubgroupSize > 32 &&
               (vk13_props.requiredSubgroupSizeStages & vk::ShaderStageFlagBits::eCompute);
    }

    /// sotc: whether a required subgroup size may be given for this stage.
    bool CanRequireSubgroupSize(vk::ShaderStageFlagBits stage) const {
        return bool(vk13_props.requiredSubgroupSizeStages & stage);
    }

    /// sotc: the subgroup size a pipeline stage is created with, or 0 to leave it to the driver.
    /// Intel runs 8, 16 or 32 lanes: left free, its compiler builds each shader for several SIMD
    /// widths, and first-time compiles took from seconds to minutes (the screen froze meanwhile),
    /// while the recompiler lowered wave64 code for SubgroupSize() lanes anyway. Intel stages are
    /// pinned to that size; SOTC_INTEL_PIN_SUBGROUP=0 turns it off. From AjncSystem#81 (jownatan).
    u32 RequiredSubgroupSize(vk::ShaderStageFlagBits stage) const {
        if (IsSubgroupSize64Supported()) {
            return CanRequireSubgroupSize(stage) ? 64u : 0u;
        }
        if (stage == vk::ShaderStageFlagBits::eCompute && IsComputeSubgroupSize32Required()) {
            return 32u;
        }
        static const bool pin_intel = [] {
            const char* v = std::getenv("SOTC_INTEL_PIN_SUBGROUP");
            return !v || std::atoi(v) != 0;
        }();
        const u32 size = SubgroupSize();
        if (pin_intel && GetVendorID() == 0x8086 && vk13_features.subgroupSizeControl &&
            CanRequireSubgroupSize(stage) && size >= vk13_props.minSubgroupSize &&
            size <= vk13_props.maxSubgroupSize) {
            return size;
        }
        return 0u;
    }

    /// Returns true when VK_KHR_workgroup_memory_explicit_layout is supported.
    bool IsWorkgroupMemoryExplicitLayoutSupported() const {
        return workgroup_memory_explicit_layout &&
               workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess;
    }

    /// Returns true if VK_NV_framebuffer_mixed_samples or
    /// VK_AMD_mixed_attachment_samples is supported
    bool IsMixedDepthSamplesSupported() const {
        return nv_framebuffer_mixed_samples || amd_mixed_attachment_samples;
    }

    /// Returns true if VK_AMD_mixed_attachment_samples is supported
    bool IsMixedAnySamplesSupported() const {
        return amd_mixed_attachment_samples;
    }

    /// Returns true when geometry shaders are supported by the device
    bool IsGeometryStageSupported() const {
        return features.geometryShader;
    }

    /// Returns true when tessellation is supported by the device
    bool IsTessellationSupported() const {
        return features.tessellationShader;
    }

    /// Returns true when the shaderSubgroupClock feature of
    /// VK_KHR_shader_clock is supported.
    bool IsShaderSubgroupClockSupported() const {
        return shader_clock && shader_clock_features.shaderSubgroupClock;
    }

    /// sotc: VK_KHR_present_id and VK_KHR_present_wait are enabled by SOTC_PRESENT_WAIT (on by default). The
    /// presenter tags each present with an id and can wait on them.
    bool IsPresentWaitEnabled() const {
        return present_wait_enabled;
    }

    /// sotc: SOTC_PRESENT_WAIT itself: FIFO is forced and the vblank thread paces the flips on
    /// the presents.
    bool IsPresentWaitPacingEnabled() const {
        return present_wait_pacing;
    }

    /// sotc: VK_KHR_present_id is enabled (SOTC_PRESENT_WAIT or SOTC_REFLEX). Presents are then
    /// tagged with an id.
    bool IsPresentIdEnabled() const {
        return present_id_enabled;
    }

    /// sotc: SOTC_LOW_LATENCY=1 (or SOTC_REFLEX=1) was set. The backend is picked by LowLatency.
    bool IsLowLatencyRequested() const {
        return low_latency_requested;
    }

    /// sotc: VK_NV_low_latency2 is enabled (SOTC_LOW_LATENCY, NVIDIA devices only).
    bool IsNvLowLatencyEnabled() const {
        return nv_low_latency_enabled;
    }

    /// sotc: VK_AMD_anti_lag is enabled with its feature (SOTC_LOW_LATENCY, AMD devices only).
    bool IsAmdAntiLagEnabled() const {
        return amd_anti_lag_enabled;
    }

    /// Returns the vendor ID of the physical device
    u32 GetVendorID() const {
        return properties.vendorID;
    }

    /// sotc: a one-dimensional dispatch of `groups` workgroups for a host shader that reads its
    /// index as x + y * NumWorkGroups.x * WorkGroupSize.x. Above maxComputeWorkGroupCount[0]
    /// (Intel: 65535; NVIDIA and AMD allow 2^31 and more) it is sent as x * y groups with
    /// x * y == groups exactly (the counts are page or texel counts: powers of two times small
    /// factors), as a larger dispatch is invalid and hung Intel GPUs: the texture detiler
    /// dispatches one group per 64 texels, 81920 for a 2048x2560 32-bit texture (issue #59).
    void Dispatch1D(vk::CommandBuffer cmdbuf, u32 groups) const {
        const auto& max = properties.limits.maxComputeWorkGroupCount;
        if (groups > max[0]) {
            for (u32 y = (groups + max[0] - 1) / max[0]; y <= max[1] && y <= groups; ++y) {
                if (groups % y == 0 && groups / y <= max[0]) {
                    cmdbuf.dispatch(groups / y, y, 1);
                    return;
                }
            }
        }
        cmdbuf.dispatch(groups, 1, 1);
    }

    /// Returns the device ID of the physical device
    u32 GetDeviceID() const {
        return properties.deviceID;
    }

    /// Returns the driver ID.
    vk::DriverId GetDriverID() const {
        return driver_id;
    }

    /// Returns the current driver version provided in Vulkan-formatted version numbers.
    u32 GetDriverVersion() const {
        return properties.driverVersion;
    }

    /// Returns the current Vulkan API version provided in Vulkan-formatted version numbers.
    u32 ApiVersion() const {
        return properties.apiVersion;
    }

    /// Returns the vendor name reported from Vulkan.
    std::string_view GetVendorName() const {
        return vendor_name;
    }

    /// Returns the list of available extensions.
    std::span<const std::string> GetAvailableExtensions() const {
        return available_extensions;
    }

    /// Returns the device name.
    std::string_view GetModelName() const {
        return properties.deviceName;
    }

    /// Returns if the device is an integrated GPU.
    bool IsIntegrated() const {
        return properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
    }

    /// sotc: true when a discrete GPU exposes a DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT memory
    /// type in a heap larger than 256 MiB (Resizable BAR). Always false on integrated GPUs.
    bool HasLargeBar() const {
        return has_large_bar;
    }

    /// sotc: index of the heap reported by HasLargeBar(). Only meaningful when it is true.
    u32 LargeBarHeapIndex() const {
        return large_bar_heap;
    }

    /// sotc: SOTC_REBAR_UPLOAD=1 (default off) and a large BAR is present.
    bool RebarUploadEnabled() const {
        return rebar_upload_enabled;
    }

    /// Returns the pipeline cache unique identifier
    const auto GetPipelineCacheUUID() const {
        return properties.pipelineCacheUUID;
    }

    /// Returns the minimum required alignment for uniforms
    vk::DeviceSize UniformMinAlignment() const {
        return properties.limits.minUniformBufferOffsetAlignment;
    }

    /// Returns the minimum required alignment for storage buffers
    vk::DeviceSize StorageMinAlignment() const {
        return properties.limits.minStorageBufferOffsetAlignment;
    }

    /// Returns the minimum alignment required for accessing host-mapped device memory
    vk::DeviceSize NonCoherentAtomSize() const {
        return properties.limits.nonCoherentAtomSize;
    }

    /// Returns the subgroup size the recompiler targets (64 or 32 when it can be required).
    u32 SubgroupSize() const {
        if (IsSubgroupSize64Supported()) {
            return 64;
        }
        if (IsComputeSubgroupSize32Required()) {
            return 32;
        }
        return vk11_props.subgroupSize;
    }

    /// Returns the maximum size of compute shared memory.
    u32 MaxComputeSharedMemorySize() const {
        return properties.limits.maxComputeSharedMemorySize;
    }

    /// Returns the maximum sampler LOD bias.
    float MaxSamplerLodBias() const {
        return properties.limits.maxSamplerLodBias;
    }

    /// Returns the maximum sampler anisotropy.
    float MaxSamplerAnisotropy() const {
        return properties.limits.maxSamplerAnisotropy;
    }

    /// Returns the maximum number of push descriptors.
    u32 MaxPushDescriptors() const {
        return push_descriptor_props.maxPushDescriptors;
    }

    /// sotc: SOTC_BIND_DESC_REUSE: the most dynamic storage buffers a pipeline layout can hold (the flat buffers).
    u32 MaxDescriptorSetStorageBuffersDynamic() const {
        return properties.limits.maxDescriptorSetStorageBuffersDynamic;
    }

    /// sotc: the most descriptor sets a pipeline layout can bind (SOTC_STAGE_BINDINGS needs 5).
    u32 GetMaxBoundDescriptorSets() const {
        return properties.limits.maxBoundDescriptorSets;
    }

    /// sotc: true when descriptors live in host-written descriptor buffers (VK_EXT_descriptor_buffer,
    /// SOTC_DESC_BUFFER=1). Push descriptors are not used in that mode.
    bool IsDescriptorBufferEnabled() const noexcept {
        return descriptor_buffer;
    }

    /// sotc: true when VK_EXT_descriptor_buffer was probed for SOTC_N5_DESCBUF (modes 1 and 2). Kept
    /// apart from IsDescriptorBufferEnabled: the layouts of the two schemes differ.
    bool IsDescriptorBufferN5Enabled() const noexcept {
        return descriptor_buffer_n5;
    }

    /// sotc: true when SOTC_DESCRIPTOR_HEAP=1 was requested and VK_EXT_descriptor_heap is enabled with its
    /// limits fitting the port's heaps (vk_descriptor_heap.h). Decided at device creation; never with the
    /// descriptor buffer switches. False in every other case, and the device is then the same as before.
    bool IsDescriptorHeapEnabled() const noexcept {
        return descriptor_heap_mode;
    }

    /// sotc: the descriptor heap limits, valid when IsDescriptorHeapEnabled().
    const vk::PhysicalDeviceDescriptorHeapPropertiesEXT& GetDescriptorHeapProperties() const noexcept {
        return descriptor_heap_props;
    }

    /// sotc: true when VK_EXT_host_image_copy is enabled (SOTC_HOST_IMAGE_COPY=1). Images created
    /// with VK_IMAGE_USAGE_HOST_TRANSFER_BIT may then be written by the host (Runtime::TryHostImageCopy).
    bool IsHostImageCopyEnabled() const noexcept {
        return host_image_copy;
    }

    /// sotc: descriptor sizes and offset alignment, valid when IsDescriptorBufferEnabled().
    const vk::PhysicalDeviceDescriptorBufferPropertiesEXT& GetDescriptorBufferProperties()
        const noexcept {
        return descriptor_buffer_props;
    }

    // sotc: feature probes of the draw architecture (ARCH-DRAW.md, sotc_arch/sotc_arch.h). Each is
    // false when its feature is missing or its switch is off; the switch is read once at device
    // creation, and nothing is enabled for a switch that is unset.

    /// sotc: SOTC_LAYOUT_UNIFIED and VK_KHR_unified_image_layouts with unifiedImageLayouts.
    bool SotcUnifiedLayouts() const {
        return sotc_unified_layouts;
    }

    /// sotc: SOTC_HEAP and VK_EXT_descriptor_heap with descriptorHeap (never together with
    /// SOTC_DESC_BUFFER or SOTC_N5_DESCBUF).
    bool SotcDescriptorHeap() const {
        return sotc_descriptor_heap;
    }

    /// sotc: SOTC_MIRROR, sparseBinding and sparseResidencyBuffer, and the graphics queue family
    /// supports sparse binding (SotcSparseQueueFamily). The sparse features are enabled at device
    /// creation whenever the device has them.
    bool SotcSparseBuffers() const {
        return sotc_sparse_buffers;
    }

    /// sotc: SOTC_MIRROR and sparseResidencyAliased (enabled at device creation). Sparse buffers made with
    /// VK_BUFFER_CREATE_SPARSE_ALIASED_BIT, so that the mirror and an arena can bind the same memory.
    bool SotcSparseAliased() const {
        return sotc_sparse_aliased;
    }

    /// sotc: returned when no queue family can bind sparse memory (SotcSparseBuffers() is false).
    static constexpr u32 NoSparseQueueFamily = ~u32{0};

    /// sotc: the queue family index with VK_QUEUE_SPARSE_BINDING_BIT (the graphics family), or
    /// NoSparseQueueFamily. The queue is family queue index 0.
    u32 SotcSparseQueueFamily() const {
        return sotc_sparse_queue_family;
    }

    /// sotc: SOTC_PASSES and VK_EXT_device_generated_commands with deviceGeneratedCommands.
    bool SotcDeviceGeneratedCommands() const {
        return sotc_device_generated_commands;
    }

    /// sotc: bufferDeviceAddress (enabled at device creation whenever the device has it) and one of
    /// SOTC_MIRROR, SOTC_SHADER_ADDR or SOTC_PACKET.
    bool SotcBufferDeviceAddress() const {
        return sotc_buffer_device_address;
    }

    /// Returns the maximum size of a single VkDeviceMemory
    vk::DeviceSize MaxMemoryAllocationSize() const {
        return vk11_props.maxMemoryAllocationSize;
    }

    /// Returns the vulkan 1.2 physical device properties.
    const vk::PhysicalDeviceVulkan12Properties& GetVk12Properties() const noexcept {
        return vk12_props;
    }

    /// Returns the memory properties of the physical device.
    const vk::PhysicalDeviceMemoryProperties& GetMemoryProperties() const noexcept {
        return memory_properties;
    }

    /// Returns true if shaders can declare the ClipDistance attribute
    bool IsShaderClipDistanceSupported() const {
        return features.shaderClipDistance;
    }

    /// Returns the maximim viewport width.
    u32 GetMaxViewportWidth() const {
        return properties.limits.maxViewportDimensions[0];
    }

    /// Returns the maximum viewport height.
    u32 GetMaxViewportHeight() const {
        return properties.limits.maxViewportDimensions[1];
    }

    /// Returns the maximum render area width.
    u32 GetMaxFramebufferWidth() const {
        return properties.limits.maxFramebufferWidth;
    }

    /// Returns the maximum render area height.
    u32 GetMaxFramebufferHeight() const {
        return properties.limits.maxFramebufferHeight;
    }

    /// Returns the maximum number of samplers that can be allocated at once.
    u32 GetMaxSamplerAllocationCount() const {
        if (driver_id == vk::DriverId::eMesaKosmickrisp) {
            // FIXME: KosmicKrisp has an internal 1024 unique sampler limit before
            // vkCreateSampler starts returning VK_ERROR_OUT_OF_HOST_MEMORY. Work
            // around this for now by reducing the value to 1024.
            return 1024;
        }
        return properties.limits.maxSamplerAllocationCount;
    }

    /// Returns the sample count flags supported by color buffers.
    vk::SampleCountFlags GetColorSampleCounts() const {
        return properties.limits.framebufferColorSampleCounts;
    }

    /// Returns the sample count flags supported by depth buffer.
    vk::SampleCountFlags GetDepthSampleCounts() const {
        return properties.limits.framebufferDepthSampleCounts &
               properties.limits.framebufferStencilSampleCounts;
    }

    /// Returns true if logic ops are supported by the device.
    bool IsLogicOpSupported() const {
        return features.logicOp;
    }

    /// Returns whether VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT is supported on compressed
    /// images.
    bool IsBlockTexelViewSupported() const {
        return supports_block_texel_view;
    }

    /// Returns whether VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT is supported on 3D images
    bool Is2dViewOf3dSupported() const {
        return image_2d_view_of_3d && image_2d_view_of_3d_features.image2DViewOf3D &&
               image_2d_view_of_3d_features.sampler2DViewOf3D;
    }

    /// Returns whether VK_EXT_image_view_min_lod is supported.
    bool IsImageViewMinLodSupported() const {
        return image_view_min_lod;
    }

    /// Returns whether shaderStorageImageMultisample is supported.
    bool IsMultisampleStorageImageSupported() const {
        return features.shaderStorageImageMultisample;
    }

    /// Returns whether the device can report memory usage.
    bool CanReportMemoryUsage() const {
        return supports_memory_budget;
    }

    /// Returns the amount of memory used.
    [[nodiscard]] u64 GetDeviceMemoryUsage() const;

    /// Returns the total memory budget available to the device.
    [[nodiscard]] u64 GetTotalMemoryBudget() const {
        return total_memory_budget;
    }

    /// Determines if a format is supported for a set of feature flags.
    [[nodiscard]] bool IsFormatSupported(vk::Format format, vk::FormatFeatureFlags2 flags) const;

    /// Logs whatever the driver reports about a device loss (VK_EXT_device_fault).
    void ReportDeviceFault() const;

private:
    /// Creates the logical device opportunistically enabling extensions
    /// window: the surface of the swapchain is not created yet, so CreateDevice makes a probe
    /// surface for the present family check and destroys it.
    bool CreateDevice(Frontend::WindowSDL& window);

    /// Creates the VMA allocator handle
    void CreateAllocator();

    /// sotc: looks for a Resizable BAR heap and logs the SOTC_REBAR_UPLOAD decision once.
    void DetectLargeBar();

    /// Collects various information from the device.
    void CollectDeviceParameters();
    void CollectPhysicalMemoryInfo();
    void CollectImageFormatInfo();
    void CollectToolingInfo() const;

    /// Gets the supported feature flags for a format.
    [[nodiscard]] vk::FormatFeatureFlags2 GetFormatFeatureFlags(vk::Format format) const;

private:
    vk::UniqueInstance instance;
    vk::PhysicalDevice physical_device;
    vk::UniqueDevice device;
    vk::PhysicalDeviceProperties properties;
    vk::PhysicalDeviceMemoryProperties memory_properties;
    vk::PhysicalDeviceVulkan11Properties vk11_props;
    vk::PhysicalDeviceVulkan12Properties vk12_props;
    vk::PhysicalDeviceVulkan13Properties vk13_props;
    vk::PhysicalDevicePushDescriptorPropertiesKHR push_descriptor_props;
    vk::PhysicalDeviceFeatures features;
    vk::PhysicalDeviceVulkan12Features vk12_features;
    vk::PhysicalDeviceVulkan13Features vk13_features;
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic_state_3_features;
    vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT shader_atomic_float2_features;
    vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR
        workgroup_memory_explicit_layout_features;
    vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT image_2d_view_of_3d_features;
    vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT list_restart_features;
    vk::PhysicalDeviceShaderClockFeaturesKHR shader_clock_features;
    vk::PhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_props;
    vk::DriverIdKHR driver_id;
    vk::UniqueDebugUtilsMessengerEXT debug_callback{};
    std::string vendor_name;
    VmaAllocator allocator{};
    vk::Queue present_queue;
    vk::Queue graphics_queue;
    std::vector<vk::PhysicalDevice> physical_devices;
    std::vector<std::string> available_extensions;
    std::unordered_map<vk::Format, vk::FormatProperties3> format_properties;
    TracyVkCtx profiler_context{};
    u32 queue_family_index{0};
    static constexpr u32 NoXferQueueFamily = ~u32{0};
    u32 xfer_queue_family_index{NoXferQueueFamily}; // sotc: see HasXferQueue()
    vk::Queue xfer_queue;                           // sotc: see HasXferQueue()
    SotcQueues::AsyncPick async_pick{}; // sotc: see HasAsyncQueue()
    vk::Queue async_queue;              // sotc: valid when HasAsyncQueue()
    bool xfer_routing{};          // sotc: see IsXferRoutingEnabled()
    SotcQueues::Probe n5_probe{}; // sotc: see N5Probe()
    bool rb_tick{};               // sotc: see IsRbTickEnabled()
    bool host_import_enabled{};   // sotc: see IsHostImportEnabled()
    u64 host_import_alignment{};  // sotc: see HostImportAlignment()
    bool gpu_trace_calibrated{};     // sotc: see IsGpuTraceCalibrated()
    bool gpu_trace_calibrated_khr{}; // sotc: see IsGpuTraceCalibratedKhr()
    bool gpu_trace_host_domain{};    // sotc: see IsGpuTraceHostDomain()
    u32 timestamp_valid_bits{};      // sotc: see GetTimestampValidBits()
    bool custom_border_color{};
    bool multi_draw{};                 // sotc: SOTC_DRAW_MDI, see IsMultiDrawSupported()
    u32 max_multi_draw_count{};        // sotc: see MaxMultiDrawCount()
    bool fragment_shader_barycentric{};
    bool amd_shader_explicit_vertex_parameter{};
    bool depth_clip_control{};
    bool depth_clip_enable{};
    bool dynamic_state_3{};
    KeyDynamicStates key_dynamic{}; // sotc: SOTC_GPL_DYN_STATE, see KeyDynamic()
    bool depth_range_unrestricted{};
    bool vertex_input_dynamic_state{};
    bool graphics_pipeline_library{}; // sotc: SOTC_PIPE_GPL
    bool list_restart{};
    bool provoking_vertex{};
    bool shader_stencil_export{};
    bool image_load_store_lod{};
    bool amd_gcn_shader{};
    bool amd_shader_trinary_minmax{};
    bool nv_framebuffer_mixed_samples{};
    bool amd_mixed_attachment_samples{};
    bool shader_atomic_float{};
    bool shader_atomic_float2{};
    bool workgroup_memory_explicit_layout{};
    bool maintenance_5{};
    bool maintenance_8{};
    bool external_memory_host{}; // sotc
    bool attachment_feedback_loop{};
    bool image_2d_view_of_3d{};
    bool image_view_min_lod{};
    bool shader_clock{};
    bool supports_memory_budget{};
    bool has_large_bar{};         // sotc: see HasLargeBar()
    bool rebar_upload_enabled{};  // sotc: see RebarUploadEnabled()
    u32 large_bar_heap{};         // sotc: see LargeBarHeapIndex()
    bool device_fault{};
    bool device_fault_vendor_binary{};
    bool descriptor_buffer{}; // sotc: VK_EXT_descriptor_buffer enabled (SOTC_DESC_BUFFER=1)
    bool descriptor_buffer_n5{}; // sotc: VK_EXT_descriptor_buffer probed for SOTC_N5_DESCBUF
    bool host_image_copy{};   // sotc: VK_EXT_host_image_copy enabled (SOTC_HOST_IMAGE_COPY=1)
    bool sotc_unified_layouts{};            // sotc: see SotcUnifiedLayouts()
    bool sotc_descriptor_heap{};            // sotc: see SotcDescriptorHeap()
    bool descriptor_heap_mode{};            // sotc: SOTC_DESCRIPTOR_HEAP=1 and the limits fit (IsDescriptorHeapEnabled)
    vk::PhysicalDeviceDescriptorHeapPropertiesEXT descriptor_heap_props{}; // sotc: valid in descriptor_heap_mode
    bool sotc_sparse_buffers{};             // sotc: see SotcSparseBuffers()
    bool sotc_sparse_aliased{};             // sotc: see SotcSparseAliased()
    u32 sotc_sparse_queue_family{NoSparseQueueFamily}; // sotc: see SotcSparseQueueFamily()
    bool sotc_device_generated_commands{};  // sotc: see SotcDeviceGeneratedCommands()
    bool sotc_buffer_device_address{};      // sotc: see SotcBufferDeviceAddress()
    bool present_wait_enabled{}; // sotc: VK_KHR_present_wait on, see IsPresentWaitEnabled
    bool present_wait_pacing{};  // sotc: SOTC_PRESENT_WAIT, see IsPresentWaitPacingEnabled
    bool present_id_enabled{};   // sotc: VK_KHR_present_id on, see IsPresentIdEnabled
    bool low_latency_requested{};    // sotc: SOTC_LOW_LATENCY / SOTC_REFLEX, see IsLowLatencyRequested
    bool nv_low_latency_enabled{};   // sotc: VK_NV_low_latency2 on, see IsNvLowLatencyEnabled
    bool amd_anti_lag_enabled{};     // sotc: VK_AMD_anti_lag on, see IsAmdAntiLagEnabled
    bool supports_block_texel_view{};
    bool bindless_samplers{};
    u64 total_memory_budget{};
    std::vector<size_t> valid_heaps;
};

} // namespace Vulkan
