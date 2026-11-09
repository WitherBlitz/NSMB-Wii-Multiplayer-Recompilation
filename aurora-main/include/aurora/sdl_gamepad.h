#pragma once

// SDL gamepad calls that never wait for a controller re-scan.
//
// SDL holds its joystick lock for all of SDL_UpdateJoysticks, and on Windows that call re-enumerates
// every DirectInput device after any device arrives or leaves (an IOCTL to each HID device, one of
// them a Bluetooth round trip): measured 150-450 ms, three times per controller connect or
// disconnect. Any other SDL gamepad call waits for it. On Windows aurora runs the update on an input
// thread (aurora::input, kicked once per frame), and the game reads controllers through these:
// - a read calls SDL when the input thread is idle and otherwise returns what the same call (same
//   gamepad, same argument) returned last;
// - a write made while the input thread is busy is queued and applied, in order, right after it.
// On other platforms they are the plain SDL calls.
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_sensor.h>

namespace aurora::sdl {
bool GetGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button) noexcept;
Sint16 GetGamepadAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) noexcept;
SDL_Joystick* GetGamepadJoystick(SDL_Gamepad* gamepad) noexcept;
SDL_Gamepad* GetGamepadFromPlayerIndex(int playerIndex) noexcept;
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID instance) noexcept;
SDL_JoystickID GetGamepadID(SDL_Gamepad* gamepad) noexcept;
const char* GetGamepadName(SDL_Gamepad* gamepad) noexcept;
SDL_GamepadType GetGamepadType(SDL_Gamepad* gamepad) noexcept;
int GetGamepadPlayerIndex(SDL_Gamepad* gamepad) noexcept;
Uint16 GetGamepadVendor(SDL_Gamepad* gamepad) noexcept;
Uint16 GetGamepadProduct(SDL_Gamepad* gamepad) noexcept;
bool GamepadHasSensor(SDL_Gamepad* gamepad, SDL_SensorType type) noexcept;
bool GamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type) noexcept;
// At most 8 values.
bool GetGamepadSensorData(SDL_Gamepad* gamepad, SDL_SensorType type, float* data, int numValues) noexcept;
SDL_PowerState GetGamepadPowerInfo(SDL_Gamepad* gamepad, int* percent) noexcept;
bool GetJoystickButton(SDL_Joystick* joystick, int button) noexcept;
Sint16 GetJoystickAxis(SDL_Joystick* joystick, int axis) noexcept;
int GetNumJoystickButtons(SDL_Joystick* joystick) noexcept;
int GetNumJoystickAxes(SDL_Joystick* joystick) noexcept;
SDL_PowerState GetJoystickPowerInfo(SDL_Joystick* joystick, int* percent) noexcept;

// Writes: true when applied or queued.
bool RumbleGamepad(SDL_Gamepad* gamepad, Uint16 lowFrequency, Uint16 highFrequency, Uint32 durationMs) noexcept;
bool SetGamepadLED(SDL_Gamepad* gamepad, Uint8 red, Uint8 green, Uint8 blue) noexcept;
bool SetGamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type, bool enabled) noexcept;
bool SetGamepadPlayerIndex(SDL_Gamepad* gamepad, int playerIndex) noexcept;
bool SendGamepadEffect(SDL_Gamepad* gamepad, const void* data, int size) noexcept;
// Applies queued writes now, waiting for the input thread if it is busy. For exit paths.
void FlushWrites() noexcept;
} // namespace aurora::sdl
