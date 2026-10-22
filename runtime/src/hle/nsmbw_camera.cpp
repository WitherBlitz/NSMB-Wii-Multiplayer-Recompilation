// NSMBW ultrawide: a zone narrower than the widened view sits in the middle of the screen.
//
// The camera's horizontal limits come from dBg_c::getLeftLimit / getRightLimit (the zone's edges,
// corrected for zoom), and dBg_c::calcScroll clamps the camera between left and right - view width.
// Fill widens the view (mVideo::l_rayoutWidthF) past 408 units, so a zone made for 16:9 - a castle
// boss's arena - can be narrower than it; the two bounds then cross and the camera pinned to one
// edge of the zone, leaving the arena off to one side. Worse, bosses place their moves from the
// screen's centre (dGameCom::getDispCenterX): Larry's shell slide (issue #8) turned at walls
// measured from a centre that had moved with the camera, so he slid off his platform.
//
// These overrides compute the limits exactly as the game does (same calls, same single-precision
// operations in the same order; this file builds with the translator's strict float options), and
// only in a widened view, when the zone is narrower than the view, return the zone's centre plus or
// minus half the view instead: the camera then centres the zone, and the screen's centre is where
// it is in 16:9. Network play never widens (display_settings.cpp forces 16:9), so it runs the
// game's numbers bit for bit.
//
// The players' side walls (daPlBase_c::checkSideViewLemit) read the same limits; that check is
// ported here too and keeps the zone's own edges, so players stop where they do in 16:9.
#include "abi_bridge.h"
#include "det_clock.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kZoomTargetMaxCommon = 0x80079060u;  // dBg_c::getZoomTargetMax_Common
constexpr uint32_t kStageViewWidth = 0x8042A0ECu;       // mVideo::l_rayoutWidthF
constexpr uint32_t kOne = 0x8042BD18u;                  // f32 constant the limits divide by the zoom
constexpr uint32_t kHalf = 0x8042BD5Cu;                 // f32 constant the zoom correction halves with
constexpr uint32_t kBgBias = 0x90000u;                  // dBg_c's scroll fields sit past this offset
constexpr uint32_t kZoneLeft = 0x30u, kZoneRight = 0x34u, kZoom = 0x0Cu, kScrollMode = 0xFEu;
constexpr float kGameViewWidth = 408.0f;

float ZoomTargetMaxCommon(uint32_t bg) {
    CpuContext* cpu = &GetPersistentCpuContext();
    cpu->gpr[3] = bg;
    InvokeIndirectCpu(kZoomTargetMaxCommon, cpu);
    return static_cast<float>(cpu->fpr[1].d);
}

// dBg_c::getLeftLimit (0x80078A70) / getRightLimit (0x80078C10), instruction for instruction.
float ZoneLimit(uint32_t bg, bool right) {
    const uint32_t base = bg + kBgBias;
    const uint8_t mode = Memory::Read8(base + kScrollMode);
    float limit = Memory::ReadFloat32(base + (right ? kZoneRight : kZoneLeft));
    if (mode == 1 || mode == 4) {
        return limit;
    }
    float zoom = Memory::ReadFloat32(base + kZoom);
    float zoomMax = ZoomTargetMaxCommon(bg);
    const float one = Memory::ReadFloat32(kOne);
    // fcmpo + bge: the correction applies only when one / zoom is (ordered) below the maximum.
    if (!(one / zoom < zoomMax)) {
        return limit;
    }
    const float width = Memory::ReadFloat32(kStageViewWidth);
    zoom = Memory::ReadFloat32(base + kZoom);
    zoomMax = ZoomTargetMaxCommon(bg);
    const float half = Memory::ReadFloat32(kHalf);
    const float atMax = half * (zoomMax * width);
    const float atZoom = half * ((one / zoom) * width);
    const float correction = atMax - atZoom;
    return right ? limit - correction : limit + correction;
}

// The limit the camera gets: the game's, or in a widened view the zone centred in the view.
// NSMBW_LOG_CAMERA: about once a second, the screen's centre (what bosses measure from) and the
// zone's edges.
void LogCamera(uint32_t bg) {
    static const bool log = std::getenv("NSMBW_LOG_CAMERA") != nullptr;
    static uint32_t calls = 0;
    if (!log || ++calls % 120 != 0) {
        return;
    }
    const uint32_t param = Memory::Read32(0x80429E00u);  // dBgParameter_c::ms_Instance_p
    const float screenLeft = Memory::ReadFloat32(param + 0x38u), screenWidth = Memory::ReadFloat32(param + 0x40u);
    const float left = ZoneLimit(bg, false), right = ZoneLimit(bg, true);
    RT_LOGF(RT_TAG_RUNTIME, "camera: screen %.1f-%.1f (centre %.1f), zone %.1f-%.1f (centre %.1f)\n", screenLeft,
            screenLeft + screenWidth, screenLeft + screenWidth * 0.5f, left, right, (left + right) * 0.5f);
}

