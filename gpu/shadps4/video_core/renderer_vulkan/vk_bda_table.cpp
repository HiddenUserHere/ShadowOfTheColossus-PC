// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BDA groundwork, see vk_bda_table.h.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "video_core/renderer_vulkan/vk_bda_table.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE gate of SOTC_N5_BDA

namespace Vulkan {

namespace {

/// The pipeline-hash keyed copies are dropped above this many stages (a bound, not a tuning knob).
constexpr size_t kMaxUploadedStages = 4096;
/// sotc: SOTC_BDA_DIRECT: the table copies kept per stage key. A copy is reused when its words and its
/// stream-buffer generation match (BdaTable::Lookup), so a few copies serve the draws that alternate between
/// a few bound sets. Bounded: the oldest copy of a key is replaced. Without SOTC_BDA_DIRECT one copy per key,
/// as before.
constexpr size_t kCopiesPerStage = 8;

/// sotc: SOTC_BDA_DIRECT on: the copy depth and the direct and table counters are kept; off, the work is the
/// same as before this switch existed.
bool DirectOn() {
    static const bool on = BdaDirectMode() != 0;
    return on;
}

u32 EnvMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_BDA");
        if (!v || v[0] == '\0') {
            return 0u;
        }
        return v[0] == '2' ? 2u : (v[0] == '1' ? 1u : 0u);
    }();
    return mode;
}

struct Counters {
    std::atomic<u64> draws{};
    std::atomic<u64> slots{};
    std::atomic<u64> nulls{};
    std::atomic<u64> no_bda{};
    std::atomic<u64> misaligned{};
    std::atomic<u64> verify_fail{};
    std::atomic<u64> base_mismatch{};
    std::atomic<u64> reuse{};
    std::atomic<u64> table_bytes{};
    std::atomic<u64> desc_skip{}; // sotc: SOTC_N5_BDA_BUFFERS=1: descriptor writes dropped
    std::atomic<u64> desc_verify{}; // sotc: SOTC_N5_BDA_BUFFERS=2: writes mode 1 would drop
    std::atomic<u64> copies{};      // sotc: SOTC_BDA_DIRECT: table copies uploaded (Store)
    std::atomic<u64> copied{};      // sotc: bytes of those copies
    std::atomic<u64> changed{};     // sotc: slots that differ from the stage's previous copy, per copy
    std::atomic<u64> miss_key{};    // sotc: Lookup: no copy kept for the stage
    std::atomic<u64> miss_gen{};    // sotc: Lookup: copies kept, none in the current generation
    std::atomic<u64> miss_words{};  // sotc: Lookup: a copy in this generation, different words
};
Counters g_counters;

std::array<std::atomic<u64>, static_cast<size_t>(BdaDirect::Count)> g_direct{};

/// sotc: SOTC_N5_BDA_BUFFERS: the verify mode checks the table on every bind.
bool VerifyMode() {
    return EnvMode() == 2 || BdaN5Mode() == 2 || BdaN5BuffersMode() == 2;
}

} // namespace

u32 BdaN5Mode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_N5_BDA");
        if (!v || v[0] == '\0' || !SotcNative::On()) {
            return 0u;
        }
        return v[0] == '2' ? 2u : (v[0] == '1' ? 1u : 0u);
    }();
    return mode;
}

u32 BdaN5BuffersMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_N5_BDA_BUFFERS");
        if (!v || v[0] == '\0' || !SotcNative::On()) {
            return 0u;
        }
        return v[0] == '2' ? 2u : (v[0] == '1' ? 1u : 0u);
    }();
    return mode;
}

void BdaNoteDescriptorSkip(bool dropped) {
    (dropped ? g_counters.desc_skip : g_counters.desc_verify)
        .fetch_add(1, std::memory_order_relaxed);
}

u32 BdaDirectMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_BDA_DIRECT");
        if (!v || v[0] == '\0') {
            return 0u;
        }
        return v[0] == '2' ? 2u : (v[0] == '1' ? 1u : 0u);
    }();
    return mode;
}

void BdaNoteDirect(BdaDirect event) {
    g_direct[static_cast<size_t>(event)].fetch_add(1, std::memory_order_relaxed);
}

bool BdaBufferDeviceAddressSupported(const Instance& instance) {
    // Probed once: the device does not change during a run.
    static const bool supported = [&instance] {
        const auto chain = instance.GetPhysicalDevice()
                               .getFeatures2<vk::PhysicalDeviceFeatures2,
                                             vk::PhysicalDeviceVulkan12Features>();
        return chain.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress == VK_TRUE;
    }();
    return supported;
}

u32 BdaN5BuffersActive(const Instance& instance) {
    static std::atomic<bool> logged{false};
    const u32 mode = BdaN5BuffersMode();
    if (mode == 0 || BdaBufferDeviceAddressSupported(instance)) {
        return mode;
    }
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
                     "SOTC N5 BDA BUFFERS: fallback: bufferDeviceAddress missing, descriptors kept\n");
    }
    return 0u;
}

