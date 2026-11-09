#include "netplay_ui.h"
#include "settings_overlay.h"
#include "host_suspend.h"
#include "hle/gameplay_options.h"
#include "frame_input.h"
#include "hle_stubs.h"
#include "input_bindings.h"
#include "memory.h"
#include "wii_remote_input.h"
#include "bt_passthrough.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// KPAD HLE fed by a real Bluetooth Wii Remote. The game calls KPADRead once per
// frame with room for 16 KPADStatus entries and only looks at entry 0; with a
// Classic Controller it also calls KPADGetUnifiedWpadStatus for the raw
// WPADCLStatus (buttons, sticks and triggers of the extension).
namespace {

constexpr uint32_t kKpadStatusSize = 0x84;

// KPADStatus field offsets (RVL SDK).
constexpr uint32_t kHold = 0x00, kTrig = 0x04, kRelease = 0x08, kAcc = 0x0C, kAccValue = 0x18,
                   kAccSpeed = 0x1C, kPos = 0x20, kAccVertical = 0x54, kDevType = 0x5C, kWpadErr = 0x5D,
                   kDpdValidFg = 0x5E, kDataFormat = 0x5F, kFsStick = 0x60, kFsAcc = 0x68, kFsAccValue = 0x74,
                   kFsAccSpeed = 0x78;
// KPADStatus.ex_status.cl (KPADEXStatus, Classic Controller view).
constexpr uint32_t kClHold = 0x60, kClTrig = 0x64, kClRelease = 0x68, kClLStick = 0x6C, kClRStick = 0x74,
                   kClLTrigger = 0x7C, kClRTrigger = 0x80;

// KPADUnifiedWpadStatus: WPADStatus / WPADFSStatus / WPADCLStatus union, then fmt.
constexpr uint32_t kUnifiedSize = 0x38;
constexpr uint32_t kUButton = 0x00, kUAccX = 0x02, kUAccY = 0x04, kUAccZ = 0x06, kUObj = 0x08, kUDev = 0x28,
                   kUErr = 0x29, kUFsStickX = 0x2A, kUFsStickY = 0x2B, kUFsAccX = 0x2C, kUFsAccY = 0x2E,
                   kUFsAccZ = 0x30, kUClButton = 0x2A, kUClLStickX = 0x2C, kUClLStickY = 0x2E, kUClRStickX = 0x30,
                   kUClRStickY = 0x32, kUClTriggerL = 0x34, kUClTriggerR = 0x35, kUFmt = 0x36;

// WPAD device types (WPAD_DEV_*) and the data formats KPAD runs each of them
// in (WPAD_FMT_*_ACC_DPD): the values KPADStatus.dev_type / data_format and
// KPADUnifiedWpadStatus.dev / fmt carry on the console.
constexpr uint8_t kDevCore = 0;
constexpr uint8_t kDevFreestyle = 1;
constexpr uint8_t kDevClassic = 2;
constexpr uint8_t kFmtCoreAccDpd = 2;
constexpr uint8_t kFmtFreestyleAccDpd = 5;
constexpr uint8_t kFmtClassicAccDpd = 8;
constexpr int8_t kWpadErrNone = 0;
constexpr int8_t kWpadErrNoController = -1;

// Raw accelerometer as WPADStatus carries it: 10 bits, 0x200 at 0 g, 100 per g.
constexpr float kRawAccZero = 512.0f;
constexpr float kRawAccPerG = 100.0f;

struct ChannelState {
    uint32_t prevHold = 0;
    uint32_t prevClHold = 0;
    float prevAcc[3] = {0.0f, -1.0f, 0.0f};
    float prevFsAcc[3] = {0.0f, -1.0f, 0.0f};
    float accVertical[2] = {1.0f, 0.0f};  // KPADStatus.acc_vertical, level at rest
};

// KPAD runs its accelerometer filters once per report from the remote, about 200 a second, so
// one 60 Hz read stands for about three of them.
constexpr int kReportsPerRead = 3;

// KPAD's calc_acc_vertical (0x801EB030 in NSMBW): the remote's attitude in the vertical plane,
// (|acc.xy|, -acc.z) / |acc|, eased toward each report with weight kp_acc_horizon_pw (0.05) times
// w^2, where w is |acc| folded around 1 g; readings of 2 g or more (swings) are ignored. Kept at
// unit length. New Super Mario Bros. Wii reads its y as the tilt of a sideways remote.
void UpdateAccVertical(ChannelState& state, const float* acc) {
    constexpr float kHorizonPower = 0.05f;
    const float xy2 = acc[0] * acc[0] + acc[1] * acc[1];
    const float length = std::sqrt(xy2 + acc[2] * acc[2]);
    if (length == 0.0f || length >= 2.0f) {
        return;
    }
    const float targetX = std::sqrt(xy2) / length;
    const float targetY = -acc[2] / length;
    const float weight = length > 1.0f ? 2.0f - length : length;
    const float k = weight * weight * kHorizonPower;
    for (int report = 0; report < kReportsPerRead; ++report) {
        const float x = state.accVertical[0] + k * (targetX - state.accVertical[0]);
        const float y = state.accVertical[1] + k * (targetY - state.accVertical[1]);
        const float norm = std::sqrt(x * x + y * y);
        if (norm == 0.0f) {
            return;
        }
        state.accVertical[0] = x / norm;
        state.accVertical[1] = y / norm;
    }
}

std::array<ChannelState, 4> g_channels{};

// Euclidean length of a 3-vector.
float Length(const float* v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Euclidean distance between two 3-vectors.
float Distance(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Length(d);
}

// Writes three big-endian floats to guest memory.
void WriteVec3(uint32_t addr, const float* v) {
    Memory::WriteFloat32(addr, v[0]);
    Memory::WriteFloat32(addr + 4, v[1]);
    Memory::WriteFloat32(addr + 8, v[2]);
}

// Zeroes `count` consecutive floats in guest memory.
void WriteZeroFloats(uint32_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        Memory::WriteFloat32(addr + i * 4, 0.0f);
    }
}

// Fills one KPADStatus at `addr` and returns the number of valid entries (1),
// or writes an "unplugged" status and returns 0.
int32_t WriteStatus(uint32_t chan, uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    ChannelState& state = g_channels[chan];
    if (sample == nullptr) {
        state = {};
        Memory::Write32(addr + kHold, 0);
        Memory::Write32(addr + kTrig, 0);
        Memory::Write32(addr + kRelease, 0);
        Memory::Write8(addr + kDevType, kDevCore);
        Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kDpdValidFg, 0);
        return 0;
    }

