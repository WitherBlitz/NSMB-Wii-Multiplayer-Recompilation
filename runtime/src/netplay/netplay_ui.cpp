#include "netplay_ui.h"

#include "netplay/game_layout.h"
#include "netplay/game_menus.h"
#include "netplay/world_map_nav.h"
#include "netplay_lobby.h"
#include "netplay_session.h"
#include "netplay_start.h"
#include "runtime_log.h"

#include "aurora_events.h"
#include "hle_stubs.h"
#include "display_settings.h"
#include "keybinds.h"
#include "settings_overlay.h"
#include "updater.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Defined in hle/vi.cpp: whether an aurora frame is open for drawing.
extern std::atomic_bool g_auroraFrameActive;

namespace NetplayUi {
namespace {

using GameLayout::Utf16;
using Phase = NetplayLobby::Phase;

// WPAD_BUTTON_* bits.
constexpr uint32_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kPlus = 0x0010, kTwo = 0x0100,
                   kOne = 0x0200, kB = 0x0400, kA = 0x0800, kMinus = 0x1000;
// The same D-pad by screen direction, for a remote held sideways (D-pad under the left thumb).
constexpr uint32_t kScreenLeft = kUp, kScreenRight = kDown, kScreenUp = kRight, kScreenDown = kLeft;

// None is Couch, the game's own screens. Every other screen is part of LAN play.
enum class Screen { None, Lan, ChooseFile, Count, Rooms, Room, Message, Settings };

constexpr uint16_t kSceneBoot = 0x000;       // the strap screen ("Hold the Wii Remote sideways")
constexpr uint16_t kSceneStage = 0x005;      // a course, or the title screen's
constexpr uint16_t kSceneWorldMap = 0x003;
constexpr uint16_t kSceneGameSetup = 0x00A;  // the file select and "Select Players"
constexpr uint16_t kProfileOpeningTitle = 0x2BB;  // EVENT_OPENING_TITLE: only on the title screen

enum Event : uint32_t {
    kEvLeft = 1, kEvRight = 2, kEvUp = 4, kEvDown = 8, kEvConfirm = 16, kEvBack = 32,
    kEvTab0 = 64,  // a tap on a settings tab: kEvTab0 << tab
};

struct Ui {
    Screen screen = Screen::None;
    int cursor = 0;
    bool toggleShown = false;    // the LAN / Couch toggle is on the file select right now
    bool toggleFocus = false;    // the cursor is on the top bar (moved up from the files): the remote is ours
    int topItem = 0;             // which of the top bar: 0 the Couch / LAN toggle, 1 the settings gear
    uint32_t prevHold = 0;       // channel 1's buttons at the last read
    uint32_t events = 0;         // menu events since the last frame
    bool swallowConfirm = false; // keep a confirm press from the game until it is released
    uint32_t heldFromMenu = 0;   // buttons held when a menu handed the remote back: hidden till released
    bool plusIsBack = false;     // + pressed on the Star Coins screen or a yes/no prompt: 1 till released
    bool countPicked = false;    // Create Room: the player confirmed a number on the game's screen
    std::string message;
    Screen afterMessage = Screen::Lan;
    int frame = 0;               // frames the current screen has been up
    int countHintFrames = 0;
    bool restarting = false;
    uint32_t injectButtons = 0;  // buttons the game is given for a few reads (a Back press)
    int injectReads = 0;
    bool pauseSettingsShown = false;  // a level's pause window is open: Settings sits in it
    bool menuSettingsShown = false;  // the world map's + menu is open: our Settings button is in it
    bool settingsFromPause = false;
};

Ui g_ui;

// ---------------------------------------------------------------- touch and mouse
// These windows' buttons as last drawn, in layout units: a tap on one picks it as the remote would.
struct Hit {
    float x, y, w, h;
    int cursor;      // -1: leave the cursor as it is
    uint32_t event;  // the menu event a tap makes (Event, below)
};
std::vector<Hit> g_hits;

void AddHit(float x, float y, float w, float h, int cursor, bool back = false) {
    g_hits.push_back(Hit{x, y, w, h, cursor, back ? static_cast<uint32_t>(kEvBack) : static_cast<uint32_t>(kEvConfirm)});
}

void AddHitEvent(float x, float y, float w, float h, int cursor, uint32_t event) {
    g_hits.push_back(Hit{x, y, w, h, cursor, event});
}

std::atomic<uint64_t> g_tap{0};  // a tap for the next frame: bit 63, then x and y as 16-bit fractions
std::atomic<bool> g_tapScreen{false};
std::atomic<bool> g_playing{false};
std::atomic<uint64_t> g_pointer{0};  // Android's finger, packed like g_tap: bit 63 while it is held

// The pointer as Draw sees it this frame, in screen pixels: the finger, else the left mouse button.
struct PointerState {
    bool held = false;
    bool pressed = false;  // held this frame and not the last
    float x = 0, y = 0;
};
PointerState g_pointerNow;

// A tap on the game's own screens is played back as remote presses on channel 1: one press, or
// D-pad steps toward a cursor position (checked against the game's cursor before each) and then
// 2 to pick it.
enum class NavKind { None, Press, File, Players, Cursor, MapWalk, WorldSelect };
struct Nav {
    NavKind kind = NavKind::None;
    int target = 0;           // the cursor position, or for Press the buttons
    uint32_t button = 0;      // what is pressed during this step
    int reads = 0;
    int steps = 0;
    bool confirming = false;
    // Cursor: the point to bring SELECT_CURSOR to, in its layout's units, and how the last step went.
    float px = 0, py = 0;
    float lastCx = 1e9f, lastCy = 1e9f;
    uint32_t lastDir = 0;
    int stuck = 0;
};
Nav g_nav;

bool InGuestRam(uint32_t a);
std::string LivePaneName(uint32_t pane);
void WalkPanes(uint32_t pane, int depth, const std::function<void(uint32_t)>& visit);
struct CursorRect;
CursorRect GameCursorRect();
bool CursorStep(Nav& n, uint32_t& button);


void StartNav(NavKind kind, int target) {
    static const char* const kKinds[] = {"none",        "press",    "file select", "select players",
                                         "menu cursor", "map walk", "world select"};
    RT_LOGF(RT_TAG_RUNTIME, "tap: %s 0x%X\n", kKinds[static_cast<int>(kind)], target);
    g_nav = Nav{};
    g_nav.kind = kind;
    g_nav.target = target;
}

void StartCursorNav(float x, float y) {
    StartNav(NavKind::Cursor, 0);
    // The cursor's layout is drawn with x squeezed by d2d::Multi_c's 608/width factor (the constant
    // display_settings.cpp keeps in step with the canvas): its global matrices carry that factor.
    const float squeeze = Memory::ReadFloat32(0x8042B084u);
    g_nav.px = x * (squeeze > 0.1f && squeeze < 2.0f ? squeeze : 1.0f);
    g_nav.py = y;
}

uint32_t NextNavButton(Nav& n) {
    if (n.kind == NavKind::Cursor) {
        uint32_t button = 0;
        if (!CursorStep(n, button)) {
            n.kind = NavKind::None;
        }
        return button;
    }
    if (n.kind == NavKind::Press) {
        if (n.steps++ == 0) {
            return static_cast<uint32_t>(n.target);
        }
        n.kind = NavKind::None;
        return 0;
    }
    if (n.kind == NavKind::WorldSelect) {
        // Select a World: turn the ring a world at a time (it skips the ones not reached yet) the
        // shorter way round its 8 islands, waiting out each turn, until the tapped one is at the front.
        if (++n.stuck > 150 || !GameMenus::WorldSelectOpen()) {
            n.kind = NavKind::None;
            return 0;
        }
        if (!GameMenus::WorldSelectWaiting()) {
            return 0;
        }
        const int world = GameMenus::WorldSelectWorld();
        if (world < 0 || world > 7 || world == n.target || ++n.steps > 8) {
            n.kind = NavKind::None;
            return 0;
        }
        return (n.target - world + 8) % 8 <= 4 ? kScreenRight : kScreenLeft;
    }
    if (n.confirming || ++n.steps > 16) {
        n.kind = NavKind::None;
        return 0;
    }
    int cursor = 0;
    int row = 0, col = 0, targetRow = 0, targetCol = 0;
    if (n.kind == NavKind::File) {
        if (!GameMenus::FileSelectWaiting()) {
            return 0;  // still moving: wait
        }
        // 0-2 the files in a row, 3 and 4 Free-for-All and Coin Battle below them.
        cursor = GameMenus::FileSelectRawCursor();
        row = cursor <= 2 ? 0 : 1;
        col = cursor <= 2 ? cursor : cursor - 3;
        targetRow = n.target <= 2 ? 0 : 1;
        targetCol = n.target <= 2 ? n.target : n.target - 3;
    } else {
        if (!GameMenus::SelectPlayersSettled()) {
            return 0;
        }
        // 0 "1 Player" on top, 1-3 "2-4 Players" in a row below it.
        cursor = GameMenus::SelectPlayersCursor();
        row = cursor == 0 ? 0 : 1;
        col = cursor == 0 ? 0 : cursor - 1;
        targetRow = n.target == 0 ? 0 : 1;
        targetCol = n.target == 0 ? 0 : n.target - 1;
    }
    RT_LOGF(RT_TAG_RUNTIME, "tap: cursor %d, going to %d\n", cursor, n.target);
    if (cursor == n.target) {
        n.confirming = true;
        return kTwo;
    }
    if (row != targetRow) {
        return targetRow > row ? kScreenDown : kScreenUp;
    }
    return targetCol > col ? kScreenRight : kScreenLeft;
}

// Once per read of channel 1: the buttons the current tap holds now (4 reads down, 4 up per step).
uint32_t NavButtons() {
    Nav& n = g_nav;
    if (n.kind == NavKind::None) {
        return 0;
    }
    if (n.kind == NavKind::MapWalk) {
        bool active = false;
        const uint32_t buttons = WorldMapNav::Buttons(active);
        if (!active) {
            n.kind = NavKind::None;
        }
        return buttons;
    }
    const int phase = n.reads++ % 8;
    if (phase == 0) {
        n.button = NextNavButton(n);
    }
    return phase < 4 ? n.button : 0;
}

struct Assets {
    bool tried = false;
    GameLayout::Layout yesNo;
    GameLayout::Layout cursor;
    std::shared_ptr<GameLayout::Font> message;
    std::shared_ptr<GameLayout::Font> mario;
};

Assets& GetAssets() {
    static Assets assets;
    if (!assets.tried) {
        assets.tried = true;
        assets.yesNo.Load("/Layout/yesnoWindow/yesnoWindow.arc");
        assets.cursor.Load("/Layout/select_cursor/select_cursor.arc");
        assets.message = GameLayout::LoadFont("mj2d00_MessageFont_32_I4.brfnt");
        assets.mario = GameLayout::LoadFont("mj2d01_marioFont_64_IA4.brfnt");
    }
    return assets;
}

// The steps of Create Room that happen on the game's own screens keep the remote with the game.
bool IsModal(Screen screen) {
    return screen != Screen::None && screen != Screen::ChooseFile && screen != Screen::Count;
}

const char* ScreenName(Screen screen) {
    switch (screen) {
    case Screen::None: return "couch";
    case Screen::Lan: return "Join or Create Room";
    case Screen::ChooseFile: return "room save file (game screen)";
    case Screen::Count: return "room size (game screen)";
    case Screen::Rooms: return "rooms";
    case Screen::Room: return "room";
    case Screen::Message: return "message";
    case Screen::Settings: return "settings";
    }
    return "?";
}

void Show(Screen screen, int cursor = 0) {
    if (screen != g_ui.screen) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay ui: %s -> %s\n", ScreenName(g_ui.screen), ScreenName(screen));
    }
    // Handing the remote back to the game: a button still held from the menu must not reach it as a
    // fresh press (the game would take the menu's confirm as its own).
    if (IsModal(g_ui.screen) && !IsModal(screen)) {
        g_ui.heldFromMenu = g_ui.prevHold;
    }
    g_ui.screen = screen;
    g_ui.cursor = cursor;
    g_ui.frame = 0;
    g_ui.events = 0;
}

void ShowMessage(const std::string& text, Screen after) {
    g_ui.message = text;
    g_ui.afterMessage = after;
    Show(Screen::Message);
}

// Menus that own the remote: the game sees it at rest.
bool Modal() {
    return IsModal(g_ui.screen);
}

// ---------------------------------------------------------------- drawing helpers
struct Canvas {
    ImDrawList* list;
    GameLayout::View view;
    float alpha;
};

void Veil(const Canvas& c) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    c.list->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, static_cast<int>(110 * c.alpha)));
}

// The yes/no window, `height` layout units tall (322 as authored), without its texts and buttons.
void Window(const Canvas& c, float height, float width = 528.0f) {
    Assets& a = GetAssets();
    GameLayout::DrawParams p;
    p.alpha = c.alpha;
    p.visible["T_questionS_00"] = false;
    p.visible["N_button_00"] = false;
    p.visible["N_otehonText_00"] = false;
    p.paneSize["P_bg_00"] = {width, height};
    p.paneSize["P_stripe_00"] = {width + 2, height};
    p.paneSize["W_yesnoWindow_00"] = {width + 14, height + 8};
    a.yesNo.Draw(c.list, c.view, "N_yesnoWindow_00", p);
}

// The game's message text: white with its dark edge, centred on (x, y) unless told otherwise.
void Text(const Canvas& c, const std::string& text, float x, float y, float size = 30.0f, int originH = 1,
          uint32_t color = 0xFFFFFFFFu) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.message.get())) {
        return;
    }
    const std::u16string t = Utf16(text);
    GameLayout::DrawText(c.list, c.view, *a.message, t, x + 2.5f, y - 2.5f, size, 0x30200AFFu, 0x30200AFFu,
                         0.55f * c.alpha, originH, 1);
    GameLayout::DrawText(c.list, c.view, *a.message, t, x, y, size, color, color, c.alpha, originH, 1);
}

// A title in the game's Mario font (yellow to orange), like "Select Players".
void Title(const Canvas& c, const std::string& text, float x, float y, float size = 44.0f) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.mario.get())) {
        return;
    }
    GameLayout::DrawText(c.list, c.view, *a.mario, Utf16(text), x, y, size, 0xFFFF00FFu, 0xFF8C00FFu, c.alpha, 1, 1);
}

float Pulse(int frame) {
    return 0.5f + 0.5f * std::sin(frame * 0.12f);
}

// The game's selection cursor (four yellow corners) around a w x h box centred on (x, y).
void Cursor(const Canvas& c, float x, float y, float w, float h, int frame) {
    Assets& a = GetAssets();
    const float out = 3.0f * Pulse(frame);
    struct Corner { const char* pane; float ax, ay, sx, sy; };
    static constexpr Corner kCorners[] = {
        {"N_LU_00", -67, 42, -1, 1}, {"N_RU_00", 67, 42, 1, 1}, {"N_LD_00", -67, -42, -1, -1}, {"N_RD_00", 67, -42, 1, -1}};
    for (const auto& corner : kCorners) {
        GameLayout::DrawParams p;
        p.alpha = c.alpha;
        p.offsetX = x + corner.sx * (w / 2 + out) - corner.ax;
        p.offsetY = y + corner.sy * (h / 2 + out) - corner.ay;
        a.cursor.Draw(c.list, c.view, corner.pane, p);
    }
}

// A green-to-yellow button pill like the game's yes/no buttons, w x 53 units, centred on (x, y).
// fixedScale > 0 draws it at that scale instead of the menus' own (a gentle pulse when selected).
void Button(const Canvas& c, const std::string& label, float x, float y, float w, bool selected, bool enabled,
            int frame, float textSize = 30.0f, float h = 53.0f, float fixedScale = 0.0f,
            const std::array<float, 9>* recolor = nullptr) {
    Assets& a = GetAssets();
    GameLayout::DrawParams p;
    p.alpha = c.alpha * (enabled ? 1.0f : 0.55f);
    p.offsetX = x;
    p.offsetY = y + 118.0f;  // the pane is authored at (0, -118)
    p.scale = fixedScale > 0.0f ? fixedScale : selected ? 1.0f + 0.04f * Pulse(frame) : 0.94f;
    p.paneScale["P_centerBase_00"] = 1.0f;  // authored at 0: it grows in through an animation
    p.paneSize["P_centerBase_00"] = {w, h};
    p.paneSize["W_button_02"] = {w + 5.0f, h + 10.0f};
    p.paneSize["P_shadow_02"] = {w, h};
    p.visible["T_center_00"] = false;
    p.visible["T_center_01"] = false;
    if (!enabled) {
        p.vertexTint["P_centerBase_00"] = 0xB0B0B0FFu;
    }
    if (recolor != nullptr) {
        p.colorMatrix["P_centerBase_00"] = *recolor;
    }
    a.yesNo.Draw(c.list, c.view, "P_centerBase_00", p);
    const float scale = p.scale;
    Canvas t = c;
    t.alpha = p.alpha;
    Text(t, label, x, y, textSize * scale);
    if (selected) {
        Cursor(c, x, y, w * scale + 6, h * scale + 6, frame);
    }
}

