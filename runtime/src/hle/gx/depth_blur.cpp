// Depth Blur off (Video settings; [video] disabled_post_processing_paths bit 0x10): the courses' depth
// of field and heat-haze / underwater shimmer, EGG::DrawPathDOF (dDOF_c), goes unseen. The desert,
// beach, underwater, lava and Bowser areas use it (dCaptureMng_c picks it per area; the others never
// enable it). Its internalDraw (0x802C3CB0, reached only through the draw path's vtable) runs in three
// steps: 0 captures the picture and builds the blurred copy in EFB work space, 1 puts the work space
// back, and 2 draws the blurred, shimmering copy over the picture: one full-screen DrawDL. Off, step 2
// still runs on the guest exactly as it does on, but aurora skips its primitives (AuroraSetDrawDrop),
// so every byte of guest memory is the same either way: render-only, and each LAN player picks freely.
#include "abi_bridge.h"
#include "game_graphics_options.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>

#include <chrono>
#include <cstdlib>

extern "C" void func_802C3CB0(CpuContext* ctx);  // the translated EGG::DrawPathDOF::internalDraw

namespace {
constexpr uint32_t kDepthBlurOffBit = 0x10u;

// NSMBW_LOG_DEPTH_BLUR: a line when the depth blur starts drawing (after a second without it), to
// see which courses use it.
void LogUse(bool dropped) {
    static const bool log = std::getenv("NSMBW_LOG_DEPTH_BLUR") != nullptr;
    if (!log) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{};
    static uint64_t calls = 0;
    const auto now = Clock::now();
    ++calls;
    if (last == Clock::time_point{} || now - last > std::chrono::seconds(1)) {
        RT_LOGF(RT_TAG_RUNTIME, "depth blur: drawing (%s), %llu step-2 calls so far\n",
                dropped ? "off: dropped" : "on", static_cast<unsigned long long>(calls));
    }
    last = now;
}
}  // namespace

extern "C" void DepthBlurInternalDraw_802c3cb0(CpuContext* ctx) {
    const bool step2 = (ctx->gpr[4] & 0xFFFFu) == 2;
    const bool drop = step2 && (RuntimeGameGraphicsOptions::DisabledPostProcessingPaths() & kDepthBlurOffBit) != 0;
    if (step2) {
        LogUse(drop);
    }
    if (!drop) {
        func_802C3CB0(ctx);
        return;
    }
    AuroraSetDrawDrop(true);
    func_802C3CB0(ctx);
    AuroraSetDrawDrop(false);
}

REGISTER_NATIVE_FUNCTION_AS(0x802C3CB0, DepthBlurInternalDraw_802c3cb0, "DepthBlurInternalDraw_802c3cb0");
