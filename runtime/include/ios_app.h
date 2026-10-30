#pragma once

// The iOS app around the game (runtime/src/platform/ios/ios_app.mm): what the Android app's Java
// side does there (LauncherActivity's import, GameActivity's on-screen remote, motion and Back menu),
// done in UIKit inside the one process. On iOS SDL runs the game on the main thread, so everything
// here is called on that thread and keeps UIKit's run loop turning while it waits.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

struct SDL_Window;

namespace IosApp {

// Before anything reads the disc: uses Documents/game when it holds the right game, else explains
// the Dolphin steps and imports a folder the player picks (copied into Documents/game). False when
// there is no game to run.
bool EnsureGameFolder();

// Once aurora has made the game's window: the on-screen sideways Wii Remote, tilt and shake, taps on
// the game's menus, and pausing when the app leaves the screen.
void AttachToWindow(SDL_Window* window);

// Paused in the background (host_suspend.cpp): one turn of the main run loop, so the notice that
// the app is back can arrive on the thread that is waiting for it.
void PumpWhilePaused();

// LAN play starts a fresh process (netplay_start.cpp). iOS can't start an app again by itself: ask
// the player to open it again, then end this run.
[[noreturn]] void RestartForSession();

} // namespace IosApp

#endif