// Two side-by-side buttons at the bottom of a window `height` tall.
void ButtonPair(const Canvas& c, const std::string& left, const std::string& right, float height, int cursor,
                bool leftEnabled = true, bool rightEnabled = true) {
    const float y = -height / 2 + 43.0f;
    Button(c, left, -126, y, 232, cursor == 0, leftEnabled, g_ui.frame);
    Button(c, right, 126, y, 232, cursor == 1, rightEnabled, g_ui.frame);
    AddHit(-126, y, 240, 60, 0);
    AddHit(126, y, 240, 60, 1);
}

std::string PlayerLine(uint8_t slot, const std::string& name, bool you) {
    return std::to_string(slot + 1) + "P  " + name + (you ? "  (you)" : "");
}

// ---------------------------------------------------------------- screens
// The LAN / Couch toggle in the empty right half of the file select's title bar: one of the game's
// green buttons, reached by moving up from the top file. Couch is the game as it is; pressing it
// switches to LAN (Join or Create Room), and back to Couch from any LAN step on the file select.
constexpr float kToggleX = 214.0f, kToggleY = 192.0f, kToggleW = 166.0f;
constexpr float kGearY = kToggleY;

constexpr float kGearX = kToggleX + kToggleW / 2 + 46.0f, kGearW = 62.0f;
// The Update button, in the middle of the title bar when a newer release is out (updater.h).
constexpr float kUpdateX = -15.0f, kUpdateW = 140.0f;
// The game's green button turned orange: red takes most of the green, green drops to amber.
constexpr std::array<float, 9> kOrange = {0.15f, 1.0f, 0.0f, 0.25f, 0.6f, 0.0f, 0.0f, 0.15f, 0.35f};

ImVec2 ToScreen(const Canvas& c, float x, float y) {
    return ImVec2(c.view.centerX + x * c.view.scale, c.view.centerY - y * c.view.scale);
}

// A cog: eight teeth around a ring, drawn in the dark of the game's text edges.
void Gear(const Canvas& c, float x, float y, float r) {
    const ImVec2 centre = ToScreen(c, x, y);
    const float s = c.view.scale;
    const ImU32 color = IM_COL32(70, 62, 40, static_cast<int>(235 * c.alpha));
    for (int i = 0; i < 8; ++i) {
        const float a = i * 3.14159265f / 4.0f;
        const float ca = std::cos(a), sa = std::sin(a);
        const float in = r * 0.72f * s, out = r * 1.18f * s, half = r * 0.26f * s;
        c.list->AddQuadFilled(ImVec2(centre.x + ca * in - sa * half, centre.y + sa * in + ca * half),
                              ImVec2(centre.x + ca * out - sa * half, centre.y + sa * out + ca * half),
                              ImVec2(centre.x + ca * out + sa * half, centre.y + sa * out - ca * half),
                              ImVec2(centre.x + ca * in + sa * half, centre.y + sa * in - ca * half), color);
    }
    c.list->AddCircle(centre, r * 0.62f * s, color, 24, r * 0.42f * s);
}

// ---------------------------------------------------------------- the world map's + menu: Settings
// A fourth button, "Settings", under Quick Save in the world map's + menu. The game's own layout
// (corseSelectMenu.arc, its panes live in guest memory) is edited every frame to make room: the
// window grows by kMenuGrow units, the first three buttons move up by kMenuRaise and Title Screen
// (with its separator and dark band) down by kMenuLower, and "Back (1)" drops below the taller
// window. The button itself is drawn here in the game's button style; the controller reaches it
// down from Quick Save or up from Title Screen, and taps or clicks work on it and on Back.
constexpr float kMenuGrow = 65.0f, kMenuRaise = 32.0f, kMenuLower = -33.0f, kMenuBackDrop = -29.0f;
constexpr float kMenuSettingsY = -22.0f + kMenuRaise - 65.0f;  // one button pitch under Quick Save
constexpr float kMenuBackX = 189.0f, kMenuBackY = -183.0f + kMenuBackDrop;

// nw4r::lyt::Pane: mpParent +0xC, child list +0x10 (size, then the terminator node {next, prev}),
// each child linked through its node at +4, mTranslate +0x2C, mSize +0x4C, mFlag +0xBB (bit 0:
// visible), mName +0xBC.
struct LivePane {
    uint32_t addr = 0;
    uint32_t parent = 0;
    float y = 0.0f;
    float h = 0.0f;
};
struct MenuLayout {
    uint32_t object = 0;
    std::map<std::string, LivePane> panes;
    std::map<uint32_t, std::string> names;
};
MenuLayout g_menuLayout;
std::atomic<bool> g_menuFocus{false};  // "Settings" holds the menu's cursor
int g_menuFocusFrom = 2;               // the game's own cursor meanwhile: Quick Save or Title Screen
std::atomic<bool> g_menuOpenSettings{false};

bool InGuestRam(uint32_t a) {
    return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u);
}

std::string LivePaneName(uint32_t pane) {
    std::string name;
    for (uint32_t i = 0; i < 17; ++i) {
        uint32_t word = 0;
        if (!Memory::TryRead32((pane + 0xBC + i) & ~3u, word)) {
            return {};
        }
        const char ch = static_cast<char>(word >> (24 - 8 * ((pane + 0xBC + i) & 3u)));
        if (ch == 0) {
            break;
        }
        name += ch;
    }
    return name;
}

// The root pane of the layout an object embeds: the first pointer in it to a pane named RootPane.
uint32_t FindRootPane(uint32_t object) {
    for (uint32_t off = 0; object != 0 && off < 0x800; off += 4) {
        uint32_t p = 0;
        if (Memory::TryRead32(object + off, p) && InGuestRam(p) && LivePaneName(p) == "RootPane") {
            return p;
        }
    }
    return 0;
}

void WalkPanes(uint32_t pane, int depth, const std::function<void(uint32_t)>& visit) {
    if (depth > 12) {
        return;
    }
    visit(pane);
    const uint32_t head = pane + 0x14;
    uint32_t node = 0;
    Memory::TryRead32(head, node);
    for (int guard = 0; node != 0 && node != head && guard < 256; ++guard) {
        WalkPanes(node - 4, depth + 1, visit);
        uint32_t next = 0;
        if (!Memory::TryRead32(node, next)) {
            break;
        }
        node = next;
    }
}

// NSMBW_DUMP_PANES=1: log every pane of a menu's layout once (name, position, size, parent), to
// find the panes a layout edit moves.
void DumpPanesOnce(uint32_t object, const char* what) {
    static const bool enabled = std::getenv("NSMBW_DUMP_PANES") != nullptr;
    static uint32_t dumped = 0;
    if (!enabled || object == 0 || object == dumped) {
        return;
    }
    const uint32_t root = FindRootPane(object);
    if (root == 0) {
        return;
    }
    dumped = object;
    WalkPanes(root, 0, [&](uint32_t pane) {
        uint32_t parent = 0, flags = 0;
        Memory::TryRead32(pane + 0xC, parent);
        Memory::TryRead32(pane + 0xB8, flags);
        RT_LOGF(RT_TAG_RUNTIME, "%s pane %-22s x %7.1f y %7.1f w %6.1f h %6.1f vis %u parent %s\n", what,
                LivePaneName(pane).c_str(), Memory::ReadFloat32(pane + 0x2C), Memory::ReadFloat32(pane + 0x30),
                Memory::ReadFloat32(pane + 0x4C), Memory::ReadFloat32(pane + 0x50), flags & 1u,
                LivePaneName(parent).c_str());
    });
}

bool AttachMenuLayout() {
    const uint32_t object = GameMenus::CourseSelectMenuObject();
    if (object == 0) {
        g_menuLayout = MenuLayout{};
        return false;
    }
    if (object == g_menuLayout.object) {
        return true;
    }
    const uint32_t root = FindRootPane(object);
    if (root == 0) {
        return false;
    }
    MenuLayout layout;
    layout.object = object;
    WalkPanes(root, 0, [&](uint32_t pane) {
        const std::string name = LivePaneName(pane);
        uint32_t parent = 0;
        Memory::TryRead32(pane + 0xC, parent);
        layout.panes[name] = LivePane{pane, parent, Memory::ReadFloat32(pane + 0x30), Memory::ReadFloat32(pane + 0x50)};
        layout.names[pane] = name;
    });
    if (layout.panes.count("W_corseSelect_01") == 0 || layout.panes.count("P_SBBase_04") == 0) {
        return false;
    }
    RT_LOGF(RT_TAG_RUNTIME, "world map menu: %zu panes, adding Settings\n", layout.panes.size());
    g_menuLayout = std::move(layout);
    return true;
}

// How far (layout units, world space) a pane of the menu moves; panes not listed follow their parent.
bool MenuShift(const std::string& name, float& dy) {
    static const std::map<std::string, float> kShifts = [] {
        std::map<std::string, float> m;
        for (const char* n : {"1", "2", "3"}) {
            m[std::string("P_SBBase_0") + n] = kMenuRaise;
            m[std::string("T_corseSelectS0") + n] = kMenuRaise;
            m[std::string("T_corseSelect_0") + n] = kMenuRaise;
            m[std::string("W_SButton_0") + n] = kMenuRaise;
            m[std::string("P_shadow_0") + n] = kMenuRaise;
        }
        for (const char* n : {"P_SBBase_04", "T_corseSelectS04", "T_corseSelect_05", "W_SButton_04", "P_shadow_04",
                              "P_line_00", "P_winbg_00"}) {
            m[n] = kMenuLower;
        }
        for (const char* n : {"N_back", "P_back", "P_backWhite", "T_back"}) {
            m[n] = kMenuBackDrop;
        }
        return m;
    }();
    const auto it = kShifts.find(name);
    if (it == kShifts.end()) {
        return false;
    }
    dy = it->second;
    return true;
}

// Moves the listed panes of a menu's layout by their world-space shift (a pane under a moved parent
// is compensated, so each ends up where its own shift says) and makes the window panes taller.
using PaneShift = bool (*)(const std::string&, float&);
void ApplyLayoutEdit(const MenuLayout& layout, PaneShift shift, std::initializer_list<const char*> grows,
                     float growBy) {
    const std::function<float(uint32_t)> worldShift = [&](uint32_t pane) -> float {
        const auto name = layout.names.find(pane);
        if (name == layout.names.end()) {
            return 0.0f;
        }
        float dy = 0.0f;
        if (shift(name->second, dy)) {
            return dy;
        }
        return worldShift(layout.panes.at(name->second).parent);
    };
    for (const auto& [name, pane] : layout.panes) {
        float dy = 0.0f;
        if (shift(name, dy)) {
            Memory::WriteFloat32(pane.addr + 0x30, pane.y + dy - worldShift(pane.parent));
        }
    }
    for (const char* grow : grows) {
        const auto it = layout.panes.find(grow);
        if (it != layout.panes.end()) {
            Memory::WriteFloat32(it->second.addr + 0x50, it->second.h + growBy);
        }
    }
}

void ApplyMenuLayout() {
    if (AttachMenuLayout()) {
        ApplyLayoutEdit(g_menuLayout, MenuShift, {"W_corseSelect_01", "P_winbg_01"}, kMenuGrow);
    }
}

// ---------------------------------------------------------------- a level's pause window: Settings
// The same for the pause window (Pausewindow_c, pauseMenu.arc): Settings goes between Continue and
// Exit Course. The window grows by kPauseGrow units, the title and Continue move up by kPauseRaise,
// Exit Course down by kPauseLower, and Settings takes the middle at the buttons' own pitch. When the
// course can't be left yet (Exit Course greyed out, its P_shadowBlack shown), the controller stops on
// Settings, as the game's own cursor stops on Continue.
constexpr float kPauseGrow = 70.0f, kPauseRaise = 35.0f, kPauseLower = -35.0f;
constexpr float kPauseSettingsY = -24.0f;  // window space: one pitch (70) under Continue at 11 + 35
MenuLayout g_pauseLayout;
std::atomic<bool> g_pauseFocus{false};  // "Settings" holds the pause window's cursor
int g_pauseFocusFrom = 0;               // the game's own cursor meanwhile: 0 Continue, 1 Exit Course
std::atomic<bool> g_pauseOpenSettings{false};

bool AttachPauseLayout() {
    const uint32_t object = GameMenus::PauseWindowObject();
    if (object == 0) {
        g_pauseLayout = MenuLayout{};
        return false;
    }
    if (object == g_pauseLayout.object) {
        return true;
    }
    const uint32_t root = FindRootPane(object);
    if (root == 0) {
        return false;
    }
    MenuLayout layout;
    layout.object = object;
    WalkPanes(root, 0, [&](uint32_t pane) {
        const std::string name = LivePaneName(pane);
        uint32_t parent = 0;
        Memory::TryRead32(pane + 0xC, parent);
        layout.panes[name] = LivePane{pane, parent, Memory::ReadFloat32(pane + 0x30), Memory::ReadFloat32(pane + 0x50)};
        layout.names[pane] = name;
    });
    if (layout.panes.count("W_N_pauseMenu_00") == 0 || layout.panes.count("P_SBBase_02") == 0) {
        return false;
    }
    RT_LOGF(RT_TAG_RUNTIME, "pause window: %zu panes, adding Settings\n", layout.panes.size());
    g_pauseLayout = std::move(layout);
    return true;
}

bool PauseShift(const std::string& name, float& dy) {
    static const std::map<std::string, float> kShifts = {
        {"N_worldText_00", kPauseRaise}, {"N_osusumeText_00", kPauseRaise}, {"N_multiText_00", kPauseRaise},
        {"P_SBBase_00", kPauseRaise},    {"N_posMulti_UP", kPauseRaise},
        {"P_SBBase_02", kPauseLower},    {"N_posMulti_Down", kPauseLower},
    };
    const auto it = kShifts.find(name);
    if (it == kShifts.end()) {
        return false;
    }
    dy = it->second;
    return true;
}

// While Settings holds the cursor, the game still thinks its own cursor is on Continue (or Exit
// Course) and keeps that button grown. Settings borrows its scale (+0x44 x, +0x48 y) and gives it
// the other button's, so only one button is grown; it gets its own back when Settings lets go.
float g_pauseOnScale[2] = {0.0f, 0.0f};  // the game's selected-button scale while Settings holds it
uint32_t g_pauseHeldPane = 0;

void HoldPauseButtonScale(bool hold) {
    static constexpr const char* kButtons[2] = {"P_SBBase_00", "P_SBBase_02"};
    if (!hold && g_pauseHeldPane == 0) {
        return;  // the usual case: nothing borrowed, nothing to give back
    }
    const bool live = AttachPauseLayout();
    if (!hold || !live) {
        if (g_pauseHeldPane != 0 && live) {
            Memory::WriteFloat32(g_pauseHeldPane + 0x44, g_pauseOnScale[0]);
            Memory::WriteFloat32(g_pauseHeldPane + 0x48, g_pauseOnScale[1]);
        }
        g_pauseHeldPane = 0;
        g_pauseOnScale[0] = g_pauseOnScale[1] = 0.0f;
        return;
    }
    const auto held = g_pauseLayout.panes.find(kButtons[g_pauseFocusFrom == 1 ? 1 : 0]);
    const auto other = g_pauseLayout.panes.find(kButtons[g_pauseFocusFrom == 1 ? 0 : 1]);
    if (held == g_pauseLayout.panes.end() || other == g_pauseLayout.panes.end()) {
        return;
    }
    if (g_pauseHeldPane != held->second.addr) {
        g_pauseHeldPane = held->second.addr;
        g_pauseOnScale[0] = Memory::ReadFloat32(g_pauseHeldPane + 0x44);
        g_pauseOnScale[1] = Memory::ReadFloat32(g_pauseHeldPane + 0x48);
    }
    Memory::WriteFloat32(g_pauseHeldPane + 0x44, Memory::ReadFloat32(other->second.addr + 0x44));
    Memory::WriteFloat32(g_pauseHeldPane + 0x48, Memory::ReadFloat32(other->second.addr + 0x48));
}

