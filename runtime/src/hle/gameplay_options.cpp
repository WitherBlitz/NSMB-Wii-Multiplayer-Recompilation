// The Gameplay tab's options, applied once a retrace on the game thread (vi.cpp's AdvanceRetrace) by
// fixing up the game's own state rather than replacing its functions: translated code calls those
// directly, and Newer hooks some of them itself.
//  - No Time Limit: dStageTimer_c counts its time (+4, seconds << 12) down by 92 a frame while it
//    runs; each such step is given back, so the timer stands still. Bigger steps (the goal's time
//    bonus counting the timer off into the score) are left alone.
//  - Infinite Lives: daPyMng_c::mRest (one count a character) losing one life in a course is
//    undone. Lives only drop by a miss; a file's own counts are loaded on the file select.
//  - P1 Character (picked on the file select, one a save file): daPyMng_c::mPlayerType (the
//    character of each player slot) gets the active file's in slot 0, swapped with whichever slot
//    had it, so no two players share a character. It is held everywhere but the boot and strap
//    screens, the file select included, so the player select, the world map and each course create
//    P1 as the chosen character. The world map's other players and its own P1 take their models
//    from elsewhere (HoldMapModels, PyMdlMngConstruct).
//  - Power-Up: given on the game thread (OnGameThread). In a course P1's dAcPy_c is asked for it
//    the way an item does (ChangePowerupWithAnimation: the player changes, with the animation, as
//    soon as it moves again); on the world map P1's daPyMng_c::mPlayerMode (one a character) is set
//    and the map's own figure takes it, as from the map's Items screen (0x808C6DF0).
// Lockstep (LAN) play keeps the game's own rules: every machine has to run the same ones.
#include "gameplay_options.h"

#include "abi_bridge.h"
#include "det_clock.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "memory.h"
#include "netplay/game_menus.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace GameplayOptions {
namespace {

constexpr uint32_t kScene = 0x80428730u;                // dScene_c::m_nextScene, m_nowScene (halfwords)
constexpr uint32_t kStageTimerInstance = 0x8042A070u;   // dStageTimer_c::m_instance
constexpr uint32_t kPlayerType = 0x80354E60u;           // daPyMng_c::mPlayerType[4]
constexpr uint32_t kRest = 0x80354E90u;                 // daPyMng_c::mRest[4]
constexpr uint32_t kPlayerMode = 0x80354E70u;           // daPyMng_c::mPlayerMode[4], each character's power-up
constexpr uint32_t kMapModels = 0x8093CDF0u;            // the world map player's model of each slot (d_basesNP)
constexpr uint32_t kWmFigures = 0x809A2390u;            // the world map's players (d_basesNP bss)
constexpr uint32_t kWmFiguresByCharacter = 1304;        // their figures, one a character
constexpr uint32_t kWmFigureModels = 296;               // a figure's dPyMdlMng_c (+568 its power-up)
constexpr uint32_t kWmFigureChange = 0x808C6DF0u;       // a figure's power-up and model (the Items screen's call)
constexpr uint32_t kGetPlayer = 0x8005F900u;            // getPlayer__9daPyMng_cFi
constexpr uint32_t kChangePowerUp = 0x80145AC0u;        // dAcPy_c__ChangePowerupWithAnimation
constexpr uint16_t kSceneBoot = 0x000, kSceneWorldMap = 0x003, kSceneStage = 0x005, kSceneBootStrap = 0x2EE;
constexpr uint16_t kProfileOpeningTitle = 0x2BB;        // only on the title screen (its course is a stage too)
constexpr int32_t kTimerStep = 92;                      // one frame of the course timer
constexpr int32_t kTimerSlack = 4 * kTimerStep;         // a few frames between two retraces

std::atomic<int> g_activeFile{-1};
std::atomic<int> g_pendingPowerUp{-1};
std::atomic<bool> g_ourMap{false};  // the world map is the game's own (a mod's may differ): HoldMapModels

uint32_t ChosenCharacter() {
    return RuntimeConfigFile::FileCharacter(g_activeFile.load(std::memory_order_relaxed));
}

// Infinite Lives or the Power-Up did something: the file being played says "Cheated" from now on.
void MarkCheated() {
    const int file = g_activeFile.load(std::memory_order_relaxed);
    if (file >= 0 && !RuntimeConfigFile::FileCheated(file)) {
        RuntimeConfigFile::SetFileCheated(file, true);
    }
}

bool InGuestRam(uint32_t address) {
    return address >= 0x80000000u && address < 0x81800000u;
}

void HoldTimer(uint16_t scene) {
    static uint32_t s_timer = 0;
    static int32_t s_last = 0;
    uint32_t timer = 0, time = 0, stopped = 0;
    if (scene != kSceneStage || !RuntimeConfigFile::NoTimeLimit() || !Memory::TryRead32(kStageTimerInstance, timer) ||
        !InGuestRam(timer) || !Memory::TryRead32(timer + 4, time) || !Memory::TryRead32(timer + 12, stopped)) {
        s_timer = 0;
        return;
    }
    const int32_t now = static_cast<int32_t>(time);
    if (timer == s_timer && (stopped >> 24) == 0 && now < s_last && s_last - now <= kTimerSlack) {
        Memory::Write32(timer + 4, static_cast<uint32_t>(s_last));
        return;
    }
    s_timer = timer;
    s_last = now;
}

void HoldLives(uint16_t scene) {
    static bool s_valid = false;
    static std::array<int32_t, 4> s_last{};
    if (scene != kSceneStage || !RuntimeConfigFile::InfiniteLives()) {
        s_valid = false;
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t value = 0;
        if (!Memory::TryRead32(kRest + 4 * i, value)) {
            s_valid = false;
            return;
        }
        const int32_t now = static_cast<int32_t>(value);
        if (s_valid && now == s_last[i] - 1) {
            Memory::Write32(kRest + 4 * i, static_cast<uint32_t>(s_last[i]));
            MarkCheated();
            continue;
        }
        s_last[i] = now;
    }
    s_valid = true;
}

// The world map builds each player slot's model from a table of its own (one model, numbered like
// the characters, a slot) rather than from mPlayerType, so it follows the slots' characters while
// P1 isn't Mario, and has the game's own values back otherwise.
void HoldMapModels(const std::array<uint32_t, 4>& types, bool chosen) {
    static constexpr std::array<uint32_t, 4> kOriginal{0, 1, 3, 2};
    static bool s_checked = false, s_ours = false, s_changed = false;
    std::array<uint32_t, 4> models{};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!Memory::TryRead32(kMapModels + 4 * i, models[i])) {
            return;
        }
    }
    if (!s_checked) {
        if (models == std::array<uint32_t, 4>{}) {
            return;  // the world map's module isn't in yet
        }
        s_ours = models == kOriginal;  // anything else (a mod's own map) is left alone
        s_checked = true;
        g_ourMap = s_ours;
    }
    if (!s_ours) {
        return;
    }
    const std::array<uint32_t, 4>& wanted = chosen ? types : kOriginal;
    if (!chosen && !s_changed) {
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (models[i] != wanted[i]) {
            Memory::Write32(kMapModels + 4 * i, wanted[i]);
        }
    }
    s_changed = chosen;
}

