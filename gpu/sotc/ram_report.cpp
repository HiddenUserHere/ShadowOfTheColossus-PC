// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_RAM_REPORT=1 (off by default). RAM breakdown, printed with the 5 s GPU report.
//
// What the numbers mean (all MiB unless noted):
//  - process working set: pages of this process in RAM, the figure Task Manager shows as
//    "Memory". Includes touched guest pages, the image, Vulkan driver pages that are resident.
//  - process private commit: PrivateUsage, private (MEM_PRIVATE) committed memory only.
//  - commit charge: PagefileUsage, private commit plus pagefile-backed sections (the guest pool
//    is one, committed at its full size when created, so the commit charge includes it).
//  - guest pool committed: runtime_memory_committed_bytes(), the guest memory the game may touch
//    (commit charge; the whole 8 GiB pool without SOTC_COMMIT_CHUNKS). Expected large.
//  - guest pool resident: estimate from QueryWorkingSetEx on one page per 64 pages of the runtime's
//    backing view (the same section as the guest view), scaled to the view size. Expected: the
//    pages the game actually touched; well below the commit.
//  - sections: MEM_MAPPED committed (the guest view and the fault ring), MEM_IMAGE committed
//    (bb-probe.exe and the system DLLs, an upper bound of their resident part), and MEM_PRIVATE
//    committed (the whole private commit, split by owner in the next lines).
//  - private split (VirtualQuery sweep, committed MEM_PRIVATE by owner):
//      loader image: the committed part of the eboot reservation (below 2 GiB: eboot, PRX
//        modules, trampolines, the low command block). Reservation and committed bytes come from
//        src/probe.c (sotc_eboot_range).
//      thread stacks: committed private memory inside the TEB stack range of each live guest host
//        thread (src/runtime_thread.c, noted by host_start). Their reserved size is printed too.
//      process heaps: HeapSummary over GetProcessHeaps (the ucrt malloc heap and any other heap),
//        committed and allocated bytes (src/compat_win.c, sotc_heap_summary).
//      mimalloc: mi_process_info current_commit, bb-probe built with BB_MIMALLOC (else 0).
//      remainder: private minus the four lines above. Vulkan driver host memory, the shader
//        compiler, other allocators. Expected to be the bulk of what is still unexplained.
//  - host structures: byte counters kept by the owners (no sweep):
//      staging held: ring blocks and large buffers per memory type (VkBuffer memory, so it sits in
//        the VMA lines, not in private).
//      buffer cache: MemoryTracker region manager pool and top-tier table.
//      texture cache: image and view slots (object sizes) and the page table second-level pages.
//      pipelines: counts of programs, graphics and compute pipelines, and the size of the last
//        driver pipeline cache blob (SPIR-V is not kept; the driver holds the module handles).
//      fault ring: BB_FAULT_RING mapping, when open.
//  - Vulkan blocks (VMA, commit): VkDeviceMemory blocks owned by VMA, by class. Device-local is
//    VRAM (or the BAR type on a discrete GPU); host-visible is system RAM used for staging and
//    readback; host-cached is the cached subset. Expected: device-local large on a discrete GPU.
//  - Vulkan heap OS usage and budget: VK_EXT_memory_budget, per heap, all Vulkan memory of this
//    process (VMA and raw). "Outside VMA" = OS usage minus VMA blocks: the raw vkAllocateMemory
//    users (staging rings, host import, driver). It is 0 when VK_EXT_memory_budget is off.
//  - host import: SOTC_HOST_IMPORT budget only (SOTC_HOST_IMPORT_MB, default 512). The live size
//    is not exposed to this file; the budget is the upper bound.
//  - other: working set minus guest resident minus image commit. Rough: image commit is an upper
//    bound, and Vulkan memory that is resident sits in it.
//
// Not counted: SPIR-V (transient in the pipeline cache) and driver allocations outside the sweep
// (they are in the private remainder, or in the Vulkan outside-VMA line).
#include "ram_report.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/texture_cache.h"
#include <vk_mem_alloc.h> // VMA declarations (vk_common.h only sets its macros)

extern "C" {
uint64_t runtime_memory_committed_bytes(void); // src/runtime_memory.c (Windows)
const unsigned char* runtime_memory_backing_view(uint64_t* size); // src/runtime_memory.c
void sotc_eboot_range(uintptr_t* base, uint64_t* reserved, uint64_t* committed); // src/probe.c
uint64_t sotc_mimalloc_committed_bytes(void); // src/probe.c (0 without BB_MIMALLOC)
uint64_t sotc_fault_ring_bytes(void); // src/probe.c
void sotc_heap_summary(uint64_t* allocated, uint64_t* committed); // src/compat_win.c
uint64_t runtime_thread_stack_bytes(void); // src/runtime_thread.c
size_t runtime_thread_stack_ranges(uintptr_t (*out)[2], size_t capacity); // src/runtime_thread.c
}

