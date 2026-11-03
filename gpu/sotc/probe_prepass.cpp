// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PREPASS_PROBE (docs/re-backend/04-depth-prepass.md, section 9; 13-resolve-sun-shadows.md).
// Observe only: every handler reads guest memory and returns HookAction::Continue.
//
// Probe A (resolve input). Entry hooks on ResolveSunShadows, 0xafcc0, and its twin 0xb0100. rsi = pass.
//   T_in = *(*(pass + 0x18) + 0x2110) and its T# is at T_in + 0xc4 (dword0 = base >> 8). The T# base is
//   compared with the main depth target (SotcPrepassProbe::main_addr, learned from the draws).
// Probe B (object lists). Entry hook on the object pass 0x11d3d0: rsi = the pass container (the main view's
//   container is view[0xc], it holds the lists at +0x68 + p * 0x20), ecx = the list index p. List p is the
//   array [begin, end) of 0x20-byte entries, begin = container+0x68+p*0x20, end = container+0x70+p*0x20; the
//   object pointer is at entry +8 (decompile 0011/0011d3d0.c).
//   Pre-pass lists 0-4: calls made inside the pre-pass job 0xc1860 (the RenderDepth job, queued by
//   BPRender_DepthAndGBufferPasses 0xc59d0 only). A wrap hook on 0xc1860 sets a thread-local tag for the job.
//   G-buffer lists 5-8: 0xc1ce0 has a prologue that is not unique (it also matches 0xc4a50), so these calls are
//   not tagged by the job. They are taken from the same container as the pre-pass lists of the frame (the main
//   view's container, SotcProbes g_main_container). Cascades and other views use other containers (assumption,
//   checked by the report: G = 0 would show it).
//
// Probe C (XPR masked draws). Entry hook on BPXPR_RecordMaskedDraws 0x11d7a0 (PREPASS_XPR_MASKED), and wraps
//   on the G-buffer jobs 0xc1ce0 and 0xc1fc0 (PREPASS_GBUFFER_VERT / _PROBE) that set a thread-local G-buffer tag.
//   Phase of a call: pre-pass (inside 0xc1860), G-buffer (inside 0xc1ce0 or 0xc1fc0), other (cascades, 0xc46c0,
//   0xcd330). rcx = mask id, rdx = view, rsi = batch vector (16-byte elements, first qword = batch entry E).
//   The scan replays 0x19af20 and 0x1a6d20 (docs/re-backend/11-xpr-args.md, sections 1.5 and 2.1), read only:
//     E + 0x18 = set record S (0 = skipped), E + 0x148 = u32 XPR index X.
//     slot = view[0x38] + 0xa800 + mask * 0x28; slot + 8 = pass record P; the qword at slot + 0x20 has the kind
//     (low dword) and hi (high dword). The mask pointer is a qword in the XPR record view[0x38] + X * 0xa8, at a
//     field chosen by kind (hi moves the index for kinds 0 and 7).
//     The dispatch is 0x1a6d20 when bits 32-39 of P[0x48] are clear and (kind 0 or 7, or view[0x48][0x22] == 0).
//     0x1a6d20 draws the bits i < count (S + 0x64, u16) set in the mask whose membership bit is set:
//     membership = (S + 0x50 -> plVar5)[+0x20] (u32 per object), bit = (membership[i] >> (kind & 31)) & 1.
//     The other dispatch (0x1a7290, not decoded) is counted as mask bits only, not as draws.
//   The mask is read at entry; the consumer reads it after the label wait in 0x11d7a0 (assumed unchanged).
//   An object is identified by (set record, index). The pre-only count uses the same key.
//   Grass 0xdb700 and the particle thunk 0x10aee0 (entries) are counted per phase.
//
// The frame is the guest flip count (SotcStats::flip_count, as the shadow cache probe). Per frame the probe
// collects the object sets; the report averages them over the frames of the window.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hooks.h"
#include "guest_mem.h"
#include "probe_prepass.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_prepass_probe.h"

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kResolveA = "PREPASS_RESOLVE_A";
constexpr const char* kResolveB = "PREPASS_RESOLVE_B";
constexpr const char* kJobPrepass = "PREPASS_JOB";
constexpr const char* kListPass = "PREPASS_LIST_PASS";
constexpr const char* kXprMasked = "PREPASS_XPR_MASKED";
constexpr const char* kJobGBufferVert = "PREPASS_GBUFFER_VERT";
constexpr const char* kJobGBufferProbe = "PREPASS_GBUFFER_PROBE";
constexpr const char* kGrass = "PREPASS_GRASS";
constexpr const char* kParticles = "PREPASS_PARTICLES";

