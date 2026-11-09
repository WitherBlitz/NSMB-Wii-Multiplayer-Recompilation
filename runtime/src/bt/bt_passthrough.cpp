// Bluetooth passthrough (bt_passthrough.h): the setting, the adapter search, the IOS device the
// game's Bluetooth stack opens, its SYSCONF and pairings, and the console's sync button.

#include "bt_passthrough.h"

#include "bt_hci_bridge.h"
#include "bt_transport.h"
#include "det_clock.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "netplay_session.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#define RT_TAG_BT "bluetooth"

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace BtPassthrough {

namespace {

constexpr const char* kDevicePath = "/dev/usb/oh1/57e/305";
constexpr int32_t kDeviceFd = 0x40000305;  // clear of the NAND (100+), network (2000+) and SHA (0x10001+) fds
constexpr auto kProbeWait = std::chrono::seconds(5);
constexpr auto kForgetHold = std::chrono::seconds(10);
// The pairings, in the NAND folder every game shares (the first game's; a second game's saves are
// in Saves/<key>/NAND): a remote has one link key for the adapter, so two games each keeping their
// own would keep breaking each other's pairing.
std::filesystem::path PassthroughFolder() {
    return RuntimeConfigFile::BaseApplicationDataDirectory() / "NAND" / "bluetooth-passthrough";
}

enum State : int { kUndecided, kOff, kOn };

std::atomic<int> g_state{kUndecided};
std::atomic<bool> g_probeStarted{false};
std::mutex g_mutex;  // g_status, the decision, g_transport/g_bridge setup
std::condition_variable g_decided;
Status g_status;
// Never freed: a game thread can still be inside them while the process exits.
BtTransport::Transport* g_transport = nullptr;
BtBridge::Bridge* g_bridge = nullptr;
bool g_transportStarted = false;
std::atomic<bool> g_open{false};
std::filesystem::path g_folder;  // set before the transport starts; read by its thread

std::mutex g_noticeMutex;
std::string g_notice;
std::chrono::steady_clock::time_point g_noticeUntil{};

std::atomic<bool> g_syncDown{false};
std::atomic<bool> g_forgetSent{false};
std::atomic<int64_t> g_syncDownSinceMs{0};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void ShowNotice(const std::string& text, std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    g_notice = text;
    g_noticeUntil = std::chrono::steady_clock::now() + duration;
}

bool IsOn() { return g_state.load(std::memory_order_acquire) == kOn; }

Mode ReadMode() {
    const std::string value = RuntimeConfigFile::BluetoothPassthroughMode();
    return value == "on" ? Mode::On : value == "off" ? Mode::Off : Mode::Auto;
}

const char* ModeName(Mode mode) { return mode == Mode::On ? "on" : mode == Mode::Off ? "off" : "auto"; }

// Off for this run, with the reason. Lock held.
void DecideOffLocked(const std::string& why) {
    g_status.decided = true;
    g_status.active = false;
    g_status.detail = why;
    g_state.store(kOff, std::memory_order_release);
    if (g_status.mode == Mode::On) {
        RT_LOGF(RT_TAG_BT, "passthrough is set to \"on\" but can't start: %s. The keyboard, controllers and "
                           "SDL's Wii Remotes work as usual.\n", why.c_str());
        ShowNotice("Bluetooth passthrough couldn't start: " + why, std::chrono::seconds(10));
    } else {
        RT_LOGF(RT_TAG_BT, "passthrough off: %s\n", why.c_str());
    }
    g_decided.notify_all();
}

struct GuestMemoryAccess final : BtBridge::GuestMemory {
    bool Read(uint32_t address, void* out, uint32_t size) override {
        if (size == 0) return true;
        if (!Memory::Contains(address, size)) return false;
        std::memcpy(out, Memory::GetPointer(address, size), size);
        return true;
    }
    bool Write(uint32_t address, const void* data, uint32_t size) override {
        if (size == 0) return true;
        if (!Memory::Contains(address, size)) return false;
        std::memcpy(Memory::GetPointer(address, size), data, size);
        return true;
    }
};

void Queue(const std::vector<BtBridge::Completion>& done) {
    for (const BtBridge::Completion& c : done) {
        NandQueueIosCallback(c.callback, c.result, c.arg);
    }
}

std::filesystem::path LinkKeyFile() { return g_folder / "link_keys.txt"; }

void SaveLinkKeys() {
    if (!g_bridge || g_folder.empty()) return;
    const std::string text = BtBridge::FormatLinkKeys(g_bridge->GetLinkKeys());
    const std::filesystem::path path = LinkKeyFile();
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            RT_LOGF(RT_TAG_BT, "couldn't write %s\n", RuntimeConfigFile::PathToUtf8(temp).c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp, path, ec);
    }
    RT_LOGF(RT_TAG_BT, "saved the pairings (%zu) to %s\n", g_bridge->GetLinkKeys().size(),
            RuntimeConfigFile::PathToUtf8(path).c_str());
}

void LoadLinkKeys() {
    std::ifstream in(LinkKeyFile(), std::ios::binary);
    if (!in) return;
    std::stringstream text;
    text << in.rdbuf();
    const BtBridge::LinkKeys keys = BtBridge::ParseLinkKeys(text.str());
    g_bridge->SetLinkKeys(keys);
    RT_LOGF(RT_TAG_BT, "loaded %zu pairing(s) from %s\n", keys.size(), RuntimeConfigFile::PathToUtf8(LinkKeyFile()).c_str());
}

// ---- the fake adapter (NSMBW_BT_FAKE_ADAPTER=1): every HCI command succeeds, nothing ever connects.
// NSMBW_BT_FAKE_SYNC_AT=<seconds>: it also presses the sync button that long after it starts. ----

class FakeTransport final : public BtTransport::Transport {
public:
    ~FakeTransport() override { Stop(); }
    void Start(BtBridge::Bridge& bridge, std::function<void()>) override {
        m_bridge = &bridge;
        bridge.SetWakeCallback([this] {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_wake = true;
            m_cv.notify_one();
        });
        m_running = true;
        m_thread = std::thread([this] { Loop(); });
    }
    void Stop() override {
        if (!m_running) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = false;
            m_cv.notify_one();
        }
        if (m_thread.joinable()) m_thread.join();
        if (m_bridge) m_bridge->SetWakeCallback(nullptr);
    }

private:
    void Answer(const std::vector<uint8_t>& command) {
        if (command.size() < 2) return;
        const uint16_t opcode = static_cast<uint16_t>(command[0] | (command[1] << 8));
        const uint8_t lo = command[0], hi = command[1];
        // Commands a controller answers with Command Status: inquiry, connections, QoS, sniff...
        const uint8_t ogf = static_cast<uint8_t>(opcode >> 10);
        const bool status = (ogf == 0x01 && opcode != 0x0402 && opcode != 0x0403 && opcode != 0x0408 &&
                             opcode != 0x040B && opcode != 0x040C && opcode != 0x040D && opcode != 0x040E) ||
                            opcode == 0x0803 || opcode == 0x0804 || opcode == 0x0807;
        if (status) {
            const uint8_t event[] = {BtBridge::kEvCommandStatus, 4, 0x00, 0x01, lo, hi};
            m_bridge->OnAdapterEventBytes(event, sizeof(event));
            if (opcode == 0x0401) {  // Inquiry: nobody answers
                const uint8_t done[] = {0x01, 1, 0x00};
                m_bridge->OnAdapterEventBytes(done, sizeof(done));
            }
            return;
        }
        std::vector<uint8_t> params(8, 0);  // status 0, then zeros
        if (opcode == 0x1009) params = {0x00, 0x56, 0x34, 0x12, 0x09, 0x1E, 0x00};  // Read BD_ADDR
        if (opcode == 0x1005) params = {0x00, 0xFD, 0x03, 0x40, 0x08, 0x00, 0x00, 0x00};  // Read Buffer Size
        if (opcode == 0x0C14) params.assign(249, 0);  // Read Local Name
        std::vector<uint8_t> event{BtBridge::kEvCommandComplete, static_cast<uint8_t>(3 + params.size()), 0x01, lo, hi};
        event.insert(event.end(), params.begin(), params.end());
        m_bridge->OnAdapterEventBytes(event.data(), event.size());
    }
    void Loop() {
        std::vector<uint8_t> packet;
        const char* syncAt = std::getenv("NSMBW_BT_FAKE_SYNC_AT");
        const auto syncTime = std::chrono::steady_clock::now() +
                              std::chrono::seconds(syncAt != nullptr ? std::atoi(syncAt) : 0);
        bool syncPending = syncAt != nullptr;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running) {
            m_cv.wait_for(lock, std::chrono::milliseconds(5), [this] { return m_wake || !m_running; });
            m_wake = false;
            lock.unlock();
            if (syncPending && std::chrono::steady_clock::now() >= syncTime) {
                syncPending = false;
                RT_LOGF(RT_TAG_BT, "fake adapter: pressing the sync button\n");
                m_bridge->PushSyncButtonEvent(false);
            }
            while (m_bridge->TakeCommand(packet, BtBridge::Clock::now())) Answer(packet);
            while (m_bridge->TakeAcl(packet)) {
            }
            lock.lock();
        }
    }
    BtBridge::Bridge* m_bridge = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_wake = false;
    bool m_running = false;
};

