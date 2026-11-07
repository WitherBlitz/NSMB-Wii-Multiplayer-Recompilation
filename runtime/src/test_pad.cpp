// Test seam: NSMBW_PAD_SCRIPT="<ms>:<button>[+<button>]:<ms held>,..." plugs in a virtual Xbox-style
// controller (SDL's virtual joystick) and presses its buttons at those times after the first frame (in
// deterministic mode: after boot, on the virtual clock):
// A, B, X, Y, LB, RB, LT, RT, BACK, START, UP, DOWN, LEFT, RIGHT, LS, RS. For testing what the game does
// with a controller (its button prompts, the bindings) on a PC with none attached. Unset, it does nothing.
#include "test_pad.h"

#include "det_clock.h"
#include "runtime_log.h"
#include "timebase_contract.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace TestPad {
namespace {

// Deterministic mode times the presses on the virtual timeline, as the scripted remote is.
uint64_t ClockMs() {
    return DetClock::Enabled() ? DetClock::Now() / (TimeBaseContract::kTicksPerSecond / 1000) : SDL_GetTicks();
}

struct Press {
    uint64_t start = 0, end = 0;
    std::vector<int> buttons;  // SDL_GamepadButton, or -1 - axis for a trigger
};

int ButtonFor(const std::string& name) {
    if (name == "A") return SDL_GAMEPAD_BUTTON_SOUTH;
    if (name == "B") return SDL_GAMEPAD_BUTTON_EAST;
    if (name == "X") return SDL_GAMEPAD_BUTTON_WEST;
    if (name == "Y") return SDL_GAMEPAD_BUTTON_NORTH;
    if (name == "LB") return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    if (name == "RB") return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    if (name == "BACK") return SDL_GAMEPAD_BUTTON_BACK;
    if (name == "START") return SDL_GAMEPAD_BUTTON_START;
    if (name == "UP") return SDL_GAMEPAD_BUTTON_DPAD_UP;
    if (name == "DOWN") return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    if (name == "LEFT") return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (name == "RIGHT") return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (name == "LS") return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    if (name == "RS") return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    if (name == "LT") return -1 - SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    if (name == "RT") return -1 - SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    return SDL_GAMEPAD_BUTTON_INVALID;
}

std::vector<Press> Parse(const std::string& text) {
    std::vector<Press> presses;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t comma = text.find(',', pos);
        const std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
        if (c1 != std::string::npos) {
            Press press;
            press.start = std::strtoull(item.substr(0, c1).c_str(), nullptr, 10);
            press.end = press.start + (c2 == std::string::npos ? 150 : std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 10));
            const std::string names = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            size_t b = 0;
            while (b <= names.size()) {
                const size_t plus = names.find('+', b);
                const int button = ButtonFor(names.substr(b, plus == std::string::npos ? std::string::npos : plus - b));
                if (button != SDL_GAMEPAD_BUTTON_INVALID) press.buttons.push_back(button);
                if (plus == std::string::npos) break;
                b = plus + 1;
            }
            presses.push_back(press);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return presses;
}

} // namespace

void Update() {
    static const char* text = std::getenv("NSMBW_PAD_SCRIPT");
    if (text == nullptr) {
        return;
    }
    static const std::vector<Press> presses = Parse(text);
    static SDL_Joystick* joystick = nullptr;
    static uint64_t origin = 0;
    if (joystick == nullptr) {
        static bool tried = false;
        if (tried) return;
        tried = true;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.vendor_id = 0x045E;  // an Xbox 360 controller's ids, for SDL's mapping
        desc.product_id = 0x028E;
        desc.name = "NSMBW test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joystick = id != 0 ? SDL_OpenJoystick(id) : nullptr;
        origin = DetClock::Enabled() ? 0 : ClockMs();  // deterministic: from boot
        RT_LOGF(RT_TAG_RUNTIME, "test pad: %s (%zu presses)\n", joystick != nullptr ? "plugged in" : SDL_GetError(),
                presses.size());
        return;
    }
    const uint64_t now = ClockMs() - origin;
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    bool axes[SDL_GAMEPAD_AXIS_COUNT] = {};
    for (const Press& press : presses) {
        if (now < press.start || now >= press.end) continue;
        for (const int button : press.buttons) {
            if (button >= 0) buttons[button] = true;
            else axes[-1 - button] = true;
        }
    }
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) SDL_SetJoystickVirtualButton(joystick, i, buttons[i]);
    for (int i = SDL_GAMEPAD_AXIS_LEFT_TRIGGER; i <= SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; ++i) {
        SDL_SetJoystickVirtualAxis(joystick, i, axes[i] ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    }
}

} // namespace TestPad
