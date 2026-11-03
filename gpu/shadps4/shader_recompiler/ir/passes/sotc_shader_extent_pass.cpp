// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_SHADER_EXTENT. The read extent of read-only buffers whose V# the shader builds itself.
//
// Many SotC compute shaders make a storage buffer descriptor out of a raw pointer: dw0/dw1 come from memory, dw2 is
// the immediate 0xffffffff ("unbounded") and dw3 an immediate too. The host then binds a 64 MiB window for it
// (SHADPS4_UNBOUNDED_BUFFER_MB) and syncs every CPU-dirty page of the window, although the pointer points at a game
// constant block of a few hundred bytes. The shader's loads say how far it reads: this pass bounds the byte address
// of every load of such a binding with a value analysis (sotc_shader_extent_domain.h) and stores the largest end in
// BufferResource::sotc_read_extent (0 = not proven). The host limits the bind to it with SOTC_SHADER_EXTENT=1.
//
// It runs on the final IR, after the last constant propagation and dead code elimination, so the analysed loads and
// addresses are exactly those the SPIR-V is emitted from. A load of binding b at element address A (units of the
// access element: 1, 2, 4 or 8 bytes) reads the bytes [A * unit, A * unit + size) from the V# base; the emitted code
// adds the bind's alignment adjustment itself (Offset()), which the host binding covers. The extent is the maximum of
// A * unit + size over every load of the binding, A over-approximated by the analysis, so no load of the shader can
// read past it. Any load whose address cannot be bounded makes the binding 0. The pass changes no IR.
//
// SOTC_SHADER_EXTENT_LOG=1 prints the verdict once per (shader, binding).

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <fmt/format.h>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/sotc_shader_extent_domain.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

using namespace SotcExtent;

enum WhyKind : u8 {
    WhyNone = 0,
    WhyInput = 1,    ///< a value the analysis does not model (memory, user data, unknown op)
    WhyOverflow = 2, ///< an op whose result range covers every u32
    WhyLoop = 3,     ///< a value that depends on itself (loop Phi)
};

struct Val {
    bool is_flt{};
    Int i;
    Flt f;
};

/// Element size and bytes read of a buffer load; false for any other instruction.
bool LoadAccess(IR::Opcode op, u32& unit, u32& size) {
    switch (op) {
    case IR::Opcode::LoadBufferU8:
        unit = 1, size = 1;
        return true;
    case IR::Opcode::LoadBufferU16:
        unit = 2, size = 2;
        return true;
    case IR::Opcode::LoadBufferU32:
    case IR::Opcode::LoadBufferF32:
    case IR::Opcode::ReadConstBuffer: // the index is in dwords (scalar_memory.cpp)
        unit = 4, size = 4;
        return true;
    case IR::Opcode::LoadBufferU32x2:
    case IR::Opcode::LoadBufferF32x2:
        unit = 4, size = 8;
        return true;
    case IR::Opcode::LoadBufferU32x3:
    case IR::Opcode::LoadBufferF32x3:
        unit = 4, size = 12;
        return true;
    case IR::Opcode::LoadBufferU32x4:
    case IR::Opcode::LoadBufferF32x4:
        unit = 4, size = 16;
        return true;
    case IR::Opcode::LoadBufferU64:
        unit = 8, size = 8;
        return true;
    default:
        return false;
    }
}

bool IsCompositeConstruct(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::CompositeConstructU32x2:
    case IR::Opcode::CompositeConstructU32x3:
    case IR::Opcode::CompositeConstructU32x4:
    case IR::Opcode::CompositeConstructF32x2:
    case IR::Opcode::CompositeConstructF32x3:
    case IR::Opcode::CompositeConstructF32x4:
        return true;
    default:
        return false;
    }
}

bool IsCompositeExtract(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::CompositeExtractU32x2:
    case IR::Opcode::CompositeExtractU32x3:
    case IR::Opcode::CompositeExtractU32x4:
    case IR::Opcode::CompositeExtractF32x2:
    case IR::Opcode::CompositeExtractF32x3:
    case IR::Opcode::CompositeExtractF32x4:
        return true;
    default:
        return false;
    }
}

