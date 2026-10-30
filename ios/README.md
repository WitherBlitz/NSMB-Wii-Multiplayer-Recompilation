# iOS

The iOS port: the same runtime as the Android, Windows and Linux builds, built for arm64 iPhones
and iPads with Xcode's iPhoneOS SDK. It draws with Metal (Dawn's `dawn-ios-arm64` package) and has
the Android app's on-screen sideways Wii Remote, tilt and shake, menu taps and pause-on-leave.

Like every other build, you need your own **New Super Mario Bros. Wii (USA, `SMNE01`, revision 1)**,
extracted with Dolphin's **Extract Entire Disc**. Building it compiles the game's code, translated
from that disc, into the app, so the app you build is for your own use.

## Requirements

- A Mac with Xcode (with the iOS SDK), CMake, Ninja and Python 3: `brew install cmake ninja`.
- The .NET 8 SDK for the translator: `brew install --cask dotnet-sdk@8` (or https://dot.net).
- An iPhone or iPad on iOS 16 or newer (A11 chip or later).
- An Apple Developer account to sign with. The app asks for two capabilities, which need a paid
  account: **Extended Virtual Addressing** (the Wii's 4 GiB address space) and **Increased Memory
  Limit**. Enable both for your bundle ID in the developer portal and make a development
  provisioning profile for it.

## Build

```sh
# First time: unpack the game's code from your extracted disc, translate it, build the app.
ios/build-ios.sh --game ~/Desktop/NSMBW-extracted

# Signed, and installed on the phone connected to the Mac.
MKW_IOS_BUNDLE_ID=com.yourname.nsmbw \
MKW_IOS_SIGN_IDENTITY="Apple Development: you@example.com (TEAMID)" \
MKW_IOS_PROFILE=~/Downloads/NSMBW_Development.mobileprovision \
ios/build-ios.sh --skip-translate --install
```

The app is `ios-build/NSMBW.app`, also packed as `ios-build/NSMBW.ipa`. Without the signing variables
the `.ipa` is unsigned, for a re-signing tool such as AltStore or Sideloadly; that tool's
certificate must still carry the two capabilities above, or the game fails at start with "unable to
reserve the 4 GiB iOS guest address space". Logs of each step go to `ios-build/logs`.

## Getting the game onto the phone

The app shows these steps until it has the game:

1. Copy the folder Dolphin extracted (with `files` and `sys` in it) to the phone: AirDrop, iCloud
   Drive, or Finder with the phone connected.
2. In the app, tap **Choose Folder…** and pick it. The app checks it is `SMNE01` revision 1, copies
   it in, and you can delete your copy.

Or drop the folder into the app's own folder (Files > On My iPhone > New Super Mario Bros. Wii, or the phone's Files tab
in Finder), name it `game`, and tap **Check Again**.

That folder also holds the saves (`NAND`, or `Saves/<game>/NAND`), `Config.toml`, the logs (`Logs`) and
`Netplay/hosts.txt`: the Tailscale or LAN devices to look for rooms on, one name or address per
line (the Android app's **Tailscale & LAN Devices** list).

## Playing

- **On-screen remote:** the Android layout. The cross-pad under the left thumb, 1 and 2 under the
  right (rest a thumb on their seam to hold both), A, Shake, + and -. It hides on the game's menus,
  which you tap instead.
- **Tilt and shake:** turn the phone like a steering wheel to tilt; jolt it to shake.
- **≡ (top right):** the aspect ratio (Fill, 16:9 or 4:3; 4:3 versus widescreen applies at the
  next start), the on-screen remote, its opacity, vibration, tilt and shake, and leaving a LAN game.
- **Game controllers** (MFi, Xbox, PlayStation) work as a sideways Wii Remote, as on Windows.
- **Video, audio and language settings** are the game's own: the gear on the save file screen, or
  **Settings** in the pause and world map menus.
- **Leaving the app** writes the game's Quick Save as a backup and pauses; coming back carries on
  and deletes the backup, as on Android.

## How the port works

| Piece | Where |
| --- | --- |
| Guest memory: the 4 GiB space where the kernel finds room, backed by Mach memory entries (iOS has no fixed free address and no `/tmp` backing files) | `runtime/src/guest_flat_memory_ios.cpp`, `runtime/include/guest_flat_memory.h` |
| App shell: game import, on-screen remote, motion, menu, lifecycle | `runtime/src/platform/ios/ios_app.mm`, `runtime/include/ios_app.h` |
| `main()` hands the main thread to UIKit through `SDL_RunApp`; the game runs on it | `runtime/src/main.cpp` |
| Paused in the background, the main run loop keeps turning so the return to the app can arrive | `runtime/src/host_suspend.cpp` |
| Saves, settings and logs in `Documents` | `runtime/src/platform/host_platform.cpp` |
| Bundle, frameworks, `-mcpu=apple-a11` | `runtime/CMakeLists.txt`, `runtime/cmake/PublicProducts.cmake`, `ios/Info.plist.in` |
| Fibers: the Apple Silicon assembly backend, unchanged | `runtime/src/platform/macos/co_switch.S` |

`runtime/tests/ios_guest_flat_memory_tests.cpp` runs the iOS memory backend on a Mac
(`MKW_TEST_DYNAMIC_GUEST_BASE`), as part of the macOS build's `ctest`.

## Known limits

- **LAN rooms on the same Wi-Fi may not show up by themselves.** iOS only lets apps holding Apple's
  multicast networking entitlement (granted on request) send broadcasts. Add the other devices to
  `Netplay/hosts.txt` (or use Tailscale) instead.
- **Joining a LAN room needs the app opened again.** The game starts a fresh process for a session;
  iOS can't relaunch an app, so it asks you to open it again.
- **Early testing.** Tested on an iPhone 16 (A18, iOS 27): it boots, reaches the title screen
  and plays a level. The title screen runs at about 30 fps there (frames wait on presentation,
  not on the game), which still needs looking into.
- **Tailscale play works across platforms.** An iPhone played with a Windows PC, and a Mac (macOS
  arm64 build) with two Windows PCs. Rooms appear once each device lists the others in
  `Netplay/hosts.txt`. Couch play with controllers hasn't been tried on iOS yet.
- **No app icon is included.** Put a square PNG at `ios/AppIcon.png` and `ios/build-ios.sh` adds it.