void ApplyPauseLayout() {
    if (AttachPauseLayout()) {
        ApplyLayoutEdit(g_pauseLayout, PauseShift, {"W_N_pauseMenu_00", "P_bg_00"}, kPauseGrow);
    }
}

// Exit Course can't be chosen in a course not yet cleared: the game shades it (P_shadowBlack).
bool PauseExitDisabled() {
    const auto it = g_pauseLayout.panes.find("P_shadowBlack");
    uint32_t flags = 0;
    return it != g_pauseLayout.panes.end() && Memory::TryRead32(it->second.addr + 0xB8, flags) && (flags & 1u) != 0;
}

// The game's own corner brackets would stay on Quick Save or Title Screen while Settings holds the
// cursor: hide them meanwhile (the visible bit of the cursor layout's root pane).
void HideGameCursor(bool hide) {
    // SELECT_CURSOR holds a layout per cursor it can show: hide (or show again) every one of them.
    static std::vector<uint32_t> hidden;
    if (hide && hidden.empty()) {
        const uint32_t object = GameMenus::SelectCursorObject();
        for (uint32_t off = 0; object != 0 && off < 0x1000; off += 4) {
            uint32_t p = 0;
            if (Memory::TryRead32(object + off, p) && InGuestRam(p) &&
                std::find(hidden.begin(), hidden.end(), p) == hidden.end() && LivePaneName(p) == "RootPane") {
                // Only the cursor's own layouts (the ones with the corner brackets): the object also
                // points at the layout of the menu it sits on (the pause window's, for one).
                bool brackets = false;
                WalkPanes(p, 0, [&](uint32_t pane) { brackets = brackets || LivePaneName(pane) == "N_LU_00"; });
                if (brackets) {
                    hidden.push_back(p);
                }
            }
        }
    }
    for (const uint32_t root : hidden) {
        uint32_t word = 0;
        if (Memory::TryRead32(root + 0xB8, word)) {
            Memory::Write32(root + 0xB8, hide ? (word & ~1u) : (word | 1u));  // mFlag bit 0: visible
        }
    }
    if (!hide) {
        hidden.clear();
    }
}

// ---------------------------------------------------------------- clicking the game's menus
// Nearly every menu in the game marks its choice with SELECT_CURSOR's corner brackets. A tap or click
// on such a menu steers that cursor: D-pad presses toward the point until the brackets enclose it,
// then 2. The cursor's place is read from its live layout (the corner panes' global matrices).
struct CursorRect {
    bool valid = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // layout units, y up
};

// One corner pane's position: the translation column of nw4r::lyt::Pane::mGlbMtx (+0x84).
bool PaneGlobalPos(uint32_t pane, float& x, float& y) {
    uint32_t vis = 0;
    if (!Memory::TryRead32(pane + 0xB8, vis)) {
        return false;
    }
    x = Memory::ReadFloat32(pane + 0x84 + 12);
    y = Memory::ReadFloat32(pane + 0x84 + 28);
    return std::isfinite(x) && std::isfinite(y);
}

CursorRect GameCursorRect() {
    CursorRect r;
    const uint32_t object = GameMenus::SelectCursorObject();
    if (object == 0) {
        return r;
    }
    // Each layout the object holds is a cursor; the shown one has its root and corners visible.
    for (uint32_t off = 0; off < 0x1000 && !r.valid; off += 4) {
        uint32_t root = 0, flags = 0;
        if (!Memory::TryRead32(object + off, root) || !InGuestRam(root) || LivePaneName(root) != "RootPane" ||
            !Memory::TryRead32(root + 0xB8, flags) || (flags & 1u) == 0) {
            continue;
        }
        float xs[4], ys[4];
        int found = 0;
        bool shown = true;
        WalkPanes(root, 0, [&](uint32_t pane) {
            const std::string name = LivePaneName(pane);
            static const char* const kCorners[] = {"N_LU_00", "N_RU_00", "N_LD_00", "N_RD_00"};
            for (int i = 0; i < 4; ++i) {
                if (name == kCorners[i] && PaneGlobalPos(pane, xs[i], ys[i])) {
                    ++found;
                }
            }
            uint32_t f = 0;
            if (name == "N_cursor_00" && Memory::TryRead32(pane + 0xB8, f) && ((f & 1u) == 0 || (f >> 24) == 0)) {
                shown = false;  // hidden, or fully transparent
            }
        });
        if (found == 4 && shown) {
            r.valid = true;
            r.x0 = std::min(xs[0], xs[2]);
            r.x1 = std::max(xs[1], xs[3]);
            r.y0 = std::min(ys[2], ys[3]);
            r.y1 = std::max(ys[0], ys[1]);
        }
    }
    return r;
}

// One step toward the tapped point: false once done (confirmed, or the cursor can't get there).
bool CursorStep(Nav& n, uint32_t& button) {
    button = 0;
    if (n.confirming || ++n.steps > 24) {
        return false;
    }
    const CursorRect r = GameCursorRect();
    if (!r.valid) {
        return n.steps <= 3;  // a cursor Settings had hidden shows again a frame later
    }
    const float cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
    const bool first = n.lastCx > 1e8f;
    const bool changed = std::fabs(cx - n.lastCx) > 0.5f || std::fabs(cy - n.lastCy) > 0.5f;
    n.lastCx = cx;
    n.lastCy = cy;
    if (changed && !first) {
        n.stuck = 0;
        n.lastDir = 0;
        return true;  // it moved (or is still sliding): look again once it has settled
    }
    uint32_t blocked = 0;
    if (!first && n.lastDir != 0) {
        ++n.stuck;  // that press didn't move it
        blocked = n.lastDir;
    }
    if (n.steps <= 2) {
        RT_LOGF(RT_TAG_RUNTIME, "tap: cursor at %.0f..%.0f x %.0f..%.0f, tap %.0f,%.0f\n", r.x0, r.x1, r.y0, r.y1,
                n.px, n.py);
    }
    if (n.px >= r.x0 && n.px <= r.x1 && n.py >= r.y0 && n.py <= r.y1) {
        n.confirming = true;
        button = kTwo;
        return true;
    }
    if (n.stuck >= 2) {
        return false;  // nothing there to pick
    }
    const float w = std::max(r.x1 - r.x0, 1.0f), h = std::max(r.y1 - r.y0, 1.0f);
    const float dx = (n.px - cx) / w, dy = (n.py - cy) / h;
    // Along the axis the point is farthest on, unless that last went nowhere.
    const bool vertical = blocked != 0 ? !(blocked == kScreenUp || blocked == kScreenDown)
                                       : std::fabs(dy) >= std::fabs(dx);
    if (vertical) {
        button = dy > 0 ? kScreenUp : kScreenDown;
    } else {
        button = dx > 0 ? kScreenRight : kScreenLeft;
    }
    n.lastDir = button;
    return true;
}

// The menu window as the game draws it this frame: centre, scale and opacity from the window pane's
// global matrix (+0x84) and global alpha (+0xB9), so Settings opens, closes and fades with it.
bool WindowTransform(const MenuLayout& layout, uint32_t liveObject, const char* windowPane, float& cx, float& cy,
                     float& scale, float& alpha) {
    if (layout.object == 0 || layout.object != liveObject) {
        return false;
    }
    const auto it = layout.panes.find(windowPane);
    if (it == layout.panes.end()) {
        return false;
    }
    const uint32_t pane = it->second.addr;
    uint32_t flags = 0;
    if (!Memory::TryRead32(pane + 0xB8, flags) || (flags & 1u) == 0) {
        return false;
    }
    // Hidden if any parent is hidden.
    for (uint32_t p = it->second.parent, guard = 0; p != 0 && guard < 16; ++guard) {
        uint32_t f = 0, parent = 0;
        if (!Memory::TryRead32(p + 0xB8, f) || (f & 1u) == 0) {
            return false;
        }
        Memory::TryRead32(p + 0xC, parent);
        p = parent;
    }
    const float squeeze = Memory::ReadFloat32(0x8042B084u);
    scale = Memory::ReadFloat32(pane + 0x84 + 20);  // the y scale: x also carries the layout squeeze
    cx = Memory::ReadFloat32(pane + 0x84 + 12) / (squeeze > 0.1f ? squeeze : 1.0f);
    cy = Memory::ReadFloat32(pane + 0x84 + 28);
    alpha = static_cast<float>((flags >> 16) & 0xFFu) / 255.0f;  // mGlbAlpha
    return std::isfinite(scale) && std::isfinite(cx) && std::isfinite(cy) && scale > 0.01f && alpha > 0.0f;
}

bool MenuWindowTransform(float& cx, float& cy, float& scale, float& alpha) {
    return WindowTransform(g_menuLayout, GameMenus::CourseSelectMenuObject(), "W_corseSelect_01", cx, cy, scale,
                           alpha);
}

bool PauseWindowTransform(float& cx, float& cy, float& scale, float& alpha) {
    return GameMenus::PauseWindowShowing() &&
           WindowTransform(g_pauseLayout, GameMenus::PauseWindowObject(), "W_N_pauseMenu_00", cx, cy, scale,
                           alpha);
}

// Draws a "Settings" button in the game's button style at y (window space) of a menu window. With
// match, it copies the window's own buttons instead: their size (the W_SButton frame, as wide and tall on screen) and
// their live scale, the selected one's when Settings is focused and the other's when it isn't.
struct MatchButtons {
    const MenuLayout* layout;
    const char* first;
    const char* second;
    const float* heldScale;  // nonzero: the selected scale, taken from a game button Settings shrank
};

void DrawWindowSettings(ImDrawList* list, bool (*transform)(float&, float&, float&, float&), float y,
                        bool focused, const MatchButtons* match = nullptr) {
    float cx = 0, cy = 0, scale = 1, alpha = 1;
    if (!transform(cx, cy, scale, alpha)) {
        return;
    }
    // Draw in a view scaled about the window's centre, the way the window itself is scaled.
    GameLayout::View view = GameLayout::GameView();
    view.centerX += cx * view.scale;
    view.centerY -= cy * view.scale;
    view.scale *= scale;
    view.gameScaleX *= scale;
    const Canvas c{list, view, alpha};
    const bool selected = focused && g_ui.screen == Screen::None;
    if (match == nullptr) {
        Button(c, "Settings", 0, y, 312, selected, true, g_ui.frame, 34.0f, 56.0f);
        return;
    }
    // The pane's own scale (+0x44): the game's "on" animation grows the button its cursor is on.
    const auto liveScale = [&](const char* name) {
        const auto it = match->layout->panes.find(name);
        const float s = it != match->layout->panes.end() ? Memory::ReadFloat32(it->second.addr + 0x44) : 1.0f;
        return std::isfinite(s) && s > 0.5f && s < 2.0f ? s : 1.0f;
    };
    const float first = liveScale(match->first), second = liveScale(match->second);
    const float held = match->heldScale != nullptr ? *match->heldScale : 0.0f;
    Button(c, "Settings", 0, y, 308, selected, true, g_ui.frame, 34.0f, 53.0f,
           !selected ? std::min(first, second) : held > 0.5f ? held : std::max(first, second));
}

void DrawMenuSettings(ImDrawList* list) {
    DrawWindowSettings(list, MenuWindowTransform, kMenuSettingsY, g_menuFocus.load());
}

void DrawPauseSettings(ImDrawList* list) {
    static const MatchButtons kPauseButtons{&g_pauseLayout, "P_SBBase_00", "P_SBBase_02", &g_pauseOnScale[0]};
    DrawWindowSettings(list, PauseWindowTransform, kPauseSettingsY, g_pauseFocus.load(), &kPauseButtons);
}

// Which of the pause window's buttons the game's own cursor is on (0 Continue, 1 Exit Course), from
// where its corner brackets are: above or below Settings. -1 without a cursor.
int PauseGameCursor() {
    float cx = 0, cy = 0, scale = 1, alpha = 1;
    const CursorRect r = GameCursorRect();
    if (!r.valid || !PauseWindowTransform(cx, cy, scale, alpha)) {
        return -1;
    }
    return (r.y0 + r.y1) / 2 > cy + kPauseSettingsY * scale ? 0 : 1;
}

bool UpdateShown() {
    return g_ui.screen == Screen::None && Updater::Get().state != Updater::State::Idle;
}

void DrawUpdate(const Canvas& c) {
    if (!UpdateShown()) {
        return;
    }
    const Updater::Status update = Updater::Get();
    std::string label = "Update";
    bool busy = true;
    switch (update.state) {
    case Updater::State::Downloading:
        label = update.progress >= 0 ? "Updating " + std::to_string(static_cast<int>(update.progress * 100)) + "%"
                                     : "Updating...";
        break;
    case Updater::State::Installing: label = "Installing..."; break;
    case Updater::State::Restarting: label = "Restarting..."; break;
    case Updater::State::Failed: label = "Retry Update"; busy = false; break;
    default: busy = false; break;
    }
    // Busy, it stays orange (greyed out it reads as broken) and only pulses when the cursor is on it.
    Button(c, label, kUpdateX, kToggleY, kUpdateW, g_ui.toggleFocus && g_ui.topItem == 2, true, g_ui.frame,
           label.size() > 7 ? 22.0f : 30.0f, 53.0f, busy ? 0.94f : 0.0f, &kOrange);
}

// The Update button pressed (by the remote or a tap): installs the newer release, or tries again.
void PressUpdate() {
    const Updater::State state = Updater::Get().state;
    if (state == Updater::State::Available || state == Updater::State::Failed) {
        Updater::Install();
    }
}

void DrawToggle(const Canvas& c) {
    DrawUpdate(c);
    // Under a message that returns to Couch (a failed update), the toggle still says Couch.
    const bool lan = g_ui.screen != Screen::None &&
                     !(g_ui.screen == Screen::Message && g_ui.afterMessage == Screen::None);
    Text(c, "Play:", kToggleX - kToggleW / 2 - 12, kToggleY, 24, 2, 0xFFFFFFFFu);
    Button(c, lan ? "LAN" : "Couch", kToggleX, kToggleY, kToggleW, g_ui.toggleFocus && g_ui.topItem == 0, true,
           g_ui.frame);
    Button(c, "", kGearX, kGearY, kGearW, g_ui.toggleFocus && g_ui.topItem == 1, true, g_ui.frame);
    Gear(c, kGearX, kGearY, 13.0f);
}

