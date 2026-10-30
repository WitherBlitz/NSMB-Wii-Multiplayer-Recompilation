// Native NW4R math leaves. These remove a translated call and its
// instrumentation from paths the game hits every frame.
#include "hle_stubs.h"

// nw4r::g3d::PlayPolicy::Onetime: returns the third argument unchanged.
extern "C" void PlayPolicyOnetime_HLE(CpuContext* ctx) {
    ctx->fpr[1].d = ctx->fpr[3].d;
}
PPC_NATIVE_OVERRIDE_VOID(80249BE0, PlayPolicyOnetime_HLE, (CpuContext* ctx), (ctx));