    const uint32_t hold = sample->hold;
    Memory::Write32(addr + kHold, hold);
    Memory::Write32(addr + kTrig, hold & ~state.prevHold);
    Memory::Write32(addr + kRelease, state.prevHold & ~hold);
    state.prevHold = hold;

    WriteVec3(addr + kAcc, sample->acc);
    Memory::WriteFloat32(addr + kAccValue, Length(sample->acc));
    Memory::WriteFloat32(addr + kAccSpeed, Distance(sample->acc, state.prevAcc));
    for (int i = 0; i < 3; ++i) state.prevAcc[i] = sample->acc[i];

    // No IR pointer: pos .. dist_speed zeroed and dpd_valid_fg clear, which
    // the game treats as "pointing away from the screen". acc_vertical comes
    // from the accelerometer alone, as on the console.
    WriteZeroFloats(addr + kPos, (kAccVertical - kPos) / 4);
    UpdateAccVertical(state, sample->acc);
    Memory::WriteFloat32(addr + kAccVertical, state.accVertical[0]);
    Memory::WriteFloat32(addr + kAccVertical + 4, state.accVertical[1]);
    Memory::Write8(addr + kDpdValidFg, 0);

    const uint8_t devType = sample->hasClassic ? kDevClassic : sample->hasNunchuk ? kDevFreestyle : kDevCore;
    const uint8_t dataFormat =
        sample->hasClassic ? kFmtClassicAccDpd : sample->hasNunchuk ? kFmtFreestyleAccDpd : kFmtCoreAccDpd;
    Memory::Write8(addr + kDevType, devType);
    Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNone));
    Memory::Write8(addr + kDataFormat, dataFormat);

    if (sample->hasClassic) {
        const uint32_t clHold = sample->clHold;
        Memory::Write32(addr + kClHold, clHold);
        Memory::Write32(addr + kClTrig, clHold & ~state.prevClHold);
        Memory::Write32(addr + kClRelease, state.prevClHold & ~clHold);
        state.prevClHold = clHold;
        Memory::WriteFloat32(addr + kClLStick, sample->clLStick[0]);
        Memory::WriteFloat32(addr + kClLStick + 4, sample->clLStick[1]);
        Memory::WriteFloat32(addr + kClRStick, sample->clRStick[0]);
        Memory::WriteFloat32(addr + kClRStick + 4, sample->clRStick[1]);
        Memory::WriteFloat32(addr + kClLTrigger, sample->clTriggerL / 255.0f);
        Memory::WriteFloat32(addr + kClRTrigger, sample->clTriggerR / 255.0f);
    } else if (sample->hasNunchuk) {
        state.prevClHold = 0;
        Memory::WriteFloat32(addr + kFsStick, sample->stick[0]);
        Memory::WriteFloat32(addr + kFsStick + 4, sample->stick[1]);
        WriteVec3(addr + kFsAcc, sample->nunchukAcc);
        Memory::WriteFloat32(addr + kFsAccValue, Length(sample->nunchukAcc));
        Memory::WriteFloat32(addr + kFsAccSpeed, Distance(sample->nunchukAcc, state.prevFsAcc));
        for (int i = 0; i < 3; ++i) state.prevFsAcc[i] = sample->nunchukAcc[i];
    } else {
        state.prevClHold = 0;
        WriteZeroFloats(addr + kFsStick, (kKpadStatusSize - kFsStick) / 4);
    }
    return 1;
}

