#include "keybinds.h"

#include "combo_engine.h"
#include "det_clock.h"
#include "runtime_config.h"
#include "timebase_contract.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace Keybinds {
namespace {

// A binding is an SDL scancode, or a mouse button as kMouseCode + its SDL button number (1 left,
// 2 middle, 3 right, 4 and 5 the side buttons), saved as "Mouse L" and so on.
constexpr int kMouseCode = 0x10000;
constexpr int kMouseButtons = 5;
constexpr const char* kMouseNames[kMouseButtons] = {"Mouse L", "Mouse M", "Mouse R", "Mouse 4", "Mouse 5"};
constexpr int Mouse(int button) { return kMouseCode + button; }
// Android turns touches into left clicks: the mouse defaults (and buttons) are the desktop's only.
#ifdef __ANDROID__
#define MOUSE_DEFAULT(button)
#else
#define MOUSE_DEFAULT(button) , Mouse(button)
#endif

struct Default {
    const char* key;   // Config.toml [keyboard] key
    const char* name;  // shown in the Keybinds tab
    std::vector<int> codes;
};

const std::array<Default, kActionCount>& Defaults() {
    static const std::array<Default, kActionCount> defaults = {{
        {"up", "Up", {SDL_SCANCODE_UP}},
        {"down", "Down", {SDL_SCANCODE_DOWN MOUSE_DEFAULT(SDL_BUTTON_RIGHT)}},  // right click crouches
        {"left", "Left", {SDL_SCANCODE_LEFT}},
        {"right", "Right", {SDL_SCANCODE_RIGHT}},
        {"jump", "Jump (2)", {SDL_SCANCODE_X, SDL_SCANCODE_SPACE}},
        {"run", "Run / Fire (1)", {SDL_SCANCODE_Z, SDL_SCANCODE_LSHIFT}},
        {"a", "A", {SDL_SCANCODE_B}},
        {"shake", "Shake (Spin)", {SDL_SCANCODE_C, SDL_SCANCODE_LCTRL}},
        {"tilt_left", "Tilt Left", {SDL_SCANCODE_Q}},
        {"tilt_right", "Tilt Right", {SDL_SCANCODE_E}},
        {"plus", "+ (Pause)", {SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE}},
        {"minus", "- (Minus)", {SDL_SCANCODE_MINUS, SDL_SCANCODE_TAB}},
    }};
    return defaults;
}

std::mutex g_mutex;
bool g_loaded = false;
std::array<std::vector<int>, kActionCount> g_codes;
std::array<std::vector<std::vector<int>>, kActionCount> g_combos;  // each sorted, 2 or 3 codes
ComboEngine::Engine g_engine;
bool g_engineDirty = true;  // the bindings changed since g_engine was set up
std::atomic<uint32_t> g_bindingsGeneration{0};  // NoteBindingsChanged
int g_capturing = -1;
std::vector<bool> g_heldAtCapture;  // keys already down when the capture began
uint32_t g_mouseHeldAtCapture = 0;  // likewise the mouse buttons (the click that started it)
std::vector<int> g_captured;        // the keys pressed in this capture, in order
uint64_t g_captureIdleSinceNs = 0;  // when nothing was last held in this capture
bool g_suppress = false;            // a capture just ended: ignore the keyboard until all keys are up
constexpr size_t kMaxCombos = 4;    // per action
constexpr uint64_t kCaptureTimeoutNs = 10'000'000'000;
std::atomic<int> g_smartSpin{-1};   // Space Smart Spin QoL: -1 until read from Config.toml
std::atomic<int> g_reverseTilt{-1};  // Reverse Tilt QoL: -1 until read from Config.toml
std::atomic<int> g_mouseGyro{-1};    // Mouse Gyro QoL: -1 until read from Config.toml

std::vector<int> Parse(const std::string& text) {
    std::vector<int> codes;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string name = RuntimeConfigFile::Trim(
            std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!name.empty()) {
            const auto mouse = std::find_if(std::begin(kMouseNames), std::end(kMouseNames),
                                            [&](const char* m) { return name == m; });
            if (mouse != std::end(kMouseNames)) {
                codes.push_back(Mouse(static_cast<int>(mouse - std::begin(kMouseNames)) + 1));
            } else if (const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str()); code != SDL_SCANCODE_UNKNOWN) {
                codes.push_back(code);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return codes;
}

std::string Format(const std::vector<int>& codes, const char* separator) {
    std::string text;
    for (const int code : codes) {
        if (!text.empty()) {
            text += separator;
        }
        text += code > kMouseCode ? kMouseNames[code - kMouseCode - 1] : SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
    }
    return text;
}

// The mouse buttons held, as SDL_BUTTON_MASK bits. None count while the cursor is over the F10 overlay.
uint32_t RawMouseButtons() {
#ifdef __ANDROID__
    return 0;
#else
    return SDL_GetMouseState(nullptr, nullptr);
#endif
}

uint32_t MouseButtons() {
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        return 0;
    }
    return RawMouseButtons();
}

// NSMBW_KEY_SCRIPT (keybinds.h Scripted).
struct KeyEvent {
    uint64_t startMs = 0, endMs = 0;
    std::vector<int> codes;
};

const std::vector<KeyEvent>& KeyScript() {
    static const std::vector<KeyEvent> events = [] {
        std::vector<KeyEvent> parsed;
        const char* text = std::getenv("NSMBW_KEY_SCRIPT");
        if (text == nullptr) return parsed;
        const std::string all(text);
        size_t pos = 0;
        while (pos < all.size()) {
            const size_t comma = all.find(',', pos);
            const std::string item = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const size_t c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
            if (c1 != std::string::npos) {
                KeyEvent event;
                event.startMs = std::strtoull(item.substr(0, c1).c_str(), nullptr, 10);
                event.endMs = event.startMs + (c2 == std::string::npos ? 150 : std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 10));
                const std::string keys = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                size_t b = 0;
                while (b <= keys.size()) {
                    const size_t plus = keys.find('+', b);
                    const SDL_Scancode code = SDL_GetScancodeFromName(keys.substr(b, plus == std::string::npos ? std::string::npos : plus - b).c_str());
                    if (code != SDL_SCANCODE_UNKNOWN) event.codes.push_back(code);
                    if (plus == std::string::npos) break;
                    b = plus + 1;
                }
                parsed.push_back(event);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return parsed;
    }();
    return events;
}

bool ScriptHeld(int code) {
    const auto& events = KeyScript();
    if (events.empty()) return false;
    // Deterministic mode times the script on the virtual timeline, as the scripted remote is, so a run
    // replays the same whatever the machine's load.
    const auto clockMs = [] {
        return DetClock::Enabled() ? DetClock::Now() / (TimeBaseContract::kTicksPerSecond / 1000) : SDL_GetTicks();
    };
    static const uint64_t origin = DetClock::Enabled() ? 0 : clockMs();  // deterministic: from boot
    const uint64_t now = clockMs() - origin;
    for (const KeyEvent& event : events) {
        if (now >= event.startMs && now < event.endMs &&
            std::find(event.codes.begin(), event.codes.end(), code) != event.codes.end()) {
            return true;
        }
    }
    return false;
}

bool CodeHeld(int code, const bool* keys, int count) {
    if (code > kMouseCode) {
        return (MouseButtons() & SDL_BUTTON_MASK(code - kMouseCode)) != 0;
    }
    return (keys != nullptr && code < count && keys[code]) || ScriptHeld(code);
}

// One name ("Left Ctrl", "Mouse L", ",") as a code, or -1: a combination's members are whole names.
int ParseOne(const std::string& name) {
    for (int button = 0; button < kMouseButtons; ++button) {
        if (name == kMouseNames[button]) return Mouse(button + 1);
    }
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(code);
}

std::string NameOf(int code) {
    return Format({code}, "");
}

// A combination as shown and saved: Ctrl, Shift, Alt and GUI first ("Left Ctrl + X").
std::vector<int> DisplayOrder(std::vector<int> members) {
    const auto modifier = [](int code) { return code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI; };
    std::stable_sort(members.begin(), members.end(), [&](int a, int b) { return modifier(a) && !modifier(b); });
    return members;
}

void LoadLocked() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    for (int i = 0; i < kActionCount; ++i) {
        const std::string saved = RuntimeConfigFile::KeyboardBinding(Defaults()[i].key);
        g_codes[i] = saved.empty() ? Defaults()[i].codes : Parse(saved);
        g_combos[i].clear();
        for (const auto& names : RuntimeConfigFile::KeyboardCombos(Defaults()[i].key)) {
            std::vector<int> members;
            for (const std::string& name : names) {
                const int code = ParseOne(name);
                if (code >= 0 && std::find(members.begin(), members.end(), code) == members.end()) {
                    members.push_back(code);
                }
            }
            std::sort(members.begin(), members.end());
            if (members.size() >= 2 && members.size() <= ComboEngine::kMaxMembers && g_combos[i].size() < kMaxCombos) {
                g_combos[i].push_back(members);
            }
        }
    }
    g_engineDirty = true;
    g_bindingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void SaveLocked(int action) {
    RuntimeConfigFile::SetKeyboardBinding(Defaults()[action].key, Format(g_codes[action], ","));
    g_engineDirty = true;
    g_bindingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void SaveCombosLocked(int action) {
    std::vector<std::vector<std::string>> combos;
    for (const auto& members : g_combos[action]) {
        std::vector<std::string> names;
        for (const int code : DisplayOrder(members)) names.push_back(NameOf(code));
        combos.push_back(std::move(names));
    }
    RuntimeConfigFile::SetKeyboardCombos(Defaults()[action].key, combos);
    g_engineDirty = true;
    g_bindingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

// The engine's view of the bindings: the combinations, every key bound alone (they wait for a
// combination they're in), and the directions' and tilt's keys (which only gate one).
void SetUpEngineLocked() {
    if (!g_engineDirty) {
        return;
    }
    g_engineDirty = false;
    ComboEngine::Setup setup;
    for (int action = 0; action < kActionCount; ++action) {
        for (const auto& members : g_combos[action]) setup.combos.push_back({action, members});
        const bool condition = action == kUp || action == kDown || action == kLeft || action == kRight ||
                               action == kTiltLeft || action == kTiltRight;
        for (const int code : g_codes[action]) {
            setup.singles.push_back(code);
            if (condition) setup.conditions.push_back(code);
        }
    }
    g_engine.Configure(std::move(setup));
}

std::string ComboLabelLocked(int action) {
    std::string text;
    for (const auto& members : g_combos[action]) {
        if (!text.empty()) text += " / ";
        text += Format(DisplayOrder(members), " + ");
    }
    return text;
}

bool AnyKeyDown() {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i] || ScriptHeld(i)) {
            return true;
        }
    }
    return RawMouseButtons() != 0;
}

} // namespace

