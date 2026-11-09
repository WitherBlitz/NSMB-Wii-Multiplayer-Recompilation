// The Gameplay tab's options, applied once a retrace on the game thread (vi.cpp's AdvanceRetrace) by
// fixing up the game's own state rather than replacing its functions: translated code calls those
// directly, and Newer hooks some of them itself.
//  - No Time Limit: dStageTimer_c counts its time (+4, seconds << 12) down by 92 a frame while it
//    runs; each such step is given back, so the timer stands still. Bigger steps (the goal's time
//    bonus counting the timer off into the score) are left alone. The HUD's timer is hidden too.
//  - Infinite Lives: daPyMng_c::mRest (one count a character) losing one life in a course is
//    undone. Lives only drop by a miss; a file's own counts are loaded on the file select.
//  - P1's character: picked on the game's own character screen, which any player can use for any
//    character (projects/nsmbw/tools/p1_character.py). Each save file remembers its last one
//    (RememberCharacter) and starts from it (StartCardsOnSavedCharacter).
//  - Power-Up: given on the game thread (OnGameThread). In a course P1's dAcPy_c is asked for it
//    the way an item does (ChangePowerupWithAnimation: the player changes, with the animation, as
//    soon as it moves again; a Star through its setStar); on the world map P1's
//    daPyMng_c::mPlayerMode (one a character) is set and the map's own figure takes it, as from the
//    map's Items screen (0x808C6DF0), or for a Star daPyMng_c::mCreateItem, which starts the next
//    course with one.
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
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace GameplayOptions {
namespace {

constexpr uint32_t kScene = 0x80428730u;                // dScene_c::m_nextScene, m_nowScene (halfwords)
constexpr uint32_t kStageTimerInstance = 0x8042A070u;   // dStageTimer_c::m_instance
constexpr uint32_t kPlayerType = 0x80354E60u;           // daPyMng_c::mPlayerType[4]
constexpr uint32_t kRest = 0x80354E90u;                 // daPyMng_c::mRest[4]
constexpr uint32_t kPlayerMode = 0x80354E70u;           // daPyMng_c::mPlayerMode[4], each character's power-up
constexpr uint32_t kCreateItem = 0x80354E80u;           // daPyMng_c::mCreateItem[4], one a character; bit 0: a Star
constexpr uint32_t kMapModels = 0x8093CDF0u;            // the world map player's model of each slot (d_basesNP)
constexpr uint32_t kWmFigures = 0x809A2390u;            // the world map's players (d_basesNP bss)
constexpr uint32_t kWmFiguresByCharacter = 1304;        // their figures, one a character
constexpr uint32_t kWmFigureModels = 296;               // a figure's dPyMdlMng_c (+568 its power-up)
constexpr uint32_t kWmFigureChange = 0x808C6DF0u;       // a figure's power-up and model (the Items screen's call)
constexpr uint32_t kGetPlayer = 0x8005F900u;            // getPlayer__9daPyMng_cFi
constexpr uint32_t kChangePowerUp = 0x80145AC0u;        // dAcPy_c__ChangePowerupWithAnimation
constexpr uint32_t kPlayerVtable = 0x60, kPlayerSetStar = 0x3D8;  // dAcPy_c's vtable, and its setStar(kind, frames)
constexpr uint32_t kStarFrames = 0x294;                 // what the Star item gives
constexpr uint32_t kGameDisplayInstance = 0x8042A328u;  // dGameDisplay_c::m_instance
constexpr uint32_t kGameDisplayTime = 0x520;            // its N_time_00 pane
constexpr uint32_t kPaneFlags = 0xBB;                   // nw4r::lyt::Pane's flags byte; bit 0: visible
constexpr uint16_t kSceneWorldMap = 0x003, kSceneStage = 0x005, kSceneGameSetup = 0x00A;
constexpr uint16_t kProfileOpeningTitle = 0x2BB;        // only on the title screen (its course is a stage too)
constexpr int32_t kTimerStep = 92;                      // one frame of the course timer
constexpr int32_t kTimerSlack = 4 * kTimerStep;         // a few frames between two retraces

std::atomic<int> g_activeFile{-1};
std::atomic<int> g_pendingPowerUp{-1};

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

// No Time Limit has no time to show. Shown again if the option is turned off in the course; a new
// course builds its HUD anew.
void HideTimer(uint16_t scene) {
    static uint32_t s_hidden = 0;
    uint32_t display = 0, pane = 0;
    if (scene != kSceneStage || !Memory::TryRead32(kGameDisplayInstance, display) || !InGuestRam(display) ||
        !Memory::TryRead32(display + kGameDisplayTime, pane) || !InGuestRam(pane)) {
        s_hidden = 0;
        return;
    }
    const uint8_t flags = Memory::Read8(pane + kPaneFlags);
    if (RuntimeConfigFile::NoTimeLimit()) {
        if ((flags & 1) != 0) {
            Memory::Write8(pane + kPaneFlags, flags & ~1u);
        }
        s_hidden = pane;
    } else if (s_hidden == pane) {
        Memory::Write8(pane + kPaneFlags, flags | 1u);
        s_hidden = 0;
    }
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

// On the setup screens each player's card starts on a character from a table of the game's, one a
// slot: P1's is the save file's last character, swapped with whichever slot had it, so no two start the
// same. The numbers are the card's own order: 0 Mario (mkwcat's code shows him as its 4), then 3 is
// character 1, 1 is 2 and 2 is 3.
void StartCardsOnSavedCharacter(uint16_t scene) {
    constexpr uint32_t kCardStarts = 0x80933D20u;
    static constexpr std::array<uint32_t, 4> kOriginal{0, 3, 2, 1}, kCardOfCharacter{0, 3, 1, 2};
    const int file = g_activeFile.load(std::memory_order_relaxed);
    if (scene != kSceneGameSetup || file < 0) {
        return;
    }
    std::array<uint32_t, 4> now{};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!Memory::TryRead32(kCardStarts + 4 * i, now[i]) || now[i] > 3) {
            return;
        }
    }
    if (now[0] + now[1] + now[2] + now[3] != 6) {
        return;  // the module isn't in yet, or a mod's own table
    }
    std::array<uint32_t, 4> wanted = kOriginal;
    const uint32_t card = kCardOfCharacter[std::min<uint32_t>(RuntimeConfigFile::FileCharacter(file), 3)];
    for (uint32_t i = 1; i < 4; ++i) {
        if (wanted[i] == card) {
            std::swap(wanted[0], wanted[i]);
            break;
        }
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (now[i] != wanted[i]) {
            Memory::Write32(kCardStarts + 4 * i, wanted[i]);
        }
    }
}

// On the world map the character P1 plays as is remembered for the save file, whether picked on the
// character screen or the map's own Add/Drop Players; the screen starts on it next time
// (StartCardsOnSavedCharacter).
void RememberCharacter(uint16_t scene) {
    const int file = g_activeFile.load(std::memory_order_relaxed);
    if (file < 0 || scene != kSceneWorldMap) {
        return;
    }
    uint32_t type = 0;
    if (Memory::TryRead32(kPlayerType, type) && type <= 3 && type != RuntimeConfigFile::FileCharacter(file)) {
        RuntimeConfigFile::SetFileCharacter(file, type);
    }
}

// mkwcat's P1 character code's writes into game data (the rest of it is in the translated code, from
// projects/nsmbw/tools/p1_character.py): the character sounds at each slot's number, and the
// character screen's icon table, once its module is in. Each checks for the disc's own value first.
void PatchCharacterData() {
    struct Word {
        uint32_t address, original, value;
    };
    static constexpr Word kSounds[] = {
        {0x802F5FC8u, 0x32B, 0x347}, {0x802F60D0u, 0x36D, 0x36E}, {0x802F60D8u, 0x36F, 0x36E},
        {0x802F61ECu, 0x3B4, 0x3B5}, {0x802F61F4u, 0x3B6, 0x3B5}, {0x802F6308u, 0x3FA, 0x3FB},
        {0x802F6310u, 0x3FC, 0x3FB},
    };
    static bool s_sounds = false, s_icons = false;
    uint32_t word = 0;
    if (!s_sounds && Memory::TryRead32(kSounds[0].address, word) && word == kSounds[0].original) {
        for (const Word& sound : kSounds) {
            if (Memory::TryRead32(sound.address, word) && word == sound.original) {
                Memory::Write32(sound.address, sound.value);
            }
        }
        s_sounds = true;
    }
    constexpr uint32_t kIcons = 0x80933CF0u;
    if (!s_icons && Memory::TryRead32(kIcons, word) && word == 0x00000006u && Memory::Read8(kIcons + 4) == 0) {
        Memory::Write32(kIcons, 0x06000102u);
        Memory::Write8(kIcons + 4, 7);
        s_icons = true;
    }
}

// The character screen's "Add Wii Remote" and its row of remotes (characterChange's N_textBase_00),
// found when a card first shows (CardPortrait) and kept hidden until the scene ends.
uint32_t g_remotesPanel = 0;
uint16_t g_remotesPanelScene = 0;

void HideRemotesPanel(uint16_t scene) {
    if (g_remotesPanel == 0) {
        return;
    }
    if (scene != g_remotesPanelScene) {
        g_remotesPanel = 0;
        return;
    }
    const uint8_t flags = Memory::Read8(g_remotesPanel + kPaneFlags);
    if ((flags & 1) != 0) {
        Memory::Write8(g_remotesPanel + kPaneFlags, flags & ~1u);
    }
}

void SingularPlayerCount(uint16_t scene);

} // namespace

