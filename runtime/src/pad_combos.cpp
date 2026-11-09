// pad_combos.cpp - see pad_combos.h.
#include "pad_combos.h"

#include "combo_engine.h"
#include "controller_button_names.h"
#include "keybinds.h"
#include "runtime_config.h"

#include <aurora/sdl_gamepad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace PadCombos {
namespace {

constexpr size_t kMaxCombos = 4;  // per action
constexpr uint64_t kCaptureTimeoutNs = 10'000'000'000;
constexpr int kAxisPress = 16384, kAxisRelease = 8000;  // as the controller rebind capture

std::mutex g_mutex;
bool g_loaded = false;
// Each combination's members as native bindings (buttons, or axis directions with their threshold).
std::array<std::vector<std::vector<uint32_t>>, Keybinds::kActionCount> g_combos;
uint32_t g_generation = 1;  // bumped when the combinations change
struct Port {
    ComboEngine::Engine engine;
    uint64_t setupKey = 0;  // the bindings g_engine was set up for (0: none yet)
    uint32_t actions = 0;
};
std::array<Port, PAD_MAX_CONTROLLERS> g_ports;

int Code(uint32_t native) { return static_cast<int>(PADAxisButtonIdentity(native)); }

uint32_t ParseNative(const std::string& name) {
    const ControllerNames::NativeButtonItem* item = ControllerNames::FindNativeButton(name);
    if (item == nullptr || item->nativeButton == PAD_NATIVE_BUTTON_DISABLED ||
        item->nativeButton == PAD_NATIVE_BUTTON_INVALID) {
        return PAD_NATIVE_BUTTON_INVALID;
    }
    if (!PADIsAxisButton(item->nativeButton)) return item->nativeButton;
    const size_t at = name.find('@');
    uint32_t threshold = 0;
    if (at == std::string::npos) return item->nativeButton;
    const char* end = name.data() + name.size();
    const auto parsed = std::from_chars(name.data() + at + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100) return item->nativeButton;
    return PADAxisButtonIdentity(item->nativeButton) | (threshold << 8);
}

std::string ConfigName(uint32_t native) {
    std::string name = ControllerNames::NativeButtonForValue(native).configName;
    if (PADIsAxisButton(native)) name += '@' + std::to_string(PADAxisButtonThreshold(native));
    return name;
}

// "South (A / Cross)" -> "A / Cross", as the game window's controller rows show buttons.
std::string ShortName(uint32_t native) {
    const std::string text = ControllerNames::NativeButtonForValue(native).label;
    const size_t open = text.find('('), close = text.rfind(')');
    return open != std::string::npos && close != std::string::npos && close > open + 1
               ? text.substr(open + 1, close - open - 1)
               : text;
}

std::string MembersLabel(const std::vector<uint32_t>& members) {
    std::string text;
    for (const uint32_t native : members) {
        if (!text.empty()) text += " + ";
        text += ShortName(native);
    }
    return text;
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    for (int action = 0; action < Keybinds::kActionCount; ++action) {
        g_combos[action].clear();
        for (const auto& names : RuntimeConfigFile::ControllerCombos(Keybinds::ActionKey(action))) {
            std::vector<uint32_t> members;
            for (const std::string& name : names) {
                const uint32_t native = ParseNative(name);
                const bool known = native != PAD_NATIVE_BUTTON_INVALID;
                if (known && std::none_of(members.begin(), members.end(),
                                          [&](uint32_t m) { return Code(m) == Code(native); })) {
                    members.push_back(native);
                }
            }
            if (members.size() >= 2 && members.size() <= ComboEngine::kMaxMembers &&
                g_combos[action].size() < kMaxCombos) {
                g_combos[action].push_back(members);
            }
        }
    }
    ++g_generation;
    Keybinds::NoteBindingsChanged();
}

void SaveLocked(int action) {
    std::vector<std::vector<std::string>> combos;
    for (const auto& members : g_combos[action]) {
        std::vector<std::string> names;
        for (const uint32_t native : members) names.push_back(ConfigName(native));
        combos.push_back(std::move(names));
    }
    RuntimeConfigFile::SetControllerCombos(Keybinds::ActionKey(action), combos);
    ++g_generation;
    Keybinds::NoteBindingsChanged();
}

bool AnyCombos() {
    return std::any_of(g_combos.begin(), g_combos.end(), [](const auto& c) { return !c.empty(); });
}

bool NativePressed(SDL_Gamepad* gamepad, uint32_t native) {
    if (PADIsAxisButton(native)) {
        const uint32_t axis = PADAxisButtonAxis(native), threshold = PADAxisButtonThreshold(native);
        if (axis >= SDL_GAMEPAD_AXIS_COUNT || threshold < 1 || threshold > 100) return false;
        int value = aurora::sdl::GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(axis));
        if (PADAxisButtonNegative(native)) value = -value;
        return value > 0 && value * 100 >= static_cast<int>(threshold) * 32767;
    }
    return native < SDL_GAMEPAD_BUTTON_COUNT && aurora::sdl::GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(native));
}

