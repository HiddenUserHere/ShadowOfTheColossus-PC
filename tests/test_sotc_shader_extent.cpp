// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_SHADER_EXTENT. The read extent analysis (gpu/shadps4/shader_recompiler/ir/passes/
// sotc_shader_extent_pass.cpp) on its value domain and on hand-built IR, without a window or the game:
//   ninja -C out/gpu shader-extent-test && out/gpu/shader-extent-test.exe
#include <cstdio>
#include <string>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/sotc_shader_extent_domain.h"

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace Shader;
namespace X = Shader::Optimization::SotcExtent;

static bool UIs(const X::Int& v, u64 lo, u64 hi) {
    u64 l, h;
    X::UBounds(v, l, h);
    return !v.IsTop() && l == lo && h == hi;
}

static void DomainTests() {
    // Wrapping arithmetic: x + (-4) wraps, adding 4 back is exact again.
    const X::Int r = X::Range(0, 10);
    const X::Int minus4 = X::Add(r, X::Const(0xFFFFFFFCu));
    CHECK(!minus4.IsTop());
    CHECK(UIs(X::Add(minus4, X::Const(4)), 0, 10));
    // Exact sets over Select arms: {-1, 1} * 4 + 8 = {4, 12}, which an interval would lose.
    const X::Int sel = X::Union(X::Const(0xFFFFFFFFu), X::Const(1));
    const X::Int set = X::Add(X::Mul(sel, X::Const(4)), X::Const(8));
    CHECK(set.kind == X::Int::Kind::Set && set.count == 2 && set.vals[0] == 4 && set.vals[1] == 12);
    // Masks, extracts and clamps bound anything.
    CHECK(UIs(X::And(X::Top(), X::Const(0xFF)), 0, 0xFF));
    CHECK(UIs(X::LShr(X::Top(), X::Const(24)), 0, 0xFF));
    CHECK(UIs(X::BfeU(X::Top(), X::Const(8), X::Const(8)), 0, 0xFF));
    CHECK(UIs(X::UMin(X::Top(), X::Const(100)), 0, 100));
    CHECK(UIs(X::SClamp(X::Top(), X::Const(0), X::Const(31)), 0, 31));
    CHECK(!UIs(X::SMin(X::Top(), X::Const(31)), 0, 31)); // negative values (huge as u32) remain possible
    CHECK(X::BfeU(X::Top(), X::Const(30), X::Const(8)).IsTop()); // offset + count > 32: undefined
    CHECK(UIs(X::Shl(X::Range(0, 0x3FFFFFFF), X::Const(2)), 0, 0xFFFFFFFC));
    CHECK(X::Shl(X::Range(0, 0x7FFFFFFF), X::Const(2)).IsTop()); // wraps around more than once
    CHECK(X::Mul(X::Top(), X::Const(4)).IsTop());
    CHECK(UIs(X::BfeS(X::Range(0, 0x7F), X::Const(0), X::Const(8)), 0, 0x7F));
    // A set wider than kMaxSet becomes its covering interval.
    X::Int wide = X::Const(0);
    for (u32 i = 1; i < 100; ++i) {
        wide = X::Union(wide, X::Const(i * 3));
    }
    CHECK(wide.kind == X::Int::Kind::Range && UIs(wide, 0, 297));
    // Floats: fract of anything is [0, 1] or NaN; floor(256 * fract(x)) converts to [0, 256] (NaN -> 0).
    const X::Flt fr = X::FFract(X::FTop());
    const X::Int idx = X::IntFromF(X::FRound(X::FMul(fr, X::FExact(256.0f)), X::Round::Floor), false);
    CHECK(UIs(idx, 0, 256));
    CHECK(UIs(X::LShr(idx, X::Const(2)), 0, 64));
    // Constant float folding and bit casts are exact.
    CHECK(X::BitsFromF(X::FExact(1.0f)).IsConst() && X::BitsFromF(X::FExact(1.0f)).vals[0] == 0x3F800000u);
    const X::Flt prod = X::FMul(X::FExact(3.0f), X::FExact(0.5f));
    CHECK(prod.IsExact() && prod.vals[0] == 1.5f);
    const X::Flt third = X::FMul(X::FExact(1.0f), X::FExact(1.0f / 3.0f));
    CHECK(third.IsExact());
    const X::Flt inexact = X::FAdd(X::FExact(1.0f), X::FExact(1e-9f)); // rounds: not exact any more
    CHECK(!inexact.IsExact() && inexact.lo <= 1.0 && inexact.hi >= 1.0 + 1e-9);
    CHECK(X::BitsFromF(X::FFromBits(X::Const(0x42000000u))).vals[0] == 0x42000000u);
    CHECK(X::IntFromF(X::FExact(37.9f), false).vals[0] == 37);
    // NaN is Top for everything but fract and the conversions.
    CHECK(X::FClamp(fr, X::FExact(0.0f), X::FExact(1.0f)).top);
}

