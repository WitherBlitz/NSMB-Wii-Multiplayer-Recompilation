// Bluetooth passthrough's emulated module (bt_hci_bridge.h) against fake guest memory and a fake
// adapter: IOS USB v0 requests as the game's IUSB library builds them, HCI round trips, ACL both
// ways, completion order, and the fakes it answers itself.

#include "bt_hci_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace BtBridge;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr uint32_t kBase = 0x90000000u;

struct FakeMemory final : GuestMemory {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000, 0);
    bool Read(uint32_t address, void* out, uint32_t size) override {
        if (address < kBase || address + size > kBase + bytes.size()) return false;
        std::memcpy(out, &bytes[address - kBase], size);
        return true;
    }
    bool Write(uint32_t address, const void* data, uint32_t size) override {
        if (address < kBase || address + size > kBase + bytes.size()) return false;
        std::memcpy(&bytes[address - kBase], data, size);
        return true;
    }
    void Put8(uint32_t a, uint8_t v) { bytes[a - kBase] = v; }
    void PutBe16(uint32_t a, uint16_t v) { Put8(a, uint8_t(v >> 8)); Put8(a + 1, uint8_t(v)); }
    void PutLe16(uint32_t a, uint16_t v) { Put8(a, uint8_t(v)); Put8(a + 1, uint8_t(v >> 8)); }
    void PutBe32(uint32_t a, uint32_t v) { PutBe16(a, uint16_t(v >> 16)); PutBe16(a + 2, uint16_t(v)); }
    void PutBytes(uint32_t a, const std::vector<uint8_t>& v) { std::memcpy(&bytes[a - kBase], v.data(), v.size()); }
    std::vector<uint8_t> Get(uint32_t a, size_t n) const { return {bytes.begin() + (a - kBase), bytes.begin() + (a - kBase) + n}; }
};

// Builds the requests the way IUSB_WriteCtrlMsgAsync / __IUSB_BlkIntrMsg lay them out.
struct Game {
    FakeMemory memory;
    Bridge& bridge;
    uint32_t next = kBase + 0x100;
    std::vector<Completion> done;
    explicit Game(Bridge& b) : bridge(b) {}

    uint32_t Alloc(uint32_t size) {
        const uint32_t a = next;
        next += (size + 31) & ~31u;
        return a;
    }
    void Command(const std::vector<uint8_t>& hci, uint32_t callback) {
        const uint32_t type = Alloc(1), request = Alloc(1), value = Alloc(2), index = Alloc(2), length = Alloc(2),
                       unused = Alloc(1), data = Alloc(static_cast<uint32_t>(hci.size())), vec = Alloc(56);
        memory.Put8(type, 0x20);
        memory.Put8(request, 0);
        memory.PutLe16(value, 0);
        memory.PutLe16(index, 0);
        memory.PutLe16(length, static_cast<uint16_t>(hci.size()));
        memory.Put8(unused, 0);
        memory.PutBytes(data, hci);
        const uint32_t vectors[7][2] = {{type, 1}, {request, 1}, {value, 2}, {index, 2}, {length, 2}, {unused, 1},
                                        {data, static_cast<uint32_t>(hci.size())}};
        for (int i = 0; i < 7; ++i) {
            memory.PutBe32(vec + i * 8, vectors[i][0]);
            memory.PutBe32(vec + i * 8 + 4, vectors[i][1]);
        }
        bridge.Ioctlv(memory, kIoctlvCtrlMsg, 6, 1, vec, callback, 0xC0DE, done);
    }
    // A bulk or interrupt request; returns the data buffer's address.
    uint32_t Transfer(uint32_t command, uint8_t endpoint, const std::vector<uint8_t>& out, uint16_t size, uint32_t callback,
                      uint32_t arg) {
        const uint32_t ep = Alloc(1), length = Alloc(2), data = Alloc(size), vec = Alloc(24);
        memory.Put8(ep, endpoint);
        memory.PutBe16(length, size);
        if (!out.empty()) memory.PutBytes(data, out);
        const uint32_t vectors[3][2] = {{ep, 1}, {length, 2}, {data, size}};
        for (int i = 0; i < 3; ++i) {
            memory.PutBe32(vec + i * 8, vectors[i][0]);
            memory.PutBe32(vec + i * 8 + 4, vectors[i][1]);
        }
        bridge.Ioctlv(memory, command, 2, 1, vec, callback, arg, done);
        return data;
    }
    uint32_t ReadEvent(uint32_t callback, uint32_t arg) { return Transfer(kIoctlvIntrMsg, kEpHciEvent, {}, 0x100, callback, arg); }
    uint32_t ReadAcl(uint32_t callback, uint32_t arg) { return Transfer(kIoctlvBulkMsg, kEpAclIn, {}, 0x200, callback, arg); }
    void Pump() { bridge.Pump(memory, done); }
};

