#pragma once

// Bluetooth passthrough (Windows and Linux): with a USB Bluetooth adapter this runtime can open and
// claim, the game runs its own Wii Remote stack (WPAD, KPAD, WUD and the Broadcom BTE in main.dol)
// against it, as on a Wii, instead of the WPAD/KPAD HLE fed by SDL. Real Wii Remotes paired to that
// adapter then work completely, speaker included. [input] bluetooth_passthrough picks "auto" (the
// default: on when such an adapter is found), "on" or "off". Never on during LAN play, in
// deterministic mode or with an input script, and compiled out on Android and macOS.
//
// The decision is made once per run, before the game starts its controllers; changing the
// setting takes effect on the next start. Without an adapter nothing here changes the game.
//
// What switches over when it is on: the WPAD/KPAD natives (hle/input) call the translated
// originals, IOS gets the /dev/usb/oh1/57e/305 device (bt_hci_bridge.h), and SC reads a SYSCONF
// of its own (NAND/bluetooth-passthrough/SYSCONF in the user data folder) holding the paired remotes.

#include <cstdint>
#include <filesystem>
#include <string>

namespace BtPassthrough {

enum class Mode { Auto, On, Off };

struct Status {
    bool available = false;  // compiled into this build
    Mode mode = Mode::Auto;  // the setting
    bool decided = false;
    bool active = false;     // in use this run
    std::string adapter;     // "CSR8510 A10 (0a12:0001)", when one was found
    std::string detail;      // why it is off, or what it is doing
};

// Reads the setting and, unless it can't be on, looks for an adapter on a worker thread.
// Called once by main before the game starts.
void StartProbe();

// True when passthrough is in use this run. The first call waits for the probe (a few hundred
// milliseconds at most); afterwards it is one atomic load.
bool Active();

Status GetStatus();

// Called by the WPADInit native just before the game's own WPADInit runs (passthrough on): what that
// stack needs from the "hardware" besides IOS, i.e. the sensor bar's GPIO register (it switches the
// bar's power in __wpadInitSub), which becomes a plain memory word (GuestFlat::BackRegisterPage).
void BeforeGameWpadInit();

// ---- The IOS device (hle/ios.cpp, hle/storage/nand_isfs.cpp) ----
// IOS_Open of `path`: the device's descriptor, or 0 when the path isn't the Bluetooth module or
// passthrough is off (the caller then answers as before).
int32_t IosOpen(const char* path);
bool IsFd(uint32_t fd);
int32_t IosClose(uint32_t fd);
// IOS_IoctlvAsync: always accepted; the callback runs from the alarm pump.
void IosIoctlvAsync(uint32_t fd, uint32_t command, uint32_t numIn, uint32_t numOut, uint32_t vectors,
                    uint32_t callback, uint32_t callbackArg);
// The synchronous IOS_Ioctlv, which the game's stack never uses on this device: an error.
int32_t IosIoctlv(uint32_t fd, uint32_t command, uint32_t numIn, uint32_t numOut, uint32_t vectors);
// The alarm pump (hle/os/os_alarm.cpp): hands what the adapter sent to the game's waiting reads.
// True when it queued guest callbacks. Never waits for the probe.
bool ProcessCompletions();

// ---- SYSCONF (hle/storage/nand_fs.cpp) ----
// With passthrough on, the host file behind the guest's /shared2/sys/SYSCONF:
// <user data>/NAND/bluetooth-passthrough/SYSCONF, shared by every game, created with no remote
// paired if missing. False when off, and the guest path resolves as before.
bool SysconfHostPath(std::filesystem::path& out);

// ---- The console's red sync button ----
// The default key, F9: press to pair remotes, hold for 10 seconds to forget every paired remote.
// A Settings button can call PressSyncButton() / ForgetPairings() directly.
void SetSyncButtonDown(bool down);
void PressSyncButton();
void ForgetPairings();

// A short message for the screen (pairing, forgetting, the adapter found at boot), or empty.
std::string Notice();

// Releases the adapter (and gives Linux's driver back). Registered with atexit by StartProbe.
void Shutdown();

}  // namespace BtPassthrough