// ---------------------------------------------------------------- settings
// Two tabs. Video: "Nx" renders N x 480 lines in a 16:9 picture (black bars beside it on a wider
// screen), "Nx Ultrawide" (offered on screens wider than 16:9) the same lines across the whole
// screen with the game's view widened, and Match Screen Resolution the screen's own pixels across
// the whole screen (the multiplier is greyed out meanwhile). Keybinds: the keyboard's remote.
constexpr float kScales[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
std::atomic<bool> g_settingsChanged{false};
std::atomic<bool> g_touchControlsToggled{false};  // the Keybinds tab's Touch Controls, since the last take  // the Android app copies them into its own settings
enum SettingsTab { kTabVideo, kTabAudio, kTabKeybinds, kTabLanguage };
int g_settingsTab = kTabVideo;
constexpr int kSettingsTabs = 4;
float g_lastScale = 1.0f;                    // the multiplier Match Screen Resolution returns to
bool g_lastWide = true;

// Cursor rows. Video: 0 tabs, 1 resolution, 2 match screen, 3 fullscreen, then renderer,
// Performance Monitor (FPS counter and frametime graph), spinning coins and V-Sync (one row, side by
// side) and OK. Keybinds: 0 tabs, 1-12
// the controls (two columns of six), 13 Reset Defaults, 14 OK.
// V-Sync is a Windows and Android row (both can lock the game's clock to the display; on Android the
// app keeps the choice through TakeSettingsChange). Fullscreen is a desktop row: a phone is always
// fullscreen.
#if defined(_WIN32) || defined(__ANDROID__)
constexpr bool kVSyncRow = true;
#else
constexpr bool kVSyncRow = false;
#endif
#if defined(__ANDROID__)
constexpr bool kFullscreenRow = false;
#else
constexpr bool kFullscreenRow = true;
#endif
constexpr int kVideoFullscreen = kFullscreenRow ? 3 : -1;
constexpr int kVideoRenderer = kFullscreenRow ? 4 : 3;
constexpr int kVideoGraph = kVideoRenderer + 1, kVideoCoins = kVideoGraph + 1;
constexpr int kVideoVSync = kVSyncRow ? kVideoCoins + 1 : -1;
constexpr int kVideoOk = (kVSyncRow ? kVideoVSync : kVideoCoins) + 1;
constexpr const char* kFullscreenNames[] = {"Off", "Borderless", "Exclusive"};
// The renderers to pick from ([video] graphics_api, the names main.cpp maps to aurora's backends);
// Automatic tries them best first and falls back down the list.
struct RendererChoice {
    const char* configName;
    const char* label;
};
#if defined(__APPLE__)
constexpr RendererChoice kRenderers[] = {{"auto", "Automatic"}, {"metal", "Metal"}};
#elif defined(__ANDROID__)
// OpenGL ES can't be picked for now (walled off for a later version); a saved one shows as Vulkan.
constexpr RendererChoice kRenderers[] = {{"auto", "Automatic"}, {"vulkan", "Vulkan"}};
#elif defined(_WIN32)
constexpr RendererChoice kRenderers[] = {
    {"auto", "Automatic"}, {"d3d12", "Direct3D 12"}, {"vulkan", "Vulkan"}, {"d3d11", "Direct3D 11"}};
#else
constexpr RendererChoice kRenderers[] = {{"auto", "Automatic"}, {"vulkan", "Vulkan"}};
#endif
constexpr int kRendererCount = static_cast<int>(sizeof(kRenderers) / sizeof(kRenderers[0]));
std::atomic<bool> g_rendererChanged{false};  // the Android app keeps the choice (TakeSettingsChange)

int RendererIndex() {
#if defined(__ANDROID__)
    const std::string saved = settings_overlay::GraphicsApiChoice();
    const std::string chosen = saved == "opengles" ? "vulkan" : saved;
#else
    const std::string chosen = settings_overlay::GraphicsApiChoice();
#endif
    for (int i = 0; i < kRendererCount; ++i) {
        if (chosen == kRenderers[i].configName) {
            return i;
        }
    }
    return 0;
}
constexpr int kKeyRows = 6;
// The QoL checkboxes under the keys: Space Smart Spin, Reverse Tilt, and Touch Controls on Android or
// Mouse as Gyro on the desktop.
constexpr int kKeySmartSpin = 1 + Keybinds::kActionCount;
constexpr int kKeyReverseTilt = kKeySmartSpin + 1;
#if defined(__ANDROID__)
constexpr bool kTouchToggle = true;
#else
constexpr bool kTouchToggle = false;
#endif
constexpr int kKeyThird = kKeyReverseTilt + 1;  // Touch Controls / Mouse as Gyro
constexpr int kKeyReset = kKeyThird + 1, kKeyOk = kKeyReset + 1;
constexpr int kKeyToggles[] = {kKeySmartSpin, kKeyReverseTilt, kKeyThird};
constexpr int kKeyToggleCount = 3;

struct VideoChoice {
    float scale;
    bool wide;
};

bool CurrentWide() {
    return settings_overlay::Aspect() == DisplaySettings::kAspectFill;
}

std::vector<VideoChoice> VideoChoices() {
    std::vector<VideoChoice> choices;
    for (const float scale : kScales) {
        choices.push_back({scale, false});
        if (DisplaySettings::SurfaceWide()) {
            choices.push_back({scale, true});
        }
    }
    return choices;
}

int CurrentChoice(const std::vector<VideoChoice>& choices) {
    const float current = settings_overlay::RenderScale();
    const float scale = current > 0.0f ? current : g_lastScale;
    const bool wide = (current > 0.0f ? CurrentWide() : g_lastWide) && DisplaySettings::SurfaceWide();
    int best = -1;
    for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
        if (std::fabs(choices[i].scale - scale) < 0.01f && choices[i].wide == wide) {
            best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            if (std::fabs(choices[i].scale - 1.0f) < 0.01f && choices[i].wide == wide) {
                best = i;
            }
        }
    }
    return std::max(best, 0);
}

std::string ChoiceLabel(const VideoChoice& choice) {
    char text[40];
    if (choice.wide) {
        std::snprintf(text, sizeof(text), "%gx Ultrawide", choice.scale);
    } else {
        std::snprintf(text, sizeof(text), "%gx (%dp)", choice.scale,
                      static_cast<int>(std::lround(480.0f * choice.scale)));
    }
    return text;
}

void ApplyVideo(float scale, bool wide) {
    if (settings_overlay::Aspect() != DisplaySettings::kAspect4x3) {
        settings_overlay::SetAspect(wide ? DisplaySettings::kAspectFill : DisplaySettings::kAspect16x9);
    }
    settings_overlay::SetRenderScale(scale);
    g_settingsChanged = true;
}

void StepVideo(int direction, bool wrap) {
    if (settings_overlay::RenderScale() <= 0.0f) {
        return;  // greyed out: Match Screen Resolution decides
    }
    const std::vector<VideoChoice> choices = VideoChoices();
    const int count = static_cast<int>(choices.size());
    int next = CurrentChoice(choices) + direction;
    if (next >= count) {
        next = wrap ? 0 : count - 1;
    } else if (next < 0) {
        next = wrap ? count - 1 : 0;
    }
    g_lastScale = choices[next].scale;
    g_lastWide = choices[next].wide;
    ApplyVideo(choices[next].scale, choices[next].wide);
}

void ToggleMatchScreen() {
    const float scale = settings_overlay::RenderScale();
    if (scale <= 0.0f) {
        ApplyVideo(g_lastScale, g_lastWide && DisplaySettings::SurfaceWide());
    } else {
        g_lastScale = scale;
        g_lastWide = CurrentWide();
        ApplyVideo(0.0f, true);
    }
}

void Arrow(const Canvas& c, float x, float y, bool right, bool selected, bool enabled = true) {
    const ImVec2 p = ToScreen(c, x, y);
    const float s = c.view.scale * (selected ? 1.0f + 0.12f * Pulse(g_ui.frame) : 1.0f);
    const float d = right ? 1.0f : -1.0f;
    const float alpha = c.alpha * (enabled ? 1.0f : 0.45f);
    const ImU32 fill = enabled ? IM_COL32(255, 230, 60, static_cast<int>(255 * alpha))
                               : IM_COL32(170, 170, 170, static_cast<int>(255 * alpha));
    const ImU32 edge = IM_COL32(48, 32, 10, static_cast<int>(220 * alpha));
    const ImVec2 a(p.x + d * 14 * s, p.y), b(p.x - d * 10 * s, p.y - 15 * s), e(p.x - d * 10 * s, p.y + 15 * s);
    c.list->AddTriangleFilled(a, b, e, fill);
    c.list->AddTriangle(a, b, e, edge, 2.5f * c.view.scale);
}

void Checkbox(const Canvas& c, float x, float y, bool on) {
    const ImVec2 p = ToScreen(c, x, y);
    const float s = c.view.scale, h = 15.0f * s;
    c.list->AddRectFilled(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), IM_COL32(255, 255, 255, static_cast<int>(240 * c.alpha)), 5 * s);
    c.list->AddRect(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), IM_COL32(48, 32, 10, static_cast<int>(230 * c.alpha)), 5 * s, 0, 3 * s);
    if (on) {
        const ImVec2 points[] = {ImVec2(p.x - 9 * s, p.y), ImVec2(p.x - 2 * s, p.y + 8 * s), ImVec2(p.x + 10 * s, p.y - 9 * s)};
        c.list->AddPolyline(points, 3, IM_COL32(40, 160, 40, static_cast<int>(255 * c.alpha)), 0, 5 * s);
    }
}

constexpr float kSettingsHeight = 440.0f, kSettingsWidth = 720.0f;

void DrawSettingsTabs(const Canvas& c) {
    const float y = kSettingsHeight / 2 - 42.0f;
    const char* names[kSettingsTabs] = {"Video", "Audio",
                                        settings_overlay::ControllerConnected() ? "Controller" : "Keybinds", "Language"};
    for (int tab = 0; tab < kSettingsTabs; ++tab) {
        const float x = (tab - 1.5f) * 168.0f;
        Button(c, names[tab], x, y, 156, g_ui.cursor == 0 && g_settingsTab == tab, g_settingsTab == tab, g_ui.frame,
               28.0f);
        AddHitEvent(x, y, 164, 60, 0, kEvTab0 << tab);
    }
}

// The Audio tab: Master (everything), Music and Sound Effects, each a slider with its percentage to
// its left. Cursor rows: 1-3 the sliders (left / right move 5%), then 4 OK. A finger or the mouse
// pressed on a slider drags it; the value is saved when it is let go.
struct VolumeRow {
    const char* label;
    const char* note;
    settings_overlay::Volume which;
};
constexpr VolumeRow kVolumeRows[] = {
    {"Master", "The volume of everything.", settings_overlay::Volume::Master},
    {"Music", "The background music.", settings_overlay::Volume::Music},
    {"Sound Effects", "Jumps, coins, enemies and the rest.", settings_overlay::Volume::SoundEffects},
};
constexpr int kAudioOk = 4;
constexpr float kSliderX0 = -10.0f, kSliderX1 = 300.0f;
int g_dragSlider = -1;  // the row a finger or the mouse is dragging, or -1

float VolumeRowY(int row) {
    return 100.0f - 72.0f * row;
}

void DrawSliderAt(const Canvas& c, float x0, float x1, float y, int percent, bool selected) {
    const ImVec2 a = ToScreen(c, x0, y), b = ToScreen(c, x1, y);
    const float s = c.view.scale;
    const float half = 10.0f * s;
    const auto alpha = [&](int value) { return static_cast<int>(value * c.alpha); };
    const ImU32 edge = IM_COL32(48, 32, 10, alpha(230));
    c.list->AddRectFilled(ImVec2(a.x, a.y - half), ImVec2(b.x, b.y + half), IM_COL32(255, 255, 255, alpha(235)), half);
    const float fill = a.x + (b.x - a.x) * static_cast<float>(percent) / 100.0f;
    if (percent > 0) {
        c.list->AddRectFilled(ImVec2(a.x, a.y - half), ImVec2(std::max(fill, a.x + 2 * half), b.y + half),
                              IM_COL32(120, 200, 40, alpha(255)), half);
    }
    c.list->AddRect(ImVec2(a.x, a.y - half), ImVec2(b.x, b.y + half), edge, half, 0, 3.0f * s);
    const float knob = (selected ? 19.0f + 2.0f * Pulse(g_ui.frame) : 17.0f) * s;
    c.list->AddCircleFilled(ImVec2(fill, a.y), knob,
                            selected ? IM_COL32(255, 230, 60, alpha(255)) : IM_COL32(255, 255, 255, alpha(255)));
    c.list->AddCircle(ImVec2(fill, a.y), knob, edge, 0, 3.5f * s);
}

void DrawSlider(const Canvas& c, float y, int percent, bool selected) {
    DrawSliderAt(c, kSliderX0, kSliderX1, y, percent, selected);
}

// The Frametime Graph row's size slider (Video tab): 5% to 200%, 50% the graph's original size.
constexpr float kGraphSliderX0 = 92.0f, kGraphSliderX1 = 232.0f;
constexpr int kGraphSizeMin = 5, kGraphSizeMax = 200;
bool g_dragGraphSize = false;

int GraphSizeToFill(int size) {
    return (size - kGraphSizeMin) * 100 / (kGraphSizeMax - kGraphSizeMin);
}

void DrawAudioTab(const Canvas& c) {
    // A press on a slider (a little past either end counts) starts dragging it.
    const ImVec2 start = ToScreen(c, kSliderX0, 0), end = ToScreen(c, kSliderX1, 0);
    const float reach = 24.0f * c.view.scale;
    if (g_pointerNow.pressed) {
        for (int row = 0; row < 3; ++row) {
            const ImVec2 at = ToScreen(c, kSliderX0, VolumeRowY(row));
            if (g_pointerNow.x >= start.x - reach && g_pointerNow.x <= end.x + reach &&
                std::fabs(g_pointerNow.y - at.y) <= 30.0f * c.view.scale) {
                g_dragSlider = row;
                g_ui.cursor = 1 + row;
            }
        }
    }
    if (g_dragSlider >= 0) {
        const settings_overlay::Volume which = kVolumeRows[g_dragSlider].which;
        const float t = std::clamp((g_pointerNow.x - start.x) / std::max(1.0f, end.x - start.x), 0.0f, 1.0f);
        const int percent = static_cast<int>(std::lround(t * 100.0f));
        if (g_pointerNow.held) {
            if (percent != settings_overlay::VolumePercent(which)) {
                settings_overlay::SetVolumePercent(which, percent, false);
            }
        } else {
            settings_overlay::SetVolumePercent(which, settings_overlay::VolumePercent(which), true);
            g_dragSlider = -1;
        }
    }
    for (int row = 0; row < 3; ++row) {
        const float y = VolumeRowY(row);
        const bool selected = g_ui.cursor == 1 + row;
        const int percent = settings_overlay::VolumePercent(kVolumeRows[row].which);
        if (selected) {
            Cursor(c, -10, y, 660, 58, g_ui.frame);
        }
        Text(c, kVolumeRows[row].label, -325, y, 28, 0);
        Text(c, std::to_string(percent) + "%", -62, y, 28, 1, selected ? 0xFFF0B0FFu : 0xFFFFFFFFu);
        DrawSlider(c, y, percent, selected);
        AddHit(-10, y, 660, 64, 1 + row);
    }
    const int row = g_ui.cursor - 1;
    Text(c, row >= 0 && row < 3 ? kVolumeRows[row].note : "Left and right change the selected volume.", 0, -126, 21);
    Button(c, "OK", 0, -kSettingsHeight / 2 + 43.0f, 232, g_ui.cursor == kAudioOk, true, g_ui.frame);
    AddHit(0, -kSettingsHeight / 2 + 43.0f, 240, 60, kAudioOk);
}

// The Language tab: the USA disc's three languages. The game reads the console language once at
// boot (SCGetLanguage), so a change shows the next time it starts. Cursor rows: 1-3, then 4 OK.
// restartNote is written in the language itself: the player choosing it may not read the one the
// game is running in.
struct LanguageChoice {
    const char* code;
    const char* name;
    const char* restartNote;
};
constexpr LanguageChoice kLanguages[] = {
    {"en", "English", "Restart the game to play in this language."},
    {"es", "Español", "Reinicia el juego para jugar en este idioma."},
    {"fr", "Français", "Redémarrez le jeu pour jouer dans cette langue."},
};
constexpr int kLanguageOk = 4;

void DrawLanguageTab(const Canvas& c) {
    static const std::string atBoot = RuntimeConfigFile::Language();
    const std::string chosen = RuntimeConfigFile::Language();
    for (int i = 0; i < 3; ++i) {
        const float y = 100.0f - 58.0f * i;
        Button(c, kLanguages[i].name, 24, y, 380, g_ui.cursor == 1 + i, true, g_ui.frame, 30.0f, 48.0f);
        Checkbox(c, -140, y, chosen == kLanguages[i].code);
        AddHit(0, y, 400, 54, 1 + i);
    }
    const char* note = "The language of the game's text and menus.";
    if (chosen != atBoot) {
        for (const LanguageChoice& language : kLanguages) {
            if (chosen == language.code) {
                note = language.restartNote;
            }
        }
    }
    Text(c, note, 0, -82, 22, 1, chosen != atBoot ? 0xFFF0B0FFu : 0xFFFFFFFFu);
    Button(c, "OK", 0, -kSettingsHeight / 2 + 43.0f, 232, g_ui.cursor == kLanguageOk, true, g_ui.frame);
    AddHit(0, -kSettingsHeight / 2 + 43.0f, 240, 60, kLanguageOk);
}