std::vector<uint8_t> CommandComplete(uint16_t opcode, std::vector<uint8_t> params, uint8_t credits = 1) {
    std::vector<uint8_t> e{kEvCommandComplete, static_cast<uint8_t>(3 + params.size()), credits, uint8_t(opcode),
                           uint8_t(opcode >> 8)};
    e.insert(e.end(), params.begin(), params.end());
    return e;
}

void FeedEvent(Bridge& b, const std::vector<uint8_t>& e) { b.OnAdapterEventBytes(e.data(), e.size()); }

void TestCommandRoundTrip() {
    Bridge bridge(false);
    Game game(bridge);
    const uint32_t buffer = game.ReadEvent(0x80001000, 1);  // the stack's standing event read
    CHECK(game.done.empty());
    game.Command({0x03, 0x0C, 0x00}, 0x80002000);  // HCI_Reset
    CHECK(game.done.size() == 1 && game.done[0].callback == 0x80002000 && game.done[0].result == 3 &&
          game.done[0].arg == 0xC0DE);
    std::vector<uint8_t> sent;
    const auto now = Clock::now();
    CHECK(bridge.TakeCommand(sent, now));
    CHECK((sent == std::vector<uint8_t>{0x03, 0x0C, 0x00}));
    CHECK(!bridge.TakeCommand(sent, now));
    const auto reply = CommandComplete(kOpReset, {0x00});
    FeedEvent(bridge, reply);
    game.done.clear();
    game.Pump();
    CHECK(game.done.size() == 1 && game.done[0].callback == 0x80001000 && game.done[0].arg == 1 &&
          game.done[0].result == static_cast<int32_t>(reply.size()));
    CHECK(game.memory.Get(buffer, reply.size()) == reply);
}

