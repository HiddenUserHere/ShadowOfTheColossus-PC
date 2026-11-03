// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_SHADER_EXTENT. The value domain of the read extent analysis (sotc_shader_extent_pass.cpp).
//
// Int over-approximates the values a 32-bit integer SSA value can take, with the wrapping (mod 2^32) arithmetic of
// SPIR-V: an exact set of up to kMaxSet values, a wrapped interval, or anything (Top). Flt over-approximates a 32-bit
// float value: an interval of finite values (with an exact set of up to kMaxFltSet bit patterns while every value is
// known exactly, e.g. the arms of a Select of constants) plus a NaN flag, or anything. Every operation returns a
// superset of the
// values the real operation can produce from any operands in its argument sets (soundness); precision is lost only by
// widening. Kept free of IR types so that it can be tested alone (tests/test_sotc_shader_extent.cpp).
//
// Float rules follow the Vulkan precision requirements: OpFAdd/OpFSub/OpFMul correctly rounded, floor/ceil/trunc
// exact, fract inherited from x - floor(x). Rounding (to nearest or toward zero) is monotone and keeps a float bound a
// bound, so an interval computed exactly and then widened to the enclosing floats holds every rounded result, also
// with FMA contraction (the fused result is the rounding of a value inside the interval). Float arithmetic may flush
// denormal operands and results to zero, so its operands are seen through ArithView and its results reach 0 when they
// reach the denormals; moves (Select, Phi, bit casts, constants) keep exact bit patterns, denormals included. One hardware assumption: OpConvertFToU / OpConvertFToS of NaN
// give 0 and out-of-range values saturate (GCN, RDNA, NVIDIA and Intel conversion instructions do so; SPIR-V leaves
// them undefined). A float that may be infinite, and NaN reaching anything but fract and those conversions, is Top.

#pragma once

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Shader::Optimization::SotcExtent {

constexpr std::int64_t kTwo32 = std::int64_t{1} << 32;
constexpr std::int64_t kTwo31 = std::int64_t{1} << 31;
constexpr std::uint32_t kMaxSet = 64;
/// Pairwise set operations are done exactly while the product of the set sizes stays within this.
constexpr std::uint32_t kMaxPairs = 4096;

struct Int {
    enum class Kind : std::uint8_t { Set, Range, Top };
    Kind kind = Kind::Top;
    std::uint8_t count = 0;  ///< Set: number of values
    std::uint8_t why_kind{}; ///< walker bookkeeping for Top: 1 = unmodelled input, 2 = widened past 2^32
    const void* why{};       ///< walker bookkeeping for Top: the IR instruction that made it
    /// Range: the values x mod 2^32 for x in [lo, hi]. Canonical: lo in [0, 2^32) with hi < 2^32 (a plain unsigned
    /// interval), else lo in [-2^32, 0) and hi in [0, 2^32) (an interval that wraps through 0); hi - lo < 2^32 - 1.
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    std::array<std::uint32_t, kMaxSet> vals{}; ///< Set: sorted, unique

    bool IsTop() const {
        return kind == Kind::Top;
    }
    bool IsConst() const {
        return kind == Kind::Set && count == 1;
    }
};

inline Int Top() {
    return Int{};
}

inline Int Const(std::uint32_t v) {
    Int r;
    r.kind = Int::Kind::Set;
    r.count = 1;
    r.vals[0] = v;
    return r;
}

namespace detail {
inline std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}
} // namespace detail

/// The values x mod 2^32 for x in [lo, hi] (any integers; Top when the interval covers every residue).
inline Int Range(std::int64_t lo, std::int64_t hi) {
    if (hi < lo || hi - lo >= kTwo32 - 1 || lo <= std::numeric_limits<std::int64_t>::min() / 4 ||
        hi >= std::numeric_limits<std::int64_t>::max() / 4) {
        return Top();
    }
    const std::int64_t k = detail::FloorDiv(lo, kTwo32);
    lo -= k * kTwo32;
    hi -= k * kTwo32;
    if (hi >= kTwo32) {
        lo -= kTwo32;
        hi -= kTwo32;
    }
    if (lo == hi) {
        return Const(static_cast<std::uint32_t>(lo < 0 ? lo + kTwo32 : lo));
    }
    Int r;
    r.kind = Int::Kind::Range;
    r.lo = lo;
    r.hi = hi;
    return r;
}