const char* ActionName(int action) {
    return action >= 0 && action < kActionCount ? Defaults()[action].name : "";
}

const char* ActionKey(int action) {
    return action >= 0 && action < kActionCount ? Defaults()[action].key : "";
}

std::string KeyLabel(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount) {
        return {};
    }
    std::string text = Format(g_codes[action], " / ");
    const std::string combos = ComboLabelLocked(action);
    if (!combos.empty()) text += (text.empty() ? "" : " / ") + combos;
    return text.empty() ? "None" : text;
}

std::vector<std::string> FirstBinding(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    std::vector<std::string> names;
    if (action < 0 || action >= kActionCount) {
        return names;
    }
    if (!g_codes[action].empty()) {
        names.push_back(NameOf(g_codes[action].front()));
    } else if (!g_combos[action].empty()) {
        for (const int code : g_combos[action].front()) names.push_back(NameOf(code));
    }
    return names;
}

std::vector<std::vector<std::string>> Bindings(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    std::vector<std::vector<std::string>> bindings;
    if (action < 0 || action >= kActionCount) {
        return bindings;
    }
    for (const int code : g_codes[action]) bindings.push_back({NameOf(code)});
    for (const auto& members : g_combos[action]) {
        std::vector<std::string> names;
        for (const int code : DisplayOrder(members)) names.push_back(NameOf(code));
        bindings.push_back(std::move(names));
    }
    return bindings;
}