/// CompositeExtract(CompositeConstruct(...), i) is the i-th constructed value; empty otherwise.
IR::Value ForwardedComponent(const IR::Inst* inst) {
    const IR::Inst* composite = inst->Arg(0).TryInst();
    if (!composite || !IsCompositeConstruct(composite->GetOpcode()) || !inst->Arg(1).IsImmediate()) {
        return {};
    }
    const u32 index = inst->Arg(1).U32();
    return index < composite->NumArgs() ? composite->Arg(index) : IR::Value{};
}

class Evaluator {
public:
    explicit Evaluator(const std::array<u32, 3>* workgroup_size) : wg{workgroup_size} {}

    Int IntOf(const IR::Value& v) {
        Evaluate(v);
        return AsInt(v);
    }

private:
    using Operands = boost::container::small_vector<IR::Value, 4>;

    /// The operands the transfer function of inst reads (nothing for values it does not model).
    static Operands Inputs(const IR::Inst* inst) {
        const IR::Opcode op = inst->GetOpcode();
        Operands out;
        if (IsCompositeExtract(op)) {
            if (const IR::Value fwd = ForwardedComponent(inst); !fwd.IsEmpty()) {
                out.push_back(fwd);
            }
            return out;
        }
        switch (op) {
        case IR::Opcode::SelectU32:
        case IR::Opcode::SelectF32:
            out.push_back(inst->Arg(1));
            out.push_back(inst->Arg(2));
            return out;
        case IR::Opcode::Phi:
        case IR::Opcode::IAdd32:
        case IR::Opcode::ISub32:
        case IR::Opcode::IMul32:
        case IR::Opcode::INeg32:
        case IR::Opcode::IAbs32:
        case IR::Opcode::UDiv32:
        case IR::Opcode::UMod32:
        case IR::Opcode::SDiv32:
        case IR::Opcode::SMod32:
        case IR::Opcode::ShiftLeftLogical32:
        case IR::Opcode::ShiftRightLogical32:
        case IR::Opcode::ShiftRightArithmetic32:
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
        case IR::Opcode::BitwiseNot32:
        case IR::Opcode::BitFieldUExtract:
        case IR::Opcode::BitFieldSExtract:
        case IR::Opcode::SMin32:
        case IR::Opcode::UMin32:
        case IR::Opcode::SMax32:
        case IR::Opcode::UMax32:
        case IR::Opcode::SMinTri32:
        case IR::Opcode::UMinTri32:
        case IR::Opcode::SMaxTri32:
        case IR::Opcode::UMaxTri32:
        case IR::Opcode::SMedTri32:
        case IR::Opcode::UMedTri32:
        case IR::Opcode::SClamp32:
        case IR::Opcode::UClamp32:
        case IR::Opcode::ConvertU32F32:
        case IR::Opcode::ConvertS32F32:
        case IR::Opcode::ConvertF32U32:
        case IR::Opcode::ConvertF32S32:
        case IR::Opcode::ConvertU32U16:
        case IR::Opcode::ConvertU16U32:
        case IR::Opcode::ConvertU32U8:
        case IR::Opcode::ConvertU8U32:
        case IR::Opcode::ConvertS32S8:
        case IR::Opcode::ConvertS32S16:
        case IR::Opcode::BitCastU32F32:
        case IR::Opcode::BitCastF32U32:
        case IR::Opcode::FPAdd32:
        case IR::Opcode::FPSub32:
        case IR::Opcode::FPMul32:
        case IR::Opcode::FPFma32:
        case IR::Opcode::FPNeg32:
        case IR::Opcode::FPAbs32:
        case IR::Opcode::FPFloor32:
        case IR::Opcode::FPCeil32:
        case IR::Opcode::FPTrunc32:
        case IR::Opcode::FPRoundEven32:
        case IR::Opcode::FPFract32:
        case IR::Opcode::FPSaturate32:
        case IR::Opcode::FPClamp32:
        case IR::Opcode::FPMin32:
        case IR::Opcode::FPMax32:
        case IR::Opcode::FPMinTri32:
        case IR::Opcode::FPMaxTri32:
        case IR::Opcode::FPMedTri32:
            for (size_t a = 0; a < inst->NumArgs(); ++a) {
                out.push_back(inst->Arg(a));
            }
            return out;
        default:
            return out; // BitCount32, GetAttributeU32, LaneId, leaves
        }
    }

