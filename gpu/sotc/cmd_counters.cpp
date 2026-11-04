// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CMD_COUNTERS=1 (off by default; observe only, nothing is changed). Cheap counters for the four
// questions of docs/re-stutters/native/arch-next (command-path step 0, readbacks steps 1-2, memory M0):
//
// 1. Identical frames (NoteDcb, NoteFrame). Per submitted command buffer: a hash of its bytes (XXH3, raw) and a hash
//    with every 64-bit value that lies in guest memory (the PS4 user range or the system-low range, the runtime's
//    USER_MIN..USER_MAX and SYSTEM_LOW_MIN..SYSTEM_LOW_MAX in src/runtime_memory.c) replaced by zero (normalized).
//    Report: buffers, distinct raw hashes, distinct normalized hashes, and the share of buffers equal to the previous
//    frame's buffer at the same submit index (raw and normalized). Submits made by the port itself (InitSequence) count
//    too, at the start of the run only.
// 2. CP time by opcode (PacketTimer, ProcessGraphics). One packet in 64 per thread is timed; the report multiplies the
//    sampled time by 64 and divides by the frames of the window: "ms/frame" per IT opcode (or per packet type),
//    top 10. Indirect buffers are not timed themselves (their packets are). Only the graphics queue is covered.
// 3. Readbacks (SpinScope, WaitRegMem in ProcessGraphics; NoteZeroMask, native_masks.cpp). Waits per window, the waits
//    that spun, their rounds, and the top 5 labels by wall time spent spinning. The zero masks of
//    SOTC_NATIVE_MASKS=prev per mask key (count only; the served data is unchanged).
// 4. Frame-slot waits (NoteFrameWait, native_sync.cpp). Each FrameWait is classed by the frame's state when it starts:
//    "cp-behind" (the command processor had not recorded the frame; the Liverpool frames_recorded counter), "gpu-behind"
//    (recorded, the GPU ticket not yet signalled), "no-counter" (no recorded counter in this mode), or "no-wait" (the GPU
//    had finished the frame). The class is taken at the start: time spent after the frame became recorded during a
//    cp-behind wait counts as cp-behind.
//
// Output: "SOTC CMD ..." lines, one per section that had activity, in the 5 s report (driver.cpp). That report runs only
// with SOTC_FPS=1 (or SOTC_FRAME_METRICS=1). The window is reset. The unique read/written counter of the host report
// (pc_ring.cpp) also needs this switch: PcHostRecordBind keeps its spans when it is on.
// Cost: the hashes and the normalized copy of every DCB, and one clock read per sampled packet and per wait.
// Measurement only: expect lower FPS while it is on.

#include "cmd_counters.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

