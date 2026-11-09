#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// NSMBW: the keyboard's sideways Wii Remote, one or more keys or mouse buttons per control,
// changeable from the game's settings window (Keybinds tab) and saved to Config.toml's [keyboard]
// table as SDL scancode names and "Mouse L/M/R/4/5" ("jump = \"X,Space\""). By default right click
// is Down (crouch); mouse buttons are the desktop's only. A control can also have combinations, 2 or
// 3 keys held together (combo_engine.h has the rules), saved beside it as a TOML array of member
// lists ("jump_combos = [[\"Left Ctrl\", \"X\"]]").
namespace Keybinds {

enum Action : int {
    kUp,
    kDown,
    kLeft,
    kRight,
    kJump,      // 2
    kRun,       // 1
    kA,
    kShake,
    kTiltLeft,
    kTiltRight,
    kPlus,
    kMinus,
    kActionCount,
};

const char* ActionName(int action);
// The action's Config.toml key ("jump"), also used for [controller] combinations.
const char* ActionKey(int action);
// "X / Space / Left Ctrl + X", or "None".
std::string KeyLabel(int action);
// Each of the action's bindings as its keys' names: its single keys first ({"X"}, {"Space"}), then its
// combinations, modifiers first ({"Left Ctrl", "X"}). The settings window draws them as key caps.
std::vector<std::vector<std::string>> Bindings(int action);
// The action's combinations alone ("Left Ctrl + X / Shift + Z"), or "".
std::string ComboLabel(int action);
// The keys of the action's first binding: its first key ("X"), else its first combination's members
// ("Left Ctrl", "X"); empty when nothing is bound. The button prompts show these (key_caps.h).
std::vector<std::string> FirstBinding(int action);
// Removes the action's combinations (its single keys stay).
void ClearCombos(int action);
// Reads what is held for the combinations; the keyboard's remote (virtual_remote.cpp) calls it once
// a read, before Held. Releases everything while `active` is false (no keyboard focus).
void StepCombos(bool active);
// Whether a key's own binding is held back right now (taken by a combination, or waiting for one).
bool Silenced(int scancode);
// Whether a key was tapped (let go while still waiting for a combination): it counts once.
bool Tapped(int scancode);
// Test seam: NSMBW_KEY_SCRIPT="2000:Left Ctrl+X:300,..." holds those keys (SDL names, "+" between
// them) for <ms> from <time> ms after the first keyboard read (in deterministic mode: after boot, on the
// virtual clock), as if pressed, rebinding captures included, and the keyboard then counts without the
// window's focus. True while the variable is set.
bool Scripted();
// Whether NSMBW_KEY_SCRIPT holds that key (an SDL scancode) now: for the other readers of the keyboard
// (a controller binding's capture: Escape, Delete).
bool ScriptKeyHeld(int scancode);
// Any of the action's keys held (false while a key is being captured, or until it is let go).
bool Held(int action);
// Held, leaving one key (an SDL scancode) out; and whether that key is one of the action's.
bool HeldIgnoring(int action, int scancode);
bool Bound(int action, int scancode);

// Space Smart Spin QoL (on by default): Space, when it is a Jump key, pressed while the player is
// in the air shakes the remote (a spin) instead of pressing 2. Saved as [keyboard] smart_spin.
bool SmartSpin();
void SetSmartSpin(bool on);

// Reverse Tilt QoL (on by default): every remote's tilt, from the phone's motion sensors, a
// controller or the keyboard, reaches the game mirrored, so a platform the player tilts goes the way
// the phone or stick goes. Saved as [keyboard] reverse_tilt.
bool ReverseTilt();
void SetReverseTilt(bool on);

// Mouse Gyro QoL (desktop; off by default, idle while a controller or Wii Remote is connected): the
// mouse stands in for the remote's motion. Moved left or right, it tilts the remote that way for
// 5 s (the tilting platforms); moved left and right quickly, it shakes. Saved as [keyboard] mouse_gyro.
bool MouseGyro();
void SetMouseGyro(bool on);

// A capture: the keys (or mouse buttons) pressed together, up to 3, are taken when they are all let
// go. One key becomes the action's key (in place of its keys, as before); 2 or 3 become a combination
// added to the action's. Escape pressed first cancels, Delete or Backspace pressed first clears the
// action (its keys and combinations); 10 s with nothing held cancels.
void BeginCapture(int action);
// The action waiting for keys, or -1.
int Capturing();
// The keys held so far in a capture, "Left Ctrl + X", or "" before the first.
std::string CaptureLabel();
// The same as the keys' names ({"Left Ctrl", "X"}), empty before the first.
std::vector<std::string> CaptureKeys();
// Polls the keyboard during a capture; true once it has ended (bound, cleared or cancelled).
bool PollCapture();
// Keys held from a capture are ignored until they are released.
bool Suppressed();
void ResetDefaults();

// Every binding the button prompts show (key_caps.h): these keys and combinations, a controller's
// buttons (aurora's mapping, set in settings_overlay.cpp) and its combinations (pad_combos.cpp). Each
// change bumps the generation, so the prompts repaint at once; NoteBindingsChanged is for the writers
// outside this file.
void NoteBindingsChanged();
uint32_t BindingsGeneration();

} // namespace Keybinds
