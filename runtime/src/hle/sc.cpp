#include "hle_stubs.h"

#include "bt_passthrough.h"
#include "guest_interrupt_context.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "runtime_config.h"
#include "aurora_events.h"
#include "runtime_log.h"

namespace {

// Use the SDK's own value tables, including its unknown-region result.
uint32_t LookupProductRegion(uint32_t table, uint32_t stride, uint32_t count,
                             const std::string& value) {
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = table + index * stride;
        if (!Memory::Contains(entry, stride)) {
            break;
        }
        const auto* bytes = static_cast<const uint8_t*>(Memory::GetPointer(entry, stride));
        if (bytes[0] == 0xFF) {
            break;
        }
        if (value.size() < stride - 1 &&
            std::memcmp(bytes + 1, value.c_str(), value.size() + 1) == 0) {
            return bytes[0];
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// The step of the real SCCheckStatus that the answer above leaves out: once SCInit's background load
// has read the files (BgJobStatus 3), parse the SYSCONF it read (ParseConfBuf), keep it as the
// working copy, and go idle; a SYSCONF that doesn't parse becomes an empty one, as on the console.
// Normal play has no SYSCONF, so it never needs this. Bluetooth passthrough has one (the paired
// remotes, BT.SPKV...), which the game's own WPAD reads through SC. Returns the status (0 ok,
// 1 still loading, 2 error, or the parse's error).
uint32_t SC_FinishBackgroundLoad()
{
    constexpr uint32_t kBgJobStatus = 0x8042AA50u;  // r13-0x4C30
    constexpr uint32_t kDirtyFlag = 0x8042AA65u;    // r13-0x4C1B
    constexpr uint32_t kControl = 0x80396140u;      // the SC control block
    constexpr uint32_t kConfBuf = 0x803962E0u;      // the working SYSCONF
    constexpr uint32_t kConfSize = 0x4000u;
    constexpr uint32_t kParseConfBuf = 0x801DBEB0u;
    const uint8_t status = Memory::Read8(kBgJobStatus);
    if (status != 3) {
        return status;
    }
    Memory::Write8(kBgJobStatus, 1);
    const uint32_t buffer = Memory::Read32(kControl + 0x16Cu);
    const uint32_t length = Memory::Read32(kControl + 0x17Cu);
    GuestInterruptCallbackContext call;
    call.get()->gpr[3] = buffer;
    call.get()->gpr[4] = length;
    InvokeIndirectCpu(kParseConfBuf, call.get());
    const uint32_t result = call.get()->gpr[3];
    if (result == 0) {
        if (buffer != kConfBuf && Memory::Contains(buffer, kConfSize)) {
            std::memmove(Memory::GetPointer(kConfBuf, kConfSize), Memory::GetPointer(buffer, kConfSize), kConfSize);
        }
    } else if (Memory::Contains(buffer, kConfSize)) {
        uint8_t* conf = Memory::GetPointer(buffer, kConfSize);
        std::memset(conf, 0, kConfSize);
        std::memcpy(conf, "SCv0", 4);
        std::memcpy(conf + kConfSize - 4, "SCed", 4);
        Memory::Write16(buffer + 6, 8);  // no items: the end offset right after the header
    }
    Memory::Write8(kDirtyFlag, 0);
    Memory::Write8(kBgJobStatus, 0);
    RT_LOGF(RT_TAG_HLE, "SC: parsed the loaded SYSCONF (%s)\n", result == 0 ? "ok" : "invalid, emptied");
    return result;
}

// 0x801DB8E0 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    if (BtPassthrough::Active()) {
        // Still loading answers "done" as below, so OSInit's loop can't hang; the parse then happens
        // at a later call (the WPADInit native makes one).
        const uint32_t status = SC_FinishBackgroundLoad();
        return status == 1 ? 0 : status;
    }
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}

PPC_NATIVE_OVERRIDE(801DB8E0, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801DD1D0 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9. The display settings' aspect at boot: 16:9 and Fill both run the
// game in widescreen (Fill then widens its canvas further).
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return DisplaySettings::GameWidescreen() ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801DD1D0, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801DD2B0 -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}

PPC_NATIVE_OVERRIDE(801DD2B0, SCGetEuRgb60Mode_HLE, uint32_t, (), ());

// 0x801DD320 -> SCGetLanguage()
// The console language (SC_LANG_*: 1 English, 3 French, 4 Spanish; the USA disc has EngUS, FraUS
// and SpaUS). NSMBW picks its message, layout and Home Button files by it. From the Language setting
// ([game] language), read once: a change takes effect the next time the game starts, never halfway.
// Italian isn't on the USA disc: the game runs in English, and its language pack replaces the English
// text (riivolution.cpp).
extern "C" uint32_t SCGetLanguage_HLE()
{
    static const uint32_t language = [] {
        const std::string chosen = RuntimeConfigFile::Language();
        const uint32_t code = chosen == "es" ? 4u : chosen == "fr" ? 3u : 1u;
        RT_LOGF(RT_TAG_HLE, "SCGetLanguage -> %u (%s)\n", code, chosen.c_str());
        return code;
    }();
    return language;
}

PPC_NATIVE_OVERRIDE(801DD320, SCGetLanguage_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    return LookupProductRegion(0x80343A08u, 5, 13,  // SC area table {code, "JPN"...} (NSMBW)
                               RuntimeConsoleIdentity::Current().area);
}

PPC_NATIVE_OVERRIDE(801DD860, SCGetProductArea_HLE, uint32_t, (), ());

#if 0  // NSMBW: SCGetProductCode is not linked into NSMBW; this wrote MKW's SC CODE buffer
extern "C" uint32_t SCGetProductCode_HLE()
{
    // Original PAL SC storage for the six-byte CODE value.
    constexpr uint32_t kProductCodeAddress = 0x803869E0u;
    const std::string& productCode = RuntimeConsoleIdentity::Current().productCode;
    const size_t size = productCode.size() + 1;
    if (!Memory::Contains(kProductCodeAddress, size)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(kProductCodeAddress, size),
                productCode.c_str(), size);
    return kProductCodeAddress;
}
#endif

#if 0  // NSMBW: SCGetProductCode is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801B2424, SCGetProductCode_HLE, uint32_t, (), ());
#endif

extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    return RuntimeScSerial::Write(serial, serialAddress,
        [](uint32_t address, size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
}

#if 0  // NSMBW: SCGetProductSN is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801B2460, SCGetProductSN_HLE, uint32_t, (uint32_t serialAddress), (serialAddress));
#endif

extern "C" uint32_t SCGetProductGameRegion_HLE()
{
    return LookupProductRegion(0x80343A50u, 4, 4,  // SC game-region table {code, "JP"...} (NSMBW)
                               RuntimeConsoleIdentity::Current().gameRegion);
}

PPC_NATIVE_OVERRIDE(801DD940, SCGetProductGameRegion_HLE, uint32_t, (), ());

// These stubs make the game think all titles are installed; otherwise it checks title ID
// 0x00010004524d4350 ("RMCP", Mario Kart Wii PAL) and reports error code 5.

// 0x801AE4A0 -> OS__IsTitleInstalled(titleIdHi, titleIdLo)
// Returns: 1 = installed, 0 = not installed
extern "C" uint32_t OS__IsTitleInstalled(uint32_t titleIdHi, uint32_t titleIdLo)
{
    RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (stubbed as installed)\n",
            titleIdHi, titleIdLo);
    return 1; // Always report installed
}

#if 0  // NSMBW: OSIsTitleInstalled is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801AE4A0, OS__IsTitleInstalled, uint32_t, (uint32_t titleIdHi, uint32_t titleIdLo), (titleIdHi, titleIdLo));
#endif

// 0x801AD1D4 -> OS__CheckInstall(requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr): returns 0 with
// outFlagsPtr = 0x3 (bit0 has data, bit1 has update; bit2 would be needs-blocks) i.e. fully installed.
extern "C" uint32_t OS__CheckInstall(uint32_t requiredBlocks, uint32_t titleIdHi, 
                                      uint32_t titleIdLo, uint32_t outFlagsPtr)
{
    RT_LOGF(RT_TAG_HLE, "OS__CheckInstall(blocks=%u, 0x%08X%08X) -> success (stubbed)\n",
            requiredBlocks, titleIdHi, titleIdLo);
    if (outFlagsPtr != 0) {
        Memory::Write32(outFlagsPtr, 0x3); // has data + has update = fully installed
    }
    return 0; // Success
}

#if 0  // NSMBW: OSCheckInstall is not linked into NSMBW (was MKW-only)
PPC_NATIVE_OVERRIDE(801AD1D4, OS__CheckInstall, uint32_t, (uint32_t requiredBlocks, uint32_t titleIdHi, uint32_t titleIdLo, uint32_t outFlagsPtr), (requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr));
#endif
