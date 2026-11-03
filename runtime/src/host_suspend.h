#pragma once

#include <cstdint>

struct CpuContext;

// NSMBW: the game paused while its app is in the background (Android: the activity left the screen).
//
// The platform asks for a pause from any thread; the game thread acts on it at its next VI retrace. With
// a backup asked for, it first saves the progress so far to the player's file (the world map's Save,
// SaveGame(_, false), as the autosave does), lets the game's NAND thread finish writing, then blocks
// until resumed. If Android kills the app meanwhile, the next start continues from that save.
// Not a Quick Save: the game treats a finished Quick Save as "save and quit", and deleting it again
// from the retrace (where OnRetrace runs) woke the boot thread; either way, back in the app, the game
// ended a few seconds later (0.3.0 and 0.3.1).
//
// The guest's clocks (time base, retrace deadlines) skip the paused time: the game resumes where it
// stopped, with nothing to catch up.
namespace HostSuspend {

// backup: Android leaving the app (a backup save first); the screen turning off pauses
// without one (the game just waits in memory).
void RequestPause(bool backup);
void RequestResume();
bool Paused();

// The game thread, at each retrace (before VI computes due retraces). Returns how long it was blocked,
// in nanoseconds (0 when it wasn't).
uint64_t OnRetrace();

// The game thread reading the remote (KPADReadEx, channel 0), once a frame: a backup save is
// started from here, where the game itself saves - a guest thread that may wait (the scheduler's idle
// loop, where OnRetrace runs, may not: a save that had to wait there resumed the boot thread, and the
// game exited). So is the autosave: back on the world map after a course's goal, the game's own Save
// (SaveGame(_, false)) runs with no prompt once the map has recorded the clear.
void OnGameThread();

// Total time the game has spent paused: subtracted from the guest's time base.
uint64_t PausedNanoseconds();

} // namespace HostSuspend