void Probe(Mode mode) {
    BtTransport::ProbeResult result;
    const char* fake = std::getenv("NSMBW_BT_FAKE_ADAPTER");
    if (fake != nullptr && std::strcmp(fake, "1") == 0) {
        result.transport = std::make_unique<FakeTransport>();
        result.adapter = "a fake adapter (NSMBW_BT_FAKE_ADAPTER=1)";
    } else {
#if defined(NSMBW_BT_PASSTHROUGH)
        result = BtTransport::ProbeLibusb(mode == Mode::On);
#else
        (void)mode;
#endif
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.load() != kUndecided) {
        // Too late: the game has started its controllers without it.
        RT_LOGF(RT_TAG_BT, "the adapter search finished after the game had started; left unused\n");
        if (result.transport) result.transport->Stop();
        return;
    }
    if (!result.transport) {
        DecideOffLocked(result.detail.empty() ? "no usable adapter" : result.detail);
        return;
    }
    g_transport = result.transport.release();
    g_bridge = new BtBridge::Bridge(result.wiiModule);
    g_status.decided = true;
    g_status.active = true;
    g_status.adapter = result.adapter;
    g_status.detail = "waiting for the game to start its Bluetooth stack";
    g_state.store(kOn, std::memory_order_release);
    RT_LOGF(RT_TAG_BT, "passthrough on (%s): %s%s. The game runs its own Wii Remote stack; the keyboard and "
                       "controllers don't drive it. Press F9 to pair remotes.\n",
            ModeName(mode), result.adapter.c_str(), result.wiiModule ? " (a Wii's own Bluetooth module)" : "");
    ShowNotice("Bluetooth passthrough: " + result.adapter + ". F9 pairs Wii Remotes.", std::chrono::seconds(8));
    g_decided.notify_all();
}

}  // namespace