static IR::BufferInstInfo NoInfo() {
    return {};
}

static void IrTests() {
    const std::array<u32, 3> wg{64, 1, 1};
    std::string reason;

    // 0xfeaef49e binding 7 shape: dword index = (floor(256 * fract(f)) >> 2) + (((ud >> 24) & 0xff) >> 2) +
    // (lid.x & 3) + 240, written as a byte offset shifted back to dwords (CalculateBufferAddress).
    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        const IR::F32 f = ir.BitCast<IR::F32>(ir.GetUserData(IR::ScalarReg::S0));
        const IR::U32 t1 = ir.ShiftRightLogical(
            ir.ConvertFToU(32, ir.FPFloor(ir.FPMul(ir.FPFract(f), ir.Imm32(256.0f)))), ir.Imm32(2));
        const IR::U32 ud = ir.GetUserData(IR::ScalarReg::S1);
        const IR::U32 t2 = ir.ShiftRightLogical(
            ir.BitwiseAnd(ir.ShiftRightLogical(ud, ir.Imm32(24)), ir.Imm32(0xFF)), ir.Imm32(2));
        const IR::U32 t3 = ir.BitwiseAnd(ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0), ir.Imm32(3));
        const IR::U32 index = ir.IAdd(ir.IAdd(ir.IAdd(t1, t2), t3), ir.Imm32(240));
        const IR::U32 addr = ir.ShiftRightLogical(ir.IMul(index, ir.Imm32(4)), ir.Imm32(2));
        (void)ir.LoadBufferU32(1, ir.Imm32(7), addr, NoInfo());
        // Another load of the same binding at a constant dword.
        (void)ir.LoadBufferU32(4, ir.Imm32(7), ir.Imm32(12), NoInfo());
        // A load of another binding with an unknown address does not matter for binding 7.
        (void)ir.LoadBufferU32(1, ir.Imm32(3), ud, NoInfo());
        IR::BlockList blocks{&block};
        // max index 64 + 63 + 3 + 240 = 370: bytes [0, 371 * 4)
        CHECK(Optimization::SotcBufferReadExtent(blocks, 7, &wg, &reason) == 1484);
        CHECK(Optimization::SotcBufferReadExtent(blocks, 3, &wg, &reason) == 0);
        CHECK(reason == "GetUserData");
        // Without a workgroup size (not compute), LocalInvocationId is unknown but masked by & 3 anyway.
        CHECK(Optimization::SotcBufferReadExtent(blocks, 7, nullptr, &reason) == 1484);
    }

    // u32 chains over Select arms: (sel ? -1 : 1) * 4 + 8 as a byte offset, read as a vec4.
    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        const IR::U1 cond = ir.IEqual(ir.GetUserData(IR::ScalarReg::S0), ir.Imm32(0));
        const IR::U32 sel{ir.Select(cond, ir.Imm32(0xFFFFFFFFu), ir.Imm32(1))};
        const IR::U32 bytes = ir.IAdd(ir.IMul(sel, ir.Imm32(4)), ir.Imm32(8));
        (void)ir.LoadBufferF32(4, ir.Imm32(5), ir.ShiftRightLogical(bytes, ir.Imm32(2)), NoInfo());
        IR::BlockList blocks{&block};
        CHECK(Optimization::SotcBufferReadExtent(blocks, 5, &wg, &reason) == (3 + 4) * 4);
    }

    // Integers kept in float registers (0x6d3136c5 shape): a Select of float-typed bit patterns, one of them a NaN
    // pattern (-1) and one a denormal (2), cast back to u32: moves keep the bits, so the set stays exact.
    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        const IR::U1 cond = ir.IEqual(ir.GetUserData(IR::ScalarReg::S0), ir.Imm32(0));
        const IR::F32 minus1 = ir.BitCast<IR::F32>(ir.Imm32(0xFFFFFFFFu));
        const IR::F32 sel{ir.Select(cond, minus1, ir.Imm32(0x1p-148f))};
        const IR::U32 bits = ir.BitCast<IR::U32>(sel);
        const IR::U32 addr = ir.IAdd(ir.IMul(bits, ir.Imm32(4)), ir.Imm32(60)); // {56, 68}
        (void)ir.LoadBufferU32(1, ir.Imm32(5), addr, NoInfo());
        IR::BlockList blocks{&block};
        CHECK(Optimization::SotcBufferReadExtent(blocks, 5, &wg, &reason) == 68 * 4 + 4);
        // The same bits through arithmetic lose them: fadd(NaN pattern, 0) is a NaN of unknown bits.
        Common::ObjectPool<IR::Inst> pool2;
        IR::Block block2(pool2);
        IR::IREmitter ir2(block2);
        const IR::U1 cond2 = ir2.IEqual(ir2.GetUserData(IR::ScalarReg::S0), ir2.Imm32(0));
        const IR::F32 sel2{ir2.Select(cond2, ir2.BitCast<IR::F32>(ir2.Imm32(0xFFFFFFFFu)), ir2.Imm32(1.0f))};
        const IR::U32 bits2 = ir2.BitCast<IR::U32>(IR::F32{ir2.FPAdd(sel2, ir2.Imm32(0.0f))});
        (void)ir2.LoadBufferU32(1, ir2.Imm32(5), bits2, NoInfo());
        IR::BlockList blocks2{&block2};
        CHECK(Optimization::SotcBufferReadExtent(blocks2, 5, &wg, &reason) == 0);
    }

    // Loops: a counter masked inside the loop is bounded; a free counter is not.
    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block entry(pool);
        IR::Block loop(pool);
        IR::IREmitter ir_entry(entry);
        ir_entry.Prologue();
        IR::Inst* masked = &*loop.PrependNewInst(loop.begin(), IR::Opcode::Phi);
        masked->SetFlags(IR::Type::U32);
        IR::Inst* counter = &*loop.PrependNewInst(loop.begin(), IR::Opcode::Phi);
        counter->SetFlags(IR::Type::U32);
        IR::IREmitter ir(loop);
        const IR::U32 next_masked = ir.BitwiseAnd(ir.IAdd(IR::U32{IR::Value{masked}}, ir.Imm32(1)), ir.Imm32(15));
        const IR::U32 next_counter = ir.IAdd(IR::U32{IR::Value{counter}}, ir.Imm32(1));
        masked->AddPhiOperand(&entry, ir.Imm32(0));
        masked->AddPhiOperand(&loop, next_masked);
        counter->AddPhiOperand(&entry, ir.Imm32(0));
        counter->AddPhiOperand(&loop, next_counter);
        (void)ir.ReadConstBuffer(ir.Imm32(2), IR::U32{IR::Value{masked}}, NoInfo());
        (void)ir.ReadConstBuffer(ir.Imm32(4), next_counter, NoInfo());
        IR::BlockList blocks{&entry, &loop};
        CHECK(Optimization::SotcBufferReadExtent(blocks, 2, &wg, &reason) == 16 * 4);
        CHECK(Optimization::SotcBufferReadExtent(blocks, 4, &wg, &reason) == 0);
        std::fprintf(stderr, "loop counter: unbounded (%s)\n", reason.c_str());
        CHECK(reason.find("depends on itself") != std::string::npos);
    }

    // A store to a binding makes it unbounded (the pass only takes read-only bindings anyway).
    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        ir.StoreBufferU32(1, ir.Imm32(1), ir.Imm32(0), ir.Imm32(5), NoInfo());
        IR::BlockList blocks{&block};
        CHECK(Optimization::SotcBufferReadExtent(blocks, 1, &wg, &reason) == 0);
    }
}

int main() {
    DomainTests();
    IrTests();
    if (failures == 0) {
        std::printf("shader extent: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
