// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PREPASS_PROBE (docs/re-backend/04-depth-prepass.md, section 9). Observe only.
// The main depth target address is learned from the draws, the same way SOTC_NO_PREPASS learns the
// target (the non-square depth target with the largest area). The probe's hooks (gpu/sotc/probe_prepass.cpp)
// read it to compare the input of ResolveSunShadows with the main depth.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "video_core/amdgpu/regs.h"
#include "video_core/sotc_stats.h"

namespace SotcPrepassProbe {

inline bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PREPASS_PROBE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

/// SOTC_NO_PREPASS=1: the opaque XPR per-object draws leave the depth pre-pass (they are measured to be redrawn by the
/// G-buffer, docs/re-backend/04-depth-prepass.md); their G-buffer draws test LESS_EQUAL with depth writes instead of
/// EQUAL. Draws with a discarding pixel shader keep their pre-pass draw (their G-buffer shader does not discard).
inline bool SkipMode() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_NO_PREPASS");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}
/// SOTC_NO_PREPASS=3: the same rule decided on the command processor thread (it has the pipeline when it queues
/// the op): a dropped draw's op carries a skip mark and the draw thread does not run it; a promoted G-buffer op carries
/// the pre-pass depth function (Liverpool::SubmitDrawIndirect, ExecuteBackOp).
inline bool CpSkipMode() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_NO_PREPASS");
        return v != nullptr && v[0] == '3';
    }();
    return on;
}

inline std::atomic<uint64_t> main_addr{0}; ///< guest address of the main depth target (0 until learned)
inline std::atomic<uint64_t> main_area{0};
inline std::atomic<uint64_t> main_dims{0}; ///< pitch << 32 | height of the main depth target

/// Called once per draw with the draw registers (the same two draw entries as SotcNoPrepass::Count).
inline void NoteDraw(const AmdGpu::Regs& regs) {
    if (!(On() || SkipMode() || CpSkipMode()) || !regs.depth_buffer.DepthValid()) {
        return;
    }
    const uint32_t pitch = regs.depth_buffer.Pitch();
    const uint32_t height = regs.depth_buffer.Height();
    if (pitch == height || pitch < 512) {
        return;
    }
    const uint64_t area = uint64_t{pitch} * height;
    uint64_t cur = main_area.load(std::memory_order_relaxed);
    while (area > cur) {
        if (main_area.compare_exchange_weak(cur, area, std::memory_order_relaxed)) {
            main_dims.store((uint64_t{pitch} << 32) | height, std::memory_order_relaxed);
            main_addr.store(regs.depth_buffer.DepthWriteAddress(), std::memory_order_relaxed);
            break;
        }
    }
}

// Readers of the main depth between the pre-pass and the G-buffer (SOTC_PREPASS_PROBE, observe only).
// Per frame, the phase is 0 before the first pre-pass draw, 1 after it, 2 from the first G-buffer draw on.
// A pre-pass draw writes the main depth and no colour target; a G-buffer draw writes it and at least one
// colour target. The phase and the frame are kept on the draw thread; the window counters are behind a mutex.
inline uint64_t phase_frame = ~uint64_t{0}; ///< flip_count the phase belongs to
inline int phase = 0;                        ///< 0, 1 or 2 (see above)

struct ReaderWindow {
    uint64_t dispatches = 0; ///< phase-1 dispatches that read the main depth
    uint64_t draws = 0;      ///< phase-1 draws that read the main depth
    uint64_t gbuffer = 0;    ///< phase-2 ops (draws and dispatches) that read the main depth
    std::vector<std::pair<uint64_t, bool>> shaders; ///< first 8 distinct phase-1 readers: key, is_draw
};

inline std::mutex& ReaderMutex() {
    static std::mutex m;
    return m;
}

inline ReaderWindow& ReaderWin() {
    static ReaderWindow w;
    return w;
}

/// 0: neither, 1: pre-pass draw (main depth, no colour target), 2: G-buffer draw (main depth and colour).
inline int DrawKind(const AmdGpu::Regs& regs, uint64_t main) {
    if (main == 0 || !regs.depth_buffer.DepthValid() || regs.depth_buffer.DepthWriteAddress() != main) {
        return 0;
    }
    for (int cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (col_buf && regs.color_target_mask.GetMask(cb) != 0) {
            return 2;
        }
    }
    return 1;
}