namespace SotcCmdCounters {
namespace {

constexpr uint32_t kSampleEvery = 64;
constexpr size_t kOpKeys = 260; // 0..255 IT opcodes, 256 + packet type for the others
constexpr uint64_t kUserMin = 0x1000000000ull, kUserMax = 0xfc00000000ull;      // src/runtime_memory.c
constexpr uint64_t kSysLowMin = 0x0700000000ull, kSysLowMax = 0x0800000000ull; // src/runtime_memory.c

struct OpStat {
    uint64_t ns = 0; // sampled wall time
    uint64_t n = 0;  // sampled packets
};

struct LabelStat {
    uint64_t waits = 0; // waits that spun
    uint64_t rounds = 0;
    uint64_t ns = 0;
};

std::mutex g_mutex; // guards every window field below
std::array<OpStat, kOpKeys> g_ops{};
uint64_t g_dcbs = 0;
uint64_t g_frames = 0;
uint64_t g_identical_raw = 0;
uint64_t g_identical_norm = 0;
std::unordered_set<uint64_t> g_raw_set, g_norm_set;
std::vector<uint64_t> g_prev_raw, g_cur_raw, g_prev_norm, g_cur_norm;
std::unordered_map<uint64_t, LabelStat> g_labels;
uint64_t g_zero_total = 0, g_zero_unknown = 0;
std::unordered_map<uint64_t, uint64_t> g_zero_keys;
uint64_t g_fw_calls = 0, g_fw_noop = 0;
uint64_t g_fw_cp = 0, g_fw_cp_ns = 0, g_fw_gpu = 0, g_fw_gpu_ns = 0, g_fw_none = 0, g_fw_none_ns = 0;
uint64_t g_recorded_steps = 0;
uint64_t g_recorded_last = 0;

// The counters of the waits and the recorded counter are read without the mutex (atomics).
std::atomic<uint64_t> g_waits{0};
std::atomic<uint64_t> g_waits_spun{0};
std::atomic<const std::atomic<uint64_t>*> g_recorded{nullptr};

// Guest address test on a 64-bit value (the user and the system-low ranges of the runtime).
bool IsGuestAddress(uint64_t v) {
    return (v >= kUserMin && v < kUserMax) || (v >= kSysLowMin && v < kSysLowMax);
}

} // namespace

uint64_t NowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void SetRecordedCounter(const std::atomic<uint64_t>* recorded) noexcept {
    g_recorded.store(recorded, std::memory_order_release);
}

void NoteDcb(std::span<const uint32_t> dcb) noexcept {
    if (!kOn || dcb.empty()) {
        return;
    }
    const uint64_t raw = XXH3_64bits(dcb.data(), dcb.size_bytes());
    std::vector<uint32_t> masked(dcb.begin(), dcb.end());
    for (size_t i = 0; i + 1 < dcb.size(); ++i) {
        const uint64_t v = uint64_t{dcb[i]} | (uint64_t{dcb[i + 1]} << 32);
        if (IsGuestAddress(v)) {
            masked[i] = 0;
            masked[i + 1] = 0;
        }
    }
    const uint64_t norm = XXH3_64bits(masked.data(), masked.size() * sizeof(uint32_t));
    std::scoped_lock lock{g_mutex};
    ++g_dcbs;
    g_raw_set.insert(raw);
    g_norm_set.insert(norm);
    const size_t index = g_cur_raw.size();
    if (index < g_prev_raw.size() && g_prev_raw[index] == raw) {
        ++g_identical_raw;
    }
    if (index < g_prev_norm.size() && g_prev_norm[index] == norm) {
        ++g_identical_norm;
    }
    g_cur_raw.push_back(raw);
    g_cur_norm.push_back(norm);
}

void NoteFrame() noexcept {
    if (!kOn) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ++g_frames;
    g_prev_raw.swap(g_cur_raw);
    g_cur_raw.clear();
    g_prev_norm.swap(g_cur_norm);
    g_cur_norm.clear();
    if (const std::atomic<uint64_t>* rec = g_recorded.load(std::memory_order_acquire)) {
        const uint64_t r = rec->load(std::memory_order_acquire);
        if (r > g_recorded_last) {
            g_recorded_steps += r - g_recorded_last;
        }
        g_recorded_last = r;
    }
}

void PacketTimer::Begin() noexcept {
    static thread_local uint32_t tick = 0;
    if ((tick++ % kSampleEvery) != 0) {
        return;
    }
    active_ = true;
    t0_ = NowNs();
}

void PacketTimer::Finish() noexcept {
    const uint64_t ns = NowNs() - t0_;
    std::scoped_lock lock{g_mutex};
    g_ops[key_].ns += ns;
    ++g_ops[key_].n;
}

SpinScope::SpinScope(uint64_t label) noexcept : label_(label) {
    if (!kOn) {
        return;
    }
    g_waits.fetch_add(1, std::memory_order_relaxed);
    t0_ = NowNs();
}

SpinScope::~SpinScope() {
    if (!kOn || rounds_ == 0) {
        return;
    }
    const uint64_t ns = NowNs() - t0_;
    g_waits_spun.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{g_mutex};
    LabelStat& s = g_labels[label_];
    ++s.waits;
    s.rounds += rounds_;
    s.ns += ns;
}

void NoteZeroMask(uint64_t key, bool unknown) noexcept {
    if (!kOn) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ++g_zero_total;
    if (unknown) {
        ++g_zero_unknown;
    }
    ++g_zero_keys[unknown ? 0 : key];
}

int RecordedAt(uint64_t frame) noexcept {
    const std::atomic<uint64_t>* rec = g_recorded.load(std::memory_order_acquire);
    if (!rec) {
        return -1;
    }
    return rec->load(std::memory_order_acquire) >= frame ? 1 : 0;
}

void NoteFrameWait(int recorded_state, uint64_t ns, bool completed_at_entry) noexcept {
    if (!kOn) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ++g_fw_calls;
    if (completed_at_entry) {
        ++g_fw_noop;
    } else if (recorded_state == 0) {
        ++g_fw_cp;
        g_fw_cp_ns += ns;
    } else if (recorded_state == 1) {
        ++g_fw_gpu;
        g_fw_gpu_ns += ns;
    } else {
        ++g_fw_none;
        g_fw_none_ns += ns;
    }
}

void Report(std::string& out, double seconds) {
    if (!kOn || seconds <= 0.0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    char buf[640];
    const double frames = g_frames ? static_cast<double>(g_frames) : 1.0;

    if (g_dcbs) {
        std::snprintf(buf, sizeof buf,
                      "SOTC CMD DCB %.1fs: buffers %llu (%.1f per frame, frames %llu), distinct raw %zu, distinct "
                      "normalized %zu, same as previous frame at the same index: raw %.1f%%, normalized %.1f%%\n",
                      seconds, static_cast<unsigned long long>(g_dcbs), static_cast<double>(g_dcbs) / frames,
                      static_cast<unsigned long long>(g_frames), g_raw_set.size(), g_norm_set.size(),
                      100.0 * static_cast<double>(g_identical_raw) / static_cast<double>(g_dcbs),
                      100.0 * static_cast<double>(g_identical_norm) / static_cast<double>(g_dcbs));
        out += buf;
    }

    // CP time by opcode: the sampled time times the sample interval, per frame of the window.
    std::vector<size_t> keys;
    for (size_t k = 0; k < kOpKeys; ++k) {
        if (g_ops[k].n) {
            keys.push_back(k);
        }
    }
    if (!keys.empty()) {
        std::sort(keys.begin(), keys.end(), [](size_t a, size_t b) { return g_ops[a].ns > g_ops[b].ns; });
        std::string line = "SOTC CMD CP ms/frame (1 in 64 sampled, frames " +
                           std::to_string(static_cast<unsigned long long>(g_frames)) + "):";
        for (size_t i = 0; i < keys.size() && i < 10; ++i) {
            const size_t k = keys[i];
            const double ms = static_cast<double>(g_ops[k].ns) * kSampleEvery / 1e6 / frames;
            std::snprintf(buf, sizeof buf, " %s 0x%02zx %.3f (n %llu)", k < 256 ? "op" : "type", k < 256 ? k : k - 256,
                          ms, static_cast<unsigned long long>(g_ops[k].n));
            line += buf;
        }
        out += line + "\n";
    }

    const uint64_t waits = g_waits.exchange(0, std::memory_order_relaxed);
    const uint64_t spun = g_waits_spun.exchange(0, std::memory_order_relaxed);
    if (waits || !g_labels.empty()) {
        uint64_t rounds = 0;
        for (const auto& [label, s] : g_labels) {
            rounds += s.rounds;
        }
        std::vector<std::pair<uint64_t, uint64_t>> top; // (ns, label)
        for (const auto& [label, s] : g_labels) {
            top.emplace_back(s.ns, label);
        }
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::snprintf(buf, sizeof buf, "SOTC CMD SPIN waits %llu, spun %llu, rounds %llu; top labels by wall ms:",
                      static_cast<unsigned long long>(waits), static_cast<unsigned long long>(spun),
                      static_cast<unsigned long long>(rounds));
        std::string line = buf;
        for (size_t i = 0; i < top.size() && i < 5; ++i) {
            const LabelStat& s = g_labels[top[i].second];
            std::snprintf(buf, sizeof buf, " 0x%llx waits %llu rounds %llu ms %.2f;",
                          static_cast<unsigned long long>(top[i].second), static_cast<unsigned long long>(s.waits),
                          static_cast<unsigned long long>(s.rounds), static_cast<double>(s.ns) / 1e6);
            line += buf;
        }
        out += line + "\n";
    }

    if (g_zero_total) {
        std::vector<std::pair<uint64_t, uint64_t>> top; // (count, key)
        for (const auto& [key, n] : g_zero_keys) {
            top.emplace_back(n, key);
        }
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::snprintf(buf, sizeof buf, "SOTC CMD ZEROMASK %llu (unknown key %llu); keys:",
                      static_cast<unsigned long long>(g_zero_total), static_cast<unsigned long long>(g_zero_unknown));
        std::string line = buf;
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
            std::snprintf(buf, sizeof buf, " 0x%llx %llu;", static_cast<unsigned long long>(top[i].second),
                          static_cast<unsigned long long>(top[i].first));
            line += buf;
        }
        out += line + "\n";
    }

    if (g_fw_calls) {
        // Without frame tickets (SOTC_CP_AHEAD=2 or SOTC_FRAME_AHEAD=1) there is no recorded counter: every wait is
        // "no-counter".
        const char* rec = g_recorded.load(std::memory_order_acquire) ? "on" : "off (no frame tickets)";
        std::snprintf(buf, sizeof buf,
                      "SOTC CMD FRAMEWAIT calls %llu, no-wait %llu, cp-behind %llu (%.2f ms), gpu-behind %llu "
                      "(%.2f ms), no-counter %llu (%.2f ms); recorded counter %s, steps %llu\n",
                      static_cast<unsigned long long>(g_fw_calls), static_cast<unsigned long long>(g_fw_noop),
                      static_cast<unsigned long long>(g_fw_cp), static_cast<double>(g_fw_cp_ns) / 1e6,
                      static_cast<unsigned long long>(g_fw_gpu), static_cast<double>(g_fw_gpu_ns) / 1e6,
                      static_cast<unsigned long long>(g_fw_none), static_cast<double>(g_fw_none_ns) / 1e6, rec,
                      static_cast<unsigned long long>(g_recorded_steps));
        out += buf;
    }

    // Window reset. The previous frame's buffers stay for the next window's comparison.
    g_ops.fill(OpStat{});
    g_dcbs = 0;
    g_frames = 0;
    g_identical_raw = 0;
    g_identical_norm = 0;
    g_raw_set.clear();
    g_norm_set.clear();
    g_labels.clear();
    g_zero_total = 0;
    g_zero_unknown = 0;
    g_zero_keys.clear();
    g_fw_calls = g_fw_noop = g_fw_cp = g_fw_cp_ns = g_fw_gpu = g_fw_gpu_ns = g_fw_none = g_fw_none_ns = 0;
    g_recorded_steps = 0;
}

} // namespace SotcCmdCounters
