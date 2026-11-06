// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_NATIVE_TRACK_EMU (fault_emu.h).
#include "fault_emu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../cpu_patches.h"
#include "../hooks.h" // GuestRegs (the store hook stub's frame)
#include "common/decoder.h"
#include "native.h"
#include "video_core/sotc_trace.h" // ImageBase (report offsets)

extern "C" const unsigned char* runtime_memory_backing_pointer(uintptr_t address);

#ifdef _WIN32
#include <windows.h>
#endif

namespace SotcFaultEmu {

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_NATIVE_TRACK_EMU");
        return v && v[0] == '1' ? 1 : (v && v[0] == '2' ? 2 : 0);
    }();
    return mode;
}

bool On() {
    return Mode() != 0 && SotcNative::On() && SotcNative::TrackMode() == 3;
}

namespace {
std::atomic<GpuQuery> g_gpu_query{nullptr};
std::atomic<TickQuery> g_tick_query{nullptr};
std::atomic<void*> g_tick_ctx{nullptr};
} // namespace

void SetGpuQuery(GpuQuery query) {
    g_gpu_query.store(query, std::memory_order_release);
}

void SetTickQuery(TickQuery query, void* ctx) {
    g_tick_ctx.store(ctx, std::memory_order_release);
    g_tick_query.store(query, std::memory_order_release);
}

#ifdef _WIN32
namespace {

constexpr uint64_t kGuestPage = 16 * 1024; // the backing view is contiguous inside one guest page
constexpr uint32_t kArithFlags = 0x8D5;     // CF PF AF ZF SF OF
constexpr uint32_t kIncDecFlags = 0x8D4;    // inc/dec keep CF

// ------------------------------------------------------------------------------------------------ statistics
struct Stats {
    std::atomic<uint64_t> emulated{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> hooked{0}; // stores done by the live hooks
    std::mutex mu;
    std::unordered_map<uint64_t, uint64_t> sites;    // RIP offset -> emulated count (whole run)
    std::unordered_map<uint64_t, uint64_t> refusals; // RIP offset -> refused count (window)
    std::unordered_map<uint64_t, const char*> refusal_reason;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
Stats& S() {
    static Stats s;
    return s;
}

void Report(Stats& s) { // under s.mu
    const auto t = std::chrono::steady_clock::now();
    if (t - s.last < std::chrono::seconds(5)) {
        return;
    }
    s.last = t;
    const auto top = [](const std::unordered_map<uint64_t, uint64_t>& m) {
        std::vector<std::pair<uint64_t, uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        v.resize(std::min<size_t>(v.size(), 8));
        return v;
    };
    std::string line;
    for (const auto& [o, n] : top(s.sites)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " %#llx=%llu", static_cast<unsigned long long>(o),
                      static_cast<unsigned long long>(n));
        line += buf;
    }
    std::string refused;
    for (const auto& [o, n] : top(s.refusals)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " %#llx=%llu(%s)", static_cast<unsigned long long>(o),
                      static_cast<unsigned long long>(n), s.refusal_reason[o]);
        refused += buf;
    }
    std::fprintf(stderr,
                 "SOTC FAULT EMU (5 s): %llu emulated at faults, %llu merged, %llu done by live hooks; %zu sites:%s; "
                 "not emulated:%s\n",
                 static_cast<unsigned long long>(s.emulated.exchange(0)),
                 static_cast<unsigned long long>(s.refused.exchange(0)),
                 static_cast<unsigned long long>(s.hooked.exchange(0)), s.sites.size(), line.c_str(),
                 refused.c_str());
    s.refusals.clear();
}

/// Counts one fault; returns the emulated count of the site so far (0 when refused).
uint64_t Note(uint64_t rip, bool emulated, const char* reason) {
    auto& s = S();
    (emulated ? s.emulated : s.refused).fetch_add(1, std::memory_order_relaxed);
    const uint64_t off = rip - SotcTrace::ImageBase();
    std::scoped_lock lock{s.mu};
    uint64_t count = 0;
    if (emulated) {
        count = ++s.sites[off];
    } else {
        ++s.refusals[off];
        s.refusal_reason[off] = reason;
    }
    Report(s);
    return count;
}

// ------------------------------------------------------------------------------------------------ registers
/// Registers of a write fault (the exception's CONTEXT).
struct ContextRegs {
    CONTEXT* c;
    DWORD64* Slot(ZydisRegister reg) const {
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
        case ZYDIS_REGISTER_RAX: return &c->Rax;
        case ZYDIS_REGISTER_RCX: return &c->Rcx;
        case ZYDIS_REGISTER_RDX: return &c->Rdx;
        case ZYDIS_REGISTER_RBX: return &c->Rbx;
        case ZYDIS_REGISTER_RSP: return &c->Rsp;
        case ZYDIS_REGISTER_RBP: return &c->Rbp;
        case ZYDIS_REGISTER_RSI: return &c->Rsi;
        case ZYDIS_REGISTER_RDI: return &c->Rdi;
        case ZYDIS_REGISTER_R8: return &c->R8;
        case ZYDIS_REGISTER_R9: return &c->R9;
        case ZYDIS_REGISTER_R10: return &c->R10;
        case ZYDIS_REGISTER_R11: return &c->R11;
        case ZYDIS_REGISTER_R12: return &c->R12;
        case ZYDIS_REGISTER_R13: return &c->R13;
        case ZYDIS_REGISTER_R14: return &c->R14;
        case ZYDIS_REGISTER_R15: return &c->R15;
        default: return nullptr;
        }
    }
    const void* Xmm(ZydisRegister reg) const {
        if (reg < ZYDIS_REGISTER_XMM0 || reg > ZYDIS_REGISTER_XMM15) {
            return nullptr;
        }
        return &(&c->Xmm0)[reg - ZYDIS_REGISTER_XMM0];
    }
    void MergeFlags(uint32_t mask, uint64_t flags) const {
        c->EFlags = static_cast<DWORD>((c->EFlags & ~mask) | (flags & mask));
    }
    /// A 256-bit source: the low half from the legacy area, the high half from the context's AVX state.
    bool Ymm(ZydisRegister reg, uint8_t out[32]) const {
        if (reg < ZYDIS_REGISTER_YMM0 || reg > ZYDIS_REGISTER_YMM15) {
            return false;
        }
        const int i = reg - ZYDIS_REGISTER_YMM0;
        DWORD64 mask = 0;
        if (!GetXStateFeaturesMask(c, &mask) || !(mask & XSTATE_MASK_AVX)) {
            return false;
        }
        DWORD length = 0;
        const auto* upper = static_cast<const uint8_t*>(LocateXStateFeature(c, XSTATE_AVX, &length));
        if (!upper || length < static_cast<DWORD>((i + 1) * 16)) {
            return false;
        }
        std::memcpy(out, &(&c->Xmm0)[i], 16);
        std::memcpy(out + 16, upper + i * 16, 16);
        return true;
    }
};

