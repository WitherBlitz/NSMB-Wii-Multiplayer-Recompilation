// The pause window's Settings button, as the game's own third button (hle/storage/layout_patch.h serves
// the layout with it). The window's code knows two buttons, Continue (0) and Exit (1); these wrappers
// teach it a third, Settings (2), which sits between them on screen:
//   - LytBase_c::GroupRegister (0x800C90F0): the window's 11 animation groups get the button's four
//     (on, idle, off, hit: animations 11-14), registered with the rest;
//   - Pausewindow_c::SelectCursorSetup (0x8015AC30): the game's corner cursor goes round it;
//   - the open, button-change and hit states (0x8015AD80, 0x8015AE70, 0x8015B030): its animations;
//   - PauseManager_c::SelectSoundSet (0x800D0C00, the cursor's move): Continue, Settings, Exit in screen
//     order (Exit only where the game allows it); the window then plays the game's cursor sound;
//   - PauseManager_c::KeyChack (0x800D0CB0): 2 or A on Settings plays the game's decide sound and the
//     button's hit animation, the window closes as it does for its other buttons, and the settings
//     window opens (netplay_ui.cpp) while the game stays paused. When it closes, the pause window comes
//     back through its own opening, with Settings selected.
// The world map's + menu (dCourseSelectMenu_c, in the disc build's prelinked d_basesNP) gets the same:
// Star Coins, Add/Drop Players, Quick Save (0-2), Settings (4), Title Screen (3) on screen; its create,
// cursor setup, open and button-change states and its input (the PauseDisp state's execute) are wrapped.
// There 2 or A plays the decide sound and the hit animation and the settings window opens over the menu,
// which stays up underneath.
// Only while the layout has the button: otherwise every wrapper is the game's function as it was. Never
// in deterministic (LAN) play, where the layout isn't patched.
#include "hle/nsmbw_menu_settings.h"

#include "abi_bridge.h"
#include "hle/storage/layout_patch.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

extern "C" void func_800C90F0(CpuContext* ctx);  // LytBase_c::GroupRegister
extern "C" void func_8015AC30(CpuContext* ctx);  // Pausewindow_c::SelectCursorSetup
extern "C" void func_8015AD80(CpuContext* ctx);  // Pausewindow_c::initializeState_OpenAnimeEndWait
extern "C" void func_8015AE70(CpuContext* ctx);  // Pausewindow_c::initializeState_ButtonChangeAnimeEndWait
extern "C" void func_8015B030(CpuContext* ctx);  // Pausewindow_c::initializeState_HitAnimeEndWait
extern "C" void func_800D0C00(CpuContext* ctx);  // PauseManager_c::SelectSoundSet (the cursor's move)
extern "C" void func_800D0CB0(CpuContext* ctx);  // PauseManager_c::KeyChack
extern "C" void func_8077AB70(CpuContext* ctx);  // dCourseSelectMenu_c::create
extern "C" void func_8077AD70(CpuContext* ctx);  // dCourseSelectMenu_c: its cursor's setup
extern "C" void func_8077ADC0(CpuContext* ctx);  // ... initializeState_OpenAnimeEndWait
extern "C" void func_8077AF10(CpuContext* ctx);  // ... initializeState_ButtonChangeAnimeEndWait
extern "C" void func_8077B010(CpuContext* ctx);  // ... executeState_PauseDisp (its input)