std::string ComboLabel(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    return action >= 0 && action < kActionCount ? ComboLabelLocked(action) : std::string();
}

void ClearCombos(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount || g_combos[action].empty()) {
        return;
    }
    g_combos[action].clear();
    SaveCombosLocked(action);
}

void StepCombos(bool active) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    SetUpEngineLocked();
    if (g_engine.Empty()) {
        return;
    }
    if (!active || g_capturing >= 0 || g_suppress) {
        g_engine.Reset();
        return;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::vector<int> held;
    for (const auto& combos : g_combos) {
        for (const auto& members : combos) {
            for (const int code : members) {
                if (CodeHeld(code, keys, count) && std::find(held.begin(), held.end(), code) == held.end()) {
                    held.push_back(code);
                }
            }
        }
    }
    g_engine.Step(held, SDL_GetTicksNS());
}

bool Silenced(int scancode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_engine.Silenced(scancode);
}

bool Tapped(int scancode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_engine.Tapped(scancode);
}

bool Scripted() {
    static const bool scripted = std::getenv("NSMBW_KEY_SCRIPT") != nullptr;
    return scripted;
}

bool ScriptKeyHeld(int scancode) {
    return ScriptHeld(scancode);
}

bool Held(int action) {
    return HeldIgnoring(action, SDL_SCANCODE_UNKNOWN);
}