/// The tightest wrapped interval [lo, hi] (lo <= hi, in Z) that holds every value of v. False for Top.
inline bool Arc(const Int& v, std::int64_t& lo, std::int64_t& hi) {
    if (v.kind == Int::Kind::Top) {
        return false;
    }
    if (v.kind == Int::Kind::Range) {
        lo = v.lo;
        hi = v.hi;
        return true;
    }
    // Set: the arc is the circle minus its largest gap between neighbouring values.
    const std::uint32_t n = v.count;
    std::int64_t best_gap = static_cast<std::int64_t>(v.vals[0]) + kTwo32 - v.vals[n - 1]; // wrap gap
    std::uint32_t best = n - 1; // the arc starts after vals[best]
    for (std::uint32_t i = 0; i + 1 < n; ++i) {
        const std::int64_t gap = static_cast<std::int64_t>(v.vals[i + 1]) - v.vals[i];
        if (gap > best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    if (best == n - 1) {
        lo = v.vals[0];
        hi = v.vals[n - 1];
    } else {
        lo = v.vals[best + 1];
        hi = static_cast<std::int64_t>(v.vals[best]) + kTwo32;
    }
    return true;
}

/// A set from arbitrary values (sorted and deduplicated here); wider than kMaxSet becomes its arc.
template <typename It>
inline Int FromValues(It begin, It end) {
    std::uint32_t tmp[kMaxPairs];
    std::uint32_t n = 0;
    for (It it = begin; it != end && n < kMaxPairs; ++it) {
        tmp[n++] = *it;
    }
    if (n == 0) {
        return Top();
    }
    std::sort(tmp, tmp + n);
    n = static_cast<std::uint32_t>(std::unique(tmp, tmp + n) - tmp);
    Int r;
    r.kind = Int::Kind::Set;
    if (n <= kMaxSet) {
        r.count = static_cast<std::uint8_t>(n);
        std::copy(tmp, tmp + n, r.vals.begin());
        return r;
    }
    // Wider than a set: the arc of all the values (the circle minus its largest gap).
    std::int64_t best_gap = static_cast<std::int64_t>(tmp[0]) + kTwo32 - tmp[n - 1];
    std::uint32_t best = n - 1;
    for (std::uint32_t i = 0; i + 1 < n; ++i) {
        const std::int64_t gap = static_cast<std::int64_t>(tmp[i + 1]) - tmp[i];
        if (gap > best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    if (best == n - 1) {
        return Range(tmp[0], tmp[n - 1]);
    }
    return Range(tmp[best + 1], static_cast<std::int64_t>(tmp[best]) + kTwo32);
}

/// Unsigned bounds of v as u32 values ([0, 2^32 - 1] when nothing better is known).
inline void UBounds(const Int& v, std::uint64_t& lo, std::uint64_t& hi) {
    lo = 0;
    hi = 0xFFFFFFFFull;
    if (v.kind == Int::Kind::Set) {
        lo = v.vals[0];
        hi = v.vals[v.count - 1];
    } else if (v.kind == Int::Kind::Range && v.lo >= 0) {
        lo = static_cast<std::uint64_t>(v.lo);
        hi = static_cast<std::uint64_t>(v.hi);
    }
}

/// Signed bounds of v as s32 values ([-2^31, 2^31 - 1] when nothing better is known).
inline void SBounds(const Int& v, std::int64_t& lo, std::int64_t& hi) {
    lo = -kTwo31;
    hi = kTwo31 - 1;
    if (v.kind == Int::Kind::Set) {
        lo = std::numeric_limits<std::int64_t>::max();
        hi = std::numeric_limits<std::int64_t>::min();
        for (std::uint32_t i = 0; i < v.count; ++i) {
            const std::int64_t s = static_cast<std::int32_t>(v.vals[i]);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        return;
    }
    if (v.kind != Int::Kind::Range) {
        return;
    }
    // Shift the representative by a multiple of 2^32 into the signed range, if the whole interval fits.
    for (const std::int64_t shift : {std::int64_t{0}, -kTwo32, kTwo32}) {
        if (v.lo + shift >= -kTwo31 && v.hi + shift < kTwo31) {
            lo = v.lo + shift;
            hi = v.hi + shift;
            return;
        }
    }
}

/// Applies op to every pair of a set x set; false when the sets are too large.
template <typename F>
inline bool Pairwise(const Int& a, const Int& b, Int& out, F&& op) {
    if (a.kind != Int::Kind::Set || b.kind != Int::Kind::Set ||
        std::uint32_t{a.count} * b.count > kMaxPairs) {
        return false;
    }
    std::uint32_t tmp[kMaxPairs];
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < a.count; ++i) {
        for (std::uint32_t j = 0; j < b.count; ++j) {
            tmp[n++] = op(a.vals[i], b.vals[j]);
        }
    }
    out = FromValues(tmp, tmp + n);
    return true;
}

template <typename F>
inline Int Map(const Int& a, F&& op) {
    std::uint32_t tmp[kMaxSet];
    for (std::uint32_t i = 0; i < a.count; ++i) {
        tmp[i] = op(a.vals[i]);
    }
    return FromValues(tmp, tmp + a.count);
}

inline Int Union(const Int& a, const Int& b) {
    if (a.IsTop() || b.IsTop()) {
        return Top();
    }
    if (a.kind == Int::Kind::Set && b.kind == Int::Kind::Set) {
        std::uint32_t tmp[2 * kMaxSet];
        std::copy(a.vals.begin(), a.vals.begin() + a.count, tmp);
        std::copy(b.vals.begin(), b.vals.begin() + b.count, tmp + a.count);
        return FromValues(tmp, tmp + a.count + b.count);
    }
    std::int64_t alo, ahi, blo, bhi;
    Arc(a, alo, ahi);
    Arc(b, blo, bhi);
    // The same residues have representatives 2^32 apart: take the narrowest hull.
    std::int64_t best_lo = 0, best_hi = -1;
    for (const std::int64_t shift : {std::int64_t{0}, -kTwo32, kTwo32}) {
        const std::int64_t lo = std::min(alo, blo + shift);
        const std::int64_t hi = std::max(ahi, bhi + shift);
        if (best_hi < best_lo || hi - lo < best_hi - best_lo) {
            best_lo = lo;
            best_hi = hi;
        }
    }
    return Range(best_lo, best_hi);
}

inline Int Add(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x + y; })) {
        return r;
    }
    std::int64_t alo, ahi, blo, bhi;
    if (!Arc(a, alo, ahi) || !Arc(b, blo, bhi)) {
        return Top();
    }
    return Range(alo + blo, ahi + bhi);
}

inline Int Sub(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x - y; })) {
        return r;
    }
    std::int64_t alo, ahi, blo, bhi;
    if (!Arc(a, alo, ahi) || !Arc(b, blo, bhi)) {
        return Top();
    }
    return Range(alo - bhi, ahi - blo);
}

