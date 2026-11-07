#include "game_folder.h"

#include "game_picker.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"
#include "wii_disc.h"

#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace GameFolder {
namespace {

namespace fs = std::filesystem;

bool IsExtractedDisc(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path / "files", ec) && fs::is_regular_file(path / "sys" / "fst.bin", ec);
}

// The game this build was translated from (runtime_product.h: the original, or a mod such as Newer).
std::string GameName() {
    return std::string(RuntimeProduct::Active().displayName);
}

// Why a disc with this ID and revision can't be used ("" when it can).
std::string IdProblem(const std::string& id, int revision) {
    const Disc kind = Identify(id, revision);
    if (kind == Disc::Supported) {
        return {};
    }
    if (kind == Disc::NeedsSupportModule) {
        // The PAL support module hooks in here: once For() offers one, the chooser asks to install it.
        if (const std::optional<SupportModule::Offer> offer = SupportModule::For(id, revision)) {
            return offer->message;
        }
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s revision %d needs a support module; none is available\n", id.c_str(),
                revision);
    }
    const std::string disc(RuntimeProduct::Active().discName);
    if (id != RuntimeProduct::Active().gameId) {
        return "That is " + id + ", not " + disc + ". This build runs only that disc: it was translated from it.";
    }
    return "That is " + id + " revision " + std::to_string(revision) + ". This build runs only " + disc +
           " (Dolphin shows the revision under Properties > Info).";
}

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
// A disc image is extracted into "<target>.importing" first, with this marker inside, and renamed to
// its target once complete. A folder that still has the marker was left by an import that never
// finished (a crash, or the program killed), so it can go.
constexpr const char* kImportingSuffix = ".importing";
constexpr const char* kImportingMarker = ".nsmbw-importing";

fs::path ProgramDirectory() {
    return RuntimeConfigFile::ExecutableDirectory().value_or(fs::path{});  // the compat build's too
}

// What to extract, and how: the chooser's Help.
std::string Steps(const char* openDolphin) {
    return "This port runs your own copy of " + std::string(RuntimeProduct::Active().discName) +
           ", from a disc you own. Choose one of these:\n\n"
           "Open ISO: the disc image you dumped from your disc (.iso or .wbfs). It's extracted once, next to "
           "this program or into its data folder, and the game reads that copy from then on.\n\n"
           "Open Extracted Disc Folder: a folder Dolphin extracted the disc to.\n"
           "1. " + openDolphin + " and find " + GameName() + " in the game list.\n"
           "2. Right-click it, choose Properties, then the Filesystem tab.\n"
           "3. Right-click the disc at the top and choose Extract Entire Disc, then pick an empty folder.\n"
           "4. Choose that folder here. Keep it: the game reads it every time.\n\n"
           "A compressed image (.rvz, .wia, .gcz) has to become an .iso first: in Dolphin, right-click the "
           "game, Convert File..., Format: ISO.\n\n"
           "(Dolphin shows the revision under Properties > Info. Or put the extracted folder next to this "
           "program and name it \"game\".)";
}

GamePicker::Text PickerText() {
    GamePicker::Text text;
    text.title = GameName();
    text.body = "This port runs your own copy of " + std::string(RuntimeProduct::Active().discName) +
                ". Open your disc image, or the folder Dolphin's Extract Entire Disc made from it.";
#if defined(_WIN32)
    text.help = Steps("Open Dolphin on this computer");
#else
    text.help = Steps("Open Dolphin");
#endif
    text.isoTitle = "Choose your disc image of " + GameName();
    text.folderTitle = "Choose the folder Dolphin extracted " + GameName() + " to";
    return text;
}

// "that isn't a Wii disc image" -> "That isn't a Wii disc image."
std::string Sentence(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        text += '.';
    }
    return text;
}

