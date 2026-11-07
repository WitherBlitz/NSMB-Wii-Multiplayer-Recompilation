#include "netplay_ui.h"

#include "hle/gameplay_options.h"
#include "hle/nsmbw_menu_settings.h"
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
#include "game_folder.h"
#include "key_caps.h"
#include "prompt_style.h"
#include "test_pad.h"
#include "keybinds.h"
#include "settings_overlay.h"
#include "ui_text.h"
#include "updater.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <functional>
#include <mutex>
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

#if defined(__ANDROID__)
namespace AndroidBridge {
void AskDirectAddress(const std::string& title, const std::string& hint);
void EditTouchLayout();
}
#endif

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
constexpr uint16_t kSceneMovie = 0x008;      // the opening movie (the title's, once it has idled)
constexpr uint32_t kStageGameMode = 0x8042A204u;  // dScStage_c::m_gameMode: 2 the title, 3 its demo replays
constexpr uint32_t kGameModeTitleReplay = 3;
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
    // Opened by the pause window's own Settings button (hle/nsmbw_menu_settings.cpp): shown once the
    // pause window has closed, and that window opens again when it closes.
    bool settingsFromGameButton = false;
    bool settingsFromMapButton = false;  // ... by the world map's menu's: the menu opens again after
    bool settingsWaitGame = false;
};

Ui g_ui;

// The settings window's opening and closing (DrawSettings).
double g_settingsOpenedAt = -10.0;
bool g_settingsClosing = false;
double g_settingsClosingAt = -10.0;
float g_settingsFit = 1.0f;  // the Settings window's scale to fit across a narrow screen (DrawSettings)
// The settings window closed: the pause window's Settings button gets the cursor back (below).
void NotifySettingsClosed();

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

// Join's Direct Connection address box (desktop; Android asks with a dialog of its own).
struct AddressEntry {
    bool open = false;
    std::string text;
};
AddressEntry g_entry;
std::mutex g_enteredMutex;
std::string g_entered;          // Android: the dialog's address, for the next frame
std::atomic<bool> g_hasEntered{false};

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
uint32_t FindRootPane(uint32_t object);

// Whether the file select's layout stacks the three files (Newer's fileSelectBase: up and down move
// between them) instead of putting them in a row (the game's own: left and right do). Read from
// where the layout puts its first two files' anchors, N_posFile1_00 and N_posFile2_00.
bool FilesStacked() {
    static uint32_t s_object = 0;
    static bool s_stacked = false;
    const uint32_t object = GameMenus::FileSelectObject();
    if (object == 0 || object == s_object) {
        return s_stacked;
    }
    const uint32_t root = FindRootPane(object);
    float x[2] = {}, y[2] = {};
    int found = 0;
    if (root != 0) {
        WalkPanes(root, 0, [&](uint32_t pane) {
            const std::string name = LivePaneName(pane);
            const int i = name == "N_posFile1_00" ? 0 : name == "N_posFile2_00" ? 1 : -1;
            if (i >= 0) {
                x[i] = Memory::ReadFloat32(pane + 0x2C);
                y[i] = Memory::ReadFloat32(pane + 0x30);
                found |= 1 << i;
            }
        });
    }
    if (found != 3) {
        return false;  // not built yet: look again next time
    }
    s_object = object;
    s_stacked = std::fabs(y[1] - y[0]) > std::fabs(x[1] - x[0]);
    return s_stacked;
}


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
        // 0-2 the files in a row, 3 and 4 Free-for-All and Coin Battle below them; on a layout
        // that stacks the files, one file per row.
        cursor = GameMenus::FileSelectRawCursor();
        const int fileRows = FilesStacked() ? 3 : 1;
        row = cursor <= 2 ? (fileRows == 3 ? cursor : 0) : fileRows;
        col = cursor <= 2 ? (fileRows == 3 ? 0 : cursor) : cursor - 3;
        targetRow = n.target <= 2 ? (fileRows == 3 ? n.target : 0) : fileRows;
        targetCol = n.target <= 2 ? (fileRows == 3 ? 0 : n.target) : n.target - 3;
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
    GameLayout::Animation cursorLoop;
    std::shared_ptr<GameLayout::Font> message;
    std::shared_ptr<GameLayout::Font> mario;
    // The pause window's (pauseMenu.arc, from the game folder in use: Newer's own there): the Settings
    // window is drawn in its style, with its buttons and their animations.
    GameLayout::Layout pause;
    GameLayout::Animation inWindow, outWindow, onButton, offButton, hitButton;
    bool pauseSlices = false;  // its buttons are three slices (Newer: Btn1_Left / Middle / Right)
};

Assets& GetAssets() {
    static Assets assets;
    if (!assets.tried) {
        assets.tried = true;
        assets.yesNo.Load("/Layout/yesnoWindow/yesnoWindow.arc");
        assets.cursor.Load("/Layout/select_cursor/select_cursor.arc");
        assets.cursorLoop.Load("/Layout/select_cursor/select_cursor.arc", "loopCursor.brlan");
        assets.message = GameLayout::LoadFont("mj2d00_MessageFont_32_I4.brfnt");
        assets.mario = GameLayout::LoadFont("mj2d01_marioFont_64_IA4.brfnt");
        constexpr const char* kPause = "/Layout/pauseMenu/pauseMenu.arc";
        float x = 0, y = 0, w = 0, h = 0;
        if (assets.pause.Load(kPause, "pauseMenu_16.brlyt") && assets.pause.PaneRect("P_SBBase_00", x, y, w, h) &&
            assets.pause.PaneRect("W_N_pauseMenu_00", x, y, w, h)) {
            assets.inWindow.Load(kPause, "_inWindow.brlan");
            assets.outWindow.Load(kPause, "_outWindow.brlan");
            assets.onButton.Load(kPause, "_onButton.brlan");
            assets.offButton.Load(kPause, "_offButon.brlan");
            assets.hitButton.Load(kPause, "_hitButton.brlan");
            assets.pauseSlices = assets.pause.PaneRect("Btn1_Middle", x, y, w, h);
        } else {
            RT_LOGF(RT_TAG_RUNTIME, "netplay ui: no pause window layout, the settings window keeps the yes/no look\n");
        }
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
    if (screen == Screen::Settings && g_ui.screen != Screen::Settings) {
        g_settingsOpenedAt = ImGui::GetTime();
        g_settingsClosing = false;
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
    a.yesNo.StretchSlices(p, "WindowLeft", "WindowCentre", "WindowRight", width - 528.0f, height - 322.0f);
    a.yesNo.Draw(c.list, c.view, "N_yesnoWindow_00", p);
}

// The game's message text: white with its dark edge, centred on (x, y) unless told otherwise. Text and
// Title put their English wording in the menus' language (UiText).
void Text(const Canvas& c, const std::string& text, float x, float y, float size = 30.0f, int originH = 1,
          uint32_t color = 0xFFFFFFFFu) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.message.get())) {
        return;
    }
    const std::u16string t = Utf16(UiText::Tr(text));
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
    GameLayout::DrawText(c.list, c.view, *a.mario, Utf16(UiText::Tr(text)), x, y, size, 0xFFFF00FFu, 0xFF8C00FFu, c.alpha, 1, 1);
}

float Pulse(int frame) {
    return 0.5f + 0.5f * std::sin(frame * 0.12f);
}

// While the Settings window draws its rows, the cursor is kept for last (FlushCursor), so its corners
// are on top of the rows next to it, as the game's own cursor is.
struct DeferredCursor {
    bool on = false;
    bool set = false;
    Canvas canvas{};
    float x = 0, y = 0, w = 0, h = 0;
    int frame = 0;
};
DeferredCursor g_deferredCursor;

// The game's selection cursor (four yellow corners) around a w x h box centred on (x, y). The
// corners breathe with the cursor layout's own loopCursor animation (65 frames), as on the game's menus.
void Cursor(const Canvas& c, float x, float y, float w, float h, int frame) {
    if (g_deferredCursor.on) {
        g_deferredCursor = {true, true, c, x, y, w, h, frame};
        return;
    }
    Assets& a = GetAssets();
    struct Corner { const char* pane; float ax, ay, sx, sy; };
    static constexpr Corner kCorners[] = {
        {"N_LU_00", -67, 42, -1, 1}, {"N_RU_00", 67, 42, 1, 1}, {"N_LD_00", -67, -42, -1, -1}, {"N_RD_00", 67, -42, 1, -1}};
    GameLayout::DrawParams loop;
    if (a.cursorLoop.Loaded()) {
        a.cursorLoop.Apply(std::fmod(static_cast<float>(ImGui::GetTime()) * 60.0f, a.cursorLoop.Frames()), a.cursor, loop);
    }
    const float out = a.cursorLoop.Loaded() ? 0.0f : 3.0f * Pulse(frame);
    for (const auto& corner : kCorners) {
        GameLayout::DrawParams p = loop;
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
    float authoredX = 0.0f, authoredY = -118.0f, authoredW = 240.0f, authoredH = 53.0f;
    a.yesNo.PaneRect("P_centerBase_00", authoredX, authoredY, authoredW, authoredH);
    p.offsetX = x - authoredX;
    p.offsetY = y - authoredY;
    p.scale = fixedScale > 0.0f ? fixedScale : selected ? 1.0f + 0.04f * Pulse(frame) : 0.94f;
    p.paneScale["P_centerBase_00"] = 1.0f;  // authored at 0: it grows in through an animation
    p.paneSize["P_centerBase_00"] = {w, h};
    p.paneSize["W_button_02"] = {w + 5.0f, h + 10.0f};
    p.paneSize["P_shadow_02"] = {w, h};
    a.yesNo.StretchSlices(p, "SingleBtnLeft", "SingleBtnMiddle", "SingleBtnRight", w - authoredW, h - authoredH);
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
    const std::string shown = UiText::Tr(label);
    float size = textSize * scale;
    if (GameLayout::FontReady(a.message.get()) && !shown.empty()) {
        const float room = (w - 22.0f) * scale, width = GameLayout::MeasureText(*a.message, Utf16(shown), size);
        if (width > room) {
            size *= room / width;
        }
    }
    Text(t, shown, x, y, size);
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

// ---------------------------------------------------------------- the pause window's look
// The Settings window is the pause window's: its red-framed dark panel, its green buttons with their
// on / off / hit animations, the cursor's corners and the game's own menu sounds.
//
// Button animations: the button the cursor is on plays onButton and holds its last frame (grown, the
// "on" colours); the one it left plays offButon; a press plays hitButton. Keys tell buttons apart.
struct ButtonAnims {
    int current = -1;
    int previous = -1;
    double changed = -10.0;
    int hit = -1;
    double hitAt = -10.0;
};
ButtonAnims g_buttonAnims;

void HitButton(int key) {
    g_buttonAnims.hit = key;
    g_buttonAnims.hitAt = ImGui::GetTime();
}

// The sounds the game's menus make (settings_overlay::PlayGameSound; couch play only).
void MenuSound(uint32_t id) {
    settings_overlay::PlayGameSound(id);
}

bool PauseLook() {
    return GetAssets().pause.Loaded();
}

// A button of the pause window's, w x h units, centred on (x, y). selected: the cursor is on it (it
// plays the game's on animation, the cursor's corners round it); lit: shown in the on colours without
// the cursor (the open tab); grow: how much of the on animation's growth it takes (rows packed close
// take less). Without the pause layout, the yes/no window's button.
void PauseButton(const Canvas& c, int key, const std::string& label, float x, float y, float w, float h,
                 bool selected, bool enabled, float textSize = 28.0f, bool lit = false, float grow = 1.0f,
                 const std::array<float, 9>* recolor = nullptr, bool cursor = true) {
    Assets& a = GetAssets();
    if (!a.pause.Loaded()) {
        Button(c, label, x, y, w, selected && cursor, enabled, g_ui.frame, textSize, h, grow <= 0.0f ? 1.0f : 0.0f, recolor);
        return;
    }
    ButtonAnims& s = g_buttonAnims;
    const double now = ImGui::GetTime();
    if (selected && s.current != key) {
        s.previous = s.current;
        s.current = key;
        s.changed = now;
    }
    GameLayout::DrawParams p;
    p.alpha = c.alpha;
    // The state: the frame of which animation.
    const GameLayout::Animation* anim = &a.offButton;
    float frame = a.offButton.Frames();
    if (selected && key == s.current) {
        if (s.hit == key && (now - s.hitAt) * 60.0 < a.hitButton.Frames()) {
            anim = &a.hitButton;
            frame = static_cast<float>((now - s.hitAt) * 60.0);
        } else {
            anim = &a.onButton;
            frame = std::min(static_cast<float>((now - s.changed) * 60.0), a.onButton.Frames());
        }
    } else if (lit) {
        anim = &a.onButton;
        frame = a.onButton.Frames();
    } else if (key == s.previous) {
        frame = std::min(static_cast<float>((now - s.changed) * 60.0), a.offButton.Frames());
    }
    static const std::vector<std::pair<std::string, std::string>> kButton = {
        {"P_SBBase_00", "P_SBBase_00"}, {"P_shadow_00", "P_shadow_00"}, {"T_tuzukeru_01", "T_tuzukeru_01"}};
    if (anim->Loaded()) {
        anim->Apply(frame, a.pause, p, kButton);
    }
    float scale = 1.0f;
    if (const auto it = p.paneScale.find("P_SBBase_00"); it != p.paneScale.end()) {
        scale = 1.0f + (it->second - 1.0f) * grow;
        it->second = scale;
    }
    float ax = 0, ay = 0, aw = 313, ah = 53;
    a.pause.PaneRect("P_SBBase_00", ax, ay, aw, ah);
    float tx = 0, ty = 0, sx = 1, sy = 1;
    a.pause.PaneTransform("P_SBBase_00", tx, ty, sx, sy);
    const float baseW = sx != 0 ? aw / sx : aw, baseH = sy != 0 ? ah / sy : ah;
    p.offsetX = x - tx;
    p.offsetY = y - ty;
    p.paneSize["P_SBBase_00"] = {w, h};
    p.paneSize["W_SButton_0"] = {w + 7.0f, h + 10.0f};
    p.paneSize["P_shadow_00"] = {w - 1.0f, h + 2.0f};
    p.paneSize["T_tuzukeru_00"] = {w - 16.0f, h};
    p.paneSize["T_tuzukeru_01"] = {w - 16.0f, h};
    if (a.pauseSlices) {
        a.pause.StretchSlices(p, "Btn1_Left", "Btn1_Middle", "Btn1_Right", w - baseW, (h - baseH) * 76.0f / 53.0f);
    }
    const std::u16string text = Utf16(UiText::Tr(label));
    p.plainText = true;
    p.text["T_tuzukeru_00"] = text;
    p.text["T_tuzukeru_01"] = text;
    float size = textSize;
    if (GameLayout::FontReady(a.message.get()) && !text.empty()) {
        // The button's text pane (T_tuzukeru) puts 2 units between letters, whatever the size, on top
        // of what MeasureText counts: only the letters shrink.
        const float room = w - 24.0f, width = GameLayout::MeasureText(*a.message, text, size);
        const float spacing = 2.0f * static_cast<float>(text.size() - 1);
        if (width + spacing > room && width > 0.0f) {
            size *= std::max(0.3f, (room - spacing) / width);
        }
    }
    p.textScale["T_tuzukeru_00"] = size / 32.0f;
    p.textScale["T_tuzukeru_01"] = size / 32.0f;
    // A short button (the Video tab's packed rows) keeps the art's white lower edge at its full
    // height, so letters centred on the button sit on it and read as cut off: they move up by a share
    // of the height the button lost.
    if (h < 34.0f) {
        const float lift = (baseH - h) * 0.12f;
        p.paneOffset["T_tuzukeru_00"] = {0.0f, lift};
        p.paneOffset["T_tuzukeru_01"] = {0.0f, lift};
    }
    if (!enabled) {
        p.alpha *= 0.6f;
        p.vertexTint["P_SBBase_00"] = 0x9A9A9AFFu;
        for (const char* slice : {"Btn1_Left", "Btn1_Middle", "Btn1_Right"}) p.vertexTint[slice] = 0x9A9A9AFFu;
    }
    if (recolor != nullptr) {
        p.colorMatrix["P_SBBase_00"] = *recolor;
        for (const char* slice : {"Btn1_Left", "Btn1_Middle", "Btn1_Right"}) p.colorMatrix[slice] = *recolor;
    }
    a.pause.Draw(c.list, c.view, "P_SBBase_00", p);
    if (selected && cursor) {
        Cursor(c, x, y, w * scale + 6, h * scale + 6, g_ui.frame);
    }
}

// The Settings window's panel, w x h units, centred: the yes/no window's cream fill and stripes, which
// are opaque, so nothing of the game's own menus under it shows through, inside the pause window's red
// rounded frame (on Newer, with its three background slices). Without the yes/no layout, the pause
// window's own dark fill.
void SettingsPanel(const Canvas& c, float w, float h) {
    Assets& a = GetAssets();
    const bool cream = a.yesNo.Loaded();
    if (cream) {
        GameLayout::DrawParams p;
        for (const char* hidden : {"T_questionS_00", "N_button_00", "N_otehonText_00", "W_yesnoWindow_00"}) {
            p.visible[hidden] = false;
        }
        p.paneSize["P_bg_00"] = {w, h};
        p.paneSize["P_stripe_00"] = {w, h};
        a.yesNo.StretchSlices(p, "WindowLeft", "WindowCentre", "WindowRight", w - 528.0f, h - 322.0f);
        a.yesNo.Draw(c.list, c.view, "N_yesnoWindow_00", p);
    }
    GameLayout::DrawParams p;
    p.alpha = c.alpha;
    for (const char* hidden : {"N_pauseMenu_01", "N_worldText_00", "N_osusumeText_00", "N_multiText_00",
                               "LevelNumShadow", "LevelNameShadow"}) {
        p.visible[hidden] = false;
    }
    p.visible["P_bg_00"] = !cream;
    float fx = 0, fy = 0, fw = 398, fh = 229;
    a.pause.PaneRect("W_N_pauseMenu_00", fx, fy, fw, fh);
    p.paneSize["P_bg_00"] = {w, h};
    p.paneSize["W_N_pauseMenu_00"] = {w + 12.0f, h + 14.0f};
    a.pause.StretchSlices(p, "BG_Left", "BG_Middle", "BG_Right", w + 12.0f - fw, h + 14.0f - fh);
    a.pause.Draw(c.list, c.view, "N_pauseMenu_00", p);
}

// A thin line across the window (under the tabs, over the prompts), in the yes/no window's own line
// colour (P_stechLine_00) on its cream.
void Rule(const Canvas& c, float x0, float x1, float y) {
    c.list->AddLine(ImVec2(c.view.centerX + x0 * c.view.scale, c.view.centerY - y * c.view.scale),
                    ImVec2(c.view.centerX + x1 * c.view.scale, c.view.centerY - y * c.view.scale),
                    IM_COL32(0xDF, 0xA9, 0x5D, static_cast<int>(220 * c.alpha)), std::max(1.0f, 2.0f * c.view.scale));
}

// The Mario font's "<" and ">" centred on x0 and x1, in the world numbers' yellow to green.
void PickerArrows(const Canvas& c, float x0, float x1, float y, bool selected, bool enabled = true) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.mario.get())) {
        return;
    }
    const float bump = selected ? 2.0f * Pulse(g_ui.frame) : 0.0f;
    const float alpha = c.alpha * (enabled ? 1.0f : 0.45f);
    GameLayout::DrawText(c.list, c.view, *a.mario, u"<", x0 - bump, y, 34, 0xFFF000FFu, 0x5AC832FFu, alpha, 1, 1);
    GameLayout::DrawText(c.list, c.view, *a.mario, u">", x1 + bump, y, 34, 0xFFF000FFu, 0x5AC832FFu, alpha, 1, 1);
}

