#include "netplay/game_menus.h"

#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace GameMenus {
namespace {

// fManager_c::m_connectManage (a cTreeMg_c): the root of every object's connect node. A node is
// {parent, child, prev, next, owner}; the owner is an fBase_c whose profile name is at +8.
constexpr uint32_t kTreeRoot = 0x80377A10u;
constexpr uint16_t kProfileSelectPlayer = 0x2BC;
constexpr uint16_t kProfileFileSelect = 0x2D5;
constexpr uint16_t kProfileCourseSelectMenu = 0x2B5;  // the world map's + menu
constexpr uint16_t kProfilePauseWindow = 0x2C6;       // a level's pause window
constexpr uint16_t kProfileCollectionCoin = 0x2B4;    // the world map's Star Coins screen
constexpr uint16_t kProfileYesNoWindow = 0x2C0;        // the game's yes/no prompts
constexpr uint16_t kProfileWorldSelect = 0x2B1;        // the world map's Select a World screen (-)

// Current state IDs (sStateMgr's current pointer) and the fields read from each object.
constexpr uint32_t kSelectPlayerState = 0x9C;
constexpr uint32_t kSelectPlayerCursor = 0x26C;
constexpr uint32_t kFileSelectState = 0x238;
constexpr uint32_t kFileSelectCursor = 0x2A4;

// dSelectPlayer_c::StateID_* (d_basesNP .bss, fixed by the pre-linked layout).
constexpr uint32_t kSpStartWait = 0x809953C0u;
constexpr uint32_t kSpOnStage = 0x80995400u;
constexpr uint32_t kSpInfoOnStage = 0x80995440u;
constexpr uint32_t kSpButtonChange = 0x80995480u;
constexpr uint32_t kSpSelect = 0x809954C0u;
constexpr uint32_t kSpMultiSelect = 0x80995500u;
constexpr uint32_t kSpButtonAnime = 0x80995540u;
constexpr uint32_t kSpExit = 0x80995580u;
// dCourseSelectMenu_c's and Pausewindow_c's current StateID (it names itself at +4).
constexpr uint32_t kPauseMenuState = 0x234;
constexpr uint32_t kCollectionCoinState = 0x434;
constexpr uint32_t kYesNoState = 0x234;
// dWorldSelect_c: its StateID, the world picked (0-8) and a byte per world, 1 once it can be picked.
constexpr uint32_t kWorldSelectState = 0x238;
constexpr uint32_t kWorldSelectWorld = 0x27C;
constexpr uint32_t kWorldSelectOpen = 0x2A8;
// dFileSelect_c::StateID_SelectWait.
constexpr uint32_t kFsSelectWait = 0x80992050u;

uint32_t Find(uint16_t profile) {
    uint32_t node = 0;
    if (!Memory::TryRead32(kTreeRoot, node)) {
        return 0;
    }
    for (int guard = 0; node != 0 && guard < 4096; ++guard) {
        uint32_t owner = 0;
        uint32_t word = 0;
        if (Memory::TryRead32(node + 0x10, owner) && owner != 0 && Memory::TryRead32(owner + 8, word) &&
            static_cast<uint16_t>(word >> 16) == profile) {
            return owner;
        }
        uint32_t child = 0;
        Memory::TryRead32(node + 4, child);
        if (child != 0) {
            node = child;
            continue;
        }
        while (node != 0) {
            uint32_t next = 0;
            Memory::TryRead32(node + 0xC, next);
            if (next != 0) {
                node = next;
                break;
            }
            uint32_t parent = 0;
            Memory::TryRead32(node, parent);
            node = parent;
        }
    }
    return 0;
}

} // namespace

bool HasProfile(uint16_t profile) {
    return Find(profile) != 0;
}

