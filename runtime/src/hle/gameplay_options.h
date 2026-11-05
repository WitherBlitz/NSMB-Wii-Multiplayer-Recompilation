#pragma once

// The Settings screen's Gameplay tab (No Time Limit, Infinite Lives) and the character each save file
// remembers for P1. Each is read from Config.toml at every retrace, so a change applies at once.
// Lockstep (LAN) play keeps the game's own rules: every machine has to run the same ones.
namespace GameplayOptions {

// Once a retrace, on the game thread.
void OnRetrace();

// The save file the file select has highlighted (0-2): P1's character starts as its last one, and
// Infinite Lives or the Power-Up mark it "Cheated" (RuntimeConfigFile::FileCheated) when they do
// something. -1 before the file select.
void SetActiveFile(int file);
int ActiveFile();

// The Gameplay tab's Power-Up: P1 gets it in a course (as from an item block, once the pause is
// over) or on the world map; picked on the title screen or the file select, once the game is on.
// The list (PowerUpName): the game's own power-up numbers, then a Star, then on Newer its Hammer Suit.
int PowerUpCount();
const char* PowerUpName(int powerUp);
void GivePowerUp(int powerUp);
bool PowerUpWaiting();  // picked in the menus, not given yet

// Once a frame, on the game thread at a point where it can call the game's own functions (KPAD).
void OnGameThread();

} // namespace GameplayOptions