namespace SotcRamReport {
namespace {

constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kPage = 4096;
constexpr uint64_t kSampleStride = 64 * kPage; // one sampled page per 64 pages (256 KiB)
constexpr uint64_t kQueryBatch = 4096;
constexpr size_t kMaxStacks = 256; // runtime_thread.c keeps this many stack slots

double Mib(uint64_t bytes) {
    return static_cast<double>(bytes) / static_cast<double>(kMiB);
}

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_RAM_REPORT");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    return on;
}

uint64_t Overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    const uint64_t lo = std::max(a0, b0);
    const uint64_t hi = std::min(a1, b1);
    return hi > lo ? hi - lo : 0;
}

// Committed bytes by region type, from a walk of the whole user address space.
struct Sweep {
    uint64_t private_commit{};
    uint64_t mapped_commit{};
    uint64_t image_commit{};
    uint64_t private_loader{}; // committed private memory inside the eboot reservation
    uint64_t private_stacks{}; // committed private memory inside a live guest thread stack
};

// stacks: stack_count (low, high) pairs of the live guest thread stacks.
Sweep SweepAddressSpace(uintptr_t eboot_base, uintptr_t eboot_end, const uintptr_t (*stacks)[2],
                        size_t stack_count) {
    Sweep s;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uintptr_t at = 0;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi)) {
            break;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) {
            break;
        }
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Type == MEM_PRIVATE) {
                const uint64_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                s.private_commit += mbi.RegionSize;
                s.private_loader += Overlap(base, next, eboot_base, eboot_end);
                for (size_t i = 0; i < stack_count; ++i) {
                    s.private_stacks += Overlap(base, next, stacks[i][0], stacks[i][1]);
                }
            } else if (mbi.Type == MEM_MAPPED) {
                s.mapped_commit += mbi.RegionSize;
            } else if (mbi.Type == MEM_IMAGE) {
                s.image_commit += mbi.RegionSize;
            }
        }
        at = next;
    }
    return s;
}

// Resident estimate of the guest pool: sample one page per kSampleStride of the backing view.
// The backing view is the same section as the guest view, so its resident pages are the guest's.
struct GuestResident {
    uint64_t view_bytes{};
    uint64_t resident_est{};
    bool ok{};
};

GuestResident SampleGuestResident() {
    GuestResident r;
    uint64_t size = 0;
    const unsigned char* view = runtime_memory_backing_view(&size);
    if (!view || size < kSampleStride) {
        return r;
    }
    const uint64_t samples = size / kSampleStride;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> batch(
        static_cast<size_t>(std::min<uint64_t>(kQueryBatch, samples)));
    uint64_t valid = 0;
    for (uint64_t first = 0; first < samples; first += kQueryBatch) {
        const uint64_t n = std::min<uint64_t>(kQueryBatch, samples - first);
        for (uint64_t i = 0; i < n; ++i) {
            batch[i].VirtualAddress =
                const_cast<unsigned char*>(view + (first + i) * kSampleStride);
        }
        if (!QueryWorkingSetEx(GetCurrentProcess(), batch.data(),
                               static_cast<DWORD>(n * sizeof batch[0]))) {
            return r;
        }
        for (uint64_t i = 0; i < n; ++i) {
            if (batch[i].VirtualAttributes.Flags & 1) { // bit 0: Valid (page in the working set)
                ++valid;
            }
        }
    }
    r.view_bytes = size;
    r.resident_est = valid * kSampleStride;
    r.ok = true;
    return r;
}

