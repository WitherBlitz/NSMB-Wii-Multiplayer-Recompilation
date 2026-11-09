#include "det_clock.h"
#include "display_settings.h"

#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"
#include "settings_overlay.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace DisplaySettings {
namespace {

// NSMBW (SMNE01 rev 1) EGG::Screen statics. sTVModeInfo holds one record per TV mode,
// {u16 width, u16 height, f32 scaleX = fbWidth / width, f32 scaleY = efbHeight / height}, written
// by EGG::Screen::Initialize from the game's own sizes; record 1 is 16:9 (832x456 canvas units).
// EGG::Screen::SetTVMode copies the current record into the root screen's canvas and flags it for
// rebuild, and NSMBW registers no TV-mode change callback, so doing those writes from here is what
// SetTVMode would do. 2D layouts (m2d), the HOME menu and depth of field read the record directly.
constexpr uint32_t kTVModeInfo = 0x803504E0u;  // EGG::Screen::sTVModeInfo
constexpr uint32_t kTVModeRecordSize = 0x0Cu;
constexpr uint32_t kTVModeWide = 1u;
constexpr uint32_t kTVMode = 0x8042AF08u;      // EGG::Screen::sTVMode
constexpr uint32_t kRootScreen = 0x8042AF0Cu;  // EGG::Screen::spRoot
constexpr uint32_t kFbWidth = 0x8042AF38u;     // EGG::StateGX::s_widthFb (u16, upper half of the word)
constexpr uint32_t kCanvasWidth = 0x08u;       // EGG::Screen: canvas width (f32)
constexpr uint32_t kScreenFlags = 0x34u;       // EGG::Screen: flags (u16), bit 0 = rebuild
// NSMBW's own copy of the 16:9 size {f32 width, f32 height}, taken from the record by d_screen.cpp's
// static initializer before EGG::Screen::Initialize runs. dScreen::GetScreenSize (the game's camera
// and screen-space maths) reads it, so it has to widen with the record.
constexpr uint32_t kGameScreenSize169 = 0x8042A058u;  // dScreen::l_ScreenSize16x9
// 608/832 (f32) in .sdata2, read by d2d::Multi_c::__ct (0x80006D48) and m2d::Simple_c::__ct
// (0x8016395C): the x scale of 16:9 layouts in the 4:3 (608-unit) 2D projection.
constexpr uint32_t kLayoutSqueezeMulti = 0x8042B084u;
constexpr uint32_t kLayoutSqueezeSimple = 0x8042DC48u;
constexpr uint32_t kLayoutBaseWidth = 608u;  // sTVModeInfo[0] (4:3) width: the 2D projection's
// The levels' view, in world units: mVideo::create (0x800E8AB0) copies 408 x 224 from .sdata2 into
// mVideo::l_rayout*, and dBg_c (camera scroll and limits, DispScaleCalc -> dBgParameter_c's visible
// area), dBgGm_c and dActor_c::screenCullCheck read the floats. Widening the width widens the level
// camera itself, so tiles, backgrounds and actors are drawn, spawned and culled across the whole
// screen instead of a 16:9 view being stretched over it.
constexpr uint32_t kStageViewWidthConst = 0x8042CD78u;  // f32 408 (.sdata2, read by mVideo::create)
constexpr uint32_t kStageViewWidthInt = 0x8042A0E4u;    // mVideo::l_rayoutWidth (s32)
constexpr uint32_t kStageViewWidth = 0x8042A0ECu;       // mVideo::l_rayoutWidthF
constexpr uint32_t kStageViewHeight = 0x8042A0F0u;      // mVideo::l_rayoutHeightF
constexpr uint32_t kStageViewAspect = 0x8042A0F4u;      // mVideo::l_rayoutAspect
constexpr float kStageViewBaseWidth = 408.0f;
// bgTex_c::drawBuffer (0x80082740) draws 28 / zoom + 2 columns of tiles (.sdata2 f32, read by it
// alone), a fixed 480 units that fall short of a wider view: grow it in step.
constexpr uint32_t kTileColumns = 0x8042BEC4u;
constexpr float kTileColumnsBase = 28.0f;

constexpr float kLinesPerScale = 480.0f;
constexpr float kRatio16x9 = 16.0f / 9.0f;
constexpr float kRatio16x10 = 16.0f / 10.0f;

std::mutex g_mutex;
Settings g_current;
Settings g_pending;
bool g_hasPending = false;
bool g_bootWidescreen = true;
bool g_rendererDirty = true;
uint32_t g_surfaceWidth = 0;
uint32_t g_surfaceHeight = 0;
uint16_t g_gameWideWidth = 0;  // the game's own 16:9 canvas width, captured before any widening

// The game booted in 4:3: neither widescreen mode exists until the next start.
int32_t EffectiveAspect(int32_t aspect) {
    return g_bootWidescreen ? aspect : kAspect4x3;
}

bool SurfaceWiderThan169() {
    return g_surfaceWidth != 0 && g_surfaceHeight != 0 &&
           static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight) > kRatio16x9;
}