void OnRetrace() {
    PatchCharacterData();
    if (DetClock::Enabled()) {
        return;
    }
    uint32_t sceneWord = 0;
    if (!Memory::TryRead32(kScene, sceneWord)) {
        return;
    }
    const auto scene = static_cast<uint16_t>(sceneWord & 0xFFFFu);
    HoldTimer(scene);
    HideTimer(scene);
    HoldLives(scene);
    StartCardsOnSavedCharacter(scene);
    RememberCharacter(scene);
    HideRemotesPanel(scene);
    SingularPlayerCount(scene);
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
uint32_t CallGame(uint32_t target, uint32_t r3, uint32_t r4, uint32_t r5 = 0) {
    GuestInterruptCallbackContext call;
    CpuContext* cpu = call.get();
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
    cpu->gpr[5] = r5;
    InvokeIndirectCpu(target, cpu);
    return cpu->gpr[3];
}

// The Power-Up list: the game's own power-up numbers, then the Star (not one of them) and, on Newer,
// its Hammer Suit (its number 7). The same place in both, so a saved pick means the same thing.
constexpr int kChoiceStar = 7, kChoiceHammer = 8, kHammerSuit = 7;

// True once the pending power-up is P1's.
bool GiveInCourse(int powerUp) {
    if (GameMenus::HasProfile(kProfileOpeningTitle)) {
        return false;  // the title screen's course: wait for the game
    }
    const uint32_t player = CallGame(kGetPlayer, 0, 0);
    if (!InGuestRam(player)) {
        return false;  // not in the course yet
    }
    if (powerUp == kChoiceStar) {
        // As the Star item does: setStar(0, its frames), through the player's vtable.
        uint32_t vtable = 0, setStar = 0;
        if (!Memory::TryRead32(player + kPlayerVtable, vtable) || !InGuestRam(vtable) ||
            !Memory::TryRead32(vtable + kPlayerSetStar, setStar) || !InGuestRam(setStar)) {
            return false;
        }
        CallGame(setStar, player, 0, kStarFrames);
        return true;
    }
    CallGame(kChangePowerUp, player, static_cast<uint32_t>(powerUp == kChoiceHammer ? kHammerSuit : powerUp));
    return true;
}

bool MapIsTheGames() {
    static constexpr std::array<uint32_t, 4> kOriginal{0, 1, 3, 2};
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t model = 0;
        if (!Memory::TryRead32(kMapModels + 4 * i, model) || model != kOriginal[i]) {
            return false;  // a mod's own map is left alone
        }
    }
    return true;
}

