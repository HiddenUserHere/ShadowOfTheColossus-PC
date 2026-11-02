# VK_EXT_descriptor_heap: spec notes for SOTC_DESCRIPTOR_HEAP (phase 2)

Sources (all read in this session):

- Spec chapter, Descriptor Heaps: https://docs.vulkan.org/spec/latest/chapters/descriptorheaps.html
- Extension proposal: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html
- Vulkan Documentation Project guide: https://docs.vulkan.org/guide/latest/descriptor_heap.html
- Man pages: https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindResourceHeapEXT.html ,
  https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDataEXT.html ,
  https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorSetAndBindingMappingEXT.html
- Authoritative struct and enum names: the installed header
  `/c/msys64/clang64/include/vulkan/vulkan_core.h` (VK_HEADER_VERSION 363), lines 16728-17095 (EXT section),
  plus `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` (line 8337), `VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT`
  (line 23631), `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT` (line 3076), and `VkPipelineCreateFlags2CreateInfo` (line 8654).
- Khronos sample and proposal source: https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_descriptor_heap.adoc
  (not fetched in full; the fetch tool truncated the long pages at about 100 000 characters.)

Several answers below come from truncated pages. They are marked **(unverified)**.

## (a) Entering heap mode

- Pipelines: `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` (0x1000000000) in `VkPipelineCreateFlags2CreateInfo`
  chained into the create info (the 32-bit `flags` field has no such bit). Spec: "Pipelines must be created with
  VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT" (chapter, "Using Heaps").
- Shader objects: `VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT`.
- Pipeline layout: the proposal says that with this flag "the pipeline layout must be NULL" (proposal, section 1).
  `VkDescriptorSetLayout` and `VkPipelineLayout` are not required (guide, "Pipelines and Shaders").
- The mapping info is ignored when the pipeline or shader uses a pipeline layout or set layouts (proposal, section 1).
- Feature: `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` (header line 16942).
- Linking (VK_KHR_pipeline_library): all linked parts must all have the flag or all lack it (proposal, section 1).
  Phase 2 disables graphics pipeline libraries (SOTC_PIPE_GPL) for heap pipelines.

## (b) Push data

- `vkCmdPushDataEXT(commandBuffer, const VkPushDataInfoEXT*)`. `VkPushDataInfoEXT { sType, pNext, uint32_t offset,
  VkHostAddressRangeConstEXT data }` (header lines 16825-16830). `offset` and `data.size` are bytes.
- VUIDs (chapter, "Push Data"): `offset + data.size <= maxPushDataSize` (VUID-VkPushDataInfoEXT-offset-11243);
  `offset` is a multiple of 4 (-11418); `data.size` is a multiple of 4 (-11419).
- `maxPushDataSize` is at least 256 bytes; the proposal recommends 512 for tooling.
- Shaders read it through the ordinary `PushConstant` storage class. The SPIR-V does not change.
- `vkCmdPushConstants` is not compatible with descriptor heaps: its state "rel[ies] on descriptor set layout state"
  (chapter). `vkCmdPushDataEXT` invalidates, and is invalidated by, push-constant and push-descriptor state
  (proposal, section 2). Consequence for phase 2: after a heap draw, the push-constant dedup cache must be voided
  (`DynamicState::InvalidatePushConstants`).
- `VkPushConstantBankInfoNV` may be chained (pushConstantBank); not used.

## (c) Mapping info (where it is chained, what each source does)

- Chained into `VkPipelineShaderStageCreateInfo::pNext` (or `VkShaderCreateInfoEXT` with SPIR-V code). Each stage
  has its own list. Header: `VkShaderDescriptorSetAndBindingMappingInfoEXT { sType, pNext, mappingCount, pMappings }`
  (lines 16929-16934).
- `VkDescriptorSetAndBindingMappingEXT { sType, pNext, descriptorSet, firstBinding, bindingCount, resourceMask,
  source, sourceData }` (lines 16918-16927). A variable is covered when its DescriptorSet matches, its Binding is in
  `[firstBinding, firstBinding + bindingCount)`, and its type matches `resourceMask`. "Overspecifying bindings or
  resource types is allowed" (chapter). Phase 2 uses `VK_SPIRV_RESOURCE_TYPE_ALL_EXT` (0x7FFFFFFF) per binding.