constexpr uint64_t kPassCtx = 0x18;      // pass + 0x18: the context
constexpr uint64_t kCtxInput = 0x2110;   // ctx + 0x2110: T_in (CTexture)
constexpr uint64_t kTsharpOffset = 0xc4; // T_in + 0xc4: the 32-byte T#, dword0 = base >> 8

constexpr uint64_t kListOffset = 0x68; // container + 0x68 + p * 0x20 = begin, + 0x70 = end
constexpr uint64_t kListStride = 0x20;
constexpr uint64_t kObjectOffset = 8;  // entry + 8 = object pointer
constexpr uint64_t kMaxEntries = 1u << 16;
constexpr int kPrePassLists = 5;       // lists 0-4
constexpr int kGBufferLists = 4;       // lists 5-8
constexpr size_t kMaxOtherBases = 8;

// Probe C offsets (decompiles 0011/0011d7a0.c, 0019/0019af20.c, 001a/001a6d20.c).
constexpr uint64_t kViewXprContext = 0x38;  // view + 0x38: the XPR context (lVar4)
constexpr uint64_t kViewFlagsPtr = 0x48;    // view + 0x48: pointer; its byte +0x22 selects the dispatch
constexpr uint64_t kViewFlagsByte = 0x22;
constexpr uint64_t kCtxMaskSlots = 0xa800;  // ctx + 0xa800 + mask * 0x28: the mask slot
constexpr uint64_t kMaskSlotStride = 0x28;
constexpr uint64_t kSlotPass = 0x8;         // slot + 8: pointer to the pass record P
constexpr uint64_t kSlotKind = 0x20;        // slot + 0x20: qword, kind = low dword, hi = high dword
constexpr uint64_t kPassFlags = 0x48;       // P + 0x48: qword, bits 32-39 select the dispatch
constexpr uint64_t kRecordStride = 0xa8;    // ctx + X * 0xa8: the per-XPR record
constexpr uint64_t kBatchBegin = 0x8;       // batch vector: begin, end
constexpr uint64_t kBatchEnd = 0x10;
constexpr uint64_t kBatchStride = 0x10;     // element: first qword = batch entry E
constexpr uint64_t kEntrySet = 0x18;        // E + 0x18: set record S
constexpr uint64_t kEntryIndex = 0x148;     // E + 0x148: u32 XPR index X
constexpr uint64_t kSetList = 0x50;         // S + 0x50: plVar5
constexpr uint64_t kSetCount = 0x64;        // S + 0x64: u16 object count
constexpr uint64_t kListMembership = 0x20;  // plVar5 + 0x20 (plVar5[4]): u32 per object
constexpr uint64_t kMaxBatch = 4096;
constexpr uint64_t kMaskBase = 0x10;        // masks 0x10-0x14 have their own slot, any other id slot 5
constexpr uint64_t kMaskSpan = 5;
constexpr int kXprSlots = 6;
constexpr int kPhasePre = 0;
constexpr int kPhaseGBuffer = 1;
constexpr int kPhaseOther = 2;
constexpr int kPhases = 3;

std::mutex g_mutex;

// Probe A: window counters and the first distinct other input bases (kept for the whole run).
struct ResolveWindow {
    uint64_t calls = 0;
    uint64_t on_main = 0;
    uint64_t other = 0;
    uint64_t tsharp_failed = 0;
};
ResolveWindow g_resolve;
std::vector<uint64_t> g_other_bases;

// Probe B: the current frame's object sets.
struct Frame {
    bool any = false;
    uint64_t key = ~uint64_t{0};
    std::array<std::unordered_set<uint64_t>, kPrePassLists> pre;
    std::array<std::unordered_set<uint64_t>, kGBufferLists> gb;
};
Frame g_frame;

