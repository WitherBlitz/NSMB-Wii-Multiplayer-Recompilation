// Exit Course in every course, not only in the ones already cleared. The pause window asks
// dGameCom::isNowCourseClear (0x800B4DE0) three times: Pausewindow_c::create to grey out Exit, and
// PauseManager_c's cursor move (0x800D0C00) and confirm (0x800D0CB0) to refuse it. Saying "cleared"
// there lets the player leave through the game's own quit path (dScStage_c::m_exitMode 2: back on
// the course's map node with the power-ups the course began with, nothing saved, nothing unlocked).
// The map-enemy ambushes (STAGE_ENEMY 1-3, levels 0x20-0x22) keep the game's own answer: their
// cursor never moves in the game either, so Exit stays grey and locked there as on the Wii.
// The same in every build and on every peer, so LAN lockstep sees the same game; the LAN protocol
// version changed with it (netplay_lobby.cpp, netplay_session.cpp).
#include "abi_bridge.h"
#include "memory.h"

#include <cstdint>

extern "C" void func_800B4DE0(CpuContext* ctx);  // the translated dGameCom::isNowCourseClear

extern "C" void PauseExitAnyCourse_800b4de0(CpuContext* ctx) {
    func_800B4DE0(ctx);
    if (ctx->gpr[3] != 0) {
        return;
    }
    constexpr uint32_t kNowScene = 0x80428732u;       // dScene_c::m_nowScene (5 = STAGE)
    constexpr uint32_t kStartGameInfo = 0x80315B90u;  // dInfo_c::m_startGameInfo, +0xD the level
    if (Memory::Read16(kNowScene) != 5) {
        return;
    }
    const uint8_t level = Memory::Read8(kStartGameInfo + 0xDu);
    if (level >= 0x20 && level <= 0x22) {
        return;  // a map-enemy ambush
    }
    ctx->gpr[3] = 1;
}

REGISTER_NATIVE_FUNCTION_AS(0x800B4DE0, PauseExitAnyCourse_800b4de0, "PauseExitAnyCourse_800b4de0");