inline Int Mul(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x * y; })) {
        return r;
    }
    std::int64_t alo, ahi, blo, bhi;
    if (!Arc(a, alo, ahi) || !Arc(b, blo, bhi)) {
        return Top();
    }
    std::int64_t lo = std::numeric_limits<std::int64_t>::max();
    std::int64_t hi = std::numeric_limits<std::int64_t>::min();
    for (const std::int64_t x : {alo, ahi}) {
        for (const std::int64_t y : {blo, bhi}) {
            std::int64_t p;
            if (__builtin_mul_overflow(x, y, &p)) {
                return Top();
            }
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
    }
    return Range(lo, hi);
}

inline Int Neg(const Int& a) {
    return Sub(Const(0), a);
}

inline Int Not(const Int& a) {
    if (a.kind == Int::Kind::Set) {
        return Map(a, [](std::uint32_t x) { return ~x; });
    }
    std::int64_t lo, hi;
    if (!Arc(a, lo, hi)) {
        return Top();
    }
    return Range(-1 - hi, -1 - lo); // ~x == -1 - x
}

/// Shift amounts usable as defined SPIR-V shifts: [k1, k2] with k2 < 32.
inline bool ShiftBounds(const Int& b, std::uint32_t& k1, std::uint32_t& k2) {
    std::uint64_t lo, hi;
    UBounds(b, lo, hi);
    if (hi >= 32) {
        return false;
    }
    k1 = static_cast<std::uint32_t>(lo);
    k2 = static_cast<std::uint32_t>(hi);
    return true;
}

inline Int Shl(const Int& a, const Int& b) {
    std::uint32_t k1, k2;
    if (!ShiftBounds(b, k1, k2)) {
        return Top();
    }
    if (k1 == k2) {
        return Mul(a, Const(std::uint32_t{1} << k1));
    }
    std::uint64_t lo, hi;
    UBounds(a, lo, hi);
    if ((hi << k2) >= static_cast<std::uint64_t>(kTwo32)) {
        return Top();
    }
    return Range(static_cast<std::int64_t>(lo << k1), static_cast<std::int64_t>(hi << k2));
}

inline Int LShr(const Int& a, const Int& b) {
    std::uint32_t k1, k2;
    if (!ShiftBounds(b, k1, k2)) {
        return Top();
    }
    if (k1 == k2 && a.kind == Int::Kind::Set) {
        return Map(a, [k1](std::uint32_t x) { return x >> k1; });
    }
    std::uint64_t lo, hi;
    UBounds(a, lo, hi);
    return Range(static_cast<std::int64_t>(lo >> k2), static_cast<std::int64_t>(hi >> k1));
}

inline Int AShr(const Int& a, const Int& b) {
    std::uint32_t k1, k2;
    if (!ShiftBounds(b, k1, k2)) {
        return Top();
    }
    if (k1 == k2 && a.kind == Int::Kind::Set) {
        return Map(a, [k1](std::uint32_t x) {
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> k1);
        });
    }
    std::int64_t lo, hi;
    SBounds(a, lo, hi);
    // For a fixed value the result is monotone in the shift; for a fixed shift, monotone in the value.
    return Range(std::min(lo >> k1, lo >> k2), std::max(hi >> k1, hi >> k2));
}

inline Int And(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x & y; })) {
        return r;
    }
    std::uint64_t alo, ahi, blo, bhi;
    UBounds(a, alo, ahi);
    UBounds(b, blo, bhi);
    return Range(0, static_cast<std::int64_t>(std::min(ahi, bhi))); // x & y <= min(x, y)
}

inline std::uint64_t AllOnesCovering(std::uint64_t v) {
    std::uint64_t m = 0;
    while (m < v) {
        m = (m << 1) | 1;
    }
    return m;
}

inline Int Or(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x | y; })) {
        return r;
    }
    std::uint64_t alo, ahi, blo, bhi;
    UBounds(a, alo, ahi);
    UBounds(b, blo, bhi);
    // max(x, y) <= x | y < 2^bit_width(max)
    return Range(static_cast<std::int64_t>(std::max(alo, blo)),
                 static_cast<std::int64_t>(AllOnesCovering(std::max(ahi, bhi))));
}