// As the map's Items screen does with a power-up item: daPyMng_c::mPlayerMode, then the figure's
// own power-up (which it takes into the next course) and model, through its change function.
bool GiveOnMap(int choice) {
    uint32_t type = 0;
    if (!Memory::TryRead32(kPlayerType, type) || type > 3) {
        return false;
    }
    const int powerUp = choice == kChoiceHammer ? kHammerSuit : choice;
    if (choice == kChoiceStar) {
        // The map's own Star item: the next course starts with one, and the figure sparkles.
        uint32_t items = 0;
        Memory::TryRead32(kCreateItem + 4 * type, items);
        Memory::Write32(kCreateItem + 4 * type, items | 1u);
        RT_LOGF(RT_TAG_RUNTIME, "gameplay: P1 (character %u) gets a Star on the world map\n", type);
        return true;
    }
    Memory::Write32(kPlayerMode + 4 * type, static_cast<uint32_t>(powerUp));
    // Going into a course takes each figure's power-up back into mPlayerMode, so the figure has to
    // have it as well.
    uint32_t map = 0, figure = 0, models = 0;
    const bool ours = !IsNewer() && MapIsTheGames() && Memory::TryRead32(kWmFigures, map) && InGuestRam(map) &&
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
    return IsNewer() ? kChoiceHammer + 1 : kChoiceStar + 1;
}