// Probe B: sums over the finalized frames of the window.
struct ListWindow {
    uint64_t frames = 0;
    uint64_t pre_union = 0;
    uint64_t gb_union = 0;
    uint64_t x_union = 0;
    std::array<uint64_t, kPrePassLists> pre_list{};
    std::array<uint64_t, kPrePassLists> x_list{};
    uint64_t list_failed = 0;
};
ListWindow g_lists;

// Probe C: an XPR object is (set record, object index).
using ObjKey = std::pair<uint64_t, uint32_t>;

// Probe C: the current frame. objs[phase][slot] = drawn objects of the phase per mask slot.
struct XprFrame {
    bool any = false;
    uint64_t key = ~uint64_t{0};
    std::array<std::array<std::set<ObjKey>, kXprSlots>, kPhases> objs;
    std::array<std::set<uint64_t>, kPhases> sets; // set records dispatched in the phase
    std::array<uint64_t, kPhases> calls{};        // 0x11d7a0 calls
    std::array<uint64_t, kPhases> other_bits{};   // mask bits on the other dispatch (not drawn here)
    std::array<uint64_t, kPhases> grass{};        // 0xdb700 calls
    std::array<uint64_t, kPhases> particles{};    // 0x10aee0 calls
    uint64_t read_failed = 0;
};
XprFrame g_xf;

// Probe C: sums over the finalized frames of the window.
struct XprWindow {
    uint64_t frames = 0;
    std::array<uint64_t, kXprSlots> pre_slot{};
    std::array<uint64_t, kXprSlots> gb_slot{};
    std::array<uint64_t, kXprSlots> pre_only_slot{};
    uint64_t pre_union = 0;
    uint64_t gb_union = 0;
    uint64_t pre_only = 0;
    std::array<uint64_t, kPhases> calls{};
    std::array<uint64_t, kPhases> sets{};
    std::array<uint64_t, kPhases> other_bits{};
    std::array<uint64_t, kPhases> grass{};
    std::array<uint64_t, kPhases> particles{};
    uint64_t read_failed = 0;
};
XprWindow g_xw;

// Set by the wrap hook on the pre-pass job (guest thread that runs the job, not shared).
thread_local bool t_in_prepass = false;
// Set by the wrap hooks on the G-buffer jobs 0xc1ce0 and 0xc1fc0 (same thread rule).
thread_local bool t_in_gbuffer = false;
// The pass container of the pre-pass lists of the current frame (the main view's).
std::atomic<uint64_t> g_main_container{0};

// Adds the frame to the window and clears it. Caller holds g_mutex.
void FinalizeFrameLocked() {
    if (!g_frame.any) {
        return;
    }
    std::unordered_set<uint64_t> gb_all, pre_all;
    for (const auto& s : g_frame.gb) {
        gb_all.insert(s.begin(), s.end());
    }
    for (const auto& s : g_frame.pre) {
        pre_all.insert(s.begin(), s.end());
    }
    uint64_t x_union = 0;
    for (uint64_t o : pre_all) {
        if (gb_all.count(o) == 0) {
            ++x_union;
        }
    }
    ++g_lists.frames;
    g_lists.pre_union += pre_all.size();
    g_lists.gb_union += gb_all.size();
    g_lists.x_union += x_union;
    for (int k = 0; k < kPrePassLists; ++k) {
        g_lists.pre_list[k] += g_frame.pre[k].size();
        for (uint64_t o : g_frame.pre[k]) {
            if (gb_all.count(o) == 0) {
                ++g_lists.x_list[k];
            }
        }
    }
    for (auto& s : g_frame.pre) {
        s.clear();
    }
    for (auto& s : g_frame.gb) {
        s.clear();
    }
    g_frame.any = false;
}