inline Int Xor(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x ^ y; })) {
        return r;
    }
    std::uint64_t alo, ahi, blo, bhi;
    UBounds(a, alo, ahi);
    UBounds(b, blo, bhi);
    return Range(0, static_cast<std::int64_t>(AllOnesCovering(std::max(ahi, bhi))));
}

inline Int UMin(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return std::min(x, y); })) {
        return r;
    }
    std::uint64_t alo, ahi, blo, bhi;
    UBounds(a, alo, ahi);
    UBounds(b, blo, bhi);
    return Range(static_cast<std::int64_t>(std::min(alo, blo)),
                 static_cast<std::int64_t>(std::min(ahi, bhi)));
}

inline Int UMax(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return std::max(x, y); })) {
        return r;
    }
    std::uint64_t alo, ahi, blo, bhi;
    UBounds(a, alo, ahi);
    UBounds(b, blo, bhi);
    return Range(static_cast<std::int64_t>(std::max(alo, blo)),
                 static_cast<std::int64_t>(std::max(ahi, bhi)));
}

inline Int SMin(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint32_t>(
                std::min(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)));
        })) {
        return r;
    }
    std::int64_t alo, ahi, blo, bhi;
    SBounds(a, alo, ahi);
    SBounds(b, blo, bhi);
    return Range(std::min(alo, blo), std::min(ahi, bhi));
}

inline Int SMax(const Int& a, const Int& b) {
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint32_t>(
                std::max(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)));
        })) {
        return r;
    }
    std::int64_t alo, ahi, blo, bhi;
    SBounds(a, alo, ahi);
    SBounds(b, blo, bhi);
    return Range(std::max(alo, blo), std::max(ahi, bhi));
}

/// GLSL.std.450 UClamp / SClamp: undefined when min > max is possible, so only a proven min <= max is modelled.
inline Int UClamp(const Int& x, const Int& mn, const Int& mx) {
    std::uint64_t nlo, nhi, xlo, xhi;
    UBounds(mn, nlo, nhi);
    UBounds(mx, xlo, xhi);
    if (nhi > xlo) {
        return Top();
    }
    return UMin(UMax(x, mn), mx);
}

inline Int SClamp(const Int& x, const Int& mn, const Int& mx) {
    std::int64_t nlo, nhi, xlo, xhi;
    SBounds(mn, nlo, nhi);
    SBounds(mx, xlo, xhi);
    if (nhi > xlo) {
        return Top();
    }
    return SMin(SMax(x, mn), mx);
}

/// Median of three: max(min(a, b), min(max(a, b), c)), monotone in each argument.
inline Int UMed3(const Int& a, const Int& b, const Int& c) {
    return UMax(UMin(a, b), UMin(UMax(a, b), c));
}

inline Int SMed3(const Int& a, const Int& b, const Int& c) {
    return SMax(SMin(a, b), SMin(SMax(a, b), c));
}

/// OpUDiv / OpUMod: undefined for a zero divisor, so only a divisor proven non-zero is modelled.
inline Int UDiv(const Int& a, const Int& b) {
    std::uint64_t blo, bhi;
    UBounds(b, blo, bhi);
    if (blo == 0) {
        return Top();
    }
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x / y; })) {
        return r;
    }
    std::uint64_t alo, ahi;
    UBounds(a, alo, ahi);
    return Range(static_cast<std::int64_t>(alo / bhi), static_cast<std::int64_t>(ahi / blo));
}

inline Int UMod(const Int& a, const Int& b) {
    std::uint64_t blo, bhi;
    UBounds(b, blo, bhi);
    if (blo == 0) {
        return Top();
    }
    Int r;
    if (Pairwise(a, b, r, [](std::uint32_t x, std::uint32_t y) { return x % y; })) {
        return r;
    }
    std::uint64_t alo, ahi;
    UBounds(a, alo, ahi);
    return Range(0, static_cast<std::int64_t>(std::min(ahi, bhi - 1)));
}

/// OpBitFieldUExtract; undefined when offset + count > 32, so only offset/count proven in range are modelled.
inline Int BfeU(const Int& base, const Int& offset, const Int& count) {
    std::uint64_t olo, ohi, clo, chi;
    UBounds(offset, olo, ohi);
    UBounds(count, clo, chi);
    if (ohi + chi > 32) {
        return Top();
    }
    if (offset.IsConst() && count.IsConst() && base.kind == Int::Kind::Set) {
        const std::uint32_t o = offset.vals[0];
        const std::uint32_t n = count.vals[0];
        return Map(base, [o, n](std::uint32_t x) {
            return n == 0 ? 0u : static_cast<std::uint32_t>((std::uint64_t{x} >> o) & ((std::uint64_t{1} << n) - 1));
        });
    }
    std::uint64_t blo, bhi;
    UBounds(base, blo, bhi);
    if (olo == ohi && clo == chi && bhi < (std::uint64_t{1} << (olo + clo))) {
        // No bit above the field is ever set: the field is base >> offset.
        return Range(static_cast<std::int64_t>(blo >> olo), static_cast<std::int64_t>(bhi >> olo));
    }
    const std::uint64_t field_max = (std::uint64_t{1} << chi) - 1;
    return Range(0, static_cast<std::int64_t>(std::min(field_max, bhi >> olo)));
}