bool IsStickAxis(uint32_t native) {
    if (!PADIsAxisButton(native)) return false;
    const uint32_t axis = PADAxisButtonAxis(native);
    return axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTX ||
           axis == SDL_GAMEPAD_AXIS_RIGHTY;
}

bool IsTriggerAxis(uint32_t native, SDL_GamepadAxis axis) {
    return PADIsAxisButton(native) && PADAxisButtonAxis(native) == static_cast<uint32_t>(axis);
}

struct Mapped {
    uint32_t native;
    uint16_t padButton;
};

// The port's button mappings (primary and alternate) that map something.
std::vector<Mapped> Mappings(uint32_t port) {
    std::vector<Mapped> mapped;
    uint32_t count = 0, altCount = 0;
    const PADButtonMapping* primary = PADGetButtonMappings(port, &count);
    const PADButtonMapping* alternate = PADGetAltButtonMappings(port, &altCount);
    for (uint32_t i = 0; primary != nullptr && i < count; ++i) {
        mapped.push_back({primary[i].nativeButton, primary[i].padButton});
    }
    for (uint32_t i = 0; alternate != nullptr && i < altCount; ++i) {
        mapped.push_back({alternate[i].nativeButton, alternate[i].padButton});
    }
    mapped.erase(std::remove_if(mapped.begin(), mapped.end(),
                                [](const Mapped& m) {
                                    return m.native == PAD_NATIVE_BUTTON_INVALID || m.native == PAD_NATIVE_BUTTON_DISABLED;
                                }),
                 mapped.end());
    return mapped;
}

// The engine for `port`: the combinations, the members that do something alone (mapped to a GameCube
// button, Back, which is the remote's -, and the triggers, - and shake), and those that only gate
// (the sticks, and buttons on the D-pad's GameCube buttons).
void SetUpPortLocked(Port& p, const std::vector<Mapped>& mapped) {
    uint64_t key = g_generation;
    for (const Mapped& m : mapped) key = key * 1099511628211ull ^ (static_cast<uint64_t>(m.native) << 16 | m.padButton);
    if (key == 0) key = 1;
    if (key == p.setupKey) return;
    p.setupKey = key;
    ComboEngine::Setup setup;
    for (int action = 0; action < Keybinds::kActionCount; ++action) {
        for (const auto& members : g_combos[action]) {
            ComboEngine::Combo combo{action, {}};
            for (const uint32_t native : members) {
                combo.members.push_back(Code(native));
                const bool dpad = std::any_of(mapped.begin(), mapped.end(), [&](const Mapped& m) {
                    return Code(m.native) == Code(native) &&
                           (m.padButton & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) != 0;
                });
                if (IsStickAxis(native) || dpad) {
                    setup.conditions.push_back(Code(native));
                    continue;
                }
                const bool single = native == SDL_GAMEPAD_BUTTON_BACK ||
                                    IsTriggerAxis(native, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ||
                                    IsTriggerAxis(native, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ||
                                    std::any_of(mapped.begin(), mapped.end(),
                                                [&](const Mapped& m) { return Code(m.native) == Code(native); });
                if (single) setup.singles.push_back(Code(native));
            }
            setup.combos.push_back(std::move(combo));
        }
    }
    p.engine.Configure(std::move(setup));
}

// Takes the silenced members' own effect out of the pad state: a GameCube button whose pressed
// sources are all silenced, Back, and a silenced trigger's pull. A tapped member (let go while it
// waited) puts its own effect in for this read instead.
void Silence(PADStatus& pad, const Port& p, const std::vector<Mapped>& mapped, SDL_Gamepad* gamepad) {
    for (const int code : p.engine.TappedCodes()) {
        for (const Mapped& m : mapped) {
            if (Code(m.native) == code) pad.button = static_cast<uint16_t>(pad.button | m.padButton);
        }
        if (code == Code(SDL_GAMEPAD_BUTTON_BACK)) pad.extButton |= PAD_BUTTON_BACK;
        if (code == Code(PADEncodeAxisButton(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, false))) pad.triggerLeft = 255;
        if (code == Code(PADEncodeAxisButton(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, false))) pad.triggerRight = 255;
    }
    if (p.engine.SilencedCodes().empty()) return;
    const auto silenced = [&](uint32_t native) { return p.engine.Silenced(Code(native)); };
    for (uint16_t bit = 1; bit != 0; bit = static_cast<uint16_t>(bit << 1)) {
        if ((pad.button & bit) == 0) continue;
        bool anyPressed = false, allSilenced = true;
        for (const Mapped& m : mapped) {
            if (m.padButton != bit || !NativePressed(gamepad, m.native)) continue;
            anyPressed = true;
            allSilenced = allSilenced && silenced(m.native);
        }
        if (anyPressed && allSilenced) pad.button = static_cast<uint16_t>(pad.button & ~bit);
    }
    if (silenced(SDL_GAMEPAD_BUTTON_BACK)) pad.extButton &= ~static_cast<PADExtButton>(PAD_BUTTON_BACK);
    const auto silenceTrigger = [&](SDL_GamepadAxis axis, uint8_t& value, uint16_t bit) {
        if (!p.engine.Silenced(Code(PADEncodeAxisButton(axis, false)))) return;
        value = 0;
        const bool other = std::any_of(mapped.begin(), mapped.end(), [&](const Mapped& m) {
            return m.padButton == bit && !silenced(m.native) && NativePressed(gamepad, m.native);
        });
        if (!other) pad.button = static_cast<uint16_t>(pad.button & ~bit);
    };
    silenceTrigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, pad.triggerLeft, PAD_TRIGGER_L);
    silenceTrigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, pad.triggerRight, PAD_TRIGGER_R);
}

