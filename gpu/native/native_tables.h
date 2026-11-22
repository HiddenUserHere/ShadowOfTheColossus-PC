// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). Phase 2.10 (native-tables): the table /
// block writers the per-group draw loop (0x11d3d0) and the shadow-view setup (0xca6d0) stage per
// draw. They are the guest's per-draw descriptor payload, instance/index bind block, dispatch-draw
// record and shadow XPR block; all four are built by small engine helpers that write into bump-
// allocated or per-view memory, and every downstream consumer reads them back by content.
//
// This module derives each of them READ-ONLY from the guest's own state (guest_bridge, no guest
// calls, no writes), so the native path can reproduce the exact table a draw would read without
// executing the helper. Each Derive* returns nullptr on success or a static gate-reason string
// explaining why the derivation cannot proceed; the callbacks in native_tables_callbacks.cpp chain
// to the guest body in either case (the helpers still run - the emulated draws need them).
//
// Authority: docs/engine/native-render-map-pass.md §4.4-§4.6, native-render-map-bind.md §2.7,
// native-render-map-cull-camera-shadow.md §0/§2.2, and the decomp corpus cited per function.
#pragma once

#include <array>

#include "common/types.h"
#include "native/guest_bridge.h"

namespace Native {

// -- 0x95320: root-table builder -----------------------------------------------------------
// FUN_00095320(u32 mask, u64* src, u32 tag) (out/re/decomp/0009/00095320.c). Packs the set slots
// of its 10-qword source window into one contiguous table in ascending mask-bit order, bump-
// allocated with 8-byte elements (the popcount*8 allocation, line 12). The table address is the
// allocation pointer (in rax at ret, proven from the raw bytes, bind doc §2.7 / §6 item 1).

/// The number of source qwords 0x95320 can pack (mask bits 0..9; bit 7 is tested as the sign of
/// the byte, 0x80).
constexpr u32 kRootTableSlots = 10;
/// The mask bits 0x95320 tests, ascending (00095320.c:14-52), including the 0x80 sign test.
constexpr u32 kRootTableMaskBits[kRootTableSlots] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80,
                                                     0x100, 0x200};
/// Bits 10..31 of the mask are never tested by the body: a mask with any of them set is not the
/// shape 0x95320 understands.
constexpr u32 kRootTableMaskMask = 0x3ff;

/// What the builder writes for one call (read-only mirror).
struct RootTablePlan {
    u32 table_id{0};   ///< the mask (u32 material+0x468 as passed by 0x11d3d0)
    u64 root_slot{0};  ///< base of the 10-qword source window (0x11d3d0 passes ctx+0x90)
    u64 static_table{0}; ///< the 3rd argument (the arena/tag id, DAT_02ed1f54 in the draw loop)
    u32 slot_count{0}; ///< popcount(mask): qwords the allocation holds
    u32 bytes{0};      ///< slot_count * 8
    u8 src_mask{0};    ///< mask & 0xff: which of the 8 qwords the decompile copies unconditionally
    bool tail_slot{false};  ///< mask bit 0x100: source qword 8 (00095320.c:46-49)
    bool last_slot{false};  ///< mask bit 0x200: source qword 9, copied last and without advancing
                            ///< the write cursor (00095320.c:50-52)
    std::array<u32, kRootTableSlots> src_index{}; ///< src[] index per packed position (ascending)
    std::array<u64, kRootTableSlots> value{};     ///< the qword each packed position receives
    /// The packed table's GPU VA. The guest allocation is frame memory and is NOT named by the
    /// guest's own return value statically (double-buffer + bump allocator); -1 means "the guest's
    /// own allocation, not statically derivable" and is the value every valid plan carries today.
    static constexpr u64 kGuestAllocation = ~0ull;
    u64 table_va{kGuestAllocation};
    bool valid{false};
};

/// Derives the packed root table 0x95320(mask, root_slot, static_table) would build. `root_slot`
/// is the base of the 10-qword window (read-only; every source qword is read). A source value of
/// 0 is legal (the guest copies it verbatim), so it is not gated.
const char* DeriveRootTable(u32 table_id, u64 root_slot, u64 static_table, RootTablePlan& out);

// -- 0xae150: user-data (SGPR) table binder -------------------------------------------------
// FUN_000ae150(long* binder, u64 table) (out/re/decomp/000a/000ae150.c). `binder` is the PASS
// context P (its first qword is the cb): enable mask u32 P+0x18 (6 bits), per-bit stage index
// u32 at *(u32*)(P+0x20) + 4*i, the cached shader record P+0x1d8 + 8*s (with its type byte +0x19
// == 2), then 0x862490(cb+0x2e8, s, &table, 2) writes the SAME 8-byte table address into that
// stage's user-data window.