const char* PowerUpName(int powerUp) {
    static constexpr const char* kNames[] = {"Small", "Super Mushroom", "Fire Flower", "Mini Mushroom",
                                             "Propeller Mushroom", "Penguin Suit", "Ice Flower", "Star",
                                             "Hammer Suit"};
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

namespace {

// The character screen's cards have portraits of Luigi and the Toads and none of Mario, who could
// never be picked here on the original game. The game's own one of him is on the disc, for the Coin
// Battle results (128x128, RGB5A3, the cards' format): read from there, centred on a canvas the size
// of the others' (150x150) and, when a card shows Mario, put in the place of its Luigi picture's
// texture. 7 and 8 are what mkwcat's code shows for Mario, free and taken.
constexpr uint16_t kProfileCharacterScreen = 0x2B6;
constexpr uint32_t kPaneMaterial = 0x28, kPaneName = 0xBC, kPaneChildren = 0x14;
constexpr uint32_t kMaterialTexMaps = 0x40;  // nw4r::lyt::Material's TexMap array; a TexMap starts with its image
constexpr uint32_t kCardPictures = 0x254, kCardPictureCount = 8;
constexpr uint32_t kNewArray = 0x802B9260u;  // operator new[](size, alignment), on the current heap
constexpr int kPortraitSide = 150, kPortraitTiles = 38;  // 150 pixels, in the format's 4x4 tiles
constexpr int kMarioSide = 128, kMarioX = 11, kMarioY = 14;

uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
    return at + 4 <= d.size() ? static_cast<uint32_t>(d[at]) << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3] : 0;
}

// The canvas's tiled pixels, empty when the disc has no such file (a mod's).
std::vector<uint8_t> LoadMarioPortrait() {
    std::vector<uint8_t> canvas;
    std::ifstream in(RuntimeConfigFile::ResolvedDvdRoot() / "files" / "Layout" / "pointResult" / "pointResult.arc",
                     std::ios::binary);
    const std::vector<uint8_t> arc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (Be32(arc, 0) != 0x55AA382Du) {
        return canvas;
    }
    const uint32_t nodes = Be32(arc, 4), count = Be32(arc, nodes + 8), strings = nodes + count * 12;
    static constexpr char kName[] = "im_mario_00.tpl";
    for (uint32_t i = 1; i < count; ++i) {
        const uint32_t node = nodes + i * 12, name = strings + (Be32(arc, node) & 0xFFFFFFu);
        if (name + sizeof(kName) > arc.size() || std::memcmp(arc.data() + name, kName, sizeof(kName)) != 0) {
            continue;
        }
        const uint32_t tpl = Be32(arc, node + 4);
        const uint32_t header = tpl + Be32(arc, tpl + Be32(arc, tpl + 8));
        const uint32_t size = Be32(arc, header), format = Be32(arc, header + 4), data = tpl + Be32(arc, header + 8);
        constexpr size_t kPixels = kMarioSide * kMarioSide * 2;
        if (size != (kMarioSide << 16 | kMarioSide) || format != 5 || data + kPixels > arc.size()) {
            return canvas;
        }
        canvas.assign(static_cast<size_t>(kPortraitTiles) * kPortraitTiles * 32, 0);
        for (int y = 0; y < kMarioSide; ++y) {
            for (int x = 0; x < kMarioSide; ++x) {
                const size_t from = data + (static_cast<size_t>(y / 4) * (kMarioSide / 4) + x / 4) * 32 + (y % 4 * 4 + x % 4) * 2;
                const int dx = x + kMarioX, dy = y + kMarioY;
                const size_t to = (static_cast<size_t>(dy / 4) * kPortraitTiles + dx / 4) * 32 + (dy % 4 * 4 + dx % 4) * 2;
                canvas[to] = arc[from];
                canvas[to + 1] = arc[from + 1];
            }
        }
        return canvas;
    }
    return canvas;
}