// The Settings rows' controls all fill one column, kColX0 to kColX1: an On / Off pair, a picker (its
// value between its two arrows), a single button, a slider, or Performance Monitor's button and
// slider side by side. No button grows when chosen, highlighted or pressed (grow 0): only its colours
// change, and the cursor's corners go round the whole column.
constexpr float kColX0 = -10.0f, kColX1 = 290.0f;
constexpr float kColW = kColX1 - kColX0, kColX = (kColX0 + kColX1) / 2;
constexpr float kPairGap = 8.0f, kPairW = (kColW - kPairGap) / 2;
constexpr float kArrowW = 28.0f;  // a picker's arrow, at each end of the column
constexpr float kPerfButtonW = 92.0f;  // Performance Monitor's On / Off, at the column's left
constexpr uint32_t kLabelDim = 0xD8E8D0FFu;  // a row's label while the cursor is elsewhere

void ColumnCursor(const Canvas& c, float y, float h) {
    Cursor(c, kColX, y, kColW + 6, h + 6, g_ui.frame);
}

// On / Off as a pair of the pause window's buttons across the column: the one in force lit, the other greyed.
void OnOff(const Canvas& c, int key, float y, float h, bool on, bool selected) {
    PauseButton(c, key * 2 + 100000, "On", kColX0 + kPairW / 2, y, kPairW, h, false, on, 24.0f, on, 0.0f, nullptr,
                false);
    PauseButton(c, key * 2 + 100001, "Off", kColX1 - kPairW / 2, y, kPairW, h, false, !on, 24.0f, !on, 0.0f, nullptr,
                false);
    if (selected) {
        ColumnCursor(c, y, h);
    }
}

// A picker across the column: its value on a button, the arrows either side.
void Picker(const Canvas& c, int key, const std::string& label, float y, float h, bool selected, float textSize) {
    PauseButton(c, key, label, kColX, y, kColW - 2 * kArrowW, h, selected, true, textSize, false, 0.0f, nullptr, false);
    // The arrows' outer edges at the column's ends (each glyph is about 19 units across; selected, they
    // bob 2 units outward).
    PickerArrows(c, kColX0 + 10.0f, kColX1 - 10.0f, y, selected);
    if (selected) {
        ColumnCursor(c, y, h);
    }
}

// A slider drawn from the pause window's button: the button's frame round a dark track, filled from
// the left in the button's own on colours, `percent` full.
void SliderBar(const Canvas& c, float x0, float x1, float y, float h, int percent, bool selected) {
    Assets& a = GetAssets();
    if (!a.pause.Loaded()) {
        return;
    }
    float tx = 0, ty = 0, sx = 1, sy = 1;
    a.pause.PaneTransform("P_SBBase_00", tx, ty, sx, sy);
    const float w = x1 - x0, cx = (x0 + x1) / 2;
    GameLayout::DrawParams p;
    p.alpha = c.alpha;
    p.paneScale["P_SBBase_00"] = 1.0f;
    for (const char* hidden : {"W_SButton_0", "P_shadow_00", "T_tuzukeru_00", "T_tuzukeru_01", "Btn1_Left",
                               "Btn1_Middle", "Btn1_Right"}) {
        p.visible[hidden] = false;
    }
    // The track.
    p.offsetX = cx - tx;
    p.offsetY = y - ty;
    p.paneSize["P_SBBase_00"] = {w, h};
    p.vertexColors["P_SBBase_00"] = {0x101810E0u, 0x101810E0u, 0x304030E0u, 0x304030E0u};
    a.pause.Draw(c.list, c.view, "P_SBBase_00", p);
    // The fill.
    const float fill = w * std::clamp(percent, 0, 100) / 100.0f;
    if (fill > 0.5f) {
        p.offsetX = x0 + fill / 2 - tx;
        p.paneSize["P_SBBase_00"] = {fill, h};
        p.vertexColors["P_SBBase_00"] = selected ? std::array<uint32_t, 4>{0x009646FFu, 0x009646FFu, 0xF0FF32FFu, 0xF0FF32FFu}
                                                 : std::array<uint32_t, 4>{0x8CDC5AFFu, 0x8CDC5AFFu, 0xFFFA80FFu, 0xFFFA80FFu};
        a.pause.Draw(c.list, c.view, "P_SBBase_00", p);
    }
    // The frame.
    float wx = 0, wy = 0, ww = 0, wh = 0;
    if (a.pause.PaneRect("W_SButton_0", wx, wy, ww, wh)) {
        GameLayout::DrawParams f;
        f.alpha = c.alpha;
        f.offsetX = cx - wx;
        f.offsetY = y - wy;
        f.paneSize["W_SButton_0"] = {w + 7.0f, h + 10.0f};
        a.pause.Draw(c.list, c.view, "W_SButton_0", f);
    }
}

// The window's prompt line, bottom right: each item the button the game's prompts show for it now
// (a Wii Remote button, the player's key or controller button) and its word. Tapping Back closes.
struct Prompt {
    KeyCaps::Button button;
    const char* word;
    uint32_t event;
};
void PromptLine(const Canvas& c, float right, float y, std::initializer_list<Prompt> prompts) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.message.get())) {
        return;
    }
    constexpr float kSize = 24.0f, kIcon = 30.0f, kGap = 22.0f;
    // Laid out from the right.
    std::vector<std::pair<const Prompt*, float>> items;
    float total = 0;
    for (const Prompt& prompt : prompts) {
        const float word = GameLayout::MeasureText(*a.message, Utf16(UiText::Tr(prompt.word)), kSize);
        const float icon = KeyCaps::Draw(nullptr, 0, 0, kIcon * c.view.scale, prompt.button, 1.0f) / c.view.scale;
        items.emplace_back(&prompt, icon + 4.0f + word);
        total += icon + 4.0f + word + kGap;
    }
    float x = right - total + kGap;
    for (const auto& [prompt, width] : items) {
        const ImVec2 at(c.view.centerX + x * c.view.scale, c.view.centerY - y * c.view.scale);
        const float icon = KeyCaps::Draw(c.list, at.x, at.y, kIcon * c.view.scale, prompt->button, c.alpha) / c.view.scale;
        Text(c, prompt->word, x + icon + 4.0f, y, kSize, 0);
        if (prompt->event != 0) {
            AddHitEvent(x + width / 2, y, width + 16.0f, 40.0f, -1, prompt->event);
        }
        x += width + kGap;
    }
}

std::string PlayerLine(uint8_t slot, const std::string& name, bool you) {
    return std::to_string(slot + 1) + "P  " + name + (you ? UiText::Tr("  (you)") : "");
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

// The file select's Back button is for a touch screen or a mouse: on a phone from the start, on a PC once
// the mouse moves or clicks; a press on a remote, controller or the keyboard takes it away again.
#if defined(__ANDROID__)
bool g_pointerInUse = true;
#else
bool g_pointerInUse = false;
#endif
constexpr float kFileBackY = -192.0f, kFileBackW = 130.0f, kFileBackH = 40.0f;

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
constexpr float kPauseSettingsY = -24.0f;  // window space: one pitch (70) under Continue at 11 + 35 (PauseSettingsY)
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
    // LevelNumShadow and LevelNameShadow are Newer's own course title lines.
    static const std::map<std::string, float> kShifts = {
        {"N_worldText_00", kPauseRaise}, {"N_osusumeText_00", kPauseRaise}, {"N_multiText_00", kPauseRaise},
        {"P_SBBase_00", kPauseRaise},    {"N_posMulti_UP", kPauseRaise},
        {"P_SBBase_02", kPauseLower},    {"N_posMulti_Down", kPauseLower},
        {"LevelNumShadow", kPauseRaise}, {"LevelNameShadow", kPauseRaise},
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
        // Newer draws the window's background as three slices (BG_Left/Middle/Right) instead of P_bg_00.
        ApplyLayoutEdit(g_pauseLayout, PauseShift, {"W_N_pauseMenu_00", "P_bg_00", "BG_Left", "BG_Middle", "BG_Right"},
                        kPauseGrow);
    }
}

// Window space, halfway between Continue and Exit Course once they have moved apart: -24 on the
// base game's window (Continue at -22 + 33 + 35), -31 on Newer's, whose buttons sit further apart.
float PauseSettingsY() {
    const auto& panes = g_pauseLayout.panes;
    const auto first = panes.find("P_SBBase_00"), second = panes.find("P_SBBase_02");
    const auto row = panes.find("N_pauseMenu_01");
    if (first == panes.end() || second == panes.end() || row == panes.end()) {
        return kPauseSettingsY;
    }
    return row->second.y + (first->second.y + kPauseRaise + second->second.y + kPauseLower) / 2;
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

// Draws a "Settings" button at y (window space) of a menu window whose layout couldn't be given one of
// its own (hle/storage/layout_patch.h): the pause window's button with its animations, the size of the
// world map menu's buttons, or with `match` of the pause window's.
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
        // The world map's menu: the game's button (its corseSelectMenu buttons are 302 x 53).
        PauseButton(c, 901, "Settings", 0, y, 302, 53, selected, true, 30.0f);
        return;
    }
    PauseButton(c, 902, "Settings", 0, y, 313, 53, selected, true, 30.0f);
}

void DrawMenuSettings(ImDrawList* list) {
    DrawWindowSettings(list, MenuWindowTransform, kMenuSettingsY, g_menuFocus.load());
}