    /// Post-order walk over the inputs without recursion: an operand still on the walk stack is a cycle (its
    /// value there reads as Top, see AsInt).
    void Evaluate(const IR::Value& root) {
        const IR::Inst* first = root.TryInst();
        if (!first || cache.contains(first)) {
            return;
        }
        struct Frame {
            const IR::Inst* inst;
            Operands inputs;
            size_t next;
        };
        std::vector<Frame> stack;
        stack.push_back({first, Inputs(first), 0});
        on_stack.insert(first);
        while (!stack.empty()) {
            Frame& frame = stack.back();
            if (frame.next < frame.inputs.size()) {
                const IR::Inst* arg = frame.inputs[frame.next++].TryInst();
                if (arg && !cache.contains(arg) && !on_stack.contains(arg)) {
                    Operands inputs = Inputs(arg);
                    stack.push_back({arg, std::move(inputs), 0});
                    on_stack.insert(arg);
                }
                continue;
            }
            const IR::Inst* inst = frame.inst;
            cache[inst] = Compute(inst);
            on_stack.erase(inst);
            stack.pop_back();
        }
    }

    Int AsInt(const IR::Value& v) const {
        if (v.IsImmediate()) {
            switch (v.Type()) {
            case IR::Type::U32:
                return Const(v.U32());
            case IR::Type::U16:
                return Const(v.U16());
            case IR::Type::U8:
                return Const(v.U8());
            default:
                return Top();
            }
        }
        const IR::Inst* inst = v.Inst();
        const auto it = cache.find(inst);
        if (it == cache.end()) {
            Int t = Top(); // on the walk stack: the value depends on itself
            t.why = inst;
            t.why_kind = WhyLoop;
            return t;
        }
        if (it->second.is_flt) {
            Int t = Top();
            t.why = inst;
            t.why_kind = WhyInput;
            return t;
        }
        return it->second.i;
    }

    Flt AsFlt(const IR::Value& v) const {
        if (v.IsImmediate()) {
            return v.Type() == IR::Type::F32 ? FExact(v.F32()) : FTop();
        }
        const IR::Inst* inst = v.Inst();
        const auto it = cache.find(inst);
        if (it == cache.end() || !it->second.is_flt) {
            Flt t = FTop();
            t.why = inst;
            t.why_kind = it == cache.end() ? WhyLoop : WhyInput;
            return t;
        }
        return it->second.f;
    }

    static Int Leaf(const IR::Inst* inst) {
        Int t = Top();
        t.why = inst;
        t.why_kind = WhyInput;
        return t;
    }

    Val Compute(const IR::Inst* inst) {
        Val out;
        const IR::Type type = inst->Type();
        if (type == IR::Type::F32) {
            out.is_flt = true;
            out.f = ComputeFlt(inst);
            if (out.f.top && out.f.why_kind == WhyNone) {
                Blame(inst, out.f.why, out.f.why_kind);
            }
        } else if (type == IR::Type::U32 || type == IR::Type::U16 || type == IR::Type::U8) {
            out.i = ComputeInt(inst);
            if (out.i.IsTop() && out.i.why_kind == WhyNone) {
                Blame(inst, out.i.why, out.i.why_kind);
            }
        } else {
            out.i = Leaf(inst);
        }
        return out;
    }

