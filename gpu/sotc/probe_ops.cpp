// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_OPS_PROBE (docs/re-backend/14-draw-reduction.md, R4 and R6). Observe only.
//
// The two object walks have one shape (decompile and disassembly of 0xc3a50 and 0xc3c70, EU 1.01). Both call
// FUN_0011d3d0(DAT_02edf140, param_2, param_3, param_5, 1):
//   rsi = param_2, the LIST CONTAINER. List p is the 0x20-byte entries from container+0x68+p*0x20 (begin) to
//         +0x70 (end); entry+8 is the object pointer. (The first version of this probe read rdx here: wrong.)
//   rdx = param_3, the pass record ("cmd"). *cmd is the object the walk writes the pass constants into
//         (cmd+0x210, +0x218, +0x21c, +0x224, +0x228, +0x230), and cmd[9] (+0x48) is the settings object.
//   rcx = param_4 (c3c70): the row of the pass-state table, (cmd[3]) + 0x2400 + row*0x50, copied by FUN_00092140.
//   r8  = param_5, the list index p. MainCamera uses 33, 35, 37, 39 and 40.
//   r9  = param_6, the constants pointer: qword at r9, dword at r9+8. MainCamera passes G+0x110, G+0x11c, G+0x128.
//   [rsp+8] at entry = param_7 (c3c70 gives it to FUN_00091e10 as the state argument; it is [rbp+0x10] at 0xc3e12).
// G = *(image + 0x2ee3ce0), the constants block (DAT_02ee3ce0). When *(cmd[9] + 0xa8c) == 0 the walk takes the
// qword and dword from r9 and G+0x11c/0x124/0x128/0x130 (c3c70, 0xc3d85..0xc3e0b); otherwise G+0xf8 and G+0x100.
//
// Hooks:
// OPS_DEPTH_WALK  0xc3a50, entry. Calls from MainCamera (list 37 and 39) and from 0xc4110 and 0xcd680.
// OPS_ALPHA_WALK  0xc3c70, entry (pre). Not a wrap: the walk reads its stack argument through rbp (0xc3e12),
//                 and WrapStackRefusal refuses that. The return address is read from [rsp] here.
// OPS_ALPHA_DONE  0xc3e48, site after the list walk call (the walk is complete; PopScope is next). It pairs with
//                 the pending record of OPS_ALPHA_WALK on the same thread.
// OPS_MAINCAM     0xcaed0, entry. Reads the gates of MainCamera's Depth and Alpha calls and starts a new call
//                 sequence for the pairs below.
// OPS_FOG_*       the six family A wrappers of the fog pass (scope "Fog"). Each call is counted.
//
// Sites. A call's site is its return address minus the image base (the eboot VA of the call instruction + 5).
// MainCamera (0xcaed0) sites, with the list and the gate of each (decompile lines in the comments):
//   BW0  0xcb6f7  block X, flag bit 0 clear: list 33 kRS_Alpha_BeforeWater (gate [ctx+0x588] != [ctx+0x580] and
//                 settings+0x244 != 0)
//   AW1  0xcb7b6  block X, flag bit 0 set: list 35 kRS_Alpha_Normal (same gate)
//   MAIN0 0xcbbd0 flag bit 0 clear, after the water block: list 35 (always)
//   DA   0xcbc70  Depth-gated pair, Alpha A: list 37 kRS_DepthPrepassAlpha_Normal, gate [ctx+0x5e0] != [ctx+0x5e8]
//   DB   0xcbcba  Alpha B, same gate (the walk of list 37 is 0xcbc46 and 0xcbc90)
//   MAIN1 0xcbde9 flag bit 0 set, after the water block: list 33 (always)
//   G620 0xcbfef  list 39 kRS_SpecialCloakEffect, gate [ctx+0x620] != [ctx+0x628] (Depth at 0xcbfc5)
//   TAIL 0xcc035  list 40 kRS_Alpha_AfterParticles (always, after the gate of G620)
// Other callers (c3e90 has no static caller, c4110 has none either) count as OTHER.
//
// The walk emits one draw per list entry: FUN_0011d3d0 calls FUN_001222e0 per entry, except entries before the
// first non-null material. "emitted" below is the walked entry count (an upper bound). The port's
// SotcStats::total_draws is not used: Rasterizer::Draw runs later on the command processor thread, so a delta
// across the guest call would read zero.
//
// Pairs. "Second call" = the second Alpha walk of one MainCamera call (the sequence restarts at OPS_MAINCAM).
// Same objects: the object pointer hashes of the two lists are equal and both lists are non-empty.
// Same consts: the effective constants written to cmd+0x210..0x230 (after the walk) are equal.
// Same target: the pass-state row (cmd[3]+0x2400+row*0x50) hashes equal, and cmd, rsi (the container) and the
// [rsp+8] state argument are equal. The row is the state FUN_00092140 sets; the render target is inside it or
// behind it (not decoded here).
//
// The frame is the guest flip count (SotcStats::flip_count), as the other probes.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "guest_mem.h"
#include "probe_ops.h"
#include "video_core/sotc_stats.h"

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kDepthWalk = "OPS_DEPTH_WALK";
constexpr const char* kAlphaWalk = "OPS_ALPHA_WALK";
constexpr const char* kAlphaDone = "OPS_ALPHA_DONE";
constexpr const char* kMainCamera = "OPS_MAINCAM";
constexpr const char* kFogHooks[] = {"OPS_FOG_A7310", "OPS_FOG_A7470", "OPS_FOG_A75B0",
                                     "OPS_FOG_A78A0", "OPS_FOG_A7EB0", "OPS_FOG_A83F0"};