float CameraLimit(uint32_t bg, bool right) {
    if (!right) {
        LogCamera(bg);
    }
    const float own = ZoneLimit(bg, right);
    const float viewWidth = Memory::ReadFloat32(kStageViewWidth);
    if (DetClock::Enabled() || !(viewWidth > kGameViewWidth + 0.5f)) {
        return own;
    }
    const float other = ZoneLimit(bg, !right);
    const float left = right ? other : own;
    const float rightEdge = right ? own : other;
    // The camera's width as dBg_c::calcScroll takes it: the view over the current zoom.
    const float width = viewWidth * (Memory::ReadFloat32(kOne) / Memory::ReadFloat32(bg + kBgBias + kZoom));
    if (!(rightEdge - left < width)) {
        return own;
    }
    const float centre = (left + rightEdge) * 0.5f;
    // The log notes each zone the camera centres (once per change of zone edges).
    static float s_loggedLeft = 0.0f, s_loggedRight = 0.0f;
    if (left != s_loggedLeft || rightEdge != s_loggedRight) {
        s_loggedLeft = left;
        s_loggedRight = rightEdge;
        RT_LOGF(RT_TAG_RUNTIME, "camera: zone %.0f-%.0f (%.0f wide) centred in a %.0f-unit view\n", left, rightEdge,
                rightEdge - left, width);
    }
    return right ? centre + width * 0.5f : centre - width * 0.5f;
}

} // namespace

extern "C" float BgGetLeftLimit_HLE_80078a70(uint32_t bg)
{
    return CameraLimit(bg, false);
}
PPC_NATIVE_OVERRIDE(80078A70, BgGetLeftLimit_HLE_80078a70, float, (uint32_t bg), (bg));

extern "C" float BgGetRightLimit_HLE_80078c10(uint32_t bg)
{
    return CameraLimit(bg, true);
}
PPC_NATIVE_OVERRIDE(80078C10, BgGetRightLimit_HLE_80078c10, float, (uint32_t bg), (bg));

// daPlBase_c::checkSideViewLemit (0x80055810), instruction for instruction, with the zone's own
// limits rather than the centred camera's.
namespace {
constexpr uint32_t kDemoMng = 0x80429C94u;          // daPyDemoMng_c::mspInstance
constexpr uint32_t kLoopType = 0x8042A208u;         // dScStage_c::m_loopType
constexpr uint32_t kBg = 0x80429DD0u;               // dBg_c::m_bg_p
constexpr uint32_t kIsDemoType = 0x8004DB40u;       // daPlBase_c::isDemoType(DemoType_e)
constexpr uint32_t kIsStatus = 0x80056CF0u;         // daPlBase_c::isStatus(int)
constexpr uint32_t kSideLimitMultL = 0x80055A00u;   // daPlBase_c::calcSideLimitMultL(float)
constexpr uint32_t kSideLimitMultR = 0x80055A70u;   // daPlBase_c::calcSideLimitMultR(float)
constexpr uint32_t kDispSideLemit = 0x80055AE0u;    // daPlBase_c::checkDispSideLemit()

uint32_t CallR3(uint32_t target, uint32_t r3, uint32_t r4) {
    CpuContext* cpu = &GetPersistentCpuContext();
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
    InvokeIndirectCpu(target, cpu);
    return cpu->gpr[3];
}

void CallF1(uint32_t target, uint32_t r3, float f1) {
    CpuContext* cpu = &GetPersistentCpuContext();
    cpu->gpr[3] = r3;
    cpu->fpr[1].d = static_cast<double>(f1);
    InvokeIndirectCpu(target, cpu);
}
} // namespace

extern "C" void PlCheckSideViewLemit_HLE_80055810(uint32_t player)
{
    const uint32_t demoMng = Memory::Read32(kDemoMng);
    if (Memory::Read32(demoMng + 0x94u) == 0 && CallR3(kIsDemoType, player, 4) != 0) {
        return;
    }
    for (const uint32_t status : {0x53u, 0x6Fu, 0x8Du}) {
        if (CallR3(kIsStatus, player, status) != 0) {
            return;
        }
    }
    if (Memory::Read32(kLoopType) == 0) {
        // fcmpo + cror: each side applies when the player is at or past it (never for a NaN).
        const float leftLimit = ZoneLimit(Memory::Read32(kBg), false);
        const float left = Memory::ReadFloat32(player + 0x3ECu) + leftLimit;
        const float x = Memory::ReadFloat32(player + 0xACu);
        if (x < left || x == left) {
            CallF1(kSideLimitMultL, player, left);
        }
        const float rightLimit = ZoneLimit(Memory::Read32(kBg), true);
        const float right = rightLimit - Memory::ReadFloat32(player + 0x3ECu);
        const float x2 = Memory::ReadFloat32(player + 0xACu);
        if (x2 > right || x2 == right) {
            CallF1(kSideLimitMultR, player, right);
        }
    }
    CallR3(kDispSideLemit, player, 0);
}
PPC_NATIVE_OVERRIDE_VOID(80055810, PlCheckSideViewLemit_HLE_80055810, (uint32_t player), (player));
