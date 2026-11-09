#pragma once

// Correctly rounded fused multiply-add without the FMA instruction (x86-64 CPUs older than Haswell /
// Excavator, which the x86-64-v2 "compat" build targets). The PowerPC's fmadd/fmsub family and the
// paired-single ps_madd family round once; a*b+c computed in two steps rounds twice and now and then
// gives different bits, which would make a v2 PC disagree with every other device in a deterministic
// LAN game. Fma and Fmaf give exactly what the FMA instruction gives (round to nearest, ties to even);
// runtime/tests/soft_fma_test.cpp checks them against it.
//
// How: both operands of the product and the addend are unpacked to integers, the product is formed
// exactly (128 bits), the addend aligned to it (bits shifted out kept as a sticky bit), the two added
// or subtracted exactly, and the result rounded once into the target format.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace PpcSoftFma {

using u128 = unsigned __int128;

inline uint64_t Bits(double x) {
    uint64_t i;
    std::memcpy(&i, &x, sizeof i);
    return i;
}

inline uint32_t Bits32(float x) {
    uint32_t i;
    std::memcpy(&i, &x, sizeof i);
    return i;
}

inline int Clz128(u128 x) {
    const uint64_t hi = static_cast<uint64_t>(x >> 64), lo = static_cast<uint64_t>(x);
    return hi != 0 ? __builtin_clzll(hi) : lo != 0 ? 64 + __builtin_clzll(lo) : 128;
}

// A finite nonzero double as m * 2^e, m an integer with its leading one at bit 52.
struct Unpacked {
    uint64_t m = 0;
    int e = 0;
    bool negative = false;
};

inline Unpacked Unpack(double x) {
    const uint64_t i = Bits(x);
    Unpacked u;
    u.negative = (i >> 63) != 0;
    const int exponent = static_cast<int>((i >> 52) & 0x7FF);
    const uint64_t fraction = i & ((uint64_t{1} << 52) - 1);
    if (exponent == 0) {
        u.m = fraction;  // subnormal: no leading one
        u.e = 1 - 1023 - 52;
    } else {
        u.m = fraction | (uint64_t{1} << 52);
        u.e = exponent - 1023 - 52;
    }
    return u;
}

// x shifted right by n, the bits shifted out ORed into bit 0 (a sticky bit).
inline u128 ShiftRightSticky(u128 x, int n) {
    if (n <= 0) {
        return x;
    }
    if (n >= 128) {
        return x != 0 ? 1 : 0;
    }
    const u128 lost = x & ((u128{1} << n) - 1);
    return (x >> n) | (lost != 0 ? 1 : 0);
}

// The exact value magnitude * 2^e (magnitude nonzero, its bit 0 possibly a sticky bit standing for
// nonzero bits below it), rounded once to nearest even into an IEEE format with `fractionBits`
// fraction bits and exponent bias `bias`; returned as that format's bit pattern.
//
// flushTiny (x86 flush-to-zero, set while the PowerPC's non-IEEE mode is on): a result that is tiny is
// replaced by a zero of its sign. Tiny is judged as x86 does (IEEE's "after rounding"): the value
// rounded to the format's full precision, as if the exponent had no lower limit, is below the smallest
// normal number. So a value just under it that rounds up to it is kept, and nothing else under it is.
inline uint64_t Round(u128 magnitude, int e, bool negative, int fractionBits, int bias, int maxBiased,
                      bool flushTiny = false) {
    // Leading one to bit 127; anything shifted in below is zero, the sticky bit stays a sticky bit.
    const int shift = Clz128(magnitude);
    magnitude <<= shift;
    e -= shift;
    // Leading bit is worth 2^(e + 127).
    int biased = e + 127 + bias;
    int keep = fractionBits + 1;  // significand bits that survive, the leading one included
    const uint64_t signBit = negative ? uint64_t{1} << (fractionBits + (maxBiased == 0x7FF ? 11 : 8)) : 0;
    if (flushTiny && biased <= 0) {
        if (biased < 0) {
            return signBit;  // below half the smallest normal: tiny however it rounds
        }
        // Leading bit one place under the smallest normal: tiny unless rounding to full precision
        // carries it up to the smallest normal itself.
        const int drop = 128 - (fractionBits + 1);
        const uint64_t full = static_cast<uint64_t>(magnitude >> drop);
        const u128 rest = magnitude & ((u128{1} << drop) - 1);
        const u128 half = u128{1} << (drop - 1);
        const bool up = rest > half || (rest == half && (full & 1) != 0);
        if (!up || ((full + 1) >> (fractionBits + 1)) == 0) {
            return signBit;
        }
        return (uint64_t{1} << fractionBits) | signBit;  // the smallest normal number
    }
    if (biased <= 0) {
        keep = fractionBits + biased;  // a subnormal keeps fewer
        biased = 0;
    }
    uint64_t significand = 0;
    if (keep > 0) {
        const int drop = 128 - keep;
        significand = static_cast<uint64_t>(magnitude >> drop);
        const u128 rest = magnitude & ((u128{1} << drop) - 1);
        const u128 half = u128{1} << (drop - 1);
        if (rest > half || (rest == half && (significand & 1) != 0)) {
            ++significand;
        }
    } else if (keep == 0) {
        // Only the rounding bit is left: half the smallest subnormal or more rounds up (ties to the
        // even zero below unless anything else is set).
        const u128 half = u128{1} << 127;
        significand = magnitude > half ? 1 : 0;
    }
    uint64_t bits;
    if (biased == 0) {
        bits = significand;  // a carry into the leading position is the smallest normal number
    } else {
        if ((significand >> (fractionBits + 1)) != 0) {
            significand >>= 1;
            ++biased;
        }
        if (biased >= maxBiased) {
            bits = static_cast<uint64_t>(maxBiased) << fractionBits;  // infinity
        } else {
            bits = (static_cast<uint64_t>(biased) << fractionBits) |
                   (significand & ((uint64_t{1} << fractionBits) - 1));
        }
    }
    return bits | signBit;
}

