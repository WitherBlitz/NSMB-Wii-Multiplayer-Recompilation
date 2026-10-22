#include "host_suspend.h"

#include "audio_backend.h"
#include "det_clock.h"
#include "guest_interrupt_context.h"
#include "memory.h"
#include "netplay/game_menus.h"
#include "netplay_session.h"
#include "runtime_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace HostSuspend {
namespace {

using Clock = std::chrono::steady_clock;

// NSMBW (USA rev 1) addresses.
constexpr uint32_t kSaveGame = 0x8092F5F0u;                 // SaveGame(void*, bool quick), d_basesNP
constexpr uint32_t kStartDeleteQuickNandSave = 0x800E11D0u;  // dSaveMng_c::startDeleteQuickNandSave
constexpr uint32_t kSaveMngInstance = 0x8042A040u;          // dSaveMng_c::m_instance
constexpr uint32_t kNandThreadInstance = 0x80429FB8u;       // dNandThread_c::m_instance; busy at +0x74
constexpr uint32_t kNowScene = 0x80428730u;                 // dScene_c::m_nowScene (low halfword)
constexpr uint32_t kGameFlag = 0x80429F80u;                 // dInfo_c::mGameFlag
constexpr uint32_t kGameFlagFreeMode = 1u << 5, kGameFlagCoinBattle = 1u << 6;
constexpr uint32_t kSceneWorldMap = 0x003, kSceneStage = 0x005;
constexpr uint16_t kProfileOpeningTitle = 0x2BB;            // the title screen's demo runs as a STAGE
constexpr auto kSaveTimeout = std::chrono::seconds(4);
// How long a pause waits for the game thread to start its backup Quick Save before pausing without.
constexpr auto kBackupStartTimeout = std::chrono::seconds(1);

enum class State { Running, Saving, Paused, DeleteBackup };

std::mutex g_mutex;
std::condition_variable g_wake;
bool g_pauseWanted = false;   // guarded by g_mutex
bool g_backupWanted = false;  // guarded by g_mutex
std::atomic<bool> g_pauseRequested{false};  // g_pauseWanted, readable without the lock
State g_state = State::Running;  // the game thread's
bool g_backupWritten = false;    // the game thread's: a Quick Save of ours is on the NAND
Clock::time_point g_saveDeadline;
// A backup asked for, waiting for the game thread (OnGameThread) to start it; when it should have.
bool g_backupPending = false;
Clock::time_point g_backupStartDeadline;
bool g_backupDeclined = false;  // the game thread found a backup not allowed now (a menu open, ...)
std::atomic<uint64_t> g_pausedNs{0};
std::atomic<bool> g_paused{false};

bool NandBusy() {
    uint32_t thread = 0, busy = 0;
    return Memory::TryRead32(kNandThreadInstance, thread) && thread != 0 && Memory::TryRead32(thread + 0x74, busy) &&
           busy != 0;
}

// Only a story game on the world map or in a course is saved: not the title screen's demo, Free Mode,
// Coin Battle or a LAN session (whose NAND is a temporary copy).
bool BackupAllowed() {
    uint32_t scene = 0, flags = 0, saveMng = 0;
    if (!Memory::TryRead32(kNowScene, scene) || !Memory::TryRead32(kGameFlag, flags) ||
        !Memory::TryRead32(kSaveMngInstance, saveMng) || saveMng == 0) {
        return false;
    }
    scene &= 0xFFFFu;
    if (scene != kSceneWorldMap && scene != kSceneStage) {
        return false;
    }
    if ((flags & (kGameFlagFreeMode | kGameFlagCoinBattle)) != 0 || NetplaySession::Active()) {
        return false;
    }
    // With the world map's + menu or a level's pause window open, a Quick Save from here is taken as
    // the menu's own Quick Save, which ends the game once written: pause without a backup then.
    if (GameMenus::PauseMenuOpen()) {
        RT_LOGF(RT_TAG_RUNTIME, "suspend: a menu is open; pausing without a backup Quick Save\n");
        return false;
    }
    return !(scene == kSceneStage && GameMenus::HasProfile(kProfileOpeningTitle)) && !NandBusy();
}

void CallGuest(uint32_t function, uint32_t r3, uint32_t r4) {
    // Like a retrace callback: on a private copy of the interrupted thread's registers.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
    InvokeIndirectCpu(function, cpu);
}

// NSMBW_TEST_SUSPEND="<pause ms>:<resume ms>" (Windows testing): a pause with a backup at the first
// time after the game's first retrace, and a resume at the second.
void TestSchedule() {
    static const auto start = Clock::now();
    static const struct { long pauseMs, resumeMs; } plan = [] {
        const char* text = std::getenv("NSMBW_TEST_SUSPEND");
        if (text == nullptr) {
            return decltype(plan){-1, -1};
        }
        char* end = nullptr;
        const long pause = std::strtol(text, &end, 10);
        const long resume = end != nullptr && *end == ':' ? std::strtol(end + 1, nullptr, 10) : -1;
        return decltype(plan){pause, resume};
    }();
    static bool paused = false;
    if (plan.pauseMs < 0) {
        return;
    }
    const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    if (!paused && ms >= plan.pauseMs) {
        paused = true;
        RequestPause(true);
        if (plan.resumeMs >= 0) {
            // The game thread is about to block: resume from another thread.
            std::thread([resumeAt = start + std::chrono::milliseconds(plan.resumeMs)] {
                std::this_thread::sleep_until(resumeAt);
                RequestResume();
            }).detach();
        }
    }
}

} // namespace