- Sources (`VkDescriptorMappingSourceEXT`, lines 16733-16746) and their `sourceData` members:
  - `HEAP_WITH_CONSTANT_OFFSET` (0): `constantOffset { heapOffset, heapArrayStride, pEmbeddedSampler,
    samplerHeapOffset, samplerHeapArrayStride }`.
  - `HEAP_WITH_PUSH_INDEX` (1): `pushIndex { heapOffset, pushOffset, heapIndexStride, heapArrayStride,
    pEmbeddedSampler, useCombinedImageSamplerIndex, samplerHeapOffset, samplerPushOffset, samplerHeapIndexStride,
    samplerHeapArrayStride }`. Address = heapOffset + push word at pushOffset * heapIndexStride + array index *
    heapArrayStride (the formula is the proposal's description; units of heapOffset and strides are bytes).
  - `HEAP_WITH_INDIRECT_INDEX` (2), `HEAP_WITH_INDIRECT_INDEX_ARRAY` (3): index read through a device address
    in push data (`addressOffset`).
  - `RESOURCE_HEAP_DATA` (4): `heapData { heapOffset, pushOffset }`, uniform buffers only, bindingCount 1.
  - `PUSH_DATA` (5): `sourceData.pushDataOffset`, uniform buffers only, bindingCount 1, multiple of 4.
  - `PUSH_ADDRESS` (6): `sourceData.pushAddressOffset`, multiple of 8, bindingCount 1, buffers and acceleration
    structures only (no images or samplers).
  - `INDIRECT_ADDRESS` (7), `HEAP_WITH_SHADER_RECORD_INDEX` (8), `SHADER_RECORD_DATA` (9), `SHADER_RECORD_ADDRESS` (10):
    ray-tracing style sources; not used.
- Units: byte offsets everywhere (the alignment VUIDs require multiples of the descriptor alignment for
  `heapOffset` and `heapArrayStride`: image, buffer or sampler alignment as appropriate; chapter, mapping section).
- Arrays: "array declarations in the shader do not affect the way multiple bindings are mapped"; an array's element
  `i` is at `heapOffset + i * heapArrayStride` (guide, "Pipelines and Shaders"; the proposal says the same for
  constant offset). **(unverified: the exact array formula for PUSH_INDEX; phase 2 relies on the proposal wording.)**
- Combined image samplers: `useCombinedImageSamplerIndex` packs the sampler index in the top 12 bits and the image
  index in the low 20 bits of one 32-bit word (proposal, section 4). Phase 2 does not use combined samplers; the
  port's layouts already use separate sampled images and samplers.
- Embedded samplers: `pEmbeddedSampler` (up to `maxDescriptorHeapEmbeddedSamplers`). Not used in phase 2.

## (d) Reserved ranges

- Each heap binding names a reserved range: `VkBindHeapInfoEXT { heapRange (VkDeviceAddressRangeEXT),
  reservedRangeOffset, reservedRangeSize }` (lines 16817-16823). The driver owns those bytes: they must be backed
  by memory, must not be accessed or changed by the application while a command buffer that binds them is pending,
  and are "bound to the command buffer until it has been reset" (chapter).
- Limits: `reservedRangeSize >= minResourceHeapReservedRange` (resp. `minSamplerHeapReservedRange`); the reserved
  range ends inside `heapRange`; `heapRange.address` is a multiple of `resourceHeapAlignment` (resp.
  `samplerHeapAlignment`), which the guide and the chapter give as 65536 in practice (VUID-...-11235 / -11226).
- The reserved offset is a multiple of `bufferDescriptorAlignment` and `imageDescriptorAlignment` (VUID -11435/-11436).
- The heap buffer must be created with `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT` and
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` (guide, "Heap Setup and Sizing").
- Phase 2 consequence: the heap buffer is allocated with `vmaCreateBufferWithAlignment` at `resourceHeapAlignment`
  (and the sampler heap at `samplerHeapAlignment`), so the bind address is aligned.

## (e) Binding and secondary command buffers

- Commands: `vkCmdBindSamplerHeapEXT(cb, const VkBindHeapInfoEXT*)` and `vkCmdBindResourceHeapEXT(cb, ...)`.
- "One descriptor heap of each type can be bound to a command buffer" (chapter, "Using Heaps").
- Heap binding is exclusive with descriptor set and descriptor buffer state: "Binding a heap invalidates those states,
  and setting those states invalidates the heaps." Recording `vkCmdBindResourceHeapEXT` / `vkCmdBindSamplerHeapEXT`
  / `vkCmdPushDataEXT` "immediately invalidates all non-heap descriptor state"; the reverse holds too (chapter,
  section 3 and 4 of the fetched text).
- Secondary command buffers: `VkCommandBufferInheritanceDescriptorHeapInfoEXT { sType, pNext, pSamplerHeapBindInfo,
  pResourceHeapBindInfo }` (lines 16973-16978). With those pointers set, the values must match the primary and the
  secondary must not call the bind commands. Without them (NULL), the secondary binds its own heaps and the primary
  must rebind after `vkCmdExecuteCommands` if a secondary lacked the info (proposal, section 6).
- Phase 2 choice: every heap draw binds both heaps itself and never sets the inheritance struct. This is correct for
  primary and secondary command buffers, at the cost of two extra calls per heap draw.

## (f) Descriptor writes and image views

- Host-side writes: `vkWriteSamplerDescriptorsEXT(device, count, const VkSamplerCreateInfo*, const VkHostAddressRangeEXT*)`
  and `vkWriteResourceDescriptorsEXT(device, count, const VkResourceDescriptorInfoEXT*, const VkHostAddressRangeEXT*)`
  (header lines 17014-17015, 17027-17040). Each `VkHostAddressRangeEXT { address, size }` names where to write;
  `size` must be at least `vkGetPhysicalDeviceDescriptorSizeEXT` for the type (chapter, "Descriptor Writes"; the
  per-type sizes in the properties struct are upper bounds).
- Writes go to host memory. The guide lists three ways: map a host-visible heap and write into it, point the write at
  the mapping, or copy. Phase 2 writes straight into the mapped heap (the buffers are host visible).
- Images: `VkResourceDescriptorInfoEXT { sType, pNext, type, data }`, `data.pImage` -> `VkImageDescriptorInfoEXT {
  pView (const VkImageViewCreateInfo*), layout }` (lines 16788-16815). No `VkImageView` object is needed, but the
  **create info** is: the view must be described by its create info at write time. Consequence: phase 2 keeps a copy
  of every image view's create info (texture_cache/image_view.cpp), keyed by the view handle.
- Samplers: `vkWriteSamplerDescriptorsEXT` takes the `VkSamplerCreateInfo`, so phase 2 keeps a copy of each sampler's
  create info (texture_cache/sampler.cpp) keyed by the sampler handle, including its custom border color struct.
- Buffers: `data.pAddressRange -> VkDeviceAddressRangeEXT { address, size }`. Null descriptors need
  `nullDescriptor` (union member NULL). Samplers have no null descriptor (chapter).
- Usage flags: SAMPLED_IMAGE needs `VK_IMAGE_USAGE_SAMPLED_BIT`, STORAGE_IMAGE needs `..._STORAGE_BIT`.
- Alignment: uniform buffer addresses are multiples of `minUniformBufferOffsetAlignment`, storage buffer addresses of
  `minStorageBufferOffsetAlignment` (chapter). **(unverified: alignment of the destination pointer; the port writes
  into 128-byte-or-larger slots.)**

## (g) Mixing with classic pipelines in one command buffer

- The spec text fetched does not forbid it. It states the exclusivity rule (section 3 above) in both directions.
  **(unverified: the rules for vkCmdBindPipeline with a NULL-layout pipeline after descriptor sets; the fetch was
  truncated at about 100 000 characters.)**
- Phase 2 choice: mixing is allowed. Every heap draw re-binds both heaps and re-pushes push data. Every classic draw
  already re-pushes or re-binds its own descriptor state. The fast-bind cache (Rasterizer::CommitFastBind) is voided
  by heap draws so a later classic draw does not skip its bind. The push-constant dedup cache is voided after each
  heap push. Descriptor-buffer modes (SOTC_DESC_BUFFER, SOTC_N5_DESCBUF) stay refused with heap mode, as in phase 1,
  because their binding state is cached by NeedsBinding and the heap bind would invalidate it.
- If (g) turns out to forbid mixing, the fallback is all-or-nothing per session: refuse heap mode at startup when any
  pipeline the session needs cannot be mapped. This is not implemented; it is noted as the first thing to check
  against the full spec text.

## Summary of design decisions for phase 2

1. Heap pipeline: NULL layout, flag in `VkPipelineCreateFlags2CreateInfo`, one mapping list per stage, no
   graphics-pipeline-library linking, no descriptor-buffer or push-descriptor layout.
2. Push block: `Shader::PushData` (56 bytes, unchanged) followed by two u32 ring base indices (offsets 56 and 60).
   Pushed with `vkCmdPushDataEXT`.
3. Resource heap: [bindless arrays (phase 1, unused in heap mode)] [per-op ring, 65536 slots] [reserved range].
   Sampler heap: [bindless array (unused)] [per-op ring, 16384 slots] [reserved range]. Slot = max(image, buffer)
   descriptor size aligned to the descriptor alignment (phase 1 `slot_size`).
4. Per binding: `heapOffset = local_slot * slot_size` (the local slot index is the binding's position in the
   per-op ring, fixed per pipeline), `pushOffset` = the ring base word, `heapIndexStride = slot_size`,
   `heapArrayStride = slot_size`. The ring base word is the absolute slot index of the op's region, so the address of
   local slot `k` is `(base + k) * slot_size`, which lies in the ring.
5. Ring reuse: the ring is divided into chunks of 64 slots. Each chunk records the scheduler tick of its last write.
   Before a chunk is written again, `Scheduler::IsFree(tick)` must hold, else `Scheduler::Wait(tick)` (which flushes
   when the tick is current). Counted as ring waits.
6. Pipelines not eligible keep the classic path. Reasons (counted per pipeline built): stage sets, descriptor buffer,
   bindless samplers or images, slot words, packet range (heap stage / shader-addressed buffer), no heap buffers,
   ring too small for the pipeline.

## Not done in phase 2 (see the report)

- Mirroring the bindless image and sampler heap arrays (BindlessImages::WriteSlot): pipelines that read them are
  refused with reason "bindless".
- Pipeline libraries (SOTC_PIPE_GPL) for heap pipelines.
- Any measurement: no game run, no build of the whole project, per instructions.
