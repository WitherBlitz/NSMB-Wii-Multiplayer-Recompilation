#include "frame_input.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"
#include "hle/controller_status_contract.h"
#include "wii_remote_input.h"
#include "wpad_speaker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <vector>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace {

constexpr uint32_t kDefaultWorkMemSize = 0x20000;
constexpr uint8_t kDefaultDpdSensitivity = 3;
constexpr int32_t kStatusOk = 0;

struct WpadStubState {
    bool initSubRan = false;
    bool simpleSyncActive = false;
    uint32_t syncDeviceCallback = 0;
    uint32_t workMemSize = kDefaultWorkMemSize;
    uint8_t dpdSensitivity = kDefaultDpdSensitivity;
    WpadContract::State contract{};
};

WpadStubState g_state{};

void InvokeWpadCallback(uint32_t callback, uint32_t chan, int32_t result)
{
    if (callback == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = chan;
    cpu.gpr[4] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callback, &cpu);
}

int32_t InitializeWpadLibrary()
{
    g_state.contract.Initialize();
    return kStatusOk;
}

// The async WPAD entry points all report their outcome to the guest callback and
// then return the same value.
int32_t CompleteWpadRequest(uint32_t chan, uint32_t callback, int32_t result)
{
    InvokeWpadCallback(callback, chan, result);
    return result;
}

} // namespace

extern "C" int32_t WPADGetStatus_HLE()
{
    // RVL::WPADGetStatus takes no channel arg; it reports global WUD library state.
    // Reading r3 here would leak a caller's stale register value into the result.
    return g_state.contract.GetLibraryStatus();
}
PPC_NATIVE_OVERRIDE(801DFB30, WPADGetStatus_HLE, int32_t, (), ());

extern "C" uint32_t WPADGetDpdSensitivity_HLE()
{
    return static_cast<uint32_t>(g_state.dpdSensitivity);
}
PPC_NATIVE_OVERRIDE(801E3890, WPADGetDpdSensitivity_HLE, uint32_t, (), ());

extern "C" int32_t WPADInitSub_HLE()
{
    if (!g_state.initSubRan) {
        g_state.initSubRan = true;
        return InitializeWpadLibrary();
    }
    return kStatusOk;
}
#if 0  // NSMBW: WPADInitSub is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801BF3B4, WPADInitSub_HLE, int32_t, (), ());
#endif

extern "C" int32_t WPADInit_HLE()
{
    return InitializeWpadLibrary();
}
PPC_NATIVE_OVERRIDE(801DFA50, WPADInit_HLE, int32_t, (), ());

extern "C" int32_t WUDGetStatus_HLE()
{
    return WpadContract::kStatusReady;
}
PPC_NATIVE_OVERRIDE(801F3530, WUDGetStatus_HLE, int32_t, (), ());

// WPADGetDataFormat reads the per-channel format set by WPADSetDataFormat. Can't reuse the
// translated SDK implementation: it dereferences Bluetooth control blocks that HLE'd WPADInit
// never constructs.
extern "C" int32_t WPADGetDataFormat_HLE(uint32_t chan)
{
    return g_state.contract.GetDataFormat(chan);
}
PPC_NATIVE_OVERRIDE(801E1150, WPADGetDataFormat_HLE, int32_t, (uint32_t chan), (chan));

// WPADSetDataFormat: records the per-channel data format the game asked for.
extern "C" int32_t WPADSetDataFormat_HLE(uint32_t chan, int32_t format)
{
    return g_state.contract.SetDataFormat(chan, format);
}
PPC_NATIVE_OVERRIDE(801E11A0, WPADSetDataFormat_HLE, int32_t, (uint32_t chan, int32_t format), (chan, format));