// The capture.
struct Capture {
    bool active = false;
    int action = -1;
    uint32_t port = 0;
    SDL_JoystickID instance = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> heldAtStart{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axisReady{};
    std::array<bool, SDL_SCANCODE_COUNT> keysAtStart{};
    std::vector<uint32_t> captured;
    uint64_t idleSinceNs = 0;
} g_capture;

SDL_Gamepad* PortGamepad(uint32_t port) {
    const s32 index = PADGetIndexForPort(port);
    return index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
}

}  // namespace

void Apply(PADStatus* pads) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (!AnyCombos()) {
        for (Port& p : g_ports) p.actions = 0;
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        Port& p = g_ports[port];
        SDL_Gamepad* gamepad = pads[port].err == PAD_ERR_NONE ? PortGamepad(port) : nullptr;
        if (gamepad == nullptr || g_capture.active) {
            p.engine.Reset();
            p.actions = 0;
            continue;
        }
        const std::vector<Mapped> mapped = Mappings(port);
        SetUpPortLocked(p, mapped);
        std::vector<int> held;
        for (const auto& combos : g_combos) {
            for (const auto& members : combos) {
                for (const uint32_t native : members) {
                    if (NativePressed(gamepad, native) && std::find(held.begin(), held.end(), Code(native)) == held.end()) {
                        held.push_back(Code(native));
                    }
                }
            }
        }
        p.actions = p.engine.Step(held, now);
        Silence(pads[port], p, mapped, gamepad);
    }
}

uint32_t Actions(uint32_t port) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return port < PAD_MAX_CONTROLLERS ? g_ports[port].actions : 0;
}

std::string Label(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= Keybinds::kActionCount) return {};
    std::string text;
    for (const auto& members : g_combos[action]) {
        if (!text.empty()) text += " / ";
        text += MembersLabel(members);
    }
    return text;
}

std::vector<uint32_t> FirstMembers(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= Keybinds::kActionCount || g_combos[action].empty()) return {};
    return g_combos[action].front();
}

void Clear(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= Keybinds::kActionCount || g_combos[action].empty()) return;
    g_combos[action].clear();
    SaveLocked(action);
}

void BeginCapture(int action, uint32_t port) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    g_capture = {};
    SDL_Gamepad* gamepad = port < PAD_MAX_CONTROLLERS ? PortGamepad(port) : nullptr;
    if (gamepad == nullptr || action < 0 || action >= Keybinds::kActionCount) return;
    g_capture.active = true;
    g_capture.action = action;
    g_capture.port = port;
    g_capture.instance = aurora::sdl::GetGamepadID(gamepad);
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        g_capture.heldAtStart[i] = aurora::sdl::GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(i));
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
        g_capture.axisReady[i] = std::abs(static_cast<int>(aurora::sdl::GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(i)))) < kAxisRelease;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)); ++i) {
        g_capture.keysAtStart[i] = keys[i] || Keybinds::ScriptKeyHeld(i);
    }
    g_capture.idleSinceNs = SDL_GetTicksNS();
}