// Render scale 0 ("match screen resolution"): exactly as many lines as the picture is tall on the
// surface, so it renders 1:1 (a 2560x1080 surface in Fill renders 2560x1080, with the game's view
// widened to match), not the window size with the EFB's workspace rows squeezed in.
float RenderLines(float renderScale, int32_t aspect) {
    if (renderScale > 0.0f || g_surfaceWidth == 0 || g_surfaceHeight == 0) {
        return LinesForScale(renderScale);
    }
    const float w = static_cast<float>(g_surfaceWidth), h = static_cast<float>(g_surfaceHeight);
    const bool fillsSurface = aspect == kAspectFill && SurfaceWiderThan169();
    const float pictureAspect = aspect == kAspect4x3 ? 4.0f / 3.0f : aspect == kAspect16x10 ? kRatio16x10 : kRatio16x9;
    return fillsSurface ? h : std::min(h, w / pictureAspect);
}

void ApplyRenderer(const Settings& settings) {
    const int32_t aspect = EffectiveAspect(settings.aspect);
    if (aspect == kAspect4x3) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
    } else if (aspect == kAspect16x10) {
        // A 16:10 picture, with bars only on a screen that isn't: the game draws a narrower view into it.
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(16, 10);
    } else if (aspect == kAspect16x9 || !SurfaceWiderThan169()) {
        // 16:9, or Fill on a surface no wider than 16:9 (the picture is never squashed).
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_16_9);
    } else {
        // The framebuffer takes the surface's aspect; the game draws the wider view into it.
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    }
    VISetFrameBufferLines(RenderLines(settings.renderScale, aspect));
    RT_LOGF(RT_TAG_VI, "display: aspect %d%s, render %.0f lines (%.2gx), surface %ux%u\n", aspect,
            aspect != settings.aspect ? " (game booted in 4:3)" : "", RenderLines(settings.renderScale, aspect),
            settings.renderScale, g_surfaceWidth, g_surfaceHeight);
}

// Fill: widen the game's 16:9 canvas to the surface (Hor+), keeping the game's own pixel aspect
// (its 832 units stand for 16:9). Otherwise keep, or put back, the game's own width.
std::atomic<float> g_wideAspect{0.0f};  // what aurora widens 16:9 cameras to, or 0

