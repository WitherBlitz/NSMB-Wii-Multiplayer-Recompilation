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
//    had it, so no two players share a character. It is held
//    everywhere but the boot and strap screens, the file select included, so the player select, the
//    world map and each course create P1 as the chosen character. The world map's other players
//    and its own P1 take their models from elsewhere (HoldMapModels, PyMdlMngConstruct).
// Lockstep (LAN) play keeps the game's own rules: every machine has to run the same ones.
#include "gameplay_options.h"

#include "abi_bridge.h"
#include "det_clock.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_config.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace GameplayOptions {
namespace {

constexpr uint32_t kScene = 0x80428730u;                // dScene_c::m_nextScene, m_nowScene (halfwords)
constexpr uint32_t kStageTimerInstance = 0x8042A070u;   // dStageTimer_c::m_instance
constexpr uint32_t kPlayerType = 0x80354E60u;           // daPyMng_c::mPlayerType[4]
constexpr uint32_t kRest = 0x80354E90u;                 // daPyMng_c::mRest[4]
constexpr uint32_t kMapModels = 0x8093CDF0u;            // the world map player's model of each slot (d_basesNP)
constexpr uint16_t kSceneBoot = 0x000, kSceneStage = 0x005, kSceneBootStrap = 0x2EE;
constexpr int32_t kTimerStep = 92;                      // one frame of the course timer
constexpr int32_t kTimerSlack = 4 * kTimerStep;         // a few frames between two retraces

std::atomic<int> g_activeFile{-1};

uint32_t ChosenCharacter() {
    return RuntimeConfigFile::FileCharacter(g_activeFile.load(std::memory_order_relaxed));
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