bool Capturing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capture.active;
}

std::string CaptureLabel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capture.active ? MembersLabel(g_capture.captured) : std::string();
}

CaptureResult PollCapture(uint32_t& single) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Capture& c = g_capture;
    if (!c.active) return CaptureResult::Cancelled;
    SDL_Gamepad* gamepad = aurora::sdl::GetGamepadFromID(c.instance);
    if (gamepad == nullptr) {
        c.active = false;  // unplugged meanwhile
        return CaptureResult::Cancelled;
    }
    // The keyboard's Escape (cancel) and Delete or Backspace (clear), before any button is held.
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    const auto keyPressed = [&](SDL_Scancode code) {
        const bool down = (keys != nullptr && code < count && keys[code]) || Keybinds::ScriptKeyHeld(code);
        if (!down) c.keysAtStart[code] = false;
        return down && !c.keysAtStart[code];
    };
    const bool escape = keyPressed(SDL_SCANCODE_ESCAPE);
    const bool clear = keyPressed(SDL_SCANCODE_DELETE) | keyPressed(SDL_SCANCODE_BACKSPACE);
    if (c.captured.empty() && escape) {
        c.active = false;
        return CaptureResult::Cancelled;
    }
    if (c.captured.empty() && clear) {
        c.active = false;
        if (!g_combos[c.action].empty()) {
            g_combos[c.action].clear();
            SaveLocked(c.action);
        }
        return CaptureResult::Cleared;
    }
    // What is held now: buttons pressed since the start, sticks and triggers pushed past half.
    std::vector<uint32_t> held;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        const bool down = aurora::sdl::GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(i));
        if (!down) c.heldAtStart[i] = false;
        if (down && !c.heldAtStart[i]) held.push_back(static_cast<uint32_t>(i));
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
        const int value = aurora::sdl::GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(i));
        if (std::abs(value) < kAxisRelease) c.axisReady[i] = true;
        const bool trigger = i == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || i == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (c.axisReady[i] && std::abs(value) >= kAxisPress && (!trigger || value > 0)) {
            held.push_back(PADEncodeAxisButton(static_cast<uint32_t>(i), value < 0));
        }
    }
    // A pushed axis stays held until it is back near the middle.
    for (const uint32_t native : c.captured) {
        if (!PADIsAxisButton(native)) continue;
        const int value = aurora::sdl::GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(PADAxisButtonAxis(native)));
        if (std::abs(value) >= kAxisRelease &&
            std::none_of(held.begin(), held.end(), [&](uint32_t h) { return Code(h) == Code(native); })) {
            held.push_back(native);
        }
    }
    for (const uint32_t native : held) {
        if (c.captured.size() < ComboEngine::kMaxMembers &&
            std::none_of(c.captured.begin(), c.captured.end(), [&](uint32_t m) { return Code(m) == Code(native); })) {
            c.captured.push_back(native);
        }
    }
    const bool anyHeld = std::any_of(c.captured.begin(), c.captured.end(), [&](uint32_t m) {
        return std::any_of(held.begin(), held.end(), [&](uint32_t h) { return Code(h) == Code(m); });
    });
    const uint64_t now = SDL_GetTicksNS();
    if (!c.captured.empty() && !anyHeld) {
        c.active = false;
        if (c.captured.size() == 1) {
            single = c.captured[0];
            return CaptureResult::Single;
        }
        std::vector<uint32_t> members = c.captured;
        std::sort(members.begin(), members.end(), [](uint32_t a, uint32_t b) { return Code(a) < Code(b); });
        const auto same = [&](const std::vector<uint32_t>& other) {
            if (other.size() != members.size()) return false;
            for (size_t i = 0; i < other.size(); ++i) {
                if (Code(other[i]) != Code(members[i])) return false;
            }
            return true;
        };
        for (int other = 0; other < Keybinds::kActionCount; ++other) {
            auto& combos = g_combos[other];
            const auto it = std::find_if(combos.begin(), combos.end(), same);
            if (it != combos.end()) {
                combos.erase(it);
                if (other != c.action) SaveLocked(other);
            }
        }
        auto& combos = g_combos[c.action];
        if (combos.size() >= kMaxCombos) combos.erase(combos.begin());
        combos.push_back(members);
        SaveLocked(c.action);
        return CaptureResult::Combo;
    }
    if (anyHeld || !held.empty()) {
        c.idleSinceNs = now;
    } else if (now - c.idleSinceNs >= kCaptureTimeoutNs) {
        c.active = false;
        return CaptureResult::Cancelled;
    }
    return CaptureResult::Running;
}

} // namespace PadCombos