void ReportVulkan(std::string& out, const Vulkan::Instance& instance) {
    const VmaAllocator allocator = instance.GetAllocator();
    const vk::PhysicalDeviceMemoryProperties& props = instance.GetMemoryProperties();

    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);

    uint64_t device_local = 0, bar = 0, host = 0, host_cached = 0;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const auto flags = static_cast<VkMemoryPropertyFlags>(props.memoryTypes[i].propertyFlags);
        const uint64_t bytes = stats.memoryType[i].statistics.blockBytes;
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                bar += bytes;
            } else {
                host += bytes;
            }
            if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
                host_cached += bytes;
            }
        } else if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            device_local += bytes;
        }
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC RAM vulkan (VMA blocks, commit): device-local %.1f, BAR (device-local "
                  "host-visible) %.1f, host-visible system %.1f (host-cached %.1f), all %.1f MiB\n",
                  Mib(device_local), Mib(bar), Mib(host), Mib(host_cached),
                  Mib(device_local + bar + host));
    out += line;

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    for (uint32_t h = 0; h < props.memoryHeapCount; ++h) {
        const bool dl = static_cast<VkMemoryHeapFlags>(props.memoryHeaps[h].flags) &
                        VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        const uint64_t usage = budgets[h].usage;
        const uint64_t vma = budgets[h].statistics.blockBytes;
        const uint64_t outside = usage > vma ? usage - vma : 0;
        std::snprintf(line, sizeof line,
                      "SOTC RAM vulkan heap %u (%s): OS usage %.1f / budget %.1f MiB, VMA blocks "
                      "%.1f, outside VMA %.1f MiB\n",
                      h, dl ? "device-local" : "system", Mib(usage), Mib(budgets[h].budget),
                      Mib(vma), Mib(outside));
        out += line;
    }
    uint64_t outside_vma = 0;
    for (uint32_t h = 0; h < props.memoryHeapCount; ++h) {
        const uint64_t usage = budgets[h].usage;
        const uint64_t vma = budgets[h].statistics.blockBytes;
        outside_vma += usage > vma ? usage - vma : 0;
    }
    std::snprintf(line, sizeof line,
                  "SOTC RAM vulkan outside VMA (raw vkAllocateMemory: staging rings, host import, "
                  "driver): %.1f MiB (0 when VK_EXT_memory_budget is off)\n",
                  Mib(outside_vma));
    out += line;
}

// Host structures the owners count themselves: staging, buffer cache, texture cache, pipelines.
void ReportHostStructures(std::string& out, Vulkan::Presenter& presenter) {
    char line[640];
    Vulkan::Runtime& runtime = presenter.SotcRuntime();
    Vulkan::StagingBufferPool& staging = runtime.GetStagingPool();
    std::snprintf(line, sizeof line,
                  "SOTC RAM host staging held (VkBuffer memory, inside the VMA lines): "
                  "DeviceLocal %.1f, HostUncached %.1f, HostCached %.1f MiB\n",
                  Mib(staging.HeldBytes(VideoCore::MemoryType::DeviceLocal)),
                  Mib(staging.HeldBytes(VideoCore::MemoryType::HostUncached)),
                  Mib(staging.HeldBytes(VideoCore::MemoryType::HostCached)));
    out += line;

    Vulkan::Rasterizer* rasterizer = presenter.SotcRasterizer();
    if (!rasterizer) {
        out += "SOTC RAM host caches: rasterizer not created yet\n";
        return;
    }
    const uint64_t buffer_bytes = rasterizer->GetBufferCache().SotcHostBytes();
    const uint64_t texture_bytes = rasterizer->GetTextureCache().SotcHostBytes();
    std::snprintf(line, sizeof line,
                  "SOTC RAM host caches: buffer cache region managers %.2f MiB, texture cache "
                  "slots and page table %.2f MiB\n",
                  Mib(buffer_bytes), Mib(texture_bytes));
    out += line;

    uint64_t programs = 0, graphics = 0, compute = 0, driver_blob = 0;
    rasterizer->GetPipelineCache().SotcCounts(programs, graphics, compute, driver_blob);
    std::snprintf(line, sizeof line,
                  "SOTC RAM host pipelines: %llu programs, %llu graphics, %llu compute pipelines; "
                  "driver pipeline cache blob %.2f MiB (last saved)\n",
                  static_cast<unsigned long long>(programs),
                  static_cast<unsigned long long>(graphics),
                  static_cast<unsigned long long>(compute), Mib(driver_blob));
    out += line;
}

void ReportVulkanHostImport(std::string& out) {
    const char* on = std::getenv("SOTC_HOST_IMPORT");
    if (!on || !on[0] || std::strcmp(on, "0") == 0) {
        return;
    }
    // Same parsing as HostImport (SOTC_HOST_IMPORT_MB, clamped 64..8192, default 512).
    const char* mb_env = std::getenv("SOTC_HOST_IMPORT_MB");
    const uint64_t mb = mb_env ? std::clamp<uint64_t>(std::strtoull(mb_env, nullptr, 10), 64, 8192)
                               : 512;
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC RAM host import: up to %llu MiB of guest memory locked (budget, not live; "
                  "raw Vulkan memory, not in VMA)\n",
                  static_cast<unsigned long long>(mb));
    out += line;
}

} // namespace