void HoldCharacter(uint16_t scene) {
    if (scene == kSceneBoot || scene == kSceneBootStrap) {
        return;
    }
    std::array<uint32_t, 4> types{};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!Memory::TryRead32(kPlayerType + 4 * i, types[i]) || types[i] > 3) {
            return;  // not set up yet
        }
    }
    // Mario is the game's own P1: he is only put back after another file's pick moved him.
    static bool s_moved = false;
    const uint32_t chosen = ChosenCharacter();
    if ((chosen != 0 || s_moved) && types[0] != chosen) {
        for (uint32_t i = 1; i < 4; ++i) {
            if (types[i] == chosen) {
                types[i] = types[0];
                Memory::Write32(kPlayerType + 4 * i, types[i]);
                break;
            }
        }
        types[0] = chosen;
        Memory::Write32(kPlayerType, chosen);
    }
    s_moved = chosen != 0;
    HoldMapModels(types, chosen != 0);
}

} // namespace

void OnRetrace() {
    if (DetClock::Enabled()) {
        return;
    }
    uint32_t sceneWord = 0;
    if (!Memory::TryRead32(kScene, sceneWord)) {
        return;
    }
    const auto scene = static_cast<uint16_t>(sceneWord & 0xFFFFu);
    HoldTimer(scene);
    HoldLives(scene);
    HoldCharacter(scene);
}

void SetActiveFile(int file) {
    if (file >= 0 && file <= 2) {
        g_activeFile.store(file, std::memory_order_relaxed);
    }
}

int ActiveFile() {
    return g_activeFile.load(std::memory_order_relaxed);
}

namespace {

bool IsNewer() {
    return RuntimeProduct::Active().gameId == "SMNE03";
}

// A call into the game, as it could make one itself: on a private copy of this thread's registers.
uint32_t CallGame(uint32_t target, uint32_t r3, uint32_t r4) {
    GuestInterruptCallbackContext call;
    CpuContext* cpu = call.get();
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
    InvokeIndirectCpu(target, cpu);
    return cpu->gpr[3];
}

// True once the pending power-up is P1's.
bool GiveInCourse(int powerUp) {
    if (GameMenus::HasProfile(kProfileOpeningTitle)) {
        return false;  // the title screen's course: wait for the game
    }
    const uint32_t player = CallGame(kGetPlayer, 0, 0);
    if (!InGuestRam(player)) {
        return false;  // not in the course yet
    }
    CallGame(kChangePowerUp, player, static_cast<uint32_t>(powerUp));
    return true;
}

// As the map's Items screen does with a power-up item: daPyMng_c::mPlayerMode, then the figure's
// own power-up (which it takes into the next course) and model, through its change function.
bool GiveOnMap(int powerUp) {
    uint32_t type = 0;
    if (!Memory::TryRead32(kPlayerType, type) || type > 3) {
        return false;
    }
    Memory::Write32(kPlayerMode + 4 * type, static_cast<uint32_t>(powerUp));
    // Going into a course takes each figure's power-up back into mPlayerMode, so the figure has to
    // have it as well.
    uint32_t map = 0, figure = 0, models = 0;
    const bool ours = !IsNewer() && g_ourMap && Memory::TryRead32(kWmFigures, map) && InGuestRam(map) &&
                      Memory::TryRead32(map + kWmFiguresByCharacter + 4 * type, figure) && InGuestRam(figure) &&
                      Memory::TryRead32(figure + kWmFigureModels, models) && InGuestRam(models);
    if (ours) {
        CallGame(kWmFigureChange, figure, static_cast<uint32_t>(powerUp));
    }
    RT_LOGF(RT_TAG_RUNTIME, "gameplay: P1 (character %u) gets power-up %d on the world map%s\n", type, powerUp,
            ours ? ", its figure too" : "");
    return true;
}

} // namespace