void ApplyGameCanvas(int32_t aspect) {
    // 3D: cameras set up for 16:9 are widened to the screen (aurora's projection), the same view
    // height with more to the sides, instead of being stretched across it.
    const bool widened = aspect == kAspectFill && SurfaceWiderThan169();
    g_wideAspect.store(aspect == kAspect16x10 ? kRatio16x10
                       : widened              ? static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight)
                                              : 0.0f,
                       std::memory_order_relaxed);
    AuroraSetWidePerspectiveAspect(g_wideAspect.load(std::memory_order_relaxed));
    uint32_t root = 0;
    if (!Memory::TryRead32(kRootScreen, root) || root == 0) {
        return;  // EGG::Screen::Initialize has not run yet
    }
    const uint32_t record = kTVModeInfo + kTVModeWide * kTVModeRecordSize;
    uint32_t size = 0;
    if (!Memory::TryRead32(record, size)) {
        return;
    }
    const auto width = static_cast<uint16_t>(size >> 16);
    if (g_gameWideWidth == 0) {
        g_gameWideWidth = width;
    }
    uint16_t target = g_gameWideWidth;
    if (aspect == kAspectFill && SurfaceWiderThan169()) {
        const float surfaceAspect = static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight);
        target = static_cast<uint16_t>(std::lround(g_gameWideWidth * surfaceAspect / kRatio16x9));
    } else if (aspect == kAspect16x10) {
        target = static_cast<uint16_t>(std::lround(g_gameWideWidth * kRatio16x10 / kRatio16x9));
    }
    if (target == width) {
        return;
    }
    uint32_t fbWord = 0;
    Memory::TryRead32(kFbWidth, fbWord);
    const auto fbWidth = static_cast<float>(fbWord >> 16);
    Memory::Write16(record, target);
    if (fbWidth > 0.0f) {
        Memory::WriteFloat32(record + 4u, fbWidth / static_cast<float>(target));
    }
    Memory::WriteFloat32(kGameScreenSize169, static_cast<float>(target));
    // The 2D layer: d2d::Multi_c and m2d::Simple_c draw their 832-unit 16:9 layouts in a 608-unit
    // projection and squeeze x by 608/832 (read from these two constants when a layout is built), so
    // on a wider canvas they stretched. 608/width keeps their pixels square on any width.
    for (const uint32_t squeeze : {kLayoutSqueezeMulti, kLayoutSqueezeSimple}) {
        Memory::WriteFloat32(squeeze, static_cast<float>(kLayoutBaseWidth) / static_cast<float>(target));
    }
    // The level camera: as wide as the canvas, in the same proportion as the game's own 408 to 832.
    const float stageWidth = kStageViewBaseWidth * static_cast<float>(target) / static_cast<float>(g_gameWideWidth);
    Memory::WriteFloat32(kStageViewWidthConst, stageWidth);
    Memory::WriteFloat32(kTileColumns, std::ceil(kTileColumnsBase * stageWidth / kStageViewBaseWidth));
    const float stageHeight = Memory::ReadFloat32(kStageViewHeight);
    if (stageHeight > 0.0f) {  // else mVideo::create has yet to run, and takes the constant
        Memory::WriteFloat32(kStageViewWidth, stageWidth);
        Memory::Write32(kStageViewWidthInt, static_cast<uint32_t>(std::lround(stageWidth)));
        Memory::WriteFloat32(kStageViewAspect, stageWidth / stageHeight);
    }
    uint32_t mode = 0;
    Memory::TryRead32(kTVMode, mode);
    if (mode == kTVModeWide) {
        uint32_t flagsWord = 0;
        Memory::TryRead32(root + kScreenFlags, flagsWord);
        Memory::WriteFloat32(root + kCanvasWidth, static_cast<float>(target));
        Memory::Write16(root + kScreenFlags, static_cast<uint16_t>((flagsWord >> 16) | 1u));
    }
    RT_LOGF(RT_TAG_VI, "display: 16:9 canvas %u -> %u units wide, fb %.0f px (surface %ux%u)\n", width,
            target, fbWidth, g_surfaceWidth, g_surfaceHeight);
}

} // namespace

float LinesForScale(float renderScale) {
    return renderScale > 0.0f ? kLinesPerScale * renderScale : 0.0f;
}

// Deterministic mode (network play) keeps every device on the same 16:9 canvas: how wide the game
// draws decides which enemies it wakes, so Fill's screen-dependent width would desync the session.
Settings Lockstep(Settings settings) {
    if (DetClock::Enabled()) {
        settings.aspect = kAspect16x9;
    }
    return settings;
}

void Initialize(const Settings& requested) {
    const Settings settings = Lockstep(requested);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_current = settings;
    g_bootWidescreen = settings.aspect != kAspect4x3;
    g_rendererDirty = true;
}

