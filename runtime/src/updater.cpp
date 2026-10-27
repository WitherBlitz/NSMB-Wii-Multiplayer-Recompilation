#include "updater.h"

#include "game_version.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
namespace AndroidBridge {
bool StartAppUpdate();
}
#else
#include <cryptopp/sha.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#elif defined(__linux__)
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#endif

namespace Updater {
namespace {

std::mutex g_mutex;
Status g_status;

void Set(State state, const std::string& message = {}, float progress = -1) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.state = state;
    g_status.message = message;
    g_status.progress = progress;
}

#if !defined(__ANDROID__) && (defined(_WIN32) || defined(__linux__))
#define NSMBW_DESKTOP_UPDATER 1
namespace fs = std::filesystem;

constexpr const char* kLatestRelease =
    "https://api.github.com/repos/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases/latest";
#if defined(_WIN32)
constexpr const char* kAssetSuffix = "-windows-x64.zip";
#else
constexpr const char* kAssetSuffix = "-linux-x86_64.tar.gz";
#endif
constexpr const char* kDownloadFolder = "update-download";
constexpr const char* kOldList = "update-old.txt";  // the files the last update set aside, to delete

struct Asset {
    std::string url;
    uint64_t size = 0;
    std::string sha256;  // lowercase hex, empty when GitHub lists none
};
Asset g_asset;

// ---------------------------------------------------------------- a small JSON reader (GitHub's answer)
struct Json {
    enum class Kind { Null, Literal, String, Array, Object } kind = Kind::Null;
    std::string text;  // a string's value, or a number's / true's / false's spelling
    std::vector<Json> items;        // an array's
    std::vector<std::string> keys;  // an object's, with values[i] for keys[i]
    std::vector<Json> values;

    const Json* Get(const char* key) const {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &values[i];
            }
        }
        return nullptr;
    }
    std::string Text(const char* key) const {
        const Json* value = Get(key);
        return value != nullptr ? value->text : std::string();
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : m_s(s) {}
    bool Parse(Json& out) { return Value(out, 0) && (Space(), m_i == m_s.size()); }

private:
    void Space() {
        while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r')) {
            ++m_i;
        }
    }
    bool Value(Json& out, int depth) {
        if (depth > 64) {
            return false;
        }
        Space();
        if (m_i >= m_s.size()) {
            return false;
        }
        const char c = m_s[m_i];
        if (c == '{') {
            out.kind = Json::Kind::Object;
            ++m_i;
            Space();
            if (m_i < m_s.size() && m_s[m_i] == '}') {
                ++m_i;
                return true;
            }
            for (;;) {
                Space();
                std::string key;
                if (!String(key)) return false;
                Space();
                if (m_i >= m_s.size() || m_s[m_i++] != ':') return false;
                Json value;
                if (!Value(value, depth + 1)) return false;
                out.keys.push_back(std::move(key));
                out.values.push_back(std::move(value));
                Space();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') { ++m_i; continue; }
                if (m_s[m_i] == '}') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::Array;
            ++m_i;
            Space();
            if (m_i < m_s.size() && m_s[m_i] == ']') {
                ++m_i;
                return true;
            }
            for (;;) {
                Json value;
                if (!Value(value, depth + 1)) return false;
                out.items.push_back(std::move(value));
                Space();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') { ++m_i; continue; }
                if (m_s[m_i] == ']') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return String(out.text);
        }
        // A number, true, false or null: kept as written.
        const size_t start = m_i;
        while (m_i < m_s.size() && std::strchr(",]} \t\r\n", m_s[m_i]) == nullptr) {
            ++m_i;
        }
        out.text = m_s.substr(start, m_i - start);
        out.kind = out.text == "null" ? Json::Kind::Null : Json::Kind::Literal;
        return m_i > start;
    }
    bool String(std::string& out) {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (m_i >= m_s.size()) return false;
            const char e = m_s[m_i++];
            switch (e) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (m_i + 4 > m_s.size()) return false;
                const unsigned code = static_cast<unsigned>(std::strtoul(m_s.substr(m_i, 4).c_str(), nullptr, 16));
                m_i += 4;
                // UTF-8 (a surrogate pair's halves come out separately: nothing here needs them).
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default: out += e; break;  // \" \\ \/
            }
        }
        return false;
    }

    const std::string& m_s;
    size_t m_i = 0;
};

// True when `remote` ("0.2.3") is a later version than `local`.
bool IsNewer(const std::string& remote, const std::string& local) {
    const auto parts = [](const std::string& v) {
        std::vector<int> out;
        size_t i = 0;
        while (i <= v.size()) {
            int n = 0;
            while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
                n = n * 10 + (v[i++] - '0');
            }
            out.push_back(n);
            const size_t dot = v.find('.', i);
            if (dot == std::string::npos) break;
            i = dot + 1;
        }
        return out;
    };
    const auto a = parts(remote), b = parts(local);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}

