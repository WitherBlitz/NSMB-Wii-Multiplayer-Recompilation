#pragma once

#include <cstdint>

struct CpuContext;

// NSMBW: the game paused while its app is in the background (Android: the activity left the screen).
//
// The platform asks for a pause from any thread; the game thread acts on it at its next VI retrace. With
// a backup asked for, it first writes the game's own Quick Save of the progress so far (the same routine
// the world map's + menu uses, SaveGame(_, true)), lets the game's NAND thread finish writing, then
// blocks until resumed. If Android kills the app meanwhile, the next start offers that Quick Save:
// the player continues on the world map, at the course they were in. Back in the app, the backup is
// deleted again, so it can never override a save made later.
//
// The guest's clocks (time base, retrace deadlines) skip the paused time: the game resumes where it
// stopped, with nothing to catch up.
namespace HostSuspend {

// backup: Android leaving the app (a backup Quick Save first); the screen turning off pauses
// without one (the game just waits in memory).
void RequestPause(bool backup);
void RequestResume();
bool Paused();

// The game thread, at each retrace (before VI computes due retraces). Returns how long it was blocked,
// in nanoseconds (0 when it wasn't).
uint64_t OnRetrace();

// The game thread reading the remote (KPADReadEx, channel 0): a backup Quick Save is started from
// here, where the game itself saves - a guest thread that may wait (the scheduler's idle loop, where
// OnRetrace runs, may not: a save that had to wait there resumed the boot thread, and the game exited).
void OnGameThread();

// Total time the game has spent paused: subtracted from the guest's time base.
uint64_t PausedNanoseconds();

} // namespace HostSuspend