void DrawPauseSettings(ImDrawList* list) {
    static const MatchButtons kPauseButtons{&g_pauseLayout, "P_SBBase_00", "P_SBBase_02", &g_pauseOnScale[0]};
    DrawWindowSettings(list, PauseWindowTransform, PauseSettingsY(), g_pauseFocus.load(), &kPauseButtons);
}

// Which of the pause window's buttons the game's own cursor is on (0 Continue, 1 Exit Course), from
// where its corner brackets are: above or below Settings. -1 without a cursor.
int PauseGameCursor() {
    float cx = 0, cy = 0, scale = 1, alpha = 1;
    const CursorRect r = GameCursorRect();
    if (!r.valid || !PauseWindowTransform(cx, cy, scale, alpha)) {
        return -1;
    }
    return (r.y0 + r.y1) / 2 > cy + PauseSettingsY() * scale ? 0 : 1;
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
        label = update.progress >= 0 ? UiText::Tr("Updating ") + std::to_string(static_cast<int>(update.progress * 100)) + "%"
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
    const bool lan = g_ui.screen != Screen::None && g_ui.screen != Screen::Settings &&
                     !(g_ui.screen == Screen::Message && g_ui.afterMessage == Screen::None);
    Text(c, "Play:", kToggleX - kToggleW / 2 - 12, kToggleY, 24, 2, 0xFFFFFFFFu);
    Button(c, lan ? "LAN" : "Couch", kToggleX, kToggleY, kToggleW, g_ui.toggleFocus && g_ui.topItem == 0, true,
           g_ui.frame);
    // Settings: the pause window's own button (as in the pause window and the world map's menu), with a cog.
    PauseButton(c, 900, "", kGearX, kGearY, kGearW, 50, g_ui.toggleFocus && g_ui.topItem == 1, true);
    Gear(c, kGearX, kGearY, 13.0f);
}

// Where the file select's Back goes: the bottom right of the picture, under the buttons there.
float FileBackX() {
    const GameLayout::View view = GameLayout::GameView();
    const int aspect = settings_overlay::Aspect();
    const float half = ImGui::GetIO().DisplaySize.x / 2 / view.scale;
    const float picture = aspect == DisplaySettings::kAspect4x3     ? 304.0f
                          : aspect == DisplaySettings::kAspect16x10 ? 365.0f
                          : aspect == DisplaySettings::kAspect16x9  ? 405.0f
                                                                    : half;
    return std::min(half, picture) - kFileBackW / 2 - 8.0f;
}

void DrawFileBack(const Canvas& c) {
    if (g_pointerInUse && GameMenus::FileSelectWaiting()) {
        Button(c, "Back", FileBackX(), kFileBackY, kFileBackW, false, true, g_ui.frame, 26.0f, kFileBackH);
    }
}

// ---------------------------------------------------------------- settings
// Two tabs. Video: "Nx" renders N x 480 lines in a 16:9 picture (black bars beside it on a wider
// screen), "Nx Ultrawide" (offered on screens wider than 16:9) the same lines across the whole
// screen with the game's view widened, and Native (the last choice) the screen's own pixels across
// the whole screen. Keybinds: the keyboard's remote.
constexpr float kScales[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
std::atomic<bool> g_settingsChanged{false};
std::atomic<bool> g_touchControlsToggled{false};  // the Keybinds tab's Touch Controls, since the last take  // the Android app copies them into its own settings
// Mods, the last tab, only on the original game (GameFolder::RetailDisc): a modded disc has none.
enum SettingsTab { kTabVideo, kTabAudio, kTabKeybinds, kTabGameplay, kTabLanguage, kTabMods };
int g_settingsTab = kTabVideo;
constexpr int kMaxSettingsTabs = 6;

int SettingsTabs() {
    return GameFolder::RetailDisc() ? kMaxSettingsTabs : kMaxSettingsTabs - 1;
}

// Cursor rows. Video: 0 tabs, 1 resolution (Native the last choice), 2 fullscreen, then renderer,
// V-Sync, Performance Monitor (FPS counter and frametime graph), spinning coins, depth blur and
// Battery Saver, one a row.
// Keybinds: 0 tabs, 1-12
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
constexpr int kVideoFullscreen = kFullscreenRow ? 2 : -1;
constexpr int kVideoRenderer = kFullscreenRow ? 3 : 2;
constexpr int kVideoVSync = kVSyncRow ? kVideoRenderer + 1 : -1;
constexpr int kVideoGraph = (kVSyncRow ? kVideoVSync : kVideoRenderer) + 1;
constexpr int kVideoCoins = kVideoGraph + 1;
constexpr int kVideoDepthBlur = kVideoCoins + 1;
constexpr int kVideoBatterySaver = kVideoDepthBlur + 1;
constexpr int kVideoLast = kVideoBatterySaver;  // the window closes from its prompt line, not an OK button
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
constexpr int kKeyReset = kKeyThird + 1;
constexpr int kKeyToggles[] = {kKeySmartSpin, kKeyReverseTilt, kKeyThird};
constexpr int kKeyToggleCount = 3;

struct VideoChoice {
    float scale;
    bool wide;
    bool sixteenTen = false;
};

bool CurrentWide() {
    return settings_overlay::Aspect() == DisplaySettings::kAspectFill;
}

bool CurrentSixteenTen() {
    return settings_overlay::Aspect() == DisplaySettings::kAspect16x10;
}

// The multipliers, each plain, 16:10 and (on a wide screen) Ultrawide, then Native: scale 0, the
// screen's own resolution across the whole screen.
std::vector<VideoChoice> VideoChoices() {
    std::vector<VideoChoice> choices;
    for (const float scale : kScales) {
        choices.push_back({scale, false});
        choices.push_back({scale, false, true});
        if (DisplaySettings::SurfaceWide()) {
            choices.push_back({scale, true});
        }
    }
    choices.push_back({0.0f, true});
    return choices;
}

int CurrentChoice(const std::vector<VideoChoice>& choices) {
    const float current = settings_overlay::RenderScale();
    if (current <= 0.0f) {
        return static_cast<int>(choices.size()) - 1;  // Native
    }
    const float scale = current;
    const bool wide = CurrentWide() && DisplaySettings::SurfaceWide(), sixteenTen = CurrentSixteenTen();
    int best = -1;
    for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
        if (std::fabs(choices[i].scale - scale) < 0.01f && choices[i].wide == wide &&
            choices[i].sixteenTen == sixteenTen) {
            best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            if (std::fabs(choices[i].scale - 1.0f) < 0.01f && choices[i].wide == wide &&
                choices[i].sixteenTen == sixteenTen) {
                best = i;
            }
        }
    }
    return std::max(best, 0);
}

std::string ChoiceLabel(const VideoChoice& choice) {
    char text[40];
    if (choice.scale <= 0.0f) {
        return "Native";
    }
    if (choice.wide) {
        std::snprintf(text, sizeof(text), "%gx %s", choice.scale, UiText::Tr("Ultrawide").c_str());
    } else if (choice.sixteenTen) {
        std::snprintf(text, sizeof(text), "%gx 16:10", choice.scale);
    } else {
        std::snprintf(text, sizeof(text), "%gx (%dp)", choice.scale,
                      static_cast<int>(std::lround(480.0f * choice.scale)));
    }
    return text;
}

void ApplyVideo(float scale, bool wide, bool sixteenTen) {
    if (settings_overlay::Aspect() != DisplaySettings::kAspect4x3) {
        settings_overlay::SetAspect(sixteenTen ? DisplaySettings::kAspect16x10
                                    : wide     ? DisplaySettings::kAspectFill
                                               : DisplaySettings::kAspect16x9);
    }
    settings_overlay::SetRenderScale(scale);
    g_settingsChanged = true;
}

void StepVideo(int direction, bool wrap) {
    const std::vector<VideoChoice> choices = VideoChoices();
    const int count = static_cast<int>(choices.size());
    int next = CurrentChoice(choices) + direction;
    if (next >= count) {
        next = wrap ? 0 : count - 1;
    } else if (next < 0) {
        next = wrap ? count - 1 : 0;
    }
    ApplyVideo(choices[next].scale, choices[next].wide, choices[next].sixteenTen);
}

// The highlighted file is the one being played once it's picked (gameplay_options.cpp).
void TrackActiveFile() {
    if (g_ui.screen != Screen::None || !g_ui.toggleShown || g_ui.toggleFocus) {
        return;
    }
    const int file = GameMenus::FileSelectRawCursor();
    if (file >= 0 && file <= 2) {
        GameplayOptions::SetActiveFile(file);
    }
}

// "Cheated" on each file Infinite Lives or the Power-Up was used on (RuntimeConfigFile::FileCheated),
// at its card's top right: the file anchors N_posFile1_00..N_posFile3_00, in the game's layout units.
// An erased file (its game data new again: dMj2dGame_c +2 bit 0) loses the mark.
constexpr uint32_t kSaveMngInstance = 0x8042A040u, kSaveMngGames = 0x6C0, kSaveGameSize = 0x980;

bool FileAnchors(float (&x)[3], float (&y)[3]) {
    static uint32_t s_object = 0;
    static float s_x[3] = {}, s_y[3] = {};
    const uint32_t object = GameMenus::FileSelectObject();
    if (object == 0) {
        return false;
    }
    if (object != s_object) {
        const uint32_t root = FindRootPane(object);
        int found = 0;
        if (root != 0) {
            WalkPanes(root, 0, [&](uint32_t pane) {
                const std::string name = LivePaneName(pane);
                const int i = name == "N_posFile1_00" ? 0 : name == "N_posFile2_00" ? 1 : name == "N_posFile3_00" ? 2 : -1;
                if (i >= 0) {
                    s_x[i] = Memory::ReadFloat32(pane + 0x2C);
                    s_y[i] = Memory::ReadFloat32(pane + 0x30);
                    found |= 1 << i;
                }
            });
        }
        if (found != 7) {
            return false;  // not built yet: look again next time
        }
        s_object = object;
    }
    std::copy(s_x, s_x + 3, x);
    std::copy(s_y, s_y + 3, y);
    return true;
}

bool FileIsNew(int file) {
    uint32_t saves = 0;
    return Memory::TryRead32(kSaveMngInstance, saves) && saves >= 0x80000000u && saves < 0x81800000u &&
           (Memory::Read8(saves + kSaveMngGames + kSaveGameSize * static_cast<uint32_t>(file) + 2) & 1) != 0;
}

void DrawFileCheated(const Canvas& c) {
    float x[3], y[3];
    if (!FileAnchors(x, y)) {
        return;
    }
    const bool stacked = FilesStacked();
    for (int file = 0; file < 3; ++file) {
        if (!RuntimeConfigFile::FileCheated(file)) {
            continue;
        }
        if (FileIsNew(file)) {
            RuntimeConfigFile::SetFileCheated(file, false);
            continue;
        }
        // Centred above the card's "World N" in a row (the game's own); at the bar's right end where stacked.
        const float gx = x[file] + (stacked ? 210.0f : 0.0f), gy = y[file] + (stacked ? 24.0f : 76.0f);
        Text(c, "Cheated", gx * c.view.gameScaleX / c.view.scale, gy, 21, 1, 0xFF9040FFu);
    }
}

constexpr float kSettingsHeight = 428.0f, kSettingsWidth = 720.0f;
// The window, top to bottom: its title, the tabs, a rule, the rows, the note, a rule, the prompts.
constexpr float kSettingsTitleY = 184.0f, kSettingsTabsY = 143.0f, kSettingsTabsRuleY = 119.0f;
constexpr float kSettingsNoteY = -153.0f, kSettingsPromptRuleY = -170.0f, kSettingsPromptY = -191.0f;
constexpr int kTabKey = 1000;  // the tabs' button keys (PauseButton): kTabKey + tab

// The help line over the prompts: one sentence on what the highlighted option does (English, put in
// the menus' language by Text), made smaller if it would run past the window's sides.
void HelpLine(const Canvas& c, const std::string& text, uint32_t color = 0xFFFFFFFFu) {
    Assets& a = GetAssets();
    float size = 20.0f;
    if (GameLayout::FontReady(a.message.get()) && !text.empty()) {
        const float width = GameLayout::MeasureText(*a.message, Utf16(UiText::Tr(text)), size);
        const float room = kSettingsWidth - 40.0f;
        if (width > room) {
            size *= room / width;
        }
    }
    Text(c, text, 0, kSettingsNoteY, size, 1, color);
}

// A help line with a value in it: `english` translated, then its "%s" replaced by `value`.
std::string TrWith(const char* english, const std::string& value) {
    std::string text = UiText::Tr(english);
    if (const size_t at = text.find("%s"); at != std::string::npos) {
        text.replace(at, 2, value);
    }
    return text;
}

bool TouchControlsTab();

enum class CheatAsk { None, Lives, PowerUp };
extern CheatAsk g_cheatAsk;
extern int g_cheatAskReturn;

// The row the tab's cursor is on: while the cheat question is up (its Yes / No take the cursor), the row
// it was asked from.
int SettingsRow() {
    return g_cheatAsk != CheatAsk::None ? g_cheatAskReturn : g_ui.cursor;
}

// The help line while the cursor is on the tabs: what the open tab holds.
const char* TabHelp() {
    switch (g_settingsTab) {
    case kTabVideo: return "How the game is drawn and shown on your screen.";
    case kTabAudio: return "How loud the game's music and sounds are.";
    case kTabKeybinds:
        return TouchControlsTab()                         ? "The on-screen controls and how tilting works."
               : settings_overlay::ControllerConnected() ? "Which controller buttons play the Wii Remote's buttons."
                                                          : "Which keys play the Wii Remote's buttons.";
    case kTabGameplay: return "Optional helpers; LAN play always keeps the game's own rules.";
    case kTabMods: return "Mods that change the game.";
    default: return "The language of the game's text and these menus.";
    }
}