std::string LocalVersion() {
    const char* test = std::getenv("NSMBW_UPDATE_TEST_VERSION");
    return test != nullptr && *test != '\0' ? test : NSMBW_VERSION;
}

std::string Utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path FromUtf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

fs::path InstallRoot() {
    return RuntimeConfigFile::ExecutableDirectory().value_or(fs::current_path());
}

// ---------------------------------------------------------------- downloads
// GET `url` into `out` (a file); `expected` bytes, when known, give the progress. Follows GitHub's
// redirect to its file host.
#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Download(const std::string& url, const fs::path& out, uint64_t expected, bool api, std::string& error) {
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        error = "bad address";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    HINTERNET session = WinHttpOpen(L"NSMBW-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {  // before Windows 8.1
        session = WinHttpOpen(L"NSMBW-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session == nullptr) {
        error = "no internet access (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connect != nullptr
                            ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr;
    const auto close = [&] {
        if (request != nullptr) WinHttpCloseHandle(request);
        if (connect != nullptr) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
    };
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (request == nullptr ||
        !WinHttpSendRequest(request, headers, api ? static_cast<DWORD>(-1) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = "couldn't reach " + std::string(host.begin(), host.end()) + " (error " +
                std::to_string(GetLastError()) + ")";
        close();
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "the server answered " + std::to_string(status);
        close();
        return false;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1 << 16);
    uint64_t done = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            error = "the download stopped (error " + std::to_string(GetLastError()) + ")";
            close();
            return false;
        }
        if (read == 0) break;
        file.write(buffer.data(), read);
        done += read;
        if (expected > 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.progress = static_cast<float>(static_cast<double>(done) / static_cast<double>(expected));
        }
    }
    close();
    if (!file) {
        error = "couldn't save the download";
        return false;
    }
    return true;
}

// Runs `command` (a whole command line) without a console window and waits: its exit code, or -1.
int Run(std::wstring command) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

