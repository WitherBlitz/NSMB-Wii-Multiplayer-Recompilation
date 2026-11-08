// hd_texture_pack.cpp - the Mods tab's HD Texture Pack: download, check, unpack, switch. See hd_texture_pack.h.
#include "hd_texture_pack.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <aurora/aurora.h>
#include <cryptopp/sha.h>

extern "C" {
#include "7z.h"
#include "7zCrc.h"
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
namespace AndroidBridge {
std::string DownloadFile(const std::string& url, const std::string& path);
}
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace HdTexturePack {
namespace {

namespace fs = std::filesystem;

// The pack's own download links (its Dolphin forum thread), never a copy of ours. The PNG edition: the
// DDS one is mostly BC7, which phones' GPUs can't sample.
constexpr const char* kDriveUrl =
    "https://drive.usercontent.google.com/download?id=1gcPQ2u_GXl3IpziEVvIsVzS90-mar2ZM&export=download&confirm=t";
// The mirror's page links the file itself (https://download<n>.mediafire.com/...).
constexpr const char* kMediaFirePage = "https://www.mediafire.com/file/goz5ep7dgz4ripo/NSMBW_HD_v2.81_PNG.7z/file";
constexpr uint64_t kArchiveSize = 23346788;
constexpr const char* kArchiveSha256 = "ACD46F6490D02ACBCB871F3E0A20B4B9F8534367E32A4535B364A51EF874B1D2";
// Inside the archive: NSMBW_HD_v2.81_PNG/SMN/... are the textures (Dolphin's Load/Textures/SMN). Its
// "~PSX Buttons" alternative, Thumbs.db and the readme stay out.
constexpr const char* kArchiveRoot = "NSMBW_HD_v2.81_PNG/";
constexpr const char* kPackFolder = "NSMBW_HD_v2.81";

constexpr const char* kFailedDownload = "The download failed. Check your connection and try again.";
constexpr const char* kFailedDamaged = "The download was damaged. Try again.";
constexpr const char* kFailedInstall = "Couldn't install the pack: is there room on the drive?";

std::mutex g_mutex;
Status g_status;
bool g_checked = false;    // whether g_status has looked at the disk yet
fs::path g_progressFile;   // the archive being downloaded, its size the progress
std::atomic<bool> g_justInstalled{false};  // for TakeJustInstalled
// Bytes the current download has received (Windows: a file still open reports its size late).
std::atomic<uint64_t> g_received{0};

fs::path DataDirectory() {
    return RuntimeConfigFile::ApplicationDataDirectory();
}

// <user data>/texture_replacements is what aurora indexes (main.cpp gives it the same user path).
fs::path InstallDirectory() {
    return DataDirectory() / "texture_replacements" / kPackFolder;
}

// Download and unpacking happen here, outside texture_replacements, and the unpacked folder is renamed
// into place last: an interrupted download never leaves a pack aurora would read.
fs::path StagingDirectory() {
    return DataDirectory() / "hd_texture_pack.tmp";
}

void SetState(State state, const std::string& message = {}) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.state = state;
    g_status.message = message;
    g_status.progress = -1;
}

// ---------------------------------------------------------------- downloads
// GET `url` into the file `out`, following redirects. An empty string, or why it failed (for the log).
#if defined(__ANDROID__)
std::string HttpGet(const std::string& url, const fs::path& out) {
    return AndroidBridge::DownloadFile(url, RuntimeConfigFile::PathToUtf8(out));
}
#elif defined(_WIN32)
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string HttpGet(const std::string& url, const fs::path& out) {
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        return "bad address";
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    HINTERNET session = WinHttpOpen(L"NSMBW", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {  // before Windows 8.1
        session = WinHttpOpen(L"NSMBW", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session == nullptr) {
        return "WinHttpOpen " + std::to_string(GetLastError());
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
    if (request == nullptr || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        const std::string error = "couldn't reach the server (WinHTTP " + std::to_string(GetLastError()) + ")";
        close();
        return error;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        close();
        return "HTTP " + std::to_string(status);
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            const std::string error = "the download stopped (WinHTTP " + std::to_string(GetLastError()) + ")";
            close();
            return error;
        }
        if (read == 0) break;
        file.write(buffer.data(), read);
        g_received += read;
    }
    close();
    return file ? std::string() : "couldn't write " + RuntimeConfigFile::PathToUtf8(out);
}
#else
// Linux: curl, else wget (as the updater does).
int Run(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
        return 127;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

std::string HttpGet(const std::string& url, const fs::path& out) {
    int code = Run({"curl", "-fsSL", "--connect-timeout", "15", "-A", "NSMBW", "-o", out.string(), url});
    if (code == 127) {
        code = Run({"wget", "-q", "-T", "30", "-U", "NSMBW", "-O", out.string(), url});
        if (code == 127) {
            return "neither curl nor wget is installed";
        }
    }
    return code == 0 ? std::string() : "curl/wget exit " + std::to_string(code);
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
    static const char* kHex = "0123456789ABCDEF";
    std::string text;
    for (const CryptoPP::byte b : digest) {
        text += kHex[b >> 4];
        text += kHex[b & 15];
    }
    return text;
}

// The archive, from Google Drive or else the MediaFire mirror, at its known size and SHA-256. False with
// `message` set when neither gave it.
bool DownloadArchive(const fs::path& archive, std::string& message) {
    const fs::path page = StagingDirectory() / "mirror.html";
    for (int source = 0; source < 2; ++source) {
        std::string url = kDriveUrl;
        if (source == 0 && std::getenv("NSMBW_HDPACK_NO_DRIVE") != nullptr) {
            continue;  // testing the mirror
        }
        if (source == 1) {
            // The mirror's page carries the file's own address.
            if (const std::string error = HttpGet(kMediaFirePage, page); !error.empty()) {
                RT_LOGF(RT_TAG_RUNTIME, "hd pack: mirror page: %s\n", error.c_str());
                continue;
            }
            std::ifstream in(page, std::ios::binary);
            const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const size_t at = html.find("https://download");
            const size_t end = at == std::string::npos ? at : html.find_first_of("\"' <>", at);
            url = end == std::string::npos ? std::string() : html.substr(at, end - at);
            if (url.find(".mediafire.com/") == std::string::npos) {
                RT_LOGF(RT_TAG_RUNTIME, "hd pack: no file address on the mirror's page\n");
                continue;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_progressFile = archive;
        }
        g_received = 0;
        const std::string error = HttpGet(url, archive);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_progressFile.clear();
        }
        std::error_code ec;
        const uint64_t size = fs::file_size(archive, ec);
        if (!error.empty() || ec || size != kArchiveSize) {
            RT_LOGF(RT_TAG_RUNTIME, "hd pack: %s: %s, %llu bytes\n", source == 0 ? "Google Drive" : "MediaFire",
                    error.empty() ? "wrong size" : error.c_str(), static_cast<unsigned long long>(ec ? 0 : size));
            message = kFailedDownload;
            continue;
        }
        if (FileSha256(archive) != kArchiveSha256) {
            RT_LOGF(RT_TAG_RUNTIME, "hd pack: %s: SHA-256 doesn't match\n", source == 0 ? "Google Drive" : "MediaFire");
            message = kFailedDamaged;
            continue;
        }
        RT_LOGF(RT_TAG_RUNTIME, "hd pack: downloaded from %s\n", source == 0 ? "Google Drive" : "MediaFire");
        return true;
    }
    if (message.empty()) {
        message = kFailedDownload;
    }
    return false;
}

// ---------------------------------------------------------------- unpacking (LZMA SDK)
struct FileInStream {
    ISeekInStream vt;
    std::FILE* file;
};

SRes FileRead(ISeekInStreamPtr pp, void* buf, size_t* size) {
    const auto* p = reinterpret_cast<const FileInStream*>(pp);
    if (*size == 0) {
        return SZ_OK;
    }
    *size = std::fread(buf, 1, *size, p->file);
    return std::ferror(p->file) ? SZ_ERROR_READ : SZ_OK;
}

SRes FileSeek(ISeekInStreamPtr pp, Int64* pos, ESzSeek origin) {
    const auto* p = reinterpret_cast<const FileInStream*>(pp);
    const int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
#if defined(_WIN32)
    if (_fseeki64(p->file, *pos, whence) != 0) return SZ_ERROR_READ;
    *pos = _ftelli64(p->file);
#else
    if (fseeko(p->file, static_cast<off_t>(*pos), whence) != 0) return SZ_ERROR_READ;
    *pos = static_cast<Int64>(ftello(p->file));
#endif
    return SZ_OK;
}

void* SzAllocImpl(ISzAllocPtr, size_t size) { return size == 0 ? nullptr : std::malloc(size); }
void SzFreeImpl(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kAlloc = {SzAllocImpl, SzFreeImpl};

// A texture's path inside the pack's root, "SMN/..." with '/' between its parts, or empty for anything
// else (or a name that could leave the folder).
std::string PackPath(const std::vector<UInt16>& name) {
    std::string path;
    for (const UInt16 c : name) {
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return {};
        }
        path += c == '\\' ? '/' : static_cast<char>(c);
    }
    const std::string root = kArchiveRoot;
    if (path.compare(0, root.size(), root) != 0) {
        return {};
    }
    path.erase(0, root.size());
    if (path.compare(0, 4, "SMN/") != 0 || path.size() < 8) {
        return {};
    }
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext != ".png") {
        return {};
    }
    for (size_t start = 0; start <= path.size();) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return {};
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return path;
}

// The archive's textures into `to` (to/SMN/...). False with the reason logged.
bool Unpack(const fs::path& archive, const fs::path& to, size_t& written) {
#if defined(_WIN32)
    std::FILE* file = _wfopen(archive.c_str(), L"rb");
#else
    std::FILE* file = std::fopen(archive.c_str(), "rb");
#endif
    if (file == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "hd pack: can't open the archive\n");
        return false;
    }
    FileInStream stream{{FileRead, FileSeek}, file};
    CLookToRead2 look{};
    LookToRead2_CreateVTable(&look, False);
    std::vector<Byte> lookBuffer(1 << 18);
    look.buf = lookBuffer.data();
    look.bufSize = lookBuffer.size();
    look.realStream = &stream.vt;
    LookToRead2_INIT(&look)
    CrcGenerateTable();
    CSzArEx db;
    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &look.vt, &kAlloc, &kAlloc);
    // One solid block: the first file decodes all of it (about 61 MB) into outBuffer, the rest are copies.
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    bool ok = res == SZ_OK;
    if (!ok) {
        RT_LOGF(RT_TAG_RUNTIME, "hd pack: not a 7z archive (%d)\n", res);
    }
    std::vector<UInt16> name;
    for (UInt32 i = 0; ok && i < db.NumFiles; ++i) {
        if (SzArEx_IsDir(&db, i)) continue;
        name.resize(SzArEx_GetFileNameUtf16(&db, i, nullptr));
        SzArEx_GetFileNameUtf16(&db, i, name.data());
        const std::string path = PackPath(name);
        if (path.empty()) continue;
        size_t offset = 0, size = 0;
        res = SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset, &size, &kAlloc, &kAlloc);
        if (res != SZ_OK) {
            RT_LOGF(RT_TAG_RUNTIME, "hd pack: unpacking %s failed (%d)\n", path.c_str(), res);
            ok = false;
            break;
        }
        const fs::path target = to / fs::u8path(path);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(outBuffer + offset), static_cast<std::streamsize>(size));
        if (!out) {
            RT_LOGF(RT_TAG_RUNTIME, "hd pack: couldn't write %s\n", path.c_str());
            ok = false;
            break;
        }
        ++written;
    }
    std::free(outBuffer);
    SzArEx_Free(&db, &kAlloc);
    std::fclose(file);
    return ok;
}

