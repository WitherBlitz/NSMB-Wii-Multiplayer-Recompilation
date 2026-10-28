#pragma once

#include <cstddef>
#include <string>

// NSMBW: the keyboard's sideways Wii Remote, one or more keys or mouse buttons per control,
// changeable from the game's settings window (Keybinds tab) and saved to Config.toml's [keyboard]
// table as SDL scancode names and "Mouse L/M/R/4/5" ("jump = \"X,Space\""). By default right click
// is Down (crouch); mouse buttons are the desktop's only.
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
// "X / Space", or "None".
std::string KeyLabel(int action);
// Any of the action's keys held (false while a key is being captured, or until it is let go).
bool Held(int action);
// Held, leaving one key (an SDL scancode) out; and whether that key is one of the action's.
bool HeldIgnoring(int action, int scancode);
bool Bound(int action, int scancode);

// Space Smart Spin QoL (on by default): Space, when it is a Jump key, pressed while the player is
// in the air shakes the remote (a spin) instead of pressing 2. Saved as [keyboard] smart_spin.
bool SmartSpin();
void SetSmartSpin(bool on);

// Reverse Tilt QoL (off by default): every remote's tilt, from the phone's motion sensors, a
// controller or the keyboard, reaches the game mirrored, so a platform the player tilts goes the way
// the phone or stick goes. Saved as [keyboard] reverse_tilt.
bool ReverseTilt();
void SetReverseTilt(bool on);

// Mouse Gyro QoL (desktop; on by default, idle while a controller or Wii Remote is connected): the
// mouse stands in for the remote's motion. Moved left or right, it tilts the remote that way for
// 5 s (the tilting platforms); moved left and right quickly, it shakes. Saved as [keyboard] mouse_gyro.
bool MouseGyro();
void SetMouseGyro(bool on);

// Waits for the next key pressed and binds it to `action` alone; Escape cancels.
void BeginCapture(int action);
// The action waiting for a key, or -1.
int Capturing();
// Polls the keyboard during a capture; true once it has ended (bound or cancelled).
bool PollCapture();
// Keys held from a capture are ignored until they are released.
bool Suppressed();
void ResetDefaults();

} // namespace Keybinds