constexpr uint64_t kGlobalConsts = 0x2ee3ce0;   // DAT_02ee3ce0 (G), image offset
constexpr uint64_t kListArray = 0x68;           // container + 0x68 + p * 0x20 = begin, + 0x70 = end
constexpr uint64_t kListStride = 0x20;
constexpr uint64_t kObjectOffset = 8;           // entry + 8 = object pointer
constexpr uint64_t kMaxEntries = 4096;          // cap per list
constexpr uint64_t kCmdFlags = 0xfc;            // cmd byte: bit 0 selects the water block, bit 1 the 0xbb840 pass
constexpr uint64_t kCmdStateTable = 0x18;       // cmd[3]: the pass-state table base
constexpr uint64_t kCmdSettings = 0x48;         // cmd[9]: the settings object
constexpr uint64_t kStateRows = 0x2400;         // table + row * 0x50
constexpr uint64_t kStateRowSize = 0x50;
constexpr uint64_t kSettingsFlag = 0xa8c;       // settings + 0xa8c: 0 = constants from r9
constexpr uint64_t kSettingsBlockGate = 0x244;  // settings + 0x244: block X needs a non-zero byte
constexpr uint64_t kEffQ0 = 0x210, kEffD0 = 0x218, kEffQ1 = 0x21c, kEffD1 = 0x224, kEffQ2 = 0x228, kEffD2 = 0x230;
constexpr uint64_t kCtxDepthA = 0x5e0, kCtxDepthB = 0x5e8; // MainCamera ctx: gate of the list 37 walks
constexpr uint64_t kCtxTailA = 0x620, kCtxTailB = 0x628;   // gate of the list 39 walks
constexpr uint64_t kCtxBlockA = 0x580, kCtxBlockB = 0x588; // gate of the water block
constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

// The Alpha call sites of MainCamera (return address minus image base).
struct SiteName {
    uint64_t offset;
    const char* name;
};
constexpr SiteName kSites[] = {
    {0xcb6f7 + 5, "BW0"},  {0xcb7b6 + 5, "AW1"},  {0xcbbd0 + 5, "MAIN0"}, {0xcbc70 + 5, "DA"},
    {0xcbcba + 5, "DB"},   {0xcbde9 + 5, "MAIN1"}, {0xcbfef + 5, "G620"}, {0xcc035 + 5, "TAIL"},
};
constexpr int kSiteCount = 8;
constexpr int kOtherSlot = 8;      // any other caller
constexpr int kSlots = 9;
constexpr int kAlphaFrameCap = 4;  // calls kept per MainCamera call for the pairs
constexpr int kKeyCap = 12;