void DrawSettingsTabs(const Canvas& c) {
    const float y = kSettingsTabsY;
    const char* names[kMaxSettingsTabs] = {"Video", "Audio",
                                           settings_overlay::ControllerConnected() ? "Controller"
                                           : kTouchToggle                          ? "Controls"
                                                                                   : "Keybinds",
                                           "Gameplay", "Language", "Mods"};
    // Five tabs 128 wide, 138 apart; six (with Mods) 104 wide, 114 apart, so they fit the same span.
    const int tabs = SettingsTabs();
    const float step = tabs == kMaxSettingsTabs ? 114.0f : 138.0f;
    // Every tab's name at one size, the largest the longest fits (PauseButton's own fit, for all).
    float size = 23.0f;
    if (Assets& a = GetAssets(); GameLayout::FontReady(a.message.get())) {
        for (int tab = 0; tab < tabs; ++tab) {
            const std::u16string text = Utf16(UiText::Tr(names[tab]));
            const float room = step - 10.0f - 24.0f, width = GameLayout::MeasureText(*a.message, text, size);
            const float spacing = 2.0f * static_cast<float>(text.size() - 1);
            if (width + spacing > room && width > 0.0f) {
                size *= std::max(0.3f, (room - spacing) / width);
            }
        }
    }
    for (int tab = 0; tab < tabs; ++tab) {
        const float x = (tab - (tabs - 1) / 2.0f) * step;
        PauseButton(c, kTabKey + tab, names[tab], x, y, step - 10.0f, 40, SettingsRow() == 0 && g_settingsTab == tab, true,
                    size, g_settingsTab == tab, 0.0f);
        AddHitEvent(x, y, step - 2.0f, 52, 0, kEvTab0 << tab);
    }
    Rule(c, -kSettingsWidth / 2 + 14.0f, kSettingsWidth / 2 - 14.0f, kSettingsTabsRuleY);
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
    {"Menu Sounds", "The menus' cursor, select and back sounds.", settings_overlay::Volume::MenuSounds},
};
constexpr int kVolumeRowCount = static_cast<int>(sizeof(kVolumeRows) / sizeof(kVolumeRows[0]));
constexpr int kAudioLast = kVolumeRowCount;
constexpr float kSliderX0 = kColX0, kSliderX1 = kColX1, kSliderH = 30.0f;
int g_dragSlider = -1;  // the row a finger or the mouse is dragging, or -1

float VolumeRowY(int row) {
    return 92.0f - 56.0f * row;
}

// A number in the Mario font (a slider's value), white like the pause window's title.
void MarioNumber(const Canvas& c, const std::string& text, float x, float y, float size) {
    Assets& a = GetAssets();
    if (GameLayout::FontReady(a.mario.get())) {
        GameLayout::DrawText(c.list, c.view, *a.mario, Utf16(text), x, y, size, 0xFFFFFFFFu, 0xFFFFFFFFu, c.alpha, 1, 1);
    }
}

// The Frametime Graph row's size slider (Video tab): 5% to 200%, 50% the graph's original size.
constexpr float kGraphSliderX0 = kColX0 + kPerfButtonW, kGraphSliderX1 = kColX1;
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
        for (int row = 0; row < kVolumeRowCount; ++row) {
            const ImVec2 at = ToScreen(c, kSliderX0, VolumeRowY(row));
            if (g_pointerNow.x >= start.x - reach && g_pointerNow.x <= end.x + reach &&
                std::fabs(g_pointerNow.y - at.y) <= 27.0f * c.view.scale) {
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
    for (int row = 0; row < kVolumeRowCount; ++row) {
        const float y = VolumeRowY(row);
        const bool selected = g_ui.cursor == 1 + row;
        const int percent = settings_overlay::VolumePercent(kVolumeRows[row].which);
        Text(c, kVolumeRows[row].label, -300, y, 26, 0, selected ? 0xFFFFFFFFu : kLabelDim);
        // The slider fills the column, its percentage on it.
        SliderBar(c, kSliderX0, kSliderX1, y, kSliderH, percent, selected);
        MarioNumber(c, std::to_string(percent) + "%", kColX, y, 26);
        if (selected) {
            ColumnCursor(c, y, kSliderH);
        }
        AddHit(-10, y, 660, 56, 1 + row);
    }
    const int row = g_ui.cursor - 1;
    HelpLine(c, row >= 0 && row < kVolumeRowCount ? kVolumeRows[row].note : TabHelp());
}

// The Language tab: the USA disc's three languages, and Italian, whose game text comes from a language
// pack (RuntimeConfigFile::LanguagePackDir) over the English files. The game reads the console language
// once at boot (SCGetLanguage), so a change shows the next time it starts; these menus change at once.
// Cursor rows: 1-4, then 5 OK.
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
    {"it", "Italiano", "Riavvia il gioco per giocare in questa lingua."},
};
constexpr int kLanguageCount = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
constexpr int kLanguageLast = kLanguageCount;

void DrawLanguageTab(const Canvas& c) {
    static const std::string atBoot = RuntimeConfigFile::Language();
    const std::string chosen = RuntimeConfigFile::Language();
    for (int i = 0; i < kLanguageCount; ++i) {
        const float y = 90.0f - 52.0f * i;
        PauseButton(c, 1 + i, kLanguages[i].name, 0, y, 380, 40, g_ui.cursor == 1 + i, true, 28.0f,
                    chosen == kLanguages[i].code, 0.0f);
        AddHit(0, y, 400, 50, 1 + i);
    }
    // The highlighted language's line; on the one chosen since the game started, that a restart
    // brings it in (in that language), and on one whose game text is missing, the pack it needs.
    const int row = g_ui.cursor - 1;
    if (row < 0 || row >= kLanguageCount) {
        HelpLine(c, TabHelp());
        return;
    }
    const LanguageChoice& language = kLanguages[row];
    if (!RuntimeConfigFile::DiscLanguage(language.code) && !RuntimeConfigFile::LanguagePackInstalled(language.code)) {
        HelpLine(c, "The game's own text needs an Italian language pack.", 0xFFF0B0FFu);
    } else if (chosen == language.code && chosen != atBoot) {
        HelpLine(c, language.restartNote, 0xFFF0B0FFu);
    } else {
        HelpLine(c, TrWith("Shows the game's text and these menus in %s.", language.name));
    }
}

// The Mods tab: nothing to pick yet, only the word, centred between the tabs' rule and the help line.
// Cursor rows: 0, the tabs.
void DrawModsTab(const Canvas& c) {
    Text(c, "Coming Soon!", 0, (kSettingsTabsRuleY + kSettingsNoteY) / 2);
    HelpLine(c, TabHelp());
}

// The Gameplay tab (gameplay_options.cpp applies it): No Time Limit and Infinite Lives, and Power-Up:
// left and right pick one, a press gives it to P1 (in a course or on the world map at once; from the
// title screen or the file select once the game is on), and on Android the on-screen Cheat Button. LAN
// play keeps the game's own rules. Cursor rows: one each, then OK.
constexpr int kGameplayTime = 1, kGameplayLives = 2, kGameplayPowerUp = 3;
constexpr int kGameplayCheat = kTouchToggle ? 4 : -1, kGameplayLast = kTouchToggle ? 4 : 3;
int g_powerUpChoice = -1;    // GameplayOptions's list; from Config.toml when the tab is first drawn
int g_powerUpGivenAt = -1;   // the frame Power-Up was last pressed, for its note

// Turning Infinite Lives on and Power-Up ask first (No Time Limit doesn't): "Are you sure? This will
// mark your save as cheated.", Yes or No (the cursor: 0 or 1, No first). A file already marked isn't
// asked about again.
CheatAsk g_cheatAsk = CheatAsk::None;
int g_cheatAskReturn = 0;  // the tab's cursor row, back to it after the answer

void DoCheat(CheatAsk cheat) {
    if (cheat == CheatAsk::Lives) {
        RuntimeConfigFile::SetInfiniteLives(true);
        g_settingsChanged = true;
    } else if (cheat == CheatAsk::PowerUp) {
        GameplayOptions::GivePowerUp(g_powerUpChoice);
        g_powerUpGivenAt = g_ui.frame;
        settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
    }
    const int file = GameplayOptions::ActiveFile();
    if (file >= 0 && cheat == CheatAsk::Lives) {
        RuntimeConfigFile::SetFileCheated(file, true);  // the Power-Up marks it once it's given
    }
}

void AskCheat(CheatAsk cheat) {
    if (RuntimeConfigFile::FileCheated(GameplayOptions::ActiveFile())) {
        DoCheat(cheat);
        return;
    }
    g_cheatAsk = cheat;
    g_cheatAskReturn = g_ui.cursor;
    g_ui.cursor = 1;
}

void DrawCheatAsk(const Canvas& c) {
    g_hits.clear();  // only Yes and No take taps now
    Veil(c);
    constexpr float kHeight = 250.0f;
    Window(c, kHeight, 600.0f);
    Text(c, "Are you sure?", 0, 62, 32);
    Text(c, "This will mark your save as cheated.", 0, 16, 24);
    // Yes and No keep one size, chosen or not (ButtonPair's chosen one breathes).
    const float y = -kHeight / 2 + 43.0f;
    Button(c, "Yes", -126, y, 232, g_ui.cursor == 0, true, g_ui.frame, 30.0f, 53.0f, 1.0f);
    Button(c, "No", 126, y, 232, g_ui.cursor == 1, true, g_ui.frame, 30.0f, 53.0f, 1.0f);
    AddHit(-126, y, 240, 60, 0);
    AddHit(126, y, 240, 60, 1);
}

// The answer: true while the question is up (it takes every press).
bool HandleCheatAsk(bool left, bool right, bool confirm, bool back) {
    if (g_cheatAsk == CheatAsk::None) {
        return false;
    }
    if (left || right) {
        g_ui.cursor = g_ui.cursor == 0 ? 1 : 0;
    } else if (confirm || back) {
        const CheatAsk cheat = g_cheatAsk;
        g_cheatAsk = CheatAsk::None;
        if (confirm && g_ui.cursor == 0) {
            DoCheat(cheat);
        }
        g_ui.cursor = g_cheatAskReturn;
    }
    return true;
}

// The Gameplay tab's rows: one setting a row, between the tabs and the hint line, spaced as the Video
// tab's were before Depth Blur (Resolution, Fullscreen on the desktop, Renderer, V-Sync on Windows and
// Android, Performance Monitor and Spinning Coins). The Video tab has two rows more (Depth Blur and
// Battery Saver).
constexpr int kRows = 4 + (kFullscreenRow ? 1 : 0) + (kVSyncRow ? 1 : 0);
// A row's buttons leave room for their frames (W_SButton reaches 5 units past the button above and
// below), so one row's never touches the next's. Eight rows (the Video tab on Windows) fill the room
// between the tabs' rule and the help line.
constexpr float RowStep(int rows) { return rows >= 8 ? 32.0f : rows == 7 ? 37.0f : rows == 6 ? 42.0f : 48.0f; }
constexpr float RowH(int rows) { return RowStep(rows) - 11.0f; }
constexpr float RowsTop(int rows) { return rows >= 8 ? 100.0f : rows == 7 ? 98.0f : rows == 6 ? 96.0f : 91.0f; }
constexpr float RowLabelSize(int rows) { return rows >= 8 ? 22.0f : rows == 7 ? 24.0f : 26.0f; }
constexpr float RowButtonSize(int rows) {
    return rows >= 8 ? 20.0f : rows == 7 ? 24.0f : rows == 6 ? 27.0f : 30.0f;
}
constexpr float RowsNoteY(int rows) { return rows >= 7 ? -136.0f : rows == 6 ? -133.0f : -126.0f; }
constexpr float kRowH = RowH(kRows), kRowStep = RowStep(kRows), kRowsTop = RowsTop(kRows);
constexpr float kRowLabelSize = RowLabelSize(kRows), kRowButtonSize = RowButtonSize(kRows);
constexpr float kRowsNoteY = RowsNoteY(kRows);
constexpr int kVideoRows = kRows + 2;

void DrawGameplayTab(const Canvas& c) {
    const int row = SettingsRow();
    int index = 0;
    // A label and, beside it, a button showing the setting; a press changes it.
    // A label and, beside it, the setting as an On / Off pair; a press changes it.
    const auto labeled = [&](const char* label, bool on, int cursor) {
        const float y = kRowsTop - index++ * kRowStep;
        Text(c, label, -300, y, kRowLabelSize, 0, row == cursor ? 0xFFFFFFFFu : kLabelDim);
        OnOff(c, cursor, y, kRowH, on, row == cursor);
        AddHit(kColX, y, kColW + 6, kRowStep - 2, cursor);
        return y;
    };
    labeled("No Time Limit", RuntimeConfigFile::NoTimeLimit(), kGameplayTime);
    labeled("Infinite Lives", RuntimeConfigFile::InfiniteLives(), kGameplayLives);
    if (g_powerUpChoice < 0) {
        g_powerUpChoice = static_cast<int>(RuntimeConfigFile::PowerUp());
    }
    g_powerUpChoice = std::clamp(g_powerUpChoice, 0, GameplayOptions::PowerUpCount() - 1);
    {
        // Picked with the arrows, like Resolution; a press gives it. The longer names need a smaller size.
        const float y = kRowsTop - index++ * kRowStep;
        const bool selected = row == kGameplayPowerUp;
        const char* name = GameplayOptions::PowerUpName(g_powerUpChoice);
        Text(c, "Power-Up", -300, y, kRowLabelSize, 0, selected ? 0xFFFFFFFFu : kLabelDim);
        Picker(c, kGameplayPowerUp, name, y, kRowH, selected, std::strlen(name) > 14 ? 22.0f : kRowButtonSize);
        AddHitEvent(kColX0 + kArrowW / 2, y, kArrowW + 8, kRowStep - 2, kGameplayPowerUp, kEvLeft);
        AddHitEvent(kColX1 - kArrowW / 2, y, kArrowW + 8, kRowStep - 2, kGameplayPowerUp, kEvRight);
        AddHit(kColX, y, kColW - 2 * kArrowW - 8, kRowStep - 2, kGameplayPowerUp);
    }
    if (kGameplayCheat > 0) {
        labeled("Cheat Button", RuntimeConfigFile::CheatButton(), kGameplayCheat);
    }
    const bool pressed = g_powerUpGivenAt >= 0 && g_ui.frame - g_powerUpGivenAt < 180;
    const char* note = row == kGameplayTime    ? "The course timer stands still: take your time."
                       : row == kGameplayLives ? "A miss costs no life."
                       : row == kGameplayCheat ? "An on-screen button that gives you the Power-Up."
                       : row == kGameplayPowerUp
                           ? (GameplayOptions::PowerUpWaiting() ? "P1 gets it once the game is on."
                              : pressed                         ? "Given to P1."
                                                                : "Left and right pick one; press it to give it to P1.")
                           : TabHelp();
    HelpLine(c, note);
}

// Android, playing by touch: the Keybinds tab has nothing to bind, so it's the touch controls' tab.
bool TouchControlsTab() {
    return kTouchToggle && !settings_overlay::ControllerConnected();
}

constexpr int kTouchEdit = 1, kTouchReverseTilt = 2, kTouchSmartSpin = 3, kTouchLast = 3;