void Request(const Settings& settings) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = Lockstep(settings);
    g_hasPending = true;
}

// NSMBW_DUMP_SCREENS=<frame>: at that frame, log every EGG::Screen in guest memory (found by the
// root screen's vtable) with its canvas size (finding the 2D layer's own screen).
void DumpScreens() {
    static const long frameToDump = [] {
        const char* value = std::getenv("NSMBW_DUMP_SCREENS");
        return value != nullptr ? std::atol(value) : 0L;
    }();
    static long frame = 0;
    if (frameToDump <= 0 || ++frame != frameToDump) {
        return;
    }
    uint32_t root = 0, vtable = 0;
    if (!Memory::TryRead32(kRootScreen, root) || root == 0) {
        return;
    }
    std::string words;
    for (uint32_t i = 0; i < 0x60; i += 4) {
        uint32_t word = 0;
        Memory::TryRead32(root + i, word);
        char text[16];
        std::snprintf(text, sizeof(text), "%08X ", word);
        words += text;
    }
    RT_LOGF(RT_TAG_VI, "screens: root %08X: %s\n", root, words.c_str());
    for (uint32_t a : {0x8042AF18u, 0x8042AF1Cu, 0x8042AF20u, 0x8042AF24u, 0x803504E0u, 0x803504E4u, 0x803504E8u,
                       0x803504ECu, 0x803504F0u, 0x803504F4u}) {
        uint32_t word = 0;
        Memory::TryRead32(a, word);
        float f;
        std::memcpy(&f, &word, 4);
        RT_LOGF(RT_TAG_VI, "screens: [%08X] = %08X (%g)\n", a, word, f);
    }
    // The vtable: the first word that points into the game's data sections.
    uint32_t vtableOffset = 0;
    for (uint32_t i = 0; i < 0x60 && vtable == 0; i += 4) {
        uint32_t word = 0;
        if (Memory::TryRead32(root + i, word) && word >= 0x80300000u && word < 0x80440000u) {
            vtable = word;
            vtableOffset = i;
        }
    }
    RT_LOGF(RT_TAG_VI, "screens: vtable %08X at +%X\n", vtable, vtableOffset);
    if (vtable == 0) {
        return;
    }
    for (const auto [begin, end] : {std::pair{0x80000000u, 0x81800000u}, std::pair{0x90000000u, 0x94000000u}}) {
        for (uint32_t a = begin; a < end; a += 4) {
            uint32_t word = 0;
            if (!Memory::TryRead32(a, word) || word != vtable || a < vtableOffset) {
                continue;
            }
            const uint32_t object = a - vtableOffset;
            uint32_t w = 0, h = 0, flags = 0;
            Memory::TryRead32(object + kCanvasWidth, w);
            Memory::TryRead32(object + kCanvasWidth + 4, h);
            Memory::TryRead32(object + kScreenFlags, flags);
            float fw, fh;
            std::memcpy(&fw, &w, 4);
            std::memcpy(&fh, &h, 4);
            RT_LOGF(RT_TAG_VI, "screens: object %08X canvas %.1f x %.1f flags %04X\n", object, fw, fh, flags >> 16);
        }
    }
}

void OnFrameBoundary(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    DumpScreens();
    Settings settings;
    bool renderer = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hasPending) {
            g_current = g_pending;
            g_hasPending = false;
            g_rendererDirty = true;
        }
        if (surfaceWidth != 0 && surfaceHeight != 0 &&
            (surfaceWidth != g_surfaceWidth || surfaceHeight != g_surfaceHeight)) {
            const bool wasWide = SurfaceWiderThan169();
            g_surfaceWidth = surfaceWidth;
            g_surfaceHeight = surfaceHeight;
            // Fill switches between stretching and the 16:9 frame as the surface crosses 16:9, and
            // a "match screen" render follows every size.
            if (SurfaceWiderThan169() != wasWide || g_current.renderScale <= 0.0f) {
                g_rendererDirty = true;
            }
        }
        settings = g_current;
        renderer = g_rendererDirty;
        g_rendererDirty = false;
    }
    if (renderer) {
        ApplyRenderer(settings);
    }
    ApplyGameCanvas(EffectiveAspect(settings.aspect));
}