void Report(std::string& out, Vulkan::Presenter* presenter) {
    if (!Enabled()) {
        return;
    }
    char line[640];

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    uint64_t working_set = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                sizeof pmc)) {
        working_set = pmc.WorkingSetSize;
        std::snprintf(line, sizeof line,
                      "SOTC RAM process: working set %.1f MiB (peak %.1f), private commit %.1f MiB, "
                      "commit charge %.1f MiB\n",
                      Mib(pmc.WorkingSetSize), Mib(pmc.PeakWorkingSetSize), Mib(pmc.PrivateUsage),
                      Mib(pmc.PagefileUsage));
        out += line;
    }

    const uint64_t guest_committed = runtime_memory_committed_bytes();
    const GuestResident guest = SampleGuestResident();
    if (guest.ok) {
        std::snprintf(line, sizeof line,
                      "SOTC RAM guest pool: committed %.1f MiB of the %.1f GiB view, resident "
                      "~%.1f MiB (sampled 1 page in 64)\n",
                      Mib(guest_committed), Mib(guest.view_bytes) / 1024.0, Mib(guest.resident_est));
    } else {
        std::snprintf(line, sizeof line,
                      "SOTC RAM guest pool: committed %.1f MiB, resident n/a (no backing view)\n",
                      Mib(guest_committed));
    }
    out += line;

    // Loader image and guest thread stacks: the inputs of the private split.
    uintptr_t eboot_base = 0;
    uint64_t eboot_reserved = 0, eboot_committed = 0;
    sotc_eboot_range(&eboot_base, &eboot_reserved, &eboot_committed);
    uintptr_t stacks[kMaxStacks][2];
    const size_t stack_count = runtime_thread_stack_ranges(stacks, kMaxStacks);
    const uint64_t stack_reserved = runtime_thread_stack_bytes();

    const Sweep sweep =
        SweepAddressSpace(eboot_base, eboot_base + eboot_reserved, stacks, stack_count);
    std::snprintf(line, sizeof line,
                  "SOTC RAM sections (VirtualQuery sweep, committed): mapped %.1f MiB (guest view, "
                  "fault ring), exe/DLL images %.1f MiB, private %.1f MiB\n",
                  Mib(sweep.mapped_commit), Mib(sweep.image_commit), Mib(sweep.private_commit));
    out += line;

    uint64_t heap_allocated = 0, heap_committed = 0;
    sotc_heap_summary(&heap_allocated, &heap_committed);
    const uint64_t mimalloc_committed = sotc_mimalloc_committed_bytes();
    const double private_remainder = Mib(sweep.private_commit) - Mib(sweep.private_loader) -
                                     Mib(sweep.private_stacks) - Mib(heap_committed) -
                                     Mib(mimalloc_committed);
    std::snprintf(line, sizeof line,
                  "SOTC RAM private loader image: %.1f MiB committed inside the %.0f MiB eboot "
                  "reservation (eboot, PRX modules, trampolines, low block; image %.1f MiB)\n",
                  Mib(sweep.private_loader), Mib(eboot_reserved), Mib(eboot_committed));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC RAM private thread stacks: %.1f MiB committed in %zu live guest host "
                  "threads (%.1f MiB reserved by their TEB limits)\n",
                  Mib(sweep.private_stacks), stack_count, Mib(stack_reserved));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC RAM private process heaps: %.1f MiB committed, %.1f MiB allocated (ucrt "
                  "malloc and the other heaps of the process, HeapSummary)\n",
                  Mib(heap_committed), Mib(heap_allocated));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC RAM private mimalloc: %.1f MiB committed (BB_MIMALLOC builds; 0 otherwise)\n",
                  Mib(mimalloc_committed));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC RAM private remainder: %.1f MiB = private - the four lines above (driver "
                  "and Vulkan host memory, shader compiler, untracked allocations)\n",
                  private_remainder);
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC RAM loader: BB_FAULT_RING mapping %.2f MiB (mapped, not private)\n",
                  Mib(sotc_fault_ring_bytes()));
    out += line;

    if (presenter) {
        ReportHostStructures(out, *presenter);
        ReportVulkan(out, presenter->GetInstance());
    }
    ReportVulkanHostImport(out);

    // other: what the working set has left once the guest pool's resident pages and the images
    // are taken out. Signed: the image commit is an upper bound, so this can come out low.
    const double other = Mib(working_set) - (guest.ok ? Mib(guest.resident_est) : 0.0) -
                         Mib(sweep.image_commit);
    std::snprintf(line, sizeof line,
                  "SOTC RAM other: %.1f MiB = working set - guest resident - image commit "
                  "(resident Vulkan and driver memory end up here)\n",
                  other);
    out += line;
    out += "SOTC RAM not counted: SPIR-V (transient in the pipeline cache); driver memory outside "
           "the sweep and outside VMA (see the remainder and outside-VMA lines)\n";
}

} // namespace SotcRamReport
