// Bluetooth passthrough: the emulated Wii Bluetooth module between the game's IOS requests and a
// real adapter's HCI traffic (bt_hci_bridge.h).

#include "bt_hci_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace BtBridge {

namespace {

// Per queue. A few seconds of four remotes' reports: a game that stops reading for longer loses the oldest.
constexpr size_t kMaxQueuedPackets = 8192;
constexpr size_t kMaxEventSize = 2 + 255;
constexpr size_t kMaxAclSize = 4 + 4096;  // above any adapter's ACL buffer
constexpr auto kCommandTimeout = std::chrono::seconds(1);
constexpr uint32_t kLoggedPerKind = 4;

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void PutLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

bool ReadBe32(GuestMemory& memory, uint32_t address, uint32_t& value) {
    uint8_t b[4];
    if (!memory.Read(address, b, 4)) return false;
    value = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    return true;
}

struct IoVec {
    uint32_t address = 0;
    uint32_t size = 0;
};

bool ReadVectors(GuestMemory& memory, uint32_t vectors, uint32_t count, std::vector<IoVec>& out) {
    if (vectors == 0 || count > 16) return false;
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!ReadBe32(memory, vectors + i * 8, out[i].address) || !ReadBe32(memory, vectors + i * 8 + 4, out[i].size)) {
            return false;
        }
    }
    return true;
}

bool ReadU8(GuestMemory& memory, const IoVec& v, uint8_t& value) {
    return v.size >= 1 && memory.Read(v.address, &value, 1);
}

std::string Hex16(uint16_t v) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%04X", v);
    return text;
}

std::string FormatAddress(const BdAddr& a) {
    char text[24];
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    return text;
}

BdAddr AddressAt(const uint8_t* p) {
    BdAddr a;
    std::memcpy(a.data(), p, a.size());
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// PacketAssembler

size_t PacketAssembler::Feed(const uint8_t* data, size_t size,
                             const std::function<void(const uint8_t*, size_t)>& emit) {
    m_buffer.insert(m_buffer.end(), data, data + size);
    const size_t header = m_kind == Kind::Event ? 2 : 4;
    const size_t limit = m_kind == Kind::Event ? kMaxEventSize : kMaxAclSize;
    size_t dropped = 0;
    size_t offset = 0;
    while (m_buffer.size() - offset >= header) {
        const uint8_t* p = m_buffer.data() + offset;
        const size_t total = header + (m_kind == Kind::Event ? p[1] : Le16(p + 2));
        if (total > limit) {
            // Not a packet boundary any more: start over with the next transfer.
            dropped += m_buffer.size() - offset;
            offset = m_buffer.size();
            break;
        }
        if (m_buffer.size() - offset < total) break;
        emit(p, total);
        offset += total;
    }
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    return dropped;
}

// ---------------------------------------------------------------------------------------------
// Bridge

Bridge::Bridge(bool wiiModule) : m_wiiModule(wiiModule) {}

void Bridge::SetWakeCallback(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wake = std::move(wake);
}

void Bridge::SetLogCallback(std::function<void(const std::string&)> log) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_log = std::move(log);
}

void Bridge::Wake() {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        wake = m_wake;
    }
    if (wake) wake();
}