// The exact a*b + c for finite nonzero a, b and finite c, as magnitude * 2^e with its sign.
// false if it is exactly zero.
inline bool ExactSum(double a, double b, double c, u128& magnitude, int& e, bool& negative) {
    const Unpacked ua = Unpack(a), ub = Unpack(b);
    u128 p = static_cast<u128>(ua.m) * ub.m;  // below 2^106
    int pe = ua.e + ub.e;
    const bool pneg = ua.negative != ub.negative;
    if (c == 0.0) {
        magnitude = p;
        e = pe;
        negative = pneg;
        return p != 0;
    }
    const Unpacked uc = Unpack(c);
    u128 q = uc.m;
    int qe = uc.e;
    // Both to the top of 127 bits (room for the sum's carry), then the smaller one aligned down.
    const int pShift = Clz128(p) - 1, qShift = Clz128(q) - 1;
    p <<= pShift;
    pe -= pShift;
    q <<= qShift;
    qe -= qShift;
    if (pe > qe) {
        q = ShiftRightSticky(q, pe - qe);
        e = pe;
    } else {
        p = ShiftRightSticky(p, qe - pe);
        e = qe;
    }
    if (pneg == uc.negative) {
        magnitude = p + q;
        negative = pneg;
    } else if (p >= q) {
        magnitude = p - q;
        negative = pneg;
    } else {
        magnitude = q - p;
        negative = uc.negative;
    }
    // A subtraction can only lose bits above the sticky bit when the exponents were close, and then
    // nothing was shifted out: a sticky bit never stands in for a cancelled value.
    return magnitude != 0;
}

// Denormal inputs read as zero (x86 denormals-are-zero, set with flush-to-zero in non-IEEE mode).
inline double DenormalAsZero(double x) {
    return std::fpclassify(x) == FP_SUBNORMAL ? std::copysign(0.0, x) : x;
}

// Fast paths. Most of the game's multiply-adds take single-precision values (paired singles, and
// doubles loaded from floats), whose product a double holds exactly; the one rounding then happens
// in the add, so plain multiply-then-add gives the fused result bit for bit. The exact unpacking
// below is only needed when the product does not fit, or (for floats) when the double sum lands
// exactly halfway between two floats, where rounding it again could go the wrong way.

// a*b is exact in a double: both normal, their significant bits fit in 53, and the product's
// exponent stays inside the double normal range (so neither flush-to-zero nor denormals-are-zero can
// touch it).
inline bool ProductIsExact(double a, double b) {
    const uint64_t ia = Bits(a), ib = Bits(b);
    const int ea = static_cast<int>((ia >> 52) & 0x7FF), eb = static_cast<int>((ib >> 52) & 0x7FF);
    if (ea == 0 || eb == 0 || ea == 0x7FF || eb == 0x7FF) {
        return false;  // zero, subnormal, infinite or NaN: the exact path's special cases
    }
    const int e = ea + eb - 2 * 1023;  // the product is in [2^e, 2^(e+2))
    if (e < -1000 || e > 1000) {
        return false;
    }
    constexpr uint64_t kLead = uint64_t{1} << 52;
    const int bitsA = 53 - __builtin_ctzll((ia & (kLead - 1)) | kLead);
    const int bitsB = 53 - __builtin_ctzll((ib & (kLead - 1)) | kLead);
    return bitsA + bitsB <= 53;
}