bool Extract(const fs::path& archive, const fs::path& to, std::string& error) {
    // Windows 10 1803 and later ship bsdtar, which reads zips.
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring command = L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + archive.wstring() +
                                 L"\" -C \"" + to.wstring() + L"\"";
    const int code = Run(command);
    if (code != 0) {
        error = code < 0 ? "Windows has no tar.exe to unpack the update" : "couldn't unpack the update";
        return false;
    }
    return true;
}
#else
// Runs argv[0] (looked up on the PATH) and waits; `whileRunning` is called every 200 ms. Its exit
// code, or 127 when it couldn't start.
template <typename F>
int Run(const std::vector<std::string>& args, F&& whileRunning) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
        return 127;
    }
    for (;;) {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (done < 0) {
            return 1;
        }
        whileRunning();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool Download(const std::string& url, const fs::path& out, uint64_t expected, bool api, std::string& error) {
    const auto progress = [&] {
        std::error_code ec;
        const auto size = fs::file_size(out, ec);
        if (!ec && expected > 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.progress = static_cast<float>(static_cast<double>(size) / static_cast<double>(expected));
        }
    };
    std::vector<std::string> curl = {"curl", "-fsSL", "--connect-timeout", "15", "-A", "NSMBW-Updater"};
    if (api) {
        curl.insert(curl.end(), {"-H", "Accept: application/vnd.github+json"});
    }
    curl.insert(curl.end(), {"-o", out.string(), url});
    int code = Run(curl, progress);
    if (code == 127) {  // no curl: wget
        code = Run({"wget", "-q", "-T", "30", "-U", "NSMBW-Updater", "-O", out.string(), url}, progress);
        if (code == 127) {
            error = "install curl or wget to update from here";
            return false;
        }
    }
    if (code != 0) {
        error = "the download failed (" + std::to_string(code) + ")";
        return false;
    }
    return true;
}

bool Extract(const fs::path& archive, const fs::path& to, std::string& error) {
    if (Run({"tar", "-xzf", archive.string(), "-C", to.string()}, [] {}) != 0) {
        error = "couldn't unpack the update";
        return false;
    }
    return true;
}
#endif

std::string FileSha256(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
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

// ---------------------------------------------------------------- the check
void Check() {
    const fs::path folder = InstallRoot() / kDownloadFolder;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path answer = folder / "latest.json";
    const char* override = std::getenv("NSMBW_UPDATE_URL");
    std::string error;
    if (!Download(override != nullptr && *override != '\0' ? override : kLatestRelease, answer, 0, true, error)) {
        RT_LOGF(RT_TAG_RUNTIME, "update: couldn't check for a newer release: %s\n", error.c_str());
        fs::remove_all(folder, ec);
        return;
    }
    std::ifstream in(answer, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    fs::remove_all(folder, ec);
    Json json;
    if (!JsonParser(text).Parse(json) || json.kind != Json::Kind::Object) {
        RT_LOGF(RT_TAG_RUNTIME, "update: GitHub's answer wasn't readable\n");
        return;
    }
    std::string version = json.Text("tag_name");
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        version.erase(0, 1);
    }
    const std::string local = LocalVersion();
    if (version.empty() || !IsNewer(version, local)) {
        RT_LOGF(RT_TAG_RUNTIME, "update: %s is the latest release\n", local.c_str());
        return;
    }
    const Json* assets = json.Get("assets");
    if (assets == nullptr) {
        return;
    }
    for (const Json& asset : assets->items) {
        const std::string name = asset.Text("name");
        if (name.size() < std::strlen(kAssetSuffix) ||
            name.compare(name.size() - std::strlen(kAssetSuffix), std::string::npos, kAssetSuffix) != 0) {
            continue;
        }
        Asset found;
        found.url = asset.Text("browser_download_url");
        found.size = std::strtoull(asset.Text("size").c_str(), nullptr, 10);
        const std::string digest = asset.Text("digest");
        if (digest.rfind("sha256:", 0) == 0) {
            found.sha256 = digest.substr(7);
            for (char& c : found.sha256) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (found.url.empty()) {
            continue;
        }
        RT_LOGF(RT_TAG_RUNTIME, "update: %s is out (this is %s): %s\n", version.c_str(), local.c_str(), name.c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_asset = found;
        g_status.version = version;
        g_status.state = State::Available;
        return;
    }
    RT_LOGF(RT_TAG_RUNTIME, "update: %s has no download for this system\n", version.c_str());
}

// ---------------------------------------------------------------- installing
// Removes the files the last update set aside (they were in use then) and its download folder.
void CleanUp() {
    const fs::path root = InstallRoot();
    std::error_code ec;
    std::ifstream list(root / kOldList);
    for (std::string line; std::getline(list, line);) {
        if (!line.empty()) {
            fs::remove(FromUtf8(line), ec);
        }
    }
    list.close();
    fs::remove(root / kOldList, ec);
    fs::remove_all(root / kDownloadFolder, ec);
}

// Moves every file of the unpacked release over the installation's, each old one aside as *.old
// (a running program's files can be renamed, not overwritten). Puts everything back on a failure.
bool SwapIn(const fs::path& unpacked, const fs::path& root, std::string& error) {
    fs::path top = unpacked;
    {
        // The archive holds one folder, "NSMBW-<version>-<system>".
        int entries = 0;
        fs::path only;
        for (const auto& e : fs::directory_iterator(unpacked)) {
            ++entries;
            only = e.path();
        }
        if (entries == 1 && fs::is_directory(only)) {
            top = only;
        }
    }
    struct Moved {
        fs::path target;
        bool hadOld;
    };
    std::vector<Moved> moved;
    std::ofstream oldList(root / kOldList, std::ios::app);
    std::error_code ec;
    const auto rollBack = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code e;
            fs::remove(it->target, e);
            if (it->hadOld) {
                fs::rename(fs::path(it->target).concat(".old"), it->target, e);
            }
        }
    };
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(top, ec)) {
        if (e.is_regular_file()) {
            files.push_back(e.path());
        }
    }
    if (ec || files.empty()) {
        error = "the update was empty";
        return false;
    }
    for (const fs::path& file : files) {
        const fs::path target = root / file.lexically_relative(top);
        const fs::path old = fs::path(target).concat(".old");
        fs::create_directories(target.parent_path(), ec);
        bool hadOld = false;
        if (fs::exists(target)) {
            fs::remove(old, ec);
            fs::rename(target, old, ec);
            if (ec) {
                error = "couldn't replace " + Utf8(target.filename()) + " (" + ec.message() + ")";
                rollBack();
                return false;
            }
            hadOld = true;
            oldList << Utf8(old) << "\n";
        }
        fs::rename(file, target, ec);
        if (ec) {
            error = "couldn't write " + Utf8(target.filename()) + " (" + ec.message() + ")";
            if (hadOld) {
                std::error_code e;
                fs::rename(old, target, e);
            }
            rollBack();
            return false;
        }
        moved.push_back({target, hadOld});
    }
    return true;
}

// Starts the (new) game in this one's place: the installation's main program, which picks the compat
// build itself when this CPU needs it.
[[noreturn]] void Restart(const fs::path& root) {
#if defined(_WIN32)
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path exe(self);
    if (exe.parent_path().filename() == L"compat") {
        std::wstring stem = exe.stem().wstring();
        if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, L"-compat") == 0) {
            stem.resize(stem.size() - 7);
        }
        exe = root / (stem + exe.extension().wstring());
    }
    std::wstring command = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "update: couldn't start the game again (error %lu)\n", GetLastError());
    }
    TerminateProcess(GetCurrentProcess(), 0);