/// Called once per draw or dispatch, after its bindings are resolved (draw thread). regs: the draw registers
/// (nullptr for a dispatch). reads: an image the op reads (bound, not a render target) has the main depth base.
/// key: the op's pipeline key (the stage pgm_hash values, combined by the caller; only used for reads).
inline void NoteBind(bool is_draw, const AmdGpu::Regs* regs, bool reads, uint64_t key) {
    if (!On()) {
        return;
    }
    const uint64_t frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != phase_frame) {
        phase_frame = frame;
        phase = 0;
    }
    if (is_draw && regs != nullptr) {
        const int kind = DrawKind(*regs, main_addr.load(std::memory_order_relaxed));
        if (kind == 1 && phase == 0) {
            phase = 1;
        } else if (kind == 2) {
            phase = 2;
        }
    }
    if (!reads) {
        return;
    }
    std::lock_guard<std::mutex> lock(ReaderMutex());
    ReaderWindow& w = ReaderWin();
    if (phase == 2) {
        ++w.gbuffer;
    } else if (phase == 1) {
        if (is_draw) {
            ++w.draws;
        } else {
            ++w.dispatches;
        }
        bool seen = false;
        for (const auto& s : w.shaders) {
            seen = seen || s.first == key;
        }
        if (!seen && w.shaders.size() < 8) {
            w.shaders.emplace_back(key, is_draw);
        }
    }
}

/// The report line of the 5 s window (driver.cpp). Silent until a pre-pass was learned.
inline void ReportReaders(std::string& out) {
    if (!On() || main_addr.load(std::memory_order_relaxed) == 0) {
        return;
    }
    ReaderWindow w;
    {
        std::lock_guard<std::mutex> lock(ReaderMutex());
        w = ReaderWin();
        ReaderWin() = ReaderWindow{};
    }
    std::string shaders;
    for (const auto& s : w.shaders) {
        char hex[40];
        std::snprintf(hex, sizeof hex, " 0x%llx(%s)", static_cast<unsigned long long>(s.first),
                      s.second ? "draw" : "dispatch");
        shaders += hex;
    }
    if (shaders.empty()) {
        shaders = " none";
    }
    char line[700];
    std::snprintf(line, sizeof line,
                  "SOTC PREPASS PROBE: depth readers between prepass and gbuffer: dispatches %llu, draws "
                  "%llu (first shaders%s), readers during gbuffer: %llu\n",
                  static_cast<unsigned long long>(w.dispatches), static_cast<unsigned long long>(w.draws),
                  shaders.c_str(), static_cast<unsigned long long>(w.gbuffer));
    out += line;
}


