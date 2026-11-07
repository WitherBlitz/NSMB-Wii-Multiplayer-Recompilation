#include "prompt_style.h"

#include "runtime_log.h"
#include "wii_remote_input.h"

#include <SDL3/SDL_events.h>
#include <aurora/sdl_gamepad.h>
#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>

namespace PromptStyle {
namespace {

std::atomic<uint8_t> g_style{static_cast<uint8_t>(Style::Wii)};
std::atomic<uint32_t> g_generation{0};

void Set(Style style, const char* why) {
    const auto value = static_cast<uint8_t>(style);
    if (g_style.exchange(value, std::memory_order_acq_rel) == value) {
        return;
    }
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    static const char* const kNames[] = {"Wii Remote", "keyboard", "controller"};
    RT_LOGF(RT_TAG_RUNTIME, "button prompts: %s (%s)\n", kNames[value], why);
}

// The window's own keys: they never change the prompts.
bool Hotkey(const SDL_KeyboardEvent& key) {
    switch (key.scancode) {
    case SDL_SCANCODE_F10:
    case SDL_SCANCODE_F11:
    case SDL_SCANCODE_PRINTSCREEN:
        return true;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        return (key.mod & SDL_KMOD_ALT) != 0;  // Alt+Enter: fullscreen
    default:
        return false;
    }
}

// A gamepad SDL's Wii driver reports for a real Wii Remote is the Wii Remote, not a controller.
bool IsWiiPad(SDL_JoystickID id) {
    SDL_Gamepad* pad = aurora::sdl::GetGamepadFromID(id);
    const char* name = pad != nullptr ? aurora::sdl::GetGamepadName(pad) : nullptr;
    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForName(name);
    return kind != WiiRemoteInput::Kind::NotWii && kind != WiiRemoteInput::Kind::WiiUPro;
}

} // namespace

Style Current() noexcept {
    // NSMBW_PROMPTS=wii|keys|pad pins one (tests).
    static const int pinned = [] {
        const char* value = std::getenv("NSMBW_PROMPTS");
        if (value == nullptr) return -1;
        const std::string text(value);
        return text == "keys" ? 1 : text == "pad" ? 2 : text == "wii" ? 0 : -1;
    }();
    if (pinned >= 0) {
        return static_cast<Style>(pinned);
    }
    return static_cast<Style>(g_style.load(std::memory_order_acquire));
}

uint32_t Generation() noexcept {
    return g_generation.load(std::memory_order_acquire);
}

void OnSdlEvent(const SDL_Event& event) noexcept {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat && !Hotkey(event.key)) {
            Set(Style::Keyboard, "a key");
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        Set(IsWiiPad(event.gbutton.which) ? Style::Wii : Style::Controller, "a button");
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        // A stick or trigger pushed well past its rest (16384 of 32767, as the bindings' captures).
        const int value = event.gaxis.value;
        if ((value >= 16384 || value <= -16384) && !IsWiiPad(event.gaxis.which)) {
            Set(Style::Controller, "a stick");
        }
        break;
    }
    default:
        break;
    }
}

void NoteKeyboard(uint32_t held) noexcept {
    static uint32_t s_last = 0;
    const uint32_t pressed = held & ~s_last;
    s_last = held;
    if (pressed != 0) {
        Set(Style::Keyboard, "a key");
    }
}

void NoteController(uint32_t port, uint32_t held) noexcept {
    static std::array<uint32_t, PAD_MAX_CONTROLLERS> s_last{};
    if (port >= s_last.size()) {
        return;
    }
    const uint32_t pressed = held & ~s_last[port];
    s_last[port] = held;
    if (pressed != 0) {
        Set(Style::Controller, "a controller");
    }
}

void NoteWiiRemote(uint32_t source, uint32_t held) noexcept {
    static std::array<uint32_t, 8> s_last{};
    if (source >= s_last.size()) {
        return;
    }
    const uint32_t pressed = held & ~s_last[source];
    s_last[source] = held;
    if (pressed != 0) {
        Set(Style::Wii, "a Wii Remote");
    }
}

void NoteTouch() noexcept {
    Set(Style::Wii, "a tap");
}

} // namespace PromptStyle
