// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bindless.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace VideoCore {

u32 BindlessImagesMode() {
    // sotc: SOTC_N5_BINDLESS needs SOTC_NATIVE (1 or 2), like every N-stage switch.
    static const u32 mode = [] {
        const char* native = std::getenv("SOTC_NATIVE");
        if (!native || (native[0] != '1' && native[0] != '2')) {
            return 0u;
        }
        const char* v = std::getenv("SOTC_N5_BINDLESS");
        if (!v || (v[0] != '1' && v[0] != '2')) {
            return 0u;
        }
        return v[0] == '2' ? 2u : 1u;
    }();
    return mode;
}

vk::DescriptorType BindlessImageDescriptorType(u32 key) {
    return key < Shader::NUM_BINDLESS_SAMPLED_IMAGE_KEYS ? vk::DescriptorType::eSampledImage
                                                         : vk::DescriptorType::eStorageImage;
}

std::array<vk::DescriptorSetLayoutBinding, Shader::NUM_BINDLESS_IMAGE_KEYS>
BindlessImageLayoutBindings() {
    std::array<vk::DescriptorSetLayoutBinding, Shader::NUM_BINDLESS_IMAGE_KEYS> bindings{};
    for (u32 key = 0; key < Shader::NUM_BINDLESS_IMAGE_KEYS; ++key) {
        bindings[key] = vk::DescriptorSetLayoutBinding{
            .binding = BindlessImageFirstBinding + key,
            .descriptorType = BindlessImageDescriptorType(key),
            .descriptorCount = BindlessImageCapacity,
            .stageFlags = vk::ShaderStageFlagBits::eAll,
        };
    }
    return bindings;
}

static std::atomic<BindlessImages*> g_active_images{nullptr};

BindlessImages* BindlessImages::Active() noexcept {
    return g_active_images.load(std::memory_order_acquire);
}

BindlessImages::BindlessImages(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                               vk::DescriptorSet set_, u32 mode_)
    : instance{instance_}, scheduler{scheduler_}, set{set_}, mode{mode_} {
    for (u32 key = 0; key < Shader::NUM_BINDLESS_IMAGE_KEYS; ++key) {
        slots[key].assign(BindlessImageCapacity, Slot{});
        next_slot[key] = 1;
        // Slot 0 holds no view: a bindless read of an unbound image returns zero (nullDescriptor).
        WriteSlot(key, 0, vk::ImageView{}, vk::ImageLayout::eUndefined);
    }
    g_active_images.store(this, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "N5 bindless images enabled (mode {}): {} sampled + {} storage arrays x {} views",
             mode, Shader::NUM_BINDLESS_SAMPLED_IMAGE_KEYS,
             Shader::NUM_BINDLESS_IMAGE_KEYS - Shader::NUM_BINDLESS_SAMPLED_IMAGE_KEYS,
             BindlessImageCapacity);
}