void Bridge::Ioctlv(GuestMemory& memory, uint32_t command, uint32_t numIn, uint32_t numOut, uint32_t vectors,
                    uint32_t callback, uint32_t arg, std::vector<Completion>& done) {
    std::vector<IoVec> v;
    const auto fail = [&] { done.push_back({callback, kIpcEinval, arg}); };
    if (!ReadVectors(memory, vectors, numIn + numOut, v)) {
        fail();
        return;
    }
    switch (command) {
    case kIoctlvCtrlMsg: {
        // in: bmRequestType, bRequest, wValue, wIndex, wLength (little-endian, as the SDK writes
        // them), one unused byte; io: the data.
        if (numIn < 5 || numOut < 1) {
            fail();
            return;
        }
        uint8_t requestType = 0;
        uint8_t length[2] = {};
        const IoVec& data = v[numIn];
        if (!ReadU8(memory, v[0], requestType) || v[4].size < 2 || !memory.Read(v[4].address, length, 2) ||
            (requestType & 0x80) != 0) {
            fail();  // the module is only ever sent HCI commands (host to device)
            return;
        }
        const uint32_t size = std::min<uint32_t>(Le16(length), data.size);
        std::vector<uint8_t> bytes(size);
        if (size != 0 && !memory.Read(data.address, bytes.data(), size)) {
            fail();
            return;
        }
        HandleGameCommand(bytes.data(), bytes.size());
        done.push_back({callback, static_cast<int32_t>(size), arg});
        Wake();
        return;
    }
    case kIoctlvBulkMsg:
    case kIoctlvIntrMsg: {
        // in: the endpoint, the length (big-endian); io: the buffer.
        if (numIn < 2 || numOut < 1) {
            fail();
            return;
        }
        uint8_t endpoint = 0;
        uint8_t lengthBe[2] = {};
        if (!ReadU8(memory, v[0], endpoint) || v[1].size < 2 || !memory.Read(v[1].address, lengthBe, 2)) {
            fail();
            return;
        }
        const IoVec& data = v[numIn];
        const uint32_t size = std::min<uint32_t>(static_cast<uint32_t>((lengthBe[0] << 8) | lengthBe[1]), data.size);
        if (command == kIoctlvIntrMsg) {
            if (endpoint != kEpHciEvent) {
                fail();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_eventReads.push_back({data.address, size, callback, arg});
            }
            Pump(memory, done);
            return;
        }
        if ((endpoint & 0x80) != 0) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_aclReads.push_back({data.address, size, callback, arg});
            }
            Pump(memory, done);
            return;
        }
        std::vector<uint8_t> bytes(size);
        if (size != 0 && !memory.Read(data.address, bytes.data(), size)) {
            fail();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_aclOut.size() >= kMaxQueuedPackets) {
                m_aclOut.pop_front();
                ++m_stats.dropped;
            }
            m_aclOut.push_back(std::move(bytes));
        }
        // Sent on: the adapter's own flow control (Number Of Completed Packets) paces the game.
        done.push_back({callback, static_cast<int32_t>(size), arg});
        Wake();
        return;
    }
    default:
        fail();
        return;
    }
}

void Bridge::FillReads(GuestMemory& memory, std::deque<Read>& reads, std::deque<std::vector<uint8_t>>& data,
                       uint64_t& counter, std::vector<Completion>& done) {
    while (!reads.empty() && !data.empty()) {
        const Read read = reads.front();
        const std::vector<uint8_t>& packet = data.front();
        const uint32_t size = std::min<uint32_t>(static_cast<uint32_t>(packet.size()), read.size);
        const bool ok = size == 0 || memory.Write(read.buffer, packet.data(), size);
        done.push_back({read.callback, ok ? static_cast<int32_t>(size) : kIpcEinval, read.arg});
        reads.pop_front();
        data.pop_front();
        ++counter;
    }
}

void Bridge::Pump(GuestMemory& memory, std::vector<Completion>& done) {
    std::lock_guard<std::mutex> lock(m_mutex);
    FillReads(memory, m_eventReads, m_events, m_stats.eventsToGame, done);
    FillReads(memory, m_aclReads, m_aclIn, m_stats.aclToGame, done);
}

void Bridge::DropGuestReads() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_eventReads.clear();
    m_aclReads.clear();
}

size_t Bridge::PendingGuestReads() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_eventReads.size() + m_aclReads.size();
}

void Bridge::PushSyncButtonEvent(bool held) {
    std::lock_guard<std::mutex> lock(m_mutex);
    PushEventLocked({kEvVendor, 0x01, held ? kSyncButtonHeld : kSyncButtonPressed}, true);
}

void Bridge::PushEventLocked(std::vector<uint8_t> event, bool front) {
    if (m_events.size() >= kMaxQueuedPackets) {
        m_events.pop_front();
        ++m_stats.dropped;
    }
    if (front) {
        m_events.push_front(std::move(event));
    } else {
        m_events.push_back(std::move(event));
    }
}

