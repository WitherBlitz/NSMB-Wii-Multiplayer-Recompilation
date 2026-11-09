// Checks PpcSoftFma::Fma / Fmaf (runtime/include/isa/ppc_soft_fma.h) bit for bit against the FMA
// instruction. Build on an x86-64 machine with FMA, e.g.:
//   clang++ -O2 -std=c++20 -mfma -I runtime/include runtime/tests/soft_fma_test.cpp -o soft_fma_test
//   soft_fma_test [millions of cases per kind, default 20]
// NaN results only have to be NaN (which NaN the hardware returns depends on the instruction form
// the compiler picks).

#include "isa/ppc_soft_fma.h"

#include <immintrin.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

double HardFma(double a, double b, double c) {
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
}

float HardFmaf(float a, float b, float c) {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}

template <typename T, typename U>
U BitsOf(T x) {
    U u;
    std::memcpy(&u, &x, sizeof u);
    return u;
}

template <typename T, typename U>
T FromBits(U u) {
    T x;
    std::memcpy(&x, &u, sizeof x);
    return x;
}

std::mt19937_64 g_rng(12345);

// Doubles from several distributions: uniform bits, a narrow exponent band (cancellation), tiny
// exponents (subnormal results), huge ones (overflow), and special values.
double RandomDouble(int kind) {
    const uint64_t r = g_rng();
    switch (kind) {
    case 0:
        return FromBits<double>(r);
    case 1: {
        const uint64_t exponent = 1023 + (r >> 60) - 8;
        return FromBits<double>((r & 0x800FFFFFFFFFFFFFull) | (exponent << 52));
    }
    case 2: {
        const uint64_t exponent = (r >> 53) % 120;
        return FromBits<double>((r & 0x800FFFFFFFFFFFFFull) | (exponent << 52));
    }
    case 3: {
        const uint64_t exponent = 2047 - 1 - (r >> 53) % 60;
        return FromBits<double>((r & 0x800FFFFFFFFFFFFFull) | (exponent << 52));
    }
    default: {
        static const double specials[] = {0.0, -0.0, 1.0, -1.0, INFINITY, -INFINITY, NAN, 4.9e-324,
                                          2.2250738585072014e-308, 1.7976931348623157e308, 0.5, 3.0};
        return specials[r % (sizeof specials / sizeof specials[0])];
    }
    }
}

float RandomFloat(int kind) {
    const uint32_t r = static_cast<uint32_t>(g_rng());
    switch (kind) {
    case 0:
        return FromBits<float>(r);
    case 1: {
        const uint32_t exponent = 127 + (r >> 28) - 8;
        return FromBits<float>((r & 0x807FFFFFu) | (exponent << 23));
    }
    case 2: {
        const uint32_t exponent = (r >> 24) % 40;
        return FromBits<float>((r & 0x807FFFFFu) | (exponent << 23));
    }
    case 3: {
        const uint32_t exponent = 255 - 1 - (r >> 24) % 30;
        return FromBits<float>((r & 0x807FFFFFu) | (exponent << 23));
    }
    default: {
        static const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, INFINITY, -INFINITY, NAN, 1.4e-45f,
                                         1.17549435e-38f, 3.40282347e38f, 0.5f, 3.0f};
        return specials[r % (sizeof specials / sizeof specials[0])];
    }
    }
}

// a*b nearly cancelling c: c is -(a*b) rounded, nudged by a few ulps.
template <typename T>
T NearNegatedProduct(T a, T b) {
    T c = -(a * b);
    const int nudge = static_cast<int>(g_rng() % 7) - 3;
    for (int i = 0; i < std::abs(nudge); ++i) {
        c = std::nextafter(c, nudge > 0 ? INFINITY : -INFINITY);
    }
    return c;
}

} // namespace