void Worker() {
    const fs::path staging = StagingDirectory();
    const fs::path archive = staging / "NSMBW_HD_v2.81_PNG.7z";
    const fs::path unpacked = staging / "unpacked";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        RT_LOGF(RT_TAG_RUNTIME, "hd pack: can't create %s: %s\n", RuntimeConfigFile::PathToUtf8(staging).c_str(),
                ec.message().c_str());
        SetState(State::Failed, kFailedInstall);
        return;
    }
    std::string message;
    if (!DownloadArchive(archive, message)) {
        fs::remove_all(staging, ec);
        SetState(State::Failed, message);
        return;
    }
    SetState(State::Installing);
    size_t written = 0;
    bool ok = Unpack(archive, unpacked, written);
    if (ok && written < 300) {  // the pack has 321 textures
        RT_LOGF(RT_TAG_RUNTIME, "hd pack: only %zu textures in the archive\n", written);
        ok = false;
    }
    const fs::path install = InstallDirectory();
    if (ok) {
        fs::remove_all(install, ec);
        fs::create_directories(install.parent_path(), ec);
        // Windows: a virus scanner still reading the new files refuses the move for a moment.
        for (int attempt = 0; attempt < 40; ++attempt) {
            fs::rename(unpacked, install, ec);
            if (!ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (ec) {
            RT_LOGF(RT_TAG_RUNTIME, "hd pack: can't move the textures into place: %s\n", ec.message().c_str());
            ok = false;
        }
    }
    fs::remove_all(staging, ec);
    if (!ok) {
        SetState(State::Failed, kFailedInstall);
        return;
    }
    RT_LOGF(RT_TAG_RUNTIME, "hd pack: installed %zu textures in %s\n", written, RuntimeConfigFile::PathToUtf8(install).c_str());
    SetState(State::Installed);
    g_justInstalled = true;
}

} // namespace