namespace {

constexpr uint32_t kPausewindowInstance = 0x8042A33Cu;  // Pausewindow_c::m_instance
constexpr uint32_t kGameKeyInstance = 0x80429F50u;      // dGameKey_c::m_instance
constexpr uint32_t kGameStop = 0x80429F48u;             // dGameCom::l_gameStop
constexpr uint32_t kSndAudioMgr = 0x8042A488u;          // SndAudioMgr::sInstance
constexpr uint32_t kStartGameInfo = 0x80315B90u;        // dInfo_c::m_startGameInfo, +0xD the level
constexpr uint32_t kStartSystemSe = 0x80195380u;
constexpr uint32_t kIsNowCourseClear = 0x800B4DE0u;
constexpr uint32_t kSelectCursorSetup = 0x800B44D0u;  // dGameCom::SelectCursorSetup(pane, int, bool)
constexpr uint32_t kAnimeStartSetup = 0x800C92F0u;
constexpr uint32_t kReverseAnimeStartSetup = 0x800C93D0u;
constexpr uint32_t kAllAnimeEndSetup = 0x800C9500u;
constexpr uint32_t kAnimeEndSetup = 0x800C9490u;

// Pausewindow_c
constexpr uint32_t kLayout = 0x70, kNextButton = 0x284, kActiveButton = 0x288, kIsActive = 0x291,
                   kButtonHit = 0x293, kIsAnimating = 0x294;
// PauseManager_c: +8 the cursor, +0xC the player who paused.
constexpr uint32_t kCursor = 0x8, kPlayer = 0xC;

constexpr int kSettings = 2;
// The window's animations: on, off and hit for Continue (0), Exit (1) and Settings (2).
constexpr int kOn[3] = {3, 4, 11}, kOff[3] = {7, 8, 13}, kHit[3] = {9, 10, 14};
constexpr uint32_t kWindowGroups = 11;
// The group's name as the game's constant data has it (layout_patch.cpp): m2d keeps the pointer.
constexpr const char* kSettingsGroup = "B00_yesButton";

// Where main.dol's constant data holds `name` (0 if nowhere).
uint32_t FindString(const char* name) {
    const size_t length = std::char_traits<char>::length(name);
    for (uint32_t at = 0x802E0000u; at < 0x80340000u; ++at) {
        uint32_t i = 0;
        while (i <= length && Memory::Read8(at + i) == (i < length ? static_cast<uint8_t>(name[i]) : 0)) ++i;
        if (i > length && Memory::Read8(at - 1) == 0) {
            return at;
        }
    }
    return 0;
}

uint32_t GroupName() {
    static const uint32_t found = FindString(kSettingsGroup);
    return found;
}

// dCourseSelectMenu_c: +0x268 the button the cursor goes to, +0x26C the one it is on, +0x272 a button
// was picked, +0x273 input off; its animations: on 2-5, hit 10-13, off 14-17; Settings' 19-22.
constexpr uint32_t kMapNext = 0x268, kMapActive = 0x26C, kMapPicked = 0x272, kMapLocked = 0x273;
constexpr uint32_t kMapStates = 0x208;                   // its state manager
constexpr uint32_t kMapStateButtonChange = 0x80991670u;  // StateID_ButtonChangeAnimeEndWait
constexpr uint32_t kMapPlayer = 0x8042A464u;             // the player the menu reads
constexpr uint32_t kMapGroups = 19;
constexpr uint32_t kMapSettings = 4;
constexpr uint32_t kMapSettingsHit = 21;
constexpr const char* kMapSettingsGroup = "B01_noButton";  // layout_patch.cpp
uint32_t g_mapMenu = 0;
std::atomic<bool> g_mapOpenRequested{false};

uint32_t MapOn(uint32_t button) { return button < 4 ? 2 + button : 19; }
uint32_t MapOff(uint32_t button) { return button < 4 ? 14 + button : 22; }

std::atomic<bool> g_openRequested{false};  // the button was pressed: the settings window opens
std::atomic<bool> g_reopen{false};         // the settings window closed: the pause window opens again
// The world map's menu, as its pause-window counterpart: Settings' hit animation, then the menu's own
// out animation (group 18) so it's gone behind the settings window, which opens; the menu takes no
// input meanwhile; when the window has closed, the menu opens again (its own open) with Settings picked.
constexpr uint32_t kMapOut = 18;
constexpr uint32_t kMapIn = 0;
constexpr int kMapHitFrames = 10;          // the hit animation's flash before the menu goes
std::atomic<bool> g_mapReopen{false};      // the settings window closed: the map menu opens again
int g_mapSeq = 0;                          // 0 the menu's own, 1 Settings hit, 2 the settings window up
int g_mapSeqFrames = 0;
bool g_reopening = false;                  // ... and is opening now, Settings selected
uint32_t g_settingsPane = 0;               // the button's picture pane (P_SBBase_03)
uint32_t g_settingsPaneWindow = 0;         // ... for this window

bool Active() {
    return MenuSettings::PauseButtonActive();
}

uint32_t Window() {
    uint32_t window = 0;
    return Memory::TryRead32(kPausewindowInstance, window) && window >= 0x80000000u && window < 0x81800000u ? window : 0;
}

std::string PaneName(uint32_t pane) {
    std::string name;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t c = Memory::Read8(pane + 0xBC + i);
        if (c == 0) break;
        name += static_cast<char>(c);
    }
    return name;
}