// Adds the XPR frame to the window and clears it. Caller holds g_mutex.
void FinalizeXprLocked() {
    if (!g_xf.any) {
        return;
    }
    std::set<ObjKey> gb_all;
    for (int s = 0; s < kXprSlots; ++s) {
        gb_all.insert(g_xf.objs[kPhaseGBuffer][s].begin(), g_xf.objs[kPhaseGBuffer][s].end());
    }
    std::set<ObjKey> pre_all;
    for (int s = 0; s < kXprSlots; ++s) {
        g_xw.pre_slot[s] += g_xf.objs[kPhasePre][s].size();
        g_xw.gb_slot[s] += g_xf.objs[kPhaseGBuffer][s].size();
        for (const ObjKey& o : g_xf.objs[kPhasePre][s]) {
            pre_all.insert(o);
            if (gb_all.count(o) == 0) {
                ++g_xw.pre_only_slot[s];
            }
        }
    }
    for (const ObjKey& o : pre_all) {
        if (gb_all.count(o) == 0) {
            ++g_xw.pre_only;
        }
    }
    g_xw.pre_union += pre_all.size();
    g_xw.gb_union += gb_all.size();
    for (int p = 0; p < kPhases; ++p) {
        g_xw.calls[p] += g_xf.calls[p];
        g_xw.sets[p] += g_xf.sets[p].size();
        g_xw.other_bits[p] += g_xf.other_bits[p];
        g_xw.grass[p] += g_xf.grass[p];
        g_xw.particles[p] += g_xf.particles[p];
    }
    g_xw.read_failed += g_xf.read_failed;
    ++g_xw.frames;
    const uint64_t key = g_xf.key;
    g_xf = XprFrame{};
    g_xf.key = key;
}

// Probe C: starts the frame of a call (finalizes the previous one). Caller holds g_mutex.
void XprFrameLocked(uint64_t frame) {
    if (frame != g_xf.key) {
        FinalizeXprLocked();
        g_xf.key = frame;
    }
    g_xf.any = true;
}

// Reads the object pointers of list p of container. False when a read fails or the list is implausible.
bool ReadListObjects(uint64_t container, uint32_t p, std::vector<uint64_t>& out) {
    out.clear();
    const uint64_t slot = uint64_t{p} * kListStride;
    uint64_t begin = 0, end = 0;
    if (!SotcGuest::Read(container + kListOffset + slot, &begin) ||
        !SotcGuest::Read(container + kListOffset + 8 + slot, &end)) {
        return false;
    }
    if (end < begin || (end - begin) % kListStride != 0) {
        return false;
    }
    const uint64_t n = (end - begin) / kListStride;
    if (n == 0) {
        return true;
    }
    if (n > kMaxEntries) {
        return false;
    }
    std::vector<unsigned char> raw(n * kListStride);
    if (!SotcGuest::Read(begin, raw.data(), raw.size())) {
        return false;
    }
    out.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t obj = 0;
        std::memcpy(&obj, raw.data() + i * kListStride + kObjectOffset, sizeof obj);
        if (obj != 0) {
            out.push_back(obj);
        }
    }
    return true;
}

// Result of one 0x11d7a0 call scan (probe C). drawn is complete only when the scan returned true.
struct XprScan {
    bool path = false;           // the 0x1a6d20 dispatch (its objects are drawn)
    std::vector<uint64_t> sets;  // set records of the batch entries that have one
    std::vector<ObjKey> drawn;   // mask bit set and membership set (0x1a6d20 dispatch only)
    uint64_t other_bits = 0;     // mask bits on the other dispatch
};

// The mask pointer of 0x19af20's second switch: kind selects the field of the XPR record at base.
// False on a read failure; *mask = 0 for kinds with no field (no draws).
bool MaskPointer(uint64_t base, uint32_t kind, uint64_t hi, uint64_t* mask) {
    *mask = 0;
    const uint64_t hi32 = hi << 32;
    int64_t off = 0;
    uint64_t addr = 0;
    switch (kind) {
    case 0:
        off = (static_cast<int64_t>(hi32) + 0x400000000LL) >> 29;
        addr = base + static_cast<uint64_t>(off);
        break;
    case 1:
        addr = base + 8;
        break;
    case 2:
    case 3:
    case 6:
        addr = base;
        break;
    case 4:
        addr = base + 0x10;
        break;
    case 5:
        addr = base + 0x18;
        break;
    case 7:
        off = (static_cast<int64_t>(hi32) + 0xc00000000LL) >> 29;
        addr = base + static_cast<uint64_t>(off);
        break;
    default:
        return true;
    }
    return SotcGuest::Read(addr, mask);
}