// The slow (exact) paths, kept out of line so the fast paths inline into translated code.
[[gnu::noinline]] inline double FmaExact(double a, double b, double c, bool flush);
[[gnu::noinline]] inline float FmafExact(float a, float b, float c, bool flush);

// fma(a, b, c) for doubles. `flush`: the x86 flush-to-zero and denormals-are-zero modes are on.
inline double Fma(double a, double b, double c, bool flush = false) {
#pragma clang fp contract(off)
    if (ProductIsExact(a, b)) [[likely]] {
        // a and b are normal, so denormals-are-zero leaves them alone; it and flush-to-zero act on c
        // and on the sum exactly as the exact path models them (it is checked against the
        // instruction in both modes). A NaN or infinite c comes out as the exact path gives it too.
        return a * b + c;
    }
    if ((a == 0.0 || b == 0.0) && std::isfinite(a) && std::isfinite(b)) {
        // A zero product (matrices are full of zeros): the exact path's own a * b + c. The FP mode
        // treats a subnormal operand here just as the exact path's denormals-are-zero step does.
        return a * b + c;
    }
    return FmaExact(a, b, c, flush);
}

// fmaf(a, b, c) for floats (each lane of the paired-single ps_madd family).
inline float Fmaf(float a, float b, float c, bool flush = false) {
#pragma clang fp contract(off)
    // Normal nonzero finite a and b (flush only changes subnormals, which go the exact way).
    const uint32_t ia = Bits32(a), ib = Bits32(b);
    const uint32_t ea = (ia >> 23) & 0xFF, eb = (ib >> 23) & 0xFF;
    const uint32_t ic = Bits32(c), ec = (ic >> 23) & 0xFF;
    if (ea != 0 && eb != 0 && ea != 0xFF && eb != 0xFF && ec != 0xFF && (ec != 0 || (ic << 1) == 0)) [[likely]] {
        // c normal or zero: the product of two floats is exact in a double, the sum rounds once.
        const double s = static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c);
        const uint64_t u = Bits(s);
        // Rounding s to a float is right unless s is exactly halfway between two floats (then the
        // part the first rounding dropped decides) or s is below the smallest normal float (where
        // floats are spaced wider and halfway points sit elsewhere). Sums that fit (not halfway with
        // a nonzero remainder) are unaffected either way, so these just take the exact path.
        if ((u & 0x1FFFFFFFull) != 0x10000000ull && (u & 0x7FFFFFFFFFFFFFFFull) >= 0x3810000000000000ull) [[likely]] {
            return static_cast<float>(s);
        }
    } else if ((a == 0.0f || b == 0.0f) && ea != 0xFF && eb != 0xFF) {
        // A zero product (matrices are full of zeros): the exact path's own a * b + c. The FP mode
        // treats a subnormal operand here just as the exact path's denormals-are-zero step does.
        return a * b + c;
    }
    return FmafExact(a, b, c, flush);
}

inline double FmaExact(double a, double b, double c, bool flush) {
    if (flush) {
        a = DenormalAsZero(a);
        b = DenormalAsZero(b);
        c = DenormalAsZero(c);
    }
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || a == 0.0 || b == 0.0) {
        if (std::isfinite(a) && std::isfinite(b) && !std::isfinite(c)) {
            return c + c;  // the finite product cannot change an infinite or NaN c (a NaN is quieted)
        }
        return a * b + c;  // the product is exact here (zero, infinite or NaN): one rounding
    }
    u128 magnitude;
    int e;
    bool negative;
    if (!ExactSum(a, b, c, magnitude, e, negative)) {
        return 0.0;  // exact cancellation is +0 when rounding to nearest
    }
    const uint64_t bits = Round(magnitude, e, negative, 52, 1023, 0x7FF, flush);
    double r;
    std::memcpy(&r, &bits, sizeof r);
    return r;
}

inline float FmafExact(float a, float b, float c, bool flush) {
    if (flush) {
        a = std::fpclassify(a) == FP_SUBNORMAL ? std::copysign(0.0f, a) : a;
        b = std::fpclassify(b) == FP_SUBNORMAL ? std::copysign(0.0f, b) : b;
        c = std::fpclassify(c) == FP_SUBNORMAL ? std::copysign(0.0f, c) : c;
    }
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || a == 0.0f || b == 0.0f) {
        if (std::isfinite(a) && std::isfinite(b) && !std::isfinite(c)) {
            return c + c;
        }
        return a * b + c;
    }
    u128 magnitude;
    int e;
    bool negative;
    if (!ExactSum(a, b, c, magnitude, e, negative)) {
        return 0.0f;
    }
    const uint32_t bits = static_cast<uint32_t>(Round(magnitude, e, negative, 23, 127, 0xFF, flush));
    float r;
    std::memcpy(&r, &bits, sizeof r);
    return r;
}

} // namespace PpcSoftFma