u32 BdaTableMode(const Instance& instance) {
    static std::atomic<bool> logged{false};
    if (!instance.IsShaderInt64Supported()) {
        if ((BdaN5Mode() != 0 || BdaN5BuffersMode() != 0) && !logged.exchange(true)) {
            std::fprintf(stderr, "SOTC N5 BDA: fallback: shaderInt64 missing, descriptors kept\n");
        }
        return 0u;
    }
    return std::max({EnvMode(), BdaN5Mode(), BdaN5BuffersActive(instance)});
}

void BdaTable::Begin(u32 num_slots, u64 null_addr) {
    null_address = null_addr;
    words.assign(static_cast<size_t>(num_slots) * 4, 0);
    for (u32 slot = 0; slot < num_slots; slot++) {
        Write(slot, null_address, 0);
    }
    g_counters.draws.fetch_add(1, std::memory_order_relaxed);
    g_counters.table_bytes.fetch_add(Bytes(), std::memory_order_relaxed);
}

void BdaTable::Write(u32 slot, u64 address, u32 size) {
    u32* w = words.data() + static_cast<size_t>(slot) * 4;
    w[0] = static_cast<u32>(address);
    w[1] = static_cast<u32>(address >> 32);
    w[2] = size;
    w[3] = 0;
}

void BdaTable::SetNull(u32 slot) {
    Write(slot, null_address, 0);
    g_counters.nulls.fetch_add(1, std::memory_order_relaxed);
}

void BdaTable::SetBlock(u32 slot, u64 device_address, u64 buffer_bytes, u64 offset, u64 size) {
    g_counters.slots.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) {
        SetNull(slot);
        return;
    }
    if (device_address == 0) {
        g_counters.no_bda.fetch_add(1, std::memory_order_relaxed);
        SetNull(slot);
        return;
    }
    // sotc: SOTC_N5_BDA_BUFFERS: the block must lie inside its buffer, since a physical-address
    // access is not covered by robustBufferAccess: a block past the buffer end is clamped to it
    // (verify: nulled). Without the switch the check stays as it was (verify modes only).
    if (offset + size > buffer_bytes && (VerifyMode() || BdaN5BuffersMode() != 0)) {
        g_counters.verify_fail.fetch_add(1, std::memory_order_relaxed);
        if (VerifyMode()) {
            SetNull(slot);
            return;
        }
        size = offset < buffer_bytes ? buffer_bytes - offset : 0;
        if (size == 0) {
            SetNull(slot);
            return;
        }
    }
    // sotc: SOTC_N5_BDA_BUFFERS: the address the shader accesses (the block start), not the
    // buffer start. Without the switch the counter stays as it was.
    const u64 access = BdaN5BuffersMode() != 0 ? device_address + offset : device_address;
    if ((access & 3) != 0) {
        g_counters.misaligned.fetch_add(1, std::memory_order_relaxed);
    }
    // The block is the whole V# range; a size above 4 GiB cannot be a slot size.
    Write(slot, device_address + offset, static_cast<u32>(size > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : size));
}