void DrawVideoTab(const Canvas& c) {
    const float scale = settings_overlay::RenderScale();
    const bool match = scale <= 0.0f;
    const std::vector<VideoChoice> choices = VideoChoices();
    const VideoChoice shown = choices[CurrentChoice(choices)];
    // Five or six rows between the tabs and the hint line.
    constexpr int kRows = 5 + (kFullscreenRow ? 1 : 0);
    const float kRowH = kRows >= 7 ? 33.0f : kRows == 6 ? 37.0f : 42.0f;
    const float step = kRows >= 7 ? 37.0f : kRows == 6 ? 43.0f : 50.0f;
    const float top = kRows >= 7 ? 125.0f : kRows == 6 ? 123.0f : 118.0f;
    const float labelSize = kRows >= 7 ? 24.0f : 26.0f, buttonSize = kRows >= 7 ? 27.0f : 30.0f;
    Text(c, "Resolution", -300, top, labelSize, 0, match ? 0xA0A0A0FFu : 0xFFFFFFFFu);
    Button(c, ChoiceLabel(shown), 105, top, 270, g_ui.cursor == 1, !match, g_ui.frame, buttonSize, kRowH);
    Arrow(c, -50, top, false, g_ui.cursor == 1, !match);
    Arrow(c, 260, top, true, g_ui.cursor == 1, !match);
    if (!match) {
        AddHitEvent(-50, top, 54, step - 2, 1, kEvLeft);
        AddHitEvent(260, top, 54, step - 2, 1, kEvRight);
        AddHitEvent(105, top, 276, step - 2, 1, kEvRight);
    }
    const auto row = [&](const char* label, int index, int cursor, bool on) {
        const float y = top - index * step;
        Button(c, label, 24, y, 470, g_ui.cursor == cursor, true, g_ui.frame, buttonSize, kRowH);
        Checkbox(c, -185, y, on);
        AddHit(0, y, 480, step - 2, cursor);
    };
    row("Match Screen Resolution", 1, 2, match);
    if (kFullscreenRow) {
        // Off, Borderless, Exclusive: each press moves to the next, like the game's own buttons.
        const float y = top - 2 * step;
        Text(c, "Fullscreen", -300, y, labelSize, 0);
        Button(c, kFullscreenNames[std::clamp(settings_overlay::Fullscreen(), 0, 2)], 105, y, 270,
               g_ui.cursor == kVideoFullscreen, true, g_ui.frame, buttonSize, kRowH);
        AddHit(105, y, 276, step - 2, kVideoFullscreen);
    }
    {
        // Renderer: each press moves to the next (left steps back); it takes effect at the next start.
        const float y = top - (kFullscreenRow ? 3 : 2) * step;
        Text(c, "Renderer", -300, y, labelSize, 0);
        Button(c, kRenderers[RendererIndex()].label, 105, y, 270, g_ui.cursor == kVideoRenderer, kRendererCount > 1,
               g_ui.frame, buttonSize, kRowH);
        AddHit(105, y, 276, step - 2, kVideoRenderer);
    }
    const int first = kFullscreenRow ? 4 : 3;
    {
        // Performance Monitor (the FPS counter and frametime graph): the button turns it on and off;
        // the slider beside it sets its size (a finger or the mouse drags it; left and right move it
        // 5% at a time).
        const float y = top - first * step;
        const bool selected = g_ui.cursor == kVideoGraph;
        Button(c, "Performance Monitor", -80, y, 320, selected, true, g_ui.frame, buttonSize * 0.9f, kRowH);
        Checkbox(c, -214, y, settings_overlay::FrameGraph());
        AddHit(-80, y, 324, step - 2, kVideoGraph);
        const ImVec2 start = ToScreen(c, kGraphSliderX0, y), end = ToScreen(c, kGraphSliderX1, y);
        const float reach = 24.0f * c.view.scale;
        if (g_pointerNow.pressed && g_pointerNow.x >= start.x - reach && g_pointerNow.x <= end.x + reach &&
            std::fabs(g_pointerNow.y - start.y) <= (step / 2) * c.view.scale) {
            g_dragGraphSize = true;
            g_ui.cursor = kVideoGraph;
        }
        if (g_dragGraphSize && g_pointerNow.held) {
            const float t = std::clamp((g_pointerNow.x - start.x) / std::max(1.0f, end.x - start.x), 0.0f, 1.0f);
            const int size = kGraphSizeMin + static_cast<int>(std::lround(t * (kGraphSizeMax - kGraphSizeMin) / 5.0f)) * 5;
            settings_overlay::SetFrameGraphSize(size, false);
        } else if (g_dragGraphSize) {
            // The finger's gone, so its position is too: keep the last size and save it.
            settings_overlay::SetFrameGraphSize(settings_overlay::FrameGraphSize(), true);
            g_dragGraphSize = false;
            g_settingsChanged = true;
        }
        const int size = settings_overlay::FrameGraphSize();
        DrawSliderAt(c, kGraphSliderX0, kGraphSliderX1, y, GraphSizeToFill(size), selected);
        Text(c, std::to_string(size) + "%", 290, y, labelSize, 1, selected ? 0xFFF0B0FFu : 0xFFFFFFFFu);
    }
    if (kVSyncRow) {
        // Spinning Coins and V-Sync side by side, like the Keybinds tab's checkboxes.
        const float y = top - (first + 1) * step;
        constexpr float kWidth = 330.0f, kGap = 340.0f;
        const auto half = [&](const char* label, float x, int cursor, bool on) {
            Button(c, label, x + 18, y, kWidth, g_ui.cursor == cursor, true, g_ui.frame, buttonSize, kRowH);
            Checkbox(c, x - kWidth / 2 + 24, y, on);
            AddHit(x, y, kWidth + 6, step - 2, cursor);
        };
        half("Spinning Coins", -kGap / 2, kVideoCoins, settings_overlay::SpinningCoins());
        half("V-Sync", kGap / 2, kVideoVSync, settings_overlay::VSync());
    } else {
        row("Spinning Coins", first + 1, kVideoCoins, settings_overlay::SpinningCoins());
    }
    const char* hint = match ? "The screen's own resolution, filling the whole screen."
                       : !DisplaySettings::SurfaceWide() ? "Rendered at this many lines, then scaled to the screen."
                       : shown.wide                      ? "Fills the whole screen with a wider view."
                                                         : "16:9, with black bars at the sides.";
    const int fullscreen = settings_overlay::Fullscreen();
    const char* fullscreenNote = fullscreen == 1 ? "Borderless: a window covering the whole screen (F11)."
                                 : fullscreen == 2 ? "Exclusive: the game takes over the display."
                                                   : "Off: the game plays in a window.";
    const std::string rendererNote = std::string(RendererIndex() == 0 ? "The best this device has. " : "") +
                                     "Takes effect at the next start. Now: " + settings_overlay::RunningGraphicsApi() + ".";
    const char* note = g_ui.cursor == kVideoFullscreen ? fullscreenNote
                       : g_ui.cursor == kVideoRenderer ? rendererNote.c_str()
                       : g_ui.cursor == kVideoGraph ? "FPS and each frame's time, top right. Left and right: its size."
                       : g_ui.cursor == kVideoCoins ? "Animated coins and blocks. Off saves a little GPU work."
                       : g_ui.cursor == kVideoVSync ? "No tearing: frames wait for the display, the game keeps its pace."
                                                    : hint;
    Text(c, note, 0, kRows >= 7 ? -136 : kRows == 6 ? -133 : -126, 21);
    Button(c, "OK", 0, -kSettingsHeight / 2 + 43.0f, 232, g_ui.cursor == kVideoOk, true, g_ui.frame);
    AddHit(0, -kSettingsHeight / 2 + 43.0f, 240, 60, kVideoOk);
}

void DrawKeybindsTab(const Canvas& c) {
    // With a controller connected, the tab is its Controller Bindings: the same controls, on its buttons.
    const bool pad = settings_overlay::ControllerConnected();
    const int capturing = pad ? settings_overlay::ControllerCapturing() : Keybinds::Capturing();
    for (int action = 0; action < Keybinds::kActionCount; ++action) {
        const int column = action / kKeyRows, row = action % kKeyRows;
        const float x0 = column == 0 ? -178.0f : 178.0f;
        const float y = 126.0f - 40.0f * row;
        Text(c, Keybinds::ActionName(action), x0 - 168, y, 21, 0);
        const std::string label = capturing == action ? (pad ? "Press a button..." : "Press a key...")
                                  : pad              ? settings_overlay::ControllerBindingLabel(action)
                                                     : Keybinds::KeyLabel(action);
        const float size = label.size() > 20 ? 14.0f : label.size() > 16 ? 17.0f : label.size() > 11 ? 19.0f : 22.0f;
        const bool enabled = !pad || settings_overlay::ControllerActionRebindable(action);
        Button(c, label, x0 + 78, y, 172, g_ui.cursor == 1 + action, enabled, g_ui.frame, size, 35.0f);
        AddHit(x0 + 78, y, 178, 38, 1 + action);
    }
    if (capturing >= 0) {
#ifdef __ANDROID__
        constexpr const char* kKeyPrompt = "Press the new key (Esc cancels).";
#else
        constexpr const char* kKeyPrompt = "Press the new key or mouse button (Esc cancels).";
#endif
        Text(c, pad ? "Press the new button (Esc cancels)." : kKeyPrompt, 0, -122, 22);
    } else {
        // The QoL switches side by side.
        const char* labels[] = {"Space Smart Spin", "Reverse Tilt", kTouchToggle ? "Touch Controls" : "Mouse as Gyro"};
        const bool on[] = {Keybinds::SmartSpin(), Keybinds::ReverseTilt(),
                           kTouchToggle ? settings_overlay::TouchControls() : Keybinds::MouseGyro()};
        constexpr float width = 226.0f, gap = 232.0f;
        for (int i = 0; i < kKeyToggleCount; ++i) {
            const float x = (static_cast<float>(i) - (kKeyToggleCount - 1) * 0.5f) * gap;
            Button(c, labels[i], x + 18, -122, width, g_ui.cursor == kKeyToggles[i], true, g_ui.frame, 21.0f, 36.0f);
            Checkbox(c, x - width / 2 + 24, -122, on[i]);
            AddHit(x, -122, width + 6, 40, kKeyToggles[i]);
        }
    }
    const float y = -kSettingsHeight / 2 + 43.0f;
    Button(c, "Reset Defaults", -126, y, 232, g_ui.cursor == kKeyReset, true, g_ui.frame, 26.0f);
    Button(c, "OK", 126, y, 232, g_ui.cursor == kKeyOk, true, g_ui.frame);
    AddHit(-126, y, 240, 60, kKeyReset);
    AddHit(126, y, 240, 60, kKeyOk);
}

void DrawSettings(const Canvas& c) {
    Window(c, kSettingsHeight, kSettingsWidth);
    DrawSettingsTabs(c);
    if (g_settingsTab == kTabVideo) {
        DrawVideoTab(c);
    } else if (g_settingsTab == kTabAudio) {
        DrawAudioTab(c);
    } else if (g_settingsTab == kTabKeybinds) {
        DrawKeybindsTab(c);
    } else {
        DrawLanguageTab(c);
    }
}

void CloseSettings() {
    Show(Screen::None);
    if (g_ui.settingsFromPause) {
        g_ui.settingsFromPause = false;  // back to the pause menu, as it was
        return;
    }
    g_ui.toggleFocus = true;  // back to the file select, the cursor on the gear
    g_ui.topItem = 1;
}

void HandleSettings(bool left, bool right, bool up, bool down, bool confirm, bool back) {
    if (Keybinds::Capturing() >= 0) {
        Keybinds::PollCapture();  // the keyboard is the capture's until a key is picked
        return;
    }
    if (settings_overlay::ControllerCapturing() >= 0) {
        settings_overlay::PollControllerCapture();  // likewise the controller's buttons
        return;
    }
    int& cursor = g_ui.cursor;
    if (cursor == 0) {
        if (left || right) {
            g_settingsTab = std::clamp(g_settingsTab + (left ? -1 : 1), 0, kSettingsTabs - 1);
        } else if (confirm) {
            g_settingsTab = (g_settingsTab + 1) % kSettingsTabs;
        } else if (down) {
            cursor = g_settingsTab == kTabVideo && settings_overlay::RenderScale() <= 0.0f ? 2 : 1;
        } else if (back) {
            CloseSettings();
        }
        return;
    }
    if (back) {
        CloseSettings();
        return;
    }
    if (g_settingsTab == kTabVideo) {
        const bool match = settings_overlay::RenderScale() <= 0.0f;
        // Spinning Coins and V-Sync share a row: left and right move between them.
        if (up) {
            cursor = cursor == 2 && match              ? 0
                     : cursor == kVideoOk && kVSyncRow ? kVideoCoins
                     : cursor == kVideoVSync           ? kVideoGraph
                                                       : cursor - 1;
        }
        if (down) cursor = cursor == kVideoCoins && kVSyncRow ? kVideoOk : std::min(kVideoOk, cursor + 1);
        if (kVSyncRow && cursor == kVideoCoins && right) {
            cursor = kVideoVSync;
        } else if (kVSyncRow && cursor == kVideoVSync && left) {
            cursor = kVideoCoins;
        } else if (cursor == kVideoRenderer && (confirm || left || right)) {
            const int next = (RendererIndex() + (left ? kRendererCount - 1 : 1)) % kRendererCount;
            settings_overlay::SetGraphicsApiChoice(kRenderers[next].configName);
            settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
            g_rendererChanged = true;
            g_settingsChanged = true;
        } else if (cursor == 1 && (left || right || confirm)) {
            StepVideo(left ? -1 : 1, confirm);
        } else if (confirm && cursor == 2) {
            ToggleMatchScreen();
        } else if (cursor == kVideoFullscreen && (confirm || left || right)) {
            // Off -> Borderless -> Exclusive -> Off (left steps back), with the file select's sound.
            settings_overlay::SetFullscreen((settings_overlay::Fullscreen() + (left ? 2 : 1)) % 3);
            settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
        } else if (confirm && cursor == kVideoGraph) {
            settings_overlay::SetFrameGraph(!settings_overlay::FrameGraph());
            g_settingsChanged = true;
        } else if ((left || right) && cursor == kVideoGraph) {
            settings_overlay::SetFrameGraphSize(settings_overlay::FrameGraphSize() + (left ? -5 : 5), true);
            g_settingsChanged = true;
        } else if (confirm && cursor == kVideoVSync) {
            settings_overlay::SetVSync(!settings_overlay::VSync());
            g_settingsChanged = true;
        } else if (confirm && cursor == kVideoCoins) {
            settings_overlay::SetSpinningCoins(!settings_overlay::SpinningCoins());
            g_settingsChanged = true;
        } else if (confirm && cursor == kVideoOk) {
            CloseSettings();
        }
        return;
    }
    if (g_settingsTab == kTabAudio) {
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kAudioOk, cursor + 1);
        if (cursor >= 1 && cursor <= 3 && (left || right)) {
            const settings_overlay::Volume which = kVolumeRows[cursor - 1].which;
            const int now = settings_overlay::VolumePercent(which);
            // Steps of 5, landing on a multiple of 5 from a dragged-in value like 37.
            const int next = left ? (now - 1) / 5 * 5 : (now + 5) / 5 * 5;
            settings_overlay::SetVolumePercent(which, next, true);
        } else if (confirm && cursor == kAudioOk) {
            CloseSettings();
        }
        return;
    }
    if (g_settingsTab == kTabLanguage) {
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kLanguageOk, cursor + 1);
        if (confirm && cursor >= 1 && cursor <= 3) {
            RuntimeConfigFile::SetLanguage(kLanguages[cursor - 1].code);
            g_settingsChanged = true;
        } else if (confirm && cursor == kLanguageOk) {
            CloseSettings();
        }
        return;
    }
    if (const auto toggle = std::find(kKeyToggles, kKeyToggles + kKeyToggleCount, cursor);
        toggle != kKeyToggles + kKeyToggleCount) {
        const auto index = static_cast<int>(toggle - kKeyToggles);
        if (left && index > 0) cursor = kKeyToggles[index - 1];
        if (right && index + 1 < kKeyToggleCount) cursor = kKeyToggles[index + 1];
        if (up) cursor = index == 0 ? kKeyRows : 2 * kKeyRows;  // the last key of the column above
        if (down) cursor = kKeyReset;
        if (confirm && cursor == kKeySmartSpin) Keybinds::SetSmartSpin(!Keybinds::SmartSpin());
        if (confirm && cursor == kKeyReverseTilt) Keybinds::SetReverseTilt(!Keybinds::ReverseTilt());
        if (confirm && cursor == kKeyThird && kTouchToggle) {
            settings_overlay::SetTouchControls(!settings_overlay::TouchControls());
            g_touchControlsToggled = true;
            g_settingsChanged = true;
        } else if (confirm && cursor == kKeyThird) {
            Keybinds::SetMouseGyro(!Keybinds::MouseGyro());
        }
        return;
    }
    if (cursor >= kKeyReset) {
        if (left || right) cursor = cursor == kKeyReset ? kKeyOk : kKeyReset;
        if (up) cursor = kKeySmartSpin;
        if (confirm && cursor == kKeyReset) {
            if (settings_overlay::ControllerConnected()) settings_overlay::ResetControllerBindings();
            else Keybinds::ResetDefaults();
        }
        if (confirm && cursor == kKeyOk) CloseSettings();
        return;
    }
    const int action = cursor - 1, column = action / kKeyRows, row = action % kKeyRows;
    if (up) cursor = row == 0 ? 0 : cursor - 1;
    if (down) cursor = row == kKeyRows - 1 ? (column == 0 ? kKeySmartSpin : kKeyReverseTilt) : cursor + 1;
    if ((left && column == 1) || (right && column == 0)) cursor = 1 + (1 - column) * kKeyRows + row;
    if (confirm) {
        if (settings_overlay::ControllerConnected()) settings_overlay::BeginControllerCapture(action);
        else Keybinds::BeginCapture(action);
    }
}

