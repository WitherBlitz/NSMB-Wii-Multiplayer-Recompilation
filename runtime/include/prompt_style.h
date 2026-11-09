#pragma once

#include <cstdint>

union SDL_Event;

// NSMBW: which buttons the game's on-screen prompts show ("Press (2) to Start"): the Wii Remote's (the
// game's own icons), the player's keys, or a controller's. It follows the input last used, like the
// prompts of a PC game: Wii icons at every start (the title screen included), keys once a key is
// really pressed (a connected keyboard alone never counts, nor the window's hotkeys), a controller's
// buttons once one of its buttons or sticks is used, and the Wii icons again on a Wii Remote or the
// phone's on-screen remote. Per machine and never saved; render-only (key_caps.h draws it), so LAN
// peers can each see their own.
namespace PromptStyle {

enum class Style : uint8_t { Wii, Keyboard, Controller };

Style Current() noexcept;
// Changes whenever Current does.
uint32_t Generation() noexcept;

// From the window's event loop (settings_overlay): key presses (the hotkeys left out) and gamepad
// buttons and sticks; a Wii Remote's own events count as the Wii Remote.
void OnSdlEvent(const SDL_Event& event) noexcept;
// From the remote reads: newly pressed buttons of the keyboard's remote (scripted keys too), of a
// controller's, and of a real or scripted Wii Remote (source = its channel, 0-3) or the phone's on-screen
// remote (source kTouchSource).
constexpr uint32_t kTouchSource = 4;
void NoteKeyboard(uint32_t heldButtons) noexcept;
void NoteController(uint32_t port, uint32_t heldButtons) noexcept;
void NoteWiiRemote(uint32_t source, uint32_t heldButtons) noexcept;
// A tap on the phone's screen (the menus take taps): the on-screen remote's prompts.
void NoteTouch() noexcept;

} // namespace PromptStyle