void DrawTouchControlsTab(const Canvas& c) {
    constexpr float kTop = 82.0f, kStep = 60.0f;
    // Edit Touch Layout across the label and column; the switches as the other tabs' rows.
    PauseButton(c, kTouchEdit, "Edit Touch Layout", (-300.0f + kColX1) / 2, kTop, kColX1 + 300.0f, 46,
                g_ui.cursor == kTouchEdit, true, 28.0f, false, 0.0f);
    AddHit((-300.0f + kColX1) / 2, kTop, kColX1 + 306.0f, 52, kTouchEdit);
    const char* labels[] = {"Reverse Tilt", "Smart Spin"};
    const bool on[] = {Keybinds::ReverseTilt(), Keybinds::SmartSpin()};
    for (int i = 0; i < 2; ++i) {
        const float y = kTop - (i + 1) * kStep;
        Text(c, labels[i], -300, y, 26, 0, g_ui.cursor == kTouchReverseTilt + i ? 0xFFFFFFFFu : kLabelDim);
        OnOff(c, kTouchReverseTilt + i, y, 40, on[i], g_ui.cursor == kTouchReverseTilt + i);
        AddHit(kColX, y, kColW + 6, 52, kTouchReverseTilt + i);
    }
    const char* note = g_ui.cursor == kTouchEdit          ? "Move and resize the on-screen controls."
                       : g_ui.cursor == kTouchReverseTilt ? "Tilting the phone the other way tilts the remote."
                       : g_ui.cursor == kTouchSmartSpin   ? "Pressing 2 in the air does a spin."
                                                          : TabHelp();
    HelpLine(c, note);
}

void DrawVideoTab(const Canvas& c) {
    const std::vector<VideoChoice> choices = VideoChoices();
    const VideoChoice shown = choices[CurrentChoice(choices)];
    const float step = RowStep(kVideoRows), top = RowsTop(kVideoRows), rowH = RowH(kVideoRows);
    const float labelSize = RowLabelSize(kVideoRows), buttonSize = RowButtonSize(kVideoRows);
    const uint32_t dim = kLabelDim;
    Text(c, "Resolution", -300, top, labelSize, 0, g_ui.cursor == 1 ? 0xFFFFFFFFu : dim);
    Picker(c, 1, ChoiceLabel(shown), top, rowH, g_ui.cursor == 1, buttonSize);
    AddHitEvent(kColX0 + kArrowW / 2, top, kArrowW + 8, step - 2, 1, kEvLeft);
    AddHitEvent(kColX1 - kArrowW / 2, top, kArrowW + 8, step - 2, 1, kEvRight);
    AddHitEvent(kColX, top, kColW - 2 * kArrowW - 8, step - 2, 1, kEvRight);
    int index = 1;
    // A label and, beside it, a button across the column showing the setting; a press changes it.
    const auto labeled = [&](const char* label, const std::string& value, int cursor, bool enabled) {
        const float y = top - index++ * step;
        Text(c, label, -300, y, labelSize, 0, g_ui.cursor == cursor ? 0xFFFFFFFFu : dim);
        PauseButton(c, cursor, value, kColX, y, kColW, rowH, g_ui.cursor == cursor, enabled, buttonSize, false, 0.0f);
        AddHit(kColX, y, kColW + 6, step - 2, cursor);
    };
    // The same with an On / Off pair.
    const auto toggle = [&](const char* label, bool on, int cursor) {
        const float y = top - index++ * step;
        Text(c, label, -300, y, labelSize, 0, g_ui.cursor == cursor ? 0xFFFFFFFFu : dim);
        OnOff(c, cursor, y, rowH, on, g_ui.cursor == cursor);
        AddHit(kColX, y, kColW + 6, step - 2, cursor);
    };
    if (kFullscreenRow) {
        // Off, Borderless, Exclusive: each press moves to the next, like the game's own buttons.
        labeled("Fullscreen", kFullscreenNames[std::clamp(settings_overlay::Fullscreen(), 0, 2)], kVideoFullscreen,
                true);
    }
    // Renderer: each press moves to the next (left steps back); it takes effect at the next start.
    labeled("Renderer", kRenderers[RendererIndex()].label, kVideoRenderer, kRendererCount > 1);
    if (kVSyncRow) {
        toggle("V-Sync", settings_overlay::VSync(), kVideoVSync);
    }
    {
        // Performance Monitor (the FPS counter and frametime graph): the button turns it on and off;
        // the slider beside it sets its size (a finger or the mouse drags it; left and right move it
        // 5% at a time).
        const float y = top - index++ * step;
        const bool selected = g_ui.cursor == kVideoGraph;
        Text(c, "Performance Monitor", -300, y, labelSize * 0.92f, 0, selected ? 0xFFFFFFFFu : dim);
        // On / Off for the monitor at the column's left, then its size: together they fill the column.
        PauseButton(c, kVideoGraph, settings_overlay::FrameGraph() ? "On" : "Off", kColX0 + kPerfButtonW / 2, y,
                    kPerfButtonW, rowH, selected, true, buttonSize, settings_overlay::FrameGraph(), 0.0f, nullptr, false);
        AddHit(kColX0 + kPerfButtonW / 2, y, kPerfButtonW, step - 2, kVideoGraph);
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
        SliderBar(c, kGraphSliderX0, kGraphSliderX1, y, rowH, GraphSizeToFill(size), selected);
        MarioNumber(c, std::to_string(size) + "%", (kGraphSliderX0 + kGraphSliderX1) / 2, y, labelSize);
        if (selected) {
            ColumnCursor(c, y, rowH);
        }
    }
    toggle("Spinning Coins", settings_overlay::SpinningCoins(), kVideoCoins);
    toggle("Depth Blur", settings_overlay::DepthBlur(), kVideoDepthBlur);
    toggle("Battery Saver", settings_overlay::BatterySaver(), kVideoBatterySaver);
    const bool match = shown.scale <= 0.0f;
    const char* hint = match ? "The screen's own resolution, filling the whole screen."
                       : shown.sixteenTen ? "For a 16:10 screen: fills it, with a slightly narrower view."
                       : !DisplaySettings::SurfaceWide() ? "Rendered at this many lines, then scaled to the screen."
                       : shown.wide                      ? "Fills the whole screen with a wider view."
                                                         : "16:9, with black bars at the sides.";
    const int fullscreen = settings_overlay::Fullscreen();
    const char* fullscreenNote = fullscreen == 1 ? "Borderless: a window covering the whole screen (F11)."
                                 : fullscreen == 2 ? "Exclusive: the game takes over the display."
                                                   : "Off: the game plays in a window.";
    const std::string rendererNote =
        TrWith(RendererIndex() == 0 ? "Picks the best graphics API this device has, from the next start (now %s)."
                                    : "Draws with this graphics API from the next start (now %s).",
               settings_overlay::RunningGraphicsApi());
    const char* note = g_ui.cursor == 0                ? TabHelp()
                       : g_ui.cursor == kVideoFullscreen ? fullscreenNote
                       : g_ui.cursor == kVideoRenderer ? rendererNote.c_str()
                       : g_ui.cursor == kVideoGraph ? "Shows the FPS and each frame's time at the top right; left and right set its size."
                       : g_ui.cursor == kVideoCoins ? "Animates coins and blocks; off saves a little GPU work."
                       : g_ui.cursor == kVideoDepthBlur ? "The blur and heat haze of desert, beach, water and lava areas."
                       : g_ui.cursor == kVideoVSync ? "No tearing: frames wait for the display, the game keeps its pace."
                       : g_ui.cursor == kVideoBatterySaver ? "Runs at 30FPS instead of 60FPS; pauses during LAN Play."
                                                    : hint;
    HelpLine(c, note);
}

// Bindings as the keys' caps (KeyCaps::DrawKey), centred on (cx, cy) within maxW units: a binding's keys
// joined by "+", the bindings by "/". Where they don't fit the caps get smaller (down to 70%), then the
// last bindings are left out for "...".
void KeyCapRow(const Canvas& c, const std::vector<std::vector<std::string>>& bindings, float cx, float cy, float maxW,
               float capH) {
    Assets& a = GetAssets();
    if (!GameLayout::FontReady(a.message.get()) || bindings.empty()) {
        return;
    }
    const float s = c.view.scale;
    float h = capH;
    const auto textSize = [&] { return h * 0.85f; };
    const auto plusW = [&] { return GameLayout::MeasureText(*a.message, u"+", textSize()) + 4.0f; };
    const auto slashW = [&] { return GameLayout::MeasureText(*a.message, u"/", textSize()) + 10.0f; };
    const auto measure = [&](size_t count) {
        float total = 0;
        for (size_t i = 0; i < count; ++i) {
            total += i > 0 ? slashW() : 0.0f;
            for (size_t k = 0; k < bindings[i].size(); ++k) {
                total += (k > 0 ? plusW() : 0.0f) + KeyCaps::DrawKey(nullptr, 0, 0, h * s, bindings[i][k], 1.0f) / s;
            }
        }
        return total;
    };
    size_t count = bindings.size();
    float width = measure(count);
    if (width > maxW) {
        h = std::max(capH * 0.7f, capH * maxW / width);
        width = measure(count);
    }
    const float more = GameLayout::MeasureText(*a.message, u"...", textSize()) + 6.0f;
    bool cut = false;
    while (width > maxW && count > 1) {
        --count;
        cut = true;
        width = measure(count) + more;
    }
    float x = cx - width / 2;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            Text(c, "/", x + slashW() / 2, cy, textSize());
            x += slashW();
        }
        for (size_t k = 0; k < bindings[i].size(); ++k) {
            if (k > 0) {
                Text(c, "+", x + plusW() / 2, cy, textSize());
                x += plusW();
            }
            x += KeyCaps::DrawKey(c.list, c.view.centerX + x * s, c.view.centerY - cy * s, h * s, bindings[i][k], c.alpha) / s;
        }
    }
    if (cut) {
        Text(c, "...", x + more / 2, cy, textSize());
    }
}

void DrawKeybindsTab(const Canvas& c) {
    // With a controller connected, the tab is its Controller Bindings: the same controls, on its buttons.
    const bool pad = settings_overlay::ControllerConnected();
    const int capturing = pad ? settings_overlay::ControllerCapturing() : Keybinds::Capturing();
    for (int action = 0; action < Keybinds::kActionCount; ++action) {
        const int column = action / kKeyRows, row = action % kKeyRows;
        const float x0 = column == 0 ? -178.0f : 178.0f;
        const float y = 98.0f - 35.0f * row;  // frames clear of each other
        Text(c, Keybinds::ActionName(action), x0 - 168, y, 21, 0, g_ui.cursor == 1 + action ? 0xFFFFFFFFu : kLabelDim);
        // A capture shows the keys or buttons held so far (CaptureLabel), until they are let go.
        const std::string held = capturing != action ? std::string()
                                 : pad               ? settings_overlay::ControllerCaptureLabel()
                                                     : Keybinds::CaptureLabel();
        const std::string label = capturing == action ? (!held.empty() ? held : pad ? "Press a button..." : "Press a key...")
                                  : pad               ? settings_overlay::ControllerBindingLabel(action)
                                                      : Keybinds::KeyLabel(action);
        const float size = label.size() > 20 ? 14.0f : label.size() > 16 ? 17.0f : label.size() > 11 ? 19.0f : 22.0f;
        const bool enabled = !pad || settings_overlay::ControllerActionRebindable(action);
        // The keyboard's keys as key caps (a combination's joined by "+"); a controller's buttons as words.
        std::vector<std::vector<std::string>> keys;
        if (!pad) {
            keys = capturing == action ? std::vector<std::vector<std::string>>{Keybinds::CaptureKeys()}
                                       : Keybinds::Bindings(action);
            if (!keys.empty() && keys.front().empty()) {
                keys.clear();  // a capture before its first key: its words
            }
        }
        PauseButton(c, 1 + action, keys.empty() ? label : std::string(), x0 + 78, y, 172, 25.0f, g_ui.cursor == 1 + action,
                    enabled, size, false, 0.0f);
        KeyCapRow(c, keys, x0 + 78, y, 160.0f, 21.0f);
        AddHit(x0 + 78, y, 178, 34, 1 + action);
    }
    if (capturing >= 0) {
        // One key, or 2 or 3 held together for a combination; taken when they are all let go.
        Text(c, pad ? "Hold a button, or 2 or 3 together, then let go. Esc cancels."
                    : "Hold a key, or 2 or 3 together, then let go. Esc cancels, Delete clears.",
             0, -120, 20, 1, 0xFFF0B0FFu);
    } else {
        // The QoL switches side by side.
        const char* labels[] = {"Space Smart Spin", "Reverse Tilt", kTouchToggle ? "Touch Controls" : "Mouse as Gyro"};
        const bool on[] = {Keybinds::SmartSpin(), Keybinds::ReverseTilt(),
                           kTouchToggle ? settings_overlay::TouchControls() : Keybinds::MouseGyro()};
        constexpr float width = 212.0f, gap = 232.0f;
        for (int i = 0; i < kKeyToggleCount; ++i) {
            const float x = (static_cast<float>(i) - (kKeyToggleCount - 1) * 0.5f) * gap;
            PauseButton(c, kKeyToggles[i], std::string(UiText::Tr(labels[i])) + (on[i] ? UiText::Tr(": On") : UiText::Tr(": Off")),
                        x, -120, width, 28.0f, g_ui.cursor == kKeyToggles[i], true, 20.0f, on[i], 0.0f);
            AddHit(x, -120, width + 6, 34, kKeyToggles[i]);
        }
    }
    // Reset Defaults on the prompt line's left, so the help line keeps its own place.
    constexpr float kResetW = 200.0f, kResetX = -kSettingsWidth / 2 + 22.0f + kResetW / 2;
    PauseButton(c, kKeyReset, "Reset Defaults", kResetX, kSettingsPromptY, kResetW, 30.0f, g_ui.cursor == kKeyReset, true,
                21.0f, false, 0.0f);
    AddHit(kResetX, kSettingsPromptY, kResetW + 6, 36, kKeyReset);
    if (capturing >= 0) {
        return;  // the capture's own line is up
    }
    // The highlighted control's line.
    static constexpr const char* kActionHelp[Keybinds::kActionCount] = {
        "The D-pad's up: climb vines, and go through doors and pipes.",
        "The D-pad's down: crouch, and ground-pound in the air.",
        "The D-pad's left: walk left, and move a menu's cursor.",
        "The D-pad's right: walk right, and move a menu's cursor.",
        "The 2 button: jump and swim, and choose in menus.",
        "The 1 button: run, throw fireballs, and go back in menus.",
        "The A button: view the whole world map.",
        "Shakes the Wii Remote: a spin jump, or picking up and throwing.",
        "Tilts the Wii Remote left, for the platforms that tilt.",
        "Tilts the Wii Remote right, for the platforms that tilt.",
        "The + button: pause a course, or open the world map's menu.",
        "The - button: open Select a World on the world map.",
    };
    const int cursor = g_ui.cursor;
    const char* note = cursor == 0 ? TabHelp() : "";
    if (cursor >= 1 && cursor <= Keybinds::kActionCount) {
        note = kActionHelp[cursor - 1];
    } else if (cursor == kKeySmartSpin) {
        note = "With Space as a Jump key, Space in the air does a spin.";
    } else if (cursor == kKeyReverseTilt) {
        note = "Platforms you tilt lean the way the stick or phone goes.";
    } else if (cursor == kKeyThird) {
        note = kTouchToggle ? "Shows the on-screen Wii Remote, for playing by touch."
                            : "Moving the mouse tilts the remote; moving it quickly shakes it.";
    } else if (cursor == kKeyReset) {
        note = pad ? "Puts every controller button back the way it started."
                   : "Puts every key back the way it started.";
    }
    HelpLine(c, note);
}

