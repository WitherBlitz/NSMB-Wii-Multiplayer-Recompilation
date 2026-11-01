// Native math leaves. These remove a translated call and its instrumentation from paths the
// game hits every frame.
#include "hle_stubs.h"

#include <cmath>

// nw4r::g3d::PlayPolicy::Onetime: returns the third argument unchanged.
extern "C" void PlayPolicyOnetime_HLE(CpuContext* ctx) {
    ctx->fpr[1].d = ctx->fpr[3].d;
}
PPC_NATIVE_OVERRIDE_VOID(80249BE0, PlayPolicyOnetime_HLE, (CpuContext* ctx), (ctx));

// __ieee754_sqrt: the guest's software double square root. The host sqrt is correctly rounded
// too, so this is bit-identical for every input the game feeds it.
extern "C" double Ieee754Sqrt_HLE(double value) {
    return std::sqrt(value);
}
PPC_NATIVE_OVERRIDE(802E8620, Ieee754Sqrt_HLE, double, (double value), (value));
