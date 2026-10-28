#pragma once

#include <cstdint>

// The Wii Remote speaker (WPADControlSpeaker / WPADSendStreamData, HLE'd in hle/input/wpad.cpp).
//
// The game always gets a working speaker on every channel, the same on every machine (LAN play
// stays in step), so the sounds it routes to the remotes (a player in a bubble, items, the
// remote's own jingles) are mixed and encoded as on the console. Where they play:
//   - a real Wii Remote on the channel: on its own speaker, from the game's ADPCM stream;
//   - otherwise (keyboard, gamepad, touch): on the PC or phone speakers, mixed in from the
//     remote's 6 kHz bus by the AX mixer (PlaysOnHost).
namespace WpadSpeaker {

// True while the game has remote `chan`'s speaker on and unmuted and no real remote there plays
// it itself. Read by the audio mixer, any thread.
bool PlaysOnHost(uint32_t chan);

} // namespace WpadSpeaker