Status Get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_checked) {
        g_checked = true;
        std::error_code ec;
        g_status.state = fs::is_directory(InstallDirectory() / "SMN", ec) ? State::Installed : State::NotInstalled;
        if (g_status.state == State::NotInstalled) {
            fs::remove_all(StagingDirectory(), ec);  // what an interrupted download left
        }
    }
    Status status = g_status;
    if (status.state == State::Downloading && !g_progressFile.empty()) {
        std::error_code ec;
        uint64_t size = fs::file_size(g_progressFile, ec);
        size = std::max<uint64_t>(ec ? 0 : size, g_received.load());
        status.progress = std::min(1.0f, static_cast<float>(static_cast<double>(size) / kArchiveSize));
    }
    return status;
}

void Download() {
    {
        Get();  // the first look at the disk
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status.state != State::NotInstalled && g_status.state != State::Failed) {
            return;
        }
        g_status = {State::Downloading, 0.0f, {}};
    }
    std::thread(Worker).detach();
}

bool TakeJustInstalled() {
    return g_justInstalled.exchange(false);
}

bool Enabled() {
    return RuntimeConfigFile::TextureReplacements(false);
}

void SetEnabled(bool on) {
    RuntimeConfigFile::SetTextureReplacements(on);
    aurora_set_texture_replacements(on);
}

} // namespace HdTexturePack