inline Int BfeS(const Int& base, const Int& offset, const Int& count) {
    if (!offset.IsConst() || !count.IsConst()) {
        return Top();
    }
    const std::uint32_t o = offset.vals[0];
    const std::uint32_t n = count.vals[0];
    if (std::uint64_t{o} + n > 32) {
        return Top();
    }
    if (n == 0) {
        return Const(0);
    }
    if (base.kind == Int::Kind::Set) {
        return Map(base, [o, n](std::uint32_t x) {
            const std::uint64_t field = (std::uint64_t{x} >> o) & ((std::uint64_t{1} << n) - 1);
            const std::int64_t s = (field >> (n - 1)) ? static_cast<std::int64_t>(field) - (std::int64_t{1} << n)
                                                      : static_cast<std::int64_t>(field);
            return static_cast<std::uint32_t>(s);
        });
    }
    std::uint64_t blo, bhi;
    UBounds(base, blo, bhi);
    if (bhi < (std::uint64_t{1} << (o + n - 1))) {
        // The sign bit of the field is never set and nothing above it either.
        return Range(static_cast<std::int64_t>(blo >> o), static_cast<std::int64_t>(bhi >> o));
    }
    return Range(-(std::int64_t{1} << (n - 1)), (std::int64_t{1} << (n - 1)) - 1);
}

/// Restricts a value known to fit in `bits` unsigned bits (u8/u16 SSA values).
inline Int ZeroExtend(const Int& a, std::uint32_t bits) {
    const std::uint64_t max = (std::uint64_t{1} << bits) - 1;
    std::uint64_t lo, hi;
    UBounds(a, lo, hi);
    if (hi <= max) {
        return a;
    }
    return Range(0, static_cast<std::int64_t>(max));
}

inline Int SignExtend(const Int& a, std::uint32_t bits) {
    return BfeS(a, Const(0), Const(bits));
}

// ---------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kMaxFltSet = 16;

struct Flt {
    bool top = true;
    bool nan = false;       ///< a NaN of unknown bits possible (one not in vals)
    bool inf = false;       ///< vals holds an infinity
    std::uint8_t count = 0; ///< > 0: every other value is one of vals[0..count), bit for bit (NaNs included)
    std::uint8_t why_kind{};
    const void* why{};
    double lo = 0.0; ///< the finite values are in [lo, hi]
    double hi = 0.0;
    std::array<float, kMaxFltSet> vals{};

    bool IsExact() const {
        return !top && !nan && count == 1 && std::isfinite(vals[0]);
    }
};

inline Flt FTop() {
    return Flt{};
}

namespace detail {
/// Relative slack for the double arithmetic that computes the bounds (a few roundings at 2^-53 each).
constexpr double kRel = 0x1p-40;

inline bool NormalOrZero(float f) {
    return f == 0.0f || std::fabs(f) >= FLT_MIN;
}

/// Largest float <= x and smallest float >= x (x finite, within float range).
inline float FloatDown(double x) {
    float f = static_cast<float>(x);
    if (static_cast<double>(f) > x) {
        f = std::nextafter(f, -std::numeric_limits<float>::infinity());
    }
    return f;
}
inline float FloatUp(double x) {
    float f = static_cast<float>(x);
    if (static_cast<double>(f) < x) {
        f = std::nextafter(f, std::numeric_limits<float>::infinity());
    }
    return f;
}

inline bool SameBits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}
} // namespace detail

/// Real bounds [lo, hi] of the non-NaN values. Any interval that reaches into the denormals is extended to 0 (a
/// flushed operand or result is 0), and one that may exceed the float range is Top (it may be infinite).
inline Flt FRangeExact(double lo, double hi, bool nan) {
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo || std::fabs(lo) > FLT_MAX ||
        std::fabs(hi) > FLT_MAX) {
        return FTop();
    }
    if (lo > 0.0 && lo < FLT_MIN) {
        lo = 0.0;
    }
    if (hi < 0.0 && hi > -FLT_MIN) {
        hi = 0.0;
    }
    Flt r;
    r.top = false;
    r.nan = nan;
    r.lo = lo;
    r.hi = hi;
    return r;
}

/// Bounds of an exact result computed in double, widened to the enclosing floats: every float rounding of a value in
/// [lo, hi] stays inside.
inline Flt FRange(double lo, double hi, bool nan) {
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo || std::fabs(lo) > FLT_MAX ||
        std::fabs(hi) > FLT_MAX) {
        return FTop();
    }
    lo -= std::fabs(lo) * detail::kRel;
    hi += std::fabs(hi) * detail::kRel;
    if (std::fabs(lo) > FLT_MAX || std::fabs(hi) > FLT_MAX) {
        return FTop();
    }
    return FRangeExact(detail::FloatDown(lo), detail::FloatUp(hi), nan);
}