void DrawLan(const Canvas& c) {
    Window(c, 322);
    Title(c, "LAN", 0, 112, 40);
    Text(c, "Join a room on another device,", 0, 40);
    Text(c, "or create one here.", 0, 5);
    Text(c, "Tailscale works, but is unstable.", 0, -38, 22, 1, 0xFFF0B0FFu);
    ButtonPair(c, "Join", "Create Room", 322, g_ui.cursor);
    Text(c, "(1) Back", 240, 130, 22, 2);  // back to Couch; tappable
    AddHit(195, 130, 140, 44, -1, true);
}

// A hint over one of the game's own screens, in the empty right half of its title bar.
void Banner(const Canvas& c, const std::string& text, uint32_t color = 0xFFFFFFFFu, float y = 192.0f) {
    const float x = 140.0f, halfWidth = 178.0f, halfHeight = 19.0f;
    c.list->AddRectFilled(
        ImVec2(c.view.centerX + (x - halfWidth) * c.view.scale, c.view.centerY - (y + halfHeight) * c.view.scale),
        ImVec2(c.view.centerX + (x + halfWidth) * c.view.scale, c.view.centerY - (y - halfHeight) * c.view.scale),
        IM_COL32(20, 40, 90, static_cast<int>(185 * c.alpha)), 10 * c.view.scale);
    Text(c, text, x, y, 24, 1, color);
}

void DrawChooseFileHint(const Canvas& c) {
    Banner(c, "Pick the save for your room.", 0xFFFFFFFFu, 148.0f);  // under the toggle
}

void DrawCountHint(const Canvas& c) {
    if (g_ui.countHintFrames > 0) {
        Banner(c, "A LAN room needs 2 to 4 players.", 0xFFE040FFu);
    } else {
        Banner(c, "How many can join your room?");
    }
}

void DrawRooms(const Canvas& c, const NetplayLobby::Snapshot& lobby) {
    const float height = 390;
    Window(c, height, 560);
    Title(c, "Rooms", 0, height / 2 - 40, 40);
    const int count = static_cast<int>(lobby.rooms.size());
    if (count == 0) {
        const int dots = (g_ui.frame / 20) % 4;
        Text(c, "Looking for rooms" + std::string(dots, '.'), 0, 30);
        Text(c, "Rooms on your Wi-Fi and Tailscale", 0, -20, 24);
        Text(c, "show up here.", 0, -48, 24);
    }
    const int visible = std::min(count, 4);
    const int first = std::clamp(g_ui.cursor - 3, 0, std::max(0, count - 4));
    for (int i = 0; i < visible; ++i) {
        const auto& room = lobby.rooms[first + i];
        const float y = height / 2 - 105 - i * 62;
        std::string label = room.name + "  " + std::to_string(room.players) + "/" + std::to_string(room.maxPlayers);
        if (room.tailscale) {
            label += "  (Tailscale)";
        }
        Button(c, label, 0, y, 480, g_ui.cursor == first + i, !room.full, g_ui.frame);
        AddHit(0, y, 490, 58, first + i);
    }
    Text(c, "(1) Back", 225, -height / 2 + 22, 22, 2);
    AddHit(175, -height / 2 + 22, 140, 44, -1, true);
}

void DrawRoom(const Canvas& c, const NetplayLobby::Snapshot& lobby) {
    const float height = 400;
    Window(c, height, 560);
    const bool host = lobby.localSlot == 0;
    Title(c, lobby.roomName.empty() ? "Room" : lobby.roomName, 0, height / 2 - 40, 36);
    for (int slot = 0; slot < std::max<int>(lobby.maxPlayers, 2); ++slot) {
        const auto member = std::find_if(lobby.members.begin(), lobby.members.end(),
                                         [&](const NetplayLobby::Member& m) { return m.slot == slot; });
        const float y = height / 2 - 100 - slot * 44;
        if (member != lobby.members.end()) {
            Text(c, PlayerLine(static_cast<uint8_t>(slot), member->name, slot == lobby.localSlot), -230, y, 28, 0);
        } else {
            Text(c, std::to_string(slot + 1) + "P  waiting...", -230, y, 28, 0, 0xC8D2E6FFu);
        }
    }
    std::string status;
    if (lobby.phase == Phase::Starting || lobby.phase == Phase::Restarting) {
        status = lobby.message.empty() ? "Starting..." : lobby.message;
    } else if (lobby.phase == Phase::Joining) {
        status = "Joining...";
    } else if (host) {
        status = lobby.members.size() < 2 ? "Others pick LAN > Join on their device." : "Start when everyone is here.";
    } else {
        status = "Waiting for the host to start...";
    }
    if (!lobby.message.empty() && lobby.phase == Phase::Hosting) {
        status = lobby.message;
    }
    Text(c, status, 0, -height / 2 + 100, 24, 1, 0xFFF0B0FFu);
    if (lobby.phase == Phase::Starting || lobby.phase == Phase::Restarting) {
        return;
    }
    if (host) {
        ButtonPair(c, "Start", "Cancel", height, g_ui.cursor, lobby.members.size() >= 2, true);
    } else {
        Button(c, "Leave", 0, -height / 2 + 43.0f, 232, true, true, g_ui.frame);
        AddHit(0, -height / 2 + 43.0f, 240, 60, 0);
    }
}

void DrawMessage(const Canvas& c) {
    Window(c, 300);
    // Up to two lines, split at a space near the middle.
    std::string first = g_ui.message, second;
    if (first.size() > 30) {
        const size_t mid = first.rfind(' ', first.size() / 2 + 6);
        if (mid != std::string::npos) {
            second = first.substr(mid + 1);
            first.resize(mid);
        }
    }
    Text(c, first, 0, second.empty() ? 30 : 50);
    if (!second.empty()) {
        Text(c, second, 0, 15);
    }
    Button(c, "OK", 0, -150 + 43.0f, 232, true, true, g_ui.frame);
    AddHit(0, -150 + 43.0f, 240, 60, 0);
}

std::string SlotName(int slot) {
    const auto& names = NetplayStart::SessionNames();
    return slot >= 0 && slot < static_cast<int>(names.size()) ? names[slot] : "player " + std::to_string(slot + 1);
}

// What the session shows: who the game is waiting for, a desync warning, and the way out when a
// player is gone (the game can't go on without them).
void DrawSession(ImDrawList* list) {
    const NetplaySession::Status status = NetplaySession::GetStatus();
    if (!status.active) {
        return;
    }
    const GameLayout::View view = GameLayout::GameView();
    Canvas c{list, view, 1.0f};
    if (status.lostSlot >= 0) {
        // The game is frozen: read the remote directly.
        static uint32_t prevHold = ~0u;
        WiiRemoteInput::KpadSample sample;
        uint32_t hold = 0;
        if (WiiRemoteInput::ReadKpadSample(0, sample)) {
            hold = sample.hold;
        }
        const bool confirm = prevHold != ~0u && (hold & ~prevHold & (kTwo | kA)) != 0;
        prevHold = hold;
        ++g_ui.frame;
        Veil(c);
        Window(c, 300);
        Text(c, "Lost the connection to", 0, 50);
        Text(c, SlotName(status.lostSlot) + ".", 0, 15);
        Button(c, "OK", 0, -150 + 43.0f, 232, true, true, g_ui.frame);
        if (confirm) {
            NetplayStart::EndSession();
        }
        return;
    }
    std::string line;
    if (status.waitingForSlot >= 0 && status.waitedMs > 700) {
        line = "Waiting for " + SlotName(status.waitingForSlot) + "...";
    } else if (status.desyncFrame != 0) {
        line = "The game got out of sync between devices.";
    }
    if (line.empty()) {
        return;
    }
    const float y = 190.0f;
    list->AddRectFilled(ImVec2(view.centerX - 290 * view.scale, view.centerY - (y + 20) * view.scale),
                        ImVec2(view.centerX + 290 * view.scale, view.centerY - (y - 20) * view.scale),
                        IM_COL32(20, 40, 90, 180), 10 * view.scale);
    Text(c, line, 0, y, 26);
}

// ---------------------------------------------------------------- logic
void HandleEvents(const NetplayLobby::Snapshot& lobby) {
    const uint32_t ev = g_ui.events;
    g_ui.events = 0;
    const bool left = ev & kEvLeft, right = ev & kEvRight, up = ev & kEvUp, down = ev & kEvDown;
    const bool confirm = ev & kEvConfirm, back = ev & kEvBack;
    for (int tab = 0; tab < kSettingsTabs; ++tab) {
        if ((ev & (kEvTab0 << tab)) != 0 && g_ui.screen == Screen::Settings) {
            g_settingsTab = tab;  // a tapped tab
            g_ui.cursor = 0;
            return;
        }
    }
    switch (g_ui.screen) {
    case Screen::Lan:
        if (left || right) g_ui.cursor ^= 1;
        if (back) {
            // Back to Couch, with the cursor still on the toggle.
            Show(Screen::None);
            g_ui.toggleFocus = true;
            g_ui.topItem = 0;
        }
        if (confirm) {
            if (g_ui.cursor == 0) {
                NetplayLobby::Browse();
                Show(Screen::Rooms);
            } else {
                // The game's own file select picks the save the room plays, then its "Select
                // Players" the room's size.
                g_ui.countHintFrames = 0;
                Show(Screen::ChooseFile);
            }
        }
        break;
    case Screen::Count:
        if (g_ui.countPicked) {
            g_ui.countPicked = false;
            const int players = GameMenus::SelectPlayersCursor() + 1;
            if (players < 2) {
                g_ui.countHintFrames = 150;
            } else {
                NetplayLobby::Host(static_cast<uint8_t>(players), GameMenus::FileSelectCursor());
                Show(Screen::Room);
            }
        }
        break;
    case Screen::Rooms: {
        const int count = static_cast<int>(lobby.rooms.size());
        if (up) g_ui.cursor = std::max(0, g_ui.cursor - 1);
        if (down) g_ui.cursor = std::min(std::max(0, count - 1), g_ui.cursor + 1);
        if (back) {
            NetplayLobby::Stop();
            Show(Screen::Lan);
        }
        if (confirm && g_ui.cursor < count && !lobby.rooms[g_ui.cursor].full) {
            NetplayLobby::Join(lobby.rooms[g_ui.cursor].id);
            Show(Screen::Room);
        }
        break;
    }
    case Screen::Room:
        if (lobby.phase == Phase::Starting || lobby.phase == Phase::Restarting) {
            break;
        }
        if (lobby.localSlot == 0 && lobby.phase == Phase::Hosting) {
            if (left || right) g_ui.cursor ^= 1;
            if (back || (confirm && g_ui.cursor == 1)) {
                // Cancel: close the room and take the game back from "Select Players" to the file
                // select, in Couch.
                NetplayLobby::Stop();
                Show(Screen::None);
                g_ui.injectButtons = kOne;
                g_ui.injectReads = 4;
            } else if (confirm && g_ui.cursor == 0 && lobby.members.size() >= 2) {
                NetplayLobby::Start();
            }
        } else if (back || confirm) {
            NetplayLobby::Stop();
            NetplayLobby::Browse();
            Show(Screen::Rooms);
        }
        break;
    case Screen::Message:
        if (confirm || back) {
            NetplayLobby::Stop();
            Show(g_ui.afterMessage);
        }
        break;
    case Screen::Settings:
        HandleSettings(left, right, up, down, confirm, back);
        break;
    case Screen::None:
        break;
    }
}

// The Couch / LAN button pressed (by the remote or a tap).
void PressToggle() {
    if (g_ui.screen == Screen::None) {
        g_ui.toggleFocus = false;
        Show(Screen::Lan);  // Couch -> LAN: Join or Create Room
    } else {
        NetplayLobby::Stop();
        Show(Screen::None);  // LAN (picking the room's save) -> Couch
        g_ui.heldFromMenu = g_ui.prevHold;
    }
}

// Select a World's ring of islands where TapAt measures, k = 0 the picked world's at the front and
// k = 1-7 the worlds after it going right round the ring (read off the screen at 16:9).
constexpr float kIslandX[8] = {-5, 164, 203, 116, 0, -121, -188, -179};
constexpr float kIslandY[8] = {-75, -65, -7, 46, 90, 46, 2, -75};

// A tap on Select a World: Back (1) at the bottom right, the front island picks its world (2), and
// another island the game lets the player pick turns the ring to it.
void TapWorldSelect(float lx, float ly) {
    const float right = GameLayout::GameView().centerX / GameLayout::GameView().scale;
    if (lx >= right - 150 && lx <= right - 20 && ly >= -190 && ly <= -135) {
        StartNav(NavKind::Press, static_cast<int>(kOne));
        return;
    }
    const int world = GameMenus::WorldSelectWorld();
    if (world < 0 || world > 7 || !GameMenus::WorldSelectWaiting()) {
        return;  // World 9 is off the ring; mid-turn the islands are moving
    }
    int island = -1;
    float nearest = 130;
    for (int k = 0; k < 8; ++k) {
        // The front island is drawn the biggest: it reaches further.
        const float distance = std::hypot(lx - kIslandX[k], ly - kIslandY[k]) * (k == 0 ? 0.6f : 1.0f);
        if (distance < nearest) {
            nearest = distance;
            island = k;
        }
    }
    if (island == 0) {
        StartNav(NavKind::Press, static_cast<int>(kTwo));
    } else if (island > 0 && GameMenus::WorldSelectCanPick((world + island) % 8)) {
        StartNav(NavKind::WorldSelect, (world + island) % 8);
    }
}