/// Registers of a live store hook (the stub's GuestRegs: GPRs, flags, xmm0-7; xmm8-15 are not saved).
struct HookRegs {
    SotcHooks::GuestRegs* g;
    uint64_t* Slot(ZydisRegister reg) const {
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
        case ZYDIS_REGISTER_RAX: return &g->rax;
        case ZYDIS_REGISTER_RCX: return &g->rcx;
        case ZYDIS_REGISTER_RDX: return &g->rdx;
        case ZYDIS_REGISTER_RBX: return &g->rbx;
        case ZYDIS_REGISTER_RSP: return &g->rsp;
        case ZYDIS_REGISTER_RBP: return &g->rbp;
        case ZYDIS_REGISTER_RSI: return &g->rsi;
        case ZYDIS_REGISTER_RDI: return &g->rdi;
        case ZYDIS_REGISTER_R8: return &g->r8;
        case ZYDIS_REGISTER_R9: return &g->r9;
        case ZYDIS_REGISTER_R10: return &g->r10;
        case ZYDIS_REGISTER_R11: return &g->r11;
        case ZYDIS_REGISTER_R12: return &g->r12;
        case ZYDIS_REGISTER_R13: return &g->r13;
        case ZYDIS_REGISTER_R14: return &g->r14;
        case ZYDIS_REGISTER_R15: return &g->r15;
        default: return nullptr;
        }
    }
    const void* Xmm(ZydisRegister reg) const {
        if (reg < ZYDIS_REGISTER_XMM0 || reg > ZYDIS_REGISTER_XMM7) {
            return nullptr; // xmm8-15: the original store runs (it faults and is emulated there)
        }
        return g->xmm[reg - ZYDIS_REGISTER_XMM0];
    }
    void MergeFlags(uint32_t mask, uint64_t flags) const {
        g->rflags = (g->rflags & ~uint64_t{mask}) | (flags & mask);
    }
    bool Ymm(ZydisRegister, uint8_t*) const {
        return false; // not saved by the stub: the original store runs (it faults and is emulated there)
    }
};

template <typename Regs>
bool ReadGpr(const Regs& r, ZydisRegister reg, uint64_t* out) {
    const auto* slot = r.Slot(reg);
    const int width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (!slot || width <= 0 || width > 64) {
        return false;
    }
    const int shift = (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH ||
                       reg == ZYDIS_REGISTER_BH)
                          ? 8
                          : 0;
    const uint64_t mask = width == 64 ? ~0ULL : ((1ULL << width) - 1);
    *out = (static_cast<uint64_t>(*slot) >> shift) & mask;
    return true;
}