void TestCommandCredits() {
    Bridge bridge(false);
    Game game(bridge);
    game.Command({0x01, 0x10, 0x00}, 1);  // Read Local Version
    game.Command({0x09, 0x10, 0x00}, 2);  // Read BD_ADDR
    std::vector<uint8_t> sent;
    const auto t0 = Clock::now();
    CHECK(bridge.TakeCommand(sent, t0) && sent[0] == 0x01);
    CHECK(!bridge.TakeCommand(sent, t0));  // one in flight until the adapter answers
    FeedEvent(bridge, CommandComplete(0x1001, {0x00, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(bridge.TakeCommand(sent, t0) && sent[0] == 0x09);
    game.Command({0x03, 0x0C, 0x00}, 3);
    CHECK(!bridge.TakeCommand(sent, t0));
    CHECK(bridge.TakeCommand(sent, t0 + std::chrono::milliseconds(1100)));  // a silent adapter: retried after 1 s
    CHECK(bridge.GetStats().commandTimeouts == 1);
}

void TestAclBothWays() {
    Bridge bridge(false);
    Game game(bridge);
    const std::vector<uint8_t> out{0x40, 0x20, 0x05, 0x00, 0x01, 0x00, 0x41, 0x00, 0xA2};
    game.Transfer(kIoctlvBulkMsg, kEpAclOut, out, static_cast<uint16_t>(out.size()), 0x80003000, 7);
    CHECK(game.done.size() == 1 && game.done[0].result == static_cast<int32_t>(out.size()) && game.done[0].arg == 7);
    std::vector<uint8_t> sent;
    CHECK(bridge.TakeAcl(sent) && sent == out);
    CHECK(!bridge.TakeAcl(sent));

    // Two packets in one transfer, then one split over two transfers.
    const std::vector<uint8_t> a{0x40, 0x20, 0x02, 0x00, 0xA1, 0x30};
    const std::vector<uint8_t> b{0x40, 0x20, 0x03, 0x00, 0xA1, 0x31, 0x08};
    const std::vector<uint8_t> c{0x40, 0x20, 0x04, 0x00, 0xA1, 0x30, 0x00, 0x10};
    std::vector<uint8_t> both = a;
    both.insert(both.end(), b.begin(), b.end());
    bridge.OnAdapterAclBytes(both.data(), both.size());
    bridge.OnAdapterAclBytes(c.data(), 3);
    game.done.clear();
    const uint32_t r1 = game.ReadAcl(11, 1);
    const uint32_t r2 = game.ReadAcl(12, 2);
    const uint32_t r3 = game.ReadAcl(13, 3);
    CHECK(game.done.size() == 2);  // the third waits for the rest of its packet
    bridge.OnAdapterAclBytes(c.data() + 3, c.size() - 3);
    game.Pump();
    CHECK(game.done.size() == 3);
    if (game.done.size() == 3) {
        CHECK(game.done[0].callback == 11 && game.done[0].result == 6);
        CHECK(game.done[1].callback == 12 && game.done[1].result == 7);
        CHECK(game.done[2].callback == 13 && game.done[2].result == 8);
    }
    CHECK(game.memory.Get(r1, a.size()) == a);
    CHECK(game.memory.Get(r2, b.size()) == b);
    CHECK(game.memory.Get(r3, c.size()) == c);
}

void TestEventsSplitAndOrdered() {
    Bridge bridge(false);
    Game game(bridge);
    // A 20-byte event arrives as 16 + 4 bytes (the interrupt endpoint's packet size), with the
    // next event's first bytes behind it.
    std::vector<uint8_t> big{0x2F, 18};
    for (int i = 0; i < 18; ++i) big.push_back(uint8_t(i));
    const std::vector<uint8_t> small{0x13, 0x05, 0x01, 0x40, 0x00, 0x01, 0x00};
    std::vector<uint8_t> stream = big;
    stream.insert(stream.end(), small.begin(), small.end());
    bridge.OnAdapterEventBytes(stream.data(), 16);
    bridge.OnAdapterEventBytes(stream.data() + 16, 6);
    bridge.OnAdapterEventBytes(stream.data() + 22, stream.size() - 22);
    const uint32_t r1 = game.ReadEvent(21, 0);
    const uint32_t r2 = game.ReadEvent(22, 0);
    CHECK(game.done.size() == 2 && game.done[0].callback == 21 && game.done[1].callback == 22);
    CHECK(game.memory.Get(r1, big.size()) == big);
    CHECK(game.memory.Get(r2, small.size()) == small);
}

void TestFakesForOtherAdapters() {
    Bridge bridge(false);
    Game game(bridge);
    game.Command({0x4C, 0xFC, 0x03, 0x00, 0x00, 0x00}, 1);  // Broadcom write RAM: answered here
    std::vector<uint8_t> sent;
    CHECK(!bridge.TakeCommand(sent, Clock::now()));
    const uint32_t r = game.ReadEvent(2, 0);
    CHECK(game.done.size() == 2 && game.done[1].callback == 2);
    CHECK((game.memory.Get(r, 6) == std::vector<uint8_t>{0x0E, 0x04, 0x01, 0x4C, 0xFC, 0x00}));

    // The Wii's own module gets the real thing.
    Bridge wii(true);
    Game wiiGame(wii);
    wiiGame.Command({0x4C, 0xFC, 0x00}, 1);
    CHECK(wii.TakeCommand(sent, Clock::now()) && sent[0] == 0x4C);
}

void TestReadBufferSizeRewrite() {
    Bridge bridge(false);
    Game game(bridge);
    // A CSR-like adapter: 310-byte ACL buffers x 8; then one claiming 15.
    FeedEvent(bridge, CommandComplete(kOpReadBufferSize, {0x00, 0x36, 0x01, 0x40, 0x08, 0x00, 0x00, 0x00}));
    FeedEvent(bridge, CommandComplete(kOpReadBufferSize, {0x00, 0xFD, 0x03, 0x40, 0x0F, 0x00, 0x00, 0x00}));
    const uint32_t r1 = game.ReadEvent(1, 0);
    const uint32_t r2 = game.ReadEvent(2, 0);
    const auto e1 = game.memory.Get(r1, 13);
    const auto e2 = game.memory.Get(r2, 13);
    CHECK(e1[6] == (339 & 0xFF) && e1[7] == (339 >> 8) && e1[8] == 64 && e1[9] == 8 && e1[11] == 0);
    CHECK(e2[9] == 10 && e2[10] == 0);
}

void TestLinkKeys() {
    Bridge bridge(false);
    Game game(bridge);
    // The remote pairs: the adapter reports its key.
    std::vector<uint8_t> note{kEvLinkKeyNotification, 23, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    for (int i = 0; i < 16; ++i) note.push_back(uint8_t(0xA0 + i));
    note.push_back(0x00);
    FeedEvent(bridge, note);
    CHECK(bridge.TakeLinkKeysChanged());
    CHECK(!bridge.TakeLinkKeysChanged());
    CHECK(bridge.GetLinkKeys().size() == 1);
    game.ReadEvent(1, 0);  // takes the notification
    // Next boot the game reads them back: Return Link Keys, then Command Complete.
    game.Command({0x0D, 0x0C, 0x07, 0, 0, 0, 0, 0, 0, 0x01}, 2);
    std::vector<uint8_t> sent;
    CHECK(!bridge.TakeCommand(sent, Clock::now()));
    const uint32_t r1 = game.ReadEvent(3, 0);
    const uint32_t r2 = game.ReadEvent(4, 0);
    const auto keys = game.memory.Get(r1, 25);
    CHECK(keys[0] == kEvReturnLinkKeys && keys[1] == 23 && keys[2] == 1 && keys[3] == 0x66 && keys[9] == 0xA0 &&
          keys[24] == 0xAF);
    const auto complete = game.memory.Get(r2, 10);
    CHECK(complete[0] == kEvCommandComplete && complete[3] == 0x0D && complete[5] == 0 && complete[6] == 0xFF &&
          complete[8] == 1);
    // The text file round trip.
    const LinkKeys parsed = ParseLinkKeys(FormatLinkKeys(bridge.GetLinkKeys()));
    CHECK(parsed == bridge.GetLinkKeys());
    CHECK(FormatLinkKeys(parsed).find("11:22:33:44:55:66 a0a1a2a3") != std::string::npos);
    // Forgetting them all.
    game.Command({0x12, 0x0C, 0x07, 0, 0, 0, 0, 0, 0, 0x01}, 5);
    CHECK(bridge.GetLinkKeys().empty() && bridge.TakeLinkKeysChanged());
}

void TestQosAfterConnect() {
    Bridge bridge(false);
    Game game(bridge);
    const std::vector<uint8_t> connected{kEvConnectionComplete, 11, 0x00, 0x0B, 0x00, 1, 2, 3, 4, 5, 6, 0x01, 0x00};
    FeedEvent(bridge, connected);
    std::vector<uint8_t> sent;
    CHECK(bridge.TakeCommand(sent, Clock::now()));
    CHECK(sent.size() == 23 && sent[0] == 0x07 && sent[1] == 0x08 && sent[2] == 20 && sent[3] == 0x0B && sent[6] == 0x02);
    // Its Command Status is ours: the game sees only the connection.
    FeedEvent(bridge, {kEvCommandStatus, 4, 0x00, 0x01, 0x07, 0x08});
    game.ReadEvent(1, 0);
    game.ReadEvent(2, 0);
    CHECK(game.done.size() == 1 && game.done[0].callback == 1);
    CHECK(bridge.PendingGuestReads() == 1);
}

void TestSyncButtonFirst() {
    Bridge bridge(false);
    Game game(bridge);
    FeedEvent(bridge, CommandComplete(kOpReset, {0x00}));
    bridge.PushSyncButtonEvent(false);
    const uint32_t r = game.ReadEvent(1, 0);
    CHECK((game.memory.Get(r, 3) == std::vector<uint8_t>{kEvVendor, 0x01, kSyncButtonPressed}));
}

void TestMalformedAndClose() {
    Bridge bridge(false);
    Game game(bridge);
    bridge.Ioctlv(game.memory, 9, 0, 0, game.Alloc(8), 1, 2, game.done);
    CHECK(game.done.size() == 1 && game.done[0].result == kIpcEinval);
    game.ReadEvent(3, 0);
    bridge.DropGuestReads();
    FeedEvent(bridge, CommandComplete(kOpReset, {0x00}));
    game.done.clear();
    game.Pump();
    CHECK(game.done.empty());  // a closed device's reads never complete
}

void TestSysconf() {
    const std::vector<uint8_t> sysconf = MakeDefaultSysconf();
    CHECK(sysconf.size() == 0x4000 && IsValidSysconf(sysconf));
    CHECK(std::memcmp(sysconf.data(), "SCv0", 4) == 0 && std::memcmp(&sysconf[0x3FFC], "SCed", 4) == 0);
    // The first entry: BT.DINF, a big array of 0x461 bytes (stored as 0x460).
    const size_t first = (size_t(sysconf[6]) << 8) | sysconf[7];
    CHECK(first == 6 + 2 * 7);
    CHECK(sysconf[first] == ((1 << 5) | 6) && std::memcmp(&sysconf[first + 1], "BT.DINF", 7) == 0);
    CHECK(sysconf[first + 8] == 0x04 && sysconf[first + 9] == 0x60);
    // BT.SPKV is 0x58.
    bool found = false;
    for (size_t i = 0; i < 6; ++i) {
        const size_t at = (size_t(sysconf[6 + 2 * i]) << 8) | sysconf[7 + 2 * i];
        if (std::memcmp(&sysconf[at + 1], "BT.SPKV", 7) == 0) {
            found = sysconf[at] == ((3 << 5) | 6) && sysconf[at + 8] == 0x58;
        }
    }
    CHECK(found);
    std::vector<uint8_t> broken = sysconf;
    broken[0] = 'X';
    CHECK(!IsValidSysconf(broken));
}

}  // namespace

int main() {
    TestCommandRoundTrip();
    TestCommandCredits();
    TestAclBothWays();
    TestEventsSplitAndOrdered();
    TestFakesForOtherAdapters();
    TestReadBufferSizeRewrite();
    TestLinkKeys();
    TestQosAfterConnect();
    TestSyncButtonFirst();
    TestMalformedAndClose();
    TestSysconf();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("bt_bridge_tests: all passed\n");
    return EXIT_SUCCESS;
}