/// Exact values, bit for bit (a move or a bit cast keeps any pattern: denormals, infinities and NaNs too). More than
/// kMaxFltSet distinct values keep only the bounds of the finite ones (Top if one is infinite or NaN).
inline Flt FValues(const float* v, std::uint32_t n, bool nan) {
    if (n == 0) {
        return FTop();
    }
    Flt r;
    r.top = false;
    r.nan = nan;
    bool first = true;
    bool special = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) {
            special = true;
            r.inf |= std::isinf(v[i]);
            continue;
        }
        r.lo = first ? v[i] : std::min(r.lo, double{v[i]});
        r.hi = first ? v[i] : std::max(r.hi, double{v[i]});
        first = false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        bool seen = false;
        for (std::uint32_t j = 0; j < r.count && !seen; ++j) {
            seen = detail::SameBits(r.vals[j], v[i]);
        }
        if (seen) {
            continue;
        }
        if (r.count == kMaxFltSet) {
            if (special) {
                return FTop();
            }
            r.count = 0; // too many: bounds only
            break;
        }
        r.vals[r.count++] = v[i];
    }
    return r;
}

/// The operand as float arithmetic sees it: a denormal may be flushed to zero, so a value that may be denormal loses
/// its exact set and its interval takes in 0.
/// The NaN patterns of the set become the unknown-NaN flag (arithmetic does not keep payloads), and an infinity is
/// Top (not modelled).
inline Flt ArithView(const Flt& a_in) {
    if (a_in.top || a_in.inf) {
        return FTop();
    }
    Flt a = a_in;
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < a_in.count; ++i) {
        if (std::isnan(a_in.vals[i])) {
            a.nan = true;
        } else {
            a.vals[n++] = a_in.vals[i];
        }
    }
    if (a_in.count != 0 && n == 0) {
        a.lo = a.hi = 0.0; // only NaNs: no finite value at all, any bounds hold
    }
    a.count = static_cast<std::uint8_t>(n);
    if (!(a.lo < FLT_MIN && a.hi > -FLT_MIN)) {
        return a; // no denormal possible
    }
    bool denormal = a.count == 0;
    for (std::uint32_t i = 0; i < a.count && !denormal; ++i) {
        denormal = !detail::NormalOrZero(a.vals[i]);
    }
    if (!denormal) {
        return a;
    }
    a.count = 0;
    a.lo = std::min(a.lo, 0.0);
    a.hi = std::max(a.hi, 0.0);
    return a;
}

inline Flt FExact(float f) {
    return FValues(&f, 1, false);
}

inline Flt FUnion(const Flt& a, const Flt& b) {
    if (a.top || b.top) {
        return FTop();
    }
    if (a.count != 0 && b.count != 0) {
        float v[2 * kMaxFltSet];
        std::copy(a.vals.begin(), a.vals.begin() + a.count, v);
        std::copy(b.vals.begin(), b.vals.begin() + b.count, v + a.count);
        return FValues(v, a.count + b.count, a.nan || b.nan);
    }
    if (a.inf || b.inf) {
        return FTop();
    }
    // Bounds only: NaN patterns of a set lose their bits.
    bool set_nan = false;
    for (std::uint32_t i = 0; i < a.count; ++i) {
        set_nan |= std::isnan(a.vals[i]);
    }
    for (std::uint32_t i = 0; i < b.count; ++i) {
        set_nan |= std::isnan(b.vals[i]);
    }
    return FRangeExact(std::min(a.lo, b.lo), std::max(a.hi, b.hi), a.nan || b.nan || set_nan);
}

/// min/max/clamp/fract may return either zero for a zero result: a set holding one zero gets the other too.
inline Flt WithBothZeros(const Flt& a) {
    for (std::uint32_t i = 0; i < a.count; ++i) {
        if (a.vals[i] == 0.0f) {
            const float zeros[2] = {0.0f, -0.0f};
            return FUnion(a, FValues(zeros, 2, false));
        }
    }
    return a;
}

/// Applies op to every value of a set, keeping it a set when every result is exactly representable (`exact`
/// reports that); false when a has no set.
template <typename F>
inline bool FMapSet(const Flt& a, Flt& out, F&& op) {
    if (a.top || a.count == 0) {
        return false;
    }
    float v[kMaxFltSet];
    for (std::uint32_t i = 0; i < a.count; ++i) {
        bool exact = false;
        v[i] = op(a.vals[i], exact);
        if (!exact || !std::isfinite(v[i])) {
            return false;
        }
    }
    out = FValues(v, a.count, a.nan);
    return true;
}

template <typename F>
inline bool FPairSet(const Flt& a, const Flt& b, Flt& out, F&& op) {
    if (a.top || b.top || a.count == 0 || b.count == 0) {
        return false;
    }
    float v[kMaxFltSet * kMaxFltSet];
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < a.count; ++i) {
        for (std::uint32_t j = 0; j < b.count; ++j) {
            bool exact = false;
            v[n] = op(a.vals[i], b.vals[j], exact);
            if (!exact || !std::isfinite(v[n])) {
                return false;
            }
            ++n;
        }
    }
    out = FValues(v, n, a.nan || b.nan);
    return true;
}

