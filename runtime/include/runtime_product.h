#pragma once

#include <cstdint>
#include <string_view>

namespace RuntimeProduct {

enum class Kind {
    BaseGame,
    RetroRewind,
};

struct Descriptor {
    Kind kind;
    std::string_view displayName;  // the window title
    // The disc the translated code came from: its ID (boot.bin), how to name it to the player, and
    // optionally a file of a mod's own code whose size tells its version (the code is baked in).
    std::string_view gameId = "SMNE01";
    std::string_view discName = "New Super Mario Bros. Wii for the USA (SMNE01), revision 1";
    std::string_view versionFile = {};
    uint64_t versionFileSize = 0;
    // The game's save in the NAND (network play hands it to every player); mods rename it.
    std::string_view saveFile = "title/00010000/534d4e45/data/wiimj2d.sav";
    // Settings, saves and caches go to a folder of their own (Documents\<this> on Windows, a
    // subfolder of that name elsewhere) so a mod never takes the base game's game folder or saves.
    std::string_view userDataFolder = {};
    // Windows and Linux: the extracted game in a folder of this name beside the program is used by
    // itself (game_folder.cpp), so a mod's own program, in the same folder, never takes the base game's.
    std::string_view programGameFolder = "game";
};

// Each public executable links exactly one small provider definition. Keeping
// this selection out of target-wide preprocessor definitions lets the native
// runtime be compiled once and shared by every product.
const Descriptor& Active() noexcept;

inline bool IsRetroRewind() noexcept {
    return Active().kind == Kind::RetroRewind;
}

} // namespace RuntimeProduct