    /// The reason of a Top result: the first Top operand's reason, else this op widened.
    void Blame(const IR::Inst* inst, const void*& why, u8& why_kind) const {
        const Operands inputs = Inputs(inst);
        for (const IR::Value& v : inputs) {
            const IR::Inst* arg = v.TryInst();
            if (!arg) {
                continue;
            }
            const auto it = cache.find(arg);
            if (it == cache.end()) {
                why = arg;
                why_kind = WhyLoop;
                return;
            }
            const bool top = it->second.is_flt ? it->second.f.top : it->second.i.IsTop();
            const u8 kind = it->second.is_flt ? it->second.f.why_kind : it->second.i.why_kind;
            if (top && kind != WhyNone) {
                why = it->second.is_flt ? it->second.f.why : it->second.i.why;
                why_kind = kind;
                return;
            }
        }
        why = inst;
        why_kind = inputs.empty() ? WhyInput : WhyOverflow;
    }

    Int ComputeInt(const IR::Inst* inst) {
        const IR::Opcode op = inst->GetOpcode();
        const auto I = [&](size_t a) { return AsInt(inst->Arg(a)); };
        const auto F = [&](size_t a) { return AsFlt(inst->Arg(a)); };
        if (IsCompositeExtract(op)) {
            const IR::Value fwd = ForwardedComponent(inst);
            return fwd.IsEmpty() ? Leaf(inst) : AsInt(fwd);
        }
        switch (op) {
        case IR::Opcode::Phi: {
            if (inst->NumArgs() == 0) {
                return Leaf(inst);
            }
            Int r = I(0);
            for (size_t a = 1; a < inst->NumArgs() && !r.IsTop(); ++a) {
                r = Union(r, I(a));
            }
            return r;
        }
        case IR::Opcode::SelectU32:
            return Union(I(1), I(2));
        case IR::Opcode::IAdd32:
            return Add(I(0), I(1));
        case IR::Opcode::ISub32:
            return Sub(I(0), I(1));
        case IR::Opcode::IMul32:
            return Mul(I(0), I(1));
        case IR::Opcode::INeg32:
            return Neg(I(0));
        case IR::Opcode::IAbs32: {
            const Int a = I(0);
            if (a.kind == Int::Kind::Set) {
                return Map(a, [](u32 x) {
                    const s32 s = static_cast<s32>(x);
                    return s < 0 ? 0u - x : x;
                });
            }
            s64 lo, hi;
            SBounds(a, lo, hi);
            if (lo >= 0) {
                return Range(lo, hi);
            }
            if (hi <= 0) {
                return Range(-hi, -lo); // -INT_MIN wraps to INT_MIN: 2^31 is the same residue
            }
            return Range(0, std::max(-lo, hi));
        }
        case IR::Opcode::UDiv32:
            return UDiv(I(0), I(1));
        case IR::Opcode::UMod32:
            return UMod(I(0), I(1));
        case IR::Opcode::SDiv32:
        case IR::Opcode::SMod32: {
            // Only non-negative operands, where they are the unsigned ops.
            s64 alo, ahi, blo, bhi;
            const Int a = I(0), b = I(1);
            SBounds(a, alo, ahi);
            SBounds(b, blo, bhi);
            if (alo < 0 || blo < 1) {
                return Leaf(inst);
            }
            return op == IR::Opcode::SDiv32 ? UDiv(a, b) : UMod(a, b);
        }
        case IR::Opcode::ShiftLeftLogical32:
            return Shl(I(0), I(1));
        case IR::Opcode::ShiftRightLogical32:
            return LShr(I(0), I(1));
        case IR::Opcode::ShiftRightArithmetic32:
            return AShr(I(0), I(1));
        case IR::Opcode::BitwiseAnd32:
            return And(I(0), I(1));
        case IR::Opcode::BitwiseOr32:
            return Or(I(0), I(1));
        case IR::Opcode::BitwiseXor32:
            return Xor(I(0), I(1));
        case IR::Opcode::BitwiseNot32:
            return Not(I(0));
        case IR::Opcode::BitFieldUExtract:
            return BfeU(I(0), I(1), I(2));
        case IR::Opcode::BitFieldSExtract:
            return BfeS(I(0), I(1), I(2));
        case IR::Opcode::BitCount32:
            return Range(0, 32);
        case IR::Opcode::SMin32:
            return SMin(I(0), I(1));
        case IR::Opcode::UMin32:
            return UMin(I(0), I(1));
        case IR::Opcode::SMax32:
            return SMax(I(0), I(1));
        case IR::Opcode::UMax32:
            return UMax(I(0), I(1));
        case IR::Opcode::SMinTri32:
            return SMin(I(0), SMin(I(1), I(2)));
        case IR::Opcode::UMinTri32:
            return UMin(I(0), UMin(I(1), I(2)));
        case IR::Opcode::SMaxTri32:
            return SMax(I(0), SMax(I(1), I(2)));
        case IR::Opcode::UMaxTri32:
            return UMax(I(0), UMax(I(1), I(2)));
        case IR::Opcode::SMedTri32:
            return SMed3(I(0), I(1), I(2));
        case IR::Opcode::UMedTri32:
            return UMed3(I(0), I(1), I(2));
        case IR::Opcode::SClamp32:
            return SClamp(I(0), I(1), I(2));
        case IR::Opcode::UClamp32:
            return UClamp(I(0), I(1), I(2));
        case IR::Opcode::GetAttributeU32: {
            const IR::Attribute attr = inst->Arg(0).Attribute();
            if (wg && attr == IR::Attribute::LocalInvocationId && inst->Arg(1).IsImmediate() &&
                inst->Arg(1).U32() < 3 && (*wg)[inst->Arg(1).U32()] != 0) {
                return Range(0, s64{(*wg)[inst->Arg(1).U32()]} - 1);
            }
            if (wg && attr == IR::Attribute::LocalInvocationIndex) {
                const s64 n = s64{(*wg)[0]} * (*wg)[1] * (*wg)[2];
                return n != 0 ? Range(0, n - 1) : Leaf(inst);
            }
            return Leaf(inst);
        }
        case IR::Opcode::LaneId:
            return Range(0, 127); // a subgroup has at most 128 invocations (Vulkan)
        case IR::Opcode::ConvertU32F32:
            return IntFromF(F(0), false);
        case IR::Opcode::ConvertS32F32:
            return IntFromF(F(0), true);
        case IR::Opcode::BitCastU32F32:
            return BitsFromF(F(0));
        case IR::Opcode::ConvertU32U16:
            return ZeroExtend(I(0), 16);
        case IR::Opcode::ConvertU32U8:
            return ZeroExtend(I(0), 8);
        case IR::Opcode::ConvertU16U32:
            return And(I(0), Const(0xFFFF));
        case IR::Opcode::ConvertU8U32:
            return And(I(0), Const(0xFF));
        case IR::Opcode::ConvertS32S16:
            return SignExtend(ZeroExtend(I(0), 16), 16);
        case IR::Opcode::ConvertS32S8:
            return SignExtend(ZeroExtend(I(0), 8), 8);
        default:
            return Leaf(inst);
        }
    }