#else
    char self[4096] = {};
    const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    fs::path exe(length > 0 ? std::string(self, static_cast<size_t>(length)) : std::string());
    if (exe.parent_path().filename() == "compat") {
        std::string name = exe.filename().string();
        if (name.size() > 7 && name.compare(name.size() - 7, 7, "-compat") == 0) {
            name.resize(name.size() - 7);
        }
        exe = root / name;
    }
    // The same arguments again.
    std::vector<std::string> args;
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    for (std::string arg; std::getline(cmdline, arg, '\0');) {
        args.push_back(arg);
    }
    if (args.empty()) {
        args.push_back(exe.string());
    }
    args[0] = exe.string();
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(exe.c_str(), argv.data());
    RT_LOGF(RT_TAG_RUNTIME, "update: couldn't start the game again\n");
#endif
    std::_Exit(0);
}

void InstallNow() {
    Asset asset;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        asset = g_asset;
    }
    const fs::path root = InstallRoot();
    const fs::path folder = root / kDownloadFolder;
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder / "files", ec);
    if (ec) {
        Set(State::Failed, "The game's folder can't be written to.");
        return;
    }
    const fs::path archive = folder / FromUtf8(asset.url.substr(asset.url.find_last_of('/') + 1));
    std::string error;
    RT_LOGF(RT_TAG_RUNTIME, "update: downloading %s\n", asset.url.c_str());
    Set(State::Downloading, {}, 0.0f);
    if (!Download(asset.url, archive, asset.size, false, error)) {
        Set(State::Failed, "Download failed: " + error);
        RT_LOGF(RT_TAG_RUNTIME, "update: %s\n", error.c_str());
        fs::remove_all(folder, ec);
        return;
    }
    Set(State::Installing);
    if (!asset.sha256.empty() && FileSha256(archive) != asset.sha256) {
        Set(State::Failed, "The download was damaged. Try again.");
        RT_LOGF(RT_TAG_RUNTIME, "update: the download doesn't match the release's SHA-256\n");
        fs::remove_all(folder, ec);
        return;
    }
    if (!Extract(archive, folder / "files", error) || !SwapIn(folder / "files", root, error)) {
        Set(State::Failed, "The update failed: " + error);
        RT_LOGF(RT_TAG_RUNTIME, "update: %s\n", error.c_str());
        fs::remove_all(folder, ec);
        return;
    }
    fs::remove_all(folder, ec);
    RT_LOGF(RT_TAG_RUNTIME, "update: installed, restarting\n");
    Set(State::Restarting);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // "Restarting..." shows for a moment
    Restart(root);
}
#endif  // desktop

} // namespace

void Start() {
#if defined(NSMBW_DESKTOP_UPDATER)
    CleanUp();
    const char* off = std::getenv("NSMBW_NO_UPDATE_CHECK");
    const char* url = std::getenv("NSMBW_UPDATE_URL");
    // Automated runs (a scripted remote) don't ask GitHub unless a test points the check elsewhere.
    if ((off != nullptr && *off == '1') || (std::getenv("NSMBW_INPUT_SCRIPT") != nullptr && url == nullptr)) {
        return;
    }
    std::thread(Check).detach();
#endif
}

Status Get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void Install() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status.state != State::Available && g_status.state != State::Failed) {
            return;
        }
#if defined(__ANDROID__)
        g_status.state = State::Downloading;
        g_status.progress = -1;
        g_status.message.clear();
#endif
    }
#if defined(__ANDROID__)
    if (!AndroidBridge::StartAppUpdate()) {
        Set(State::Failed, "Update from the app's start screen instead.");
    }
#elif defined(NSMBW_DESKTOP_UPDATER)
    Set(State::Downloading, {}, 0.0f);
    std::thread(InstallNow).detach();
#endif
}

#if defined(__ANDROID__)
void SetFromApp(State state, const std::string& version, float progress, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.state = state;
    if (!version.empty()) {
        g_status.version = version;
    }
    g_status.progress = progress;
    g_status.message = message;
}
#endif

} // namespace Updater