bool PlayerInSpinnableAir(int player, float* verticalSpeed, bool* onSpringboard) {
    // dBaseActor_c's last frame's movement (position minus the one before, set in postExecute).
    constexpr uint32_t kActorMoveY = 0xC8u;
    // daPyMng_c::m_playerID[player]: the player's fBaseID, also the first word of its object.
    constexpr uint32_t kPlayerIds = 0x80354E10u;
    // The player's sStateMgr is at +0x1464; its method's current StateID (method +0x18, +0x1C).
    constexpr uint32_t kPlayerStateId = 0x1464u + 0x18u + 0x1Cu;
    // dAcPy_c's and daPlBase_c's StateID_Jump, _Fall, _SitJump, plus WallJump (kicked off a wall)
    // and Propel (the propeller suit's flight). Not JumpDai (on a springboard): 2 held there bounces
    // him higher, and a spin would throw that away.
    static constexpr uint32_t kSpinnable[] = {
        0x80375C6Cu, 0x80375CACu, 0x80375D6Cu, 0x803761B0u, 0x80376230u,
        0x8035429Cu, 0x8035431Cu, 0x803542DCu,
    };
    constexpr uint32_t kStateJumpDai = 0x803544DCu;  // daPlBase_c::StateID_JumpDai
    if (onSpringboard != nullptr) {
        *onSpringboard = false;
    }
    uint32_t id = 0;
    if (player < 0 || player > 3 || !Memory::TryRead32(kPlayerIds + 4u * static_cast<uint32_t>(player), id) ||
        id == 0) {
        return false;
    }
    uint32_t node = 0;
    if (!Memory::TryRead32(kTreeRoot, node)) {
        return false;
    }
    for (int guard = 0; node != 0 && guard < 4096; ++guard) {
        uint32_t owner = 0, ownerId = 0;
        if (Memory::TryRead32(node + 0x10, owner) && owner != 0 && Memory::TryRead32(owner, ownerId) &&
            ownerId == id) {
            uint32_t state = 0;
            if (!Memory::TryRead32(owner + kPlayerStateId, state)) {
                return false;
            }
            const bool spinnable = std::find(std::begin(kSpinnable), std::end(kSpinnable), state) != std::end(kSpinnable);
            if (onSpringboard != nullptr) {
                *onSpringboard = state == kStateJumpDai;
            }
            if (verticalSpeed != nullptr) {
                *verticalSpeed = Memory::ReadFloat32(owner + kActorMoveY);
            }
            static const bool log = std::getenv("NSMBW_SMART_SPIN_LOG") != nullptr;
            if (log) {
                RT_LOGF(RT_TAG_RUNTIME, "smart spin: player %d state %08x moving %+.2f/frame -> %s\n", player, state,
                        Memory::ReadFloat32(owner + kActorMoveY), spinnable ? "in the air" : "not in the air");
            }
            return spinnable;
        }
        uint32_t child = 0;
        Memory::TryRead32(node + 4, child);
        if (child != 0) {
            node = child;
            continue;
        }
        while (node != 0) {
            uint32_t next = 0;
            Memory::TryRead32(node + 0xC, next);
            if (next != 0) {
                node = next;
                break;
            }
            uint32_t parent = 0;
            Memory::TryRead32(node, parent);
            node = parent;
        }
    }
    return false;
}

std::vector<uint16_t> Profiles() {
    std::vector<uint16_t> profiles;
    uint32_t node = 0;
    if (!Memory::TryRead32(kTreeRoot, node)) {
        return profiles;
    }
    for (int guard = 0; node != 0 && guard < 4096; ++guard) {
        uint32_t owner = 0;
        uint32_t word = 0;
        if (Memory::TryRead32(node + 0x10, owner) && owner != 0 && Memory::TryRead32(owner + 8, word)) {
            profiles.push_back(static_cast<uint16_t>(word >> 16));
        }
        uint32_t child = 0;
        Memory::TryRead32(node + 4, child);
        if (child != 0) {
            node = child;
            continue;
        }
        while (node != 0) {
            uint32_t next = 0;
            Memory::TryRead32(node + 0xC, next);
            if (next != 0) {
                node = next;
                break;
            }
            uint32_t parent = 0;
            Memory::TryRead32(node, parent);
            node = parent;
        }
    }
    std::sort(profiles.begin(), profiles.end());
    profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
    return profiles;
}