// One accelerometer axis of KPAD's g vector back to the 10-bit raw WPAD value.
uint16_t RawAcc(float g) {
    const float raw = kRawAccZero + g * kRawAccPerG;
    return static_cast<uint16_t>(std::clamp(raw, 0.0f, 1023.0f));
}

// Fills one KPADUnifiedWpadStatus at `addr` from the sample: the WPADStatus core
// (remote buttons, raw accelerometer, no IR objects), then the Nunchuk or Classic
// Controller tail, then the data format.
void WriteUnifiedStatus(uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    for (uint32_t offset = 0; offset < kUnifiedSize; offset += 4) {
        Memory::Write32(addr + offset, 0);
    }
    if (sample == nullptr) {
        Memory::Write8(addr + kUDev, kDevCore);
        Memory::Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kUFmt, kFmtCoreAccDpd);
        return;
    }
    Memory::Write16(addr + kUButton, static_cast<uint16_t>(sample->hold & 0xFFFF));
    // KPAD's acc is (-wiiX, -wiiZ, wiiY); WPADStatus keeps the remote's own axes.
    Memory::Write16(addr + kUAccX, RawAcc(-sample->acc[0]));
    Memory::Write16(addr + kUAccY, RawAcc(sample->acc[2]));
    Memory::Write16(addr + kUAccZ, RawAcc(-sample->acc[1]));
    // No IR: every DPDObject invalid (x/y at the sensor's out-of-range value).
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::Write16(addr + kUObj + i * 8, 0x3FF);
        Memory::Write16(addr + kUObj + i * 8 + 2, 0x3FF);
    }
    Memory::Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNone));
    if (sample->hasClassic) {
        Memory::Write8(addr + kUDev, kDevClassic);
        Memory::Write16(addr + kUClButton, static_cast<uint16_t>(sample->clHold & 0xFFFF));
        Memory::Write16(addr + kUClLStickX, static_cast<uint16_t>(sample->clLStickRaw[0]));
        Memory::Write16(addr + kUClLStickY, static_cast<uint16_t>(sample->clLStickRaw[1]));
        Memory::Write16(addr + kUClRStickX, static_cast<uint16_t>(sample->clRStickRaw[0]));
        Memory::Write16(addr + kUClRStickY, static_cast<uint16_t>(sample->clRStickRaw[1]));
        Memory::Write8(addr + kUClTriggerL, sample->clTriggerL);
        Memory::Write8(addr + kUClTriggerR, sample->clTriggerR);
        Memory::Write8(addr + kUFmt, kFmtClassicAccDpd);
    } else if (sample->hasNunchuk) {
        Memory::Write8(addr + kUDev, kDevFreestyle);
        // WPADFSStatus: 8-bit stick (centre 128) and 10-bit Nunchuk accelerometer.
        Memory::Write8(addr + kUFsStickX,
                       static_cast<uint8_t>(std::clamp(128.0f + sample->stick[0] * 100.0f, 0.0f, 255.0f)));
        Memory::Write8(addr + kUFsStickY,
                       static_cast<uint8_t>(std::clamp(128.0f + sample->stick[1] * 100.0f, 0.0f, 255.0f)));
        Memory::Write16(addr + kUFsAccX, RawAcc(-sample->nunchukAcc[0]));
        Memory::Write16(addr + kUFsAccY, RawAcc(sample->nunchukAcc[2]));
        Memory::Write16(addr + kUFsAccZ, RawAcc(-sample->nunchukAcc[1]));
        Memory::Write8(addr + kUFmt, kFmtFreestyleAccDpd);
    } else {
        Memory::Write8(addr + kUDev, kDevCore);
        Memory::Write8(addr + kUFmt, kFmtCoreAccDpd);
    }
}

