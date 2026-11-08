**This is irrelevant to the recompilation, but I'm currently trying to find testers that're willing to test my new Android app out, it's multi-use and turns your phone into a PC once connected to a Monitor, or an Android TV when connected to a TV. If you're willing to test it, friend me at "WitherBlltz" on Discord. This is a Google Play Store Exclusive and doesn't work on IOS (yet)! you may need to pay 5 cents on the Play Store to download the app as a heads up! That was the minimum Google would let me put it as.**

# NSMB Wii Multiplayer Recompilation

<p align="center">
  <a href="https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases/latest"><img alt="Android 9+, arm64" src="https://img.shields.io/badge/Android-9%2B%20%C2%B7%20arm64-3DDC84?logo=android&amp;logoColor=white"></a>
  <a href="https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases/latest"><img alt="Windows 10 / 11, x64" src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078D4"></a>
  <a href="https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases/latest"><img alt="Linux, x86-64" src="https://img.shields.io/badge/Linux-x86--64-FCC624?logo=linux&amp;logoColor=black"></a>
</p>
<p align="center">
  <a href="#building-from-source"><img alt="PowerPC static recompilation" src="https://img.shields.io/badge/PowerPC-static%20recompilation-FF9F0A"></a>
  <a href="https://github.com/patchzyy/Wiicompiled"><img alt="Built on WiiCompiled" src="https://img.shields.io/badge/built%20on-WiiCompiled-8B5CF6"></a>
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-2EA44F?logo=gnu&amp;logoColor=white"></a>
</p>

A native port of New Super Mario Bros. Wii for Android phones and Windows and Linux PCs, made with
static recompilation. Newer Super Mario Bros. Wii runs on it too.

There's no emulator in the loop, no interpreter, no JIT, no PowerPC anywhere at runtime.