// Probe C: replays the object selection of 0x11d7a0 for one mask (read only). False on a read failure.
bool ScanXpr(uint64_t view, uint64_t mask_id, uint64_t batch, XprScan& out) {
    out = XprScan{};
    uint64_t ctx = 0;
    if (!SotcGuest::Read(view + kViewXprContext, &ctx) || ctx == 0) {
        return false;
    }
    const uint64_t slot = ctx + kCtxMaskSlots + mask_id * kMaskSlotStride;
    uint64_t pass = 0, kind_word = 0;
    if (!SotcGuest::Read(slot + kSlotPass, &pass) || pass == 0 || !SotcGuest::Read(slot + kSlotKind, &kind_word)) {
        return false;
    }
    const uint32_t kind = static_cast<uint32_t>(kind_word);
    const uint64_t hi = kind_word >> 32;
    uint64_t pass_flags = 0, view_ptr = 0;
    uint8_t view_byte = 0;
    if (!SotcGuest::Read(pass + kPassFlags, &pass_flags) || !SotcGuest::Read(view + kViewFlagsPtr, &view_ptr) ||
        view_ptr == 0 || !SotcGuest::Read(view_ptr + kViewFlagsByte, &view_byte)) {
        return false;
    }
    // 0x19af20: the 0x1a6d20 dispatch when bits 32-39 of the pass flags are clear and (kind 0 or 7, or view byte 0).
    out.path = (pass_flags & 0xff00000000ull) == 0 && (kind == 0 || kind == 7 || view_byte == 0);

    uint64_t begin = 0, end = 0;
    if (!SotcGuest::Read(batch + kBatchBegin, &begin) || !SotcGuest::Read(batch + kBatchEnd, &end)) {
        return false;
    }
    if (end < begin || (end - begin) % kBatchStride != 0) {
        return false;
    }
    const uint64_t n = (end - begin) / kBatchStride;
    if (n > kMaxBatch) {
        return false;
    }
    std::vector<unsigned char> raw(n * kBatchStride);
    if (n != 0 && !SotcGuest::Read(begin, raw.data(), raw.size())) {
        return false;
    }
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t entry = 0;
        std::memcpy(&entry, raw.data() + i * kBatchStride, sizeof entry);
        if (entry == 0) {
            continue;
        }
        uint64_t set = 0;
        if (!SotcGuest::Read(entry + kEntrySet, &set)) {
            return false;
        }
        if (set == 0) {
            continue;
        }
        out.sets.push_back(set);
        uint32_t index = 0;
        if (!SotcGuest::Read(entry + kEntryIndex, &index)) {
            return false;
        }
        uint64_t mask = 0;
        if (!MaskPointer(ctx + uint64_t{index} * kRecordStride, kind, hi, &mask)) {
            return false;
        }
        if (mask == 0) {
            continue;
        }
        uint16_t count = 0;
        if (!SotcGuest::Read(set + kSetCount, &count)) {
            return false;
        }
        if (count == 0) {
            continue;
        }
        const uint64_t words = (uint64_t{count} + 63) / 64;
        std::vector<uint64_t> bits(words);
        if (!SotcGuest::Read(mask, bits.data(), words * 8)) {
            return false;
        }
        std::vector<uint32_t> member;
        if (out.path) {
            uint64_t list = 0, membership = 0;
            if (!SotcGuest::Read(set + kSetList, &list) || !SotcGuest::Read(list + kListMembership, &membership)) {
                return false;
            }
            member.resize(count);
            if (!SotcGuest::Read(membership, member.data(), uint64_t{count} * 4)) {
                return false;
            }
        }
        for (uint64_t w = 0; w < words; ++w) {
            uint64_t word = bits[w];
            while (word != 0) {
                const unsigned b = static_cast<unsigned>(__builtin_ctzll(word));
                word &= word - 1;
                const uint64_t obj = w * 64 + b;
                if (obj >= count) {
                    break;
                }
                if (!out.path) {
                    ++out.other_bits;
                } else if ((member[obj] >> (kind & 31)) & 1u) {
                    out.drawn.emplace_back(set, static_cast<uint32_t>(obj));
                }
            }
        }
    }
    return true;
}