int SlotOf(uint64_t site) {
    for (int i = 0; i < kSiteCount; ++i) {
        if (kSites[i].offset == site) {
            return i;
        }
    }
    return kOtherSlot;
}

const char* const kSlotNames[kSlots] = {"BW0", "AW1", "MAIN0", "DA", "DB", "MAIN1", "G620", "TAIL", "other"};

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

// One Alpha walk: the state at entry (pre) and the effective constants after the walk (done).
struct AlphaCall {
    int slot = kOtherSlot;
    uint64_t site = 0;       // return address minus image base
    uint64_t list = 0;       // p (r8)
    uint64_t container = 0;  // rsi: the list container (the camera/view object of MainCamera)
    uint64_t cmd = 0;        // rdx: the pass record
    uint64_t row = 0;        // rcx: the pass-state row
    uint64_t g = 0;          // G, the constants block (0 when unreadable)
    uint64_t r9 = 0;         // param_6 (raw)
    uint64_t arg7 = 0;       // [rsp+8] (raw)
    uint64_t objs = 0;       // entries walked
    uint64_t obj_hash = 0;
    uint64_t row_hash = 0;
    uint64_t eff_hash = 0;
    int flag = -1;           // *(settings + 0xa8c), -1 unreadable
    bool list_ok = false;
    bool row_ok = false;
    bool eff_ok = false;
};

struct ListInfo {
    uint64_t count = 0;
    uint64_t hash = kFnvBasis;
    bool ok = false;
};

std::mutex g_mutex;
uint64_t g_flip = ~uint64_t{0};

// Sums over the window (reset by ReportOpsProbe).
struct Window {
    uint64_t frames = 0;
    // MainCamera
    uint64_t mc_calls = 0;
    uint64_t mc_depth_open = 0;   // [ctx+0x5e0] != [ctx+0x5e8]
    uint64_t mc_tail_open = 0;    // [ctx+0x620] != [ctx+0x628]
    uint64_t mc_block_open = 0;   // [ctx+0x588] != [ctx+0x580] and settings+0x244 != 0
    uint64_t mc_bit0 = 0;         // cmd flag bit 0 set at entry
    uint64_t mc_bit1 = 0;         // cmd flag bit 1 set at entry
    // Depth walk (list 37 and 39 and the helpers)
    uint64_t depth_calls = 0;
    uint64_t depth_objects = 0;
    uint64_t depth_empty = 0;
    uint64_t depth_failed = 0;
    uint64_t depth_flag_set = 0;
    uint64_t depth_flag_clear = 0;
    // Alpha walk
    uint64_t alpha_calls = 0;     // entries (OPS_ALPHA_WALK)
    uint64_t alpha_done = 0;      // completions (OPS_ALPHA_DONE)
    uint64_t alpha_objects = 0;
    uint64_t alpha_failed = 0;
    uint64_t slot_calls[kSlots] = {};
    uint64_t slot_objects[kSlots] = {};
    uint64_t unknown_site = 0;
    uint64_t first_slot[kSlots] = {};
    uint64_t second_slot[kSlots] = {};
    uint64_t pairs = 0;
    uint64_t pair_same_objects = 0;
    uint64_t pair_same_consts = 0;
    uint64_t pair_same_target = 0;
    uint64_t pair_all = 0;
    uint64_t calls_3plus = 0;     // MainCamera calls with a third Alpha walk
    // Fog
    uint64_t fog_calls = 0;
};
Window g_w;

