#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PADStatus;

// NSMBW: combination bindings on controllers: 2 or 3 of a controller's buttons or axis directions held
// together for one of the keyboard's actions (Keybinds::Action), the rules in combo_engine.h. Saved
// as Config.toml [controller] "<action>_combos" lists of the [controller] button names
// ("shake_combos = [[\"left_shoulder\", \"right_shoulder\"]]"), shared by every port like the rest of
// [controller]. Decided on this device before the remote's input is sampled, so LAN peers see the
// result as plain remote buttons.
namespace PadCombos {

// After PADRead (virtual_remote.cpp): takes the members' own buttons out of `pads` while they are
// taken by a combination or waiting for one, and works out each port's combination actions.
void Apply(PADStatus* pads);
// The actions (1 << Keybinds::Action) held through combinations on `port` at the last Apply.
uint32_t Actions(uint32_t port);
// "LB / L1 + RB / R1", the action's combinations, or "".
std::string Label(int action);
// The members (native buttons, aurora's PAD encoding) of the action's first combination, or none.
std::vector<uint32_t> FirstMembers(int action);
void Clear(int action);

// A capture on `port`'s controller: the buttons pressed together (sticks and triggers pushed past
// half), up to 3, taken when all are let go. Escape on the keyboard cancels, Delete or Backspace
// clears; 10 s with nothing held cancels.
void BeginCapture(int action, uint32_t port);
// The buttons held so far, "LB / L1 + A / Cross", or "".
std::string CaptureLabel();
enum class CaptureResult { Running, Single, Combo, Cleared, Cancelled };
// Running until the capture ends. Single: one button, in `single` (the caller binds it as before);
// Combo: added to the action's combinations; Cleared: the caller clears the single, the action's
// combinations are already gone.
CaptureResult PollCapture(uint32_t& single);
bool Capturing();

} // namespace PadCombos