// WPADProbe: reports the extension type of a Bluetooth remote on `chan`, or no controller.
extern "C" int32_t WPADProbe_HLE(uint32_t chan, uint32_t typePtr)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }

    // Drive the rescan state machine here too: a reconnect probe can arrive
    // before the next PADRead, and only Poll() brings a dropped remote back.
    WiiRemoteInput::Poll();

    if (FrameInput::Active()) {
        // Deterministic mode: connections change only at frame boundaries, like the input itself.
        const FrameInput::Remote& remote = FrameInput::Get(chan);
        if (typePtr != 0) {
            uint32_t type = WpadContract::kExtensionCore;
            if (remote.connected && remote.kind == WiiRemoteInput::Kind::RemoteWithNunchuk) type = 1u;
            if (remote.connected && remote.kind == WiiRemoteInput::Kind::RemoteWithClassic) type = 2u;
            Memory::Write32(typePtr, type);
        }
        return remote.connected ? kStatusOk : WpadContract::kErrorNoController;
    }

    // A real Bluetooth remote: WPAD_DEV_CORE (0) for a bare remote,
    // WPAD_DEV_FREESTYLE (1) with a Nunchuk, WPAD_DEV_CLASSIC (2) with a Classic
    // Controller. The game reads the type from here (not from
    // KPADStatus.dev_type) to pick its control scheme, and re-reads it when it
    // changes, which is what makes an extension swap mid-game work like on the
    // console. EffectiveKind keeps the last type through SDL's re-creation of
    // the joystick after a swap.
    const WiiRemoteInput::Kind kind = WiiRemoteInput::EffectiveKind(chan);
    if (WiiRemoteInput::IsRemoteChannel(chan)) {
        if (typePtr != 0) {
            uint32_t type = WpadContract::kExtensionCore;
            if (kind == WiiRemoteInput::Kind::RemoteWithNunchuk) type = 1u;
            if (kind == WiiRemoteInput::Kind::RemoteWithClassic) type = 2u;
            Memory::Write32(typePtr, type);
        }
        return kStatusOk;
    }
    if (typePtr != 0) {
        Memory::Write32(typePtr, WpadContract::kExtensionCore);
    }
    return WpadContract::kErrorNoController;
}
PPC_NATIVE_OVERRIDE(801E0F40, WPADProbe_HLE, int32_t, (uint32_t chan, uint32_t typePtr), (chan, typePtr));

extern "C" void WPADControlMotor_HLE(uint32_t chan, uint32_t command)
{
    (void)chan;
    (void)command;
}
PPC_NATIVE_OVERRIDE(801E14E0, WPADControlMotor_HLE, void, (uint32_t chan, uint32_t command), (chan, command));

extern "C" int32_t WPADGetInfoAsync_HLE(uint32_t chan, uint32_t infoPtr, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }

    (void)infoPtr;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(801E12C0, WPADGetInfoAsync_HLE, int32_t,
         (uint32_t chan, uint32_t infoPtr, uint32_t callback), (chan, infoPtr, callback));

extern "C" int32_t WPADControlLed_HLE(uint32_t chan, uint32_t ledMask, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }
    (void)ledMask;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(801E1630, WPADControlLed_HLE, int32_t,
         (uint32_t chan, uint32_t ledMask, uint32_t callback), (chan, ledMask, callback));

extern "C" int32_t WPADStartSimpleSync_HLE()
{
    if (g_state.simpleSyncActive) {
        return 0;
    }
    g_state.simpleSyncActive = true;
    return 1;
}
#if 0  // NSMBW: WPADStartSimpleSync is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801BF634, WPADStartSimpleSync_HLE, int32_t, (), ()); // WUDStartSyncSimple
#endif
#if 0  // NSMBW: WPADStartSimpleSync is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801BF638, WPADStartSimpleSync_HLE, int32_t, (), ()); // WPADStartSimpleSync (HBM)
#endif

