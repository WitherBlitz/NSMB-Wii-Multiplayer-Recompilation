// Native nw4r::g3d rotation leaf: math::MTX34RotXYZFIdx plus the fixed-point sine/cosine
// table lookup it uses. The formula and table layout follow the guest body and the decomp
// (ogws/math_types.cpp, math_triangular.cpp).
//
// 0x802382B0 math::MTX34RotXYZFIdx
#include "hle_stubs.h"

#include "memory.h"

#include <cmath>
#include <cstdint>

namespace {

constexpr uint32_t kSinCosTable = 0x802F74A0;  // detail::gSinCosTbl, 256 x 16 bytes

float ReadF32(uint32_t addr) {
    return MemoryInline::FlatReadFloat32(addr);
}

// math::SinCosFIdx: range reduce in table units, look up the sample and interpolate.
void SinCosFIdx(float fidx, float* outSin, float* outCos) {
    float magnitude = std::fabs(fidx);
    while (magnitude > 65536.0f) {
        magnitude -= 65536.0f;
    }
    const uint16_t whole = static_cast<uint16_t>(static_cast<int32_t>(magnitude));
    const float frac = magnitude - static_cast<float>(whole);
    const uint32_t entry = kSinCosTable + (whole & 0xFFu) * 16u;
    const float sinValue = ReadF32(entry);
    const float cosValue = ReadF32(entry + 4);
    const float sinDelta = ReadF32(entry + 8);
    const float cosDelta = ReadF32(entry + 12);
    const float sine = std::fma(frac, sinDelta, sinValue);
    *outCos = std::fma(frac, cosDelta, cosValue);
    *outSin = (fidx < 0.0f) ? -sine : sine;
}

// math::MTX34RotXYZFIdx, operation order from the guest body.
void RotXYZFIdxNative(uint32_t mtx, float fx, float fy, float fz) {
    float sx, cx, sy, cy, sz, cz;
    SinCosFIdx(fx, &sx, &cx);
    SinCosFIdx(fy, &sy, &cy);
    SinCosFIdx(fz, &sz, &cz);

    const float cx_sz = cx * sz;
    const float sx_cz = sx * cz;
    const float sx_sz = sx * sz;
    const float cx_cz = cx * cz;

    MemoryInline::FlatWriteFloat32(mtx + 0x00, cz * cy);
    MemoryInline::FlatWriteFloat32(mtx + 0x04, std::fma(sx_cz, sy, -cx_sz));
    MemoryInline::FlatWriteFloat32(mtx + 0x08, std::fma(cx_cz, sy, sx_sz));
    MemoryInline::FlatWriteFloat32(mtx + 0x0C, 0.0f);
    MemoryInline::FlatWriteFloat32(mtx + 0x10, sz * cy);
    MemoryInline::FlatWriteFloat32(mtx + 0x14, std::fma(sx_sz, sy, cx_cz));
    MemoryInline::FlatWriteFloat32(mtx + 0x18, std::fma(cx_sz, sy, -sx_cz));
    MemoryInline::FlatWriteFloat32(mtx + 0x1C, 0.0f);
    MemoryInline::FlatWriteFloat32(mtx + 0x20, -sy);
    MemoryInline::FlatWriteFloat32(mtx + 0x24, cy * sx);
    MemoryInline::FlatWriteFloat32(mtx + 0x28, cy * cx);
    MemoryInline::FlatWriteFloat32(mtx + 0x2C, 0.0f);
}

}  // namespace

extern "C" void NW4R_G3D_RotXYZFIdx_HLE(CpuContext* ctx) {
    const uint32_t mtx = ctx->gpr[3];
    const float fx = static_cast<float>(ctx->fpr[1].d);
    const float fy = static_cast<float>(ctx->fpr[2].d);
    const float fz = static_cast<float>(ctx->fpr[3].d);
    RotXYZFIdxNative(mtx, fx, fy, fz);
}

PPC_NATIVE_OVERRIDE_VOID(802382B0, NW4R_G3D_RotXYZFIdx_HLE, (CpuContext* ctx), (ctx));