BindlessImages::~BindlessImages() {
    // Views still alive after this point release nothing (Active is null).
    BindlessImages* self = this;
    g_active_images.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

u32 BindlessImages::Acquire(u32 key, vk::ImageView view, vk::ImageLayout layout) {
    std::scoped_lock lk{mutex};
    ++acquires;
    Reclaim();
    u32 slot = Shader::NO_BINDLESS_IMAGE_SLOT;
    if (!free_slots[key].empty()) {
        slot = free_slots[key].back();
        free_slots[key].pop_back();
    } else if (next_slot[key] < BindlessImageCapacity) {
        slot = next_slot[key]++;
    } else {
        slot = Evict(key);
    }
    if (slot == Shader::NO_BINDLESS_IMAGE_SLOT) {
        ++full;
        if (!logged_full) {
            logged_full = true;
            LOG_WARNING(Render_Vulkan, "N5 bindless image heap full (key {}): no slot the GPU is done with",
                        key);
        }
        return 0;
    }
    WriteSlot(key, slot, view, layout);
    Slot& taken = slots[key][slot];
    taken.state = SlotState::Live;
    taken.last_use = scheduler.CurrentTick();
    return slot;
}

u32 BindlessImages::Evict(u32 key) {
    // The least recently used live slot. The GPU may still read it through the draws that used it
    // last, so it is taken only when that tick has finished; the minimum is the only candidate that
    // can be finished before the others.
    u32 victim = Shader::NO_BINDLESS_IMAGE_SLOT;
    u64 oldest = ~0ULL;
    for (u32 slot = 1; slot < next_slot[key]; ++slot) {
        const Slot& s = slots[key][slot];
        if (s.state == SlotState::Live && s.last_use < oldest) {
            oldest = s.last_use;
            victim = slot;
        }
    }
    if (victim == Shader::NO_BINDLESS_IMAGE_SLOT || !scheduler.IsFree(oldest)) {
        return Shader::NO_BINDLESS_IMAGE_SLOT;
    }
    // The owner of the victim finds it no longer held (Holds) and takes a new one at its next bind.
    slots[key][victim].state = SlotState::Free;
    generation.fetch_add(1, std::memory_order_acq_rel);
    ++evictions;
    return victim;
}

bool BindlessImages::Holds(u32 key, u32 slot, vk::ImageView view, vk::ImageLayout layout) {
    if (slot == 0 || slot >= BindlessImageCapacity) {
        return false;
    }
    std::scoped_lock lk{mutex};
    const Slot& s = slots[key][slot];
    return s.state == SlotState::Live && s.view == view && s.layout == layout;
}

void BindlessImages::Use(u32 key, u32 slot) {
    if (slot == 0 || slot >= BindlessImageCapacity) {
        return;
    }
    std::scoped_lock lk{mutex};
    Slot& s = slots[key][slot];
    if (s.state == SlotState::Live) {
        s.last_use = scheduler.CurrentTick();
        ++served;
    }
}

void BindlessImages::Release(u32 key, u32 slot, vk::ImageView view) {
    if (slot == 0 || slot >= BindlessImageCapacity) {
        return;
    }
    std::scoped_lock lk{mutex};
    Slot& s = slots[key][slot];
    if (s.state != SlotState::Live || s.view != view) {
        // Evicted since: the slot belongs to another view now (or is free), nothing to give back.
        return;
    }
    ++releases;
    // The GPU may still read the slot through submitted draws: it is cleared and reused only once
    // the tick current now has finished (Reclaim). Until then it is not held by anyone.
    s.state = SlotState::Pending;
    generation.fetch_add(1, std::memory_order_acq_rel);
    pending.push_back(ReleasedSlot{key, slot, scheduler.CurrentTick()});
}

void BindlessImages::Reclaim() {
    while (!pending.empty() && scheduler.IsFree(pending.front().tick)) {
        const ReleasedSlot done = pending.front();
        pending.pop_front();
        WriteSlot(done.key, done.slot, vk::ImageView{}, vk::ImageLayout::eUndefined);
        slots[done.key][done.slot].state = SlotState::Free;
        free_slots[done.key].push_back(done.slot);
        ++reclaims;
    }
}

void BindlessImages::WriteSlot(u32 key, u32 slot, vk::ImageView view, vk::ImageLayout layout) {
    const vk::DescriptorImageInfo info{{}, view, view ? layout : vk::ImageLayout::eUndefined};
    const vk::WriteDescriptorSet write{
        .dstSet = set,
        .dstBinding = BindlessImageFirstBinding + key,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = BindlessImageDescriptorType(key),
        .pImageInfo = &info,
    };
    instance.GetDevice().updateDescriptorSets(write, {});
    slots[key][slot].view = view;
    slots[key][slot].layout = layout;
    ++writes;
}

bool BindlessImages::Verify(u32 key, u32 slot, vk::ImageView expected) {
    std::scoped_lock lk{mutex};
    ++verifies;
    const bool same = slot < BindlessImageCapacity && slots[key][slot].view == expected;
    if (!same) {
        ++mismatches;
    }
    return same;
}

u32 BindlessImages::Report(char* out, u32 size) {
    std::scoped_lock lk{mutex};
    const int n = std::snprintf(
        out, size,
        "SOTC N5 BINDLESS acquire %llu writes %llu releases %llu reclaims %llu evict %llu full %llu "
        "verify %llu mismatch %llu pending %zu served %llu",
        static_cast<unsigned long long>(acquires), static_cast<unsigned long long>(writes),
        static_cast<unsigned long long>(releases), static_cast<unsigned long long>(reclaims),
        static_cast<unsigned long long>(evictions), static_cast<unsigned long long>(full),
        static_cast<unsigned long long>(verifies), static_cast<unsigned long long>(mismatches),
        pending.size(), static_cast<unsigned long long>(served));
    acquires = writes = releases = reclaims = evictions = full = verifies = mismatches = served = 0;
    return n < 0 ? 0 : static_cast<u32>(n);
}

} // namespace VideoCore