void Bridge::PushCommandCompleteLocked(uint16_t opcode, const std::vector<uint8_t>& params) {
    std::vector<uint8_t> event{kEvCommandComplete, static_cast<uint8_t>(3 + params.size()), 0x01};
    PutLe16(event, opcode);
    event.insert(event.end(), params.begin(), params.end());
    PushEventLocked(std::move(event));
}

void Bridge::HandleGameCommand(const uint8_t* bytes, size_t size) {
    std::string line;
    std::function<void(const std::string&)> log;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.commandsFromGame;
        if (size < 3) {
            return;
        }
        const uint16_t opcode = Le16(bytes);
        const uint8_t* params = bytes + 3;
        const size_t paramSize = std::min<size_t>(bytes[2], size - 3);
        if (ShouldLogLocked(opcode)) {
            line = "game: HCI command " + Hex16(opcode) + " (" + std::to_string(paramSize) + " bytes)";
            log = m_log;
        }
        bool answered = false;
        if (!m_wiiModule) {
            switch (opcode) {
            case kOpBroadcomWriteRam:
            case kOpBroadcomLaunchRam:
                // The Wii module's firmware patch: other adapters would reject it, so it "succeeds".
                PushCommandCompleteLocked(opcode, {0x00});
                answered = true;
                break;
            case kOpReadStoredLinkKey: {
                // The pairings the game reads back at boot: from this runtime's own table.
                const bool all = paramSize >= 7 && params[6] != 0;
                std::vector<std::pair<BdAddr, LinkKey>> keys;
                for (const auto& [address, key] : m_linkKeys) {
                    if (all || (paramSize >= 6 && std::memcmp(address.data(), params, 6) == 0)) {
                        keys.emplace_back(address, key);
                    }
                }
                for (size_t first = 0; first < keys.size(); first += 11) {
                    const size_t count = std::min<size_t>(11, keys.size() - first);
                    std::vector<uint8_t> event{kEvReturnLinkKeys, static_cast<uint8_t>(1 + 22 * count),
                                               static_cast<uint8_t>(count)};
                    for (size_t i = first; i < first + count; ++i) {
                        event.insert(event.end(), keys[i].first.begin(), keys[i].first.end());
                        event.insert(event.end(), keys[i].second.begin(), keys[i].second.end());
                    }
                    PushEventLocked(std::move(event));
                }
                std::vector<uint8_t> reply{0x00};
                PutLe16(reply, kStoredLinkKeyCapacity);
                PutLe16(reply, static_cast<uint16_t>(keys.size()));
                PushCommandCompleteLocked(opcode, reply);
                answered = true;
                break;
            }
            case kOpWriteStoredLinkKey: {
                const size_t count = paramSize >= 1 ? std::min<size_t>(params[0], (paramSize - 1) / 22) : 0;
                for (size_t i = 0; i < count; ++i) {
                    const uint8_t* entry = params + 1 + i * 22;
                    LinkKey key;
                    std::memcpy(key.data(), entry + 6, key.size());
                    m_linkKeys[AddressAt(entry)] = key;
                }
                m_linkKeysChanged = m_linkKeysChanged || count != 0;
                PushCommandCompleteLocked(opcode, {0x00, static_cast<uint8_t>(count)});
                answered = true;
                break;
            }
            case kOpDeleteStoredLinkKey: {
                size_t deleted = 0;
                if (paramSize >= 7 && params[6] != 0) {
                    deleted = m_linkKeys.size();
                    m_linkKeys.clear();
                } else if (paramSize >= 6) {
                    deleted = m_linkKeys.erase(AddressAt(params));
                }
                m_linkKeysChanged = m_linkKeysChanged || deleted != 0;
                std::vector<uint8_t> reply{0x00};
                PutLe16(reply, static_cast<uint16_t>(deleted));
                PushCommandCompleteLocked(opcode, reply);
                answered = true;
                break;
            }
            default:
                break;
            }
        }
        if (!answered) {
            m_commands.push_back({std::vector<uint8_t>(bytes, bytes + size), false});
        } else if (log) {
            line += ", answered here";
        }
    }
    if (log) log(line);
}