bool HeldIgnoring(int action, int scancode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (g_capturing >= 0 || g_suppress || action < 0 || action >= kActionCount) {
        return false;
    }
    if ((g_engine.Actions() & (1u << action)) != 0) {
        return true;  // a combination bound to it
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (const int code : g_codes[action]) {
        if (code != scancode && ((!g_engine.Silenced(code) && CodeHeld(code, keys, count)) || g_engine.Tapped(code))) {
            return true;
        }
    }
    return false;
}

bool Bound(int action, int scancode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount) {
        return false;
    }
    const auto& codes = g_codes[action];
    return std::find(codes.begin(), codes.end(), scancode) != codes.end();
}

bool SmartSpin() {
    int on = g_smartSpin.load(std::memory_order_relaxed);
    if (on < 0) {
        on = RuntimeConfigFile::KeyboardBinding("smart_spin") == "false" ? 0 : 1;
        g_smartSpin.store(on, std::memory_order_relaxed);
    }
    return on != 0;
}

void SetSmartSpin(bool on) {
    RuntimeConfigFile::SetKeyboardBinding("smart_spin", on ? "true" : "false");
    g_smartSpin.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool ReverseTilt() {
    int on = g_reverseTilt.load(std::memory_order_relaxed);
    if (on < 0) {
        on = RuntimeConfigFile::KeyboardBinding("reverse_tilt") == "false" ? 0 : 1;
        g_reverseTilt.store(on, std::memory_order_relaxed);
    }
    return on != 0;
}

void SetReverseTilt(bool on) {
    RuntimeConfigFile::SetKeyboardBinding("reverse_tilt", on ? "true" : "false");
    g_reverseTilt.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool MouseGyro() {
#ifdef __ANDROID__
    return false;
#else
    int on = g_mouseGyro.load(std::memory_order_relaxed);
    if (on < 0) {
        on = RuntimeConfigFile::KeyboardBinding("mouse_gyro") == "true" ? 1 : 0;
        g_mouseGyro.store(on, std::memory_order_relaxed);
    }
    return on != 0;
#endif
}

void SetMouseGyro(bool on) {
    RuntimeConfigFile::SetKeyboardBinding("mouse_gyro", on ? "true" : "false");
    g_mouseGyro.store(on ? 1 : 0, std::memory_order_relaxed);
}

void BeginCapture(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    g_heldAtCapture.assign(static_cast<size_t>(count), false);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        g_heldAtCapture[i] = keys[i] || ScriptHeld(i);
    }
    g_mouseHeldAtCapture = RawMouseButtons();
    g_captured.clear();
    g_captureIdleSinceNs = SDL_GetTicksNS();
    g_engine.Reset();
    g_capturing = action;
}

int Capturing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturing;
}

std::string CaptureLabel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturing >= 0 ? Format(g_captured, " + ") : std::string();
}

std::vector<std::string> CaptureKeys() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> names;
    if (g_capturing >= 0) {
        for (const int code : DisplayOrder(g_captured)) names.push_back(NameOf(code));  // as it will be saved
    }
    return names;
}