// Removes a folder this program made. Windows Defender may hold a file it has just been handed for a
// moment, so a failure is retried.
bool RemoveTree(const fs::path& path) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (!fs::exists(fs::symlink_status(path, ec))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool RenameTree(const fs::path& from, const fs::path& to, std::error_code& ec) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        fs::rename(from, to, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// Where a disc image is extracted, best first: "game" next to the program (picked up there at every
// start, so two installs keep two games), then "game" in the data folder. A portable install keeps
// everything in its data folder.
std::vector<fs::path> ExtractTargets() {
    const fs::path data = RuntimeConfigFile::ApplicationDataDirectory() / "game";
    const fs::path program = ProgramDirectory();
    if (RuntimeConfigFile::PortableRootDirectory() || program.empty()) {
        return {data};
    }
    return {program / "game", data};
}

fs::path Staging(const fs::path& target) {
    fs::path staging = target;
    staging += kImportingSuffix;
    return staging;
}

void CleanStaleImports() {
    for (const fs::path& target : ExtractTargets()) {
        const fs::path staging = Staging(target);
        std::error_code ec;
        if (fs::is_regular_file(staging / kImportingMarker, ec)) {
            RT_LOGF(RT_TAG_RUNTIME, "disc image: removing an unfinished import at %s\n",
                    RuntimeConfigFile::PathToUtf8(staging).c_str());
            RemoveTree(staging);
        }
    }
}

std::string Megabytes(uint64_t bytes) {
    return std::to_string((bytes + (1u << 20) - 1) >> 20);
}

// Extracts the disc image at `image` and makes it the game folder. False when it can't: `problem`
// then says why, or is empty when the player cancelled.
bool ImportDiscImage(const fs::path& image, std::string& problem) {
    std::string error;
    const std::unique_ptr<WiiDisc::Disc> disc = WiiDisc::Disc::Open(image, error);
    if (!disc) {
        problem = Sentence(error);
        return false;
    }
    // The ID and revision first, before anything is written.
    problem = IdProblem(disc->Id(), disc->Revision());
    if (!problem.empty()) {
        return false;
    }

    fs::path target, staging, taken;
    for (const fs::path& candidate : ExtractTargets()) {
        const fs::path folder = Staging(candidate);
        std::error_code ec;
        // Never write over a folder this program didn't make: a "game" next to the program may be a
        // link to the player's own Dolphin extract.
        if (fs::exists(fs::symlink_status(candidate, ec))) {
            taken = candidate;
            continue;
        }
        if (fs::exists(fs::symlink_status(folder, ec))) {
            taken = folder;
            continue;
        }
        fs::create_directories(candidate.parent_path(), ec);
        if (!fs::create_directory(folder, ec)) {
            continue;  // can't write here (Program Files, read-only media, /opt)
        }
        std::ofstream marker(folder / kImportingMarker);
        marker << "An unfinished import of a disc image; removed at the next start.\n";
        marker.close();
        if (!marker) {
            RemoveTree(folder);
            continue;
        }
        target = candidate;
        staging = folder;
        break;
    }
    if (target.empty()) {
        if (!taken.empty()) {
            problem = "There's already a folder named " + RuntimeConfigFile::PathToUtf8(taken.filename()) + " in " +
                      RuntimeConfigFile::PathToUtf8(taken.parent_path()) + ". Move or delete it, then try again.";
        } else {
            problem = "Couldn't make a folder for the game next to this program or in " +
                      RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ApplicationDataDirectory()) + ".";
        }
        return false;
    }

    const uint64_t size = disc->ExtractedSize();
    std::error_code spaceError;
    const fs::space_info space = fs::space(target.parent_path(), spaceError);
    if (!spaceError && space.available < size + (64u << 20)) {
        RemoveTree(staging);
        problem = "There isn't enough free space in " + RuntimeConfigFile::PathToUtf8(target.parent_path()) +
                  ": the game takes " + Megabytes(size) + " MB.";
        return false;
    }

    RT_LOGF(RT_TAG_RUNTIME, "disc image: extracting %s (%s revision %d) to %s\n",
            RuntimeConfigFile::PathToUtf8(image).c_str(), disc->Id().c_str(), disc->Revision(),
            RuntimeConfigFile::PathToUtf8(staging).c_str());
    const auto start = std::chrono::steady_clock::now();
    int files = 0;
    const std::string result = GamePicker::RunWithProgress(
        PickerText(), "Extracting the disc image",
        [&](const GamePicker::Progress& progress, const std::atomic<bool>& cancel) {
            return disc->Extract(staging, progress, cancel, files);
        });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (result == "cancelled") {
        RT_LOGF(RT_TAG_RUNTIME, "disc image: cancelled after %.1f s\n", seconds);
        RemoveTree(staging);
        problem.clear();
        return false;
    }
    if (!result.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "disc image: failed after %.1f s: %s\n", seconds, result.c_str());
        RemoveTree(staging);
        problem = "Couldn't extract the disc image: " + result + ". Nothing was changed.";
        return false;
    }
    // What a folder is checked for: the code modules, and a mod's version.
    problem = Problem(staging);
    if (!problem.empty()) {
        RemoveTree(staging);
        return false;
    }
    std::error_code ec;
    fs::remove(staging / kImportingMarker, ec);
    if (!RenameTree(staging, target, ec)) {
        RemoveTree(staging);
        problem = "Couldn't extract the disc image: couldn't rename " + RuntimeConfigFile::PathToUtf8(staging) +
                  " (" + ec.message() + "). Nothing was changed.";
        return false;
    }
    RT_LOGF(RT_TAG_RUNTIME, "disc image: %s -> %s (%llu bytes, %d files, %.1f s)\n",
            RuntimeConfigFile::PathToUtf8(image).c_str(), RuntimeConfigFile::PathToUtf8(target).c_str(),
            static_cast<unsigned long long>(size), files, seconds);

    if (target.parent_path() == RuntimeConfigFile::ResolveConfigPath().parent_path()) {
        // Beside Config.toml (the data folder): saved as relative, so a portable install can move.
        RuntimeConfigFile::SetDvdRoot(fs::path("game"));
    } else if (RuntimeConfigFile::DvdRoot().empty()) {
        RuntimeConfigFile::SetDvdRoot(target);
    } else {
        RuntimeConfigFile::UseDvdRootForThisRun(target);  // next to the program: found there every start
    }
    return true;
}
#endif

} // namespace