// due to multiplayer controller screen hle this to avoid startsyncdevice to return fail every frame
// causing you to get softlocked in the game
extern "C" int32_t WPADStopSimpleSync_HLE()
{
    if (g_state.simpleSyncActive) {
        g_state.simpleSyncActive = false;
        const uint32_t callback = g_state.syncDeviceCallback;
        if (callback != 0 && TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
            NandQueueIosCallback(callback, 1, 0); // callback(WUD_SYNC_DONE, devicesSynced=0)
        }
    }
    return 1;
}
PPC_NATIVE_OVERRIDE(801DFAD0, WPADStopSimpleSync_HLE, int32_t, (), ());

// ---- The remote speaker (wpad_speaker.h) ----
//
// The translated originals talk to a Bluetooth stack that isn't there, so the game never got a
// speaker and its remote sounds were silent (issue #18). These answer as a working remote would,
// on every channel alike. A real remote on the channel gets the same reports the console sends
// (WiiBrew, "Wiimote/Speaker": 4-bit Yamaha ADPCM at 6 kHz, the rate AX mixes the remote buses
// at), through SDL's Wii driver.
namespace {

enum SpeakerCommand : uint32_t {
    kSpeakerOff = 0,
    kSpeakerOn = 1,
    kSpeakerMute = 2,
    kSpeakerUnmute = 3,
    kSpeakerPlay = 4,
};

struct SpeakerChannel {
    std::atomic<bool> on{false};
    std::atomic<bool> muted{false};
    std::atomic<bool> real{false};  // a real remote plays this channel's speaker itself
};
std::array<SpeakerChannel, WpadContract::kChannelCount> g_speakers;

void SendReport(uint32_t chan, std::initializer_list<uint8_t> bytes)
{
    const std::vector<uint8_t> report(bytes);
    WiiRemoteInput::SendRawReport(chan, report.data(), static_cast<int>(report.size()));
}

// Output report 0x16: write `data` to the remote's register space at `address`.
void WriteRegister(uint32_t chan, uint32_t address, std::initializer_list<uint8_t> data)
{
    std::array<uint8_t, 22> report{0x16, 0x04, static_cast<uint8_t>(address >> 16), static_cast<uint8_t>(address >> 8),
                                   static_cast<uint8_t>(address), static_cast<uint8_t>(data.size())};
    std::copy(data.begin(), data.end(), report.begin() + 6);
    WiiRemoteInput::SendRawReport(chan, report.data(), static_cast<int>(report.size()));
}

void SetUpRealSpeaker(uint32_t chan)
{
    SendReport(chan, {0x14, 0x04});  // speaker on
    SendReport(chan, {0x19, 0x04});  // muted while it is configured
    WriteRegister(chan, 0xA20009, {0x01});
    WriteRegister(chan, 0xA20001, {0x08});
    // 4-bit ADPCM, sample rate 6 MHz / 1000 = 6 kHz, volume 0x40.
    WriteRegister(chan, 0xA20001, {0x00, 0x00, 0xE8, 0x03, 0x40, 0x00, 0x00});
    WriteRegister(chan, 0xA20008, {0x01});
    SendReport(chan, {0x19, 0x00});  // unmuted
}

} // namespace

bool WpadSpeaker::PlaysOnHost(uint32_t chan)
{
    if (chan >= g_speakers.size()) {
        return false;
    }
    const SpeakerChannel& speaker = g_speakers[chan];
    return speaker.on.load(std::memory_order_relaxed) && !speaker.muted.load(std::memory_order_relaxed) &&
           !speaker.real.load(std::memory_order_relaxed);
}