// A tap at (lx, ly) in layout units (y up, the game's 4:3 layout space centred on the screen). The
// game's own buttons are where its layouts (fileSelectBase, fileSelectPlayer) put them.
// `gx` is x in the game's own layout units, which differ from these menus' in Fill (View::gameScaleX).
void TapAt(float lx, float ly, float gx, uint16_t scene, bool title) {
    const auto in = [&](float cx, float cy, float w, float h) {
        return std::fabs(lx - cx) <= w / 2 && std::fabs(ly - cy) <= h / 2;
    };
    const auto inGame = [&](float cx, float cy, float w, float h) {
        return std::fabs(gx - cx) <= w / 2 && std::fabs(ly - cy) <= h / 2;
    };
    if (Modal()) {
        for (const Hit& hit : g_hits) {
            if (in(hit.x, hit.y, hit.w, hit.h)) {
                if (hit.cursor >= 0) {
                    g_ui.cursor = hit.cursor;
                }
                g_ui.events |= hit.event;
                return;
            }
        }
        return;
    }
    if (g_ui.toggleShown && in(kToggleX, kToggleY, kToggleW + 16, 66)) {
        PressToggle();
        return;
    }
    if (g_ui.toggleShown && UpdateShown() && in(kUpdateX, kToggleY, kUpdateW + 16, 66)) {
        g_ui.toggleFocus = false;
        PressUpdate();
        return;
    }
    if (g_ui.toggleShown && g_ui.screen == Screen::None && in(kGearX, kGearY, kGearW + 16, 66)) {
        g_ui.toggleFocus = false;
        Show(Screen::Settings);
        return;
    }
    // Anywhere else is the game's: the cursor leaves the toggle so the game takes the presses.
    g_ui.toggleFocus = false;
    if (g_nav.kind == NavKind::MapWalk) {
        WorldMapNav::Cancel();  // a new tap on the map replaces the walk
        g_nav.kind = NavKind::None;
    }
    if (g_nav.kind != NavKind::None) {
        return;  // still playing back the last tap
    }
    if (scene == kSceneBoot || title) {
        StartNav(NavKind::Press, static_cast<int>(kTwo | kA));  // the strap screen, "Press 2 to Start"
        return;
    }
    if (scene == kSceneWorldMap && GameMenus::StarCoinsWaiting()) {
        if (lx > 0 && ly < -170) {
            StartNav(NavKind::Press, static_cast<int>(kOne));  // Back (1), below the panel on the right
        }
        return;  // nothing else on the Star Coins screen takes a tap
    }
    if (scene == kSceneWorldMap && GameMenus::WorldSelectOpen()) {
        TapWorldSelect(lx, ly);
        return;
    }
    if (scene == kSceneWorldMap && g_ui.menuSettingsShown) {
        if (in(0, kMenuSettingsY, 345, 64)) {
            g_menuFocus = true;
            g_menuFocusFrom = std::max(2, GameMenus::CourseMenuCursor());
            g_ui.settingsFromPause = true;
            Show(Screen::Settings);
        } else if (in(kMenuBackX, kMenuBackY, 210, 40)) {
            StartNav(NavKind::Press, static_cast<int>(kOne));  // Back (1)
        } else {
            g_menuFocus = false;  // the game's own cursor (hidden while Settings has it) takes the tap
            StartCursorNav(lx, ly);  // one of the game's own buttons
        }
        return;
    }
    if (scene == kSceneStage && g_ui.pauseSettingsShown) {
        float cx = 0, cy = 0, scale = 1, alpha = 1;
        if (PauseWindowTransform(cx, cy, scale, alpha) &&
            in(cx, cy + kPauseSettingsY * scale, 345 * scale, 62 * scale)) {
            const int cursor = PauseGameCursor();
            g_pauseFocus = true;
            g_pauseFocusFrom = cursor >= 0 ? cursor : g_pauseFocusFrom;
            g_ui.settingsFromPause = true;
            Show(Screen::Settings);
        } else {
            g_pauseFocus = false;
            StartCursorNav(lx, ly);  // Continue or Exit Course
        }
        return;
    }
    // Any other menu of the game's with its select cursor up: steer the cursor to the tap.
    if (scene != kSceneGameSetup && GameCursorRect().valid) {
        StartCursorNav(lx, ly);
        return;
    }
    // The world map's button hints, anchored to the screen's corners: tapping one presses its button.
    if (scene == kSceneWorldMap && !GameMenus::PauseMenuOpen()) {
        const float right = GameLayout::GameView().centerX / GameLayout::GameView().scale;
        const auto band = [&](float x0, float x1, float y0, float y1) {
            return lx >= x0 && lx <= x1 && ly >= y0 && ly <= y1;
        };
        uint32_t press = 0;
        if (band(right - 150, right - 25, 160, 202)) press = kPlus;            // Menu (+)
        else if (band(right - 250, right - 25, 122, 164)) press = kMinus;      // Select World (-)
        else if (band(-right + 25, -right + 210, -200, -152)) press = kA;      // View Map (A)
        else if (band(right - 160, right - 25, -200, -152)) press = kOne;      // Items (1)
        if (press != 0) {
            StartNav(NavKind::Press, static_cast<int>(press));
            return;
        }
        // A course (or house, castle...): walk there; on the one the player stands on, enter it.
        const int point = WorldMapNav::PointAtTap(lx, ly);
        if (point >= 0) {
            if (WorldMapNav::PlayerOn(point)) {
                StartNav(NavKind::Press, static_cast<int>(kTwo));
            } else {
                StartNav(NavKind::MapWalk, point);
                WorldMapNav::WalkTo(point);
            }
        }
        return;
    }
    if (scene != kSceneGameSetup) {
        return;
    }
    if (GameMenus::FileSelectWaiting()) {
        for (int file = 0; file < 3; ++file) {
            if (inGame(-190.0f + 190.0f * file, 45, 184, 196)) {
                StartNav(NavKind::File, file);
                return;
            }
        }
        if (inGame(-143, -143, 259, 92)) {
            StartNav(NavKind::File, 3);  // Free-for-All
        } else if (inGame(143, -143, 259, 92)) {
            StartNav(NavKind::File, 4);  // Coin Battle
        } else if (inGame(-265, -62, 190, 46)) {
            StartNav(NavKind::Press, static_cast<int>(kMinus));  // Erase
        } else if (inGame(265, -62, 190, 46)) {
            StartNav(NavKind::Press, static_cast<int>(kPlus));  // Copy
        }
        return;
    }
    if (GameMenus::SelectPlayersState() == GameMenus::SelectPlayers::Choosing) {
        // Measured on screen: "1" spans the width above "2", "3" and "4".
        if (inGame(0, 40, 410, 122)) {
            StartNav(NavKind::Players, 0);
            return;
        }
        for (int count = 2; count <= 4; ++count) {
            if (inGame(-120.0f + 120.0f * (count - 2), -81, 112, 104)) {
                StartNav(NavKind::Players, count - 1);
                return;
            }
        }
        // "Back (1)" sits at the bottom right, nearer the edge on a wide screen.
        if (inGame(320, -183, 260, 70)) {
            StartNav(NavKind::Press, static_cast<int>(kOne));
        }
    }
}

} // namespace

void Tap(float x, float y) {
    const uint64_t fx = static_cast<uint64_t>(std::clamp(x, 0.0f, 1.0f) * 65535.0f);
    const uint64_t fy = static_cast<uint64_t>(std::clamp(y, 0.0f, 1.0f) * 65535.0f);
    g_tap.store((1ull << 63) | (fx << 16) | fy, std::memory_order_release);
}

void Pointer(float x, float y, bool held) {
    const uint64_t fx = static_cast<uint64_t>(std::clamp(x, 0.0f, 1.0f) * 65535.0f);
    const uint64_t fy = static_cast<uint64_t>(std::clamp(y, 0.0f, 1.0f) * 65535.0f);
    g_pointer.store((held ? 1ull << 63 : 0) | (fx << 16) | fy, std::memory_order_release);
}

bool TapScreenUp() {
    return g_tapScreen.load(std::memory_order_acquire);
}

bool Playing() {
    return g_playing.load(std::memory_order_acquire);
}

bool TakeSettingsChange(float& renderScale, bool& showFps, int& aspect, bool& spinningCoins, bool& frameGraph,
                        bool& vsync, int& frameGraphSize, int& touchControls, int& renderer) {
    if (!g_settingsChanged.exchange(false)) {
        return false;
    }
    renderScale = settings_overlay::RenderScale();
    showFps = settings_overlay::FrameGraph();  // the FPS counter is part of the Performance Monitor
    aspect = settings_overlay::Aspect();
    spinningCoins = settings_overlay::SpinningCoins();
    frameGraph = settings_overlay::FrameGraph();
    vsync = settings_overlay::VSync();
    frameGraphSize = settings_overlay::FrameGraphSize();
    // Only a change made here: the app's own menu changes it too, and tells the game.
    touchControls = g_touchControlsToggled.exchange(false) ? (settings_overlay::TouchControls() ? 1 : 0) : -1;
    renderer = g_rendererChanged.exchange(false) ? RendererIndex() : -1;
    return true;
}

void OnSessionStall() {
    // At most ~30 presents a second: enough for the banner, cheap while nothing moves.
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(33)) {
        return;
    }
    last = now;
    UpdateAuroraAndProcessEvents();
    if (g_auroraFrameActive.load(std::memory_order_acquire)) {
        settings_overlay::Draw();
        VI_HLE_PresentFrame(/*presentedXfb=*/false, /*paceToRetrace=*/false);
    }
}

// NSMBW_LOBBY_TEST (Android: debug.nsmbw.lobbytest) drives the lobby without the menus, to test
// rooms between devices: "host" opens a room for 2 and starts it once someone joins, "join" joins
// the first room it finds. The session then starts as it does from the menus.
bool LobbyTest() {
    static const std::string mode = [] {
        const char* value = std::getenv("NSMBW_LOBBY_TEST");
        return value != nullptr ? std::string(value) : std::string();
    }();
    if (mode != "host" && mode != "join") {
        return false;
    }
    static bool begun = false;
    if (!begun) {
        begun = true;
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: %s\n", mode.c_str());
        if (mode == "host") {
            NetplayLobby::Host(2, 0);
        } else {
            NetplayLobby::Browse();
        }
        return true;
    }
    const NetplayLobby::Snapshot lobby = NetplayLobby::Get();
    if (mode == "host" && lobby.phase == Phase::Hosting && lobby.members.size() >= 2) {
        NetplayLobby::Start();
    }
    if (mode == "join" && lobby.phase == Phase::Browsing && !lobby.rooms.empty()) {
        const auto& room = lobby.rooms.front();
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: joining '%s' at %s%s\n", room.name.c_str(), room.address.c_str(),
                room.tailscale ? " (Tailscale)" : "");
        NetplayLobby::Join(room.id);
    }
    static std::string lastMessage;
    if (lobby.phase == Phase::Failed && lobby.message != lastMessage) {
        lastMessage = lobby.message;
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: failed: %s\n", lobby.message.c_str());
    }
    if (lobby.phase == Phase::Restarting && !g_ui.restarting) {
        NetplayLobby::Plan plan;
        if (NetplayLobby::TakePlan(plan)) {
            g_ui.restarting = true;
            NetplayStart::Restart(plan);
        }
    }
    return true;
}

// NSMBW_DUMP_PROFILES=1: log the object profiles whenever the set changes (finding screens).
void DumpProfiles() {
    static const bool on = [] {
        const char* value = std::getenv("NSMBW_DUMP_PROFILES");
        return value != nullptr && value[0] == '1';
    }();
    static int frame = 0;
    static std::vector<uint16_t> last;
    if (!on || ++frame % 30 != 0) {
        return;
    }
    const auto profiles = GameMenus::Profiles();
    if (profiles == last) {
        return;
    }
    std::string text;
    for (const uint16_t p : profiles) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%03X ", p);
        text += hex;
    }
    RT_LOGF(RT_TAG_RUNTIME, "profiles (scene %03X, %zu): %s\n", GameMenus::CurrentScene(), profiles.size(), text.c_str());
    last = profiles;
}

// NSMBW_WARP=<world>-<course> (debug.nsmbw.warp on Android): entering any course from the world map
// enters that one instead (testing a level without a save that reaches it). Courses 1-9 are the
// numbered ones; others are STAGE_e numbers: 20 ghost house, 21 tower, 23 castle, 32-34 the map
// enemies' courses (5-32: World 5's giant Piranha Plant), 37 airship. Works by
// rewriting dInfo_c::m_startGameInfo (0x80315B90; world/course shown at +0xC/+0xD and loaded at
// +0xE/+0xF, all from 0)
// while the map fades out to the course-in scene, which reads it. <world>-<course>:<area>:<entrance>
// starts in that area (from 0) at that entrance id instead of the course's start.
void Warp() {
    static int world = -1, course = -1, area = 0, entrance = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* value = std::getenv("NSMBW_WARP"); value != nullptr && value[0] != 0) {
            int w = 0, c = 0, a = 0, e = 0;
            if (std::sscanf(value, "%d-%d:%d:%d", &w, &c, &a, &e) >= 2 && w >= 1 && w <= 9 && c >= 1 && c <= 42) {
                world = w - 1;
                course = c <= 9 ? c - 1 : c;  // 1-9 as shown, the rest as STAGE_e numbers
                area = a;
                entrance = e;
            }
        }
    }
    if (world < 0) {
        return;
    }
    uint32_t sceneWord = 0;
    if (!Memory::TryRead32(0x80428730u, sceneWord)) {
        return;
    }
    const uint16_t next = static_cast<uint16_t>(sceneWord >> 16), now = static_cast<uint16_t>(sceneWord & 0xFFFFu);
    static bool done = false;
    if (next == 0x007 && now == 0x003) {
        if (!done) {
            Memory::Write8(0x80315B90u + 0xC, static_cast<uint8_t>(world));
            Memory::Write8(0x80315B90u + 0xD, static_cast<uint8_t>(course));
            Memory::Write8(0x80315B90u + 0xE, static_cast<uint8_t>(world));  // the course loaded
            Memory::Write8(0x80315B90u + 0xF, static_cast<uint8_t>(course));
            Memory::Write8(0x80315B90u + 0x5, static_cast<uint8_t>(entrance));
            Memory::Write8(0x80315B90u + 0x6, static_cast<uint8_t>(area));
            RT_LOGF(RT_TAG_RUNTIME, "warp: course %d-%d\n", world + 1, course + 1);
            done = true;
        }
    } else if ((now == 0x006 || next == 0x006) && (area != 0 || entrance != 0)) {
        // RESTART_CRSIN (a save that boots back into its last course) starts from its own copy,
        // dScRestartCrsin_c::m_startGameInfo: with an area given, boot straight into it.
        constexpr uint32_t kRestart = 0x80373D60u;
        Memory::Write8(kRestart + 0xC, static_cast<uint8_t>(world));
        Memory::Write8(kRestart + 0xD, static_cast<uint8_t>(course));
        Memory::Write8(kRestart + 0xE, static_cast<uint8_t>(world));
        Memory::Write8(kRestart + 0xF, static_cast<uint8_t>(course));
        Memory::Write8(kRestart + 0x5, static_cast<uint8_t>(entrance));
        Memory::Write8(kRestart + 0x6, static_cast<uint8_t>(area));
    } else if (now == 0x007 && done && (area != 0 || entrance != 0)) {
        // The course-in scene can set its own start point after the map's: keep ours until the
        // course loads.
        Memory::Write8(0x80315B90u + 0x5, static_cast<uint8_t>(entrance));
        Memory::Write8(0x80315B90u + 0x6, static_cast<uint8_t>(area));
    } else if (now != 0x007) {
        done = false;
    }
}

// NSMBW_DUMP_LAYOUT=<arc>;<arc>...: log every pane's position in those layouts once (finding
// where the game's own buttons are).
void DumpLayouts() {
    static bool done = false;
    const char* value = std::getenv("NSMBW_DUMP_LAYOUT");
    if (done || value == nullptr) {
        return;
    }
    done = true;
    std::string all = value;
    for (size_t pos = 0; pos < all.size();) {
        const size_t end = std::min(all.find(';', pos), all.size());
        const std::string arc = all.substr(pos, end - pos);
        pos = end + 1;
        GameLayout::Layout layout;
        if (!layout.Load(arc)) {
            RT_LOGF(RT_TAG_RUNTIME, "layout dump: can't load %s\n", arc.c_str());
            continue;
        }
        for (const auto& name : layout.PaneNames()) {
            float x = 0, y = 0, w = 0, h = 0;
            if (layout.PaneWorldRect(name, x, y, w, h)) {
                RT_LOGF(RT_TAG_RUNTIME, "layout %s %s: x %.0f y %.0f w %.0f h %.0f\n", arc.c_str(), name.c_str(), x, y, w, h);
            }
        }
    }
}

// NSMBW_TAP_SCRIPT="<ms>:<x>:<y>,..." (Android: debug.nsmbw.tapscript): taps at fractions of the
// window, <ms> after the first frame, through the same path as real ones (testing without a hand).
void ScriptedTaps() {
    struct ScriptTap { long long ms; float x, y; };
    static std::vector<ScriptTap> taps = [] {
        std::vector<ScriptTap> list;
        const char* value = std::getenv("NSMBW_TAP_SCRIPT");
        if (value == nullptr) {
            return list;
        }
        long long ms = 0;
        float x = 0, y = 0;
        for (const char* p = value; *p != '\0';) {
            if (std::sscanf(p, "%lld:%f:%f", &ms, &x, &y) == 3) {
                list.push_back({ms, x, y});
            }
            const char* comma = std::strchr(p, ',');
            if (comma == nullptr) {
                break;
            }
            p = comma + 1;
        }
        return list;
    }();
    static const auto start = std::chrono::steady_clock::now();
    static size_t next = 0;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    while (next < taps.size() && taps[next].ms <= elapsed) {
        RT_LOGF(RT_TAG_RUNTIME, "tap script: %.2f, %.2f\n", taps[next].x, taps[next].y);
        Tap(taps[next].x, taps[next].y);
        ++next;
    }
}