bool PaneNamed(uint32_t pane, const char* name) {
    for (uint32_t i = 0;; ++i) {
        const auto ch = static_cast<char>(Memory::Read8(pane + kPaneName + i));
        if (ch != name[i]) {
            return false;
        }
        if (ch == 0) {
            return true;
        }
    }
}

uint32_t FindPane(uint32_t pane, const char* name, int depth = 0) {
    if (PaneNamed(pane, name)) {
        return pane;
    }
    const uint32_t head = pane + kPaneChildren;
    uint32_t node = 0;
    Memory::TryRead32(head, node);
    for (int guard = 0; depth < 12 && node != 0 && node != head && guard < 256; ++guard) {
        if (const uint32_t found = FindPane(node - 4, name, depth + 1)) {
            return found;
        }
        if (!Memory::TryRead32(node, node)) {
            break;
        }
    }
    return 0;
}

// The character screen's heading T_titleNinzu_00 is message 2/0x1C, "<count> Players", its count
// dInfo_c's number of players (TagProcessor_c::getPlayNumber). The game only opened the screen for 2
// to 4 players; with P1 alone it reads "1 Players". The disc has no singular wording (its player
// count screen shows only the digits), so for 1 the plural word at the end is swapped for its
// singular, in the text box's own string (mTextBuf, mTextLen), after the game has set it. Only that
// text box changes, and not in lockstep (LAN) play, where OnRetrace stops before this. Text with no
// word from this table (another language, a mod's) is left as it is.
void SingularPlayerCount(uint16_t scene) {
    constexpr uint32_t kInfoInstance = 0x80429F7Cu;  // dInfo_c::m_instance
    constexpr uint32_t kInfoPlayerCount = 0x3CC;     // what the message's count tag shows
    constexpr uint32_t kScreenTitle = 0x608;         // the screen's T_titleNinzu_00 (0x807A10D8)
    constexpr uint32_t kTextBuf = 0xD8, kTextLen = 0xFE;  // nw4r::lyt::TextBox
    struct Word {
        std::u16string_view plural, singular;
    };
    // The plural as the discs' languages write it (USA and PAL), and the Italian language pack's.
    static constexpr Word kWords[] = {
        {u"Players", u"Player"},     {u"joueurs", u"joueur"},     {u"Jugadores", u"Jugador"},
        {u"jugadores", u"jugador"},  {u"giocatori", u"giocatore"},
    };
    uint32_t info = 0, count = 0, screen = 0, title = 0, buffer = 0;
    if (scene != kSceneGameSetup || !Memory::TryRead32(kInfoInstance, info) || !InGuestRam(info) ||
        !Memory::TryRead32(info + kInfoPlayerCount, count) || count != 1 ||
        (screen = GameMenus::ProfileObject(kProfileCharacterScreen)) == 0 ||
        !Memory::TryRead32(screen + kScreenTitle, title) || !InGuestRam(title) || !PaneNamed(title, "T_titleNinzu_00") ||
        !Memory::TryRead32(title + kTextBuf, buffer) || !InGuestRam(buffer)) {
        return;
    }
    const uint32_t length = Memory::Read16(title + kTextLen);
    for (const Word& word : kWords) {
        const auto size = static_cast<uint32_t>(word.plural.size());
        if (length < size) {
            continue;
        }
        const uint32_t start = buffer + 2 * (length - size);
        bool match = true;
        for (uint32_t i = 0; i < size && match; ++i) {
            match = Memory::Read16(start + 2 * i) == word.plural[i];
        }
        if (!match) {
            continue;
        }
        for (uint32_t i = 0; i < word.singular.size(); ++i) {
            Memory::Write16(start + 2 * i, word.singular[i]);
        }
        const auto shown = static_cast<uint32_t>(length - size + word.singular.size());
        Memory::Write16(buffer + 2 * shown, 0);
        Memory::Write16(title + kTextLen, static_cast<uint16_t>(shown));
        return;
    }
}

