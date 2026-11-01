// Native nw4r::g3d animation leaves. These replace translated code that runs once per bone
// per model per frame, so the interpreter overhead around a few float operations dominated.
#include "hle_stubs.h"

#include "memory_access.h"
#include "ppc_runtime.h"

#include <cmath>

namespace {

constexpr uint32_t kFvsFrameValuesOffset = 0x10;

// The guest is big-endian: the frame index is the first byte at the entry address, which is
// the high byte of the value read back through FlatRead32.
inline uint8_t FvsFrameIndex(uint32_t entry) {
    return static_cast<uint8_t>(entry >> 24);
}

// A frame entry packs a u8 frame index, a 12-bit value and a signed 12-bit slope (5 fraction bits).
inline float FvsFrameValue(uint32_t entry) {
    return static_cast<float>((entry >> 12) & 0xFFFu);
}

inline float FvsFrameSlope(uint32_t entry) {
    const int32_t slope = static_cast<int32_t>(entry << 20) >> 20;
    return static_cast<float>(slope) / 32.0f;
}

// Single-precision subtract with the hardware's flush rules (no inline PpcFsubs exists).
inline double FvsSubS(double a, double b) {
    return static_cast<double>(PpcForceSingleValueInline(a - b));
}

// Gekko fres refined once, as nw4r math::FInv does: inv = 2r - x*r*r.
inline double FvsRefinedReciprocal(double value) {
    const float x = static_cast<float>(value);
    const float estimate = static_cast<float>(PpcGetPs0Inline(PPC_Fres(PpcPackPairedInline(x, x))));
    const float squared = estimate * estimate;
    return PpcForceSingleValueInline(static_cast<double>(std::fma(-x, squared, estimate + estimate)));
}

inline uint8_t FvsQuantizeFrame(double frame) {
    const int32_t value = static_cast<int32_t>(frame);
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

inline uint16_t FvsQuantizeEstimate(double estimate) {
    const int32_t value = static_cast<int32_t>(estimate);
    return static_cast<uint16_t>(value < 0 ? 0 : (value > 65535 ? 65535 : value));
}

}  // namespace

// f32 CalcAnimationFVS<FVS32>(const ResAnmChrFVSData* data, f32 frame)
extern "C" double CalcAnimationFVS_HLE(uint32_t base, double frameArg) {
    const double frame = frameArg;
    const uint16_t count = MemoryInline::FlatRead16(base);
    const double invKeyFrameRange = MemoryInline::FlatReadFloat32(base + 4);
    const double scale = MemoryInline::FlatReadFloat32(base + 8);
    const double offset = MemoryInline::FlatReadFloat32(base + 12);
    const uint32_t first = MemoryInline::FlatRead32(base + kFvsFrameValuesOffset);
    const uint32_t last =
        MemoryInline::FlatRead32(base + kFvsFrameValuesOffset + static_cast<uint32_t>(count - 1) * 4u);
    const double firstFrame = static_cast<double>(FvsFrameIndex(first));
    const double lastFrame = static_cast<double>(FvsFrameIndex(last));

    const auto frameValue = [&](uint32_t entry) {
        return PpcFmaddsInline(scale, static_cast<double>(FvsFrameValue(entry)), offset);
    };

    double result;
    if (frame <= firstFrame) {
        result = frameValue(first);
    } else if (lastFrame <= frame) {
        result = frameValue(last);
    } else {
        const double frameOffset = FvsSubS(frame, firstFrame);
        const double estimate =
            PpcFmulsInline(invKeyFrameRange,
                           PpcFmulsInline(frameOffset, static_cast<double>(count)));
        uint32_t index = FvsQuantizeEstimate(estimate);

        const uint8_t quantized = FvsQuantizeFrame(frame);
        uint32_t left = MemoryInline::FlatRead32(base + kFvsFrameValuesOffset + index * 4u);
        if (quantized < FvsFrameIndex(left)) {
            do {
                --index;
                left = MemoryInline::FlatRead32(base + kFvsFrameValuesOffset + index * 4u);
            } while (quantized < FvsFrameIndex(left));
        } else {
            do {
                ++index;
                left = MemoryInline::FlatRead32(base + kFvsFrameValuesOffset + index * 4u);
            } while (FvsFrameIndex(left) <= quantized);
            --index;
            left = MemoryInline::FlatRead32(base + kFvsFrameValuesOffset + index * 4u);
        }

        const double leftValue = frameValue(left);
        if (frame == static_cast<double>(FvsFrameIndex(left))) {
            result = leftValue;
        } else {
            const uint32_t right =
                MemoryInline::FlatRead32(base + kFvsFrameValuesOffset + (index + 1u) * 4u);
            const double rightValue = frameValue(right);
            const double frameDelta = FvsSubS(frame, static_cast<double>(FvsFrameIndex(left)));
            const double keyFrameDelta =
                FvsSubS(static_cast<double>(FvsFrameIndex(right)),
                               static_cast<double>(FvsFrameIndex(left)));
            const double inv = FvsRefinedReciprocal(keyFrameDelta);
            const double s = PpcFmulsInline(frameDelta, inv);
            const double s1 = PpcFmsubsInline(frameDelta, inv, 1.0);
            const double twoS = PpcFmsubsInline(2.0, s, 3.0);
            const double u = PpcFmaddsInline(
                s1, static_cast<double>(FvsFrameSlope(left)), PpcFmulsInline(s, FvsFrameSlope(right)));
            const double curve = PpcFmulsInline(
                s, PpcFmulsInline(twoS, FvsSubS(leftValue, rightValue)));
            result = PpcFmaddsInline(PpcFmulsInline(frameDelta, s1), u,
                                     PpcFmaddsInline(s, curve, leftValue));
        }
    }

    return result;
}
PPC_NATIVE_OVERRIDE(802403C0, CalcAnimationFVS_HLE, double, (uint32_t base, double frameArg), (base, frameArg));