// Phase of the calling guest thread: the pre-pass job, a G-buffer job, or other.
int CurrentPhase() {
    if (t_in_prepass) {
        return kPhasePre;
    }
    if (t_in_gbuffer) {
        return kPhaseGBuffer;
    }
    return kPhaseOther;
}

// Mask slot of a mask id: 0x10-0x14 have their own slots, any other id shares the last one.
int XprSlot(uint64_t mask) {
    return (mask >= kMaskBase && mask < kMaskBase + kMaskSpan) ? static_cast<int>(mask - kMaskBase)
                                                               : kXprSlots - 1;
}

// Probe A: entry of ResolveSunShadows (0xafcc0) and its twin (0xb0100). rsi = pass.
HookAction OnResolve(GuestRegs& regs, void*) {
    const uint64_t pass = regs.rsi;
    uint64_t ctx = 0, tin = 0;
    uint32_t dword0 = 0;
    const bool ok = SotcGuest::Read(pass + kPassCtx, &ctx) && ctx != 0 &&
                    SotcGuest::Read(ctx + kCtxInput, &tin) && tin != 0 &&
                    SotcGuest::Read(tin + kTsharpOffset, &dword0);
    const uint64_t base = uint64_t{dword0} << 8;
    const uint64_t main = SotcPrepassProbe::main_addr.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_resolve.calls;
    if (!ok) {
        ++g_resolve.tsharp_failed;
    } else if (main != 0 && base == main) {
        ++g_resolve.on_main;
    } else {
        ++g_resolve.other;
        bool seen = false;
        for (uint64_t b : g_other_bases) {
            seen = seen || b == base;
        }
        if (!seen && g_other_bases.size() < kMaxOtherBases) {
            g_other_bases.push_back(base);
        }
    }
    return HookAction::Continue;
}

// Probe B: wrap of the pre-pass job 0xc1860 (the tag is set for the calls it makes).
HookAction OnPrePassJobPre(GuestRegs&, void*) {
    t_in_prepass = true;
    return HookAction::Continue;
}

void OnPrePassJobPost(GuestRegs&, void*) {
    t_in_prepass = false;
}

// Probe C: wrap of the G-buffer jobs 0xc1ce0 and 0xc1fc0 (the tag is set for the calls they make).
HookAction OnGBufferJobPre(GuestRegs&, void*) {
    t_in_gbuffer = true;
    return HookAction::Continue;
}

void OnGBufferJobPost(GuestRegs&, void*) {
    t_in_gbuffer = false;
}

// Probe B: entry of the object pass 0x11d3d0. rsi = container, rcx = list index p.
HookAction OnListPass(GuestRegs& regs, void*) {
    const uint64_t container = regs.rsi;
    const uint32_t p = static_cast<uint32_t>(regs.rcx);
    const bool pre = t_in_prepass && p < kPrePassLists;
    const bool gb = p >= kPrePassLists && p < kPrePassLists + kGBufferLists && container != 0 &&
                    container == g_main_container.load(std::memory_order_relaxed);
    if (!pre && !gb) {
        return HookAction::Continue;
    }
    std::vector<uint64_t> objs;
    const bool ok = ReadListObjects(container, p, objs);
    const uint64_t frame = SotcStats::flip_count.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ok) {
        ++g_lists.list_failed;
        return HookAction::Continue;
    }
    if (frame != g_frame.key) {
        FinalizeFrameLocked();
        g_frame.key = frame;
    }
    g_frame.any = true;
    if (pre) {
        g_main_container.store(container, std::memory_order_relaxed);
        g_frame.pre[p].insert(objs.begin(), objs.end());
    } else {
        g_frame.gb[p - kPrePassLists].insert(objs.begin(), objs.end());
    }
    return HookAction::Continue;
}