int PowerUpCount() {
    return IsNewer() ? 8 : 7;
}

const char* PowerUpName(int powerUp) {
    static constexpr const char* kNames[] = {"Small", "Super Mushroom", "Fire Flower", "Mini Mushroom",
                                             "Propeller Mushroom", "Penguin Suit", "Ice Flower", "Hammer Suit"};
    return powerUp >= 0 && powerUp < PowerUpCount() ? kNames[powerUp] : "";
}

void GivePowerUp(int powerUp) {
    if (!DetClock::Enabled() && powerUp >= 0 && powerUp < PowerUpCount()) {
        g_pendingPowerUp.store(powerUp, std::memory_order_relaxed);
    }
}

bool PowerUpWaiting() {
    return g_pendingPowerUp.load(std::memory_order_relaxed) >= 0;
}

namespace {

// Automated runs: NSMBW_GIVE_POWERUP="<power-up>[@<ms>]" presses Power-Up that long after the
// first frame, as the Gameplay tab would.
void ScriptedPowerUp() {
    static int s_powerUp = -2;
    static std::chrono::steady_clock::time_point s_at;
    if (s_powerUp == -2) {
        s_powerUp = -1;
        if (const char* value = std::getenv("NSMBW_GIVE_POWERUP"); value != nullptr && *value != '\0') {
            char* end = nullptr;
            s_powerUp = static_cast<int>(std::strtol(value, &end, 10));
            const long delay = end != nullptr && *end == '@' ? std::strtol(end + 1, nullptr, 10) : 0;
            s_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
        }
    }
    if (s_powerUp >= 0 && std::chrono::steady_clock::now() >= s_at) {
        GivePowerUp(s_powerUp);
        s_powerUp = -1;
    }
}

} // namespace

void OnGameThread() {
    ScriptedPowerUp();
    int powerUp = g_pendingPowerUp.load(std::memory_order_relaxed);
    if (powerUp < 0 || DetClock::Enabled()) {
        return;
    }
    const uint16_t scene = GameMenus::CurrentScene();
    const bool given = scene == kSceneStage ? GiveInCourse(powerUp) : scene == kSceneWorldMap && GiveOnMap(powerUp);
    if (given) {
        g_pendingPowerUp.compare_exchange_strong(powerUp, -1, std::memory_order_relaxed);
        MarkCheated();
    }
}

} // namespace GameplayOptions

// dPyMdlMng_c::dPyMdlMng_c(ModelType_e), as translated: the vtable, two zeroed words, then the
// model type's setup (0x800D6D10). The world map's own player (its constructor calls this at
// 0x80902810) always asks for Mario's model; with another P1 character it gets that one's (the
// model types are numbered like the characters). Newer's own map player (from 0x80E09E70) is left
// as Mario: its map has only his model loaded, and anyone else's crashes it.
extern "C" void func_800D6D10(CpuContext* ctx);

namespace {

constexpr uint32_t kWorldMapPlayerModelCall = 0x80902814u;  // the return address of that call
constexpr uint32_t kPyMdlMngVtable = 0x80318A80u;

void PyMdlMngConstruct(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3], lr = ctx->lr;
    uint32_t type = ctx->gpr[4] & 0xFFu;
    if (lr == kWorldMapPlayerModelCall && !DetClock::Enabled() && GameplayOptions::ChosenCharacter() != 0) {
        uint32_t p1 = 0;
        if (Memory::TryRead32(GameplayOptions::kPlayerType, p1) && p1 <= 3) {
            type = p1;
        }
    }
    Memory::Write32(self, kPyMdlMngVtable);
    Memory::Write32(self + 4, 0);
    Memory::Write32(self + 8, 0);
    ctx->gpr[0] = 0;
    ctx->gpr[4] = type;
    ctx->gpr[5] = kPyMdlMngVtable;
    ctx->lr = 0x800D6CF4u;
    func_800D6D10(ctx);
    ctx->gpr[3] = self;
    ctx->lr = lr;
}

} // namespace

PPC_NATIVE_OVERRIDE_VOID(800D6CC0, PyMdlMngConstruct, (CpuContext* ctx), (ctx));