// Distinct (slot, list, constants offset, state offset, row, flag) of the window, with counts.
struct KeyEntry {
    bool used = false;
    int slot = 0;
    uint64_t list = 0;
    uint64_t r9_off = 0;   // r9 - G (raw r9 when G is unknown)
    uint64_t arg7_off = 0; // [rsp+8] - G (raw when G is unknown)
    uint64_t row = 0;
    int flag = 0;
    uint64_t count = 0;
};
KeyEntry g_keys[kKeyCap];

// Alpha calls of the current MainCamera call, in order. Reset at OPS_MAINCAM.
AlphaCall g_alpha_frame[kAlphaFrameCap];
uint32_t g_alpha_n = 0;

// The pre phase record of the Alpha walk, consumed by OPS_ALPHA_DONE on the same thread.
thread_local AlphaCall t_pending;
thread_local bool t_pending_on = false;

// Starts a new frame when the flip changed. Caller holds g_mutex.
void AdvanceLocked(uint64_t flip) {
    if (flip == g_flip) {
        return;
    }
    g_flip = flip;
    ++g_w.frames;
}

// Count and object hash of list p of the container (rsi of the walks). Reads guest memory only.
ListInfo ReadList(uint64_t container, uint64_t p) {
    ListInfo info;
    uint64_t begin = 0, end = 0;
    const uint64_t slot = container + kListArray + p * kListStride;
    if (container == 0 || !SotcGuest::Read(slot, &begin) || !SotcGuest::Read(slot + 8, &end) || end < begin) {
        return info;
    }
    uint64_t n = (end - begin) / kListStride;
    if (n > kMaxEntries) {
        n = kMaxEntries;
    }
    info.count = n;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t obj = 0;
        if (!SotcGuest::Read(begin + i * kListStride + kObjectOffset, &obj)) {
            info.count = 0;
            info.hash = kFnvBasis;
            return info;
        }
        info.hash = Fnv(info.hash, &obj, sizeof obj);
    }
    info.ok = true;
    return info;
}

// The constants c3a50 copies from param_6: qword at p, dword at p + 8.
struct Consts {
    uint64_t q = 0;
    uint32_t d = 0;
    bool ok = false;
};

Consts ReadConsts(uint64_t p) {
    Consts c;
    c.ok = SotcGuest::Read(p, &c.q) && SotcGuest::Read(p + 8, &c.d, sizeof c.d);
    return c;
}

// *(cmd[9] + 0xa8c), or -1 when unreadable.
int ReadFlag(uint64_t cmd) {
    uint64_t settings = 0;
    unsigned char b = 0;
    if (!SotcGuest::Read(cmd + kCmdSettings, &settings) || settings == 0 ||
        !SotcGuest::Read(settings + kSettingsFlag, &b)) {
        return -1;
    }
    return b;
}

// Hash of the pass-state row the walk copies (cmd[3] + 0x2400 + row * 0x50).
bool ReadRow(uint64_t cmd, uint64_t row, uint64_t* hash) {
    uint64_t table = 0;
    unsigned char bytes[kStateRowSize];
    if (!SotcGuest::Read(cmd + kCmdStateTable, &table) || table == 0 ||
        !SotcGuest::Read(table + kStateRows + row * kStateRowSize, bytes, sizeof bytes)) {
        return false;
    }
    *hash = Fnv(kFnvBasis, bytes, sizeof bytes);
    return true;
}

// Hash of the effective constants the walk wrote into the pass record (after the walk).
bool ReadEffective(uint64_t cmd, uint64_t* hash) {
    uint64_t q0 = 0, q1 = 0, q2 = 0;
    uint32_t d0 = 0, d1 = 0, d2 = 0;
    if (cmd == 0 || !SotcGuest::Read(cmd + kEffQ0, &q0) || !SotcGuest::Read(cmd + kEffD0, &d0, sizeof d0) ||
        !SotcGuest::Read(cmd + kEffQ1, &q1) || !SotcGuest::Read(cmd + kEffD1, &d1, sizeof d1) ||
        !SotcGuest::Read(cmd + kEffQ2, &q2) || !SotcGuest::Read(cmd + kEffD2, &d2, sizeof d2)) {
        return false;
    }
    uint64_t h = kFnvBasis;
    h = Fnv(h, &q0, sizeof q0);
    h = Fnv(h, &d0, sizeof d0);
    h = Fnv(h, &q1, sizeof q1);
    h = Fnv(h, &d1, sizeof d1);
    h = Fnv(h, &q2, sizeof q2);
    h = Fnv(h, &d2, sizeof d2);
    *hash = h;
    return true;
}

