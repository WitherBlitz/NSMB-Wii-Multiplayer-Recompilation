#pragma once

// The first-start chooser for the game files on Windows and Linux (game_folder.cpp asks it):
// "Open ISO", "or", "Open Extracted Disc Folder", then Mods (only "Coming Soon!" for now), with Help
// and Cancel. Windows draws it with a
// native dialog (game_picker_win32.cpp), Linux with an SDL window and Dear ImGui
// (game_picker_sdl.cpp). Both run before aurora brings up the game window.
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace GamePicker {

// What the chooser shows, in the product's names (runtime_product.h).
struct Text {
    std::string title;        // the window title: the game's name
    std::string body;         // what to pick
    std::string help;         // the steps behind Help
    std::string isoTitle;     // the file picker's title
    std::string folderTitle;  // the folder picker's title
    bool mods = true;         // the Mods button: the original game's build only
};

enum class Choice { Iso, Folder, Cancel };

struct Picked {
    Choice choice = Choice::Cancel;
    std::filesystem::path path;
};

// Shows the chooser, with `problem` (when not empty) in red under the text, until the player picks
// a disc image or a folder, or cancels. Cancelling a file or folder picker keeps the chooser open.
Picked Show(const Text& text, const std::string& problem);

using Progress = std::function<void(uint64_t done, uint64_t total)>;
using Job = std::function<std::string(const Progress& progress, const std::atomic<bool>& cancel)>;

// Runs `job` while a progress window (titled text.title, with `line` and a Cancel button) is up.
// Cancel sets `cancel`; the job is expected to stop soon after. Returns what the job returned.
std::string RunWithProgress(const Text& text, const std::string& line, const Job& job);

// An error message box over the chooser (titled text.title), with OK; returns once it's closed.
void ShowError(const Text& text, const std::string& message);

// Puts away whatever the chooser kept open between calls (Linux keeps its window). Call it once
// the game folder is settled, before aurora creates the game window.
void Close();

} // namespace GamePicker

#endif
