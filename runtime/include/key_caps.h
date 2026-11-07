#pragma once

#include <cstdint>
#include <string>

struct ImDrawList;

// NSMBW: the game's button prompts drawn as the player's own keys or controller buttons
// (prompt_style.h picks which). The game writes its prompts ("Press (2) to Start", "Back (1)") with
// icon characters of its picture font, mj2d00_PictureFont_32_RGBA8.brfnt; this repaints that font's
// button cells - 2, 1, A, +, - and the D-pad - as key caps of the keys bound to them, or as the bound
// controller's buttons with Xbox labels, and hands the repainted sheet to aurora in place of the
// game's (AuroraSetTextureOverride). The prompts the game draws as textures of their own get the same:
// the "2" of the strap and letter screens (os_2btn_00) and the map guides' D-pad (ImIconjujiButton).
// Everything is made from Nintendo's own art on the player's disc:
// the cap is the font's own B button with its letter taken out, a face button its round 1 button,
// the labels the game's message font. Render-only: the game's memory, text and glyph widths never
// change, so LAN peers can each see their own prompts. Combinations show as small caps stacked in
// the cell. With the Wii prompts nothing is replaced.
namespace KeyCaps {

// Once a frame on the UI thread (NetplayUi::Draw): follows the prompt style, the bindings, the
// render scale and the active game folder's font (base or Newer); repaints only on a change.
void Update();

// A Wii Remote button a prompt names.
enum class Button : uint8_t { Two, One, A, Plus, Minus, DPad, B, Home };

// The runtime's own menus: `button` as the game's prompts show it now, `height` pixels tall, its left
// edge at x and centred on y. Returns the width drawn (also for width-only measuring: list null).
float Draw(ImDrawList* list, float x, float y, float height, Button button, float alpha);

// The runtime's own menus (the Keybinds tab): one keyboard key, by its SDL name or "Mouse L", as a key
// cap made like the prompts' and as wide as its label needs, `height` pixels tall, its left edge at x
// and centred on y. Returns the width (list null: measures only). Whatever the prompt style.
float DrawKey(ImDrawList* list, float x, float y, float height, const std::string& name, float alpha);

} // namespace KeyCaps