extern "C" int32_t WPADControlSpeaker_HLE(uint32_t chan, uint32_t command, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }
    SpeakerChannel& speaker = g_speakers[chan];
    const bool real = WiiRemoteInput::HasRealRemote(chan);
    speaker.real.store(real, std::memory_order_relaxed);
    if (command == kSpeakerOn || command == kSpeakerOff) {
        RT_LOGF(RT_TAG_AUDIO, "remote %u speaker %s (%s)\n", chan + 1, command == kSpeakerOn ? "on" : "off",
                real ? "on the Wii Remote" : "on this device's speakers");
    }
    switch (command) {
    case kSpeakerOff:
        speaker.on.store(false, std::memory_order_relaxed);
        if (real) {
            SendReport(chan, {0x19, 0x04});
            SendReport(chan, {0x14, 0x00});
        }
        break;
    case kSpeakerOn:
        speaker.on.store(true, std::memory_order_relaxed);
        speaker.muted.store(false, std::memory_order_relaxed);
        if (real) {
            SetUpRealSpeaker(chan);
        }
        break;
    case kSpeakerMute:
    case kSpeakerUnmute:
        speaker.muted.store(command == kSpeakerMute, std::memory_order_relaxed);
        if (real) {
            SendReport(chan, {0x19, static_cast<uint8_t>(command == kSpeakerMute ? 0x04 : 0x00)});
        }
        break;
    case kSpeakerPlay:
        speaker.muted.store(false, std::memory_order_relaxed);
        if (real) {
            WriteRegister(chan, 0xA20008, {0x01});
        }
        break;
    default:
        break;
    }
    // Answered later, like the remote's acknowledgement on the console: callback(chan, result).
    if (callback != 0) {
        NandQueueIosCallback(callback, static_cast<int32_t>(chan), static_cast<uint32_t>(kStatusOk));
    }
    return kStatusOk;
}
PPC_NATIVE_OVERRIDE(801E1E20, WPADControlSpeaker_HLE, int32_t,
         (uint32_t chan, uint32_t command, uint32_t callback), (chan, command, callback));

extern "C" int32_t WPADIsSpeakerEnabled_HLE(uint32_t chan)
{
    return chan < g_speakers.size() && g_speakers[chan].on.load(std::memory_order_relaxed) ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801E1DD0, WPADIsSpeakerEnabled_HLE, int32_t, (uint32_t chan), (chan));

extern "C" int32_t WPADCanSendStreamData_HLE(uint32_t chan)
{
    return WPADIsSpeakerEnabled_HLE(chan);
}
PPC_NATIVE_OVERRIDE(801E35F0, WPADCanSendStreamData_HLE, int32_t, (uint32_t chan), (chan));

// One packet of the game's ADPCM stream (20 bytes, 40 samples). A real remote gets it as output
// report 0x18; anywhere else the mixer already plays the same sound from the remote's bus.
extern "C" int32_t WPADSendStreamData_HLE(uint32_t chan, uint32_t bufferPtr, uint32_t length)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }
    SpeakerChannel& speaker = g_speakers[chan];
    // A remote connected (or gone) since the game turned its speaker on: set it up, or hand its
    // sound to this device's speakers.
    const bool real = WiiRemoteInput::HasRealRemote(chan);
    if (speaker.real.exchange(real, std::memory_order_relaxed) != real && real &&
        speaker.on.load(std::memory_order_relaxed)) {
        SetUpRealSpeaker(chan);
    }
    if (speaker.on.load(std::memory_order_relaxed) && real) {
        std::array<uint8_t, 22> report{0x18};
        const uint32_t size = std::min<uint32_t>(length & 0xFFFF, 20);
        report[1] = static_cast<uint8_t>(size << 3);
        for (uint32_t i = 0; i < size; ++i) {
            report[2 + i] = Memory::Read8(bufferPtr + i);
        }
        WiiRemoteInput::SendRawReport(chan, report.data(), static_cast<int>(report.size()));
    }
    return kStatusOk;
}
PPC_NATIVE_OVERRIDE(801E3680, WPADSendStreamData_HLE, int32_t,
         (uint32_t chan, uint32_t bufferPtr, uint32_t length), (chan, bufferPtr, length));

extern "C" uint32_t WPADSetSyncDeviceCallback_HLE(uint32_t callback)
{
    const uint32_t previous = g_state.syncDeviceCallback;
    g_state.syncDeviceCallback = callback;
    return previous;
}
#if 0  // NSMBW: WPADSetSyncDeviceCallback is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801BF640, WPADSetSyncDeviceCallback_HLE, uint32_t, (uint32_t callback), (callback));
#endif