// G = *(image + 0x2ee3ce0); 0 when unreadable.
uint64_t ReadGlobalConsts() {
    uint64_t g = 0;
    const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
    if (base == 0 || !SotcGuest::Read(base + kGlobalConsts, &g)) {
        return 0;
    }
    return g;
}

// [rsp] and [rsp+8] of the guest stack. The stack is ordinary memory of the running guest thread.
uint64_t StackWord(uint64_t address) {
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), sizeof v);
    return v;
}

// Adds the call to the key table (caller holds g_mutex).
void AddKeyLocked(const AlphaCall& c) {
    const uint64_t r9_off = c.g ? c.r9 - c.g : c.r9;
    const uint64_t arg7_off = c.g ? c.arg7 - c.g : c.arg7;
    for (KeyEntry& k : g_keys) {
        if (k.used && k.slot == c.slot && k.list == c.list && k.r9_off == r9_off && k.arg7_off == arg7_off &&
            k.row == c.row && k.flag == c.flag) {
            ++k.count;
            return;
        }
    }
    for (KeyEntry& k : g_keys) {
        if (!k.used) {
            k.used = true;
            k.slot = c.slot;
            k.list = c.list;
            k.r9_off = r9_off;
            k.arg7_off = arg7_off;
            k.row = c.row;
            k.flag = c.flag;
            k.count = 1;
            return;
        }
    }
}

// Compares the second Alpha walk of a MainCamera call with the first (caller holds g_mutex).
void ComparePairLocked(const AlphaCall& a, const AlphaCall& b) {
    ++g_w.pairs;
    ++g_w.second_slot[b.slot];
    const bool same_objects = a.list_ok && b.list_ok && a.objs > 0 && b.objs > 0 && a.obj_hash == b.obj_hash;
    const bool same_consts = a.eff_ok && b.eff_ok && a.eff_hash == b.eff_hash;
    const bool same_target = a.row_ok && b.row_ok && a.row_hash == b.row_hash && a.cmd == b.cmd &&
                             a.container == b.container && a.arg7 == b.arg7;
    g_w.pair_same_objects += same_objects;
    g_w.pair_same_consts += same_consts;
    g_w.pair_same_target += same_target;
    g_w.pair_all += same_objects && same_consts && same_target;
}

// Entry of c3a50 (the Depth walk). rsi = container, rdx = cmd, r8 = list p, r9 = param_6.
HookAction OnDepthWalk(GuestRegs& regs, void*) {
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    const ListInfo info = ReadList(regs.rsi, regs.r8 & 0xffffffffu);
    const Consts c = ReadConsts(regs.r9);
    const int flag = ReadFlag(regs.rdx);

    std::lock_guard<std::mutex> lock(g_mutex);
    AdvanceLocked(flip);
    ++g_w.depth_calls;
    if (!info.ok || !c.ok) {
        ++g_w.depth_failed;
    }
    g_w.depth_objects += info.count;
    if (info.count == 0) {
        ++g_w.depth_empty;
    }
    if (flag == 1) {
        ++g_w.depth_flag_set;
    } else if (flag == 0) {
        ++g_w.depth_flag_clear;
    }
    return HookAction::Continue;
}