void BdaTable::CheckSlot(u32 slot, u64 expected_address) {
    const u32* w = words.data() + static_cast<size_t>(slot) * 4;
    const u64 address = static_cast<u64>(w[0]) | (static_cast<u64>(w[1]) << 32);
    if (w[2] != 0 && address != expected_address) {
        g_counters.base_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
}

bool BdaTable::Lookup(u64 key, u64 generation, u64* offset) {
    const auto it = uploaded.find(key);
    if (it == uploaded.end()) {
        g_counters.miss_key.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool same_generation = false;
    for (const Uploaded& copy : it->second.entries) {
        if (copy.generation != generation) {
            continue;
        }
        same_generation = true;
        if (copy.words == words) {
            *offset = copy.offset;
            g_counters.reuse.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    if (DirectOn()) {
        (same_generation ? g_counters.miss_words : g_counters.miss_gen)
            .fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void BdaTable::Store(u64 key, u64 generation, u64 offset) {
    if (uploaded.size() >= kMaxUploadedStages && !uploaded.count(key)) {
        uploaded.clear();
    }
    StageCopies& stage = uploaded[key];
    if (DirectOn()) {
        g_counters.copies.fetch_add(1, std::memory_order_relaxed);
        g_counters.copied.fetch_add(Bytes(), std::memory_order_relaxed);
        if (!stage.entries.empty()) {
            // The most recent copy of this key: the slots that differ from it are the ones a patch would write.
            const size_t last = (stage.next + stage.entries.size() - 1) % stage.entries.size();
            const std::vector<u32>& prev = stage.entries[last].words;
            u64 changed = 0;
            for (size_t slot = 0; slot * 4 < words.size(); ++slot) {
                for (size_t w = 0; w < 4; ++w) {
                    if (slot * 4 + w >= prev.size() || prev[slot * 4 + w] != words[slot * 4 + w]) {
                        ++changed;
                        break;
                    }
                }
            }
            g_counters.changed.fetch_add(changed, std::memory_order_relaxed);
        }
    }
    // One copy per key without SOTC_BDA_DIRECT: the same entry is replaced at each copy, as before.
    const size_t depth = DirectOn() ? kCopiesPerStage : 1;
    Uploaded copy{words, generation, offset};
    if (stage.entries.size() < depth) {
        stage.entries.push_back(std::move(copy));
    } else {
        stage.entries[stage.next] = std::move(copy);
        stage.next = (stage.next + 1) % depth;
    }
}

void BdaTableReport(char* out, u32 out_size) {
    const u64 draws = g_counters.draws.exchange(0, std::memory_order_relaxed);
    const u64 slots = g_counters.slots.exchange(0, std::memory_order_relaxed);
    const u64 nulls = g_counters.nulls.exchange(0, std::memory_order_relaxed);
    const u64 no_bda = g_counters.no_bda.exchange(0, std::memory_order_relaxed);
    const u64 misaligned = g_counters.misaligned.exchange(0, std::memory_order_relaxed);
    const u64 verify_fail = g_counters.verify_fail.exchange(0, std::memory_order_relaxed);
    const u64 base_mismatch = g_counters.base_mismatch.exchange(0, std::memory_order_relaxed);
    const u64 reuse = g_counters.reuse.exchange(0, std::memory_order_relaxed);
    const u64 bytes = g_counters.table_bytes.exchange(0, std::memory_order_relaxed);
    const u64 desc_skip = g_counters.desc_skip.exchange(0, std::memory_order_relaxed);
    const u64 desc_verify = g_counters.desc_verify.exchange(0, std::memory_order_relaxed);
    const u64 copies = g_counters.copies.exchange(0, std::memory_order_relaxed);
    const u64 copied = g_counters.copied.exchange(0, std::memory_order_relaxed);
    const u64 changed = g_counters.changed.exchange(0, std::memory_order_relaxed);
    const u64 miss_key = g_counters.miss_key.exchange(0, std::memory_order_relaxed);
    const u64 miss_gen = g_counters.miss_gen.exchange(0, std::memory_order_relaxed);
    const u64 miss_words = g_counters.miss_words.exchange(0, std::memory_order_relaxed);
    std::array<u64, static_cast<size_t>(BdaDirect::Count)> direct{};
    for (size_t i = 0; i < direct.size(); ++i) {
        direct[i] = g_direct[i].exchange(0, std::memory_order_relaxed);
    }
    const auto d = [&direct](BdaDirect e) {
        return static_cast<unsigned long long>(direct[static_cast<size_t>(e)]);
    };
    char line[256];
    std::string report;
    // Without SOTC_BDA_DIRECT the report is the one line it always was (no trailing newline).
    const char* first_end = DirectOn() ? "\n" : "";
    std::snprintf(line, sizeof line,
                  "SOTC BDA draws %llu slots %llu nulls %llu no_bda %llu misaligned %llu "
                  "verify_fail %llu base_mismatch %llu reuse %llu table %llu B "
                  "desc_skip %llu desc_verify %llu%s",
                  static_cast<unsigned long long>(draws), static_cast<unsigned long long>(slots),
                  static_cast<unsigned long long>(nulls), static_cast<unsigned long long>(no_bda),
                  static_cast<unsigned long long>(misaligned),
                  static_cast<unsigned long long>(verify_fail),
                  static_cast<unsigned long long>(base_mismatch),
                  static_cast<unsigned long long>(reuse),
                  static_cast<unsigned long long>(bytes),
                  static_cast<unsigned long long>(desc_skip),
                  static_cast<unsigned long long>(desc_verify), first_end);
    report += line;
    if (DirectOn()) {
        std::snprintf(line, sizeof line,
                      "SOTC BDA DIRECT bind %llu verified %llu mismatch %llu fb arena %llu straddle %llu "
                      "notserved %llu noaddr %llu maintain %llu nobuf %llu gate %llu\n",
                      d(BdaDirect::Bind), d(BdaDirect::Verified), d(BdaDirect::Mismatch),
                      d(BdaDirect::FbArena), d(BdaDirect::FbStraddle), d(BdaDirect::FbNotServed),
                      d(BdaDirect::FbNoAddress), d(BdaDirect::FbMaintain), d(BdaDirect::FbNoBuffer),
                      d(BdaDirect::FbGate));
        report += line;
        std::snprintf(line, sizeof line,
                      "SOTC BDA TABLE copies %llu copied %llu B changed %llu miss key %llu gen %llu "
                      "words %llu\n",
                      static_cast<unsigned long long>(copies), static_cast<unsigned long long>(copied),
                      static_cast<unsigned long long>(changed), static_cast<unsigned long long>(miss_key),
                      static_cast<unsigned long long>(miss_gen),
                      static_cast<unsigned long long>(miss_words));
        report += line;
    }
    std::snprintf(out, out_size, "%s", report.c_str());
}

} // namespace Vulkan
