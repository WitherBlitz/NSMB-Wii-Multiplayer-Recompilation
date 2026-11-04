#pragma once

// The Settings screen's Gameplay tab (No Time Limit, Infinite Lives) and the file select's P1
// character, one a save file. Each is read from Config.toml at every retrace, so a change applies at
// once. Lockstep (LAN) play keeps the game's own rules: every machine has to run the same ones.
namespace GameplayOptions {

// Once a retrace, on the game thread.
void OnRetrace();

// The save file the file select has highlighted (0-2): its P1 character is the one played, and
// Infinite Lives or the Power-Up mark it "Cheated" (RuntimeConfigFile::FileCheated) when they do
// something. -1 before the file select.
void SetActiveFile(int file);
int ActiveFile();

// The Gameplay tab's Power-Up: P1 gets it in a course (as from an item block, once the pause is
// over) or on the world map; picked on the title screen or the file select, once the game is on.
// Power-ups are the game's own numbers (PowerUpName); Newer adds its Hammer Suit.
int PowerUpCount();
const char* PowerUpName(int powerUp);
void GivePowerUp(int powerUp);
bool PowerUpWaiting();  // picked in the menus, not given yet

// Once a frame, on the game thread at a point where it can call the game's own functions (KPAD).
void OnGameThread();

} // namespace GameplayOptions
