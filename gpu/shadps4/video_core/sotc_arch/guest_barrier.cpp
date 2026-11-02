// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_BARRIER_GUEST. See guest_barrier.h for the rule and the modes.
//
// Threading: every function here runs on the draw thread (Runtime::AccessBuffer, the rasterizer's barrier decisions and
// the guest sync markers), like the runtime's barrier state. The counters are atomics for the report only.
#include <atomic>
#include <cstdio>
#include <vector>

#include "video_core/sotc_arch/guest_barrier.h"

namespace SotcArch::GuestBarrier {

namespace {

constexpr u32 kGranuleShift = 16;          // 64 KiB granules of guest address space
constexpr u64 kBits = u64{1} << 20;        // hashed bits per map
constexpr u64 kWords = kBits / 64;

// A hashed bitmap of granules with a dirty list, so Clear touches only the words that were set.
struct Map {
    std::vector<u64> words = std::vector<u64>(kWords);
    std::vector<u32> dirty;
    bool all{}; // a range covered the whole hash space: every query hits until the next Clear

    bool Empty() const noexcept {
        return dirty.empty() && !all;
    }

    void Set(VAddr addr, u64 size) {
        if (size == 0 || all) {
            return;
        }
        const u64 g0 = addr >> kGranuleShift;
        const u64 g1 = (addr + size - 1) >> kGranuleShift;
        if (g1 - g0 + 1 >= kBits) {
            all = true;
            return;
        }
        for (u64 g = g0; g <= g1; ++g) {
            const u64 bit = g & (kBits - 1);
            u64& word = words[bit >> 6];
            if (word == 0) {
                dirty.push_back(static_cast<u32>(bit >> 6));
            }
            word |= u64{1} << (bit & 63);
        }
    }

    bool Hit(VAddr addr, u64 size) const {
        if (size == 0 || Empty()) {
            return false;
        }
        if (all) {
            return true;
        }
        const u64 g0 = addr >> kGranuleShift;
        const u64 g1 = (addr + size - 1) >> kGranuleShift;
        if (g1 - g0 + 1 >= kBits) {
            return true;
        }
        for (u64 g = g0; g <= g1; ++g) {
            const u64 bit = g & (kBits - 1);
            if ((words[bit >> 6] >> (bit & 63)) & 1) {
                return true;
            }
        }
        return false;
    }

