// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_tables implementation (Phase 2.10). See native_tables.h for
// the contract summary. Every derivation here is READ-ONLY: it reads the guest's own state (guest
// pointers are host addresses, the "PC memory model") and returns a plan or a static gate reason.
// Nothing in this file calls into the guest, allocates guest memory, or writes guest memory.
#include <cstring>
#include <vector>

#include "common/types.h"
#include "native/guest_bridge.h"
#include "native/native_tables.h"

namespace Native {

namespace {

inline u8 Read8(u64 addr, u64 off) {
    return *reinterpret_cast<const u8*>(addr + off);
}
inline u32 Read32(u64 addr, u64 off) {
    return *reinterpret_cast<const u32*>(addr + off);
}
inline u64 Read64(u64 addr, u64 off) {
    return *reinterpret_cast<const u64*>(addr + off);
}

/// A pointer-sized value that can be a real guest heap object (same envelope as the PlausiblePtr
/// guard in native_cull.cpp / native_bind.cpp). Guest VAs are host pointers here.
inline bool PlausiblePtr(u64 p) {
    return p != 0 && p != ~0ull && p >= 0x10000 && p < 0x800000000000ull;
}

inline u32 PopCount(u32 v) {
    u32 n = 0;
    while (v != 0) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// 0x95320 — root-table builder (docs/engine/native-render-map-pass.md §4.4, bind doc §2.7)
// ---------------------------------------------------------------------------------------------
const char* DeriveRootTable(u32 table_id, u64 root_slot, u64 static_table, RootTablePlan& out) {
    out = {};
    out.table_id = table_id;
    out.root_slot = root_slot;
    out.static_table = static_table;
    // The body tests exactly bits 0..9 (00095320.c:14-52; bit 7 is the sign test on the mask
    // byte). A higher bit set means the mask is not a 0x95320 mask.
    if ((table_id & ~kRootTableMaskMask) != 0) {
        return "root-table mask has bits above 9";
    }
    if (!PlausiblePtr(root_slot)) {
        return "root-table source window implausible";
    }
    // 0x11d3d0 passes ctx+0x90, so the 10 qwords are read from a live guest window. The window is
    // always 10 qwords wide regardless of the mask: the body indexes src[] by bit position.
    out.slot_count = PopCount(table_id);
    out.bytes = out.slot_count * 8;
    out.src_mask = static_cast<u8>(table_id & 0xff);
    out.tail_slot = (table_id & 0x100) != 0; // 00095320.c:46-49 writes src[8]
    out.last_slot = (table_id & 0x200) != 0; // 00095320.c:50-52 writes src[9] last, no cursor step
    // The body's copy switch covers src[0..8] (00095320.c:14-49); the src[9] branch (line 50) is
    // not in the decompiled body as a mask test (`& 0x200`), so bit 9 alone would copy nothing.
    // A mask asking for slot 9 without any of 0..8 is therefore a shape this body does not emit.
    if ((table_id & 0x1ff) == 0) {
        return "root-table mask selects no low slot";
    }
    u32 packed = 0;
    for (u32 i = 0; i < kRootTableSlots; ++i) {
        const u32 bit = kRootTableMaskBits[i];
        if ((table_id & bit) == 0) {
            continue;
        }
        out.src_index[packed] = i;
        out.value[packed] = Read64(root_slot, i * 8);
        ++packed;
    }
    // The packed table is bump-allocated frame memory; its address is the allocator's return, not
    // a function of the mask/source. Left as kGuestAllocation (not statically derivable).
    out.table_va = RootTablePlan::kGuestAllocation;
    out.valid = true;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// 0xae150 — user-data (SGPR) table binder (pass map §4.5, bind doc §2.7)
// ---------------------------------------------------------------------------------------------
const char* DeriveUserDataTable(u64 cb, u64 table, UserDataTablePlan& out) {
    out = {};
    out.cb = cb;
    out.table = table;
    if (!PlausiblePtr(cb)) {
        return "user-data binder context implausible";
    }
    const u64 device = Read64(cb, 0); // *binder = cb (the material block owner)
    out.enable_mask = Read32(cb, 0x18); // pass ctx +0x18: the 6-bit stage-enable mask
    out.stage_table = Read64(cb, 0x20); // pass ctx +0x20: PTR_DAT_01604408 / 01604428
    if ((out.enable_mask & ~0x3fu) != 0) {
        return "user-data enable mask has unknown bits";
    }
    if (out.enable_mask == 0) {
        out.valid = true; // the binder writes nothing (000ae150.c:11-58: every branch masked)
        return nullptr;
    }
    if (!PlausiblePtr(out.stage_table)) {
        return "user-data stage-index table implausible";
    }
    if (!PlausiblePtr(device)) {
        return "user-data binder has no device";
    }
    for (u32 bit = 0; bit < 6; ++bit) {
        const u32 flag = 1u << bit;
        if ((out.enable_mask & flag) == 0) {
            continue;
        }
        UserDataSlot& s = out.slot[out.stage_count];
        s.bit = bit;
        // stage s = *(u32*)(*(binder+0x20) + 4*i) (000ae150.c:13 for i=0, :21 for 1, ...).
        s.stage = Read32(out.stage_table, 4 * bit);
        if (s.stage > 6) { // GNM ordinals 0=CS..6=LS (bind doc §2.4)
            return "user-data stage index out of range";
        }
        // P+0x1d8 + 8*s cache slot (000ae150.c:13 reads binder[(u32)binder[4][i] + 0x3b]).
        s.record = Read64(cb, 0x1d8 + 8 * s.stage);
        s.record_type = PlausiblePtr(s.record) ? Read8(s.record, 0x19) : 0;
        s.written = PlausiblePtr(s.record) && s.record_type == 2;
        if (s.written) {
            // 0x862490: the layout record at material+0xc180+8*stage, u16@+0x10 = {slot, len}.
            const u64 layout = Read64(device + Guest::DeviceOff::MaterialBlock, 0xc180 + 8 * s.stage);
            if (PlausiblePtr(layout)) {
                const u16 packed = static_cast<u16>(Read32(layout, 0x10) & 0xffff);
                s.sgpr = packed & 0xff;
                s.dwords = packed >> 8;
            }
            ++out.written_count;
        }
        ++out.stage_count;
    }
    out.valid = true;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// 0x123810 / 0x123cc0 — instance / object block writer (pass map §4.6, bind doc §2.7)
// ---------------------------------------------------------------------------------------------
namespace {

/// The instance V# qword both functions compute: inst[0] | (inst[1] & 0xfff) << 32 (00123810.c:14,
/// 0x123cc0 forwards the pair at renderable+0x90 into param_5, raw 0x123cc0 `lea r9,[rdi+0x90]`).
constexpr u64 kInstPairDestOffset = 0xa8; // param_2[0x15]
constexpr u32 kInstStride = 0x58;         // lVar6 = index * 0x58 (00123810.c:19)

} // namespace

const char* DeriveInstanceBlock(u64 record, u64 dest, u32 index, bool flag, u64 pair,
                                u64* out_desc, u32* out_stride) {
    (void)record;
    (void)index;
    (void)flag;
    if (out_desc == nullptr || out_stride == nullptr) {
        return "instance block output pointers missing";
    }
    if (!PlausiblePtr(dest)) {
        return "instance block destination implausible";
    }
    if (!PlausiblePtr(pair)) {
        return "instance pair implausible";
    }
    // 0x123810:14 writes the qword regardless of index/flag; the pair is read unconditionally.
    *out_desc = dest;
    *out_stride = kInstStride;
    return nullptr;
}

const char* DeriveInstanceBind(u64 rec2, u64 ctx, u32 index, bool flag, InstanceBindPlan& out) {
    out = {};
    out.record = rec2;
    out.pass = ctx;
    out.index = index;
    out.flag = flag;
    if (!PlausiblePtr(rec2)) {
        return "instance bind renderable implausible";
    }
    if (!PlausiblePtr(ctx)) {
        return "instance bind pass object implausible";
    }
    // 00123810.c:14: the instance V# qword; the pair lives at record+0x90 for 0x123cc0 callers.
    const u64 pair = rec2 + 0x90;
    const u32 lo = Read32(pair, 0);
    const u32 hi = Read32(pair, 4);
    (void)lo;
    (void)hi;
    // 00123810.c:18-19: the per-instance scope/constant block.
    const u64 table = Read64(rec2, 0xf0);
    if (!PlausiblePtr(table)) {
        return "instance bind table implausible";
    }
    out.scope_bind_block = table + static_cast<u64>(index) * kInstStride;
    out.stride = kInstStride;
    // 00123810.c:15-16: optional scope bind from the renderable's +0xb slot.
    out.optional_scope = (Read8(rec2, 0x10) & 0x10) != 0;
    // 00123810.c:13 / :21: the device + the VS-program flag.
    out.cb = Read64(ctx, 0);
    if (!PlausiblePtr(out.cb)) {
        return "instance bind has no device";
    }
    out.vs_flag_clear = (Read8(out.cb, Guest::DeviceOff::End) & 4) == 0;
    // 00123810.c:22 / :28: the index-buffer branch (only when the flag is clear).
    out.index_block = table + 0x20 + static_cast<u64>(index) * kInstStride;
    const u64 index_desc_slot = table + 0x50 + static_cast<u64>(index) * kInstStride;
    const u64 index_desc = PlausiblePtr(out.index_block) ? Read64(index_desc_slot, 0) : 0;
    out.index_buf_desc = PlausiblePtr(index_desc) ? index_desc + 0x28 : 0;
    // 00123810.c:30-44: stencil ref for pass slots k in {2,3,4,5}.
    const u32 pass_slot = Read32(ctx, Guest::ContextOff::PassSlot);
    out.stencil_path = pass_slot >= 2 && pass_slot <= 5;
    const u32 rec_word = Read32(rec2, 0x10);
    out.stencil_word = ((rec_word >> 0x1a) & 0x3c) | ((rec_word >> 0x13) & 0xf) | 0x40;
    // The guest's cache (pass+0x124 front low / +0x129 back low) selects Set vs Skip.
    const u64 cached_front = Read32(ctx, 0x124);
    const u64 cached_back = Read32(ctx, 0x129);
    out.stencil_front = cached_front;
    out.stencil_back = cached_back;
    const u8 word = static_cast<u8>(out.stencil_word);
    const bool stored_flag = Read8(ctx, 0x127) == 1 && Read8(ctx, 0x12b) == 1;
    const bool front_changed = ((rec_word & 1) != 0) && cached_front != word;
    const bool back_changed = cached_back != word;
    out.stencil_write = out.stencil_path && (front_changed || !stored_flag || back_changed);
    out.valid = true;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// 0xca6d0 — per-view draw-table double buffer (000ca6d0.c:47-75; cull doc §0)
// ---------------------------------------------------------------------------------------------
namespace {

void AddCopy(DrawTablesPlan& out, u64 base, u64 src_off, u64 dst_off) {
    if (out.copy_count >= kDrawTableMaxCopies) {
        return;
    }
    out.copy[out.copy_count].dst = base + dst_off;
    out.copy[out.copy_count].src = base + src_off;
    ++out.copy_count;
}

} // namespace

const char* DeriveDrawTables(u64 R, u64 F, u64 ctx, DrawTablesPlan& out) {
    out = {};
    out.R = R;
    out.F = F;
    out.ctx = ctx;
    if (!PlausiblePtr(R)) {
        return "draw-table holder implausible";
    }
    if (!PlausiblePtr(ctx)) {
        return "draw-table job context implausible";
    }
    // 000ca6d0.c:49-51: the three table heads, [n] -> [n+1].
    AddCopy(out, R, 0xe0, 0xe8);
    AddCopy(out, R, 0x100, 0x108);
    AddCopy(out, R, 0x120, 0x128);
    // 000ca6d0.c:52-56: lVar3 = 0x148; dst = *(R + lVar3), src = *(R + lVar3 - 8), stride 0x20,
    // while (lVar3 != 0x6c8).
    for (u64 off = 0x148; off != 0x6c8; off += 0x20) {
        AddCopy(out, R, off - 8, off);
    }
    // 000ca6d0.c:57-74: the fixed tail pairs.
    static constexpr u64 kPairs[][2] = {
        {0x6c0, 0x6c8}, {0x6e0, 0x6e8}, {0x700, 0x708}, {0x720, 0x728},
        {0x740, 0x748}, {0x8e0, 0x8e8}, {0x760, 0x768}, {0x780, 0x788},
        {0x7a0, 0x7a8}, {0x7c0, 0x7c8}, {0x7e0, 0x7e8}, {0x800, 0x808},
        {0x820, 0x828}, {0x840, 0x848}, {0x860, 0x868}, {0x880, 0x888},
        {0x8a0, 0x8a8}, {0x8c0, 0x8c8},
    };
    for (const auto& pair : kPairs) {
        AddCopy(out, R, pair[0], pair[1]);
    }
    out.clear_8f8 = true; // 000ca6d0.c:75 *(u64*)(R+0x8f8) = 0 (after the copy loop)
    // 000ca6d0.c:48 *(u32*)(ctx+0xf4) = 1: the pass-slot/epoch store. It is a ctx write, listed
    // as a flag so the commit path can mirror it below its own gate; it must NOT be performed by
    // the derivation.
    out.epoch_write = true;
    // F is only read by the trailing record-vector loop (000ca6d0.c:100-103); the copy set does not
    // depend on it, so it is recorded but never gated on here.
    out.valid = true;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// 0x11dbe0 — shared dispatch-draw record (0011dbe0.c:115-172; cull doc §2.2)
// ---------------------------------------------------------------------------------------------
const char* DeriveDispatchRecord(u64 rc, bool neo, DispatchRecordPlan& out) {
    out = {};
    out.rc = rc;
    if (!PlausiblePtr(rc)) {
        return "dispatch record context implausible";
    }
    // 0011dbe0.c:115-119: plVar10 = param_3[3] + 0x21e8, or +0x2238 when *(u8*)(param_3[9]+0x1c)
    // is set (Neo). `rc` is ctx[7] (param_3[3]).
    out.record_offset = neo ? 0x2238 : 0x21e8;
    out.record = rc + out.record_offset;
    // lVar5 = *(u64*)record is the view-constant source record (its +0xc4..+0xdc words are copied
    // verbatim at 0011dbe0.c:159-167).
    out.record_owner = Read64(out.record, 0);
    if (!PlausiblePtr(out.record_owner)) {
        return "dispatch record source record implausible";
    }
    // ctx[6] (+0x30) is the current target the frustum floats and dims are read from (0011dbe0.c:81,
    // :142-143); ctx[1] (+0x08) is the XPR entry vector the body walks (0011dbe0.c:67, :170-171).
    out.view_ctx = Read64(rc, 0x30);
    if (!PlausiblePtr(out.view_ctx)) {
        return "dispatch record view context implausible";
    }
    const u64 vectors = Read64(rc, 0x08);
    if (!PlausiblePtr(vectors)) {
        return "dispatch record entry vector implausible";
    }
    const u64 begin = Read64(vectors, 0x08);
    const u64 end = Read64(vectors, 0x10);
    if (!PlausiblePtr(begin) || !PlausiblePtr(end) || end < begin || ((end - begin) % 16) != 0) {
        return "dispatch record entry vector malformed";
    }
    out.batch_count = static_cast<u32>((end - begin) / 16);
    out.args_record = 0; // ctx[0x13] is written by the body (0011dbe0.c:168)
    out.valid = true;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// 0xca150 — shadow XPR block (000ca150.c; cull doc §2.2)
// ---------------------------------------------------------------------------------------------
const char* DeriveShadowBlock(u64 rc, ShadowBlockPlan& out) {
    out = {};
    out.ctx = rc;
    if (!PlausiblePtr(rc)) {
        return "shadow block context implausible";
    }
    // The block is a fresh bump allocation: 00017170(0xae40, DAT_02ed1f58, 8), memset to 0, put at
    // ctx+0x38 (000ca150.c:13-15). Its address is the allocator's; only the content is derived.
    out.block = 0;
    out.allocated = true;
    out.table_zeroed = true;
    const u64 image = Guest::GuestImageBase();
    if (image == 0) {
        return "guest image not mapped";
    }
    // 000ca150.c:16-18: template A (0x370 bytes at image 0x2ede710) lands at block+0xa800, template
    // B (0x2d0 bytes at image 0x2edea80) at block+0xab70 (in this order, after the memset).
    out.template_a = image + kShadowTemplateAVa;
    out.template_b = image + kShadowTemplateBVa;
    // 000ca150.c:19-70: the view pointers come from ctx+0xe8 (lVar4) as a plain address: block+0xadf0
    // = ctx+0xe8+0x68, block+0xadf8 = ctx+0xe8+0x70, lVar1 = ctx+0xe8+0x40 (record name ptrs /
    // destinations), lVar2 = block+0xadf0.
    out.e8 = rc + 0xe8;
    out.adf0 = out.e8 + 0x68;
    out.adf8 = out.e8 + 0x70;
    // 000ca150.c:75: the extra template writes happen only when ctx+0x48 is a live object.
    const u64 config = Read64(rc, 0x48);
    out.ae3c_flag = (config & 1) != 0; // 000ca150.c:75 *(char*)(ctx+0x48)+0x1f
    // Slot records: template A at +0xa800 is 0x370 bytes = 0x16 records of 0x28 bytes, each
    // {name ptr @+0x00, count @+0x08, derived ptr @+0x10, 0 @+0x14, 0 @+0x18, 0 @+0x1c, ...}
    // (raw image words: +0x2ede710 = {0x104786f, 0, 0x1692490, 0}, +0x2ede720 = {8, 0, 0x16924c0,
    // 0}). 0xca120 fills a slot's +0x20 (job id) / +0x24 (telemetry id) / +0x10 (name string);
    // the fill loop writes records i = 0 .. 0xf.
    out.slot_count = kShadowSlotCount;
    for (u32 i = 0; i < kShadowSlotCount; ++i) {
        ShadowSlot& s = out.slot[i];
        s.index = i;
        // Base is expressed relative to the (unknown) allocation: the commit path adds `block`.
        s.base = 0xa800 + static_cast<u64>(kShadowSlotStride) * i;
        s.token_ptr = s.base;          // +0x00: the token/name word pair (0x11d7a0 waits this)
        s.name_ptr = s.base + 0x10;    // +0x10: the scope name (0xca120 stores it here)
        s.aux_a = s.base + 0x14;       // +0x14
        s.aux_b = s.base + 0x18;       // +0x18
        s.job_id_ptr = s.base + 0x20;  // +0x20: the per-cascade job id (0xca120 param_3)
        s.telemetry_ptr = s.base + 0x24; // +0x24: the telemetry id (0xca120 param_4)
    }
    out.valid = true;
    return nullptr;
}

} // namespace Native
