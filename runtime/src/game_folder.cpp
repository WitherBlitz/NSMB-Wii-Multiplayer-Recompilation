#include "game_folder.h"

#include "runtime_config.h"
#include "runtime_log.h"

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

#ifndef NSMBW_GAME_ID
#define NSMBW_GAME_ID "SMNE01"  // set by runtime/CMakeLists.txt
#endif

namespace GameFolder {
namespace {

namespace fs = std::filesystem;

bool IsExtractedDisc(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path / "files", ec) && fs::is_regular_file(path / "sys" / "fst.bin", ec);
}

#if defined(_WIN32)
constexpr const wchar_t* kTitle = L"New Super Mario Bros. Wii";

constexpr const wchar_t* kSteps =
    L"This port runs your own copy of New Super Mario Bros. Wii (USA, SMNE01, revision 1). "
    L"Extract it once with Dolphin:\n\n"
    L"1. Open Dolphin on this computer and find New Super Mario Bros. Wii in the game list.\n"
    L"2. Right-click it, choose Properties, then the Filesystem tab.\n"
    L"3. Right-click the disc at the top and choose Extract Entire Disc, then pick an empty folder.\n"
    L"4. Click OK here and choose that folder. Keep it: the game reads it every time.\n\n"
    L"(Dolphin shows the revision under Properties > Info. Or put the extracted folder next to this "
    L"program and name it \"game\".)";

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
        dialog->SetTitle(L"Choose the folder Dolphin extracted New Super Mario Bros. Wii to");
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
constexpr const char* kTitle = "New Super Mario Bros. Wii";

constexpr const char* kSteps =
    "This port runs your own copy of New Super Mario Bros. Wii (USA, SMNE01, revision 1). "
    "Extract it once with Dolphin:\n\n"
    "1. Open Dolphin and find New Super Mario Bros. Wii in the game list.\n"
    "2. Right-click it, choose Properties, then the Filesystem tab.\n"
    "3. Right-click the disc at the top and choose Extract Entire Disc, then pick an empty folder.\n"
    "4. Click OK here and choose that folder. Keep it: the game reads it every time.\n\n"
    "(Dolphin shows the revision under Properties > Info. Or put the extracted folder next to this "
    "program and name it \"game\".)";

fs::path ProgramDirectory() {
    return RuntimeConfigFile::ExecutableDirectory().value_or(fs::path{});  // the compat build's too
}

// OK / Cancel; true for OK.
bool AskOkCancel(const std::string& message) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"},
    };
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_INFORMATION, nullptr, kTitle, message.c_str(), 2, buttons, nullptr};
    int pressed = 0;
    return SDL_ShowMessageBox(&data, &pressed) && pressed == 1;
}

// The desktop's folder picker (zenity on GNOME and most others, kdialog on KDE); empty when cancelled,
// nullopt when neither is installed.
std::optional<fs::path> PickFolder() {
    const char* const commands[] = {
        "zenity --file-selection --directory "
        "--title='Choose the folder Dolphin extracted New Super Mario Bros. Wii to' 2>/dev/null",
        "kdialog --getexistingdirectory \"$HOME\" "
        "--title 'Choose the folder Dolphin extracted New Super Mario Bros. Wii to' 2>/dev/null",
    };
    for (const char* command : commands) {
        const std::string tool(command, std::string(command).find(' '));
        if (std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) != 0) {
            continue;
        }
        FILE* pipe = popen(command, "r");
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
    if (id != NSMBW_GAME_ID && std::string(NSMBW_GAME_ID) != "SMNE01") {
        return "That is " + id + ", not " NSMBW_GAME_ID ", the game this build was translated from.";
    }
    if (id != NSMBW_GAME_ID) {
        return "That is " + id + ", not New Super Mario Bros. Wii for the USA (SMNE01). This port runs "
               "only the USA disc, revision 1.";
    }
    if (revision != 1) {
        return "That is New Super Mario Bros. Wii revision " + std::to_string(revision) +
               ". This port runs only revision 1 (Dolphin shows it under Properties > Info).";
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
    std::wstring message = kSteps;
    if (!configured.empty()) {
        message = Widen(Problem(configured)) + L"\n\n" + message;
    }
    for (;;) {
        if (MessageBoxW(nullptr, message.c_str(), kTitle, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            return false;
        }
        const fs::path picked = PickFolder();
        if (picked.empty()) {
            continue;
        }
        const fs::path root = Find(picked);
        const std::string problem = Problem(root.empty() ? picked : root);
        if (!problem.empty()) {
            message = Widen(problem) + L"\n\n" + kSteps;
            continue;
        }
        RuntimeConfigFile::SetDvdRoot(root);
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(root).c_str());
        return true;
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    std::string message = kSteps;
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
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, kTitle, help.c_str(), nullptr);
            return false;
        }
        if (picked->empty()) {
            continue;
        }
        const fs::path root = Find(*picked);
        const std::string problem = Problem(root.empty() ? *picked : root);
        if (!problem.empty()) {
            message = problem + "\n\n" + kSteps;
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