// nw4r::lyt::Pane: children list at +0x10 (its terminator node at +0x14), each child linked at +4.
uint32_t FindPane(uint32_t pane, const std::string& name, int depth = 0) {
    if (depth > 12 || pane < 0x80000000u || pane >= 0x81800000u) return 0;
    if (PaneName(pane) == name) return pane;
    const uint32_t head = pane + 0x14;
    uint32_t node = 0;
    Memory::TryRead32(head, node);
    for (int guard = 0; node != 0 && node != head && guard < 256; ++guard) {
        if (const uint32_t found = FindPane(node - 4, name, depth + 1)) return found;
        if (!Memory::TryRead32(node, node)) break;
    }
    return 0;
}

uint32_t MapGroupName() {
    static const uint32_t found = FindString(kMapSettingsGroup);
    return found;
}

uint32_t RootPane(uint32_t object) {
    for (uint32_t off = 0; off < 0x200; off += 4) {
        uint32_t p = 0;
        if (Memory::TryRead32(object + 0x70 + off, p) && p >= 0x80000000u && p < 0x81800000u && PaneName(p) == "RootPane") {
            return p;
        }
    }
    return 0;
}

uint32_t SettingsPane(uint32_t window) {
    if (window == g_settingsPaneWindow && g_settingsPane != 0) return g_settingsPane;
    for (uint32_t off = 0; off < 0x200; off += 4) {
        uint32_t p = 0;
        if (Memory::TryRead32(window + kLayout + off, p) && p >= 0x80000000u && p < 0x81800000u &&
            PaneName(p) == "RootPane") {
            g_settingsPane = FindPane(p, "P_SBBase_03");
            g_settingsPaneWindow = window;
            return g_settingsPane;
        }
    }
    return 0;
}

bool MapActive() {
    return LayoutPatch::MapButtonPatched();
}

// The key trigger bits (WPAD buttons) of the player the menus read.
uint32_t Trigger(uint32_t player) {
    uint32_t keys = 0, key = 0, trigger = 0;
    if (player < 4 && Memory::TryRead32(kGameKeyInstance, keys) && keys != 0 && Memory::TryRead32(keys + 4 + player * 4, key) &&
        key != 0 && Memory::TryRead32(key + 0x1C, trigger)) {
        return trigger;
    }
    return 0;
}

// A call into the game with this thread's registers (the wrappers' callers expect them clobbered as by
// the functions they replace).
void Call(CpuContext* ctx, uint32_t target, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    InvokeIndirectCpu(target, ctx);
}

int Button(uint32_t value) {
    return value <= 2 ? static_cast<int>(value) : 0;
}

} // namespace

namespace MenuSettings {

bool PauseButtonActive() noexcept {
    return LayoutPatch::PauseButtonPatched();
}

bool TakeOpenRequest() noexcept {
    return g_openRequested.exchange(false, std::memory_order_acq_rel);
}

bool MapButtonActive() noexcept {
    return LayoutPatch::MapButtonPatched();
}

bool TakeMapOpenRequest() noexcept {
    return g_mapOpenRequested.exchange(false, std::memory_order_acq_rel);
}

bool PauseWindowClosed() noexcept {
    const uint32_t window = Window();
    return window == 0 || (Memory::Read8(window + kIsActive) == 0 && Memory::Read8(window + kIsAnimating) == 0);
}

void ReopenPauseWindow() noexcept {
    g_reopen.store(true, std::memory_order_release);
}

void ReopenMapMenu() noexcept {
    g_mapReopen.store(true, std::memory_order_release);
}

} // namespace MenuSettings

