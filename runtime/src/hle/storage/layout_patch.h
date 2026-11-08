#pragma once

#include <filesystem>
#include <string>

// NSMBW: the pause window gets a third button, Settings, that is the game's own: its archive
// (/Layout/pauseMenu/pauseMenu.arc) is served with Exit's button copied under new names between
// Continue and Exit, the window grown by one button and re-centred, the copy's animation group
// (registered under a name the game already holds, B00_yesButton) and its tracks in the animations. hle/nsmbw_menu_settings.cpp
// teaches the pause window's code the button: the game animates it, moves its cursor to it and plays
// its sounds. Patched while the disc index is built, from whatever archive the game would read (the
// disc's, Newer's or a mod's); an archive whose panes aren't the expected ones is left as it is, and
// so is every archive in deterministic (LAN) play, where the two-button window stays: every peer
// decides the same way at boot.
namespace LayoutPatch {

// Italy's EU archives have the same layouts the USA game expects, but their
// U8 entries use EU resource names. Return a cached, USA-named copy when one
// of the known Italian language archives is supplied.
std::filesystem::path AdaptLanguageArchive(const std::string& dvdPath, const std::filesystem::path& hostPath);

// dvd.cpp, once per disc path the game may read, after the overlays: a patched copy of the file to
// serve in its place, or empty to serve it as it is.
std::filesystem::path Patch(const std::string& dvdPath, const std::filesystem::path& hostPath);

// The pause window's archive is served with the Settings button.
bool PauseButtonPatched() noexcept;
// The same for the world map's + menu (/Layout/corseSelectMenu/corseSelectMenu.arc; the disc's game only):
// Settings under Quick Save, Title Screen below it.
bool MapButtonPatched() noexcept;

// The patched pause layout's pitch between two buttons, in layout units (70 on the disc, 76 in Newer).
float PausePitch() noexcept;

} // namespace LayoutPatch