/// Indirect draws into the main depth target (the XPR per-object draws are DrawIndexIndirect): which argument records
/// the pre-pass draws and which the G-buffer draws, per frame. Identity A: the argument address. Identity B: the index
/// buffer base with the argument offset modulo 200000 (the engine's 10000 x 20-byte args buffer), for passes that use
/// separate argument buffers with the same layout. Draw thread only, except the report (under ArgsMutex).
struct ArgsWindow {
    uint64_t frames = 0, pre = 0, gb = 0, pre_only_a = 0, pre_only_b = 0, gb_only_a = 0;
    uint64_t pre_discard = 0, unsafe = 0;
    uint64_t pre_stencil_w = 0, gb_stencil_w = 0, phases = 0, phase_frames = 0, frames_multi = 0; ///< stencil writers, pre-pass phases ///< pre-pass records with discard; of those, G-buffer draw without discard
};
inline std::mutex& ArgsMutex() {
    static std::mutex m;
    return m;
}
inline ArgsWindow& ArgsWin() {
    static ArgsWindow w;
    return w;
}
inline void NoteArgs(const AmdGpu::Regs& regs, uint64_t arg_addr, bool discard) {
    if (!On()) {
        return;
    }
    static std::unordered_set<uint64_t> pre_a, gb_a, pre_b, gb_b, pre_kill, gb_kill;
    static uint64_t frame = ~uint64_t{0};
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (flip != frame) {
        if (frame != ~uint64_t{0} && (!pre_a.empty() || !gb_a.empty())) {
            ArgsWindow f;
            f.frames = 1;
            f.pre = pre_a.size();
            f.gb = gb_a.size();
            for (const auto a : pre_a) {
                f.pre_only_a += gb_a.count(a) ? 0 : 1;
            }
            for (const auto b : pre_b) {
                f.pre_only_b += gb_b.count(b) ? 0 : 1;
            }
            for (const auto a : gb_a) {
                f.gb_only_a += pre_a.count(a) ? 0 : 1;
            }
            for (const auto a : pre_kill) {
                f.pre_discard += 1;
                f.unsafe += gb_kill.count(a) ? 0 : 1;
            }
            std::lock_guard<std::mutex> lock(ArgsMutex());
            auto& w = ArgsWin();
            w.frames += 1;
            w.pre += f.pre;
            w.gb += f.gb;
            w.pre_only_a += f.pre_only_a;
            w.pre_only_b += f.pre_only_b;
            w.gb_only_a += f.gb_only_a;
            w.pre_discard += f.pre_discard;
            w.unsafe += f.unsafe;
        }
        pre_a.clear();
        gb_a.clear();
        pre_b.clear();
        gb_b.clear();
        pre_kill.clear();
        gb_kill.clear();
        frame = flip;
    }
    const int kind = DrawKind(regs, main_addr.load(std::memory_order_relaxed));
    if (kind == 0) {
        return;
    }
    {
        // Stencil written by this draw: test on, a non-Keep op on either face, a non-zero write mask.
        const auto& sc = regs.stencil_control;
        const bool ops = sc.stencil_fail_front != AmdGpu::StencilFunc::Keep ||
                         sc.stencil_zpass_front != AmdGpu::StencilFunc::Keep ||
                         sc.stencil_zfail_front != AmdGpu::StencilFunc::Keep ||
                         sc.stencil_fail_back != AmdGpu::StencilFunc::Keep ||
                         sc.stencil_zpass_back != AmdGpu::StencilFunc::Keep ||
                         sc.stencil_zfail_back != AmdGpu::StencilFunc::Keep;
        const bool writes = regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid() && ops &&
                            (regs.stencil_ref_front.stencil_write_mask != 0 ||
                             regs.stencil_ref_back.stencil_write_mask != 0);
        std::lock_guard<std::mutex> lock(ArgsMutex());
        auto& w = ArgsWin();
        (kind == 1 ? w.pre_stencil_w : w.gb_stencil_w) += writes ? 1 : 0;
        // Pre-pass phases per frame: a pre-pass draw after a G-buffer draw starts another view.
        static uint64_t ph_frame = ~uint64_t{0};
        static int last = 0;
        static uint64_t ph_count = 0;
        if (flip != ph_frame) {
            if (ph_frame != ~uint64_t{0}) {
                w.phases += ph_count;
                w.phase_frames += 1;
                w.frames_multi += ph_count > 1 ? 1 : 0;
            }
            ph_frame = flip;
            ph_count = 0;
            last = 0;
        }
        if (kind == 1 && last != 1) {
            ++ph_count;
        }
        last = kind;
    }
    const uint64_t b = (regs.index_base_address.Address<uint64_t>() << 20) ^ (arg_addr % 200000);
    (kind == 1 ? pre_a : gb_a).insert(arg_addr);
    (kind == 1 ? pre_b : gb_b).insert(b);
    if (discard) {
        (kind == 1 ? pre_kill : gb_kill).insert(arg_addr);
    }
}
inline void ReportArgs(std::string& out) {
    if (!On()) {
        return;
    }
    ArgsWindow w;
    {
        std::lock_guard<std::mutex> lock(ArgsMutex());
        w = ArgsWin();
        ArgsWin() = ArgsWindow{};
    }
    if (w.frames == 0) {
        return;
    }
    const double f = double(w.frames);
    char line[600];
    std::snprintf(line, sizeof line,
                  "SOTC PREPASS PROBE ARGS: indirect records per frame: pre-pass %.0f, G-buffer %.0f, pre-pass only "
                  "%.0f by address / %.0f by index base+offset, G-buffer only %.0f; pre-pass with discard %.0f, of those G-buffer without discard %.0f; stencil writers pre %.0f gb %.0f; pre-pass phases per frame %.2f (frames with 2+: %llu); over %llu frames\n",
                  w.pre / f, w.gb / f, w.pre_only_a / f, w.pre_only_b / f, w.gb_only_a / f,
                  w.pre_discard / f, w.unsafe / f, w.pre_stencil_w / f, w.gb_stencil_w / f,
                  w.phase_frames ? double(w.phases) / double(w.phase_frames) : 0.0,
                  static_cast<unsigned long long>(w.frames_multi), static_cast<unsigned long long>(w.frames));
    out += line;
}