inline Flt FNeg(const Flt& a_in) {
    const Flt a = ArithView(a_in);
    if (a.top) {
        return FTop();
    }
    Flt r = a;
    for (std::uint32_t i = 0; i < a.count; ++i) {
        r.vals[i] = -a.vals[i];
    }
    r.lo = -a.hi;
    r.hi = -a.lo;
    return r;
}

inline Flt FAbs(const Flt& a_in) {
    const Flt a = ArithView(a_in);
    if (a.top) {
        return FTop();
    }
    Flt r;
    if (FMapSet(a, r, [](float x, bool& exact) {
            exact = true;
            return std::fabs(x);
        })) {
        return r;
    }
    if (a.lo >= 0.0) {
        return a;
    }
    if (a.hi <= 0.0) {
        return FNeg(a);
    }
    return FRangeExact(0.0, std::max(-a.lo, a.hi), a.nan);
}

inline Flt FAdd(const Flt& a_in, const Flt& b_in) {
    const Flt a = ArithView(a_in), b = ArithView(b_in);
    if (a.top || b.top) {
        return FTop();
    }
    Flt r;
    if (FPairSet(a, b, r, [](float x, float y, bool& exact) {
            // TwoSum: the rounding error of the float sum, exact in round-to-nearest.
            const volatile float s = x + y;
            const float bb = s - x;
            const float err = (x - (s - bb)) + (y - bb);
            exact = err == 0.0f && detail::NormalOrZero(s);
            return static_cast<float>(s);
        })) {
        return r;
    }
    return FRange(a.lo + b.lo, a.hi + b.hi, a.nan || b.nan);
}

inline Flt FSub(const Flt& a, const Flt& b) {
    return FAdd(a, FNeg(b));
}