/// Writes a general register as the CPU does: 64 and 32 bits replace the whole register (32 zero-extends), 16 and 8
/// bits replace their part only.
template <typename Regs>
bool WriteGpr(const Regs& r, ZydisRegister reg, uint64_t v) {
    auto* slot = r.Slot(reg);
    const int width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (!slot || width <= 0 || width > 64) {
        return false;
    }
    if (width == 64) {
        *slot = v;
    } else if (width == 32) {
        *slot = v & 0xffffffffULL;
    } else {
        const int shift = (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH ||
                           reg == ZYDIS_REGISTER_BH)
                              ? 8
                              : 0;
        const uint64_t mask = ((1ULL << width) - 1) << shift;
        *slot = (static_cast<uint64_t>(*slot) & ~mask) | ((v << shift) & mask);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ the store
// Locked read-modify-write on the backing view, with the flags it leaves (the same instruction on the same bytes).
#define SOTC_EMU_RMW2(NAME, INSN)                                                                                  \
    uint64_t NAME(void* p, uint64_t v, unsigned size) {                                                            \
        uint64_t fl = 0;                                                                                           \
        switch (size) {                                                                                            \
        case 1: asm volatile("lock " INSN "b %2, %0\n\tpushfq\n\tpopq %1"                                          \
                             : "+m"(*static_cast<uint8_t*>(p)), "=r"(fl) : "q"(static_cast<uint8_t>(v)) : "cc", "memory"); break; \
        case 2: asm volatile("lock " INSN "w %2, %0\n\tpushfq\n\tpopq %1"                                          \
                             : "+m"(*static_cast<uint16_t*>(p)), "=r"(fl) : "r"(static_cast<uint16_t>(v)) : "cc", "memory"); break; \
        case 4: asm volatile("lock " INSN "l %2, %0\n\tpushfq\n\tpopq %1"                                          \
                             : "+m"(*static_cast<uint32_t*>(p)), "=r"(fl) : "r"(static_cast<uint32_t>(v)) : "cc", "memory"); break; \
        default: asm volatile("lock " INSN "q %2, %0\n\tpushfq\n\tpopq %1"                                         \
                              : "+m"(*static_cast<uint64_t*>(p)), "=r"(fl) : "r"(v) : "cc", "memory"); break;       \
        }                                                                                                          \
        return fl;                                                                                                 \
    }
#define SOTC_EMU_RMW1(NAME, INSN)                                                                                  \
    uint64_t NAME(void* p, unsigned size) {                                                                        \
        uint64_t fl = 0;                                                                                           \
        switch (size) {                                                                                            \
        case 1: asm volatile("lock " INSN "b %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint8_t*>(p)), "=r"(fl) : : "cc", "memory"); break; \
        case 2: asm volatile("lock " INSN "w %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint16_t*>(p)), "=r"(fl) : : "cc", "memory"); break; \
        case 4: asm volatile("lock " INSN "l %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint32_t*>(p)), "=r"(fl) : : "cc", "memory"); break; \
        default: asm volatile("lock " INSN "q %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint64_t*>(p)), "=r"(fl) : : "cc", "memory"); break; \
        }                                                                                                          \
        return fl;                                                                                                 \
    }
SOTC_EMU_RMW2(EmuAdd, "add")
SOTC_EMU_RMW2(EmuSub, "sub")
SOTC_EMU_RMW2(EmuAnd, "and")
SOTC_EMU_RMW2(EmuOr, "or")
SOTC_EMU_RMW2(EmuXor, "xor")
SOTC_EMU_RMW1(EmuInc, "inc")
SOTC_EMU_RMW1(EmuDec, "dec")
#undef SOTC_EMU_RMW1
#undef SOTC_EMU_RMW2