Settings Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_current;
}

Settings Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hasPending ? g_pending : g_current;
}

bool GameWidescreen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bootWidescreen;
}

bool SurfaceWide() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return SurfaceWiderThan169();
}

} // namespace DisplaySettings

// 0x800B32E0 CrapUpPositions(Point3d* out, const Point3d* in): a layout position (608 units across
// 4:3, centred) to the 2D scenes' 3D space, the W x H top-left orthographic camera the Items screen,
// player select and the like draw their models with (W = mVideo's 640 x 4/3 in widescreen). Aurora
// widens that camera to the surface when Fill is wider than 16:9, which keeps the models' shape but
// pulls them toward the middle; their layout cards don't move, so the models are spread out again by
// the same factor here. Otherwise the game's own arithmetic, step for step.
extern "C" void CrapUpPositions_800b32e0(uint32_t out, uint32_t in) {
    constexpr uint32_t kVideo = 0x8042A480u;         // mVideo::m_video
    constexpr uint32_t kAspectRatio = 0x80428750u;   // mVideo::l_AspectRatio (4:3 to 16:9)
    constexpr uint32_t kHalf = 0x8042C578u, kLayoutWidth = 0x8042C588u;  // 0.5, 608
    const uint32_t video = Memory::Read32(Memory::Read32(kVideo));
    float width = static_cast<float>(Memory::Read16(video + 4u));
    const float height = static_cast<float>(Memory::Read16(video + 6u));
    if (DisplaySettings::GameWidescreen()) width *= Memory::ReadFloat32(kAspectRatio);
    const float half = Memory::ReadFloat32(kHalf);
    const float scale = width / Memory::ReadFloat32(kLayoutWidth);
    const float x = Memory::ReadFloat32(in), y = Memory::ReadFloat32(in + 4u), z = Memory::ReadFloat32(in + 8u);
    const float centreX = half * width, centreY = half * height;
    float offset = -((centreX - (centreX + x)) * scale);
    const float wide = DisplaySettings::g_wideAspect.load(std::memory_order_relaxed);
    if (wide > 0.0f && height > 0.0f) {
        const float cameraAspect = width / height;  // aurora widens cameras from 1.68 to 1.88
        if (cameraAspect > 1.68f && cameraAspect < 1.88f) offset *= wide / cameraAspect;
    }
    Memory::WriteFloat32(out + 8u, z);
    Memory::WriteFloat32(out + 4u, centreY + y);
    Memory::WriteFloat32(out, centreX + offset);
}
PPC_NATIVE_OVERRIDE_VOID(800B32E0, CrapUpPositions_800b32e0, (uint32_t out, uint32_t in), (out, in));