bool Bridge::TakeCommand(std::vector<uint8_t>& out, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_commands.empty()) return false;
    if (m_commandCredits == 0) {
        if (now - m_lastCommandSent < kCommandTimeout) return false;
        m_commandCredits = 1;  // the adapter never answered the last one
        ++m_stats.commandTimeouts;
    }
    Command command = std::move(m_commands.front());
    m_commands.pop_front();
    --m_commandCredits;
    m_lastCommandSent = now;
    if (command.injected && command.bytes.size() >= 2) {
        ++m_injectedOutstanding[Le16(command.bytes.data())];
    }
    ++m_stats.commandsToAdapter;
    out = std::move(command.bytes);
    return true;
}

bool Bridge::TakeAcl(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_aclOut.empty()) return false;
    out = std::move(m_aclOut.front());
    m_aclOut.pop_front();
    ++m_stats.aclToAdapter;
    return true;
}

void Bridge::OnAdapterEventBytes(const uint8_t* data, size_t size) {
    std::vector<std::string> logs;
    std::function<void(const std::string&)> log;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.dropped += m_eventAssembler.Feed(data, size, [&](const uint8_t* event, size_t eventSize) {
            HandleAdapterEvent(event, eventSize, logs);
        });
        log = m_log;
    }
    if (log) {
        for (const std::string& line : logs) log(line);
    }
}

void Bridge::OnAdapterAclBytes(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.dropped += m_aclAssembler.Feed(data, size, [&](const uint8_t* packet, size_t packetSize) {
        ++m_stats.aclFromAdapter;
        if (m_aclIn.size() >= kMaxQueuedPackets) {
            m_aclIn.pop_front();
            ++m_stats.dropped;
        }
        m_aclIn.emplace_back(packet, packet + packetSize);
    });
}