/// lock xadd: *p += v, v = the old value; the flags of the add.
uint64_t EmuXadd(void* p, uint64_t* v, unsigned size) {
    uint64_t fl = 0;
    switch (size) {
    case 1: {
        uint8_t x = static_cast<uint8_t>(*v);
        asm volatile("lock xaddb %2, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint8_t*>(p)), "=r"(fl), "+q"(x) : : "cc", "memory");
        *v = x;
        break;
    }
    case 2: {
        uint16_t x = static_cast<uint16_t>(*v);
        asm volatile("lock xaddw %2, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint16_t*>(p)), "=r"(fl), "+r"(x) : : "cc", "memory");
        *v = x;
        break;
    }
    case 4: {
        uint32_t x = static_cast<uint32_t>(*v);
        asm volatile("lock xaddl %2, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint32_t*>(p)), "=r"(fl), "+r"(x) : : "cc", "memory");
        *v = x;
        break;
    }
    default: {
        uint64_t x = *v;
        asm volatile("lock xaddq %2, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint64_t*>(p)), "=r"(fl), "+r"(x) : : "cc", "memory");
        *v = x;
        break;
    }
    }
    return fl;
}

/// lock cmpxchg: compares *acc with *p; equal: *p = v, else *acc = *p. The flags of the compare.
uint64_t EmuCmpxchg(void* p, uint64_t* acc, uint64_t v, unsigned size) {
    uint64_t fl = 0;
    switch (size) {
    case 1: {
        uint8_t a = static_cast<uint8_t>(*acc);
        asm volatile("lock cmpxchgb %3, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint8_t*>(p)), "=r"(fl), "+a"(a) : "q"(static_cast<uint8_t>(v)) : "cc", "memory");
        *acc = a;
        break;
    }
    case 2: {
        uint16_t a = static_cast<uint16_t>(*acc);
        asm volatile("lock cmpxchgw %3, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint16_t*>(p)), "=r"(fl), "+a"(a) : "r"(static_cast<uint16_t>(v)) : "cc", "memory");
        *acc = a;
        break;
    }
    case 4: {
        uint32_t a = static_cast<uint32_t>(*acc);
        asm volatile("lock cmpxchgl %3, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint32_t*>(p)), "=r"(fl), "+a"(a) : "r"(static_cast<uint32_t>(v)) : "cc", "memory");
        *acc = a;
        break;
    }
    default: {
        uint64_t a = *acc;
        asm volatile("lock cmpxchgq %3, %0\n\tpushfq\n\tpopq %1" : "+m"(*static_cast<uint64_t*>(p)), "=r"(fl), "+a"(a) : "r"(v) : "cc", "memory");
        *acc = a;
        break;
    }
    }
    return fl;
}

uint64_t EmuXchg(void* p, uint64_t v, unsigned size) {
    switch (size) {
    case 1: return __atomic_exchange_n(static_cast<uint8_t*>(p), static_cast<uint8_t>(v), __ATOMIC_SEQ_CST);
    case 2: return __atomic_exchange_n(static_cast<uint16_t*>(p), static_cast<uint16_t>(v), __ATOMIC_SEQ_CST);
    case 4: return __atomic_exchange_n(static_cast<uint32_t*>(p), static_cast<uint32_t>(v), __ATOMIC_SEQ_CST);
    default: return __atomic_exchange_n(static_cast<uint64_t*>(p), v, __ATOMIC_SEQ_CST);
    }
}

void StoreScalar(void* p, uint64_t v, unsigned size) {
    switch (size) {
    case 1: __atomic_store_n(static_cast<uint8_t*>(p), static_cast<uint8_t>(v), __ATOMIC_SEQ_CST); break;
    case 2: __atomic_store_n(static_cast<uint16_t*>(p), static_cast<uint16_t>(v), __ATOMIC_SEQ_CST); break;
    case 4: __atomic_store_n(static_cast<uint32_t*>(p), static_cast<uint32_t>(v), __ATOMIC_SEQ_CST); break;
    default: __atomic_store_n(static_cast<uint64_t*>(p), v, __ATOMIC_SEQ_CST); break;
    }
}

// Emulated stores whose bytes the arena may not hold yet (see ForEachNotPending), by start address. An entry ends
// when the drain copied it (NoteDrained, a copy made after the store) and the GPU ran that copy, plus a grace for a
// readback recorded before the copy and applied after it; or after kPendingMaxNs in any case.
struct PendingEntry {
    uint64_t end{};
    int64_t stored_ns{};  // the last store into it
    uint64_t tick{};      // the tick of the copy that holds it (0: not copied yet)
    int64_t drained_ns{}; // when that copy was recorded
};
struct Pending {
    std::mutex mu;
    std::map<uint64_t, PendingEntry> ranges;
    std::atomic<uint64_t> count{0};
};
Pending& P() {
    static Pending p;
    return p;
}
int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
constexpr int64_t kPendingGraceNs = 250000000LL;  // after the copy ran
constexpr int64_t kPendingMaxNs = 30000000000LL;  // never drained: dropped after 30 s

bool PendingDone(const PendingEntry& e, int64_t now) {
    if (now - e.stored_ns > kPendingMaxNs) {
        return true;
    }
    if (e.tick == 0 || now - e.drained_ns < kPendingGraceNs) {
        return false;
    }
    const TickQuery query = g_tick_query.load(std::memory_order_acquire);
    return query && query(g_tick_ctx.load(std::memory_order_acquire), e.tick);
}

void AddPending(uint64_t addr, uint64_t size) {
    auto& p = P();
    const int64_t now = NowNs();
    std::scoped_lock lock{p.mu};
    auto [it, added] = p.ranges.try_emplace(addr);
    it->second.end = std::max(it->second.end, addr + size);
    it->second.stored_ns = now;
    it->second.tick = 0; // a new store: not copied yet
    if (p.ranges.size() > 8192) { // amortized pruning
        for (auto i = p.ranges.begin(); i != p.ranges.end();) {
            i = PendingDone(i->second, now) ? p.ranges.erase(i) : std::next(i);
        }
    }
    p.count.store(p.ranges.size(), std::memory_order_relaxed);
}

/// Emulates the decoded store (rip: its address, for RIP-relative operands). fault_addr != 0: it must lie in the
/// operand. gpu_only: a store whose bytes are not GPU-written is not done (the caller runs the original). Returns
/// nullptr when done (the flags merged; the caller moves RIP or skips the instruction), else why not.
template <typename Regs>
const char* EmulateStore(const Regs& regs, uint64_t rip, const ZydisDecodedInstruction& insn,
                         const ZydisDecodedOperand* ops, uint64_t fault_addr, bool gpu_only) {
    if (insn.operand_count_visible < 1 || ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY ||
        !(ops[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
        return "no memory destination";
    }
    for (int i = 1; i < insn.operand_count_visible; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            return "two memory operands";
        }
    }
    const auto& mem = ops[0].mem;
    if (mem.segment == ZYDIS_REGISTER_FS || mem.segment == ZYDIS_REGISTER_GS) {
        return "segment";
    }
    uint64_t ea = mem.disp.value;
    if (mem.base == ZYDIS_REGISTER_RIP) {
        ea += rip + insn.length;
    } else if (mem.base != ZYDIS_REGISTER_NONE) {
        uint64_t b = 0;
        if (!ReadGpr(regs, mem.base, &b)) {
            return "base register";
        }
        ea += b;
    }
    if (mem.index != ZYDIS_REGISTER_NONE) {
        uint64_t x = 0;
        if (!ReadGpr(regs, mem.index, &x)) {
            return "index register";
        }
        ea += x * mem.scale;
    }
    const unsigned size = ops[0].size / 8;
    if (size == 0 || size > 32) {
        return "size";
    }
    if (fault_addr != 0 && (fault_addr < ea || fault_addr >= ea + size)) {
        return "fault outside the operand";
    }
    if (ea / kGuestPage != (ea + size - 1) / kGuestPage) {
        return "crosses a guest page";
    }
    if (gpu_only) {
        const GpuQuery query = g_gpu_query.load(std::memory_order_acquire);
        if (!query || !query(ea, size)) {
            return "not GPU-written";
        }
    }
    const unsigned char* backing = runtime_memory_backing_pointer(ea);
    if (!backing) {
        return "no backing";
    }
    void* p = const_cast<unsigned char*>(backing);

    const auto source = [&](uint64_t* v) {
        if (insn.operand_count_visible < 2) {
            return false;
        }
        const auto& s = ops[1];
        if (s.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            *v = s.imm.is_signed ? static_cast<uint64_t>(s.imm.value.s) : s.imm.value.u;
            return true;
        }
        if (s.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            return ReadGpr(regs, s.reg.value, v);
        }
        return false;
    };
    uint64_t flags = 0;
    uint32_t flag_mask = 0;
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_MOV:
    case ZYDIS_MNEMONIC_MOVNTI: {
        uint64_t v = 0;
        if (size > 8 || !source(&v)) {
            return "mov source";
        }
        StoreScalar(p, v, size);
        break;
    }
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC:
        if (size > 8) {
            return "size";
        }
        flags = insn.mnemonic == ZYDIS_MNEMONIC_INC ? EmuInc(p, size) : EmuDec(p, size);
        flag_mask = kIncDecFlags;
        break;
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR: {
        uint64_t v = 0;
        if (size > 8 || !source(&v)) {
            return "arith source";
        }
        switch (insn.mnemonic) {
        case ZYDIS_MNEMONIC_ADD: flags = EmuAdd(p, v, size); break;
        case ZYDIS_MNEMONIC_SUB: flags = EmuSub(p, v, size); break;
        case ZYDIS_MNEMONIC_AND: flags = EmuAnd(p, v, size); break;
        case ZYDIS_MNEMONIC_OR: flags = EmuOr(p, v, size); break;
        default: flags = EmuXor(p, v, size); break;
        }
        flag_mask = kArithFlags;
        break;
    }
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVNTDQ:
    case ZYDIS_MNEMONIC_MOVNTPS:
    case ZYDIS_MNEMONIC_VMOVUPS:
    case ZYDIS_MNEMONIC_VMOVAPS:
    case ZYDIS_MNEMONIC_VMOVDQU:
    case ZYDIS_MNEMONIC_VMOVDQA:
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_VMOVQ:
    case ZYDIS_MNEMONIC_VMOVD:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVSS:
    case ZYDIS_MNEMONIC_VMOVSD:
    case ZYDIS_MNEMONIC_VMOVSS: {
        if (insn.operand_count_visible != 2 || ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return "vector source";
        }
        if (const void* x = regs.Xmm(ops[1].reg.value)) {
            uint8_t bytes[16];
            std::memcpy(bytes, x, 16);
            std::memcpy(p, bytes, size);
        } else if (insn.mnemonic == ZYDIS_MNEMONIC_MOVQ || insn.mnemonic == ZYDIS_MNEMONIC_MOVD) {
            uint64_t v = 0;
            if (size > 8 || !ReadGpr(regs, ops[1].reg.value, &v)) {
                return "vector source";
            }
            StoreScalar(p, v, size);
        } else if (size == 32) {
            uint8_t bytes[32];
            if (!regs.Ymm(ops[1].reg.value, bytes)) {
                return "ymm source";
            }
            std::memcpy(p, bytes, 32);
        } else {
            return "ymm or xmm8-15 source";
        }
        break;
    }
    case ZYDIS_MNEMONIC_MOVLPS:
    case ZYDIS_MNEMONIC_MOVLPD:
    case ZYDIS_MNEMONIC_VMOVLPS:
    case ZYDIS_MNEMONIC_VMOVLPD:
    case ZYDIS_MNEMONIC_MOVHPS:
    case ZYDIS_MNEMONIC_MOVHPD:
    case ZYDIS_MNEMONIC_VMOVHPS:
    case ZYDIS_MNEMONIC_VMOVHPD: {
        // The low or the high 8 bytes of an xmm register.
        if (insn.operand_count_visible != 2 || ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER || size != 8) {
            return "vector source";
        }
        const void* x = regs.Xmm(ops[1].reg.value);
        if (!x) {
            return "xmm8-15 source";
        }
        const bool high = insn.mnemonic == ZYDIS_MNEMONIC_MOVHPS || insn.mnemonic == ZYDIS_MNEMONIC_MOVHPD ||
                          insn.mnemonic == ZYDIS_MNEMONIC_VMOVHPS || insn.mnemonic == ZYDIS_MNEMONIC_VMOVHPD;
        std::memcpy(p, static_cast<const uint8_t*>(x) + (high ? 8 : 0), 8);
        break;
    }
    case ZYDIS_MNEMONIC_XCHG: {
        uint64_t v = 0;
        if (size > 8 || insn.operand_count_visible != 2 || ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            !ReadGpr(regs, ops[1].reg.value, &v)) {
            return "xchg source";
        }
        if (!WriteGpr(regs, ops[1].reg.value, EmuXchg(p, v, size))) {
            return "xchg register";
        }
        break;
    }
    case ZYDIS_MNEMONIC_XADD: {
        uint64_t v = 0;
        if (size > 8 || insn.operand_count_visible != 2 || ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            !ReadGpr(regs, ops[1].reg.value, &v)) {
            return "xadd source";
        }
        flags = EmuXadd(p, &v, size);
        if (!WriteGpr(regs, ops[1].reg.value, v)) {
            return "xadd register";
        }
        flag_mask = kArithFlags;
        break;
    }
    case ZYDIS_MNEMONIC_CMPXCHG: {
        uint64_t v = 0;
        if (size > 8 || insn.operand_count_visible != 2 || ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            !ReadGpr(regs, ops[1].reg.value, &v)) {
            return "cmpxchg source";
        }
        const ZydisRegister acc_reg = size == 1   ? ZYDIS_REGISTER_AL
                                      : size == 2 ? ZYDIS_REGISTER_AX
                                      : size == 4 ? ZYDIS_REGISTER_EAX
                                                  : ZYDIS_REGISTER_RAX;
        uint64_t acc = 0;
        if (!ReadGpr(regs, acc_reg, &acc)) {
            return "cmpxchg accumulator";
        }
        const uint64_t before = acc;
        flags = EmuCmpxchg(p, &acc, v, size);
        // On failure the accumulator takes the memory value (eax: zero-extended into rax, as the CPU does); on
        // success it is not written.
        if (acc != before || !(flags & 0x40)) {
            WriteGpr(regs, acc_reg, acc);
        }
        flag_mask = kArithFlags;
        break;
    }
    default:
        return ZydisMnemonicGetString(insn.mnemonic);
    }
    if (flag_mask != 0) {
        regs.MergeFlags(flag_mask, flags);
    }
    AddPending(ea, size);
    SotcNative::Announce(ea, size, SotcNative::Kind::PerFrame);
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ live hooks
// Sites of the live store hooks (the hooked instruction, decoded once). Fixed table: the hook handler reads it with
// no lock while installs add to it.
struct Site {
    uint64_t rip{};
    ZydisDecodedInstruction insn{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
};
constexpr uint32_t kMaxSites = 1u << 16;
std::unique_ptr<Site> g_sites[kMaxSites];
std::atomic<uint32_t> g_site_count{0};
std::mutex g_site_mu;

uint32_t RegisterSite(uint64_t address, const uint8_t* bytes, uint32_t length) {
    std::scoped_lock lock{g_site_mu};
    const uint32_t id = g_site_count.load(std::memory_order_relaxed);
    if (id >= kMaxSites) {
        return ~0u;
    }
    auto site = std::make_unique<Site>();
    site->rip = address;
    uint8_t copy[16]{};
    std::memcpy(copy, bytes, std::min<uint32_t>(length, 15));
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(site->insn, site->ops, copy, 15))) {
        return ~0u;
    }
    g_sites[id] = std::move(site);
    g_site_count.store(id + 1, std::memory_order_release);
    return id;
}

uint32_t RegisterSiteThunk(uint64_t address, const uint8_t* bytes, uint32_t length) {
    return RegisterSite(address, bytes, length);
}

extern "C" uint32_t sotc_store_hook(void* regs, uint32_t site_id, uint32_t /*phase*/) {
    if (site_id >= g_site_count.load(std::memory_order_acquire)) {
        return 0;
    }
    const Site& site = *g_sites[site_id];
    const HookRegs r{static_cast<SotcHooks::GuestRegs*>(regs)};
    if (EmulateStore(r, site.rip, site.insn, site.ops, 0, true) != nullptr) {
        return 0; // the original store runs (not a GPU page, or a store the emulator does not know)
    }
    S().hooked.fetch_add(1, std::memory_order_relaxed);
    return Core::kStoreHookDone;
}

// ------------------------------------------------------------------------------------------------ learned file
// SOTC_HOOKS_LEARNED (default user/hooks_learned.bin): the sites hooked live, installed again at the next boot.
// "SOTCLRN1", then 32-byte records: u64 image offset, u32 instruction length, u32 kind (StoreHookKind), 16 bytes of
// the instruction as it was (checked against the image before an install).
constexpr char kLearnedMagic[8] = {'S', 'O', 'T', 'C', 'L', 'R', 'N', '1'};
struct LearnedRecord {
    uint64_t offset;
    uint32_t length;
    uint32_t kind;
    uint8_t bytes[16];
};
static_assert(sizeof(LearnedRecord) == 32);

const char* LearnedPath() {
    const char* v = std::getenv("SOTC_HOOKS_LEARNED");
    return v && v[0] ? v : "user/hooks_learned.bin";
}

std::mutex g_learned_mu;
std::unordered_set<uint64_t> g_learned; // offsets in the file (loaded at boot, added live)

void SaveLearned(uint64_t offset, const uint8_t* bytes, uint32_t length, Core::StoreHookKind kind) {
    std::scoped_lock lock{g_learned_mu};
    if (!g_learned.insert(offset).second) {
        return;
    }
    const char* path = LearnedPath();
    FILE* f = std::fopen(path, "ab");
    if (!f) {
        std::fprintf(stderr, "SOTC STORE HOOK: cannot write %s\n", path);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    if (std::ftell(f) == 0) {
        std::fwrite(kLearnedMagic, 1, sizeof(kLearnedMagic), f);
    }
    LearnedRecord rec{};
    rec.offset = offset;
    rec.length = length;
    rec.kind = static_cast<uint32_t>(kind);
    std::memcpy(rec.bytes, bytes, std::min<uint32_t>(length, 16));
    std::fwrite(&rec, sizeof(rec), 1, f);
    std::fclose(f);
}

const char* KindName(Core::StoreHookKind kind) {
    switch (kind) {
    case Core::StoreHookKind::Detour: return "hook";
    case Core::StoreHookKind::Relocated: return "function relocated";
    case Core::StoreHookKind::AlreadyRelocated: return "function already relocated";
    default: return "refused";
    }
}

// The fault-time trigger: a site emulated SOTC_NATIVE_TRACK_EMU_HOT times (default 16) gets a live hook, one attempt
// per site.
uint64_t HotFaults() {
    static const uint64_t n = [] {
        const char* v = std::getenv("SOTC_NATIVE_TRACK_EMU_HOT");
        return v ? std::max<uint64_t>(1, std::strtoull(v, nullptr, 10)) : 16ULL;
    }();
    return n;
}

// Threads inside TryEmulate: the RIP of their fault and the one after the instruction (where they resume). A live
// install is refused while one of them lies inside the bytes it rewrites (LivePatchWrite pauses the threads, but a
// thread in a fault handler resumes at its saved context, which GetThreadContext does not show).
struct InFlight {
    std::atomic<uint64_t> rip{0};
    std::atomic<uint64_t> next{0};
};
constexpr size_t kInFlight = 256;
InFlight g_in_flight[kInFlight];

struct InFlightScope {
    InFlight* slot{};
    InFlightScope(uint64_t rip, uint64_t next) {
        for (auto& s : g_in_flight) {
            uint64_t expected = 0;
            if (s.rip.compare_exchange_strong(expected, rip, std::memory_order_acq_rel)) {
                s.next.store(next, std::memory_order_release);
                slot = &s;
                return;
            }
        }
    }
    ~InFlightScope() {
        if (slot) {
            slot->next.store(0, std::memory_order_release);
            slot->rip.store(0, std::memory_order_release);
        }
    }
};

bool InFlightBusy(uint64_t lo, uint64_t hi) {
    // Resuming exactly at lo is fine (the new jump starts there); anywhere else inside is not.
    const auto inside = [lo, hi](uint64_t a) { return a > lo && a < hi; };
    for (const auto& s : g_in_flight) {
        const uint64_t rip = s.rip.load(std::memory_order_acquire);
        if (rip == 0) {
            continue;
        }
        if (inside(rip) || inside(s.next.load(std::memory_order_acquire))) {
            return true;
        }
    }
    return false;
}

/// A hot site gets a live hook. Returns the kind installed (Refused when nothing was done now). The calling thread
/// is in g_in_flight with its fault RIP and the RIP after the store; a detour at the site itself is accepted with the
/// caller about to re-run the instruction (the caller then does not advance RIP), so for it only the RIP after the
/// store may not lie inside: the caller's slot is switched to (rip, rip) for the attempt.
Core::StoreHookKind MaybeHookLive(uint64_t rip, uint64_t count, const ZydisDecodedInstruction& insn,
                                  InFlightScope& self) {
    const uint64_t hot = HotFaults();
    if (Mode() != 2 || count < hot || count % hot != 0) {
        return Core::StoreHookKind::Refused;
    }
    static std::mutex mu;
    static std::unordered_map<uint64_t, uint32_t> attempts; // site -> attempts (8 at most), ~0u once installed
    {
        std::scoped_lock lock{mu};
        uint32_t& n = attempts[rip];
        if (n == ~0u || n >= 8) {
            return Core::StoreHookKind::Refused;
        }
        ++n;
    }
    const uint64_t base = SotcTrace::ImageBase();
    uint8_t bytes[16]{};
    std::memcpy(bytes, reinterpret_cast<const void*>(rip), std::min<uint32_t>(insn.length, 16));
    const uint32_t id = RegisterSite(rip, bytes, insn.length);
    if (id == ~0u) {
        return Core::StoreHookKind::Refused;
    }
    const char* reason = "";
    const auto t0 = std::chrono::steady_clock::now();
    // During the attempt the caller's slot says it resumes at rip: when the install rewrites the bytes at rip (a
    // detour at the site, or a relocated function whose entry is rip) the caller re-runs the instruction from rip
    // through the new code (TryEmulate compares the bytes). Any patch that starts below rip and covers it is refused
    // by InFlightBusy (rip inside), so the caller never resumes inside rewritten bytes.
    if (self.slot) {
        self.slot->next.store(rip, std::memory_order_release);
    }
    auto kind = Core::InstallStoreHook(rip, id, reinterpret_cast<const void*>(&sotc_store_hook), &RegisterSiteThunk,
                                       true, &reason, &InFlightBusy);
    if (self.slot) {
        self.slot->next.store(rip + insn.length, std::memory_order_release);
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0);
    std::fprintf(stderr, "SOTC STORE HOOK live %#llx (%s): %s%s%s (%lld us)\n",
                 static_cast<unsigned long long>(rip - base), ZydisMnemonicGetString(insn.mnemonic), KindName(kind),
                 reason && reason[0] ? ": " : "", reason ? reason : "", static_cast<long long>(us.count()));
    if (kind == Core::StoreHookKind::Detour || kind == Core::StoreHookKind::Relocated) {
        SaveLearned(rip - base, bytes, insn.length, kind);
        std::scoped_lock lock{mu};
        attempts[rip] = ~0u;
    }
    return kind;
}

} // namespace

bool TryEmulate(void* context, uint64_t fault_addr) {
    auto* c = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
    const uint64_t rip = c->Rip;
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(insn, ops, reinterpret_cast<void*>(rip)))) {
        Note(rip, false, "decode");
        return false;
    }
    InFlightScope in_flight{rip, rip + insn.length};
    // A site about to become hot is hooked before its store: a detour at rip is then run by re-executing the
    // instruction (RIP unchanged), which goes through the hook.
    if (Mode() == 2) {
        uint64_t seen = 0;
        {
            auto& s = S();
            std::scoped_lock lock{s.mu};
            const auto it = s.sites.find(rip - SotcTrace::ImageBase());
            seen = it == s.sites.end() ? 0 : it->second;
        }
        uint8_t before[16]{};
        std::memcpy(before, reinterpret_cast<const void*>(rip), insn.length);
        MaybeHookLive(rip, seen + 1, insn, in_flight);
        // The bytes at rip were rewritten (a detour at the site, or a relocated function whose entry it is): the
        // instruction runs again from rip, through the new code, which does the store.
        if (std::memcmp(before, reinterpret_cast<const void*>(rip), insn.length) != 0) {
            Note(rip, true, "");
            return true;
        }
    }
    const ContextRegs r{c};
    if (const char* why = EmulateStore(r, rip, insn, ops, fault_addr, false)) {
        Note(rip, false, why);
        return false;
    }
    c->Rip += insn.length;
    Note(rip, true, "");
    return true;
}

void InstallLearned(uint64_t image_base) {
    if (Mode() != 2 || !On()) {
        return;
    }
    const char* path = LearnedPath();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return;
    }
    char magic[8]{};
    if (std::fread(magic, 1, sizeof(magic), f) != sizeof(magic) || std::memcmp(magic, kLearnedMagic, 8) != 0) {
        std::fclose(f);
        std::fprintf(stderr, "SOTC STORE HOOK: %s is not a learned-hook file\n", path);
        return;
    }
    uint32_t installed = 0;
    uint32_t mismatched = 0;
    uint32_t refused = 0;
    LearnedRecord rec{};
    while (std::fread(&rec, sizeof(rec), 1, f) == 1) {
        {
            std::scoped_lock lock{g_learned_mu};
            g_learned.insert(rec.offset);
        }
        const uint64_t site = image_base + rec.offset;
        if (rec.length == 0 || rec.length > 15 ||
            std::memcmp(reinterpret_cast<const void*>(site), rec.bytes, rec.length) != 0) {
            ++mismatched; // another build, or another patch covers the bytes now
            continue;
        }
        const uint32_t id = RegisterSite(site, rec.bytes, rec.length);
        const char* reason = "";
        const auto kind = id == ~0u ? Core::StoreHookKind::Refused
                                    : Core::InstallStoreHook(site, id, reinterpret_cast<const void*>(&sotc_store_hook),
                                                             &RegisterSiteThunk, false, &reason);
        if (kind == Core::StoreHookKind::Refused) {
            ++refused;
            std::fprintf(stderr, "SOTC STORE HOOK boot %#llx: refused: %s\n",
                         static_cast<unsigned long long>(rec.offset), reason ? reason : "");
        } else {
            ++installed;
        }
    }
    std::fclose(f);
    std::fprintf(stderr,
                 "SOTC STORE HOOK boot: %u learned sites installed, %u not matching the image, %u refused (%s)\n",
                 installed, mismatched, refused, path);
}

void NoteDrained(uint64_t addr, uint64_t size, uint64_t tick, int64_t copy_ns) {
    auto& p = P();
    if (p.count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const int64_t now = NowNs();
    const uint64_t end = addr + size;
    std::scoped_lock lock{p.mu};
    for (auto it = p.ranges.lower_bound(addr >= 16 ? addr - 16 : 0); it != p.ranges.end() && it->first < end; ++it) {
        // Covered whole by the copy, and stored before the copy read guest memory.
        if (it->first >= addr && it->second.end <= end && it->second.stored_ns <= copy_ns) {
            it->second.tick = tick;
            it->second.drained_ns = now;
        }
    }
}

namespace Detail {
bool AnyPending() {
    return P().count.load(std::memory_order_relaxed) != 0;
}
void PendingIn(uint64_t addr, uint64_t size, uint64_t* out, uint32_t* count, uint32_t max) {
    auto& p = P();
    const int64_t now = NowNs();
    const uint64_t end = addr + size;
    *count = 0;
    std::scoped_lock lock{p.mu};
    // Entries are at most 16 bytes long: one starting up to 16 bytes before addr may reach into it.
    auto it = p.ranges.lower_bound(addr >= 16 ? addr - 16 : 0);
    while (it != p.ranges.end() && it->first < end && *count < max) {
        if (PendingDone(it->second, now)) {
            it = p.ranges.erase(it);
            continue;
        }
        if (it->second.end > addr) {
            out[*count * 2] = std::max(it->first, addr);
            out[*count * 2 + 1] = std::min(it->second.end, end);
            ++*count;
        }
        ++it;
    }
    p.count.store(p.ranges.size(), std::memory_order_relaxed);
}
} // namespace Detail
#else
void NoteDrained(uint64_t, uint64_t, uint64_t, int64_t) {}
bool TryEmulate(void*, uint64_t) {
    return false;
}
void InstallLearned(uint64_t) {}
namespace Detail {
bool AnyPending() {
    return false;
}
void PendingIn(uint64_t, uint64_t, uint64_t*, uint32_t* count, uint32_t) {
    *count = 0;
}
} // namespace Detail
#endif

} // namespace SotcFaultEmu