    Flt ComputeFlt(const IR::Inst* inst) {
        const IR::Opcode op = inst->GetOpcode();
        const auto F = [&](size_t a) { return AsFlt(inst->Arg(a)); };
        const auto LeafF = [inst] {
            Flt t = FTop();
            t.why = inst;
            t.why_kind = WhyInput;
            return t;
        };
        if (IsCompositeExtract(op)) {
            const IR::Value fwd = ForwardedComponent(inst);
            return fwd.IsEmpty() ? LeafF() : AsFlt(fwd);
        }
        switch (op) {
        case IR::Opcode::Phi: {
            if (inst->NumArgs() == 0) {
                return LeafF();
            }
            Flt r = F(0);
            for (size_t a = 1; a < inst->NumArgs() && !r.top; ++a) {
                r = FUnion(r, F(a));
            }
            return r;
        }
        case IR::Opcode::SelectF32:
            return FUnion(F(1), F(2));
        case IR::Opcode::FPAdd32:
            return FAdd(F(0), F(1));
        case IR::Opcode::FPSub32:
            return FSub(F(0), F(1));
        case IR::Opcode::FPMul32:
            return FMul(F(0), F(1));
        case IR::Opcode::FPFma32:
            return FAdd(FMul(F(0), F(1)), F(2));
        case IR::Opcode::FPNeg32:
            return FNeg(F(0));
        case IR::Opcode::FPAbs32:
            return FAbs(F(0));
        case IR::Opcode::FPFloor32:
            return FRound(F(0), Round::Floor);
        case IR::Opcode::FPCeil32:
            return FRound(F(0), Round::Ceil);
        case IR::Opcode::FPTrunc32:
            return FRound(F(0), Round::Trunc);
        case IR::Opcode::FPRoundEven32:
            return FRound(F(0), Round::Even);
        case IR::Opcode::FPFract32:
            return FFract(F(0));
        case IR::Opcode::FPSaturate32:
            return FClamp(F(0), FExact(0.0f), FExact(1.0f));
        case IR::Opcode::FPClamp32:
            return FClamp(F(0), F(1), F(2));
        case IR::Opcode::FPMin32:
            return FMin(F(0), F(1));
        case IR::Opcode::FPMax32:
            return FMax(F(0), F(1));
        case IR::Opcode::FPMinTri32:
            return FMin(F(0), FMin(F(1), F(2)));
        case IR::Opcode::FPMaxTri32:
            return FMax(F(0), FMax(F(1), F(2)));
        case IR::Opcode::FPMedTri32: {
            const Flt a = F(0), b = F(1), c = F(2);
            return FMax(FMin(a, b), FMin(FMax(a, b), c));
        }
        case IR::Opcode::ConvertF32U32:
            return FFromInt(AsInt(inst->Arg(0)), false);
        case IR::Opcode::ConvertF32S32:
            return FFromInt(AsInt(inst->Arg(0)), true);
        case IR::Opcode::BitCastF32U32:
            return FFromBits(AsInt(inst->Arg(0)));
        default:
            return LeafF();
        }
    }