// Probe C: entry of BPXPR_RecordMaskedDraws 0x11d7a0. rcx = mask id, rdx = view, rsi = batch vector.
HookAction OnXprMasked(GuestRegs& regs, void*) {
    const int phase = CurrentPhase();
    const uint64_t frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    const uint64_t mask = regs.rcx & 0xffffffffu;
    XprScan scan;
    bool ok = true;
    if (phase != kPhaseOther) {
        ok = ScanXpr(regs.rdx, mask, regs.rsi, scan);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    XprFrameLocked(frame);
    ++g_xf.calls[phase];
    if (phase == kPhaseOther) {
        return HookAction::Continue;
    }
    if (!ok) {
        ++g_xf.read_failed;
        return HookAction::Continue;
    }
    const int slot = XprSlot(mask);
    for (uint64_t s : scan.sets) {
        g_xf.sets[phase].insert(s);
    }
    for (const ObjKey& o : scan.drawn) {
        g_xf.objs[phase][slot].insert(o);
    }
    g_xf.other_bits[phase] += scan.other_bits;
    return HookAction::Continue;
}

// Probe C: counts one call of a draw function in the current phase.
void CountPhaseCall(std::array<uint64_t, kPhases>& counter) {
    const int phase = CurrentPhase();
    const uint64_t frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    XprFrameLocked(frame);
    ++counter[phase];
}

// Probe C: entry of the grass draw 0xdb700 and of the particle thunk 0x10aee0.
HookAction OnGrass(GuestRegs&, void*) {
    CountPhaseCall(g_xf.grass);
    return HookAction::Continue;
}

HookAction OnParticles(GuestRegs&, void*) {
    CountPhaseCall(g_xf.particles);
    return HookAction::Continue;
}

// "a/b/c/d/e/f": one decimal per mask slot, each divided by f.
std::string PerSlot(const std::array<uint64_t, kXprSlots>& v, double f) {
    std::string s;
    for (int i = 0; i < kXprSlots; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, i ? "/%.1f" : "%.1f", static_cast<double>(v[i]) / f);
        s += buf;
    }
    return s;
}

} // namespace

void RegisterPrepassProbe() {
    SotcHooks::Handler resolve;
    resolve.pre = &OnResolve;
    resolve.post = nullptr;
    resolve.user = nullptr;
    SotcHooks::RegisterHandler(kResolveA, resolve);
    SotcHooks::RegisterHandler(kResolveB, resolve);

    SotcHooks::Handler job;
    job.pre = &OnPrePassJobPre;
    job.post = &OnPrePassJobPost;
    job.user = nullptr;
    SotcHooks::RegisterHandler(kJobPrepass, job);

    SotcHooks::Handler list;
    list.pre = &OnListPass;
    list.post = nullptr;
    list.user = nullptr;
    SotcHooks::RegisterHandler(kListPass, list);

    SotcHooks::Handler gbuffer;
    gbuffer.pre = &OnGBufferJobPre;
    gbuffer.post = &OnGBufferJobPost;
    gbuffer.user = nullptr;
    SotcHooks::RegisterHandler(kJobGBufferVert, gbuffer);
    SotcHooks::RegisterHandler(kJobGBufferProbe, gbuffer);

    SotcHooks::Handler xpr;
    xpr.pre = &OnXprMasked;
    xpr.post = nullptr;
    xpr.user = nullptr;
    SotcHooks::RegisterHandler(kXprMasked, xpr);

    SotcHooks::Handler grass;
    grass.pre = &OnGrass;
    grass.post = nullptr;
    grass.user = nullptr;
    SotcHooks::RegisterHandler(kGrass, grass);

    SotcHooks::Handler particles;
    particles.pre = &OnParticles;
    particles.post = nullptr;
    particles.user = nullptr;
    SotcHooks::RegisterHandler(kParticles, particles);
}