    void Clear() {
        for (const u32 w : dirty) {
            words[w] = 0;
        }
        dirty.clear();
        all = false;
    }
};

Map& WriteMap() {
    static Map map;
    return map;
}

Map& ReadMap() {
    static Map map;
    return map;
}

// Pending since the last barrier of the rule (in verify: of the simulated rule).
bool write_pending{};

std::atomic<u64> c_ops{};
std::atomic<u64> c_image{};   // decisions the image side alone asked for
std::atomic<u64> c_net{};     // decisions the safety net alone asked for (no image need)
std::atomic<u64> c_old{};     // =2: today's decisions (flushes)
std::atomic<u64> c_new{};     // =2: barriers of the new rule (decisions and simulated guest barriers)
std::atomic<u64> c_guest{};   // guest sync points with a pending write (mode 1: recorded; =2: simulated)
std::atomic<u64> c_missed{};  // =2: today's buffer hazard, a write pending and the net did not hit (must stay 0)
std::atomic<u64> c_extra{};   // =2: the net asks for a barrier today's rule does not
std::atomic<u64> c_recorded{}; // mode 1: memory barriers recorded (FlushBarriers)
std::atomic<u64> c_sync[static_cast<u32>(Sync::Count)]{};

const char* const kSyncNames[] = {"eop", "eos", "event", "acquire", "release", "dma"};

// Verify: the simulated rule's barrier. Everything pending is ordered.
void SimulatedBarrier() {
    WriteMap().Clear();
    ReadMap().Clear();
    write_pending = false;
}

} // namespace

int Mode() {
    static const int mode = SwitchMode("SOTC_BARRIER_GUEST");
    return mode;
}

bool On() {
    return Mode() == 1;
}

bool Verifying() {
    return Mode() == 2;
}

bool Active() {
    return Mode() == 1 || Mode() == 2;
}

void NoteAccess(VAddr addr, u64 size, bool write) {
    ReadMap().Set(addr, size);
    if (write) {
        WriteMap().Set(addr, size);
        write_pending = true;
    }
}

void NoteWritePending() {
    write_pending = true;
}

bool WritePending() {
    return write_pending;
}

bool SyncPoint(Sync kind) {
    c_sync[static_cast<u32>(kind)].fetch_add(1, std::memory_order_relaxed);
    if (!write_pending) {
        return false;
    }
    c_guest.fetch_add(1, std::memory_order_relaxed);
    if (On()) {
        return true;
    }
    if (Verifying()) {
        c_new.fetch_add(1, std::memory_order_relaxed);
        SimulatedBarrier();
    }
    return false;
}

bool NetHit(std::span<const std::pair<VAddr, u64>> reads, std::span<const std::pair<VAddr, u64>> writes,
            bool unknown_reads) {
    if (unknown_reads && write_pending) {
        return true;
    }
    for (const auto& [addr, size] : reads) {
        if (WriteMap().Hit(addr, size)) {
            return true;
        }
    }
    for (const auto& [addr, size] : writes) {
        if (WriteMap().Hit(addr, size) || ReadMap().Hit(addr, size)) {
            return true;
        }
    }
    return false;
}

bool Decision(bool image, bool net, bool buffer_old, bool old_due) {
    c_ops.fetch_add(1, std::memory_order_relaxed);
    if (image) {
        c_image.fetch_add(1, std::memory_order_relaxed);
    }
    if (net && !image) {
        c_net.fetch_add(1, std::memory_order_relaxed);
    }
    const bool due = image || net;
    if (Verifying()) {
        if (old_due) {
            c_old.fetch_add(1, std::memory_order_relaxed);
        }
        if (buffer_old && !net && write_pending) {
            c_missed.fetch_add(1, std::memory_order_relaxed);
        }
        if (net && !image && !buffer_old) {
            c_extra.fetch_add(1, std::memory_order_relaxed);
        }
        if (due) {
            c_new.fetch_add(1, std::memory_order_relaxed);
            SimulatedBarrier();
        }
    }
    return due;
}

void Recorded() {
    if (!On()) {
        return;
    }
    WriteMap().Clear();
    ReadMap().Clear();
    write_pending = false;
    c_recorded.fetch_add(1, std::memory_order_relaxed);
}

void Report(char* out, u32 size, double seconds) {
    out[0] = '\0';
    if (!Active()) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const u64 ops = take(c_ops);
    const u64 image = take(c_image);
    const u64 net = take(c_net);
    const u64 old_n = take(c_old);
    const u64 new_n = take(c_new);
    const u64 guest = take(c_guest);
    const u64 missed = take(c_missed);
    const u64 extra = take(c_extra);
    const u64 recorded = take(c_recorded);
    u64 sync[static_cast<u32>(Sync::Count)];
    for (u32 k = 0; k < static_cast<u32>(Sync::Count); ++k) {
        sync[k] = take(c_sync[k]);
    }
    (void)seconds; // the counters cover the report interval (the driver's 5 s)

    int n = 0;
    if (On()) {
        n = std::snprintf(out, size,
                          "SOTC BARRIER_GUEST: mode 1, ops %llu, guest %llu, net %llu, image %llu, recorded %llu, "
                          "guest sync points by type",
                          static_cast<unsigned long long>(ops), static_cast<unsigned long long>(guest),
                          static_cast<unsigned long long>(net), static_cast<unsigned long long>(image),
                          static_cast<unsigned long long>(recorded));
    } else {
        n = std::snprintf(out, size,
                          "SOTC BARRIER_GUEST: ops %llu, old %llu, guest %llu, net %llu, missed %llu, extra %llu, "
                          "new %llu, guest sync points by type",
                          static_cast<unsigned long long>(ops), static_cast<unsigned long long>(old_n),
                          static_cast<unsigned long long>(guest), static_cast<unsigned long long>(net),
                          static_cast<unsigned long long>(missed), static_cast<unsigned long long>(extra),
                          static_cast<unsigned long long>(new_n));
    }
    if (n < 0 || static_cast<u32>(n) >= size) {
        return;
    }
    for (u32 k = 0; k < static_cast<u32>(Sync::Count); ++k) {
        const int m = std::snprintf(out + n, size - n, " %s %llu", kSyncNames[k],
                                    static_cast<unsigned long long>(sync[k]));
        if (m < 0 || static_cast<u32>(n + m) >= size) {
            break;
        }
        n += m;
    }
}

} // namespace SotcArch::GuestBarrier