namespace {
// The capture's keys, all let go: one is the action's key, 2 or 3 a combination added to it (moved
// from any other action that had the same one).
void TakeCaptureLocked() {
    const int action = g_capturing;
    std::vector<int> members = g_captured;
    if (members.size() == 1) {
        g_codes[action] = members;
        SaveLocked(action);
        return;
    }
    std::sort(members.begin(), members.end());
    for (int other = 0; other < kActionCount; ++other) {
        auto& combos = g_combos[other];
        const auto same = std::find(combos.begin(), combos.end(), members);
        if (same != combos.end()) {
            combos.erase(same);
            if (other != action) SaveCombosLocked(other);
        }
    }
    auto& combos = g_combos[action];
    if (combos.size() >= kMaxCombos) combos.erase(combos.begin());  // the oldest makes room
    combos.push_back(members);
    SaveCombosLocked(action);
}

void EndCaptureLocked() {
    g_capturing = -1;
    g_captured.clear();
    g_suppress = true;
}
}  // namespace

bool PollCapture() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capturing < 0) {
        return true;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    const uint32_t mouse = RawMouseButtons();
    g_mouseHeldAtCapture &= mouse;
    // What was pressed since the capture began (keys already down then count once let go).
    std::vector<int> pressed;
    for (int i = 0; keys != nullptr && i < count; ++i) {
        const bool before = i < static_cast<int>(g_heldAtCapture.size()) && g_heldAtCapture[i];
        if (!keys[i] && !ScriptHeld(i)) {  // NSMBW_KEY_SCRIPT's keys are captured like real ones
            if (before) g_heldAtCapture[i] = false;  // released: a later press of it counts
            continue;
        }
        if (!before) pressed.push_back(i);
    }
    for (int button = 1; button <= kMouseButtons; ++button) {
        if ((mouse & SDL_BUTTON_MASK(button)) != 0 && (g_mouseHeldAtCapture & SDL_BUTTON_MASK(button)) == 0) {
            pressed.push_back(Mouse(button));
        }
    }
    const uint64_t now = SDL_GetTicksNS();
    if (g_captured.empty()) {
        for (const int code : pressed) {
            if (code == SDL_SCANCODE_ESCAPE) {
                EndCaptureLocked();  // cancelled
                return true;
            }
            if (code == SDL_SCANCODE_DELETE || code == SDL_SCANCODE_BACKSPACE) {
                g_codes[g_capturing].clear();
                SaveLocked(g_capturing);
                g_combos[g_capturing].clear();
                SaveCombosLocked(g_capturing);
                EndCaptureLocked();
                return true;
            }
        }
    }
    for (const int code : pressed) {
        const bool reserved = code == SDL_SCANCODE_ESCAPE || code == SDL_SCANCODE_DELETE ||
                              code == SDL_SCANCODE_BACKSPACE || code == SDL_SCANCODE_F10 || code == SDL_SCANCODE_F11;
        if (!reserved && g_captured.size() < ComboEngine::kMaxMembers &&
            std::find(g_captured.begin(), g_captured.end(), code) == g_captured.end()) {
            g_captured.push_back(code);
        }
    }
    const bool anyHeld = std::any_of(g_captured.begin(), g_captured.end(),
                                     [&](int code) { return std::find(pressed.begin(), pressed.end(), code) != pressed.end(); });
    if (!g_captured.empty() && !anyHeld) {
        TakeCaptureLocked();  // all let go: saved
        EndCaptureLocked();
        return true;
    }
    if (anyHeld || !pressed.empty()) {
        g_captureIdleSinceNs = now;
    } else if (now - g_captureIdleSinceNs >= kCaptureTimeoutNs) {
        EndCaptureLocked();  // nothing pressed for 10 s: cancelled
        return true;
    }
    return false;
}

bool Suppressed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_suppress && !AnyKeyDown()) {
        g_suppress = false;
    }
    return g_capturing >= 0 || g_suppress;
}

void NoteBindingsChanged() {
    g_bindingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

uint32_t BindingsGeneration() {
    return g_bindingsGeneration.load(std::memory_order_acquire);
}

void ResetDefaults() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    for (int i = 0; i < kActionCount; ++i) {
        g_codes[i] = Defaults()[i].codes;
        SaveLocked(i);
        if (!g_combos[i].empty()) {
            g_combos[i].clear();
            SaveCombosLocked(i);
        }
    }
}

} // namespace Keybinds