fs::path Find(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    if (IsExtractedDisc(path)) {
        return path;
    }
    if (IsExtractedDisc(path / "DATA")) {
        return path / "DATA";
    }
    return {};
}

Disc Identify(std::string_view id, int revision) {
    const RuntimeProduct::Descriptor& product = RuntimeProduct::Active();
    if (id == product.gameId) {
        return revision == 1 ? Disc::Supported : Disc::Unsupported;
    }
    // The PAL disc, revision 1, for the base game.
    if (product.gameId == "SMNE01" && id == "SMNP01" && revision == 1) {
        return Disc::NeedsSupportModule;
    }
    return Disc::Unsupported;
}

namespace SupportModule {
std::optional<Offer> For(std::string_view, int) {
    return std::nullopt;  // none exists yet
}
} // namespace SupportModule

std::string Problem(const fs::path& root) {
    if (!IsExtractedDisc(root)) {
        return "That folder isn't an extracted game: it needs the files and sys folders Dolphin's Extract "
               "Entire Disc makes.";
    }
    std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
    char header[8] = {};
    if (!boot.read(header, sizeof(header))) {
        return "That folder has no sys/boot.bin, so the game can't be identified.";
    }
    const std::string id(header, 6);
    const int revision = static_cast<unsigned char>(header[7]);
    const RuntimeProduct::Descriptor& product = RuntimeProduct::Active();
    const std::string disc(product.discName);
    if (std::string problem = IdProblem(id, revision); !problem.empty()) {
        return problem;
    }
    // The game's code modules were translated with the rest: a disc whose modules were changed (a
    // repacked or modded image keeps the disc ID) would link nothing and stop at the first scene.
    // Each is the original's LZ file or, decompressed, the original's .rel. Newer's disc has the same.
    struct Module {
        const char* name;
        uintmax_t lzSize, relSize;
    };
    static constexpr Module kModules[] = {
        {"d_profileNP", 8791, 24244},
        {"d_basesNP", 1190805, 3019336},
        {"d_enemiesNP", 779805, 2137424},
        {"d_en_bossNP", 229984, 605552},
    };
    for (const Module& module : kModules) {
        const std::string lz = "files/rels/" + std::string(module.name) + ".rel.LZ";
        const std::string rel = "files/rels/" + std::string(module.name) + ".rel";
        std::error_code lzError, relError;
        const uintmax_t lzSize = fs::file_size(root / fs::u8path(lz), lzError);
        const uintmax_t relSize = fs::file_size(root / fs::u8path(rel), relError);
        if ((!lzError && lzSize == module.lzSize) || (!relError && relSize == module.relSize)) {
            continue;
        }
        if (lzError && relError) {
            return "That copy of " + id + " has no " + lz + ". This build needs " + disc +
                   " as Dolphin's Extract Entire Disc makes it, with nothing changed or left out.";
        }
        const std::string& present = lzError ? rel : lz;
        return "That copy of " + id + " has a changed " + present + " (" + std::to_string(lzError ? relSize : lzSize) +
               " bytes, not " + std::to_string(lzError ? module.relSize : module.lzSize) +
               "): it isn't the original disc (a modded or repacked image?). This build runs only " + disc +
               ", unchanged: it was translated from that disc's code.";
    }
    // A mod keeps its disc ID from version to version; its own code tells them apart.
    if (!product.versionFile.empty()) {
        const std::string file = "files/" + std::string(product.versionFile);
        std::error_code ec;
        const uintmax_t size = fs::file_size(root / fs::u8path(file), ec);
        if (ec) {
            return "That is " + id + ", but it has no " + file + ". This build runs only " + disc + ".";
        }
        if (size != product.versionFileSize) {
            return "That is another version of " + GameName() + " (its " + file + " is " + std::to_string(size) +
                   " bytes, not " + std::to_string(product.versionFileSize) + "). This build runs only " + disc +
                   ": it was translated from that version's code.";
        }
    }
    return {};
}