// Neutral sticks and no buttons while an overlay owns input; the remote stays connected.
bool ReadSample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (FrameInput::Active()) {
        // Deterministic mode: the frame's latched state (already neutral while an overlay owns input).
        const FrameInput::Remote& remote = FrameInput::Get(chan);
        if (!remote.connected) {
            return false;
        }
        sample = remote.sample;
        return true;
    }
    if (!WiiRemoteInput::ReadKpadSample(chan, sample)) {
        return false;
    }
    NetplayUi::FilterGameInput(chan, sample);
    if (InputBindings::InputBlocked()) {
        sample.hold = 0;
        sample.clHold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clLStick[0] = sample.clLStick[1] = 0.0f;
        sample.clRStick[0] = sample.clRStick[1] = 0.0f;
        sample.clLStickRaw[0] = sample.clLStickRaw[1] = 0;
        sample.clRStickRaw[0] = sample.clRStickRaw[1] = 0;
        sample.clTriggerL = sample.clTriggerR = 0;
    }
    return true;
}

// The runtime's per-frame work on the game's own thread, run from the first remote's read.
void OnGameThread() {
    settings_overlay::FlushGameSound();  // a Settings button's sound, from the game's own thread
    HostSuspend::OnGameThread();         // a backup save before Android's pause, likewise
    GameplayOptions::OnGameThread();     // the Gameplay tab's Power-Up, likewise
}

// Bluetooth passthrough: the game's own KPAD has read the real remote into `count` KPADStatus
// entries (newest first). The runtime's game-styled menus still see and steer its buttons, as
// ReadSample lets them with the HLE: FilterGameInput may add presses (a tap on the screen) or take
// them away (while one of those menus is open), and an overlay that owns input blocks them all.
// When neither changes anything, which is all of normal play, the entries are left untouched.
uint32_t g_passthroughAdded[4] = {};
void FilterPassthroughStatus(uint32_t chan, uint32_t statusPtr, int32_t count) {
    if (chan >= 4 || statusPtr == 0 || count <= 0) {
        return;
    }
    try {
        const uint32_t raw = Memory::Read32(statusPtr + kHold);
        const uint8_t devType = Memory::Read8(statusPtr + kDevType);
        WiiRemoteInput::KpadSample sample;
        sample.hold = raw;
        sample.hasNunchuk = devType == kDevFreestyle;
        sample.hasClassic = devType == kDevClassic;
        NetplayUi::FilterGameInput(chan, sample);
        const bool blocked = InputBindings::InputBlocked();
        const uint32_t filtered = blocked ? 0 : sample.hold;
        const uint32_t removed = raw & ~filtered;
        const uint32_t added = filtered & ~raw;
        uint32_t& previouslyAdded = g_passthroughAdded[chan];
        if (removed == 0 && added == 0 && previouslyAdded == 0 && !blocked) {
            return;
        }
        for (int32_t i = 0; i < std::min<int32_t>(count, 16); ++i) {
            const uint32_t entry = statusPtr + static_cast<uint32_t>(i) * kKpadStatusSize;
            uint32_t hold = (Memory::Read32(entry + kHold) & ~removed) | added;
            uint32_t trig = Memory::Read32(entry + kTrig) & ~removed;
            uint32_t release = Memory::Read32(entry + kRelease) & ~removed;
            if (i == 0) {
                trig |= added & ~previouslyAdded;
                release |= previouslyAdded & ~filtered;
            }
            Memory::Write32(entry + kHold, hold);
            Memory::Write32(entry + kTrig, trig);
            Memory::Write32(entry + kRelease, release);
            if (blocked) {
                const uint8_t type = Memory::Read8(entry + kDevType);
                if (type == kDevFreestyle) {
                    Memory::WriteFloat32(entry + kFsStick, 0.0f);
                    Memory::WriteFloat32(entry + kFsStick + 4, 0.0f);
                } else if (type == kDevClassic) {
                    Memory::Write32(entry + kClHold, 0);
                    Memory::Write32(entry + kClTrig, 0);
                    Memory::Write32(entry + kClRelease, 0);
                    WriteZeroFloats(entry + kClLStick, 4);
                    WriteZeroFloats(entry + kClLTrigger, 2);
                }
            }
        }
        previouslyAdded = added;
    } catch (const Memory::AccessViolation&) {
    }
}

} // namespace