// The window's animation groups, with the Settings button's four after the game's eleven.
extern "C" void PauseGroupRegister_800c90f0(CpuContext* ctx) {
    // The world map's menu: its 19 groups and Settings' four, copies of Quick Save's (on, idle, hit, off).
    if (MapActive() && ctx->gpr[6] == kMapGroups && g_mapMenu != 0 && ctx->gpr[3] == g_mapMenu + kLayout && MapGroupName() != 0) {
        const uint32_t sp = ctx->gpr[1];
        const uint32_t frame = (sp - 0x100u) & ~0xFu;
        const uint32_t names = frame + 0x10, indices = names + 23 * 4;
        Memory::Write32(frame, sp);
        for (uint32_t i = 0; i < kMapGroups; ++i) {
            Memory::Write32(names + i * 4, Memory::Read32(ctx->gpr[4] + i * 4));
            Memory::Write32(indices + i * 4, Memory::Read32(ctx->gpr[5] + i * 4));
        }
        const uint32_t quickSave[4] = {4, 8, 12, 16};
        for (uint32_t i = 0; i < 4; ++i) {
            Memory::Write32(names + (kMapGroups + i) * 4, MapGroupName());
            Memory::Write32(indices + (kMapGroups + i) * 4, Memory::Read32(ctx->gpr[5] + quickSave[i] * 4));
        }
        ctx->gpr[1] = frame;
        ctx->gpr[4] = names;
        ctx->gpr[5] = indices;
        ctx->gpr[6] = kMapGroups + 4;
        func_800C90F0(ctx);
        ctx->gpr[1] = sp;
        RT_LOGF(RT_TAG_RUNTIME, "world map menu: Settings is one of its buttons\n");
        return;
    }
    const uint32_t window = Active() && ctx->gpr[6] == kWindowGroups ? Window() : 0;
    const uint32_t group = window != 0 ? GroupName() : 0;
    if (window == 0 || ctx->gpr[3] != window + kLayout || group == 0) {
        func_800C90F0(ctx);
        return;
    }
    // The extended tables on the stack, below the caller's frame (read only during the call): 15 names
    // and 15 animation numbers.
    const uint32_t sp = ctx->gpr[1];
    const uint32_t frame = (sp - 0x100u) & ~0xFu;
    const uint32_t names = frame + 0x10, indices = names + 15 * 4;
    Memory::Write32(frame, sp);  // a back chain, as a real frame has
    for (uint32_t i = 0; i < kWindowGroups; ++i) {
        Memory::Write32(names + i * 4, Memory::Read32(ctx->gpr[4] + i * 4));
        Memory::Write32(indices + i * 4, Memory::Read32(ctx->gpr[5] + i * 4));
    }
    const uint32_t animations[4] = {3, 4, 5, 6};  // onButton, idleButton, offButon, hitButton
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::Write32(names + (kWindowGroups + i) * 4, group);
        Memory::Write32(indices + (kWindowGroups + i) * 4, animations[i]);
    }
    ctx->gpr[1] = frame;
    ctx->gpr[4] = names;
    ctx->gpr[5] = indices;
    ctx->gpr[6] = kWindowGroups + 4;
    func_800C90F0(ctx);
    ctx->gpr[1] = sp;
    RT_LOGF(RT_TAG_RUNTIME, "pause window: Settings is one of its buttons\n");
}

extern "C" void PauseCursorSetup_8015ac30(CpuContext* ctx) {
    const uint32_t window = ctx->gpr[3];
    if (!Active() || Memory::Read32(window + kNextButton) != kSettings) {
        func_8015AC30(ctx);
        return;
    }
    if (const uint32_t pane = SettingsPane(window)) {
        Call(ctx, kSelectCursorSetup, pane, 0, 0);
    }
}

extern "C" void PauseOpenAnime_8015ad80(CpuContext* ctx) {
    const uint32_t window = ctx->gpr[3];
    func_8015AD80(ctx);
    if (!Active()) return;
    // The Settings button starts plain like the others.
    Call(ctx, kReverseAnimeStartSetup, window + kLayout, static_cast<uint32_t>(kOn[kSettings]), 0);
    if (g_reopening) {
        g_reopening = false;
        Memory::Write32(window + kNextButton, kSettings);  // back from the settings window: on Settings
    }
}