    const std::array<u32, 3>* wg;
    std::unordered_map<const IR::Inst*, Val> cache;
    std::unordered_set<const IR::Inst*> on_stack;
};

std::string Describe(const void* why, u8 why_kind) {
    const IR::Inst* inst = static_cast<const IR::Inst*>(why);
    if (!inst) {
        return "unknown";
    }
    std::string name{IR::NameOf(inst->GetOpcode())};
    if (inst->GetOpcode() == IR::Opcode::GetAttributeU32) {
        name += " " + IR::NameOf(inst->Arg(0).Attribute());
    }
    switch (why_kind) {
    case WhyOverflow:
        return name + " covers every u32";
    case WhyLoop:
        return name + " depends on itself";
    default:
        return name;
    }
}

struct BindingResult {
    bool candidate{};
    bool unbounded{};
    u64 end{};
    std::string reason;
};

void Analyze(const IR::BlockList& blocks, const std::array<u32, 3>* workgroup_size,
             std::vector<BindingResult>& results) {
    Evaluator eval{workgroup_size};
    for (const IR::Block* block : blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (!IsBufferInstruction(inst)) {
                continue;
            }
            if (!inst.Arg(0).IsImmediate()) {
                // Resource patching leaves an immediate binding in every buffer access; anything else could be any
                // binding.
                for (BindingResult& res : results) {
                    if (res.candidate && !res.unbounded) {
                        res.unbounded = true;
                        res.reason = "buffer access with a runtime binding";
                    }
                }
                continue;
            }
            const u32 binding = inst.Arg(0).U32();
            if (binding >= results.size() || !results[binding].candidate || results[binding].unbounded) {
                continue;
            }
            BindingResult& res = results[binding];
            u32 unit = 0, size = 0;
            if (!LoadAccess(inst.GetOpcode(), unit, size)) {
                // A store, an atomic or a formatted access of a binding marked read-only: not modelled.
                res.unbounded = true;
                res.reason = fmt::format("{} access", IR::NameOf(inst.GetOpcode()));
                continue;
            }
            const Int addr = eval.IntOf(inst.Arg(1));
            u64 lo, hi;
            UBounds(addr, lo, hi);
            if (addr.IsTop() || (addr.kind == Int::Kind::Range && addr.lo < 0)) {
                res.unbounded = true;
                res.reason = addr.IsTop() ? Describe(addr.why, addr.why_kind) : "address may wrap";
                continue;
            }
            const u64 end = hi * unit + size;
            if (end > 0xFFFFFFFFull) {
                res.unbounded = true;
                res.reason = fmt::format("address up to {:#x}", hi);
                continue;
            }
            res.end = std::max(res.end, end);
        }
    }
}

