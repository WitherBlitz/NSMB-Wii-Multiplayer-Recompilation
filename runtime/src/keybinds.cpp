#include "keybinds.h"

#include "runtime_config.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
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
int g_capturing = -1;
std::vector<bool> g_heldAtCapture;  // keys already down when the capture began
uint32_t g_mouseHeldAtCapture = 0;  // likewise the mouse buttons (the click that started it)
bool g_suppress = false;            // a capture just ended: ignore the keyboard until all keys are up
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

bool CodeHeld(int code, const bool* keys, int count) {
    if (code > kMouseCode) {
        return (MouseButtons() & SDL_BUTTON_MASK(code - kMouseCode)) != 0;
    }
    return keys != nullptr && code < count && keys[code];
}

void LoadLocked() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    for (int i = 0; i < kActionCount; ++i) {
        const std::string saved = RuntimeConfigFile::KeyboardBinding(Defaults()[i].key);
        g_codes[i] = saved.empty() ? Defaults()[i].codes : Parse(saved);
    }
}

void SaveLocked(int action) {
    RuntimeConfigFile::SetKeyboardBinding(Defaults()[action].key, Format(g_codes[action], ","));
}

bool AnyKeyDown() {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i]) {
            return true;
        }
    }
    return RawMouseButtons() != 0;
}

} // namespace

const char* ActionName(int action) {
    return action >= 0 && action < kActionCount ? Defaults()[action].name : "";
}

std::string KeyLabel(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount) {
        return {};
    }
    const std::string text = Format(g_codes[action], " / ");
    return text.empty() ? "None" : text;
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
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (const int code : g_codes[action]) {
        if (code != scancode && CodeHeld(code, keys, count)) {
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
        g_heldAtCapture[i] = keys[i];
    }
    g_mouseHeldAtCapture = RawMouseButtons();
    g_capturing = action;
}

int Capturing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturing;
}

bool PollCapture() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capturing < 0) {
        return true;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        const bool before = i < static_cast<int>(g_heldAtCapture.size()) && g_heldAtCapture[i];
        if (!keys[i]) {
            if (before) {
                g_heldAtCapture[i] = false;  // released: a later press of it counts
            }
            continue;
        }
        if (before) {
            continue;
        }
        if (i != SDL_SCANCODE_ESCAPE) {
            g_codes[g_capturing] = {i};
            SaveLocked(g_capturing);
        }
        g_capturing = -1;
        g_suppress = true;
        return true;
    }
    // A mouse button pressed since the capture began (the click that began it counts once let go).
    const uint32_t mouse = RawMouseButtons();
    g_mouseHeldAtCapture &= mouse;
    for (int button = 1; button <= kMouseButtons; ++button) {
        if ((mouse & SDL_BUTTON_MASK(button)) != 0 && (g_mouseHeldAtCapture & SDL_BUTTON_MASK(button)) == 0) {
            g_codes[g_capturing] = {Mouse(button)};
            SaveLocked(g_capturing);
            g_capturing = -1;
            g_suppress = true;
            return true;
        }
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

void ResetDefaults() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    for (int i = 0; i < kActionCount; ++i) {
        g_codes[i] = Defaults()[i].codes;
        SaveLocked(i);
    }
}

} // namespace Keybinds
