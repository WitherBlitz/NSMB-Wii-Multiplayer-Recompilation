// Native PSMTXConcat (0x801C0540). The guest SDK computes the same 3x4 affine
// multiply with paired-single loads; rows are read into locals first so the
// destination may alias either input.
#include "hle_stubs.h"
#include "memory.h"
#include "memory_access.h"

#include <cstdint>

extern "C" void PSMTXConcat_HLE_801c0540(uint32_t a, uint32_t b, uint32_t ab) {

    float av[12];
    float bv[12];
    float out[12];
    for (int i = 0; i < 12; ++i) {
        av[i] = MemoryInline::FlatReadFloat32(a + static_cast<uint32_t>(i) * 4u);
        bv[i] = MemoryInline::FlatReadFloat32(b + static_cast<uint32_t>(i) * 4u);
    }

    for (int row = 0; row < 3; ++row) {
        const float* ar = av + row * 4;
        float* o = out + row * 4;
        // Separate steps keep single-precision rounding without FMA contraction.
        const float x0 = ar[0] * bv[0];
        const float x1 = ar[1] * bv[4];
        const float x2 = ar[2] * bv[8];
        o[0] = x0 + x1 + x2;

        const float y0 = ar[0] * bv[1];
        const float y1 = ar[1] * bv[5];
        const float y2 = ar[2] * bv[9];
        o[1] = y0 + y1 + y2;

        const float z0 = ar[0] * bv[2];
        const float z1 = ar[1] * bv[6];
        const float z2 = ar[2] * bv[10];
        o[2] = z0 + z1 + z2;

        const float w0 = ar[0] * bv[3];
        const float w1 = ar[1] * bv[7];
        const float w2 = ar[2] * bv[11];
        o[3] = ar[3] + w0 + w1 + w2;
    }

    for (int i = 0; i < 12; ++i) {
        MemoryInline::FlatWriteFloat32(ab + static_cast<uint32_t>(i) * 4u, out[i]);
    }
}

PPC_NATIVE_OVERRIDE_VOID(801C0540, PSMTXConcat_HLE_801c0540, (uint32_t a, uint32_t b, uint32_t ab), (a, b, ab));