void StartProbe() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::atexit(&Shutdown);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.mode = ReadMode();
#if defined(NSMBW_BT_PASSTHROUGH)
        g_status.available = true;
#else
        g_status.available = false;
        DecideOffLocked("not part of this build");
        return;
#endif
        if (g_status.mode == Mode::Off) {
            DecideOffLocked("bluetooth_passthrough = \"off\"");
        } else if (NetplaySession::Active()) {
            DecideOffLocked("LAN play uses the normal controls");
        } else if (DetClock::Enabled()) {
            DecideOffLocked("deterministic mode (NSMBW_DETERMINISTIC)");
        } else if (const char* script = std::getenv("NSMBW_INPUT_SCRIPT"); script != nullptr && *script != '\0') {
            DecideOffLocked("an input script (NSMBW_INPUT_SCRIPT) drives the game");
        } else {
            g_probeStarted.store(true);
            const Mode mode = g_status.mode;
            std::thread([mode] { Probe(mode); }).detach();
        }
    });
}

bool Active() {
    const int state = g_state.load(std::memory_order_acquire);
    if (state != kUndecided) return state == kOn;
    if (!g_probeStarted.load()) return false;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_decided.wait_for(lock, kProbeWait, [] { return g_state.load() != kUndecided; })) {
        DecideOffLocked("the adapter search took too long");
    }
    return g_state.load() == kOn;
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void BeforeGameWpadInit() {
#if defined(NSMBW_BT_PASSTHROUGH)
    constexpr uint32_t kHollywoodGpioOut = 0xCD0000C0u;  // HW_GPIOB_OUT; bit 8 powers the sensor bar
    if (GuestFlat::BackRegisterPage(kHollywoodGpioOut)) {
        RT_LOGF(RT_TAG_BT, "the game's own WPAD starts (the sensor bar's GPIO register is plain memory)\n");
    } else {
        RT_LOGF(RT_TAG_BT, "the game's own WPAD starts, but the sensor bar's GPIO register couldn't be backed\n");
    }
#endif
}