SelectPlayers SelectPlayersState() {
    const uint32_t object = Find(kProfileSelectPlayer);
    uint32_t state = 0;
    if (object == 0 || !Memory::TryRead32(object + kSelectPlayerState, state)) {
        return SelectPlayers::Hidden;
    }
    switch (state) {
    case kSpOnStage:
    case kSpInfoOnStage:
        return SelectPlayers::Appearing;
    case kSpSelect:
    case kSpButtonChange:
        return SelectPlayers::Choosing;
    case kSpButtonAnime:
    case kSpExit:
        return SelectPlayers::Leaving;
    case kSpMultiSelect:
        return SelectPlayers::Other;
    case kSpStartWait:
    default:
        return SelectPlayers::Hidden;
    }
}

bool SelectPlayersSettled() {
    const uint32_t object = Find(kProfileSelectPlayer);
    uint32_t state = 0;
    return object != 0 && Memory::TryRead32(object + kSelectPlayerState, state) && state == kSpSelect;
}

int SelectPlayersCursor() {
    const uint32_t object = Find(kProfileSelectPlayer);
    uint32_t cursor = 0;
    if (object == 0 || !Memory::TryRead32(object + kSelectPlayerCursor, cursor) || cursor > 3) {
        return 0;
    }
    return static_cast<int>(cursor);
}

bool FileSelectWaiting() {
    const uint32_t object = Find(kProfileFileSelect);
    uint32_t state = 0;
    return object != 0 && Memory::TryRead32(object + kFileSelectState, state) && state == kFsSelectWait;
}

int FileSelectCursor() {
    const uint32_t object = Find(kProfileFileSelect);
    uint32_t cursor = 0;
    if (object == 0 || !Memory::TryRead32(object + kFileSelectCursor, cursor) || cursor > 2) {
        return 0;
    }
    return static_cast<int>(cursor);
}

int FileSelectRawCursor() {
    const uint32_t object = Find(kProfileFileSelect);
    uint32_t cursor = 0;
    if (object == 0 || !Memory::TryRead32(object + kFileSelectCursor, cursor)) {
        return -1;
    }
    return static_cast<int>(cursor);
}