bool LogEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_SHADER_EXTENT_LOG");
        return v && v[0] == '1';
    }();
    return on;
}

/// A guest V# built by the shader with an immediate "unbounded" num_records, read only.
bool IsCandidate(const BufferResource& desc) {
    const auto& fetch = desc.sharp_fetch;
    return desc.buffer_type == BufferType::Guest && !desc.is_written && !desc.is_formatted &&
           fetch.summary != SharpFetch<AmdGpu::Buffer>::Summary::Invalid && (fetch.load_mask & 0x4) == 0 &&
           fetch.immediates[2] >= 0xFFFF0000u;
}

} // Anonymous namespace

u32 SotcBufferReadExtent(const IR::BlockList& blocks, u32 binding, const std::array<u32, 3>* workgroup_size,
                         std::string* reason) {
    std::vector<BindingResult> results(binding + 1);
    results[binding].candidate = true;
    Analyze(blocks, workgroup_size, results);
    const BindingResult& res = results[binding];
    if (reason) {
        *reason = res.unbounded ? res.reason : std::string{};
    }
    return res.unbounded ? 0u : static_cast<u32>(res.end);
}

void SotcShaderExtentPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    Info& info = program.info;
    std::vector<BindingResult> results(info.buffers.size());
    bool any = false;
    for (size_t i = 0; i < info.buffers.size(); ++i) {
        info.buffers[i].sotc_read_extent = 0;
        results[i].candidate = IsCandidate(info.buffers[i]);
        any |= results[i].candidate;
    }
    if (!any) {
        return;
    }
    const bool compute = info.sw_stage == SwStage::Compute;
    Analyze(program.blocks, compute ? &runtime_info.hw.cs.workgroup_size : nullptr, results);

    static std::mutex log_mutex;
    static std::unordered_set<u64> logged;
    for (size_t i = 0; i < results.size(); ++i) {
        const BindingResult& res = results[i];
        if (!res.candidate) {
            continue;
        }
        // A binding without any load reads nothing: 0 keeps the host's default window there too.
        if (!res.unbounded) {
            info.buffers[i].sotc_read_extent = static_cast<u32>(res.end);
        }
        if (!LogEnabled()) {
            continue;
        }
        {
            std::scoped_lock lock{log_mutex};
            if (!logged.insert(info.pgm_hash ^ (u64{i} * 0x9E3779B97F4A7C15ull)).second) {
                continue;
            }
        }
        if (res.unbounded) {
            std::fprintf(stderr, "SOTC SHADER EXTENT: %#llx/b%zu read-only shader-built V#: unbounded (%s)\n",
                         static_cast<unsigned long long>(info.pgm_hash), i, res.reason.c_str());
        } else {
            std::fprintf(stderr, "SOTC SHADER EXTENT: %#llx/b%zu read-only shader-built V#: extent %llu\n",
                         static_cast<unsigned long long>(info.pgm_hash), i,
                         static_cast<unsigned long long>(res.end));
        }
    }
}

} // namespace Shader::Optimization