// The window opens and closes as the pause window does: its inWindow / outWindow animations' scale
// (N_pauseMenu_00), about its centre. Closing finishes once outWindow has played (FinishCloseSettings).
float SettingsWindowScale(bool& closed) {
    Assets& a = GetAssets();
    closed = false;
    const GameLayout::Animation& anim = g_settingsClosing ? a.outWindow : a.inWindow;
    if (!anim.Loaded()) {
        closed = g_settingsClosing;
        return 1.0f;
    }
    const double since = ImGui::GetTime() - (g_settingsClosing ? g_settingsClosingAt : g_settingsOpenedAt);
    const float frame = static_cast<float>(since * 60.0);
    if (g_settingsClosing && frame >= anim.Frames()) {
        closed = true;
        return 0.0f;
    }
    GameLayout::DrawParams p;
    anim.Apply(std::min(frame, anim.Frames()), a.pause, p, {{"N_pauseMenu_00", "N_pauseMenu_00"}});
    const auto it = p.paneScale.find("N_pauseMenu_00");
    return it != p.paneScale.end() ? std::clamp(it->second, 0.0f, 1.2f) : 1.0f;
}

void FinishCloseSettings();

void DrawSettings(const Canvas& outer) {
    Assets& a = GetAssets();
    Canvas c = outer;
    // On a screen narrower than the window (a 4:3 picture), the window is made smaller to fit across it;
    // taps on it are measured the same way (TapAt).
    g_settingsFit = std::min(1.0f, ImGui::GetIO().DisplaySize.x / ((kSettingsWidth + 24.0f) * c.view.scale));
    c.view.scale *= g_settingsFit;
    c.view.gameScaleX *= g_settingsFit;
    if (g_ui.settingsWaitGame) {
        if (!MenuSettings::PauseWindowClosed()) {
            g_settingsOpenedAt = ImGui::GetTime();  // opens once the pause window is gone
            return;
        }
        g_ui.settingsWaitGame = false;
    }
    if (a.pause.Loaded()) {
        bool closed = false;
        const float scale = SettingsWindowScale(closed);
        if (closed) {
            FinishCloseSettings();
            return;
        }
        // The game dimmed to half under the window, like the pause window's.
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float dim = g_settingsClosing ? std::min(1.0f, scale) : std::min(1.0f, outer.alpha * 1.5f);
        c.list->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, static_cast<int>(150 * dim)));
        c.alpha = 1.0f;
        c.view.scale *= scale;
        c.view.gameScaleX *= scale;
        SettingsPanel(c, kSettingsWidth, kSettingsHeight);
        if (GameLayout::FontReady(a.mario.get())) {
            GameLayout::DrawText(c.list, c.view, *a.mario, Utf16(UiText::Tr("Settings")), 0, kSettingsTitleY, 38,
                                 0xFFFFFFFFu, 0xFFFFFFFFu, c.alpha, 1, 1);
        }
        Rule(c, -kSettingsWidth / 2 + 14.0f, kSettingsWidth / 2 - 14.0f, kSettingsPromptRuleY);
        PromptLine(c, kSettingsWidth / 2 - 22.0f, kSettingsPromptY,
                   {{KeyCaps::Button::Two, "Select", 0}, {KeyCaps::Button::One, "Back", kEvBack}});
    } else {
        Veil(c);
        Window(c, kSettingsHeight, kSettingsWidth);
    }
    g_deferredCursor.on = true;
    g_deferredCursor.set = false;
    DrawSettingsTabs(c);
    if (g_settingsTab == kTabVideo) {
        DrawVideoTab(c);
    } else if (g_settingsTab == kTabAudio) {
        DrawAudioTab(c);
    } else if (g_settingsTab == kTabKeybinds && TouchControlsTab()) {
        DrawTouchControlsTab(c);
    } else if (g_settingsTab == kTabKeybinds) {
        DrawKeybindsTab(c);
    } else if (g_settingsTab == kTabGameplay) {
        DrawGameplayTab(c);
    } else if (g_settingsTab == kTabMods) {
        DrawModsTab(c);
    } else {
        DrawLanguageTab(c);
    }
    const DeferredCursor cursor = g_deferredCursor;
    g_deferredCursor.on = false;
    if (cursor.set) {
        Cursor(cursor.canvas, cursor.x, cursor.y, cursor.w, cursor.h, cursor.frame);
    }
    if (g_cheatAsk != CheatAsk::None) {
        DrawCheatAsk(c);
    }
}

void NotifySettingsClosed() {
    if (g_ui.settingsFromGameButton) {
        g_ui.settingsFromGameButton = false;
        MenuSettings::ReopenPauseWindow();
    }
    if (g_ui.settingsFromMapButton) {
        g_ui.settingsFromMapButton = false;
        MenuSettings::ReopenMapMenu();
    }
}

// Closing: the back sound, then outWindow (DrawSettings ends it with FinishCloseSettings).
void CloseSettings() {
    if (g_settingsClosing) {
        return;
    }
    MenuSound(settings_overlay::kGameSoundBack);
    g_settingsClosing = true;
    g_settingsClosingAt = ImGui::GetTime();
    if (!GetAssets().outWindow.Loaded()) {
        FinishCloseSettings();
    }
}

void FinishCloseSettings() {
    g_settingsClosing = false;
    Show(Screen::None);
    NotifySettingsClosed();
    if (g_ui.settingsFromPause) {
        g_ui.settingsFromPause = false;  // back to the pause menu, as it was
        return;
    }
    g_ui.toggleFocus = true;  // back to the file select, the cursor on the gear
    g_ui.topItem = 1;
}

void HandleSettingsInput(bool left, bool right, bool up, bool down, bool confirm, bool back);

// A choice the window can't take: the renderer with one to pick from, a controller row the right stick
// owns (the tilts).
bool SettingsChoiceInvalid() {
    const int cursor = g_ui.cursor;
    if (g_settingsTab == kTabVideo && cursor == kVideoRenderer && kRendererCount <= 1) {
        return true;
    }
    return g_settingsTab == kTabKeybinds && !TouchControlsTab() && settings_overlay::ControllerConnected() &&
           cursor >= 1 && cursor <= Keybinds::kActionCount && !settings_overlay::ControllerActionRebindable(cursor - 1);
}

// The window's sounds are the game's menus' own: a move or a value stepped, the cursor sound; another
// tab, the page sound; a press, decide (invalid where it can't be taken); closing, back (CloseSettings).
void HandleSettings(bool left, bool right, bool up, bool down, bool confirm, bool back) {
    if (g_settingsClosing || g_ui.settingsWaitGame) {
        return;
    }
    const bool capturing = Keybinds::Capturing() >= 0 || settings_overlay::ControllerCapturing() >= 0;
    const bool asking = g_cheatAsk != CheatAsk::None;
    const int tab = g_settingsTab, cursor = g_ui.cursor;
    const bool invalid = confirm && !capturing && !asking && SettingsChoiceInvalid();
    if (confirm && cursor > 0 && !capturing && !asking && !invalid) {
        HitButton(cursor);
    }
    HandleSettingsInput(left, right, up, down, confirm, back);
    if (capturing || g_settingsClosing) {
        return;
    }
    if (g_settingsTab != tab) {
        MenuSound(settings_overlay::kGameSoundPage);
    } else if (g_ui.cursor != cursor && !asking) {
        MenuSound(settings_overlay::kGameSoundCursor);
    } else if (invalid) {
        MenuSound(settings_overlay::kGameSoundInvalid);
    } else if (confirm) {
        MenuSound(settings_overlay::kGameSoundDecide);
    } else if (back && asking) {
        MenuSound(settings_overlay::kGameSoundBack);
    } else if (left || right) {
        MenuSound(settings_overlay::kGameSoundCursor);
    }
}