extern "C" void PauseButtonChange_8015ae70(CpuContext* ctx) {
    const uint32_t window = ctx->gpr[3];
    const uint32_t next = Memory::Read32(window + kNextButton);
    const int32_t active = static_cast<int32_t>(Memory::Read32(window + kActiveButton));
    if (!Active() || (next != kSettings && active != kSettings)) {
        func_8015AE70(ctx);
        return;
    }
    Call(ctx, kAllAnimeEndSetup, window + kLayout);
    Memory::Write8(window + kIsAnimating, 1);
    if (active >= 0) {
        Call(ctx, kAnimeStartSetup, window + kLayout, static_cast<uint32_t>(kOff[Button(static_cast<uint32_t>(active))]), 0);
    }
    Memory::Write32(window + kActiveButton, next);
    Call(ctx, kAnimeStartSetup, window + kLayout, static_cast<uint32_t>(kOn[Button(next)]), 0);
}

extern "C" void PauseHitAnime_8015b030(CpuContext* ctx) {
    const uint32_t window = ctx->gpr[3];
    if (!Active() || Memory::Read32(window + kNextButton) != kSettings) {
        func_8015B030(ctx);
        return;
    }
    Memory::Write8(window + kIsAnimating, 1);
    Call(ctx, kAnimeStartSetup, window + kLayout, static_cast<uint32_t>(kHit[kSettings]), 0);
}

// The cursor's move: `r4` is the cursor plus or minus one, in the game's numbering (Continue 0, Exit 1);
// on screen the order is Continue, Settings, Exit.
extern "C" void PauseCursorMove_800d0c00(CpuContext* ctx) {
    if (!Active()) {
        func_800D0C00(ctx);
        return;
    }
    const uint32_t manager = ctx->gpr[3];
    const uint32_t window = Window();
    const int cursor = Button(Memory::Read32(manager + kCursor));
    const int delta = static_cast<int32_t>(ctx->gpr[4]) - cursor < 0 ? -1 : 1;
    static constexpr int kScreen[3] = {0, 2, 1};  // button -> place on screen
    static constexpr int kButtonAt[3] = {0, 2, 1};  // place on screen -> button
    const int place = std::clamp(kScreen[cursor] + delta, 0, 2);
    const int target = kButtonAt[place];
    const uint8_t level = Memory::Read8(kStartGameInfo + 0xDu);
    if (target == 1) {
        // Exit only where the game allows it (dGameCom::isNowCourseClear or the cannon course), and never
        // in the map-enemy ambushes (levels 0x20-0x22), whose window doesn't move in the game either.
        Call(ctx, kIsNowCourseClear, 0);
        const bool allowed = (ctx->gpr[3] != 0 || level == 0x23) && !(level >= 0x20 && level <= 0x22);
        if (!allowed) return;
    }
    if (target == cursor || window == 0) return;
    Memory::Write32(window + kNextButton, static_cast<uint32_t>(target));
    Memory::Write32(manager + kCursor, static_cast<uint32_t>(target));
}

