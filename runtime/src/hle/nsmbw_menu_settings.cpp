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
// Star Coins, Add/Drop Players, Quick Save (0-2), Settings (4), Title Screen (3) on screen. Its create
// and all six of its states are native here (ported from the game's code, below), Settings built in.
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
constexpr uint32_t kGroupRegister = 0x800C90F0u;

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

// dCourseSelectMenu_c, the world map's + menu. Its fields: +0x70 the layout (LytBase_c: +0x184 the
// animation groups, +0x188 each group's playing flag, +0x190 how many, +0x194 the last one started),
// +0x208 its state manager, +0x244 the window pane, +0x248 its six button panes, +0x260 its two text
// boxes, +0x268 the button the cursor goes to, +0x26C the one it is on, +0x270 built, +0x271 open (the
// state manager runs only then; the world map waits while it is), +0x272 a button was picked, +0x273
// input off, +0x274 the cursor wants placing. Animation groups: 0 in, 1 loop, on 2-5, idle 6-9, hit
// 10-13, off 14-17, 18 out; Settings' on, idle, hit, off are 19-22.
constexpr uint32_t kMapWindow = 0x244, kMapButtonPanes = 0x248, kMapTexts = 0x260, kMapNext = 0x268,
                   kMapActive = 0x26C, kMapOpen = 0x271, kMapPicked = 0x272, kMapLocked = 0x273,
                   kMapPlaceCursor = 0x274;
constexpr uint32_t kMapStates = 0x208;  // its state manager (changeState is its vtable's +0x18)
// Its states, in order: StartWait (runs once the world map opens it: starts the in-animation), Open,
// ButtonChange, PauseDisp (input), Hit, Exit.
constexpr uint32_t kMapStateStartWait = 0x809915F0u, kMapStateOpen = 0x80991630u,
                   kMapStateButtonChange = 0x80991670u, kMapStatePauseDisp = 0x809916B0u,
                   kMapStateHit = 0x809916F0u, kMapStateExit = 0x80991730u;
// Its create: the resource names (from 0x80941288), the groups' animation numbers, the text boxes'
// messages, and the cursor slot of each button.
constexpr uint32_t kMapNames = 0x80941288u, kMapGroupAnims = 0x80933FD8u, kMapTextMessages = 0x80934024u,
                   kMapCursorPanes = 0x80934040u;
constexpr uint32_t kReadResource = 0x800C8CB0u, kAnimResRegister = 0x800C8FB0u, kPPaneRegister = 0x800C8E40u,
                   kTPaneRegister = 0x800C8EB0u, kTextMessages = 0x800C8F20u;
constexpr uint32_t kSelectCursor = 0x8042A2C8u, kSelectCursorCancel = 0x8010C780u;  // dSelectCursor_c
constexpr uint32_t kMapCloseKey = 0x800B53A0u;  // another way out than +, as the game checks it
constexpr uint32_t kMapKeys = 0x8042A468u;      // its vtable's +0x20: was one of these pressed (2 or A)
constexpr uint32_t kMapPlayer = 0x8042A464u;    // the player the menu reads
constexpr uint32_t kMapIn = 0, kMapOut = 18;
constexpr uint32_t kMapGroups = 19;
constexpr uint32_t kMapSettings = 4;
constexpr uint32_t kSeCursor = 0x78, kSeDecide = 0x79, kSeMenuClose = 0x7A;
constexpr const char* kMapSettingsGroup = "B01_noButton";  // layout_patch.cpp
std::atomic<bool> g_mapOpenRequested{false};  // Settings was picked: the settings window opens
std::atomic<bool> g_mapReopen{false};         // the settings window closed: the menu opens again
// Settings after its hit: 0 not picked, 1 the menu going out, 2 gone (the settings window is up).
int g_mapSettings = 0;
bool g_mapBackToSettings = false;  // the menu is opening again after the settings window: Settings picked

uint32_t MapOn(uint32_t button) { return button < 4 ? 2 + button : 19; }
uint32_t MapHit(uint32_t button) { return button < 4 ? 10 + button : 21; }
uint32_t MapOff(uint32_t button) { return button < 4 ? 14 + button : 22; }