void Draw() {
    DumpProfiles();
    Warp();
    if (std::getenv("NSMBW_DUMP_STATES") != nullptr) {
        static std::string lastStates;
        std::string states = GameMenus::DumpStates();
        if (states != lastStates) {
            RT_LOGF(RT_TAG_RUNTIME, "states: %s\n", states.c_str());
            lastStates = std::move(states);
        }
    }
    DumpLayouts();
    ScriptedTaps();
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    // A pause menu: looked for every few frames (walking the object tree is not free).
    static int pauseCheck = 0;
    static int pauseKind = 0;
    if (++pauseCheck % 6 == 0) {
        pauseKind = GameMenus::PauseMenuKind();
    }
    if (NetplaySession::Active()) {
        g_tapScreen.store(false, std::memory_order_release);
        const uint16_t scene = GameMenus::CurrentScene();
        g_playing.store((scene == kSceneStage || scene == kSceneWorldMap) && pauseKind == 0 && !Modal(),
                        std::memory_order_release);
        DrawSession(list);
        return;
    }
    if (LobbyTest()) {
        return;
    }

    // The game's own setup screens decide when the toggle shows and the LAN steps go away.
    const uint16_t scene = GameMenus::CurrentScene();
    const bool fileWaiting = GameMenus::FileSelectWaiting();
    const GameMenus::SelectPlayers players = GameMenus::SelectPlayersState();
    if (scene != kSceneGameSetup) {
        // Back to the title, or into the game: the next file select starts in Couch.
        if (g_ui.screen == Screen::Lan || g_ui.screen == Screen::ChooseFile || g_ui.screen == Screen::Count) {
            Show(Screen::None);
        }
    }
    // The LAN / Couch toggle: on the file select while it waits for a pick (also under LAN's windows).
    g_ui.toggleShown = scene == kSceneGameSetup && fileWaiting && g_ui.screen != Screen::Count;
    if (!g_ui.toggleShown) {
        g_ui.toggleFocus = false;
    }
    // A failed update says why, once, on the file select (the button then offers to try again).
    static bool updateFailureShown = false;
    if (const Updater::Status update = Updater::Get(); update.state != Updater::State::Failed) {
        updateFailureShown = false;
    } else if (!updateFailureShown && g_ui.toggleShown && g_ui.screen == Screen::None) {
        updateFailureShown = true;
        ShowMessage(update.message, Screen::None);
    }
    if (g_ui.toggleFocus && !Modal()) {
        const uint32_t ev = g_ui.events;
        g_ui.events = 0;
        if (ev & (kEvDown | kEvBack)) {
            // Back down to the files; the press that left the toggle stays with us.
            g_ui.toggleFocus = false;
            g_ui.heldFromMenu = g_ui.prevHold;
        } else if (ev & kEvRight) {
            g_ui.topItem = g_ui.topItem == 2 ? 0 : 1;  // Update -> Couch -> gear
        } else if (ev & kEvLeft) {
            g_ui.topItem = g_ui.topItem == 1 || !UpdateShown() ? 0 : 2;
        } else if ((ev & kEvConfirm) && g_ui.topItem == 1) {
            g_ui.toggleFocus = false;
            Show(Screen::Settings);
        } else if ((ev & kEvConfirm) && g_ui.topItem == 2) {
            PressUpdate();
        } else if (ev & kEvConfirm) {
            PressToggle();
        }
    }
    // Create Room: picking a file leads to the game's "Select Players", which sizes the room; Back
    // there returns to the file pick, and Free-for-All or Coin Battle leave room-making behind.
    if (g_ui.screen == Screen::ChooseFile && players == GameMenus::SelectPlayers::Choosing) {
        Show(Screen::Count);
    } else if (g_ui.screen == Screen::Count && players == GameMenus::SelectPlayers::Hidden && fileWaiting) {
        Show(Screen::ChooseFile);
    } else if ((g_ui.screen == Screen::ChooseFile || g_ui.screen == Screen::Count) &&
               players == GameMenus::SelectPlayers::Other) {
        Show(Screen::None);
    }
    // The pause menus' gear.
    if (pauseCheck % 6 == 0) {
        g_ui.pauseSettingsShown = scene == kSceneStage && pauseKind == 2;
        g_ui.menuSettingsShown = scene == kSceneWorldMap && pauseKind == 1;
        if (pauseKind == 2) {
            DumpPanesOnce(GameMenus::PauseWindowObject(), "pause");
        }
    }
    if (scene == kSceneWorldMap) {
        ApplyMenuLayout();
        static const bool dumpMap = std::getenv("NSMBW_DUMP_WMAP") != nullptr;
        static int dumpFrame = 0;
        if (dumpMap && ++dumpFrame % 600 == 0) {
            WorldMapNav::Dump();
        }
    } else if (g_nav.kind == NavKind::MapWalk) {
        WorldMapNav::Cancel();
        g_nav.kind = NavKind::None;
    }
    if (scene == kSceneStage) {
        ApplyPauseLayout();
    }
    if (g_menuOpenSettings.exchange(false) || g_pauseOpenSettings.exchange(false)) {
        g_ui.settingsFromPause = true;
        Show(Screen::Settings);
    }
    HideGameCursor(((g_menuFocus.load() && g_ui.menuSettingsShown) || (g_pauseFocus.load() && g_ui.pauseSettingsShown)) &&
                   g_ui.screen == Screen::None);
    if (scene == kSceneStage) {
        HoldPauseButtonScale(g_pauseFocus.load() && g_ui.pauseSettingsShown && g_ui.screen == Screen::None);
    }
    if (!g_ui.menuSettingsShown) {
        g_menuFocus = false;
    }
    if (!g_ui.pauseSettingsShown && !g_ui.settingsFromPause) {
        g_pauseFocus = false;
    }
    // Touch (the Android app's Tap) and mouse clicks on the menus.
    static int titleCheck = 0;
    static bool title = false;
    if (++titleCheck % 10 == 0) {
        title = scene == kSceneStage && GameMenus::HasProfile(kProfileOpeningTitle);
    }
    const bool tapScreen = Modal() || g_ui.toggleShown || scene == kSceneBoot || title ||
                           (scene == kSceneGameSetup && players == GameMenus::SelectPlayers::Choosing);
    g_tapScreen.store(tapScreen, std::memory_order_release);
    g_playing.store((scene == kSceneStage || scene == kSceneWorldMap) && !title && !tapScreen && pauseKind == 0 &&
                        g_ui.screen == Screen::None,
                    std::memory_order_release);
    {
        const ImGuiIO& io = ImGui::GetIO();
        const uint64_t finger = g_pointer.load(std::memory_order_acquire);
        const bool wasHeld = g_pointerNow.held;
        if ((finger >> 63) != 0) {
            g_pointerNow.held = true;
            g_pointerNow.x = static_cast<float>((finger >> 16) & 0xFFFF) / 65535.0f * io.DisplaySize.x;
            g_pointerNow.y = static_cast<float>(finger & 0xFFFF) / 65535.0f * io.DisplaySize.y;
        } else {
            g_pointerNow.held = io.MouseDown[0] && !io.WantCaptureMouse;
            g_pointerNow.x = io.MousePos.x;
            g_pointerNow.y = io.MousePos.y;
        }
        g_pointerNow.pressed = g_pointerNow.held && !wasHeld;
        float tapX = -1.0f, tapY = -1.0f;
        const uint64_t tap = g_tap.exchange(0, std::memory_order_acq_rel);
        if ((tap >> 63) != 0) {
            tapX = static_cast<float>((tap >> 16) & 0xFFFF) / 65535.0f * io.DisplaySize.x;
            tapY = static_cast<float>(tap & 0xFFFF) / 65535.0f * io.DisplaySize.y;
        } else if (io.MouseClicked[0] && !io.WantCaptureMouse) {
            tapX = io.MousePos.x;
            tapY = io.MousePos.y;
        }
        if (tapX >= 0.0f) {
            const GameLayout::View view = GameLayout::GameView();
            TapAt((tapX - view.centerX) / view.scale, (view.centerY - tapY) / view.scale,
                  (tapX - view.centerX) / view.gameScaleX, scene, title);
        }
    }

    if (g_ui.screen == Screen::None) {
        g_ui.events = 0;  // presses on the game's own screens are the game's
        if (g_ui.toggleShown) {
            ++g_ui.frame;
            DrawToggle(Canvas{list, GameLayout::GameView(), 1.0f});
        }
        if (scene == kSceneStage) {
            ++g_ui.frame;
            DrawPauseSettings(list);
        }
        if (scene == kSceneWorldMap) {
            ++g_ui.frame;
            DrawMenuSettings(list);
        }
        return;
    }

    const NetplayLobby::Snapshot lobby = NetplayLobby::Get();
    if (lobby.phase == Phase::Failed && g_ui.screen != Screen::Message) {
        ShowMessage(lobby.message.empty() ? "Something went wrong." : lobby.message, Screen::Lan);
    }
    if (lobby.phase == Phase::Restarting && !g_ui.restarting) {
        NetplayLobby::Plan plan;
        if (NetplayLobby::TakePlan(plan)) {
            g_ui.restarting = true;
            if (!NetplayStart::Restart(plan)) {
                g_ui.restarting = false;
                NetplayLobby::Stop();
                ShowMessage("Couldn't start the session on this device.", Screen::Lan);
            }
        }
    }
    HandleEvents(lobby);
    if (g_ui.countHintFrames > 0) {
        --g_ui.countHintFrames;
    }

    GetAssets();
    ++g_ui.frame;
    Canvas c{list, GameLayout::GameView(), std::min(1.0f, g_ui.frame / 8.0f)};
    g_hits.clear();  // the screen below registers its buttons as it draws them
    if (g_ui.toggleShown) {
        DrawToggle(Canvas{list, c.view, 1.0f});  // under LAN's windows and their veil
    }
    switch (g_ui.screen) {
    case Screen::ChooseFile:
        DrawChooseFileHint(c);
        break;
    case Screen::Count:
        DrawCountHint(c);
        break;
    case Screen::Lan:
        Veil(c);
        DrawLan(c);
        break;
    case Screen::Rooms:
        Veil(c);
        DrawRooms(c, lobby);
        break;
    case Screen::Room:
        Veil(c);
        DrawRoom(c, lobby);
        break;
    case Screen::Message:
        Veil(c);
        DrawMessage(c);
        break;
    case Screen::Settings:
        Veil(c);
        DrawSettings(c);
        break;
    case Screen::None:
        break;
    }
}

void FilterGameInput(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (NetplaySession::Active()) {
        return;
    }
    if (chan == 0) {
        // A tap on the game's own screens plays back as presses of the first remote. Those steer the
        // game's cursor straight to the tapped button, so the Settings rows below leave them alone.
        const bool tapping = g_nav.kind == NavKind::Cursor;
        if (!Modal()) {
            sample.hold |= NavButtons();
        }
        // Menu events from the first remote: screen directions for a sideways remote, the D-pad's
        // own for one held upright with a Nunchuk.
        const uint32_t hold = sample.hold;
        const uint32_t pressed = hold & ~g_ui.prevHold;
        g_ui.prevHold = hold;
        const bool upright = sample.hasNunchuk;
        if (pressed & (upright ? kLeft : kUp)) g_ui.events |= kEvLeft;
        if (pressed & (upright ? kRight : kDown)) g_ui.events |= kEvRight;
        if (pressed & (upright ? kUp : kRight)) g_ui.events |= kEvUp;
        if (pressed & (upright ? kDown : kLeft)) g_ui.events |= kEvDown;
        if (pressed & (kTwo | kA)) g_ui.events |= kEvConfirm;
        if (pressed & (kOne | kB)) g_ui.events |= kEvBack;
        if ((pressed & kPlus) && g_ui.screen == Screen::Settings) g_ui.events |= kEvBack;  // Esc / + closes it

        // The world map's + menu: Settings sits between Quick Save and Title Screen. The game never
        // sees the presses that move onto or off it, and its own cursor is moved to match.
        if (g_ui.menuSettingsShown && !Modal() && !tapping) {
            const uint32_t down = upright ? kDown : kLeft, up = upright ? kUp : kRight;
            const int cursor = GameMenus::CourseMenuCursor();
            if (!g_menuFocus) {
                if ((pressed & down) != 0 && cursor == 2) {
                    g_menuFocus = true;
                    g_menuFocusFrom = 2;
                    g_ui.heldFromMenu |= down;
                } else if ((pressed & up) != 0 && cursor == 3) {
                    g_menuFocus = true;
                    g_menuFocusFrom = 3;
                    g_ui.heldFromMenu |= up;
                }
            } else {
                if ((pressed & up) != 0) {
                    g_menuFocus = false;
                    g_ui.heldFromMenu |= up;
                    if (g_menuFocusFrom == 3) {
                        g_ui.injectButtons = up;  // the game's cursor: Title Screen -> Quick Save
                        g_ui.injectReads = 4;
                    }
                } else if ((pressed & down) != 0) {
                    g_menuFocus = false;
                    g_ui.heldFromMenu |= down;
                    if (g_menuFocusFrom == 2) {
                        g_ui.injectButtons = down;  // Quick Save -> Title Screen
                        g_ui.injectReads = 4;
                    }
                } else if ((pressed & (kTwo | kA)) != 0) {
                    g_ui.heldFromMenu |= pressed & (kTwo | kA);
                    g_menuOpenSettings = true;
                } else if ((pressed & (kOne | kB)) != 0) {
                    g_menuFocus = false;  // the game closes its menu
                }
                if (g_menuFocus) {
                    sample.hold &= ~(kUp | kDown | kLeft | kRight | kTwo | kA);
                }
            }
        }

        // A level's pause window: Settings sits between Continue and Exit Course, the same way.
        if (g_ui.pauseSettingsShown && !Modal() && !tapping) {
            const uint32_t down = upright ? kDown : kLeft, up = upright ? kUp : kRight;
            if (!g_pauseFocus) {
                const int cursor = PauseGameCursor();
                if ((pressed & down) != 0 && cursor == 0) {
                    g_pauseFocus = true;
                    g_pauseFocusFrom = 0;
                    g_ui.heldFromMenu |= down;
                } else if ((pressed & up) != 0 && cursor == 1) {
                    g_pauseFocus = true;
                    g_pauseFocusFrom = 1;
                    g_ui.heldFromMenu |= up;
                }
            } else {
                if ((pressed & up) != 0) {
                    g_pauseFocus = false;
                    g_ui.heldFromMenu |= up;
                    if (g_pauseFocusFrom == 1) {
                        g_ui.injectButtons = up;  // the game's cursor: Exit Course -> Continue
                        g_ui.injectReads = 4;
                    }
                } else if ((pressed & down) != 0) {
                    g_ui.heldFromMenu |= down;
                    if (!PauseExitDisabled()) {  // otherwise Settings is as far down as it goes
                        g_pauseFocus = false;
                        if (g_pauseFocusFrom == 0) {
                            g_ui.injectButtons = down;  // Continue -> Exit Course
                            g_ui.injectReads = 4;
                        }
                    }
                } else if ((pressed & (kTwo | kA)) != 0) {
                    g_ui.heldFromMenu |= pressed & (kTwo | kA);
                    g_pauseOpenSettings = true;
                } else if ((pressed & (kOne | kB | kPlus)) != 0) {
                    g_pauseFocus = false;  // the game closes its window
                }
                if (g_pauseFocus) {
                    sample.hold &= ~(kUp | kDown | kLeft | kRight | kTwo | kA);
                }
            }
        }

        // The Star Coins screen only leaves on 1, and the yes/no prompts cancel on it: + (Esc on a
        // keyboard) does the same there. The game never sees that +, so it doesn't open the + menu
        // again once the screen has closed.
        if ((pressed & kPlus) != 0 && !Modal() && (GameMenus::StarCoinsWaiting() || GameMenus::YesNoWaiting())) {
            g_ui.plusIsBack = true;
        }
        if ((hold & kPlus) == 0) {
            g_ui.plusIsBack = false;
        }
        if (g_ui.plusIsBack) {
            g_ui.heldFromMenu |= kPlus;
        }

        // Up from any of the three files (also in Create Room's file pick) moves onto the LAN /
        // Couch toggle above them; the game never sees that press, so its cursor stays put.
        const uint32_t screenUp = upright ? kUp : kRight;
        const int fileCursor = GameMenus::FileSelectRawCursor();
        if (g_ui.toggleShown && !g_ui.toggleFocus && !Modal() && (pressed & screenUp) != 0 && fileCursor >= 0 &&
            fileCursor <= 2) {
            g_ui.toggleFocus = true;
            g_ui.topItem = 0;
            g_ui.events &= ~kEvUp;
            g_ui.heldFromMenu |= screenUp;
        }
        // Create Room: the game's own screen picks the number; its confirm press is ours.
        if (g_ui.screen == Screen::Count && (pressed & (kTwo | kA)) != 0 &&
            GameMenus::SelectPlayersState() == GameMenus::SelectPlayers::Choosing) {
            g_ui.countPicked = true;
            g_ui.swallowConfirm = true;
        }
        if (g_ui.swallowConfirm) {
            if ((hold & (kTwo | kA)) == 0) {
                g_ui.swallowConfirm = false;
            }
            sample.hold &= ~(kTwo | kA);
        }
        g_ui.heldFromMenu &= hold;
        sample.hold &= ~g_ui.heldFromMenu;
        if (g_ui.plusIsBack) {
            sample.hold |= kOne;
        }
        if (g_ui.injectReads > 0 && !Modal()) {
            --g_ui.injectReads;
            sample.hold |= g_ui.injectButtons;
        }
    }
    if (Modal() || g_ui.toggleFocus) {
        // The game sees the remote connected and at rest.
        sample.hold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clHold = 0;
    }
}

} // namespace NetplayUi