void RequestPause(bool backup) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pauseWanted = true;
    g_backupWanted = g_backupWanted || backup;
    g_pauseRequested.store(true, std::memory_order_release);
}

void RequestResume() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pauseWanted = false;
        g_backupWanted = false;
        g_pauseRequested.store(false, std::memory_order_release);
    }
    g_wake.notify_all();
}

bool Paused() {
    return g_paused.load(std::memory_order_acquire);
}

uint64_t PausedNanoseconds() {
    return g_pausedNs.load(std::memory_order_acquire);
}

uint64_t OnRetrace() {
    TestSchedule();
    // The usual case, on every retrace check: running, and nobody asked for a pause.
    if (g_state == State::Running && !g_pauseRequested.load(std::memory_order_acquire)) {
        return 0;
    }
    // The guest code called from here can poll VI again: that nested retrace is left alone.
    static thread_local bool inside = false;
    if (inside) {
        return 0;
    }
    struct Guard {
        Guard() { inside = true; }
        ~Guard() { inside = false; }
    } guard;
    if (DetClock::Enabled()) {
        return 0;  // deterministic (network) play keeps its own time: never paused from here
    }
    bool pauseWanted = false, backupWanted = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pauseWanted = g_pauseWanted;
        backupWanted = g_backupWanted;
    }
    switch (g_state) {
    case State::Running:
        if (!pauseWanted) {
            g_backupPending = false;
            return 0;
        }
        if (backupWanted && !g_backupDeclined) {
            // The game thread starts the save (OnGameThread); the game runs on until it has.
            if (!g_backupPending) {
                g_backupPending = true;
                g_backupStartDeadline = Clock::now() + kBackupStartTimeout;
                return 0;
            }
            if (Clock::now() < g_backupStartDeadline) {
                return 0;
            }
            RT_LOGF(RT_TAG_RUNTIME, "suspend: the game thread did not save in time; pausing without a backup\n");
        }
        g_backupPending = false;
        g_backupDeclined = false;
        g_state = State::Paused;
        break;
    case State::Saving:
        if (NandBusy() && Clock::now() < g_saveDeadline) {
            return 0;
        }
        RT_LOGF(RT_TAG_RUNTIME, "suspend: backup %s\n", NandBusy() ? "still writing (timed out)" : "written");
        g_state = pauseWanted ? State::Paused : State::DeleteBackup;
        if (g_state == State::DeleteBackup) {
            return 0;
        }
        break;
    case State::DeleteBackup:
        if (NandBusy()) {
            return 0;
        }
        CallGuest(kStartDeleteQuickNandSave, Memory::Read32(kSaveMngInstance), 0);
        g_backupWritten = false;
        g_state = State::Running;
        RT_LOGF(RT_TAG_RUNTIME, "suspend: back in the game, backup Quick Save deleted\n");
        return 0;
    case State::Paused:
        break;
    }

    // Paused: block the game thread (and with it every guest thread) until resumed.
    RT_LOGF(RT_TAG_RUNTIME, "suspend: game paused\n");
    const auto stopped = Clock::now();
    g_paused.store(true, std::memory_order_release);
    AudioBackend::Instance().SetDevicePaused(true);  // nothing plays, so nothing is streamed
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait(lock, [] { return !g_pauseWanted; });
    }
    AudioBackend::Instance().SetDevicePaused(false);
    g_paused.store(false, std::memory_order_release);
    const uint64_t pausedNs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stopped).count());
    g_pausedNs.fetch_add(pausedNs, std::memory_order_acq_rel);
    g_state = g_backupWritten ? State::DeleteBackup : State::Running;
    RT_LOGF(RT_TAG_RUNTIME, "suspend: resumed after %.1f s\n", static_cast<double>(pausedNs) / 1e9);
    return pausedNs;
}

void OnGameThread() {
    if (!g_backupPending || g_state != State::Running) {
        return;
    }
    g_backupPending = false;
    if (!BackupAllowed()) {
        g_backupDeclined = true;  // OnRetrace pauses without one
        return;
    }
    CallGuest(kSaveGame, 0, 1);  // SaveGame(_, quick = true), as the + menu's Quick Save calls it
    g_backupWritten = true;
    g_saveDeadline = Clock::now() + kSaveTimeout;
    g_state = State::Saving;
    RT_LOGF(RT_TAG_RUNTIME, "suspend: writing a Quick Save as a backup\n");
}

} // namespace HostSuspend