void HandleSettingsInput(bool left, bool right, bool up, bool down, bool confirm, bool back) {
    if (Keybinds::Capturing() >= 0) {
        Keybinds::PollCapture();  // the keyboard is the capture's until a key is picked
        return;
    }
    if (settings_overlay::ControllerCapturing() >= 0) {
        settings_overlay::PollControllerCapture();  // likewise the controller's buttons
        return;
    }
    if (HandleCheatAsk(left, right, confirm, back)) {
        return;
    }
    int& cursor = g_ui.cursor;
    if (cursor == 0) {
        if (left || right) {
            g_settingsTab = std::clamp(g_settingsTab + (left ? -1 : 1), 0, SettingsTabs() - 1);
        } else if (confirm) {
            g_settingsTab = (g_settingsTab + 1) % SettingsTabs();
        } else if (down && g_settingsTab != kTabMods) {  // Mods has no rows
            cursor = 1;
        } else if (back) {
            CloseSettings();
        }
        return;
    }
    if (back) {
        CloseSettings();
        return;
    }
    if (g_settingsTab == kTabMods) {
        cursor = 0;
        return;
    }
    if (g_settingsTab == kTabVideo) {
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kVideoLast, cursor + 1);
        if (cursor == kVideoRenderer && (confirm || left || right)) {
            const int next = (RendererIndex() + (left ? kRendererCount - 1 : 1)) % kRendererCount;
            settings_overlay::SetGraphicsApiChoice(kRenderers[next].configName);
            settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
            g_rendererChanged = true;
            g_settingsChanged = true;
        } else if (cursor == 1 && (left || right || confirm)) {
            StepVideo(left ? -1 : 1, confirm);
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
        } else if ((confirm || left || right) && cursor == kVideoVSync) {
            settings_overlay::SetVSync(!settings_overlay::VSync());
            g_settingsChanged = true;
        } else if ((confirm || left || right) && cursor == kVideoCoins) {
            settings_overlay::SetSpinningCoins(!settings_overlay::SpinningCoins());
            g_settingsChanged = true;
        } else if ((confirm || left || right) && cursor == kVideoDepthBlur) {
            settings_overlay::SetDepthBlur(!settings_overlay::DepthBlur());
            g_settingsChanged = true;
        } else if ((confirm || left || right) && cursor == kVideoBatterySaver) {
            settings_overlay::SetBatterySaver(!settings_overlay::BatterySaver());
            g_settingsChanged = true;
        }
        return;
    }
    if (g_settingsTab == kTabAudio) {
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kAudioLast, cursor + 1);
        if (cursor >= 1 && cursor <= kVolumeRowCount && (left || right)) {
            const settings_overlay::Volume which = kVolumeRows[cursor - 1].which;
            const int now = settings_overlay::VolumePercent(which);
            // Steps of 5, landing on a multiple of 5 from a dragged-in value like 37.
            const int next = left ? (now - 1) / 5 * 5 : (now + 5) / 5 * 5;
            settings_overlay::SetVolumePercent(which, next, true);
        }
        return;
    }
    if (g_settingsTab == kTabGameplay) {
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kGameplayLast, cursor + 1);
        if (confirm && cursor == kGameplayTime) {
            RuntimeConfigFile::SetNoTimeLimit(!RuntimeConfigFile::NoTimeLimit());
            g_settingsChanged = true;
        } else if (confirm && cursor == kGameplayLives && RuntimeConfigFile::InfiniteLives()) {
            RuntimeConfigFile::SetInfiniteLives(false);
            g_settingsChanged = true;
        } else if (confirm && cursor == kGameplayLives) {
            AskCheat(CheatAsk::Lives);
        } else if ((left || right) && cursor == kGameplayPowerUp) {
            const int count = GameplayOptions::PowerUpCount();
            g_powerUpChoice = (g_powerUpChoice + (left ? count - 1 : 1)) % count;
            RuntimeConfigFile::SetPowerUp(static_cast<uint32_t>(g_powerUpChoice));
        } else if (confirm && cursor == kGameplayPowerUp) {
            AskCheat(CheatAsk::PowerUp);
        } else if ((confirm || left || right) && cursor == kGameplayCheat) {
            RuntimeConfigFile::SetCheatButton(!RuntimeConfigFile::CheatButton());
        }
        return;
    }
    if (g_settingsTab == kTabKeybinds && TouchControlsTab()) {
        cursor = std::min(cursor, kTouchLast);
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kTouchLast, cursor + 1);
        if (confirm && cursor == kTouchEdit) {
            CloseSettings();
#if defined(__ANDROID__)
            AndroidBridge::EditTouchLayout();
#endif
        } else if ((confirm || left || right) && cursor == kTouchReverseTilt) {
            Keybinds::SetReverseTilt(!Keybinds::ReverseTilt());
        } else if ((confirm || left || right) && cursor == kTouchSmartSpin) {
            Keybinds::SetSmartSpin(!Keybinds::SmartSpin());
        }
        return;
    }
    if (g_settingsTab == kTabLanguage) {
        if (up) cursor = cursor - 1;
        if (down) cursor = std::min(kLanguageLast, cursor + 1);
        if (confirm && cursor >= 1 && cursor <= kLanguageCount) {
            RuntimeConfigFile::SetLanguage(kLanguages[cursor - 1].code);
            UiText::SetLanguage(kLanguages[cursor - 1].code);
            g_settingsChanged = true;
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
        cursor = kKeyReset;
        if (up) cursor = kKeySmartSpin;
        if (confirm) {
            if (settings_overlay::ControllerConnected()) settings_overlay::ResetControllerBindings();
            else Keybinds::ResetDefaults();
        }
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
        Text(c, UiText::Tr("Looking for rooms") + std::string(dots, '.'), 0, 30);
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
    // Direct Connection: below the list, its cursor row one past the rooms.
    const float directY = -height / 2 + 40;
    Button(c, "Direct Connection", -70, directY, 300, g_ui.cursor == count, true, g_ui.frame, 26.0f, 48.0f);
    AddHit(-70, directY, 310, 54, count);
    std::string status;
    uint32_t color = 0xFFF0B0FFu;
    if (!lobby.direct.empty()) {
        status = UiText::Tr("Asking ") + lobby.direct + "...";
    } else if (!lobby.message.empty()) {
        status = UiText::Tr(lobby.message);
        color = 0xFFE040FFu;
    }
    if (!status.empty()) {
        Text(c, status, 0, directY + 40, 21, 1, color);
    }
    Text(c, "(1) Back", 225, -height / 2 + 22, 22, 2);
    AddHit(175, -height / 2 + 22, 140, 44, -1, true);
    if (g_entry.open) {
        Veil(c);
        Window(c, 240, 520);
        Title(c, "Direct Connection", 0, 80, 34);
        const float boxW = 400, boxH = 46, boxY = 14;
        const ImVec2 a = ToScreen(c, -boxW / 2, boxY + boxH / 2), b = ToScreen(c, boxW / 2, boxY - boxH / 2);
        c.list->AddRectFilled(a, b, IM_COL32(20, 40, 90, static_cast<int>(235 * c.alpha)), 8 * c.view.scale);
        c.list->AddRect(a, b, IM_COL32(60, 60, 60, static_cast<int>(255 * c.alpha)), 8 * c.view.scale, 0,
                        2 * c.view.scale);
        const bool caret = (g_ui.frame / 30) % 2 == 0;
        const std::string shown = g_entry.text.empty() && !caret ? std::string(" ") : g_entry.text + (caret ? "|" : " ");
        Text(c, g_entry.text.empty() && !caret ? UiText::Tr("IP:Port") : shown, 0, boxY, 28, 1,
             g_entry.text.empty() && !caret ? 0xA0A0A0FFu : 0xFFFFFFFFu);
        Text(c, "The host's IP:Port, shown at the top of its room.", 0, -40, 20);
        Text(c, "Enter connects, Esc cancels.", 0, -70, 20);
    }
}

void DrawRoom(const Canvas& c, const NetplayLobby::Snapshot& lobby) {
    const float height = 400;
    Window(c, height, 560);
    const bool host = lobby.localSlot == 0;
    Title(c, lobby.roomName.empty() ? "Room" : lobby.roomName, 0, height / 2 - 40, 36);
    if (host && !lobby.hostAddresses.empty()) {
        // What another player types into Join > Direct Connection.
        std::string line = lobby.hostAddresses[0];
        if (lobby.hostAddresses.size() > 1) {
            line += "   " + lobby.hostAddresses[1];
        }
        Text(c, UiText::Tr("IP:Port") + "  " + line, 0, height / 2 - 72, 21, 1, 0xC8D2E6FFu);
    }
    for (int slot = 0; slot < std::max<int>(lobby.maxPlayers, 2); ++slot) {
        const auto member = std::find_if(lobby.members.begin(), lobby.members.end(),
                                         [&](const NetplayLobby::Member& m) { return m.slot == slot; });
        const float y = height / 2 - 100 - slot * 44;
        if (member != lobby.members.end()) {
            Text(c, PlayerLine(static_cast<uint8_t>(slot), member->name, slot == lobby.localSlot), -230, y, 28, 0);
        } else {
            Text(c, std::to_string(slot + 1) + UiText::Tr("P  waiting..."), -230, y, 28, 0, 0xC8D2E6FFu);
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
    // Up to two lines, split at a space near the middle (of the translation: the halves aren't in UiText).
    std::string first = UiText::Tr(g_ui.message), second;
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
    return slot >= 0 && slot < static_cast<int>(names.size()) ? names[slot] : UiText::Tr("player ") + std::to_string(slot + 1);
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
        line = UiText::Tr("Waiting for ") + SlotName(status.waitingForSlot) + "...";
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
    for (int tab = 0; tab < SettingsTabs(); ++tab) {
        if ((ev & (kEvTab0 << tab)) != 0 && g_ui.screen == Screen::Settings && !g_settingsClosing) {
            if (g_settingsTab != tab) {
                settings_overlay::PlayGameSound(settings_overlay::kGameSoundPage);
            }
            g_settingsTab = tab;  // a tapped tab
            g_ui.cursor = 0;
            return;
        }
    }
    // A controller's L and R step through the settings tabs (wrapping), wherever the cursor is.
    const int shoulder = settings_overlay::ControllerShoulderPressed();
    if (shoulder != 0 && g_ui.screen == Screen::Settings && Keybinds::Capturing() < 0 &&
        settings_overlay::ControllerCapturing() < 0 && g_cheatAsk == CheatAsk::None) {
        g_settingsTab = (g_settingsTab + shoulder + SettingsTabs()) % SettingsTabs();
        g_ui.cursor = 0;
        settings_overlay::PlayGameSound(settings_overlay::kGameSoundPage);
        return;
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
        if (g_entry.open) {
            break;  // the address box takes the keyboard (HandleSdlEvent)
        }
        if (g_hasEntered.exchange(false)) {
            std::string address;
            {
                std::lock_guard<std::mutex> lock(g_enteredMutex);
                address = g_entered;
            }
            if (!address.empty()) {
                NetplayLobby::ConnectDirect(address);
            }
        }
        // Direct Connection joins once the host answers (the lobby joins it itself): its room, as a
        // room picked from the list shows.
        if (lobby.phase == Phase::Joining || lobby.phase == Phase::InRoom) {
            Show(Screen::Room);
            break;
        }
        const int count = static_cast<int>(lobby.rooms.size());
        if (g_ui.cursor > count) g_ui.cursor = count;  // a room went away
        if (up) g_ui.cursor = std::max(0, g_ui.cursor - 1);
        if (down) g_ui.cursor = std::min(count, g_ui.cursor + 1);
        if (back) {
            NetplayLobby::Stop();
            Show(Screen::Lan);
        }
        if (confirm && g_ui.cursor == count) {
#if defined(__ANDROID__)
            AndroidBridge::AskDirectAddress(UiText::Tr("Direct Connection"),
                                            UiText::Tr("The host's IP:Port, shown at the top of its room."));
#else
            g_entry.open = true;
            if (SDL_Window* window = SDL_GetKeyboardFocus()) {
                SDL_StartTextInput(window);
            }
#endif
        } else if (confirm && g_ui.cursor < count && !lobby.rooms[g_ui.cursor].full) {
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

// ---------------------------------------------------------------- the character screen
// The card screen after Select Players (and the world map's Add/Drop Players): each player's card
// (charaChangeSelectBase), its arrows (charaChangeSelectArrow's P_arrowL_01 / P_arrowR_00) and, under
// it, its "(2) OK" bar (characterChangeIndicator's P_InfoBase_00, whose lamps P_lamp_08..11 show the
// player's number), and "Back (1)" (N_back). Each is found where the game draws it this frame, from the
// live panes' global matrices, so the taps line up at every aspect. Taps press remote 1's buttons, so
// they work P1's card: the one whose first lamp is lit (or the only card shown).
constexpr uint16_t kProfileCharacterScreen = 0x2B6;

struct TapRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // (x, y) in the panes' global space (x squeezed, see TapCharacterScreen), `pad` layout units round it.
    bool Contains(float x, float y, float pad, float squeeze) const {
        return x >= x0 - pad * squeeze && x <= x1 + pad * squeeze && y >= y0 - pad && y <= y1 + pad;
    }
    float CenterX() const { return (x0 + x1) / 2; }
};

// Shown: its own and every parent's visible bit (mFlag +0xBB bit 0), and some opacity (mGlbAlpha +0xB9).
bool LivePaneShown(uint32_t pane) {
    uint32_t flags = 0;
    if (!Memory::TryRead32(pane + 0xB8, flags) || ((flags >> 16) & 0xFFu) == 0) {
        return false;
    }
    for (int depth = 0; pane != 0 && depth < 16; ++depth) {
        if (!Memory::TryRead32(pane + 0xB8, flags) || (flags & 1u) == 0) {
            return false;
        }
        if (!Memory::TryRead32(pane + 0xC, pane) || (pane != 0 && !InGuestRam(pane))) {
            return false;
        }
    }
    return true;
}

// Where the pane is drawn, in its layout's global space (x carries the 2D squeeze, as in
// StartCursorNav): its size (+0x4C) at its global matrix's (+0x84) place and y scale (these layouts
// scale evenly; the x column isn't a pane's width scale on every pane, N_back's), x squeezed, placed by
// its origin (`origin` as BRLYT has it: 0-8, left/centre/right by top/middle/bottom).
bool LivePaneRect(uint32_t pane, int origin, float squeeze, TapRect& r) {
    float m[12];
    for (int i = 0; i < 12; ++i) {
        m[i] = Memory::ReadFloat32(pane + 0x84 + 4 * i);
        if (!std::isfinite(m[i])) {
            return false;
        }
    }
    const float w = Memory::ReadFloat32(pane + 0x4C) * std::fabs(m[5]) * squeeze;
    const float h = Memory::ReadFloat32(pane + 0x50) * std::fabs(m[5]);
    if (!(w > 0.5f && h > 0.5f) || w > 2000.0f || h > 2000.0f) {
        return false;
    }
    const int ox = origin % 3, oy = origin / 3;
    r.x0 = m[3] - w * (ox == 0 ? 0.0f : ox == 1 ? 0.5f : 1.0f);
    r.y1 = m[7] + h * (oy == 0 ? 0.0f : oy == 1 ? 0.5f : 1.0f);
    r.x1 = r.x0 + w;
    r.y0 = r.y1 - h;
    return true;
}

struct CharacterCard {
    TapRect ok;
    bool firstLamp = false;  // its first lamp lit: remote 1's card
    int lamps = 0;           // how many of its lamps are lit
};
struct CharacterScreen {
    std::vector<CharacterCard> cards;
    std::vector<std::pair<TapRect, bool>> arrows;  // the arrow, and whether it points right
    bool hasBack = false;
    TapRect back;
};

CharacterScreen FindCharacterScreen(float squeeze) {
    CharacterScreen screen;
    std::vector<uint32_t> roots;
    for (const uint32_t object : GameMenus::Objects()) {
        for (uint32_t off = 0; off < 0x1000; off += 4) {
            uint32_t p = 0;
            if (Memory::TryRead32(object + off, p) && InGuestRam(p) &&
                std::find(roots.begin(), roots.end(), p) == roots.end() && LivePaneName(p) == "RootPane") {
                roots.push_back(p);
            }
        }
    }
    for (const uint32_t root : roots) {
        CharacterCard card;
        bool hasOk = false;
        WalkPanes(root, 0, [&](uint32_t pane) {
            const std::string name = LivePaneName(pane);
            TapRect r;
            if (name == "P_InfoBase_00" && LivePaneShown(pane) && LivePaneRect(pane, 4, squeeze, r)) {
                card.ok = r;
                hasOk = true;
            } else if ((name == "P_arrowR_00" || name == "P_arrowL_01") && LivePaneShown(pane) && LivePaneRect(pane, 4, squeeze, r)) {
                screen.arrows.emplace_back(r, name == "P_arrowR_00");
            } else if (name == "N_back" && LivePaneShown(pane) && LivePaneRect(pane, 5, squeeze, r)) {
                // Its right edge is where "Back (1)" ends (the pane is sized 30 x 40 live): the words
                // and the button's picture take about 150 x 40 units leftward from there.
                const float cy = (r.y0 + r.y1) / 2;
                screen.back = {r.x1 - 150.0f * squeeze, cy - 20.0f, r.x1, cy + 20.0f};
                screen.hasBack = true;
            } else if (name.rfind("P_lamp_", 0) == 0 && name.size() == 9 && name[7] == '0' && name[8] >= '8' &&
                       LivePaneShown(pane)) {
                card.firstLamp |= name[8] == '8';
                ++card.lamps;
            }
        });
        if (hasOk) {
            screen.cards.push_back(card);
        }
    }
    return screen;
}

// A tap on the character screen: true when it was one of its controls (or the screen is up and the
// tap is left alone), false when the screen isn't up.
bool TapCharacterScreen(float lx, float gx, float ly) {
    if (!GameMenus::HasProfile(kProfileCharacterScreen)) {
        return false;
    }
    // The panes' x carries d2d's 608/width squeeze (StartCursorNav): the tap's gets it too.
    const float read = Memory::ReadFloat32(0x8042B084u);
    const float squeeze = read > 0.1f && read < 2.0f ? read : 1.0f;
    const CharacterScreen screen = FindCharacterScreen(squeeze);
    if (screen.cards.empty()) {
        // Every card is in: the game asks Back / Start Game with its select cursor; steer it to the tap.
        if (GameCursorRect().valid) {
            StartCursorNav(lx, ly);
            return true;
        }
        return false;  // not on screen yet (or gone): the other tap rules apply
    }
    // Remote 1's card: its first lamp alone lit, else the only card.
    const CharacterCard* mine = nullptr;
    for (const CharacterCard& card : screen.cards) {
        if (card.firstLamp && card.lamps == 1) {
            mine = &card;
        }
    }
    if (mine == nullptr && screen.cards.size() == 1) {
        mine = &screen.cards.front();
    }
    const float x = gx * squeeze;
    RT_LOGF(RT_TAG_RUNTIME,
            "tap: character screen at %.0f, %.0f (x %.0f): %zu card(s)%s, %zu arrow(s), back %.0f..%.0f x %.0f..%.0f%s\n",
            gx, ly, x, screen.cards.size(), mine != nullptr ? " (P1's found)" : "", screen.arrows.size(), screen.back.x0,
            screen.back.x1, screen.back.y0, screen.back.y1, screen.hasBack ? "" : " (not shown)");
    if (screen.hasBack && screen.back.Contains(x, ly, 8.0f, squeeze)) {
        StartNav(NavKind::Press, static_cast<int>(kOne));
        return true;
    }
    if (mine != nullptr && mine->ok.Contains(x, ly, 6.0f, squeeze)) {
        StartNav(NavKind::Press, static_cast<int>(kTwo));
        return true;
    }
    // P1's arrows: the ones beside its card (nearest its OK bar), with room round the small arrow for a finger.
    for (const auto& [rect, right] : screen.arrows) {
        if (mine != nullptr &&
            std::fabs(rect.CenterX() - mine->ok.CenterX()) < (mine->ok.x1 - mine->ok.x0) / 2 + 40.0f * squeeze &&
            rect.Contains(x, ly, 14.0f, squeeze)) {
            StartNav(NavKind::Press, static_cast<int>(right ? kScreenRight : kScreenLeft));
            return true;
        }
    }
    // Once every card is in, the game asks Back / Start Game with its select cursor: steer it to the tap.
    if (GameCursorRect().valid) {
        StartCursorNav(lx, ly);
    }
    return true;
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
        const float fit = g_ui.screen == Screen::Settings ? g_settingsFit : 1.0f;  // drawn smaller to fit
        for (const Hit& hit : g_hits) {
            if (in(hit.x * fit, hit.y * fit, hit.w * fit, hit.h * fit)) {
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
        settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
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
        // the strap screen, "Press 2 to Start", or what the title plays when it idles (skips it)
        StartNav(NavKind::Press, static_cast<int>(kTwo | kA));
        return;
    }
    // Only after Select Players: on the world map the character screen's object stays loaded while
    // hidden, and claiming taps there took every tap from the map (its buttons, menu and levels).
    if (scene == kSceneGameSetup && TapCharacterScreen(lx, gx, ly)) {
        return;  // the character screen: OK, the arrows, Back
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
    if (scene == kSceneWorldMap && g_ui.menuSettingsShown && MenuSettings::MapButtonActive()) {
        if (in(kMenuBackX, -183.0f, 210, 40)) {  // where the layout has it
            StartNav(NavKind::Press, static_cast<int>(kOne));  // Back (1)
        } else {
            StartCursorNav(lx, ly);  // the game's own buttons, Settings among them
        }
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
    if (scene == kSceneStage && g_ui.pauseSettingsShown && !MenuSettings::PauseButtonActive()) {
        float cx = 0, cy = 0, scale = 1, alpha = 1;
        if (PauseWindowTransform(cx, cy, scale, alpha) &&
            in(cx, cy + PauseSettingsY() * scale, 345 * scale, 62 * scale)) {
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
        if (g_pointerInUse && std::fabs(lx - FileBackX()) <= kFileBackW / 2 + 4.0f &&
            std::fabs(ly - kFileBackY) <= kFileBackH / 2 + 4.0f) {
            StartNav(NavKind::Press, static_cast<int>(kOne));  // 1 is the file select's way back to the title
            return;
        }
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
#if defined(__ANDROID__)
    PromptStyle::NoteTouch();  // a finger on the screen: the on-screen remote's prompts
#endif
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

static void CloseAddressEntry() {
    g_entry.open = false;
    g_entry.text.clear();
    if (SDL_Window* window = SDL_GetKeyboardFocus()) {
        SDL_StopTextInput(window);
    }
}

bool HandleSdlEvent(const SDL_Event& event) {
    if (!g_entry.open) {
        return false;
    }
    if (event.type == SDL_EVENT_TEXT_INPUT && event.text.text != nullptr) {
        for (const char* p = event.text.text; *p != '\0'; ++p) {
            const char ch = *p;
            const bool allowed = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                 ch == '.' || ch == ':' || ch == '-' || ch == '_';
            if (allowed && g_entry.text.size() < 64) {
                g_entry.text += ch;
            }
        }
        return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        switch (event.key.scancode) {
        case SDL_SCANCODE_BACKSPACE:
            if (!g_entry.text.empty()) g_entry.text.pop_back();
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: {
            const std::string text = g_entry.text;
            CloseAddressEntry();
            NetplayLobby::ConnectDirect(text);
            break;
        }
        case SDL_SCANCODE_ESCAPE:
            CloseAddressEntry();
            break;
        default:
            break;
        }
        return true;
    }
    return event.type == SDL_EVENT_KEY_UP;
}

void DirectAddressEntered(const std::string& address) {
    std::lock_guard<std::mutex> lock(g_enteredMutex);
    g_entered = address;
    g_hasEntered = true;
}

bool TakeSettingsChange(float& renderScale, bool& showFps, int& aspect, bool& spinningCoins, bool& frameGraph,
                        bool& vsync, int& frameGraphSize, int& touchControls, int& renderer, bool& depthBlur,
                        bool& batterySaver) {
    if (!g_settingsChanged.exchange(false)) {
        return false;
    }
    renderScale = settings_overlay::RenderScale();
    showFps = settings_overlay::FrameGraph();  // the FPS counter is part of the Performance Monitor
    aspect = settings_overlay::Aspect();
    spinningCoins = settings_overlay::SpinningCoins();
    depthBlur = settings_overlay::DepthBlur();
    batterySaver = settings_overlay::BatterySaver();
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

// NSMBW_DUMP_EF=1: twice a second, the particle effects alive per nw4r::ef::EffectSystem group
// (active/closing), to tell effects that are never created from effects that are not drawn.
// The system is at 0x804170D4: group count at +0xC014, its ActivityList array (0x1C each) at +0xC018.
void DumpEffects() {
    static const bool on = std::getenv("NSMBW_DUMP_EF") != nullptr;
    static int frame = 0;
    if (!on || ++frame % 30 != 0) {
        return;
    }
    constexpr uint32_t kEffectSystem = 0x804170D4u;
    uint32_t groups = 0, lists = 0;
    if (!Memory::TryRead32(kEffectSystem + 0xC014u, groups) || !Memory::TryRead32(kEffectSystem + 0xC018u, lists) ||
        groups > 64) {
        return;
    }
    std::string text;
    for (uint32_t g = 0; g < groups; ++g) {
        uint32_t active = 0, closing = 0;
        Memory::TryRead32(lists + g * 0x1Cu + 0x08u, active);
        Memory::TryRead32(lists + g * 0x1Cu + 0x14u, closing);
        if ((active >> 16) != 0 || (closing >> 16) != 0) {
            char item[32];
            std::snprintf(item, sizeof(item), "g%u=%u/%u ", g, active >> 16, closing >> 16);
            text += item;
        }
    }
    // nw4r::ef::Resource (0x80423170): effect projects (ut::List) with their emitter total at +0xC,
    // texture projects at +0x10 with their texture total at +0x1C.
    constexpr uint32_t kResource = 0x80423170u;
    uint32_t projects = 0, emitters = 0, texProjects = 0, textures = 0;
    Memory::TryRead32(kResource + 0x08u, projects);
    Memory::TryRead32(kResource + 0x0Cu, emitters);
    Memory::TryRead32(kResource + 0x18u, texProjects);
    Memory::TryRead32(kResource + 0x1Cu, textures);
    RT_LOGF(RT_TAG_RUNTIME, "ef (scene %03X, %u groups, lists %08X, %u breff/%u emitters, %u breft/%u textures): %s\n",
            GameMenus::CurrentScene(), groups, lists, projects >> 16, emitters, texProjects >> 16, textures,
            text.c_str());
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
            if (area != 0 || entrance != 0) {
                // dInfo_c::startGame (0x800BB780) already handed the course-in scene its start
                // point as the scene parameter, area << 8 | entrance (dScene_c::mPara 0x8042A048).
                Memory::Write32(0x8042A048u, static_cast<uint32_t>(area << 8 | entrance));
            }
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
    // The game's button prompts as the input last used shows them (key_caps.h); every screen, LAN too.
    KeyCaps::Update();
    TestPad::Update();
    DumpProfiles();
    DumpEffects();
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
            const int before = g_ui.topItem;
            g_ui.topItem = g_ui.topItem == 2 ? 0 : 1;  // Update -> Couch -> gear
            if (g_ui.topItem != before) settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
        } else if (ev & kEvLeft) {
            const int before = g_ui.topItem;
            g_ui.topItem = g_ui.topItem == 1 || !UpdateShown() ? 0 : 2;
            if (g_ui.topItem != before) settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
        } else if ((ev & kEvConfirm) && g_ui.topItem == 1) {
            g_ui.toggleFocus = false;
            HitButton(900);
            settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
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
    // The world map's menu with its own Settings button (patched layout): the settings window once the
    // menu has gone out, and the menu opens again when it closes.
    const bool mapButton = MenuSettings::MapButtonActive();
    if (MenuSettings::TakeMapOpenRequest()) {
        g_ui.settingsFromPause = true;
        g_ui.settingsFromMapButton = true;
        Show(Screen::Settings);
    }
    if (scene == kSceneWorldMap) {
        if (!mapButton) {
            ApplyMenuLayout();
        }
        static const bool dumpMap = std::getenv("NSMBW_DUMP_WMAP") != nullptr;
        static int dumpFrame = 0;
        if (dumpMap && ++dumpFrame % 600 == 0) {
            WorldMapNav::Dump();
        }
    } else if (g_nav.kind == NavKind::MapWalk) {
        WorldMapNav::Cancel();
        g_nav.kind = NavKind::None;
    }
    // The pause window's own Settings button (patched layout): its window, once the game's has closed.
    const bool gameButton = MenuSettings::PauseButtonActive();
    if (MenuSettings::TakeOpenRequest()) {
        g_ui.settingsFromPause = true;
        g_ui.settingsFromGameButton = true;
        g_ui.settingsWaitGame = true;
        Show(Screen::Settings);
    }
    if (scene == kSceneStage && !gameButton) {
        ApplyPauseLayout();
    }
    if (g_menuOpenSettings.exchange(false) || g_pauseOpenSettings.exchange(false)) {
        g_ui.settingsFromPause = true;
        settings_overlay::PlayGameSound(settings_overlay::kGameSoundDecide);
        Show(Screen::Settings);
    }
    HideGameCursor(((!mapButton && g_menuFocus.load() && g_ui.menuSettingsShown) ||
                    (!gameButton && g_pauseFocus.load() && g_ui.pauseSettingsShown)) &&
                   g_ui.screen == Screen::None);
    if (scene == kSceneStage && !gameButton) {
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
    static bool characters = false;  // the character screen after Select Players (TapCharacterScreen)
    // What the title plays once it idles: the opening movie, then demo replays of courses. A press
    // skips them back to the title, so a tap does too. (The movie also plays for a new file, after the
    // file select: that one is left alone.)
    static bool fromTitle = false;
    static bool attract = false;
    if (++titleCheck % 10 == 0) {
        title = scene == kSceneStage && GameMenus::HasProfile(kProfileOpeningTitle);
        characters = scene == kSceneGameSetup && GameMenus::HasProfile(kProfileCharacterScreen);
        if (title) {
            fromTitle = true;
        } else if (scene == kSceneGameSetup || scene == kSceneWorldMap) {
            fromTitle = false;
        }
        uint32_t gameMode = 0;
        const bool demo = scene == kSceneStage && Memory::TryRead32(kStageGameMode, gameMode) &&
                          gameMode == kGameModeTitleReplay;
        const bool was = attract;
        attract = demo || (fromTitle && scene == kSceneMovie);
        if (attract != was) {
            RT_LOGF(RT_TAG_RUNTIME, "tap: title demo %s (scene %03X)\n", attract ? "on" : "off", scene);
        }
    }
    const bool tapScreen = Modal() || g_ui.toggleShown || scene == kSceneBoot || title || attract || characters ||
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
        if (tapX >= 0.0f || io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            g_pointerInUse = true;
        }
        if (tapX >= 0.0f) {
            RT_LOGF(RT_TAG_RUNTIME, "tap: %s at %.0f,%.0f px, screen %s\n", (tap >> 63) != 0 ? "app" : "mouse", tapX,
                    tapY, ScreenName(g_ui.screen));
            const GameLayout::View view = GameLayout::GameView();
            TapAt((tapX - view.centerX) / view.scale, (view.centerY - tapY) / view.scale,
                  (tapX - view.centerX) / view.gameScaleX, scene, title || attract);
        }
    }

    if (g_ui.screen == Screen::None) {
        g_ui.events = 0;  // presses on the game's own screens are the game's
        if (g_ui.toggleShown) {
            ++g_ui.frame;
            DrawToggle(Canvas{list, GameLayout::GameView(), 1.0f});
            TrackActiveFile();
            DrawFileCheated(Canvas{list, GameLayout::GameView(), 1.0f});
            DrawFileBack(Canvas{list, GameLayout::GameView(), 1.0f});
        }
        if (scene == kSceneStage && !MenuSettings::PauseButtonActive()) {
            ++g_ui.frame;
            DrawPauseSettings(list);
        }
        if (scene == kSceneWorldMap && !MenuSettings::MapButtonActive()) {
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
        DrawSettings(c);  // with its own dimming
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
        // A press on a real controller or keyboard takes the Back button away, not the presses a tap
        // plays back, nor the on-screen remote's.
        static uint32_t s_realHold = 0;
        if ((sample.hold & ~s_realHold) != 0 && (!kTouchToggle || settings_overlay::ControllerConnected())) {
            g_pointerInUse = false;
        }
        s_realHold = sample.hold;
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
        if (g_ui.menuSettingsShown && !Modal() && !tapping && !MenuSettings::MapButtonActive()) {
            const uint32_t down = upright ? kDown : kLeft, up = upright ? kUp : kRight;
            const int cursor = GameMenus::CourseMenuCursor();
            if (!g_menuFocus) {
                if ((pressed & down) != 0 && cursor == 2) {
                    g_menuFocus = true;
                    g_menuFocusFrom = 2;
                    g_ui.heldFromMenu |= down;
                    settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
                } else if ((pressed & up) != 0 && cursor == 3) {
                    g_menuFocus = true;
                    g_menuFocusFrom = 3;
                    g_ui.heldFromMenu |= up;
                    settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
                }
            } else {
                if ((pressed & up) != 0) {
                    g_menuFocus = false;
                    g_ui.heldFromMenu |= up;
                    if (g_menuFocusFrom == 3) {
                        g_ui.injectButtons = up;  // the game's cursor: Title Screen -> Quick Save
                        g_ui.injectReads = 4;
                    } else {
                        settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
                    }
                } else if ((pressed & down) != 0) {
                    g_menuFocus = false;
                    g_ui.heldFromMenu |= down;
                    if (g_menuFocusFrom == 2) {
                        g_ui.injectButtons = down;  // Quick Save -> Title Screen
                        g_ui.injectReads = 4;
                    } else {
                        settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
                    }
                } else if ((pressed & (kTwo | kA)) != 0) {
                    g_ui.heldFromMenu |= pressed & (kTwo | kA);
                    HitButton(901);
                    g_menuOpenSettings = true;
                } else if ((pressed & (kOne | kB)) != 0) {
                    g_menuFocus = false;  // the game closes its menu
                }
                if (g_menuFocus) {
                    sample.hold &= ~(kUp | kDown | kLeft | kRight | kTwo | kA);
                }
            }
        }

        // A level's pause window: Settings sits between Continue and Exit Course, the same way (unless the
        // window has its own Settings button: hle/nsmbw_menu_settings.cpp).
        if (g_ui.pauseSettingsShown && !Modal() && !tapping && !MenuSettings::PauseButtonActive()) {
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
        // Couch toggle above them; the game never sees that press, so its cursor stays put. Where
        // the layout stacks the files, only from the top one: up from the others is the game's.
        const uint32_t screenUp = upright ? kUp : kRight;
        const int fileCursor = GameMenus::FileSelectRawCursor();
        if (g_ui.toggleShown && !g_ui.toggleFocus && !Modal() && (pressed & screenUp) != 0 && fileCursor >= 0 &&
            fileCursor <= (FilesStacked() ? 0 : 2)) {
            g_ui.toggleFocus = true;
            g_ui.topItem = 0;
            settings_overlay::PlayGameSound(settings_overlay::kGameSoundCursor);
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
