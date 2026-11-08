#pragma once

// Bluetooth passthrough, the part between the game and the adapter: the Wii's Bluetooth module as
// IOS shows it (/dev/usb/oh1/57e/305, IOS USB "v0" ioctlv requests), forwarded to a real USB
// Bluetooth adapter as HCI commands, HCI events and ACL data. The game's own Bluetooth stack (WPAD,
// WUD and Broadcom's BTE, all in main.dol) talks to it exactly as it talks to the console's module.
//
// The design follows Dolphin's BTReal / LibUSBBluetoothAdapter (GPL-2.0-or-later): the fake replies
// to Broadcom's patch-RAM commands, the Read Buffer Size rewrite, the QoS request after a remote
// connects and the sync button's vendor events. The code is written for this runtime. One thing
// is done differently: the module's stored link keys (the pairing secrets) are kept here, not in
// the adapter, because many adapters keep none or lose them on reset.
//
// Nothing here touches libusb or guest memory directly: the runtime hands in a GuestMemory and an
// adapter thread pulls commands and pushes events, so tests run it against a fake adapter.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace BtBridge {

using Clock = std::chrono::steady_clock;

// IOS USB v0 ioctlv commands.
constexpr uint32_t kIoctlvCtrlMsg = 0;
constexpr uint32_t kIoctlvBulkMsg = 1;
constexpr uint32_t kIoctlvIntrMsg = 2;
// The module's endpoints, as the game addresses them.
constexpr uint8_t kEpHciEvent = 0x81;
constexpr uint8_t kEpAclIn = 0x82;
constexpr uint8_t kEpAclOut = 0x02;
// What IOS answers a malformed request with.
constexpr int32_t kIpcEinval = -4;

// HCI opcodes and event codes used here.
constexpr uint16_t kOpReset = 0x0C03;
constexpr uint16_t kOpReadStoredLinkKey = 0x0C0D;
constexpr uint16_t kOpWriteStoredLinkKey = 0x0C11;
constexpr uint16_t kOpDeleteStoredLinkKey = 0x0C12;
constexpr uint16_t kOpReadBufferSize = 0x1005;
constexpr uint16_t kOpQosSetup = 0x0807;
constexpr uint16_t kOpBroadcomWriteRam = 0xFC4C;   // the Wii module's patch upload
constexpr uint16_t kOpBroadcomLaunchRam = 0xFC4F;  // and its start
constexpr uint8_t kEvConnectionComplete = 0x03;
constexpr uint8_t kEvDisconnectionComplete = 0x05;
constexpr uint8_t kEvCommandComplete = 0x0E;
constexpr uint8_t kEvCommandStatus = 0x0F;
constexpr uint8_t kEvReturnLinkKeys = 0x15;
constexpr uint8_t kEvLinkKeyNotification = 0x18;
constexpr uint8_t kEvVendor = 0xFF;
// The vendor event's payload when the console's red sync button is pressed, or held to forget
// every paired remote.
constexpr uint8_t kSyncButtonPressed = 0x08;
constexpr uint8_t kSyncButtonHeld = 0x09;

// The Wii module's buffers, which the game's stack is built around (Dolphin's ACL_PKT_SIZE etc.).
constexpr uint16_t kWiiAclPacketSize = 339;
constexpr uint16_t kWiiAclPacketCount = 10;
constexpr uint8_t kWiiScoPacketSize = 64;
// The stored link key table's size reported to the game.
constexpr uint16_t kStoredLinkKeyCapacity = 255;

// A Bluetooth device address in HCI byte order (least significant byte first), and a link key.
using BdAddr = std::array<uint8_t, 6>;
using LinkKey = std::array<uint8_t, 16>;
using LinkKeys = std::map<BdAddr, LinkKey>;

// Guest memory, as the runtime sees it. Addresses are guest addresses; false when out of range.
struct GuestMemory {
    virtual ~GuestMemory() = default;
    virtual bool Read(uint32_t address, void* out, uint32_t size) = 0;
    virtual bool Write(uint32_t address, const void* data, uint32_t size) = 0;
};

// A finished guest request: IOS calls callback(result, arg).
struct Completion {
    uint32_t callback = 0;
    int32_t result = 0;
    uint32_t arg = 0;
};

// Splits a byte stream from one endpoint into whole packets by their headers: HCI events
// (2-byte header, 1-byte length at offset 1) or ACL data (4-byte header, 16-bit length at
// offset 2), so a packet split over two USB transfers, or two packets in one, still arrive whole.
class PacketAssembler {
public:
    enum class Kind { Event, Acl };
    explicit PacketAssembler(Kind kind) : m_kind(kind) {}
    // Calls `emit(data, size)` for every complete packet; returns the number of bytes dropped as
    // unparseable (a length no packet can have).
    size_t Feed(const uint8_t* data, size_t size, const std::function<void(const uint8_t*, size_t)>& emit);
    size_t Buffered() const { return m_buffer.size(); }

private:
    Kind m_kind;
    std::vector<uint8_t> m_buffer;
};

struct Stats {
    uint64_t commandsFromGame = 0;
    uint64_t commandsToAdapter = 0;
    uint64_t eventsFromAdapter = 0;
    uint64_t eventsToGame = 0;
    uint64_t aclToAdapter = 0;
    uint64_t aclFromAdapter = 0;
    uint64_t aclToGame = 0;
    uint64_t dropped = 0;
    uint64_t commandTimeouts = 0;
};