// KPADRead: fills KPADStatus[0] for `chan` from the Bluetooth remote, returns the entry count.
extern "C" int32_t KPAD__Read_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        return WriteStatus(chan, statusPtr, have ? &sample : nullptr);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
}
// With Bluetooth passthrough on, the game's own KPADRead (kept by REGISTER_NATIVE_FUNCTION_AS; see
// wpad.cpp) reads the real remote. The call stays written out as func_xxx(ctx) so the translator
// syncs the whole register file around this native.
extern "C" void func_801ED3A0(CpuContext* ctx);  // the translated KPADRead
extern "C" void KPADRead_Native_801ed3a0(CpuContext* ctx)
{
    const uint32_t chan = ctx->gpr[3];
    const uint32_t statusPtr = ctx->gpr[4];
    if (BtPassthrough::Active()) {
        func_801ED3A0(ctx);
        FilterPassthroughStatus(chan, statusPtr, static_cast<int32_t>(ctx->gpr[3]));
        return;
    }
    ctx->gpr[3] = static_cast<uint32_t>(KPAD__Read_HLE(chan, statusPtr, ctx->gpr[5]));
}
REGISTER_NATIVE_FUNCTION_AS(0x801ED3A0, KPADRead_Native_801ed3a0, "KPADRead_Native_801ed3a0");

// KPADReadEx: KPADRead plus an optional s32* error out-parameter. In NSMBW both are thin wrappers
// around KPADiRead, and the main input path (EGG::CoreControllerMgr, call at 0x802BCE0C) uses this
// one, so it needs the same native or the remote would read as disconnected.
extern "C" int32_t KPAD__ReadEx_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count, uint32_t errPtr)
{
    constexpr int32_t kKpadReadErrNone = 0;
    constexpr int32_t kKpadReadErrNoController = -2;
    if (chan == 0) {
        OnGameThread();
    }
    const int32_t read = KPAD__Read_HLE(chan, statusPtr, count);
    if (errPtr != 0) {
        try {
            Memory::Write32(errPtr, static_cast<uint32_t>(read > 0 ? kKpadReadErrNone : kKpadReadErrNoController));
        } catch (const Memory::AccessViolation&) {
        }
    }
    return read;
}
extern "C" void func_801ED3B0(CpuContext* ctx);  // the translated KPADReadEx
extern "C" void KPADReadEx_Native_801ed3b0(CpuContext* ctx)
{
    const uint32_t chan = ctx->gpr[3];
    const uint32_t statusPtr = ctx->gpr[4];
    if (BtPassthrough::Active()) {
        if (chan == 0) {
            OnGameThread();
        }
        func_801ED3B0(ctx);
        FilterPassthroughStatus(chan, statusPtr, static_cast<int32_t>(ctx->gpr[3]));
        return;
    }
    ctx->gpr[3] = static_cast<uint32_t>(KPAD__ReadEx_HLE(chan, statusPtr, ctx->gpr[5], ctx->gpr[6]));
}
REGISTER_NATIVE_FUNCTION_AS(0x801ED3B0, KPADReadEx_Native_801ed3b0, "KPADReadEx_Native_801ed3b0");

// KPADGetUnifiedWpadStatus: the raw WPAD status behind KPADStatus. The game
// reads the Classic Controller's buttons, sticks and triggers from here. The
// SDK fills `count` entries with the channel's recent samples (the game asks for
// as many as it asked KPADRead for and looks at entry 0); with one sample per
// frame here, every entry gets the current one.
extern "C" int32_t KPAD__GetUnifiedWpadStatus_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    constexpr uint32_t kMaxEntries = 16; // KPAD_MAX_READ_BUFS
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        const uint32_t entries = std::min(count, kMaxEntries);
        for (uint32_t i = 0; i < entries; ++i) {
            WriteUnifiedStatus(statusPtr + i * kUnifiedSize, have ? &sample : nullptr);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
    return have ? 1 : 0;
}
#if 0  // NSMBW: KPADGetUnifiedWpadStatus is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(8019812C, KPAD__GetUnifiedWpadStatus_HLE, int32_t,
         (uint32_t chan, uint32_t statusPtr, uint32_t count), (chan, statusPtr, count));
#endif