// Entry of c3c70 (the Alpha walk, pre phase). Keeps the state for OPS_ALPHA_DONE.
HookAction OnAlphaWalk(GuestRegs& regs, void*) {
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    AlphaCall c;
    const uint64_t ret = StackWord(regs.rsp);
    c.arg7 = StackWord(regs.rsp + 8);
    const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
    c.site = ret - base;
    c.slot = SlotOf(c.site);
    c.list = regs.r8 & 0xffffffffu;
    c.container = regs.rsi;
    c.cmd = regs.rdx;
    c.row = regs.rcx & 0xffffffffu;
    c.r9 = regs.r9;
    c.g = ReadGlobalConsts();
    const ListInfo info = ReadList(c.container, c.list);
    c.list_ok = info.ok;
    c.objs = info.count;
    c.obj_hash = info.hash;
    c.row_ok = ReadRow(c.cmd, c.row, &c.row_hash);
    c.flag = ReadFlag(c.cmd);
    t_pending = c;
    t_pending_on = true;

    std::lock_guard<std::mutex> lock(g_mutex);
    AdvanceLocked(flip);
    ++g_w.alpha_calls;
    g_w.alpha_objects += c.objs;
    return HookAction::Continue;
}

// Site after the list walk of c3c70 (pre phase of a site hook). Completes the pending Alpha record.
HookAction OnAlphaDone(GuestRegs&, void*) {
    if (!t_pending_on) {
        return HookAction::Continue;
    }
    t_pending_on = false;
    AlphaCall c = t_pending;
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    c.eff_ok = ReadEffective(c.cmd, &c.eff_hash);

    std::lock_guard<std::mutex> lock(g_mutex);
    AdvanceLocked(flip);
    ++g_w.alpha_done;
    ++g_w.slot_calls[c.slot];
    g_w.slot_objects[c.slot] += c.objs;
    if (c.slot == kOtherSlot) {
        g_w.unknown_site = c.site;
    }
    if (!c.list_ok || !c.row_ok || !c.eff_ok) {
        ++g_w.alpha_failed;
    }
    AddKeyLocked(c);
    if (g_alpha_n == 0) {
        ++g_w.first_slot[c.slot];
    }
    if (g_alpha_n == 1) {
        ComparePairLocked(g_alpha_frame[0], c);
    }
    if (g_alpha_n == 2) {
        ++g_w.calls_3plus;
    }
    if (g_alpha_n < kAlphaFrameCap) {
        g_alpha_frame[g_alpha_n] = c;
    }
    ++g_alpha_n;
    return HookAction::Continue;
}

// Entry of RenderView::MainCamera (0xcaed0): the gates of its walk calls, read before they run.
HookAction OnMainCamera(GuestRegs& regs, void*) {
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    const uint64_t ctx = regs.rdi;
    const uint64_t cmd = regs.rdx;
    uint64_t depth_a = 0, depth_b = 0, tail_a = 0, tail_b = 0, block_a = 0, block_b = 0, settings = 0;
    unsigned char flags = 0;
    unsigned char block_gate = 0;
    SotcGuest::Read(ctx + kCtxDepthA, &depth_a);
    SotcGuest::Read(ctx + kCtxDepthB, &depth_b);
    SotcGuest::Read(ctx + kCtxTailA, &tail_a);
    SotcGuest::Read(ctx + kCtxTailB, &tail_b);
    SotcGuest::Read(ctx + kCtxBlockA, &block_a);
    SotcGuest::Read(ctx + kCtxBlockB, &block_b);
    SotcGuest::Read(cmd + kCmdFlags, &flags);
    if (SotcGuest::Read(cmd + kCmdSettings, &settings) && settings != 0) {
        SotcGuest::Read(settings + kSettingsBlockGate, &block_gate);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    AdvanceLocked(flip);
    ++g_w.mc_calls;
    g_w.mc_depth_open += depth_a != depth_b;
    g_w.mc_tail_open += tail_a != tail_b;
    g_w.mc_block_open += (block_a != block_b) && block_gate != 0;
    g_w.mc_bit0 += (flags & 1) != 0;
    g_w.mc_bit1 += (flags & 2) != 0;
    g_alpha_n = 0;
    return HookAction::Continue;
}

// Entry of a family A wrapper of the fog pass. Counts calls only.
HookAction OnFog(GuestRegs&, void*) {
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    AdvanceLocked(flip);
    ++g_w.fog_calls;
    return HookAction::Continue;
}

} // namespace