/// One stage slot the binder would write (mask bit i -> GNM stage s).
struct UserDataSlot {
    u32 bit{0};        ///< enable-mask bit index i (0..5)
    u32 stage{0};      ///< the GNM stage ordinal s = *(u32*)(stage_table + 4*i)
    u64 record{0};     ///< the cached shader record P+0x1d8+8*s
    u8 record_type{0}; ///< the record's type byte (+0x19)
    bool written{false}; ///< record != 0 && record_type == 2 (the binder's own condition)
    u32 sgpr{0};       ///< the layout's SGPR slot (layout record cb+0x2e8+0xc180+8*s, u16@+0x10
                       ///< low byte) when the layout is readable, else 0
    u32 dwords{0};     ///< the write length (u16@+0x10 >> 8): the '2' argument admits 2 dwords
};

/// What the binder writes for one call (read-only mirror).
struct UserDataTablePlan {
    u64 cb{0};                     ///< *binder (the device / material-block owner)
    u64 table{0};                  ///< the root-table address every active stage receives
    u32 enable_mask{0};            ///< u32 P+0x18 (bits 0..5 are live)
    u64 stage_table{0};            ///< *(u64*)(P+0x20): the static per-pass stage-index table
    u32 stage_count{0};            ///< number of set bits in enable_mask (0..6)
    UserDataSlot slot[6]{};        ///< one per set bit, ascending i
    u32 written_count{0};          ///< how many slots pass the record type-2 gate
    bool valid{false};
};

/// Derives what 0xae150(cb, table) would write. `cb` is the pass context P (0xae150's param_1);
/// the enable mask/stage table are read from it, the stage records from P's cache slots.
const char* DeriveUserDataTable(u64 cb, u64 table, UserDataTablePlan& out);

// -- 0x123810 / 0x123cc0: instance / object block writer ------------------------------------
// FUN_00123810(renderable, pass, index, flag, uint* inst) (out/re/decomp/0012/00123810.c), reached
// through the thunk FUN_00123cc0(renderable, pass, index, flag) which forwards the instance pair
// at renderable+0x90 (raw 0x123cc0: lea rax,[rdi+0xB8]; lea r8,[rdi+0x68]; lea r9,[rdi+0x90]).

/// The instance V# descriptor 0x123810 computes (00123810.c:14) and where it lands.
struct InstanceBlockPlan {
    u64 record{0};       ///< param_1: the renderable / draw-record block
    u64 dest{0};         ///< *param_2: the object receiving the qword
    u64 dest_offset{0};  ///< param_2[0x15] = param_2+0xa8
    u64 src_pair{0};     ///< param_5: the uint[2] instance pair
    u32 lo{0};           ///< inst[0]
    u32 hi{0};           ///< inst[1] & 0xfff
    u64 qword{0};        ///< lo | (hi & 0xfff) << 32
    bool valid{false};
};

/// Derives the instance V# descriptor and its destination for 0x123810(record, dest, index, flag,
/// pair). `pair` is the address of the uint[2] instance descriptor (0x123cc0 reads it from
/// renderable+0x90; a direct 0x123810 caller passes its own). `out_desc` receives the destination
/// (param_2) the qword is stored into, `out_stride` the index stride (0x58, 00123810.c:19) used by
/// the per-instance scope bind at record+0xf0+index*0x58.
const char* DeriveInstanceBlock(u64 record, u64 dest, u32 index, bool flag, u64 pair,
                                u64* out_desc, u32* out_stride);