// NSMBW_DUMP_STATES=2B5,2C6: every state machine field (a pointer to a StateID, which names
// itself through the string pointer at +4) in those objects, logged when it changes (finding
// which field says a menu is open).
std::string DumpStates() {
    static const std::vector<uint16_t> wanted = [] {
        std::vector<uint16_t> list;
        if (const char* value = std::getenv("NSMBW_DUMP_STATES")) {
            for (const char* c = value; *c != 0;) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(c, &end, 16);
                if (end == c) break;
                list.push_back(static_cast<uint16_t>(v));
                c = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    std::string text;
    for (const uint16_t profile : wanted) {
        const uint32_t object = Find(profile);
        char head[32];
        std::snprintf(head, sizeof(head), "[%03X %08X]", profile, object);
        text += head;
        for (uint32_t off = 0; object != 0 && off < 0x800; off += 4) {
            uint32_t id = 0, namePtr = 0;
            if (!Memory::TryRead32(object + off, id) || id < 0x80000000u || id >= 0x81800000u ||
                !Memory::TryRead32(id + 4, namePtr) || namePtr < 0x80000000u || namePtr >= 0x81800000u) {
                continue;
            }
            char name[96] = {};
            int n = 0;
            for (; n < 95; ++n) {
                uint32_t word = 0;
                if (!Memory::TryRead32((namePtr + n) & ~3u, word)) break;
                const char ch = static_cast<char>(word >> (24 - 8 * ((namePtr + n) & 3u)));
                if (ch == 0) break;
                if (ch < 32 || ch > 126) { n = 0; break; }
                name[n] = ch;
            }
            name[n] = 0;
            if (n > 8 && std::strstr(name, "StateID_") != nullptr) {
                char item[140];
                std::snprintf(item, sizeof(item), " +%X=%s", off, name);
                text += item;
            }
        }
    }
    return text;
}

// NSMBW_DUMP_OBJ=<profile>: log the words of that object's first 0x800 bytes that change (finding
// fields such as a menu's cursor).
void DumpObjectChanges() {
    static const int profile = [] {
        const char* value = std::getenv("NSMBW_DUMP_OBJ");
        return value != nullptr ? static_cast<int>(std::strtol(value, nullptr, 16)) : -1;
    }();
    if (profile < 0) {
        return;
    }
    static uint32_t lastObject = 0;
    static std::vector<uint32_t> last;
    const uint32_t object = Find(static_cast<uint16_t>(profile));
    if (object == 0) {
        lastObject = 0;
        return;
    }
    std::vector<uint32_t> now(0x200);
    for (uint32_t i = 0; i < now.size(); ++i) {
        Memory::TryRead32(object + i * 4, now[i]);
    }
    if (object == lastObject) {
        std::string text;
        for (uint32_t i = 0; i < now.size(); ++i) {
            if (now[i] != last[i] && now[i] < 0x10000 && last[i] < 0x10000) {
                char item[40];
                std::snprintf(item, sizeof(item), " +%X:%X->%X", i * 4, last[i], now[i]);
                text += item;
            }
        }
        if (!text.empty()) {
            std::fprintf(stderr, "[obj] %s\n", text.c_str());
        }
    }
    lastObject = object;
    last = std::move(now);
}

int PauseMenuKind() {
    DumpObjectChanges();
    // The state's name, by StateID address (they never move).
    static uint32_t knownOpen[4] = {}, knownClosed[16] = {};
    for (const uint16_t profile : {kProfileCourseSelectMenu, kProfilePauseWindow}) {
        const uint32_t object = Find(profile);
        uint32_t id = 0;
        if (object == 0 || !Memory::TryRead32(object + kPauseMenuState, id) || id == 0) {
            continue;
        }
        const int kind = profile == kProfileCourseSelectMenu ? 1 : 2;
        if (std::find(std::begin(knownOpen), std::end(knownOpen), id) != std::end(knownOpen)) {
            return kind;
        }
        if (std::find(std::begin(knownClosed), std::end(knownClosed), id) != std::end(knownClosed)) {
            continue;
        }
        uint32_t namePtr = 0;
        std::string name;
        if (Memory::TryRead32(id + 4, namePtr) && namePtr >= 0x80000000u && namePtr < 0x81800000u) {
            for (uint32_t n = 0; n < 96; ++n) {
                uint32_t word = 0;
                if (!Memory::TryRead32((namePtr + n) & ~3u, word)) break;
                const char ch = static_cast<char>(word >> (24 - 8 * ((namePtr + n) & 3u)));
                if (ch == 0) break;
                name += ch;
            }
        }
        // Waiting for a choice, or moving between choices: the menu is up and steady.
        const bool open = name.find("StateID_PauseDisp") != std::string::npos ||
                          name.find("StateID_ButtonChangeAnimeEndWait") != std::string::npos;
        uint32_t* list = open ? knownOpen : knownClosed;
        const size_t size = open ? std::size(knownOpen) : std::size(knownClosed);
        for (size_t i = 0; i < size; ++i) {
            if (list[i] == 0) {
                list[i] = id;
                break;
            }
        }
        if (open) {
            return kind;
        }
    }
    return 0;
}

bool PauseMenuOpen() {
    return PauseMenuKind() != 0;
}

uint32_t CourseSelectMenuObject() {
    return Find(kProfileCourseSelectMenu);
}

uint32_t PauseWindowObject() {
    return Find(kProfilePauseWindow);
}

// A StateID's name ("dCollectionCoin_c::StateID_KeyWait"), through its string pointer at +4.
static std::string StateName(uint32_t id) {
    uint32_t namePtr = 0;
    std::string name;
    if (Memory::TryRead32(id + 4, namePtr) && namePtr >= 0x80000000u && namePtr < 0x81800000u) {
        for (uint32_t n = 0; n < 96; ++n) {
            uint32_t word = 0;
            if (!Memory::TryRead32((namePtr + n) & ~3u, word)) break;
            const char ch = static_cast<char>(word >> (24 - 8 * ((namePtr + n) & 3u)));
            if (ch == 0) break;
            name += ch;
        }
    }
    return name;
}

bool PauseWindowShowing() {
    // The window object lives as long as the course; between pauses it idles in StateID_InitWait
    // (its panes keep their visible flags, the game just doesn't draw the layout then).
    static uint32_t idleState = 0;
    const uint32_t object = Find(kProfilePauseWindow);
    uint32_t id = 0;
    if (object == 0 || !Memory::TryRead32(object + kPauseMenuState, id) || id == 0) {
        return false;
    }
    if (idleState == 0 && StateName(id).find("StateID_InitWait") != std::string::npos) {
        idleState = id;
    }
    return id != idleState;
}

// The object of `profile` is in the state named `state` (its StateID is remembered in `cached` the
// first time it is seen, so later calls only compare pointers).
static bool InState(uint16_t profile, uint32_t stateOffset, const char* state, uint32_t& cached) {
    const uint32_t object = Find(profile);
    uint32_t id = 0;
    if (object == 0 || !Memory::TryRead32(object + stateOffset, id) || id == 0) {
        return false;
    }
    if (cached == 0 && StateName(id).find(state) != std::string::npos) {
        cached = id;
    }
    return id == cached;
}

bool StarCoinsWaiting() {
    // The screen's object lives as long as the world map; StateID_KeyWait is the screen up and
    // waiting for a button (it opens through OpenAnimeEndWait and closes through ExitAnimeEndWait).
    static uint32_t keyWait = 0;
    return InState(kProfileCollectionCoin, kCollectionCoinState, "dCollectionCoin_c::StateID_KeyWait", keyWait);
}

bool YesNoWaiting() {
    static uint32_t selectWait = 0;
    return InState(kProfileYesNoWindow, kYesNoState, "dYesNoWindow_c::StateID_SelectWait", selectWait);
}

// The screen's object lives as long as the world map, in StateID_OnStageWait while the screen is
// closed; StateID_Select is it up and waiting for a button (between turns of the ring).
static uint32_t WorldSelectStateId(const char* name, uint32_t& cached) {
    const uint32_t object = Find(kProfileWorldSelect);
    uint32_t id = 0;
    if (object == 0 || !Memory::TryRead32(object + kWorldSelectState, id) || id == 0) {
        return 0;
    }
    if (cached == 0 && StateName(id) == name) {  // exactly: StateID_SelectExpand shares the prefix
        cached = id;
    }
    return id;
}

bool WorldSelectOpen() {
    static uint32_t closed = 0;
    const uint32_t id = WorldSelectStateId("dWorldSelect_c::StateID_OnStageWait", closed);
    // Before the closed state has been seen (the screen opened first), any other state is open.
    return id != 0 && id != closed && (closed != 0 || StateName(id) != "dWorldSelect_c::StateID_OnStageWait");
}

bool WorldSelectWaiting() {
    static uint32_t select = 0;
    const uint32_t id = WorldSelectStateId("dWorldSelect_c::StateID_Select", select);
    return id != 0 && id == select;
}

int WorldSelectWorld() {
    const uint32_t object = Find(kProfileWorldSelect);
    uint32_t world = 0;
    if (object == 0 || !Memory::TryRead32(object + kWorldSelectWorld, world) || world > 8) {
        return -1;
    }
    return static_cast<int>(world);
}

bool WorldSelectCanPick(int world) {
    const uint32_t object = Find(kProfileWorldSelect);
    return object != 0 && world >= 0 && world <= 8 && Memory::Read8(object + kWorldSelectOpen + world) != 0;
}

int CourseMenuCursor() {
    const uint32_t object = Find(kProfileCourseSelectMenu);
    uint32_t cursor = 0;
    if (object == 0 || !Memory::TryRead32(object + 0x268, cursor) || cursor > 3) {
        return -1;
    }
    return static_cast<int>(cursor);
}

uint32_t SelectCursorObject() {
    return Find(0x2CF);  // SELECT_CURSOR: the corner brackets the game's menus share
}

uint16_t CurrentScene() {
    uint32_t word = 0;
    return Memory::TryRead32(0x80428730u, word) ? static_cast<uint16_t>(word & 0xFFFFu) : 0;
}

} // namespace GameMenus