// With the lock held, on the adapter thread.
void Bridge::HandleAdapterEvent(const uint8_t* event, size_t size, std::vector<std::string>& logs) {
    ++m_stats.eventsFromAdapter;
    std::vector<uint8_t> copy(event, event + size);
    const uint8_t code = copy[0];
    const auto swallowed = [&](uint16_t opcode) {
        auto it = m_injectedOutstanding.find(opcode);
        if (it == m_injectedOutstanding.end() || it->second == 0) return false;
        --it->second;
        return true;
    };
    // Not Number Of Completed Packets (one per ACL packet sent); command replies by their opcode.
    const uint32_t logKey = 0x10000u | (uint32_t(code) << 16) |
                            ((code == kEvCommandComplete && size >= 6) ? Le16(&copy[3])
                             : (code == kEvCommandStatus && size >= 6) ? Le16(&copy[4]) : 0u);
    bool logIt = code != 0x13 && ShouldLogLocked(logKey);
    std::string note;
    if (code == kEvCommandComplete && size >= 6) {
        m_commandCredits = copy[2];
        const uint16_t opcode = Le16(&copy[3]);
        note = " for " + Hex16(opcode) + ", status " + std::to_string(copy[5]);
        if (swallowed(opcode)) {
            if (logIt) logs.push_back("adapter: Command Complete" + note + " (ours, not passed on)");
            return;
        }
        if (opcode == kOpReadBufferSize && !m_wiiModule && size >= 13) {
            // The game's stack is built around the Wii module's buffers; never claim more than ten.
            const uint16_t packets = Le16(&copy[9]);
            copy[5] = 0x00;
            copy[6] = static_cast<uint8_t>(kWiiAclPacketSize);
            copy[7] = static_cast<uint8_t>(kWiiAclPacketSize >> 8);
            copy[8] = kWiiScoPacketSize;
            const uint16_t used = (event[5] != 0 || packets == 0) ? kWiiAclPacketCount
                                                                  : std::min(packets, kWiiAclPacketCount);
            copy[9] = static_cast<uint8_t>(used);
            copy[10] = static_cast<uint8_t>(used >> 8);
            copy[11] = 0;
            copy[12] = 0;
            logs.push_back("adapter: " + std::to_string(Le16(&event[6])) + "-byte ACL buffers x" +
                           std::to_string(packets) + ", shown to the game as the Wii module's " +
                           std::to_string(kWiiAclPacketSize) + " x" + std::to_string(used));
        }
    } else if (code == kEvCommandStatus && size >= 6) {
        m_commandCredits = copy[3];
        const uint16_t opcode = Le16(&copy[4]);
        note = " for " + Hex16(opcode) + ", status " + std::to_string(copy[2]);
        if (swallowed(opcode)) {
            if (logIt) logs.push_back("adapter: Command Status" + note + " (ours, not passed on)");
            return;
        }
    } else if (code == kEvConnectionComplete && size >= 13) {
        logIt = true;
        const uint16_t handle = Le16(&copy[3]) & 0x0FFF;
        note = ": status " + std::to_string(copy[2]) + ", " + FormatAddress(AddressAt(&copy[5])) + ", handle " +
               std::to_string(handle);
        if (copy[2] == 0 && copy[11] == 0x01 && !m_wiiModule) {
            // A remote connected: ask for guaranteed service, or some adapters (best effort by default)
            // deliver its reports at well under 200 a second.
            std::vector<uint8_t> qos{static_cast<uint8_t>(kOpQosSetup), static_cast<uint8_t>(kOpQosSetup >> 8), 20};
            PutLe16(qos, handle);
            qos.push_back(0x00);  // flags
            qos.push_back(0x02);  // service type: guaranteed
            PutLe32(qos, 0xFFFFFFFFu);  // token rate
            PutLe32(qos, 0xFFFFFFFFu);  // peak bandwidth
            PutLe32(qos, 10000);        // latency, microseconds
            PutLe32(qos, 0xFFFFFFFFu);  // delay variation
            m_commands.push_back({std::move(qos), true});
        }
    } else if (code == kEvDisconnectionComplete && size >= 6) {
        logIt = true;
        note = ": handle " + std::to_string(Le16(&copy[3]) & 0x0FFF) + ", reason " + std::to_string(copy[5]);
    } else if (code == kEvLinkKeyNotification && size >= 25) {
        logIt = true;
        const BdAddr address = AddressAt(&copy[2]);
        note = ": " + FormatAddress(address) + " paired";
        if (!m_wiiModule) {
            LinkKey key;
            std::memcpy(key.data(), &copy[8], key.size());
            m_linkKeys[address] = key;
            m_linkKeysChanged = true;
        }
    }
    if (logIt) {
        char head[48];
        std::snprintf(head, sizeof(head), "adapter: HCI event 0x%02X", code);
        logs.push_back(head + note);
    }
    PushEventLocked(std::move(copy));
}

bool Bridge::ShouldLogLocked(uint32_t key) {
    return ++m_logCounts[key] <= kLoggedPerKind;
}

void Bridge::SetLinkKeys(const LinkKeys& keys) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_linkKeys = keys;
}

LinkKeys Bridge::GetLinkKeys() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_linkKeys;
}

bool Bridge::TakeLinkKeysChanged() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool changed = m_linkKeysChanged;
    m_linkKeysChanged = false;
    return changed;
}

Stats Bridge::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

// ---------------------------------------------------------------------------------------------
// Link key file

std::string FormatLinkKeys(const LinkKeys& keys) {
    std::string text =
        "# Wii Remote pairings for Bluetooth passthrough: address, then the link key in HCI byte order.\n"
        "# Delete this file (or hold the sync key for 10 seconds in the game) to forget them all.\n";
    for (const auto& [address, key] : keys) {
        text += FormatAddress(address);
        text += ' ';
        char digits[3];
        for (uint8_t b : key) {
            std::snprintf(digits, sizeof(digits), "%02x", b);
            text += digits;
        }
        text += '\n';
    }
    return text;
}