**Port made by [WitherBlitz](https://github.com/WitherBlitz) with the help of
[WiiCompiled](https://github.com/patchzyy/Wiicompiled)'s code, and was entirely coded by
Claude Opus 5.5.**

> [!IMPORTANT]
> There are no game files in this project or its releases: no disc image, no levels, music,
> graphics or sounds. You need your own copy of **New Super Mario Bros. Wii (USA, `SMNE01`,
> revision 1)** and [Dolphin](https://dolphin-emu.org/) on a computer to extract it. The downloads do
> contain the game's code, translated to native code, the way KartPad's do. See
> [What the downloads contain](#what-the-downloads-contain).

**[I just want to play](#quick-start)**

<p align="center">
  <img src="docs/screenshots/android-title.webp" alt="New Super Mario Bros. Wii running natively on an Android phone, with the on-screen Wii Remote controls" width="800">
  <br><sub>Running natively on Android at 60 fps, with the on-screen sideways Wii Remote.</sub>
</p>

---

## Contents

- [Features](#features)
- [Tested devices](#tested-devices)
- [Requirements](#requirements)
- [Getting the game files](#getting-the-game-files)
- [Quick start](#quick-start)
- [Installing](#installing)
- [Controls](#controls)
- [LAN and Tailscale multiplayer](#lan-and-tailscale-multiplayer)
- [Real Wii Remotes over Bluetooth (experimental)](#real-wii-remotes-over-bluetooth-experimental)
- [Building from source](#building-from-source)
- [What the downloads contain](#what-the-downloads-contain)
- [FAQ](#faq)
- [AI usage](#ai-usage)
- [Credits and thanks](#credits-and-thanks)
- [License](#license)

---

## Features

Everything the port does, grouped. Items marked *untested* or *experimental* are listed again under
[Not tested yet](#not-tested-yet).

### 📦 Platforms and downloads

- **Android** phones and tablets: arm64, Android 9 or newer. Vulkan, or OpenGL ES 3.1 where the
  phone's Vulkan driver is too old. Runs on older ARMv8 CPUs too.
- **Windows** 10 and 11, 64-bit: Direct3D 12, Vulkan or Direct3D 11.
- **Linux** x86-64: Vulkan, glibc 2.35 or newer, X11 and Wayland.
- **Older PC processors** (no AVX2, older than Intel Haswell or AMD Excavator): the download's
  `compat` programs start by themselves. Same game, math done the long way, still plays LAN games with
  everyone else.
- **Newer Super Mario Bros. Wii** comes in the same downloads: its own program on Windows and Linux,
  and a support module the Android app downloads for you.
- **Native code throughout:** the game's PowerPC code is translated ahead of time to ARM64 and
  x86-64. Graphics go through [aurora](https://github.com/encounter/aurora) and Dawn.

### 🖥️ Display and graphics

- **Aspect ratios:** Original 4:3, 16:9, 16:10 and Ultrawide. Switching between 4:3 and widescreen
  takes effect after a restart.
- **Real ultrawide:** on screens wider than 16:9 (ultrawide monitors, most phones) the game really
  draws wider. Levels, the title screen, the opening, the world map and the menus show more to the
  sides instead of stretching, and enemies appear at the real edges of your screen.
  - Rooms narrower than your screen, like the castle boss arenas, sit in the middle, so bosses move
    exactly as in 16:9.
  - The Items screen's models stay on their cards.
  - LAN games always play in 16:9, so every device sees the same thing.
- **Resolution:** 0.5x to 4x the Wii's 480 lines (240p to 1920p), or **Native**, your screen's own
  resolution.
- **Renderer:** Automatic, or pick one yourself. Windows: Direct3D 12, Vulkan, Direct3D 11. Android:
  Vulkan, falling back to OpenGL ES.
- **Fullscreen** (Windows and Linux): On/Off in Settings, or **F11**. On is a borderless window
  covering the screen.
- **V-Sync** (Windows and Android): no tearing, and the game's clock follows the display's refresh,
  so frames never pile up. Off by default on Windows, on by default on Android.
- **Frame Interpolation** (*experimental*): Off, 120, 180, 240 or 300 FPS, for screens above 60 Hz.
  With Battery Saver on, 60 turns its 30 FPS back into 60 smooth frames.
- **Battery Saver:** the game keeps running at full speed but shows 30 FPS, so the GPU does half the
  work. It pauses during LAN play.
- **Spinning Coins:** animated coins and blocks, updated on the GPU. On by default.
- **Depth Blur:** turn off the blur and heat haze of desert, beach, water, lava and Bowser areas.
- **Performance Monitor:** the FPS counter and a frametime graph, sized from 5% to 200%. With
  V-Sync on, the graph shows how many screen refreshes each frame stayed up.
- **Fewer shader hitches:** the GPU driver's shader cache is saved, and NSMBW's own list of about 1,800
  shaders is built once, the first time you start a new version ("Compiling shaders").
- **Older GPUs:**
  - Direct3D 11 for GPUs without Vulkan or Direct3D 12 (like Intel HD 2000/2500/4000).
  - Fixes for Intel GPUs on Direct3D 11. Tested on an HD 4600 at 60 fps; other Intel GPUs get the
    same fixes but haven't been tested.
  - **Graphics Compatibility** (Android): fixes for phone GPUs that scramble the game's models (Adreno
    needs them), automatic by default.
- **GPU Driver** (Android, Snapdragon phones): import a driver zip such as Turnip to use instead of the
  phone's own. If it stops the game from starting, it goes back to Default by itself.

### ⚡ Performance

- **60 fps** on every device in the [tested devices](#tested-devices) table that's supported.
- **Faster game thread (0.5.0):** the game's own work per frame dropped by about 20–24% on a slower
  Android device we measured, in courses and on the world map, and a little on desktops too.
- **Graphics on a second CPU core**, like Dolphin's dual-core mode: up to half again as many frames
  where the CPU held the game back. On with 4 or more cores, and it helps two- and three-thread CPUs too.
- **No waiting on the disk:** on Windows, and on phones with 3 GB of memory or more, the game files are
  read into memory in the background.
- **Smoother scene changes:** effects load natively, shared textures are reused, and big ones are
  decoded on several cores.
- **Cheaper math:** the Wii's exact float rounding takes a fast path that gives the same bits.
- **Windows:** the game runs at above-normal priority and opts out of Windows' power-saving slowdown.
- **Drop reports:** every frame over 20 ms is written to the log with where the time went.

### 🎮 Controls

- **Keyboard and mouse** (a Wii Remote held sideways):
  - Every key can be changed in the Keybinds tab, and any control can use a **mouse button** (right
    click is Down by default).
  - **Combination bindings:** 2 or 3 keys or buttons pressed together for one control.
  - **Space Smart Spin** (on by default): Space in the air spins, like shaking the remote. Wall jumps,
    swimming and Yoshi's flutter keep using 2, and near the ground it's your next jump.
  - **Mouse as Gyro** (off by default): move the mouse to tilt; move it quickly left and right to
    shake.
  - **Key prompts:** the game's on-screen prompts show the keys you bound and follow your changes.
- **Controllers:**
  - Work as a sideways Wii Remote, on PC and Android.
  - Every button can be rebound in the Controller tab or F10's controller setup, combinations too.
  - **Tilt Left / Tilt Right can be rebound (0.5.0):** a trigger, a stick or a button. On a trigger or
    stick, how far you press sets how far the remote tilts.
  - **Button prompts** in the Xbox layout.
  - **Motion sensors:** on a controller that has them, tilt and shake use the controller (*untested on
    a real controller*).
  - Plugging one in or out no longer freezes the game on Windows. On Android a controller that
    reconnects works again, a real controller always comes before a stickless TV remote, and controllers
    that report themselves as keyboards work as controllers.
- **Touch** (Android):
  - An on-screen sideways Wii Remote (D-pad, 1, 2, A, Shake, + and -) with adjustable opacity and
    vibration.
  - **Layout editor:** move, add and delete buttons (any Wii Remote button, plus the Power-Up button),
    and save layouts as profiles that survive updates.
  - It hides when you use a controller; tap the screen to bring it back. A Touch Controls switch is in
    the game's Controls tab.
- **Tilt and shake:** turn the phone like a steering wheel to tilt (gyroscope and accelerometer), jolt
  it to shake. **Reverse Tilt** (on by default, all devices) makes tilting platforms go the way you tilt.
- **Rumble:** the Wii Remote's rumble reaches controllers, real Wii Remotes and phones.
- **Real Wii Remotes:**
  - A Wii Remote connected through your PC's own Bluetooth works like a controller.
  - **Bluetooth passthrough (0.5.0, *experimental*, off by default, Windows and Linux):** the game's own
    Wii Remote code drives real remotes through a spare USB Bluetooth adapter, speaker included. *Not
    yet tried with real hardware.* See [Real Wii Remotes over Bluetooth](#real-wii-remotes-over-bluetooth-experimental).

### 👆 Tap or click the menus

- Tap (Android) or click (PC) the game's own menus: the title screen, Select a File, Select Players,
  the character select, the LAN menus, the pause and **+** menus, Save / Don't Save windows, the Star
  Coins screen and Select a World's islands.
- **World map:** tap a level, Toad house, tower or castle and the player walks there along the paths;
  tap the one you're on to play it. The Menu / Select World / View Map / Items buttons can be tapped.
- A tap skips the title screen's idle movie and demos.
- A **Back** button on the file select returns to the title screen (shown when you use touch or a
  mouse).
- The on-screen remote hides on the menus and comes back for the game.

### 👥 Multiplayer and LAN

- **Couch:** the normal local multiplayer, up to 4 players on one device.
- **LAN play:** up to 4 players on any mix of phones and PCs. Every device runs the game and only the
  players' inputs travel, in lockstep.
  - A **Couch / LAN** button at the top right of the file select, in the game's own style.
  - **Create Room** (choose how many are playing) or **Join** from a list that fills itself.
  - **Direct Connection:** join a host by typing its address.
  - **Over the internet with [Tailscale](https://tailscale.com)**, phones included. Devices find and
    remember each other. It works, but can be unstable.
  - If a player drops, the others get a message instead of a frozen game, and the host keeps the
    progress.
- See [LAN and Tailscale multiplayer](#lan-and-tailscale-multiplayer).

### 🍄 Gameplay options

- **Pick your character in single player:** Mario, Luigi, Blue Toad or Yellow Toad, remembered per save
  file. Uses mkwcat's "Player 1 Can Change Character" code.
- **Autosave after every level:** clearing a course saves your file as soon as you're back on the
  world map.
- **Exit Course** from the pause menu in courses you haven't cleared yet (not the map's ambush fights).
- **Gameplay tab:**
  - **No Time Limit** (also hides the timer).
  - **Infinite Lives.**
  - **Power-Up:** gives player 1 any power-up, the Star included (and the Hammer Suit in Newer).
  - Infinite Lives and Power-Up ask first, then mark that save "Cheated" on the file select.
  - **Cheat Button** (Android): an on-screen button that gives you the chosen power-up.
  - These keep the game's own rules in LAN play.

### ⚙️ Settings window

- **Laid out for 16:9 (0.5.0):** nearly full screen over the pause menu's see-through dark fill, with
  big tabs, rows and buttons, and dropdowns for Resolution, Aspect Ratio and Frame Interpolation. With
  a 4:3 picture it fits inside the picture.
- **Opens from** the gear on the file select, **Settings** in the world map's **+** menu and in a
  level's pause menu (built from the game's own disc art, with its animations).
- **Tabs:** Display, Graphics, Audio, Keybinds (Controller with a controller, Controls on touch),
  Gameplay, Language and Mods.
- **Audio:** Master, Music, Sound Effects and Menu Sounds sliders. Drag them, or move them 5% at a time.
  Each device in a LAN game sets its own.
- A one-line explanation under every option.
- **F10** on PC opens the settings bar too: controller setup, LAN devices and more.
- Everything you change is saved and restored next launch.

### 🧩 Mods

- **HD Texture Pack (0.5.0):** Bighead's NSMBW HD Texture Pack v2.81, made by Bighead, razius, jaco8,
  CDAMJC and Amon1995, from **Settings > Mods**.
  - Downloads from the pack's own links (this project never hosts it), checks it, unpacks it and turns
    it on. About 23 MB to download, 38 MB on disk.
  - Turn it on and off while you play. The first time each HD texture shows up costs a moment.
  - On Android, the app downloads it. For the original game only.
- **Other games and mods:** saves are kept per game, so a modded disc can sit beside the original (see
  [Installing](#installing)).

### 🌟 Newer Super Mario Bros. Wii

- Plays your own **Newer v1.30 (USA, `SMNE03`)**.
- **Windows and Linux (0.5.0):** its own `NewerSMBW` program in the download, with a compat copy, its
  own `game-newer` folder, settings and saves. It asks for your Newer disc image or extracted folder at
  the first start.
- **Android:** the **Newer Super Mario Bros. Wii** tab on the start menu downloads its support module
  and imports your Newer disc image, extracted folder or a zip of it. Since 0.5.0 the module is built
  for Android 9 and up, like the app.
- Newer keeps its own settings, saves and logs apart from the base game's.
- On Newer's world map the figure stays Mario, whichever character you pick.

### 🌍 Languages

- **English, Español, Français:** Nintendo's own translations from the USA disc. A change applies the
  next time the game starts.
- **Italiano:** the start menu and the game's menus, with the accented letters the disc's fonts lack.
  The game's own text comes from your own European disc:
  - **PC:** copy its `files/EU/ItaEU` into your game folder (`DATA/files/EU/ItaEU`); the strap screen,
    Course Clear and Game Over layouts work too. Steps:
    [`BUILDING.md`](projects/nsmbw/BUILDING.md#italian-language-pack-layouts).
  - **Android:** the start menu's language button imports a language pack.
- Android's start screen has its own **Language** button, the same setting as the game's.

### 💾 Saves and updates

- **Your data stays put:** settings, saves and caches live in `Documents\MarioWiiSaveData` (Windows) or
  `~/.local/share/WiiCompiled` (Linux), so a new version extracted anywhere carries on. Saves made on
  Windows load on Linux.
- **Saves per game:** a mod or Newer never touches the original game's saves.
- **Update button** (Windows, Linux, Android): when a new release is out, an orange **Update** button
  shows on the file select. It downloads the release, checks it against GitHub's checksum, installs it
  and restarts, keeping saves, settings and game files.
- **Android:** **Check for Updates** on the start menu, and **Auto Update** (off by default), which
  also checks when you come back to the app.
- **Game checks:** only USA `SMNE01` revision 1 is accepted. Modded or repacked discs are named clearly,
  and a folder missing any of the disc's files says "The folder doesn't include all required files."
  instead of crashing.
- **PC game picker:** at the first start, **Open ISO** (`.iso` or `.wbfs`, extracted once) or **Open
  Extracted Disc Folder**.

### 📱 Android extras

- **Leaving the app pauses the game,** after writing a Quick Save as a backup, so if Android closes the
  app you continue from that level. Coming back normally removes the backup.
- **Sleeping the phone** freezes the game in memory: nothing runs, nothing plays.
- **Import** an extracted folder, a zip of it, or a Wii `.iso` straight on the phone.
- **Skip Start Menu:** open straight into the game.
- **Back gesture** in the game: Start Menu, Leave LAN Game or Quit.
- **Xiaomi Mode:** fixes the black screen on Xiaomi, Redmi and POCO phones. On by default only there.
- **Download Logs (For Github Reports):** a zip of recent logs, your settings and the phone's model,
  ready for an issue. No saves or game files.
- **Settings** on the start menu: display, controls, touch layout and more, kept in step with the game's
  own Settings window.
- **Help** shows the setup steps again.

---

## Tested devices

| Device | Chipset | GPU | Android | Result |
| --- | --- | --- | --- | --- |
| Samsung Galaxy S26 Ultra (SM-S948U) | Qualcomm Snapdragon 8 Elite Gen 5 for Galaxy (SM8850) | Adreno 840 | 16 | 60 fps, LAN play |
| T-Mobile REVVL 7 5G (TMRV075G) | Qualcomm Snapdragon 6 Gen 1 (SM6450) | Adreno 710 | 14 | 60 fps, LAN play |
| Google Pixel 3 XL | Qualcomm Snapdragon 845 (SDM845) | Adreno 630 | 12 | 60 fps (thanks to v0.2.2 performance), LAN play |
| LG V30+ (LS998) | Qualcomm Snapdragon 835 (MSM8998) | Adreno 540 | 8.0 | Not supported: needs Android 9 or newer |
| Windows 10 PC | AMD Ryzen 5 5600G | NVIDIA GeForce RTX 5070 | - | 60 fps, LAN play with the phones |
| Windows PC | AMD Ryzen 7 5700X3D | NVIDIA GeForce RTX 5070 Ti | - | 60 fps, LAN play |
| Windows PC | Intel Core i5-4440 | Intel HD Graphics 4600 (integrated) | - | 60 fps, LAN play |

Linux was run on Ubuntu 26.04 and Fedora 44 up to version 0.2.2, both under WSL with software
rendering: it booted, played with sound, and computed the same frames as Windows for LAN play. Reports
from real Linux PCs and the Steam Deck are welcome.

> [!WARNING]
> **Untested on Mali GPUs** (most phones with MediaTek, Samsung Exynos or Google Tensor chips).
> Adreno GPUs needed workarounds for their shader compilers (Display > Graphics Compatibility); Mali
> may need its own. If you try one, please open an issue with what you see.

### Not tested yet

If you can try any of these, an issue with what you saw helps a lot.

- **Mali GPUs** (above).
- **The Linux programs of recent versions:** we haven't run them ourselves since 0.2.2. The Linux disc
  picker (Open ISO / Open Extracted Disc Folder) hasn't been seen on a real Linux desktop.
- **Bluetooth passthrough** with a real adapter and real Wii Remotes. It's experimental and off by
  default.
- **Controller motion sensors** on a real controller.
- **Intel GPUs on Direct3D 11** other than the HD 4600.
- **Newer Super Mario Bros. Wii** hasn't been played all the way through.

## Requirements

- **Android:** an arm64 phone on Android 9 or newer, with Vulkan (OpenGL ES 3.1 where Vulkan can't run
  the game). About 1 GB free for the app, the game files and the import.
- **Windows:** Windows 10 or 11, 64-bit, with a Direct3D 12, Vulkan or Direct3D 11 GPU. Processors older than
  Intel Haswell / AMD Excavator (no AVX2) get the compat build automatically (see Installing).
- **Linux:** a 64-bit x86 PC with a Vulkan driver and glibc 2.35 or newer: Fedora 36 or newer,
  Ubuntu 22.04 or newer, Debian 12 or newer, Arch, SteamOS and their relatives. X11 and Wayland both
  work.
- Your own **USA `SMNE01` revision 1** copy of New Super Mario Bros. Wii, and Dolphin on a computer
  to extract it.
- For Newer: your own **Newer Super Mario Bros. Wii v1.30 (USA, `SMNE03`)**.

Only the USA disc, revision 1 works. Other regions and revisions put the game's code at other
addresses, so the app and the PC version check the disc and refuse anything else.

> [!NOTE]
> Nobody here will tell you where to get the game. Dumping your own disc is on you, and links to
> game files won't be provided or tolerated.

## Getting the game files

You do this once, with Dolphin on a computer:

1. In Dolphin, right-click **New Super Mario Bros. Wii** and choose **Properties**.
2. Check the **Info** tab: the game ID must be `SMNE01` and the revision **1**.
3. Open the **Filesystem** tab, right-click the disc at the top and choose **Extract Entire Disc**.
   Pick an empty folder. You get a folder with `files` and `sys` in it.

**Android:** copy that folder (or a zip of it) to your phone. Open the app and tap
**Import from Extracted Game Data Folder…** or **Import Game Data Zip…** and choose it. A Wii `.iso`
can be imported too. The app checks the game, copies it in, and you can delete the copy afterwards.
Your saves are kept when you import again.

**Windows and Linux:** start `NSMBW.exe` (Linux: `./NSMBW`) and choose **Open Extracted Disc Folder**
and that folder, or **Open ISO** and your `.iso` or `.wbfs` (it's extracted once, and the game reads
that copy from then on). Or name the folder `game` and put it next to `NSMBW.exe` (`NSMBW`). Keep the
folder: the PC version reads it every time.

If any of the disc's files are missing (an extract that stopped partway, say), the game says "The
folder doesn't include all required files." Extract the disc again.

The app's start screen shows these steps until a game is imported, and **Help** shows them again.

## Quick start

Get everything from the [Releases](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases/latest)
page, and [extract your game with Dolphin](#getting-the-game-files) first.

**Android**
1. Download `NSMBW-0.5.0-android-arm64.apk` on your phone, open it and allow installing when Android asks.
2. Open the app and import your extracted game (folder, zip or `.iso`).
3. Tap **Play**.

**Windows**
1. Download `NSMBW-0.5.0-windows-x64.zip` and extract it anywhere.
2. Run `NSMBW.exe` and choose your extracted game folder or disc image.
3. Allow the firewall prompt the first time you host a LAN room.

**Linux**
1. Download `NSMBW-0.5.0-linux-x86_64.tar.gz` and extract it anywhere.
2. Run `./NSMBW` and choose your extracted game folder or disc image.

**Newer Super Mario Bros. Wii**
- **Windows / Linux:** run `NewerSMBW.exe` (`./NewerSMBW`) from the same folder and choose your Newer
  v1.30 disc image or extracted folder.
- **Android:** open the **Newer Super Mario Bros. Wii** tab on the start menu, download the support
  module there, and import your Newer v1.30.

## Installing

Download from the [Releases](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases/latest)
page:

- **Android:** `NSMBW-0.5.0-android-arm64.apk`. Open it on your phone and allow installing from that
  app when Android asks. Updates install over it and keep your saves and game files.
- **Windows:** `NSMBW-0.5.0-windows-x64.zip`, with `NSMBW.exe`, `NewerSMBW.exe` and a `compat` folder
  holding `NSMBW-compat.exe` and `NewerSMBW-compat.exe`. Extract it anywhere and run `NSMBW.exe`.
  Settings (including where your game files are), saves and caches live in `Documents\MarioWiiSaveData`,
  so a new version, extracted anywhere, carries on where the last one left off. The first start of 0.0.8
  or later takes over the data of an older version extracted in the same folder (or of the version it
  replaces). On a processor without AVX2 (older than Intel Haswell or AMD Excavator), `NSMBW.exe`
  starts `compat\NSMBW-compat.exe` by itself: the same game, with its math done the long way, so it
  still plays LAN games with everyone else. Keep the `compat` folder next to `NSMBW.exe`.
- **Linux:** `NSMBW-0.5.0-linux-x86_64.tar.gz`, with the same four programs: `NSMBW`, `NewerSMBW`,
  `compat/NSMBW-compat` and `compat/NewerSMBW-compat`. Extract it anywhere and run `./NSMBW`. The first
  start asks for your game (with zenity or kdialog; or put the extracted folder next to `NSMBW`, named
  `game`). Settings, saves and caches live in `~/.local/share/WiiCompiled` (`$XDG_DATA_HOME/WiiCompiled`
  if set). Older processors get `compat/NSMBW-compat` automatically, as on Windows. Fedora Workstation,
  KDE and Ubuntu desktops already have everything else it needs; if the game says it found no Vulkan
  driver, install it with `sudo dnf install mesa-vulkan-drivers vulkan-loader` (Fedora) or
  `sudo apt install mesa-vulkan-drivers libvulkan1` (Ubuntu, Debian), or your NVIDIA driver.
- **Newer Super Mario Bros. Wii:**
  - **Windows and Linux:** run `NewerSMBW.exe` (`./NewerSMBW`) from the same folder. The first start
    asks for your own Newer v1.30 (USA, `SMNE03`), or put its extracted folder next to it named
    `game-newer`. Newer keeps its own settings and saves: `Documents\NewerWiiSaveData` on Windows, a
    `NewerWiiSaveData` folder inside the base game's data folder on Linux. If you built Newer from
    source and kept its game in a `game` folder, rename that folder to `game-newer`.
  - **Android:** open the Newer tab on the start menu, download its support module there, and import
    your own Newer v1.30 (its disc image, the folder Dolphin's Extract Entire Disc makes, or a zip of
    that folder). `NSMBW-0.5.0-newer-android-arm64.zip` on the Releases page is that module; the app
    downloads it for you.
- **Another game, such as a mod:** saves are kept per game. Extract a second copy of the release and
  put that game's extracted folder next to its `NSMBW.exe`, named `game`. That copy plays it with
  saves of its own, and the first copy keeps playing the original.

> [!CAUTION]
> Only take builds from this repository's
> [Releases](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/releases) page. If someone's
> sharing an APK or zip anywhere else, don't touch it.

## Controls

**Android:** the on-screen remote, tilt and shake, all adjustable under Settings on the start menu,
and the layout editor for the buttons. A controller works too.

**Windows and Linux** (a Wii Remote held sideways):

| Key | Wii Remote |
| --- | --- |
| Arrow keys | D-pad |
| X or Space | 2 (jump); Space in the air spins (Space Smart Spin QoL) |
| Z or Left Shift | 1 (run, fireball) |
| C or Ctrl | Shake (spin, pick up) |
| Q / E | Tilt left / right |
| B | A |
| Right mouse button | Down (crouch) |
| Enter or Esc | + (pause, the world map's menu) |
| Minus or Tab | - |
| F9 | Pair Wii Remotes (Bluetooth passthrough only) |
| F10 | Settings bar |
| F11 | Fullscreen on / off |

Every key can be changed in the Settings window's **Keybinds** tab, mouse buttons and combinations
included. A game controller works as a sideways Wii Remote too: with one connected, the tab becomes
**Controller**, and F10's controller setup offers the same and more.

## LAN and Tailscale multiplayer

<p align="center">
  <img src="docs/screenshots/windows-lan-couch-toggle.png" alt="The Couch / LAN button at the top right of the Select a File screen, on Windows" width="640">
  <br><sub>The Couch / LAN button on the save file screen (Windows), in the game's own style. Press Up from any file to reach it.</sub>
</p>

Up to 4 players, on any mix of phones and PCs. Every player needs their own game files, and everyone
needs the same version of this port.

1. One player presses Up on the save file screen to reach the **Couch** button at the top right,
   switches it to **LAN**, then picks **Create Room** and how many are playing.
2. The others switch it to **LAN** too, pick **Join**, and choose the room from the list (or use
   **Direct Connection** and type the host's address).
3. The host starts the room. Every device restarts into the session with the host's save, and the
   game goes to the player select by itself.

**Same Wi-Fi:** rooms show up by themselves.

**Over the internet, with Tailscale:** install [Tailscale](https://tailscale.com) on every device
and sign in to the same tailnet (or [share a device](https://tailscale.com/kb/1084/sharing) with
friends). Tailscale can't broadcast, so the game finds rooms there like this:

- A **PC** asks every device in `tailscale status`, and a PC's room is announced to every Tailscale
  device, so it shows up on the phones by themselves.
- Devices **remember** the Tailscale devices they meet (on the same Wi-Fi or over Tailscale) and
  **pass them on**: once any device has seen another, every game device on the tailnet learns it.
- To find another **phone** the very first time with no PC around, add its Tailscale name (as shown
  in the Tailscale app, e.g. `s26-ultra`) or its 100.x address under **Tailscale & LAN Devices**:
  on the start menu on Android, or **F10 > LAN Play** on Windows. Those
  screens also show the device's own addresses to tell others. Only one of the two needs to add the
  other.

Rooms on the same Wi-Fi are always used over the Wi-Fi, even when Tailscale is on. Tailscale play
was tested between the Windows PC and the REVVL 7 over Tailscale alone, in both directions. It works,
but can be unstable.

Windows asks to let the game through the firewall the first time you host a room; allow it, or
others can't find your room. **Couch** is the normal local multiplayer on one device.

## Real Wii Remotes over Bluetooth (experimental)

> [!WARNING]
> New in 0.5.0, **off by default, and not yet tried with a real adapter or a real Wii Remote.** Windows
> and Linux only. While it's on, the keyboard and controllers don't control the game. It's always off
> in LAN play.

With a USB Bluetooth adapter the game can take over, the game's own Wii Remote code drives real Wii
Remotes, as on a Wii: buttons, motion, rumble, lights and the speaker.

1. **Use a spare USB Bluetooth dongle**, not the one your keyboard, mouse or headphones need. The same
   adapters as Dolphin's passthrough work best: a Broadcom BCM20702 or CSR8510 A10 chip. Most Realtek,
   Intel and MediaTek adapters won't work.
2. **Windows:** give it the WinUSB driver with [Zadig](https://zadig.akeo.ie). Choose *Options > List
   All Devices*, pick the dongle (check its USB ID, so it isn't your PC's built-in Bluetooth), choose
   **WinUSB** and **Replace Driver**. To give it back to Windows later: Device Manager > the dongle >
   *Uninstall device* with its driver software, then unplug and replug it.
   **Linux:** give your user read and write access to the dongle's USB device (a udev rule with its
   IDs from `lsusb`). The game borrows it from the system while it runs.
3. **Turn it on** in **Settings > Gameplay > Bluetooth Passthrough** (Auto / On / Off), then restart
   the game.
4. **Pair each remote once:** press **F9** (or **Pair Wii Remotes** in the same tab), then the red SYNC
   button under the remote's battery cover. After that, any button connects it. Holding F9 for 10
   seconds forgets every paired remote.

If you try it, please open an issue with the `[bluetooth]` lines from the newest `console.log` in the
game's `Logs` folder.

## Building from source

Owning the game is still required even if you compile everything yourself.

You'll need: .NET 8 SDK, CMake, Ninja, LLVM/Clang (LLVM-MinGW for Windows, the Android NDK for
Android) and Python 3. The full steps, from Dolphin's extraction to the APK and the exe, Newer and the
Italian language files included, are in [`projects/nsmbw/BUILDING.md`](projects/nsmbw/BUILDING.md).

- The NSMBW project: [`projects/nsmbw`](projects/nsmbw) (translator config, function map, tools).
- The runtime, retargeted from Mario Kart Wii to NSMBW: [`runtime`](runtime).
- The Android app: [`android`](android).
- WiiCompiled's own README, for the translator and the runtime it comes from:
  [`docs/WiiCompiled-README.md`](docs/WiiCompiled-README.md).

## What the downloads contain

The release downloads include the game's code, translated from the disc's PowerPC executable to
native ARM64 and x86-64 code, but no game files: no disc image, no levels, music, graphics or
sounds, and no console keys. You supply those from your own disc. This is the same model KartPad
uses for Mario Kart Wii. WiiCompiled itself does not ship translated code: its setup translates the
game on your own machine. The HD Texture Pack isn't included either: the game downloads it from the
pack's own links when you ask.

The source code in this repository is free software under the GPLv3 (see [License](#license)).
That license covers this project's code only; it gives no rights to Nintendo's game, its code or
its assets.

## FAQ

**Is this an emulator?**
No. Everything is compiled to native code before you ever press play. At runtime there's nothing
emulating a Wii CPU or GPU.

**Is it a decompilation?**
Not in the hand-written sense: it's a static recompilation. WiiCompiled's translator turns the
game's PowerPC code into C++ automatically, and that is compiled for your phone or PC. The runtime
around it (graphics, audio, input, the Wii's system software) is source code.

**Do you provide the game?**
No. Don't ask.

**Which game version works?**
USA `SMNE01`, revision 1. Other regions and revisions are **rejected**: their code sits at other
addresses, and this port's runtime hooks the game at exact addresses. For Newer, v1.30 (USA,
`SMNE03`).

**Will you fix original bugs?**
No, behaving like the real game is the goal. Only report things where this port differs from the
original game on a Wii or in Dolphin.

**The game crashed or shows something wrong.**
Open an issue with your device, what you did, and the log from `Documents\MarioWiiSaveData\Logs`
(Windows) or `~/.local/share/WiiCompiled/Logs` (Linux). On Android, **Download Logs (For Github
Reports)** at the bottom of the start screen makes a zip to attach.

**Is it done?**
No, this is 0.5.0. It boots and plays on the tested devices, but not every level has been checked,
and rendering, performance and LAN play are still being worked on.

## AI usage

This port was entirely coded by Claude Opus 5.5, Anthropic's AI model, directed and tested by
WitherBlitz on the devices above. That includes moving WiiCompiled's runtime from Mario Kart Wii to
New Super Mario Bros. Wii, the Android app, and LAN play.

## Credits and thanks

**Contributors and helpers**

- **[Andreas Gruber (@andygruber)](https://github.com/andygruber)** - Newer Super Mario Bros. Wii
  support ([#21](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/pull/21)), the Linux
  X11 frame-time fix ([#22](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/pull/22)),
  the Linux LAN host fix, safer host saves in LAN play and the Linux build script.
- **PhilRoll** - Italian from the European disc's language files and remappable tilt
  ([#38](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation/pull/38)).
- **[Jude (@YlPorts)](https://github.com/YlPorts)** - the Android draw-count and Mali GPU changes.
- **mkwcat** - the "Player 1 Can Change Character" code (MIT) behind single-player character choice.
- **Bighead, razius, jaco8, CDAMJC and Amon1995** - the NSMBW HD Texture Pack.
- **moshy** - testing builds of 0.4.0 and 0.4.1 before release.
- **mothy** - the idea for mouse-button bindings.
- Everyone who opened an issue: much of this port came from your reports.

**Projects this port is built on**

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors - the
  static recompiler and runtime this port is built on.
- **[KartPad](https://github.com/chrissotraidis/kartpad)** - WiiCompiled on Android, whose approach
  and game-file import steps this port follows.
- **[aurora](https://github.com/encounter/aurora)** - the GX rendering/windowing backend. MIT
  licensed.
- **[Dawn](https://dawn.googlesource.com/dawn)** - Google's WebGPU implementation, powering aurora's
  Vulkan and Direct3D backends.
- **[Dolphin Emulator](https://github.com/dolphin-emu/dolphin)** - the reference for Wii hardware
  behavior and for the Bluetooth passthrough's design, the free DSP coefficient ROM and the default
  WiiConnect24 tree bundled with the runtime, and the tool you extract your game with.
- **The NSMBW reverse-engineering community** - RootCubed's
  [NSMBW symbols](https://github.com/RootCubed/nsmbw-symbols), the NSMBW decompilation and the
  NSMBW address maps (NSMBW-Updated by RoadrunnerWMC, originally by Ninji and CLF78), which the
  function map is built from.
- **[libadrenotools](https://github.com/bylaws/libadrenotools)** - the Android GPU Driver option.
- **[libusb](https://github.com/libusb/libusb)** - USB access for the Bluetooth passthrough.
- **[LZMA SDK](https://www.7-zip.org/sdk.html)** by Igor Pavlov (public domain) - unpacks the HD
  Texture Pack.
- Everyone in the static recompilation community.

Bundled third-party components and their licenses live in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## License

This project is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License, version 3](LICENSE) as published by the Free Software Foundation, like
WiiCompiled, which it is based on.

**Credit is required.** Under section 7(b) of the GPLv3, if you use this project's own code, you
must credit WitherBlitz in your project's README, with a link to this repository. A GitHub fork of
this repository doesn't need to: its page already shows where it came from. If you only used this
project as a reference, please credit it the same way. If you're making a recompilation from it,
please make it a fork. See [`NOTICE.md`](NOTICE.md) for the exact terms.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
License for more details.

Not affiliated with, endorsed by, or associated with Nintendo. New Super Mario Bros. Wii is a
trademark of Nintendo.

**I plan to fully decompile this game in the near future, but for now with the time I have at the moment, this'll have to do. At first, this project was to just see the limits of what I can do with Opus, but now I'd like to see it to the end haha. I'll use this as a base for some of the features and copy them over once I decompile the game. Sorry for the misleading repo name! I changed my mind midway through the creation of this project.**
