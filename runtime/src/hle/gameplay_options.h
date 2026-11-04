#pragma once

// The Settings screen's Gameplay tab (No Time Limit, Infinite Lives) and the file select's P1
// character, one a save file. Each is read from Config.toml at every retrace, so a change applies at
// once. Lockstep (LAN) play keeps the game's own rules: every machine has to run the same ones.
namespace GameplayOptions {

// Once a retrace, on the game thread.
void OnRetrace();

// The save file the file select has highlighted (0-2): its P1 character is the one played.
void SetActiveFile(int file);

} // namespace GameplayOptions