void CardPortrait(CpuContext* ctx) {
    const uint32_t card = ctx->gpr[3], which = ctx->gpr[4], side = ctx->gpr[6];
    const uint32_t screen = GameMenus::ProfileObject(kProfileCharacterScreen);
    static uint32_t s_screenId = 0, s_image = 0;
    struct Saved {
        uint32_t material, luigi;
    };
    static std::array<Saved, 8> s_saved{};
    static const std::vector<uint8_t> s_portrait = LoadMarioPortrait();
    uint32_t screenId = 0;
    if (screen != 0 && Memory::TryRead32(screen, screenId) && screenId != s_screenId) {
        // A new screen: what was allocated went with the old one's heap, and the panes are new too.
        s_screenId = screenId;
        s_image = 0;
        s_saved = {};
        for (uint32_t off = 0; off < 0x800; off += 4) {
            uint32_t root = 0;
            if (Memory::TryRead32(screen + off, root) && InGuestRam(root) && PaneNamed(root, "RootPane")) {
                uint32_t sceneWord = 0;
                g_remotesPanel = FindPane(root, "N_textBase_00");
                Memory::TryRead32(kScene, sceneWord);
                g_remotesPanelScene = static_cast<uint16_t>(sceneWord & 0xFFFFu);
                break;
            }
        }
    }
    char name[] = "P_lui_00";
    name[7] = static_cast<char>('0' + (side & 1));
    uint32_t picture = 0;
    for (uint32_t i = 0; i < kCardPictureCount && picture == 0; ++i) {
        uint32_t pane = 0;
        if (Memory::TryRead32(card + kCardPictures + 4 * i, pane) && InGuestRam(pane) && PaneNamed(pane, name)) {
            picture = pane;
        }
    }
    uint32_t material = 0, texMaps = 0, image = 0;
    if (picture == 0 || !Memory::TryRead32(picture + kPaneMaterial, material) || !InGuestRam(material) ||
        !Memory::TryRead32(material + kMaterialTexMaps, texMaps) || !InGuestRam(texMaps) ||
        !Memory::TryRead32(texMaps, image)) {
        return;
    }
    Saved* saved = nullptr;
    for (Saved& entry : s_saved) {
        if (entry.material == material || (saved == nullptr && entry.material == 0)) {
            saved = &entry;
            if (entry.material == material) {
                break;
            }
        }
    }
    if (which != 7 && which != 8) {
        if (s_image != 0 && image == s_image && saved != nullptr && saved->material == material) {
            Memory::Write32(texMaps, saved->luigi);  // Luigi's picture is Luigi's again
        }
        return;
    }
    if (s_portrait.empty()) {
        return;
    }
    if (s_image == 0) {
        s_image = CallGame(kNewArray, static_cast<uint32_t>(s_portrait.size()), 0x20);
        if (!InGuestRam(s_image)) {
            s_image = 0;
            return;
        }
        for (size_t i = 0; i < s_portrait.size(); ++i) {
            Memory::Write8(s_image + static_cast<uint32_t>(i), s_portrait[i]);
        }
    }
    if (image != s_image && saved != nullptr) {
        *saved = {material, image};
        Memory::Write32(texMaps, s_image);
    }
    const uint8_t flags = Memory::Read8(picture + kPaneFlags);
    Memory::Write8(picture + kPaneFlags, flags | 1u);
}

} // namespace

} // namespace GameplayOptions

namespace {

// projects/nsmbw/tools/p1_character.py calls this from each card, before it shows its character.
void CardPortraitHook(CpuContext* ctx) {
    GameplayOptions::CardPortrait(ctx);
}

} // namespace

PPC_NATIVE_OVERRIDE_VOID(80B8E340, CardPortraitHook, (CpuContext* ctx), (ctx));