bool EnsureConfigured() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    CleanStaleImports();
    // A "game" folder next to the program needs no questions, and wins over the saved folder: every
    // install shares Documents\MarioWiiSaveData, so this is how two installs play two games (say the
    // original and a mod), each with its own saves (nand_path.h keeps saves per game).
    if (const fs::path beside = Find(ProgramDirectory() / "game"); !beside.empty() && Problem(beside).empty()) {
        if (RuntimeConfigFile::DvdRoot().empty()) {
            RuntimeConfigFile::SetDvdRoot(beside);
        } else {
            RuntimeConfigFile::UseDvdRootForThisRun(beside);
        }
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(beside).c_str());
        return true;
    }
#endif
    const fs::path configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
    if (!configured.empty() && Problem(configured).empty()) {
        return true;
    }
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    std::string problem;
    if (!RuntimeConfigFile::DvdRoot().empty()) {
        const fs::path saved = RuntimeConfigFile::ResolvedDvdRoot();
        std::error_code ec;
        problem = fs::exists(saved, ec) ? Problem(configured.empty() ? saved : configured)
                                        : "The game folder you chose before (" + RuntimeConfigFile::PathToUtf8(saved) +
                                              ") isn't there any more.";
    }
    const GamePicker::Text text = PickerText();
    for (;;) {
        const GamePicker::Picked picked = GamePicker::Show(text, problem);
        if (picked.choice == GamePicker::Choice::Cancel) {
            GamePicker::Close();
            return false;
        }
        if (picked.choice == GamePicker::Choice::Iso) {
            if (!ImportDiscImage(picked.path, problem)) {
                continue;
            }
        } else {
            const fs::path root = Find(picked.path);
            problem = Problem(root.empty() ? picked.path : root);
            if (!problem.empty()) {
                continue;
            }
            RuntimeConfigFile::SetDvdRoot(root);
        }
        GamePicker::Close();
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n",
                RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolvedDvdRoot()).c_str());
        return true;
    }
#else
    // The app chose and checked the folder; what this side finds still goes into the log.
    if (const std::string problem = Problem(configured.empty() ? RuntimeConfigFile::ResolvedDvdRoot() : configured);
        !problem.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "game folder problem: %s\n", problem.c_str());
    }
    return true;
#endif
}

} // namespace GameFolder