class Bridge {
public:
    // `wiiModule`: the adapter is the Wii's own module (057e:0305), which needs none of the fakes.
    explicit Bridge(bool wiiModule);

    // ---- the emulation thread (where the game's IOS calls and the alarm pump run) ----

    // One IOS_IoctlvAsync on the device. Finished requests are appended to `done`; reads that wait
    // for the adapter finish later, from Pump.
    void Ioctlv(GuestMemory& memory, uint32_t command, uint32_t numIn, uint32_t numOut, uint32_t vectors,
                uint32_t callback, uint32_t arg, std::vector<Completion>& done);
    // Fills the game's waiting reads from what the adapter sent.
    void Pump(GuestMemory& memory, std::vector<Completion>& done);
    // The game closed the device: its waiting reads are forgotten (IOS would cancel them; their
    // callbacks are not run, so nothing resubmits into a closed device).
    void DropGuestReads();
    // The console's sync button: queued ahead of everything else for the game's next event read.
    void PushSyncButtonEvent(bool held);
    size_t PendingGuestReads() const;

    // ---- the adapter thread ----

    // The next HCI command for the adapter, honouring the adapter's command credits (one command
    // in flight unless it says otherwise; a silent adapter gets the next one after a second).
    bool TakeCommand(std::vector<uint8_t>& out, Clock::time_point now);
    // The next ACL packet for the adapter.
    bool TakeAcl(std::vector<uint8_t>& out);
    // Bytes the adapter sent on its HCI event (interrupt) and ACL (bulk) endpoints.
    void OnAdapterEventBytes(const uint8_t* data, size_t size);
    void OnAdapterAclBytes(const uint8_t* data, size_t size);
    // Called (without the lock) whenever there is something new for the adapter thread.
    void SetWakeCallback(std::function<void()> wake);
    // Called (on the adapter thread, without the lock) with each line worth a log entry.
    void SetLogCallback(std::function<void(const std::string&)> log);

    // ---- the stored link keys (any thread) ----
    void SetLinkKeys(const LinkKeys& keys);
    LinkKeys GetLinkKeys() const;
    // True once after the game paired, re-paired or forgot a remote.
    bool TakeLinkKeysChanged();

    Stats GetStats() const;

private:
    struct Read {
        uint32_t buffer = 0;
        uint32_t size = 0;
        uint32_t callback = 0;
        uint32_t arg = 0;
    };
    struct Command {
        std::vector<uint8_t> bytes;
        bool injected = false;  // ours, not the game's: its reply is not passed on
    };

    void HandleGameCommand(const uint8_t* bytes, size_t size);
    void HandleAdapterEvent(const uint8_t* event, size_t size, std::vector<std::string>& logs);
    void PushEventLocked(std::vector<uint8_t> event, bool front = false);
    void PushCommandCompleteLocked(uint16_t opcode, const std::vector<uint8_t>& params);
    void Wake();
    static void FillReads(GuestMemory& memory, std::deque<Read>& reads, std::deque<std::vector<uint8_t>>& data,
                          uint64_t& counter, std::vector<Completion>& done);

    const bool m_wiiModule;
    mutable std::mutex m_mutex;
    // To the game.
    std::deque<std::vector<uint8_t>> m_events;
    std::deque<std::vector<uint8_t>> m_aclIn;
    std::deque<Read> m_eventReads;
    std::deque<Read> m_aclReads;
    // To the adapter.
    std::deque<Command> m_commands;
    std::deque<std::vector<uint8_t>> m_aclOut;
    uint32_t m_commandCredits = 1;
    Clock::time_point m_lastCommandSent{};
    std::map<uint16_t, uint32_t> m_injectedOutstanding;
    // The adapter's byte streams.
    PacketAssembler m_eventAssembler{PacketAssembler::Kind::Event};
    PacketAssembler m_aclAssembler{PacketAssembler::Kind::Acl};
    // The module's stored link keys.
    LinkKeys m_linkKeys;
    bool m_linkKeysChanged = false;
    Stats m_stats;
    // The log keeps the first few of each command and event kind (the game repeats some, e.g. Write
    // Scan Enable a few times a second while no remote is connected).
    bool ShouldLogLocked(uint32_t key);
    std::map<uint32_t, uint32_t> m_logCounts;
    std::function<void()> m_wake;
    std::function<void(const std::string&)> m_log;
};

// The link key file next to the passthrough's SYSCONF: one "aa:bb:cc:dd:ee:ff <32 hex digits>" per
// line (the address as people write it, the key in HCI byte order).
std::string FormatLinkKeys(const LinkKeys& keys);
LinkKeys ParseLinkKeys(const std::string& text);

// A SYSCONF holding only the Bluetooth entries (BT.DINF, BT.CDIF, BT.SENS, BT.BAR, BT.SPKV, BT.MOT)
// at the defaults of a console that has no remote paired yet: 0x4000 bytes, "SCv0" ... "SCed".
std::vector<uint8_t> MakeDefaultSysconf();
// True for a buffer the SDK's SC library will load: the size, both magics, a sane entry table.
bool IsValidSysconf(const std::vector<uint8_t>& data);

}  // namespace BtBridge