/// SOTC_NO_PREPASS=1 state (draw thread only). t_promote: the current draw's EQUAL depth test becomes LESS_EQUAL with
/// writes (read by Rasterizer::ApplyDynamic).
inline thread_local bool t_promote = false;
/// The depth function the dropped pre-pass draw used (the engine's convention, e.g. reverse Z): the promoted G-buffer
/// draw tests with its "or equal" form, so the nearest fragment wins as it did in the pre-pass.
inline thread_local AmdGpu::CompareFunc t_promote_func = AmdGpu::CompareFunc::LessEqual;
inline std::atomic<uint64_t> skip_count{0}, promote_count{0}, kept_discard{0};
inline std::unordered_map<uint64_t, AmdGpu::CompareFunc>& Skipped() {
    static std::unordered_map<uint64_t, AmdGpu::CompareFunc> s;
    return s;
}
/// An indirect draw: true when it is an opaque pre-pass draw to drop. Records it for its G-buffer draw.
inline bool SkipPrepassDraw(const AmdGpu::Regs& regs, uint64_t arg_addr, bool discard) {
    if (!SkipMode()) {
        return false;
    }
    // The map lives from the first pre-pass draw of a view to the next one: the flip counter can advance between a
    // frame's pre-pass and its G-buffer, so it must not end the map (a G-buffer draw would miss its record and test
    // EQUAL against depth nobody wrote: the object vanishes for that frame).
    static int last_kind = 0;
    const int kind = DrawKind(regs, main_addr.load(std::memory_order_relaxed));
    if (kind == 1 && last_kind == 2) {
        Skipped().clear();
    }
    if (kind != 0) {
        last_kind = kind;
    }
    if (kind == 1) {
        if (discard) {
            kept_discard.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Skipped()[arg_addr] = regs.depth_control.depth_func;
        skip_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (const auto it = Skipped().find(arg_addr); kind == 2 && it != Skipped().end()) {
        t_promote = true;
        t_promote_func = it->second;
        promote_count.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}
inline void ReportSkip(std::string& out) {
    if (!SkipMode()) {
        return;
    }
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC NO PREPASS: mode 1, pre-pass draws dropped %llu, kept (discard) %llu, G-buffer draws promoted to "
                  "LESS_EQUAL %llu (5 s window), main depth %#llx\n",
                  (unsigned long long)skip_count.exchange(0), (unsigned long long)kept_discard.exchange(0),
                  (unsigned long long)promote_count.exchange(0), (unsigned long long)main_addr.load());
    out += line;
}


/// The draw thread's view of a CP mark: 0 none, otherwise 2 + the pre-pass depth function (promote).
inline thread_local uint8_t t_cp_mark = 0;
inline std::atomic<uint64_t> cp_skip{0}, cp_kept{0}, cp_promote{0};
/// CP thread: the mark for an indirect draw. 1: drop (opaque pre-pass draw), 2 + func: promote, 0: as is.
inline uint8_t CpClassify(const AmdGpu::Regs& regs, uint64_t arg_addr, bool has_pipeline, bool discard) {
    // A record stays valid for its own pre-pass phase and the next one: a second pre-pass (another view, a scene
    // change) can start before every G-buffer draw of the first has been parsed. Older records are pruned.
    struct Rec {
        AmdGpu::CompareFunc func;
        uint64_t phase;
    };
    static std::unordered_map<uint64_t, Rec> skipped; // CP thread only
    static int last_kind = 0;
    static uint64_t phase = 0;
    const int kind = DrawKind(regs, main_addr.load(std::memory_order_relaxed));
    if (kind == 1 && last_kind == 2) {
        ++phase;
        if (skipped.size() > 16384) {
            std::erase_if(skipped, [](const auto& e) { return e.second.phase + 1 < phase; });
        }
    }
    if (kind != 0) {
        last_kind = kind;
    }
    if (kind == 1 && has_pipeline) {
        if (discard) {
            skipped.erase(arg_addr); // this phase keeps its pre-pass draw: an older record must not promote it
            cp_kept.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        skipped[arg_addr] = Rec{regs.depth_control.depth_func, phase};
        cp_skip.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    if (kind == 2) {
        if (const auto it = skipped.find(arg_addr); it != skipped.end() && it->second.phase + 1 >= phase) {
            cp_promote.fetch_add(1, std::memory_order_relaxed);
            return static_cast<uint8_t>(2 + static_cast<uint32_t>(it->second.func));
        }
    }
    return 0;
}
inline void ReportCpSkip(std::string& out) {
    if (!CpSkipMode()) {
        return;
    }
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC NO PREPASS: mode 3 (CP), pre-pass draws dropped %llu, kept (discard) %llu, G-buffer draws "
                  "promoted %llu (5 s window), main depth %#llx\n",
                  (unsigned long long)cp_skip.exchange(0), (unsigned long long)cp_kept.exchange(0),
                  (unsigned long long)cp_promote.exchange(0), (unsigned long long)main_addr.load());
    out += line;
}

} // namespace SotcPrepassProbe
