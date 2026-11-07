#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The player's own copy of New Super Mario Bros. Wii, extracted with Dolphin ("Extract Entire Disc"):
// a folder holding files/ and sys/. This port runs only the USA disc, revision 1 (SMNE01 rev 1);
// other revisions place the game's code elsewhere.
namespace GameFolder {

// The extracted game at `path` (or its DATA subfolder); empty when there is none.
std::filesystem::path Find(const std::filesystem::path& path);

// Why the game at `root` can't be used ("" when it can): not an extracted disc, or another game or
// revision, read from sys/boot.bin.
std::string Problem(const std::filesystem::path& root);

// What a disc is to this build, from its ID and revision (boot.bin bytes 0-5 and 7).
enum class Disc { Supported, NeedsSupportModule, Unsupported };
Disc Identify(std::string_view id, int revision);

// A support module lets this build run a disc it wasn't translated from (the PAL disc, SMNP01
// revision 1, for the base game). Not built yet: For() finds none, so such a disc gets the same
// "That is SMNP01, not ..." answer as any other.
namespace SupportModule {
struct Offer {
    std::string message;         // what the player is told
    uint64_t downloadBytes = 0;  // 0 when there's nothing to download
};
std::optional<Offer> For(std::string_view id, int revision);
} // namespace SupportModule

// At startup, before anything reads the disc. Windows and Linux: a game folder that is missing or
// wrong is asked for (Open ISO, or Open Extracted Disc Folder; a disc image is extracted once, next
// to the program or into the data folder), and saved to Config.toml. A "game" folder next to the
// program is picked up by itself. Elsewhere the app supplies the folder.
// Returns false when the player gave up (the program then quits).
bool EnsureConfigured();

} // namespace GameFolder