int main(int argc, char** argv) {
    const long millions = argc > 1 ? std::atol(argv[1]) : 20;
    // "flush" as a second argument: run with the x86 flush-to-zero and denormals-are-zero modes on (the
    // runtime's non-IEEE mode) and check the soft versions' flush handling.
    const bool flush = argc > 2 && std::strcmp(argv[2], "flush") == 0;
    if (flush) {
        _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
    }
    const long perKind = millions * 1000000 / 6;
    long failures = 0, cases = 0;
    for (int kind = 0; kind < 6; ++kind) {
        for (long i = 0; i < perKind; ++i) {
            const int k = kind == 5 ? static_cast<int>(g_rng() % 5) : kind;
            const double a = RandomDouble(k), b = RandomDouble(kind == 5 ? static_cast<int>(g_rng() % 5) : k);
            const double c = kind == 1 && (i & 1) ? NearNegatedProduct(a, b) : RandomDouble(kind == 5 ? 4 : k);
            const double hard = HardFma(a, b, c), soft = PpcSoftFma::Fma(a, b, c, flush);
            ++cases;
            if (!(std::isnan(hard) && std::isnan(soft)) && BitsOf<double, uint64_t>(hard) != BitsOf<double, uint64_t>(soft)) {
                if (++failures <= 20) {
                    std::printf("double fma(%a, %a, %a): hardware %a, soft %a\n", a, b, c, hard, soft);
                }
            }
            const float fa = RandomFloat(k), fb = RandomFloat(kind == 5 ? static_cast<int>(g_rng() % 5) : k);
            const float fc = kind == 1 && (i & 1) ? NearNegatedProduct(fa, fb) : RandomFloat(kind == 5 ? 4 : k);
            const float fhard = HardFmaf(fa, fb, fc), fsoft = PpcSoftFma::Fmaf(fa, fb, fc, flush);
            ++cases;
            if (!(std::isnan(fhard) && std::isnan(fsoft)) &&
                BitsOf<float, uint32_t>(fhard) != BitsOf<float, uint32_t>(fsoft)) {
                if (++failures <= 40) {
                    std::printf("float fmaf(%a, %a, %a): hardware %a, soft %a\n", fa, fb, fc, fhard, fsoft);
                }
            }
        }
    }
    // The fast paths' territory: doubles that hold floats (and the 25-bit operands of fmadds), and
    // float sums steered onto or next to a halfway point between two floats.
    for (long i = 0; i < perKind * 2; ++i) {
        const int k = static_cast<int>(g_rng() % 5);
        const double a = RandomFloat(k);
        double b = RandomFloat(static_cast<int>(g_rng() % 5));
        if (i & 1) {
            b = FromBits<double>(BitsOf<double, uint64_t>(b) | ((g_rng() & 1) << 28));  // a 25th bit
        }
        const double c = (i & 2) ? static_cast<double>(RandomFloat(k)) : (i & 4) ? NearNegatedProduct(a, b) : RandomDouble(k);
        const double hard = HardFma(a, b, c), soft = PpcSoftFma::Fma(a, b, c, flush);
        ++cases;
        if (!(std::isnan(hard) && std::isnan(soft)) && BitsOf<double, uint64_t>(hard) != BitsOf<double, uint64_t>(soft)) {
            if (++failures <= 60) {
                std::printf("double fma(%a, %a, %a): hardware %a, soft %a\n", a, b, c, hard, soft);
            }
        }
        const float fa = RandomFloat(k), fb = RandomFloat(static_cast<int>(g_rng() % 5));
        // c that puts a*b + c on (or a hair from) the halfway point between the floats around a*b.
        const double p = static_cast<double>(fa) * static_cast<double>(fb);
        const float nearest = static_cast<float>(p);
        const float other = std::nextafter(nearest, (static_cast<double>(nearest) < p) ? INFINITY : -INFINITY);
        const double half = (static_cast<double>(nearest) + static_cast<double>(other)) / 2.0;
        float fc = static_cast<float>(half - p);
        if (i & 8) {
            fc = std::nextafter(fc, (i & 16) ? INFINITY : -INFINITY);
        }
        const float fhard = HardFmaf(fa, fb, fc), fsoft = PpcSoftFma::Fmaf(fa, fb, fc, flush);
        ++cases;
        if (!(std::isnan(fhard) && std::isnan(fsoft)) && BitsOf<float, uint32_t>(fhard) != BitsOf<float, uint32_t>(fsoft)) {
            if (++failures <= 80) {
                std::printf("float fmaf(%a, %a, %a): hardware %a, soft %a\n", fa, fb, fc, fhard, fsoft);
            }
        }
    }
    std::printf("%ld cases, %ld mismatches\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