extern "C" void PauseKeyCheck_800d0cb0(CpuContext* ctx) {
    const uint32_t manager = ctx->gpr[3];
    const uint32_t window = Active() ? Window() : 0;
    if (window == 0) {
        func_800D0CB0(ctx);
        return;
    }
    // Back from the settings window: the pause window opens again through its own opening.
    if (g_reopen.exchange(false, std::memory_order_acq_rel)) {
        g_reopening = true;
        Memory::Write32(manager + kCursor, kSettings);
        Memory::Write8(window + kIsActive, 1);
        return;
    }
    const bool still = (Memory::Read32(kGameStop) & 0xFFFFFFFAu) == 0 && Memory::Read8(window + kIsAnimating) == 0 &&
                       Memory::Read8(window + kIsActive) != 0;
    if (still && Memory::Read32(manager + kCursor) == kSettings) {
        const uint32_t player = Memory::Read32(manager + kPlayer);
        uint32_t keys = 0, key = 0, trigger = 0;
        if (player < 4 && Memory::TryRead32(kGameKeyInstance, keys) && keys != 0 &&
            Memory::TryRead32(keys + 4 + player * 4, key) && key != 0 && Memory::TryRead32(key + 0x1C, trigger) &&
            (trigger & 0x0900u) != 0 && (trigger & 0x0010u) == 0) {  // 2 or A, not +
            Memory::Write8(window + kButtonHit, 1);  // the hit animation, then the window closes
            uint32_t sound = 0;
            if (Memory::TryRead32(kSndAudioMgr, sound) && sound != 0) {
                Call(ctx, kStartSystemSe, sound, 0x79, 1);  // SE_SYS_DECIDE
            }
            g_openRequested.store(true, std::memory_order_release);
            RT_LOGF(RT_TAG_RUNTIME, "pause window: Settings\n");
            return;
        }
    }
    func_800D0CB0(ctx);
}

// ---------------------------------------------------------------- the world map's + menu
extern "C" void MapMenuCreate_8077ab70(CpuContext* ctx) {
    if (MapActive()) {
        g_mapMenu = ctx->gpr[3];  // GroupRegister, below, knows its layout by this
        g_mapSeq = 0;
    }
    func_8077AB70(ctx);
}

extern "C" void MapMenuCursorSetup_8077ad70(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    if (!MapActive() || Memory::Read32(menu + kMapNext) != kMapSettings) {
        func_8077AD70(ctx);
        return;
    }
    static uint32_t s_menu = 0, s_pane = 0;
    if (menu != s_menu || s_pane == 0) {
        s_menu = menu;
        s_pane = FindPane(RootPane(menu), "P_SBBase_06");
    }
    if (s_pane != 0) {
        Call(ctx, kSelectCursorSetup, s_pane, 4, 0);  // the menu's cursor slot
    }
}

extern "C" void MapMenuOpenAnime_8077adc0(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    func_8077ADC0(ctx);
    if (MapActive()) {
        Call(ctx, kReverseAnimeStartSetup, menu + kLayout, MapOn(kMapSettings), 0);
    }
}

extern "C" void MapMenuButtonChange_8077af10(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    const uint32_t next = Memory::Read32(menu + kMapNext);
    const int32_t active = static_cast<int32_t>(Memory::Read32(menu + kMapActive));
    if (!MapActive() || (next != kMapSettings && active != static_cast<int32_t>(kMapSettings))) {
        func_8077AF10(ctx);
        return;
    }
    Call(ctx, kAllAnimeEndSetup, menu + kLayout);
    if (active >= 0) {
        uint32_t sound = 0;
        if (Memory::TryRead32(kSndAudioMgr, sound) && sound != 0) {
            Call(ctx, kStartSystemSe, sound, 0x78, 1);  // SE_SYS_CURSOR, as the menu plays on a move
        }
        Call(ctx, kAnimeStartSetup, menu + kLayout, MapOff(static_cast<uint32_t>(active)), 0);
    }
    Memory::Write32(menu + kMapActive, next);
    Call(ctx, kAnimeStartSetup, menu + kLayout, MapOn(next), 0);
}