// 0x800E4E40 dSys_c::execute: the game's frame, ported call for call from the translated function,
// so Battery Saver can leave the drawing out of every other frame. The game keeps its 60 steps a
// second: everything from m2d::reset on (and m3d::calcMaterial, dPad::beginPad_BR, m3d::reset before
// it) runs every frame, and only the draw lists (painter, the fader's draw) are left out. What that
// leaves out only draws: painter is the m2d list (d2d::draw), the 2D and 3D the frame's fManager_c draw
// pass entered, and it is reset after endRender either way; particles (dLastActor_c::execute), layout
// animations (m2d::Simple_c::calc), the depth blur's shimmer (dCaptureMng_c::execute) and the faders
// and wipes (dFader_c::calc; their draw only enters a layout) all advance in the logic half.
// beginRender and endRender still run: EGG::AsyncDisplay's next beginFrame copies and swaps the XFB
// as after any frame (left out, it turned the VI black on the XFB it never filled). That copy would show
// an empty picture, so hle/gx/gx_copy.cpp drops it (TakeUndrawnFrame) and the VI keeps the last one.
namespace {

// The game's globals, below r13 (SMNE01 rev 1).
constexpr uint32_t kSdaGraph = 22312;       // dGraph_c::ms_Instance
constexpr uint32_t kSdaGameKey = 22320;     // dGameKey_c::m_instance
constexpr uint32_t kSdaDvdErr = 21480;      // dDvdErr_c::m_pInstance
constexpr uint32_t kSdaHbm = 21452;         // dHbm::Manage_c's instance
constexpr uint32_t kSdaReset = 21460;       // dReset::Manage_c's instance
constexpr uint32_t kSdaDylinkPhase = 21984; // dSys_c's myDylinkInitPhase (sPhase_c)
constexpr uint32_t kSdaExeFrame = 21268;    // cCounter_c::m_exeFrame
constexpr uint32_t kSdaGameFrame = 21272;   // cCounter_c::m_gameFrame
constexpr uint32_t kDvdErrOccurred = 0x04u; // dDvdErr_c: isErrorOccured's byte
// dHbm::Manage_c's state: 0 before Load, 1 the game running, 3 opening (this frame's Calculate runs
// MenuInit, which captures this frame's picture as the menu's background), 2 open.
constexpr uint32_t kHbmState = 0x1D0u;
constexpr uint32_t kHbmOpen = 2u;
constexpr uint32_t kResetModeProc = 0x08u;  // dReset::Manage_c::mModeProc, 0 = NORMAL
constexpr uint32_t kPhaseDone = 2u;         // sPhase_c::DONE

// A bl from dSys_c::execute: the callee returns to `returnAddress` (its saved LR).
template <uint32_t Target>
void Call(CpuContext* ctx, uint32_t returnAddress) {
    ctx->lr = returnAddress;
    InvokeDirectCpu<Target>(ctx);
}

// Battery Saver: whether this frame goes undrawn. Every other frame while the game simply runs: never
// with the HOME Menu opening or open, a disc error showing or a reset under way, nor in LAN play.
bool UndrawnFrame(uint32_t r13, bool dvdErr) {
    static bool s_drewLast = true;
    const bool undrawn = s_drewLast && settings_overlay::BatterySaverActive() && !dvdErr &&
                         Memory::Read32(Memory::Read32(r13 - kSdaHbm) + kHbmState) < kHbmOpen &&
                         Memory::Read32(Memory::Read32(r13 - kSdaReset) + kResetModeProc) == 0;
    s_drewLast = !undrawn;
    return undrawn;
}

void SysExecute_800e4e40(CpuContext* ctx) {
    const uint32_t r13 = ctx->gpr[13];
    const auto instance = [r13](uint32_t sda) { return Memory::Read32(r13 - sda); };
    const auto counterAdd = [r13](uint32_t sda) { Memory::Write32(r13 - sda, Memory::Read32(r13 - sda) + 1); };
    // Its 16-byte frame, the caller's LR where the game saves it.
    const uint32_t sp = ctx->gpr[1];
    const uint32_t lr = ctx->lr;
    Memory::Write32(sp - 16, sp);
    Memory::Write32(sp + 4, lr);
    ctx->gpr[1] = sp - 16;

    const bool dvdErr = Memory::Read8(instance(kSdaDvdErr) + kDvdErrOccurred) != 0;
    bool inHbm = Memory::Read32(instance(kSdaHbm) + kHbmState) == kHbmOpen;
    Call<0x800E46E0u>(ctx, 0x800E4E78u);  // dSys_c::beginFrame
    Call<0x80164D50u>(ctx, 0x800E4E7Cu);  // m3d::calcMaterial
    Call<0x800D0610u>(ctx, 0x800E4E80u);  // dPad::beginPad_BR
    Call<0x80164EC0u>(ctx, 0x800E4E84u);  // m3d::reset
    const bool undrawn = UndrawnFrame(r13, dvdErr);
    Call<0x800E4660u>(ctx, 0x800E4E88u);  // dSys_c::beginRender
    if (!inHbm && !dvdErr && !undrawn) {
        ctx->gpr[3] = instance(kSdaGraph);
        Call<0x800B64E0u>(ctx, 0x800E4EA0u);  // dGraph_c::painter
        Call<0x800B0B20u>(ctx, 0x800E4EA4u);  // dFader_c::draw
    }
    ctx->gpr[3] = instance(kSdaHbm);
    Call<0x80109730u>(ctx, 0x800E4EACu);  // dHbm::Manage_c::DrawMenu
    ctx->gpr[3] = instance(kSdaReset);
    Call<0x80108130u>(ctx, 0x800E4EB4u);  // dReset::Manage_c::Draw
    ctx->gpr[3] = instance(kSdaDvdErr);
    Call<0x80107460u>(ctx, 0x800E4EBCu);  // dDvdErr_c::draw
    ctx->gpr[3] = instance(kSdaHbm);
    Call<0x801097F0u>(ctx, 0x800E4EC4u);  // dHbm::Manage_c::DrawIcon
    Call<0x800E4680u>(ctx, 0x800E4EC8u);  // dSys_c::endRender
    if (undrawn) {
        settings_overlay::MarkUndrawnFrame();
    }
    if (!inHbm) {
        Call<0x80163170u>(ctx, 0x800E4ED4u);  // m2d::reset
        Call<0x80164E70u>(ctx, 0x800E4ED8u);  // m3d::clear
    }
    ctx->gpr[3] = instance(kSdaDvdErr);
    Call<0x801076B0u>(ctx, 0x800E4EE0u);  // dDvdErr_c::execute
    const bool dvdErrNew = Memory::Read8(instance(kSdaDvdErr) + kDvdErrOccurred) != 0;
    if (dvdErrNew && !dvdErr) {
        ctx->gpr[3] = 1;
        Call<0x801BE2F0u>(ctx, 0x800E4F04u);  // VIEnableDimming(TRUE)
    }
    ctx->gpr[3] = instance(kSdaHbm);
    Call<0x80109660u>(ctx, 0x800E4F0Cu);  // dHbm::Manage_c::Calculate
    ctx->gpr[3] = instance(kSdaReset);
    Call<0x801080F0u>(ctx, 0x800E4F14u);  // dReset::Manage_c::Calculate
    inHbm = Memory::Read32(instance(kSdaHbm) + kHbmState) == kHbmOpen;
    Call<0x800DC480u>(ctx, 0x800E4F2Cu);  // dRemoconMng_c::execute
    Call<0x80102140u>(ctx, 0x800E4F30u);  // dScStage_c::play
    ctx->gpr[3] = instance(kSdaGameKey);
    Call<0x800B5AB0u>(ctx, 0x800E4F38u);  // dGameKey_c::read
    Call<0x8006A130u>(ctx, 0x800E4F3Cu);  // dAudio::updateBgmInfo
    if (!inHbm && !dvdErrNew) {
        ctx->gpr[3] = r13 - kSdaDylinkPhase;
        ctx->gpr[4] = 0;
        Call<0x8015F620u>(ctx, 0x800E4F58u);  // sPhase_c::callMethod(nullptr)
        if (ctx->gpr[3] == kPhaseDone) {
            Call<0x800E1DE0u>(ctx, 0x800E4F64u);  // dScene_c::createNextScene
            Call<0x80162E10u>(ctx, 0x800E4F68u);  // fManager_c::mainLoop
            counterAdd(kSdaExeFrame);
        }
        Call<0x800B0B00u>(ctx, 0x800E4F78u);  // dFader_c::calc
    }
    Call<0x800E4820u>(ctx, 0x800E4F7Cu);  // dSys_c::calcAudio
    Call<0x800D08A0u>(ctx, 0x800E4F80u);  // dPad::endPad_BR
    Call<0x800E4740u>(ctx, 0x800E4F84u);  // dSys_c::endFrame
    counterAdd(kSdaGameFrame);

    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

} // namespace

PPC_NATIVE_OVERRIDE_VOID(800E4E40, SysExecute_800e4e40, (CpuContext* ctx), (ctx));
