#include "game_folder.h"

#include "game_picker.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"
#include "ui_text.h"
#include "wii_disc.h"

#include <cryptopp/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
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

// The one sentence a folder with any of the game's files left out gets, instead of starting and
// stopping in the boot (issue #29: part of Dolphin's extract got as far as the shaders, then closed).
// Translated in ui_text.cpp; the Android app's AppFiles.INCOMPLETE is the same sentence.
constexpr const char* kIncomplete = "The folder doesn't include all required files.";

// An FST name, or "" when it isn't plain ASCII (extractors spell other names differently, so such a
// file isn't looked for) or can't be a file name.
std::string AsciiName(const std::vector<char>& fst, size_t at) {
    std::string name;
    for (; at < fst.size() && fst[at] != 0; ++at) {
        const auto c = static_cast<unsigned char>(fst[at]);
        if (c < 0x20 || c >= 0x80 || c == '/' || c == '\\') {
            return {};
        }
        name += static_cast<char>(c);
    }
    return name == "." || name == ".." ? std::string() : name;
}

// The files the game needs that `root` doesn't have, as paths relative to it, at most `limit`:
// sys/boot.bin, sys/main.dol and sys/fst.bin, then every file the disc's own file table (fst.bin)
// lists under files/, so any region's or revision's disc passes as long as nothing was left out.
// Only whether each is there is checked (about 1200 files for the base game), never what's in it.
// The Android app does the same (GameFileList.java).
std::vector<std::string> MissingFiles(const fs::path& root, size_t limit) {
    std::vector<std::string> missing;
    std::error_code ec;
    for (const char* name : {"sys/boot.bin", "sys/main.dol", "sys/fst.bin"}) {
        if (!fs::is_regular_file(root / fs::u8path(name), ec)) {
            missing.emplace_back(name);
        }
    }
    if (!missing.empty()) {
        return missing;
    }
    std::ifstream in(root / "sys" / "fst.bin", std::ios::binary);
    const std::vector<char> fst((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto be32 = [&](size_t at) {
        return uint32_t(uint8_t(fst[at])) << 24 | uint32_t(uint8_t(fst[at + 1])) << 16 |
               uint32_t(uint8_t(fst[at + 2])) << 8 | uint32_t(uint8_t(fst[at + 3]));
    };
    // Entry 0 is the root: its third word is the number of entries, as a directory's is the index
    // past its last entry. The names follow the entries.
    const uint32_t entries = fst.size() >= 12 ? be32(8) : 0;
    if (entries == 0 || uint64_t(entries) * 12 > fst.size()) {
        return {"sys/fst.bin"};
    }
    const size_t strings = size_t(entries) * 12;
    std::vector<std::string> dirs(size_t(entries) + 1);  // "" below a folder that isn't looked in
    std::vector<uint32_t> ends(size_t(entries) + 1);
    size_t depth = 0;
    dirs[0] = "files";
    ends[0] = entries;
    for (uint32_t i = 1; i < entries && missing.size() < limit; ++i) {
        while (depth > 0 && i >= ends[depth]) {
            --depth;
        }
        const uint32_t word = be32(size_t(i) * 12);
        const std::string name = AsciiName(fst, strings + (word & 0xFFFFFF));
        const std::string path = dirs[depth].empty() || name.empty() ? std::string() : dirs[depth] + "/" + name;
        if ((word >> 24) != 0) {
            ++depth;
            dirs[depth] = path;
            ends[depth] = std::min(entries, be32(size_t(i) * 12 + 8));
        } else if (!path.empty() && !fs::is_regular_file(root / fs::u8path(path), ec)) {
            missing.push_back(path);
        }
    }
    return missing;
}

// Problem(), and whether the trouble is files left out (`incomplete`: the player gets kIncomplete
// in a message box too). Below.
std::string Check(const fs::path& root, bool* incomplete);

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
    // The release's two programs: each points to the other's game.
    if (RuntimeProduct::Active().gameId == "SMNE01" && id == "SMNE03") {
        return "That is Newer Super Mario Bros. Wii (SMNE03): open it with NewerSMBW, the release's other "
               "program. This one runs only " + disc + ".";
    }
    if (RuntimeProduct::Active().gameId == "SMNE03" && id == "SMNE01") {
        return "That is New Super Mario Bros. Wii (SMNE01): open it with NSMBW, the release's other program. "
               "This one runs only " + disc + ".";
    }
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

// The folder beside the program that is used by itself: "game", or a mod's own (runtime_product.h).
std::string GameFolderName() {
    return std::string(RuntimeProduct::Active().programGameFolder);
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
           "program and name it \"" + GameFolderName() + "\".)";
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
    text.mods = RuntimeProduct::Active().gameId == "SMNE01";  // the original game's build only
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
// start, so two installs keep two games; a mod has a name of its own, so its program can sit beside
// this one), then "game" in the data folder (already the mod's own). A portable install keeps
// everything in its data folder.
std::vector<fs::path> ExtractTargets() {
    const fs::path data = RuntimeConfigFile::ApplicationDataDirectory() / "game";
    const fs::path program = ProgramDirectory();
    if (RuntimeConfigFile::PortableRootDirectory() || program.empty()) {
        return {data};
    }
    return {program / fs::u8path(GameFolderName()), data};
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
// then says why, or is empty when the player cancelled; `incomplete` as Check() sets it.
bool ImportDiscImage(const fs::path& image, std::string& problem, bool& incomplete) {
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
    // What a folder is checked for: every file there, the code modules, and a mod's version.
    problem = Check(staging, &incomplete);
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

namespace {
std::string Check(const fs::path& root, bool* incomplete) {
    // Files left out: kIncomplete (in the player's language) and which ones, for the chooser's red line.
    const auto leftOut = [&](const std::string& which) {
        if (incomplete != nullptr) {
            *incomplete = true;
        }
        RT_LOGF(RT_TAG_RUNTIME, "game folder %s: %s\n", RuntimeConfigFile::PathToUtf8(root).c_str(), which.c_str());
        return UiText::Tr(kIncomplete) + " " + which;
    };
    if (!IsExtractedDisc(root)) {
        return leftOut("It needs the files and sys folders Dolphin's Extract Entire Disc makes: choose the folder "
                       "that holds both.");
    }
    std::error_code bootError;
    if (!fs::is_regular_file(root / "sys" / "boot.bin", bootError)) {
        return leftOut("It has no sys/boot.bin.");
    }
    std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
    char header[8] = {};
    if (!boot.read(header, sizeof(header))) {
        return "That folder's sys/boot.bin is too short, so the game can't be identified.";
    }
    const std::string id(header, 6);
    const int revision = static_cast<unsigned char>(header[7]);
    const RuntimeProduct::Descriptor& product = RuntimeProduct::Active();
    const std::string disc(product.discName);
    if (std::string problem = IdProblem(id, revision); !problem.empty()) {
        return problem;
    }
    // Every file the game reads (its own file table's list): a folder copied in part would start,
    // build its shaders and stop at whatever file isn't there.
    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::string> missing = MissingFiles(root, SIZE_MAX);
    RT_LOGF(RT_TAG_RUNTIME, "game folder: %zu required file(s) missing (%.1f ms)\n", missing.size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    if (!missing.empty()) {
        return leftOut(missing.size() == 1 ? "Missing: " + missing[0] + "."
                                           : "Missing: " + missing[0] + " and " + std::to_string(missing.size() - 1) +
                                                 " more files.");
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
} // namespace

std::string Problem(const fs::path& root) {
    return Check(root, nullptr);
}

namespace {
// Each retail disc's main.dol, as projects/nsmbw/variants/variants.toml pins it (main_dol_sha256).
// The Japanese disc has none pinned yet, so it never counts as retail until one is added here.
struct RetailDol {
    const char* id;
    const char* sha256;
};
constexpr RetailDol kRetailDols[] = {
    {"SMNE01", "6d755ec24c395fee600a856a85785e0e99c0b7ecb680d573a6cdd1aa6f7beee7"},  // USA rev 1
    {"SMNP01", "51a2a8b0941e8fe3390404a9b2094e3c78246a999efa58fcc8a7c534c11d7b5b"},  // Europe rev 1
};

// A file's SHA-256 in lowercase hex; empty when it can't be read.
std::string MainDolSha256(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    CryptoPP::SHA256 hash;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) {
            hash.Update(reinterpret_cast<const CryptoPP::byte*>(buffer.data()), static_cast<size_t>(in.gcount()));
        }
    }
    CryptoPP::byte digest[CryptoPP::SHA256::DIGESTSIZE];
    hash.Final(digest);
    static const char* hex = "0123456789abcdef";
    std::string text;
    for (const auto b : digest) {
        text += hex[b >> 4];
        text += hex[b & 15];
    }
    return text;
}
} // namespace

bool RetailDisc() {
    static const bool retail = [] {
        const fs::path root = Find(RuntimeConfigFile::ResolvedDvdRoot());
        if (root.empty()) {
            return false;
        }
        std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
        char header[6] = {};
        if (!boot.read(header, sizeof(header))) {
            return false;
        }
        const std::string id(header, 6);
        for (const RetailDol& dol : kRetailDols) {
            if (id == dol.id) {
                const bool same = MainDolSha256(root / "sys" / "main.dol") == dol.sha256;
                RT_LOGF(RT_TAG_RUNTIME, "game folder: %s, %s main.dol\n", id.c_str(), same ? "the retail" : "a changed");
                return same;
            }
        }
        return false;
    }();
    return retail;
}

bool EnsureConfigured() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    CleanStaleImports();
    // A "game" folder next to the program needs no questions, and wins over the saved folder: every
    // install shares Documents\MarioWiiSaveData, so this is how two installs play two games (say the
    // original and a mod), each with its own saves (nand_path.h keeps saves per game). A mod's program
    // looks for its own name ("game-newer"), so the release's two programs keep their games apart.
    std::string besideProblem;
    bool besideIncomplete = false;
    std::error_code besideError;
    if (const fs::path besidePath = ProgramDirectory() / fs::u8path(GameFolderName());
        !ProgramDirectory().empty() && fs::is_directory(besidePath, besideError)) {
        const fs::path beside = Find(besidePath);
        besideProblem = Check(beside.empty() ? besidePath : beside, &besideIncomplete);
        if (besideProblem.empty()) {
            if (RuntimeConfigFile::DvdRoot().empty()) {
                RuntimeConfigFile::SetDvdRoot(beside);
            } else {
                RuntimeConfigFile::UseDvdRootForThisRun(beside);
            }
            RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(beside).c_str());
            return true;
        }
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s can't be used: %s\n", RuntimeConfigFile::PathToUtf8(besidePath).c_str(),
                besideProblem.c_str());
    }
#endif
    const fs::path configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
    if (!configured.empty() && Problem(configured).empty()) {
        return true;
    }
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    std::string problem;
    bool incomplete = false;
    if (!RuntimeConfigFile::DvdRoot().empty()) {
        const fs::path saved = RuntimeConfigFile::ResolvedDvdRoot();
        std::error_code ec;
        problem = fs::exists(saved, ec) ? Check(configured.empty() ? saved : configured, &incomplete)
                                        : "The game folder you chose before (" + RuntimeConfigFile::PathToUtf8(saved) +
                                              ") isn't there any more.";
    }
    if (problem.empty() && !besideProblem.empty()) {
        problem = besideProblem;
        incomplete = besideIncomplete;
    }
    const GamePicker::Text text = PickerText();
    for (;;) {
        // Files left out: said in a message box as well as in the chooser's red line.
        if (incomplete) {
            GamePicker::ShowError(text, UiText::Tr(kIncomplete));
            incomplete = false;
        }
        const GamePicker::Picked picked = GamePicker::Show(text, problem);
        if (picked.choice == GamePicker::Choice::Cancel) {
            GamePicker::Close();
            return false;
        }
        if (picked.choice == GamePicker::Choice::Iso) {
            if (!ImportDiscImage(picked.path, problem, incomplete)) {
                continue;
            }
        } else {
            const fs::path root = Find(picked.path);
            problem = Check(root.empty() ? picked.path : root, &incomplete);
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