// The menu's input: up and down move through the buttons in screen order, Settings between Quick Save
// and Title Screen; 2 or A on Settings opens the settings window. Everything else is the game's.
extern "C" void MapMenuInput_8077b010(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    if (!MapActive() || Memory::Read8(menu + kMapLocked) != 0) {
        func_8077B010(ctx);
        return;
    }
    if (g_mapSeq == 1) {  // Settings' hit flash, then the menu goes and the settings window opens
        if (++g_mapSeqFrames >= kMapHitFrames) {
            Call(ctx, kAnimeStartSetup, menu + kLayout, kMapOut, 0);
            g_mapSeq = 2;
            g_mapOpenRequested.store(true, std::memory_order_release);
            RT_LOGF(RT_TAG_RUNTIME, "world map menu: Settings (menu out)\n");
        }
        return;
    }
    if (g_mapSeq == 2) {  // the settings window is up: the menu waits, out of sight, for it to close
        if (g_mapReopen.exchange(false, std::memory_order_acq_rel)) {
            g_mapSeq = 0;
            // The menu's own open ends every group itself (the out animation too); ending one here
            // first ended it twice, and the second found no pane link and jumped to 0 (the S26 crash).
            func_8077ADC0(ctx);  // the menu's own open: every button off, its in-animation, its open state
            Call(ctx, kAnimeStartSetup, menu + kLayout, MapOn(kMapSettings), 0);  // Settings still picked
            RT_LOGF(RT_TAG_RUNTIME, "world map menu: back from Settings\n");
        }
        return;
    }
    const uint32_t cursor = Memory::Read32(menu + kMapNext);
    const uint32_t trigger = Trigger(Memory::Read32(kMapPlayer));
    const bool up = (trigger & 0x2u) != 0, down = !up && (trigger & 0x1u) != 0;
    const bool plus = (trigger & 0x10u) != 0, confirm = !up && !down && !plus && (trigger & 0x900u) != 0;
    uint32_t target = cursor;
    if (cursor == kMapSettings && up) target = 2;
    else if (cursor == kMapSettings && down) target = 3;
    else if (cursor == 2 && down) target = kMapSettings;
    else if (cursor == 3 && up) target = kMapSettings;
    if (target != cursor) {
        Memory::Write8(menu + kMapPicked, 0);
        Memory::Write32(menu + kMapNext, target);
        const uint32_t states = menu + kMapStates;
        Call(ctx, Memory::Read32(Memory::Read32(states) + 0x18), states, kMapStateButtonChange);
        return;
    }
    if (cursor == kMapSettings && confirm) {
        uint32_t sound = 0;
        if (Memory::TryRead32(kSndAudioMgr, sound) && sound != 0) {
            Call(ctx, kStartSystemSe, sound, 0x79, 1);  // SE_SYS_DECIDE
        }
        Call(ctx, kAnimeStartSetup, menu + kLayout, kMapSettingsHit, 0);
        g_mapReopen.store(false, std::memory_order_release);
        g_mapSeq = 1;
        g_mapSeqFrames = 0;
        return;
    }
    func_8077B010(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x800C90F0, PauseGroupRegister_800c90f0, "PauseGroupRegister_800c90f0");
REGISTER_NATIVE_FUNCTION_AS(0x8077AB70, MapMenuCreate_8077ab70, "MapMenuCreate_8077ab70");
REGISTER_NATIVE_FUNCTION_AS(0x8077AD70, MapMenuCursorSetup_8077ad70, "MapMenuCursorSetup_8077ad70");
REGISTER_NATIVE_FUNCTION_AS(0x8077ADC0, MapMenuOpenAnime_8077adc0, "MapMenuOpenAnime_8077adc0");
REGISTER_NATIVE_FUNCTION_AS(0x8077AF10, MapMenuButtonChange_8077af10, "MapMenuButtonChange_8077af10");
REGISTER_NATIVE_FUNCTION_AS(0x8077B010, MapMenuInput_8077b010, "MapMenuInput_8077b010");
REGISTER_NATIVE_FUNCTION_AS(0x8015AC30, PauseCursorSetup_8015ac30, "PauseCursorSetup_8015ac30");
REGISTER_NATIVE_FUNCTION_AS(0x8015AD80, PauseOpenAnime_8015ad80, "PauseOpenAnime_8015ad80");
REGISTER_NATIVE_FUNCTION_AS(0x8015AE70, PauseButtonChange_8015ae70, "PauseButtonChange_8015ae70");
REGISTER_NATIVE_FUNCTION_AS(0x8015B030, PauseHitAnime_8015b030, "PauseHitAnime_8015b030");
REGISTER_NATIVE_FUNCTION_AS(0x800D0C00, PauseCursorMove_800d0c00, "PauseCursorMove_800d0c00");
REGISTER_NATIVE_FUNCTION_AS(0x800D0CB0, PauseKeyCheck_800d0cb0, "PauseKeyCheck_800d0cb0");