std::atomic<bool> g_openRequested{false};  // the button was pressed: the settings window opens
std::atomic<bool> g_reopen{false};         // the settings window closed: the pause window opens again
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
void Call(CpuContext* ctx, uint32_t target, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0,
          uint32_t r7 = 0) {
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[7] = r7;
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

} // namespace MenuSettings

// The window's animation groups, with the Settings button's four after the game's eleven.
extern "C" void PauseGroupRegister_800c90f0(CpuContext* ctx) {
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
// dCourseSelectMenu_c's create and states, as the game has them (ported from its code), with Settings
// among its buttons while the layout has it: Star Coins, Add/Drop Players, Quick Save (0-2), Settings
// (4), Title Screen (3) on screen. Picking Settings plays its hit animation, then the menu's out
// animation; the settings window opens once the menu is gone, the world map still waiting on it. When
// the window closes, the menu opens again through its own StartWait state, Settings picked.
// (Each state that waits on an animation waits on the last one started: Settings' button is set up
// before the in-animation starts, so the menu waits for that, as it does without Settings.)
namespace {

void MapChangeState(CpuContext* ctx, uint32_t menu, uint32_t state) {
    const uint32_t states = menu + kMapStates;
    Call(ctx, Memory::Read32(Memory::Read32(states) + 0x18), states, state);
}

// LytBase_c::isAnime(-1): the last animation started is still playing.
bool MapAnimating(uint32_t menu) {
    const uint32_t layout = menu + kLayout;
    const int32_t last = static_cast<int32_t>(Memory::Read32(layout + 0x194));
    return Memory::Read8(Memory::Read32(layout + 0x188) + static_cast<uint32_t>(last)) != 0;
}

void MapSound(CpuContext* ctx, uint32_t id) {
    uint32_t sound = 0;
    if (Memory::TryRead32(kSndAudioMgr, sound) && sound != 0) {
        Call(ctx, kStartSystemSe, sound, id, 1);
    }
}

void MapShowWindow(uint32_t menu, bool shown) {
    const uint32_t pane = Memory::Read32(menu + kMapWindow);
    const uint8_t flags = Memory::Read8(pane + 0xBB);
    Memory::Write8(pane + 0xBB, static_cast<uint8_t>(shown ? (flags | 1u) : (flags & 0xFEu)));
}

// The next button up or down in screen order, round at the ends.
uint32_t MapStep(uint32_t button, bool down) {
    static constexpr uint32_t kOrder[5] = {0, 1, 2, kMapSettings, 3};
    const uint32_t count = MapActive() ? 5 : 4;
    uint32_t at = 0;
    while (at < count && kOrder[at] != button) ++at;
    if (at == count) at = 0;
    return kOrder[(at + (down ? 1 : count - 1)) % count];
}

}  // namespace

namespace MenuSettings {

void ReopenMapMenu() noexcept {
    g_mapReopen.store(true, std::memory_order_release);
}

}  // namespace MenuSettings

// create: its layout, animations, groups (Settings' four after the game's 19, copies of Quick Save's
// on, idle, hit and off), panes and messages. Returns whether the layout's resource was there.
extern "C" void MapMenuCreate_8077ab70(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    const uint32_t layout = menu + kLayout;
    g_mapSettings = 0;
    g_mapBackToSettings = false;
    g_mapReopen.store(false, std::memory_order_release);
    Call(ctx, kReadResource, layout, kMapNames + 780, 0);
    if (ctx->gpr[3] == 0) {
        return;
    }
    Call(ctx, Memory::Read32(Memory::Read32(layout + 8) + 20), layout, kMapNames + 816, 0);  // Build
    Call(ctx, kAnimResRegister, layout, kMapNames + 316, 7);
    const uint32_t settingsGroup = MapActive() ? MapGroupName() : 0;
    if (settingsGroup == 0) {
        Call(ctx, kGroupRegister, layout, kMapNames + 404, kMapGroupAnims, kMapGroups);
    } else {
        // The extended tables on the stack, below the caller's frame (read only during the call).
        const uint32_t sp = ctx->gpr[1];
        const uint32_t frame = (sp - 0x100u) & ~0xFu;
        const uint32_t names = frame + 0x10, anims = names + (kMapGroups + 4) * 4;
        Memory::Write32(frame, sp);  // a back chain, as a real frame has
        for (uint32_t i = 0; i < kMapGroups; ++i) {
            Memory::Write32(names + i * 4, Memory::Read32(kMapNames + 404 + i * 4));
            Memory::Write32(anims + i * 4, Memory::Read32(kMapGroupAnims + i * 4));
        }
        const uint32_t quickSave[4] = {4, 8, 12, 16};  // its on, idle, hit, off
        for (uint32_t i = 0; i < 4; ++i) {
            Memory::Write32(names + (kMapGroups + i) * 4, settingsGroup);
            Memory::Write32(anims + (kMapGroups + i) * 4, Memory::Read32(kMapGroupAnims + quickSave[i] * 4));
        }
        ctx->gpr[1] = frame;
        Call(ctx, kGroupRegister, layout, names, anims, kMapGroups + 4);
        ctx->gpr[1] = sp;
        RT_LOGF(RT_TAG_RUNTIME, "world map menu: Settings is one of its buttons\n");
    }
    Memory::Write32(menu + kMapWindow, Memory::Read32(layout + 32));  // the root pane
    Call(ctx, kPPaneRegister, layout, kMapNames + 552, menu + kMapButtonPanes, 6);
    Call(ctx, kTPaneRegister, layout, kMapNames + 616, menu + kMapTexts, 2);
    Call(ctx, kTextMessages, layout, kMapNames + 752, kMapTextMessages, 2, 7);
    ctx->gpr[3] = 1;
}

// The game's corner cursor onto the button it goes to.
extern "C" void MapMenuCursorSetup_8077ad70(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    const uint32_t next = Memory::Read32(menu + kMapNext);
    uint32_t pane = 0;
    if (MapActive() && next == kMapSettings) {
        static uint32_t s_menu = 0, s_pane = 0;
        if (menu != s_menu || s_pane == 0) {
            s_menu = menu;
            s_pane = FindPane(RootPane(menu), "P_SBBase_06");
        }
        pane = s_pane;
    } else if (next < 4) {
        pane = Memory::Read32(menu + kMapButtonPanes + Memory::Read32(kMapCursorPanes + next * 4) * 4);
    }
    if (pane != 0) {
        Call(ctx, kSelectCursorSetup, pane, 4, 0);  // the menu's cursor slot
    }
}

// StartWait (execute): the world map has opened the menu. Every button plain, the in-animation last.
extern "C" void MapMenuStartWait_8077adc0(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    const uint32_t layout = menu + kLayout;
    Call(ctx, kAllAnimeEndSetup, layout);
    for (uint32_t button = 0; button < 3; ++button) {
        Call(ctx, kReverseAnimeStartSetup, layout, MapOn(button), 0);
    }
    if (MapActive()) {
        Call(ctx, kReverseAnimeStartSetup, layout, MapOn(kMapSettings), 0);
    }
    Call(ctx, kAnimeStartSetup, layout, kMapIn, 0);
    Memory::Write8(menu + kMapPicked, 0);
    MapShowWindow(menu, true);
    MapChangeState(ctx, menu, kMapStateOpen);
}

// Open (execute): once the in-animation has played, the cursor's button lights up.
extern "C" void MapMenuOpen_8077ae90(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    if (!MapAnimating(menu)) {
        MapChangeState(ctx, menu, kMapStateButtonChange);
    }
}

// Open (finalize): the cursor starts on Star Coins, or on Settings when back from its window.
extern "C" void MapMenuOpenEnd_8077aef0(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    Memory::Write32(menu + kMapNext, g_mapBackToSettings ? kMapSettings : 0);
    Memory::Write32(menu + kMapActive, 0xFFFFFFFFu);
    g_mapBackToSettings = false;
}

// ButtonChange (initialize): the old button off (with the cursor sound), the new one on.
extern "C" void MapMenuButtonChange_8077af10(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    const uint32_t layout = menu + kLayout;
    Call(ctx, kAllAnimeEndSetup, layout);
    const int32_t active = static_cast<int32_t>(Memory::Read32(menu + kMapActive));
    if (active >= 0) {
        MapSound(ctx, kSeCursor);
        Call(ctx, kAnimeStartSetup, layout, MapOff(static_cast<uint32_t>(active)), 0);
    }
    const uint32_t next = Memory::Read32(menu + kMapNext);
    Memory::Write32(menu + kMapActive, next);
    Call(ctx, kAnimeStartSetup, layout, MapOn(next), 0);
}

// ButtonChange (execute): once the new button is on, the menu takes input.
extern "C" void MapMenuButtonChanged_8077af90(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    if (!MapAnimating(menu)) {
        MapChangeState(ctx, menu, kMapStatePauseDisp);
    }
}

// PauseDisp (initialize): the cursor is placed again.
extern "C" void MapMenuPauseDispBegin_8077b000(CpuContext* ctx) {
    Memory::Write8(ctx->gpr[3] + kMapPlaceCursor, 1);
}

// PauseDisp (execute), its input: up and down through the buttons, + (or the game's other way out)
// closes, 2 or A picks.
extern "C" void MapMenuInput_8077b010(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    Memory::Write8(menu + kMapPicked, 0);
    if (Memory::Read8(menu + kMapLocked) != 0) {
        return;
    }
    MapMenuCursorSetup_8077ad70(ctx);
    const uint32_t trigger = Trigger(Memory::Read32(kMapPlayer));
    if ((trigger & 0x3u) != 0) {  // 0x2 up, else 0x1 down
        Memory::Write32(menu + kMapNext, MapStep(Memory::Read32(menu + kMapNext), (trigger & 0x2u) == 0));
        MapChangeState(ctx, menu, kMapStateButtonChange);
        return;
    }
    bool close = (trigger & 0x10u) != 0;
    if (!close) {
        Call(ctx, kMapCloseKey, 0);
        close = ctx->gpr[3] != 0;
    }
    if (close) {
        MapChangeState(ctx, menu, kMapStateExit);
        return;
    }
    const uint32_t keys = Memory::Read32(kMapKeys);
    Call(ctx, Memory::Read32(Memory::Read32(keys) + 0x20), keys, 0x900);
    if (ctx->gpr[3] != 0) {
        MapSound(ctx, kSeDecide);
        MapChangeState(ctx, menu, kMapStateHit);
    }
}

// PauseDisp (finalize): the cursor goes.
extern "C" void MapMenuPauseDispEnd_8077b190(CpuContext* ctx) {
    Call(ctx, kSelectCursorCancel, Memory::Read32(kSelectCursor), 4);
}

// Hit (initialize): the picked button's hit animation.
extern "C" void MapMenuHit_8077b1a0(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    g_mapSettings = 0;
    Call(ctx, kAnimeStartSetup, menu + kLayout, MapHit(Memory::Read32(menu + kMapNext)), 0);
}

// Hit (execute): once it has played, the menu closes with the button picked. For Settings the menu
// goes out instead, the settings window opens once it's gone, and when that has closed the menu opens
// again.
extern "C" void MapMenuHitWait_8077b1c0(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    if (MapActive() && Memory::Read32(menu + kMapNext) == kMapSettings) {
        if (g_mapSettings == 0 && !MapAnimating(menu)) {
            Call(ctx, kAllAnimeEndSetup, menu + kLayout);
            Call(ctx, kAnimeStartSetup, menu + kLayout, kMapOut, 0);
            g_mapSettings = 1;
        } else if (g_mapSettings == 1 && !MapAnimating(menu)) {
            MapShowWindow(menu, false);
            g_mapSettings = 2;
            g_mapReopen.store(false, std::memory_order_release);
            g_mapOpenRequested.store(true, std::memory_order_release);
            RT_LOGF(RT_TAG_RUNTIME, "world map menu: Settings\n");
        } else if (g_mapSettings == 2 && g_mapReopen.exchange(false, std::memory_order_acq_rel)) {
            g_mapSettings = 0;
            g_mapBackToSettings = true;
            MapChangeState(ctx, menu, kMapStateStartWait);  // its own opening, from the next frame
            RT_LOGF(RT_TAG_RUNTIME, "world map menu: back from Settings\n");
        }
        return;
    }
    if (!MapAnimating(menu)) {
        Memory::Write8(menu + kMapPicked, 1);
        MapChangeState(ctx, menu, kMapStateExit);
    }
}

// Exit (initialize): the close sound and the out animation.
extern "C" void MapMenuExit_8077b230(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    MapSound(ctx, kSeMenuClose);
    Call(ctx, kAllAnimeEndSetup, menu + kLayout);
    Call(ctx, kAnimeStartSetup, menu + kLayout, kMapOut, 0);
}

// Exit (execute): once it has played, the menu is closed and hidden, waiting to be opened.
extern "C" void MapMenuExitWait_8077b290(CpuContext* ctx) {
    const uint32_t menu = ctx->gpr[3];
    if (!MapAnimating(menu)) {
        Memory::Write8(menu + kMapOpen, 0);
        MapShowWindow(menu, false);
        MapChangeState(ctx, menu, kMapStateStartWait);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x800C90F0, PauseGroupRegister_800c90f0, "PauseGroupRegister_800c90f0");
REGISTER_NATIVE_FUNCTION_AS(0x8077AB70, MapMenuCreate_8077ab70, "MapMenuCreate_8077ab70");
REGISTER_NATIVE_FUNCTION_AS(0x8077AD70, MapMenuCursorSetup_8077ad70, "MapMenuCursorSetup_8077ad70");
REGISTER_NATIVE_FUNCTION_AS(0x8077ADC0, MapMenuStartWait_8077adc0, "MapMenuStartWait_8077adc0");
REGISTER_NATIVE_FUNCTION_AS(0x8077AE90, MapMenuOpen_8077ae90, "MapMenuOpen_8077ae90");
REGISTER_NATIVE_FUNCTION_AS(0x8077AEF0, MapMenuOpenEnd_8077aef0, "MapMenuOpenEnd_8077aef0");
REGISTER_NATIVE_FUNCTION_AS(0x8077AF10, MapMenuButtonChange_8077af10, "MapMenuButtonChange_8077af10");
REGISTER_NATIVE_FUNCTION_AS(0x8077AF90, MapMenuButtonChanged_8077af90, "MapMenuButtonChanged_8077af90");
REGISTER_NATIVE_FUNCTION_AS(0x8077B000, MapMenuPauseDispBegin_8077b000, "MapMenuPauseDispBegin_8077b000");
REGISTER_NATIVE_FUNCTION_AS(0x8077B010, MapMenuInput_8077b010, "MapMenuInput_8077b010");
REGISTER_NATIVE_FUNCTION_AS(0x8077B190, MapMenuPauseDispEnd_8077b190, "MapMenuPauseDispEnd_8077b190");
REGISTER_NATIVE_FUNCTION_AS(0x8077B1A0, MapMenuHit_8077b1a0, "MapMenuHit_8077b1a0");
REGISTER_NATIVE_FUNCTION_AS(0x8077B1C0, MapMenuHitWait_8077b1c0, "MapMenuHitWait_8077b1c0");
REGISTER_NATIVE_FUNCTION_AS(0x8077B230, MapMenuExit_8077b230, "MapMenuExit_8077b230");
REGISTER_NATIVE_FUNCTION_AS(0x8077B290, MapMenuExitWait_8077b290, "MapMenuExitWait_8077b290");
REGISTER_NATIVE_FUNCTION_AS(0x8015AC30, PauseCursorSetup_8015ac30, "PauseCursorSetup_8015ac30");
REGISTER_NATIVE_FUNCTION_AS(0x8015AD80, PauseOpenAnime_8015ad80, "PauseOpenAnime_8015ad80");
REGISTER_NATIVE_FUNCTION_AS(0x8015AE70, PauseButtonChange_8015ae70, "PauseButtonChange_8015ae70");
REGISTER_NATIVE_FUNCTION_AS(0x8015B030, PauseHitAnime_8015b030, "PauseHitAnime_8015b030");
REGISTER_NATIVE_FUNCTION_AS(0x800D0C00, PauseCursorMove_800d0c00, "PauseCursorMove_800d0c00");
REGISTER_NATIVE_FUNCTION_AS(0x800D0CB0, PauseKeyCheck_800d0cb0, "PauseKeyCheck_800d0cb0");