int32_t IosOpen(const char* path) {
    if (path == nullptr || std::strcmp(path, kDevicePath) != 0 || !Active()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_transportStarted) {
        g_transportStarted = true;
        g_folder = PassthroughFolder();
        std::error_code ec;
        std::filesystem::create_directories(g_folder, ec);
        LoadLinkKeys();
        g_bridge->SetLogCallback([](const std::string& line) { RT_LOGF(RT_TAG_BT, "%s\n", line.c_str()); });
        g_transport->Start(*g_bridge, [] { SaveLinkKeys(); });
    }
    g_status.detail = "in use by the game";
    g_open.store(true);
    RT_LOGF(RT_TAG_BT, "the game opened its Bluetooth module (%s)\n", kDevicePath);
    return kDeviceFd;
}

bool IsFd(uint32_t fd) {
    return static_cast<int32_t>(fd) == kDeviceFd && g_open.load(std::memory_order_relaxed);
}

int32_t IosClose(uint32_t fd) {
    if (!IsFd(fd)) return BtBridge::kIpcEinval;
    g_open.store(false);
    g_bridge->DropGuestReads();
    RT_LOGF(RT_TAG_BT, "the game closed its Bluetooth module\n");
    return 0;
}

void IosIoctlvAsync(uint32_t fd, uint32_t command, uint32_t numIn, uint32_t numOut, uint32_t vectors,
                    uint32_t callback, uint32_t callbackArg) {
    std::vector<BtBridge::Completion> done;
    if (!IsFd(fd)) {
        done.push_back({callback, BtBridge::kIpcEinval, callbackArg});
    } else {
        GuestMemoryAccess memory;
        g_bridge->Ioctlv(memory, command, numIn, numOut, vectors, callback, callbackArg, done);
    }
    Queue(done);
}

int32_t IosIoctlv(uint32_t fd, uint32_t command, uint32_t, uint32_t, uint32_t) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        RT_LOGF(RT_TAG_BT, "the game used the synchronous IOS_Ioctlv (command %u) on fd 0x%X: not supported\n", command, fd);
    }
    return BtBridge::kIpcEinval;
}

bool ProcessCompletions() {
    if (!IsOn() || !g_open.load(std::memory_order_relaxed)) {
        return false;
    }
    if (g_syncDown.load() && !g_forgetSent.load() &&
        NowMs() - g_syncDownSinceMs.load() >= std::chrono::milliseconds(kForgetHold).count()) {
        g_forgetSent.store(true);
        ForgetPairings();
    }
    std::vector<BtBridge::Completion> done;
    GuestMemoryAccess memory;
    g_bridge->Pump(memory, done);
    Queue(done);
    return !done.empty();
}