/// The rest of what 0x123810 binds alongside the instance block (read-only view of the branches).
struct InstanceBindPlan {
    u64 record{0};          ///< the renderable
    u64 pass{0};            ///< param_2: the object the instance V# qword lands in (+0xa8)
    u32 index{0};           ///< param_3
    bool flag{0};           ///< param_4 (the char flag: index-buffer source select)
    u64 scope_bind_block{0};///< record+0xf0 + index*0x58: the per-instance scope/constant block
    u32 stride{0};          ///< 0x58 (00123810.c:19)
    bool optional_scope{false}; ///< (*(u8*)(record+0x10) & 0x10) != 0: 0xab310(*(record[0xb]+0x78))
    bool vs_flag_clear{false};  ///< (*(u8*)(cb+8) & 4) == 0 gates the index/material branch
    u64 cb{0};              ///< *pass (the device)
    u64 index_block{0};     ///< record+0x20 + index*0x58 (the 0x95580/0x14d7b0 slot)
    u64 index_buf_desc{0};  ///< *(record+0x50 + index*0x58) + 0x28 (the 0x94c60 argument)
    bool stencil_path{false}; ///< pass slot k = pass+0xf4 in {2,3,4,5} (00123810.c:30)
    u32 stencil_word{0};    ///< (record_u32_0x10 >> 0x1a & 0x3c) | (>> 0x13 & 0xf) | 0x40
    u64 stencil_front{0};   ///< pass+0x124 (the cached front STENCILREFMASK, or the fresh word)
    u64 stencil_back{0};    ///< pass+0x129 (the cached back word, or the fresh word)
    bool stencil_write{false}; ///< the cache-compare the guest performs (00123810.c:33-37)
    bool valid{false};
};

/// Derives the full 0x123810/0x123cc0 write set. `rec2` = param_1 (renderable), `ctx` = param_2
/// (the object receiving the qword), `index` = param_3, `flag` = param_4.
const char* DeriveInstanceBind(u64 rec2, u64 ctx, u32 index, bool flag, InstanceBindPlan& out);

// -- 0xca6d0: per-view draw-table double buffer ---------------------------------------------
// FUN_000ca6d0(R, F, ctx) (out/re/decomp/000c/000ca6d0.c): copies every per-view draw-table
// cursor table [n] -> [n+1] (frame N -> N+1) before MainCamera runs: the three +0x20/window
// copies at +0xe0/+0x100/+0x120 (lines 49-51), the 0x20-stride loop over +0x140..+0x6c0
// (lines 52-56), and the fixed pairs up to +0x8e0/+0x8f8 (lines 57-75).

/// One copied qword: dst = *src.
struct DrawTableCopy {
    u64 dst{0};
    u64 src{0};
};

constexpr u32 kDrawTableMaxCopies = 64;

/// The full copy set of one 0xca6d0 call.
struct DrawTablesPlan {
    u64 R{0};              ///< param_1 (the renderer / per-view holder)
    u64 F{0};              ///< param_2 (the frame object: the record-vector list at +0x1a8/+0x1b0)
    u64 ctx{0};            ///< param_3 (the job ctx; ctx[0] = cb)
    u32 copy_count{0};     ///< number of qword copies in `copy`
    DrawTableCopy copy[kDrawTableMaxCopies]{};
    bool epoch_write{false};   ///< *(u32*)(ctx+0xf4) = 1 (000ca6d0.c:48)
    bool clear_8f8{false};     ///< *(u64*)(R+0x8f8) = 0 (000ca6d0.c:75)
    bool valid{false};
};

/// Derives the 0xca6d0 double-buffer copy set. Read-only: no copy is performed.
const char* DeriveDrawTables(u64 R, u64 F, u64 ctx, DrawTablesPlan& out);

// -- 0x11dbe0: shared dispatch-draw record --------------------------------------------------
// FUN_0011dbe0(G, list, ctx, id) (out/re/decomp/0011/0011dbe0.c): per shadow view id, waits the
// XPR slot at *(ctx+0x38)+0xa800+id*0x28 (RC), binds index/args, then builds the shared 0xa0-byte
// dispatch-draw record from the view constants and stores it at ctx[0x13]. The record base is the
// per-view block RC+0x21e8 (Neo: RC+0x2238, 0011dbe0.c:115-119).

constexpr u32 kPerViewStride = 0x50;  // the per-view record stride (cull doc §4.3)

/// The dispatch-draw record layout (0xa0 bytes, 40 dwords; 0011dbe0.c:121-167). All offsets are
/// dword indices into the record.
struct DispatchRecordPlan {
    u64 rc{0};            ///< ctx[7]: the per-view record block base
    u64 record{0};        ///< rc+0x21e8 (graphics) or rc+0x2238 (Neo, ctx[9]+0x1c != 0)
    u64 record_offset{0}; ///< 0x21e8 / 0x2238
    u64 record_owner{0};  ///< *(record): copied into the view-constant source record (local_b0)
    u64 view_ctx{0};      ///< *(ctx+0x30): the current target (frustum floats +0x31..+0x3c,
                          ///< dims at +0/+4 via 0x96f50/0x96f60) once the caller fills it
    u64 args_record{0};   ///< *(ctx+0x98): ctx[0x13] after the store (0 before the body runs)
    u32 id{0};            ///< param_4 & 0xffffffff
    u32 batch_count{0};   ///< (ctx[1]+0x10 - *(ctx[1]+0x8)) >> 4: XPR entries the body walks
    bool valid{false};
};

