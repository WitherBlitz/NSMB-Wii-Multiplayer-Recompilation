#include "game_folder.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <SDL3/SDL_messagebox.h>

#include <cstdio>
#include <unistd.h>
#endif

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

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
// What to extract, and how.
std::string Steps(const char* openDolphin) {
    return "This port runs your own copy of " + std::string(RuntimeProduct::Active().discName) +
           ". Extract it once with Dolphin:\n\n"
           "1. " + openDolphin + " and find " + GameName() + " in the game list.\n"
           "2. Right-click it, choose Properties, then the Filesystem tab.\n"
           "3. Right-click the disc at the top and choose Extract Entire Disc, then pick an empty folder.\n"
           "4. Click OK here and choose that folder. Keep it: the game reads it every time.\n\n"
           "(Dolphin shows the revision under Properties > Info. Or put the extracted folder next to this "
           "program and name it \"game\".)";
}

std::string PickerTitle() {
    return "Choose the folder Dolphin extracted " + GameName() + " to";
}
#endif

#if defined(_WIN32)
std::wstring Widen(const std::string& utf8);

fs::path ProgramDirectory() {
    return RuntimeConfigFile::ExecutableDirectory().value_or(fs::path{});  // the compat build's too
}

// The Windows folder picker; empty when cancelled.
fs::path PickFolder() {
    fs::path result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(Widen(PickerTitle()).c_str());
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    result = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return result;
}

std::wstring Widen(const std::string& utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}
#elif defined(__linux__) && !defined(__ANDROID__)
fs::path ProgramDirectory() {
    return RuntimeConfigFile::ExecutableDirectory().value_or(fs::path{});  // the compat build's too
}

// OK / Cancel; true for OK.
bool AskOkCancel(const std::string& message) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"},
    };
    const std::string title = GameName();
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_INFORMATION, nullptr, title.c_str(), message.c_str(), 2, buttons,
                                     nullptr};
    int pressed = 0;
    return SDL_ShowMessageBox(&data, &pressed) && pressed == 1;
}

// The desktop's folder picker (zenity on GNOME and most others, kdialog on KDE); empty when cancelled,
// nullopt when neither is installed.
std::optional<fs::path> PickFolder() {
    const std::string commands[] = {
        "zenity --file-selection --directory --title='" + PickerTitle() + "' 2>/dev/null",
        "kdialog --getexistingdirectory \"$HOME\" --title '" + PickerTitle() + "' 2>/dev/null",
    };
    for (const std::string& command : commands) {
        const std::string tool = command.substr(0, command.find(' '));
        if (std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) != 0) {
            continue;
        }
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr) {
            continue;
        }
        std::string output;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        pclose(pipe);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        return fs::path(output);
    }
    return std::nullopt;
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
    if (id != product.gameId) {
        return "That is " + id + ", not " + disc + ". This build runs only that disc: it was translated from it.";
    }
    if (revision != 1) {
        return "That is " + id + " revision " + std::to_string(revision) + ". This build runs only " + disc +
               " (Dolphin shows the revision under Properties > Info).";
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
#if defined(_WIN32)
    const std::wstring steps = Widen(Steps("Open Dolphin on this computer"));
    const std::wstring title = Widen(GameName());
    std::wstring message = steps;
    if (!configured.empty()) {
        message = Widen(Problem(configured)) + L"\n\n" + message;
    }
    for (;;) {
        if (MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            return false;
        }
        const fs::path picked = PickFolder();
        if (picked.empty()) {
            continue;
        }
        const fs::path root = Find(picked);
        const std::string problem = Problem(root.empty() ? picked : root);
        if (!problem.empty()) {
            message = Widen(problem) + L"\n\n" + steps;
            continue;
        }
        RuntimeConfigFile::SetDvdRoot(root);
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(root).c_str());
        return true;
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    const std::string steps = Steps("Open Dolphin");
    std::string message = steps;
    if (!configured.empty()) {
        message = Problem(configured) + "\n\n" + message;
    }
    for (;;) {
        if (!AskOkCancel(message)) {
            return false;
        }
        const std::optional<fs::path> picked = PickFolder();
        if (!picked) {
            // No folder picker on this desktop: say where the folder can go instead.
            const std::string config = RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolveConfigPath());
            const std::string help =
                "No folder picker (zenity or kdialog) is installed. Put the extracted game next to this "
                "program in a folder named \"game\", or write its path into " + config +
                " under [paths] as dvd_root = '/path/to/the/folder', then start the game again.";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, GameName().c_str(), help.c_str(), nullptr);
            return false;
        }
        if (picked->empty()) {
            continue;
        }
        const fs::path root = Find(*picked);
        const std::string problem = Problem(root.empty() ? *picked : root);
        if (!problem.empty()) {
            message = problem + "\n\n" + steps;
            continue;
        }
        RuntimeConfigFile::SetDvdRoot(root);
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(root).c_str());
        return true;
    }
#else
    return true;  // the app chose and checked the folder
#endif
}

} // namespace GameFolder