LinkKeys ParseLinkKeys(const std::string& text) {
    LinkKeys keys;
    std::istringstream in(text);
    std::string line;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string addressText, keyText;
        fields >> addressText >> keyText;
        if (addressText.size() != 17 || keyText.size() != 32) continue;
        BdAddr address;
        LinkKey key;
        bool ok = true;
        for (int i = 0; i < 6 && ok; ++i) {
            const int hi = nibble(addressText[i * 3]);
            const int lo = nibble(addressText[i * 3 + 1]);
            ok = hi >= 0 && lo >= 0 && (i == 5 || addressText[i * 3 + 2] == ':');
            address[5 - i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        for (int i = 0; i < 16 && ok; ++i) {
            const int hi = nibble(keyText[i * 2]);
            const int lo = nibble(keyText[i * 2 + 1]);
            ok = hi >= 0 && lo >= 0;
            key[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        if (ok) keys[address] = key;
    }
    return keys;
}

// ---------------------------------------------------------------------------------------------
// SYSCONF

namespace {

constexpr size_t kSysconfSize = 0x4000;
enum SysconfType : uint8_t { kBigArray = 1, kSmallArray = 2, kByte = 3, kShort = 4, kLong = 5, kLongLong = 6, kBool = 7 };

struct SysconfEntry {
    const char* name;
    SysconfType type;
    std::vector<uint8_t> data;
};

}  // namespace

std::vector<uint8_t> MakeDefaultSysconf() {
    // The values a console's SYSCONF has before any remote is paired (as Dolphin's defaults):
    // no registered remotes, sensor bar sensitivity 3, bar above the TV, speaker volume 0x58,
    // rumble on.
    const std::vector<SysconfEntry> entries = {
        {"BT.DINF", kBigArray, std::vector<uint8_t>(0x461, 0)},
        {"BT.CDIF", kBigArray, std::vector<uint8_t>(0x205, 0)},
        {"BT.SENS", kLong, {0x00, 0x00, 0x00, 0x03}},
        {"BT.BAR", kByte, {0x01}},
        {"BT.SPKV", kByte, {0x58}},
        {"BT.MOT", kByte, {0x01}},
    };
    std::vector<uint8_t> out(kSysconfSize, 0);
    std::memcpy(out.data(), "SCv0", 4);
    const size_t count = entries.size();
    out[4] = static_cast<uint8_t>(count >> 8);
    out[5] = static_cast<uint8_t>(count);
    size_t offset = 6 + 2 * (count + 1);
    for (size_t i = 0; i <= count; ++i) {
        out[6 + 2 * i] = static_cast<uint8_t>(offset >> 8);
        out[7 + 2 * i] = static_cast<uint8_t>(offset);
        if (i == count) break;
        const SysconfEntry& e = entries[i];
        const size_t nameSize = std::strlen(e.name);
        out[offset++] = static_cast<uint8_t>((e.type << 5) | (nameSize - 1));
        std::memcpy(&out[offset], e.name, nameSize);
        offset += nameSize;
        if (e.type == kBigArray) {
            const size_t stored = e.data.size() - 1;
            out[offset++] = static_cast<uint8_t>(stored >> 8);
            out[offset++] = static_cast<uint8_t>(stored);
        } else if (e.type == kSmallArray) {
            out[offset++] = static_cast<uint8_t>(e.data.size() - 1);
        }
        std::memcpy(&out[offset], e.data.data(), e.data.size());
        offset += e.data.size();
    }
    std::memcpy(&out[kSysconfSize - 4], "SCed", 4);
    return out;
}

bool IsValidSysconf(const std::vector<uint8_t>& data) {
    if (data.size() != kSysconfSize || std::memcmp(data.data(), "SCv0", 4) != 0 ||
        std::memcmp(&data[kSysconfSize - 4], "SCed", 4) != 0) {
        return false;
    }
    const size_t count = (size_t(data[4]) << 8) | data[5];
    if (count == 0 || 6 + 2 * (count + 1) >= kSysconfSize) return false;
    bool hasDinf = false;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = (size_t(data[6 + 2 * i]) << 8) | data[7 + 2 * i];
        if (offset + 1 >= kSysconfSize - 4) return false;
        const size_t nameSize = (data[offset] & 0x1F) + 1;
        if (offset + 1 + nameSize >= kSysconfSize - 4) return false;
        if (nameSize == 7 && std::memcmp(&data[offset + 1], "BT.DINF", 7) == 0) hasDinf = true;
    }
    return hasDinf;
}

}  // namespace BtBridge