void RegisterOpsProbe() {
    SotcHooks::Handler depth;
    depth.pre = &OnDepthWalk;
    depth.post = nullptr;
    depth.user = nullptr;
    SotcHooks::RegisterHandler(kDepthWalk, depth);

    SotcHooks::Handler alpha;
    alpha.pre = &OnAlphaWalk;
    alpha.post = nullptr;
    alpha.user = nullptr;
    SotcHooks::RegisterHandler(kAlphaWalk, alpha);

    SotcHooks::Handler done;
    done.pre = &OnAlphaDone; // a site hook: the pre phase is the only phase
    done.post = nullptr;
    done.user = nullptr;
    SotcHooks::RegisterHandler(kAlphaDone, done);

    SotcHooks::Handler maincam;
    maincam.pre = &OnMainCamera;
    maincam.post = nullptr;
    maincam.user = nullptr;
    SotcHooks::RegisterHandler(kMainCamera, maincam);

    SotcHooks::Handler fog;
    fog.pre = &OnFog;
    fog.post = nullptr;
    fog.user = nullptr;
    for (const char* name : kFogHooks) {
        SotcHooks::RegisterHandler(name, fog);
    }
}

void ReportOpsProbe(std::string& out, double seconds) {
    (void)seconds;
    if (!SotcHooks::IsInstalled(kDepthWalk) && !SotcHooks::IsInstalled(kAlphaWalk) &&
        !SotcHooks::IsInstalled(kMainCamera) && !SotcHooks::IsInstalled(kFogHooks[0])) {
        return;
    }
    Window w;
    KeyEntry keys[kKeyCap];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        w = g_w;
        g_w = Window{};
        std::memcpy(keys, g_keys, sizeof keys);
        std::memset(g_keys, 0, sizeof g_keys);
    }
    if (w.frames == 0) {
        return;
    }
    const double f = static_cast<double>(w.frames);
    const double mc = w.mc_calls ? static_cast<double>(w.mc_calls) : 1.0;
    const double depth_per_call = w.depth_calls ? static_cast<double>(w.depth_objects) / w.depth_calls : 0.0;
    const double alpha_per_call = w.alpha_done ? static_cast<double>(w.alpha_objects) / w.alpha_done : 0.0;
    char buf[1400];

    std::snprintf(buf, sizeof buf,
                  "SOTC OPS PROBE (%llu frames): MainCamera %.2f calls/frame; per call: gate 5e0!=5e8 %.2f, "
                  "620!=628 %.2f, water block %.2f; flag bit0 %.2f bit1 %.2f\n",
                  static_cast<unsigned long long>(w.frames), static_cast<double>(w.mc_calls) / f,
                  static_cast<double>(w.mc_depth_open) / mc, static_cast<double>(w.mc_tail_open) / mc,
                  static_cast<double>(w.mc_block_open) / mc, static_cast<double>(w.mc_bit0) / mc,
                  static_cast<double>(w.mc_bit1) / mc);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "SOTC OPS PROBE: Depth walk 0xc3a50 %.2f calls/frame, %.1f objects/call, empty %llu, "
                  "failed %llu, flag set/clear %llu/%llu\n",
                  static_cast<double>(w.depth_calls) / f, depth_per_call,
                  static_cast<unsigned long long>(w.depth_empty),
                  static_cast<unsigned long long>(w.depth_failed),
                  static_cast<unsigned long long>(w.depth_flag_set),
                  static_cast<unsigned long long>(w.depth_flag_clear));
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "SOTC OPS PROBE: Alpha walk 0xc3c70 %.2f calls/frame, %.1f objects/call (= draws emitted, "
                  "upper bound), %.1f/frame, failed %llu\n",
                  static_cast<double>(w.alpha_done) / f, alpha_per_call,
                  static_cast<double>(w.alpha_objects) / f, static_cast<unsigned long long>(w.alpha_failed));
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "SOTC OPS PROBE: Alpha calls/frame by site: BW0 %.2f AW1 %.2f MAIN0 %.2f DA %.2f DB %.2f "
                  "MAIN1 %.2f G620 %.2f TAIL %.2f other %.2f (last other site 0x%llx)\n",
                  static_cast<double>(w.slot_calls[0]) / f, static_cast<double>(w.slot_calls[1]) / f,
                  static_cast<double>(w.slot_calls[2]) / f, static_cast<double>(w.slot_calls[3]) / f,
                  static_cast<double>(w.slot_calls[4]) / f, static_cast<double>(w.slot_calls[5]) / f,
                  static_cast<double>(w.slot_calls[6]) / f, static_cast<double>(w.slot_calls[7]) / f,
                  static_cast<double>(w.slot_calls[8]) / f, static_cast<unsigned long long>(w.unknown_site));
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "SOTC OPS PROBE: Alpha second call (%llu MainCamera calls with 2+; %llu with 3+): same objects "
                  "%llu, same consts %llu, same target %llu, all three %llu\n",
                  static_cast<unsigned long long>(w.pairs), static_cast<unsigned long long>(w.calls_3plus),
                  static_cast<unsigned long long>(w.pair_same_objects),
                  static_cast<unsigned long long>(w.pair_same_consts),
                  static_cast<unsigned long long>(w.pair_same_target), static_cast<unsigned long long>(w.pair_all));
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "SOTC OPS PROBE: first call sites BW0 %llu AW1 %llu MAIN0 %llu DA %llu DB %llu MAIN1 %llu "
                  "G620 %llu TAIL %llu other %llu; second call sites BW0 %llu AW1 %llu MAIN0 %llu DA %llu DB %llu "
                  "MAIN1 %llu G620 %llu TAIL %llu other %llu\n",
                  static_cast<unsigned long long>(w.first_slot[0]), static_cast<unsigned long long>(w.first_slot[1]),
                  static_cast<unsigned long long>(w.first_slot[2]), static_cast<unsigned long long>(w.first_slot[3]),
                  static_cast<unsigned long long>(w.first_slot[4]), static_cast<unsigned long long>(w.first_slot[5]),
                  static_cast<unsigned long long>(w.first_slot[6]), static_cast<unsigned long long>(w.first_slot[7]),
                  static_cast<unsigned long long>(w.first_slot[8]), static_cast<unsigned long long>(w.second_slot[0]),
                  static_cast<unsigned long long>(w.second_slot[1]), static_cast<unsigned long long>(w.second_slot[2]),
                  static_cast<unsigned long long>(w.second_slot[3]), static_cast<unsigned long long>(w.second_slot[4]),
                  static_cast<unsigned long long>(w.second_slot[5]), static_cast<unsigned long long>(w.second_slot[6]),
                  static_cast<unsigned long long>(w.second_slot[7]), static_cast<unsigned long long>(w.second_slot[8]));
    out += buf;
    std::snprintf(buf, sizeof buf, "SOTC OPS PROBE: fog family A wrappers (a7310 a7470 a75b0 a78a0 a7eb0 a83f0) %.2f calls/frame\n",
                  static_cast<double>(w.fog_calls) / f);
    out += buf;
    for (const KeyEntry& k : keys) {
        if (!k.used) {
            continue;
        }
        std::snprintf(buf, sizeof buf,
                      "SOTC OPS PROBE KEY %s list %llu r9-G %lld arg7-G %lld row %llu flag %d: %llu\n",
                      kSlotNames[k.slot], static_cast<unsigned long long>(k.list),
                      static_cast<long long>(k.r9_off), static_cast<long long>(k.arg7_off),
                      static_cast<unsigned long long>(k.row), k.flag, static_cast<unsigned long long>(k.count));
        out += buf;
    }
}

} // namespace SotcProbes