inline Flt FMul(const Flt& a_in, const Flt& b_in) {
    const Flt a = ArithView(a_in), b = ArithView(b_in);
    if (a.top || b.top) {
        return FTop();
    }
    Flt r;
    if (FPairSet(a, b, r, [](float x, float y, bool& exact) {
            const volatile float p = x * y;
            exact = detail::NormalOrZero(p) &&
                    static_cast<double>(x) * static_cast<double>(y) == static_cast<double>(p);
            return static_cast<float>(p);
        })) {
        return r;
    }
    const double c[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    return FRange(*std::min_element(c, c + 4), *std::max_element(c, c + 4), a.nan || b.nan);
}

enum class Round { Floor, Ceil, Trunc, Even };

inline double ApplyRound(Round mode, double x) {
    switch (mode) {
    case Round::Floor:
        return std::floor(x);
    case Round::Ceil:
        return std::ceil(x);
    case Round::Trunc:
        return std::trunc(x);
    case Round::Even:
    default:
        return std::nearbyint(x); // the default rounding mode is to nearest even
    }
}

/// Floor/ceil/trunc/round-even: exact and monotone. Bounds that are not floats round to whole numbers that may not
/// be floats either, which stays a superset.
inline Flt FRound(const Flt& a_in, Round mode) {
    const Flt a = ArithView(a_in);
    if (a.top) {
        return FTop();
    }
    Flt r;
    if (FMapSet(a, r, [mode](float x, bool& exact) {
            exact = true;
            return static_cast<float>(ApplyRound(mode, x));
        })) {
        return r;
    }
    return FRangeExact(ApplyRound(mode, a.lo), ApplyRound(mode, a.hi), a.nan);
}

/// fract(x) = x - floor(x): in [0, 1] for every finite x (1.0 when the subtraction rounds up), NaN for NaN or
/// infinite x. So even an unknown input gives a bounded result.
inline Flt FFract(const Flt& a_in) {
    const Flt a = ArithView(a_in);
    Flt r;
    if (FMapSet(a, r, [](float x, bool& exact) {
            const double d = static_cast<double>(x) - std::floor(static_cast<double>(x));
            const float f = static_cast<float>(d);
            exact = static_cast<double>(f) == d;
            return f;
        })) {
        return WithBothZeros(r);
    }
    return FRangeExact(0.0, 1.0, a.top || a.nan);
}

/// GLSL.std.450 FClamp / FMin / FMax are undefined for NaN operands: those are Top.
inline Flt FClamp(const Flt& x_in, const Flt& mn_in, const Flt& mx_in) {
    const Flt x = ArithView(x_in), mn = ArithView(mn_in), mx = ArithView(mx_in);
    if (x.top || mn.top || mx.top || x.nan || mn.nan || mx.nan || mn.hi > mx.lo) {
        return FTop();
    }
    if (mn.IsExact() && mx.IsExact()) {
        Flt r;
        const float lo = mn.vals[0], hi = mx.vals[0];
        if (FMapSet(x, r, [lo, hi](float v, bool& exact) {
                exact = true;
                return std::min(std::max(v, lo), hi);
            })) {
            return WithBothZeros(r);
        }
    }
    return FRangeExact(std::min(std::max(x.lo, mn.lo), mx.lo), std::min(std::max(x.hi, mn.hi), mx.hi), false);
}

inline Flt FMin(const Flt& a_in, const Flt& b_in) {
    const Flt a = ArithView(a_in), b = ArithView(b_in);
    if (a.top || b.top || a.nan || b.nan) {
        return FTop();
    }
    Flt r;
    if (FPairSet(a, b, r, [](float x, float y, bool& exact) {
            exact = true;
            return std::min(x, y);
        })) {
        return WithBothZeros(r);
    }
    return FRangeExact(std::min(a.lo, b.lo), std::min(a.hi, b.hi), false);
}

inline Flt FMax(const Flt& a_in, const Flt& b_in) {
    const Flt a = ArithView(a_in), b = ArithView(b_in);
    if (a.top || b.top || a.nan || b.nan) {
        return FTop();
    }
    Flt r;
    if (FPairSet(a, b, r, [](float x, float y, bool& exact) {
            exact = true;
            return std::max(x, y);
        })) {
        return WithBothZeros(r);
    }
    return FRangeExact(std::max(a.lo, b.lo), std::max(a.hi, b.hi), false);
}

/// OpConvertUToF / OpConvertSToF (correctly rounded, monotone).
inline Flt FFromInt(const Int& a, bool is_signed) {
    if (a.kind == Int::Kind::Set && a.count <= kMaxFltSet) {
        float v[kMaxFltSet];
        bool exact = true;
        for (std::uint32_t i = 0; i < a.count && exact; ++i) {
            const double d = is_signed ? static_cast<double>(static_cast<std::int32_t>(a.vals[i]))
                                       : static_cast<double>(a.vals[i]);
            v[i] = static_cast<float>(d);
            exact = static_cast<double>(v[i]) == d;
        }
        if (exact) {
            return FValues(v, a.count, false);
        }
    }
    if (is_signed) {
        std::int64_t lo, hi;
        SBounds(a, lo, hi);
        return FRange(static_cast<double>(lo), static_cast<double>(hi), false);
    }
    std::uint64_t lo, hi;
    UBounds(a, lo, hi);
    return FRange(static_cast<double>(lo), static_cast<double>(hi), false);
}

inline std::uint32_t FloatBits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float BitsFloat(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

/// OpBitcast u32 -> f32.
inline Flt FFromBits(const Int& a) {
    if (a.kind == Int::Kind::Set) {
        float v[kMaxSet];
        for (std::uint32_t i = 0; i < a.count; ++i) {
            v[i] = BitsFloat(a.vals[i]);
        }
        return FValues(v, a.count, false); // Top if any pattern is NaN or infinite
    }
    std::uint64_t lo, hi;
    UBounds(a, lo, hi);
    if (hi <= 0x7F7FFFFFull) {
        // Non-negative finite patterns: the float order is the integer order.
        return FRangeExact(BitsFloat(static_cast<std::uint32_t>(lo)), BitsFloat(static_cast<std::uint32_t>(hi)),
                           false);
    }
    return FTop();
}

/// OpBitcast f32 -> u32: exact for a set of exact floats; for a sign-definite interval the bit patterns are ordered
/// as the magnitudes. -0.0 is possible whenever 0 is.
inline Int BitsFromF(const Flt& a) {
    if (a.top || a.nan) {
        return Top();
    }
    if (a.count != 0) {
        std::uint32_t v[kMaxFltSet];
        for (std::uint32_t i = 0; i < a.count; ++i) {
            v[i] = FloatBits(a.vals[i]);
        }
        return FromValues(v, v + a.count);
    }
    if (a.lo >= 0.0) {
        const Int pos = Range(FloatBits(detail::FloatDown(a.lo)), FloatBits(detail::FloatUp(a.hi)));
        return a.lo == 0.0 ? Union(pos, Const(0x80000000u)) : pos;
    }
    if (a.hi <= 0.0) {
        const Int neg = Range(std::int64_t{0x80000000} + FloatBits(detail::FloatDown(-a.hi)),
                              std::int64_t{0x80000000} + FloatBits(detail::FloatUp(-a.lo)));
        return a.hi == 0.0 ? Union(neg, Const(0)) : neg;
    }
    return Top();
}

/// OpConvertFToU / OpConvertFToS, truncating; NaN gives 0 and out-of-range values saturate (hardware assumption, see
/// the header comment).
inline Int IntFromF(const Flt& a_in, bool is_signed) {
    const Flt a = ArithView(a_in);
    if (a.top) {
        return Top();
    }
    const double min = is_signed ? -2147483648.0 : 0.0;
    const double max = is_signed ? 2147483647.0 : 4294967295.0;
    const auto convert = [&](double x) {
        return static_cast<std::int64_t>(std::clamp(std::trunc(x), min, max));
    };
    if (a.count != 0) {
        std::uint32_t v[kMaxFltSet + 1];
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < a.count; ++i) {
            v[n++] = static_cast<std::uint32_t>(convert(a.vals[i]));
        }
        if (a.nan) {
            v[n++] = 0;
        }
        return FromValues(v, v + n);
    }
    std::int64_t lo = convert(a.lo);
    std::int64_t hi = convert(a.hi);
    if (a.nan) {
        lo = std::min<std::int64_t>(lo, 0);
        hi = std::max<std::int64_t>(hi, 0);
    }
    return Range(lo, hi);
}

} // namespace Shader::Optimization::SotcExtent