bool SysconfHostPath(std::filesystem::path& out) {
    if (!Active()) return false;
    static std::once_flag once;
    static std::filesystem::path path;
    std::call_once(once, [&] {
        const std::filesystem::path folder = PassthroughFolder();
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        path = folder / "SYSCONF";
        std::vector<uint8_t> existing;
        {
            std::ifstream in(path, std::ios::binary);
            if (in) existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (BtBridge::IsValidSysconf(existing)) {
            RT_LOGF(RT_TAG_BT, "SYSCONF: %s\n", RuntimeConfigFile::PathToUtf8(path).c_str());
            return;
        }
        if (!existing.empty()) {
            std::filesystem::path bad = path;
            bad += ".bad";
            std::filesystem::rename(path, bad, ec);
        }
        const std::vector<uint8_t> fresh = BtBridge::MakeDefaultSysconf();
        std::ofstream outFile(path, std::ios::binary | std::ios::trunc);
        outFile.write(reinterpret_cast<const char*>(fresh.data()), static_cast<std::streamsize>(fresh.size()));
        RT_LOGF(RT_TAG_BT, "SYSCONF: made %s (no remote paired yet)%s\n", RuntimeConfigFile::PathToUtf8(path).c_str(),
                outFile ? "" : " - writing it failed");
    });
    out = path;
    return true;
}

void SetSyncButtonDown(bool down) {
    if (!IsOn()) return;
    if (down && !g_syncDown.load()) {
        g_syncDownSinceMs.store(NowMs());
        g_forgetSent.store(false);
        g_syncDown.store(true);
    } else if (!down && g_syncDown.exchange(false)) {
        if (!g_forgetSent.load()) PressSyncButton();
    }
}

void PressSyncButton() {
    if (!IsOn()) return;
    g_bridge->PushSyncButtonEvent(false);
    RT_LOGF(RT_TAG_BT, "sync button: looking for Wii Remotes to pair\n");
    ShowNotice("Pairing: press the red SYNC button on each Wii Remote (under its battery cover).",
               std::chrono::seconds(8));
}

void ForgetPairings() {
    if (!IsOn()) return;
    g_bridge->PushSyncButtonEvent(true);
    RT_LOGF(RT_TAG_BT, "sync button held: forgetting every paired Wii Remote\n");
    ShowNotice("Forgot every paired Wii Remote.", std::chrono::seconds(6));
}

std::string Notice() {
    if (!IsOn() && g_status.mode != Mode::On) return {};
    if (g_syncDown.load() && !g_forgetSent.load()) {
        const int64_t held = NowMs() - g_syncDownSinceMs.load();
        if (held >= 1000) {
            const int64_t left = (std::chrono::milliseconds(kForgetHold).count() - held + 999) / 1000;
            return "Keep holding F9 to forget every paired Wii Remote (" + std::to_string(left) + " s)";
        }
    }
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    if (std::chrono::steady_clock::now() >= g_noticeUntil) return {};
    return g_notice;
}

void Shutdown() {
    BtTransport::Transport* transport = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::swap(transport, g_transport);
    }
    if (transport == nullptr) return;
    transport->Stop();
    if (g_bridge) {
        if (g_bridge->TakeLinkKeysChanged()) SaveLinkKeys();
        const BtBridge::Stats s = g_bridge->GetStats();
        RT_LOGF(RT_TAG_BT, "totals: %llu commands from the game (%llu sent), %llu events (%llu to the game), ACL %llu out / "
                           "%llu in, %llu dropped, %llu command timeouts\n",
                static_cast<unsigned long long>(s.commandsFromGame), static_cast<unsigned long long>(s.commandsToAdapter),
                static_cast<unsigned long long>(s.eventsFromAdapter), static_cast<unsigned long long>(s.eventsToGame),
                static_cast<unsigned long long>(s.aclToAdapter), static_cast<unsigned long long>(s.aclFromAdapter),
                static_cast<unsigned long long>(s.dropped), static_cast<unsigned long long>(s.commandTimeouts));
    }
}

}  // namespace BtPassthrough