void ReportPrepassProbe(std::string& out, double seconds) {
    (void)seconds;
    if (!SotcPrepassProbe::On()) {
        return;
    }
    if (!SotcHooks::IsInstalled(kResolveA) && !SotcHooks::IsInstalled(kResolveB) &&
        !SotcHooks::IsInstalled(kListPass) && !SotcHooks::IsInstalled(kXprMasked) &&
        !SotcHooks::IsInstalled(kGrass) && !SotcHooks::IsInstalled(kParticles)) {
        return;
    }
    ResolveWindow r;
    ListWindow l;
    XprWindow x;
    std::vector<uint64_t> bases;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        FinalizeFrameLocked();
        FinalizeXprLocked();
        r = g_resolve;
        l = g_lists;
        x = g_xw;
        bases = g_other_bases;
        g_resolve = ResolveWindow{};
        g_lists = ListWindow{};
        g_xw = XprWindow{};
    }
    if (r.calls == 0 && l.frames == 0 && x.frames == 0) {
        return;
    }
    if (r.calls != 0 || l.frames != 0) {
        const double f = l.frames ? static_cast<double>(l.frames) : 1.0;
        const uint64_t main = SotcPrepassProbe::main_addr.load(std::memory_order_relaxed);
        char line[700];
        std::snprintf(
            line, sizeof line,
            "SOTC PREPASS PROBE: resolve calls %llu (input == main depth %llu, other %llu), prepass objects "
            "%.1f (per list %.1f/%.1f/%.1f/%.1f/%.1f), gbuffer objects %.1f, prepass-only %.1f (per list "
            "%.1f/%.1f/%.1f/%.1f/%.1f) per frame over %llu frames, main depth 0x%llx\n",
            static_cast<unsigned long long>(r.calls), static_cast<unsigned long long>(r.on_main),
            static_cast<unsigned long long>(r.other), l.pre_union / f,
            l.pre_list[0] / f, l.pre_list[1] / f, l.pre_list[2] / f, l.pre_list[3] / f, l.pre_list[4] / f,
            l.gb_union / f, l.x_union / f, l.x_list[0] / f, l.x_list[1] / f, l.x_list[2] / f,
            l.x_list[3] / f, l.x_list[4] / f, static_cast<unsigned long long>(l.frames),
            static_cast<unsigned long long>(main));
        out += line;
        std::string bl = "SOTC PREPASS PROBE: other resolve input bases (first 8 distinct):";
        for (uint64_t b : bases) {
            char hex[32];
            std::snprintf(hex, sizeof hex, " 0x%llx", static_cast<unsigned long long>(b));
            bl += hex;
        }
        char tail[160];
        std::snprintf(tail, sizeof tail, "; T# unreadable %llu, list reads failed %llu\n",
                      static_cast<unsigned long long>(r.tsharp_failed),
                      static_cast<unsigned long long>(l.list_failed));
        bl += tail;
        out += bl;
    }
    if (x.frames != 0) {
        const double f = static_cast<double>(x.frames);
        char num[64];
        std::string a = "SOTC PREPASS PROBE XPR: drawn per frame by mask 0x10/0x11/0x12/0x13/0x14/other: pre-pass ";
        a += PerSlot(x.pre_slot, f);
        std::snprintf(num, sizeof num, " (union %.1f), G-buffer ", static_cast<double>(x.pre_union) / f);
        a += num;
        a += PerSlot(x.gb_slot, f);
        std::snprintf(num, sizeof num, " (union %.1f), pre-only %.1f (by mask ", static_cast<double>(x.gb_union) / f,
                      static_cast<double>(x.pre_only) / f);
        a += num;
        a += PerSlot(x.pre_only_slot, f);
        a += "); over " + std::to_string(x.frames) + " frames\n";
        out += a;

        char b[512];
        std::snprintf(
            b, sizeof b,
            "SOTC PREPASS PROBE XPR: 0x11d7a0 calls per frame pre %.1f gb %.1f other %.1f; set records per frame "
            "pre %.1f gb %.1f; mask bits on the other dispatch (0x1a7290, not drawn here) per frame pre %.1f gb "
            "%.1f; reads failed %llu\n",
            x.calls[kPhasePre] / f, x.calls[kPhaseGBuffer] / f, x.calls[kPhaseOther] / f,
            x.sets[kPhasePre] / f, x.sets[kPhaseGBuffer] / f, x.other_bits[kPhasePre] / f,
            x.other_bits[kPhaseGBuffer] / f, static_cast<unsigned long long>(x.read_failed));
        out += b;

        char c[512];
        std::snprintf(
            c, sizeof c,
            "SOTC PREPASS PROBE XPR: grass 0xdb700 calls per frame pre %.1f gb %.1f other %.1f; particles "
            "0x10aee0 calls per frame pre %.1f gb %.1f other %.1f\n",
            x.grass[kPhasePre] / f, x.grass[kPhaseGBuffer] / f, x.grass[kPhaseOther] / f,
            x.particles[kPhasePre] / f, x.particles[kPhaseGBuffer] / f, x.particles[kPhaseOther] / f);
        out += c;
    }
}

} // namespace SotcProbes