/// Derives the RC+0x21e8 (Neo +0x2238) shared dispatch record for one 0x11dbe0 call.
const char* DeriveDispatchRecord(u64 rc, bool neo, DispatchRecordPlan& out);

// -- 0xca150: shadow XPR block --------------------------------------------------------------
// FUN_000ca150(ctx) (out/re/decomp/000c/000ca150.c): bump-allocates a zeroed 0xae40-byte block,
// copies the 0x370-byte template at image DAT_02ede710 into +0xa800 and the 0x2d0-byte template
// at DAT_02edea80 into +0xab70, then fills per-slot triples at +0xa800+0x28*i (token, name, ...)
// and the view pointers at +0xadf0/+0xadf8 from ctx+0xe8.

constexpr u32 kShadowBlockBytes = 0xae40;
constexpr u32 kShadowTemplateA = 0x370;  ///< DAT_02ede710 -> +0xa800
constexpr u32 kShadowTemplateB = 0x2d0;  ///< DAT_02edea80 -> +0xab70
constexpr u32 kShadowSlotStride = 0x28;  ///< per-slot record stride in the XPR block
constexpr u32 kShadowSlotCount = 16;     ///< records at +0xa800+0x28*i: i = 0..0xf (0x370/0x28)
constexpr VAddr kShadowTemplateAVa = 0x2ede710; ///< image-anchored
constexpr VAddr kShadowTemplateBVa = 0x2edea80; ///< image-anchored

/// One per-slot record in the XPR block (0x28 bytes, template A at +0xa800; raw template words:
/// +0x00 = name pointer, +0x08 = count, +0x10 = derived pointer, +0x1c = 0).
struct ShadowSlot {
    u32 index{0};
    u64 base{0};          ///< 0xa800 + 0x28*i, relative to the (unknown) allocation
    u64 token_ptr{0};     ///< base + 0x00: the token/name word 0x11d7a0 spins on
    u64 name_ptr{0};      ///< base + 0x10: the scope name (0xca120 writes it)
    u64 aux_a{0};         ///< base + 0x14
    u64 aux_b{0};         ///< base + 0x18
    u64 job_id_ptr{0};    ///< base + 0x20: the per-cascade job id (0xca120 param_3)
    u64 telemetry_ptr{0}; ///< base + 0x24: the telemetry id (0xca120 param_4)
};

struct ShadowBlockPlan {
    u64 ctx{0};           ///< param_1 (the pass context the block pointers are read from)
    u64 e8{0};            ///< ctx+0xe8 (the per-view draw-table cursor source)
    u64 block{0};         ///< the allocated block VA (0 when the guest's slot does not hold one;
                          ///< the allocation itself is not statically nameable, see `allocated`)
    bool allocated{false};///< the block is a fresh 0xae40-byte bump allocation per call
    u64 template_a{0};    ///< image + 0x2ede710 (0 if the image is not mapped)
    u64 template_b{0};    ///< image + 0x2edea80
    u32 template_a_bytes{kShadowTemplateA};
    u32 template_b_bytes{kShadowTemplateB};
    bool table_zeroed{false}; ///< the allocation is memset to 0 first (000ca150.c:14)
    u32 template_word_ae34{2}; ///< *(u32*)(block+0xae34) = 2 (000ca150.c:67)
    u32 ae28_word{5};          ///< *(u32*)(block+0xae28) = 5 (000ca150.c:74)
    bool ae3c_flag{false};     ///< *(u8*)(block+0xae3c) = 1 when bit 0x80 of the byte at
                               ///< ctx+0x48 is set (000ca150.c:75 `*(char*)(*(long*)(ctx+0x48)+0x1f)`,
                               ///< i.e. the last byte of the qword at ctx+0x48)
    u64 adf0{0};          ///< *(u64*)(block+0xadf0) = ctx+0xe8 + 0x68
    u64 adf8{0};          ///< *(u64*)(block+0xadf8) = ctx+0xe8 + 0x70
    ShadowSlot slot[kShadowSlotCount]{};
    u32 slot_count{0};
    bool valid{false};
};

/// Derives the block layout 0xca150 builds: template placement, the per-slot triples and the
/// view-pointer writes. Read-only (the image templates are read through GuestImageBase()).
const char* DeriveShadowBlock(u64 rc, ShadowBlockPlan& out);

} // namespace Native
