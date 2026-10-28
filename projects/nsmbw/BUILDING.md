# Building New Super Mario Bros. Wii from source

Building it yourself still needs your own **New Super Mario Bros. Wii (USA, SMNE01, revision 1)**,
extracted with Dolphin's **Extract Entire Disc**. These are the steps and scripts the 0.0.1 release
was built with, on Windows 10 x64.

The PowerShell scripts in [`tools/`](tools) were written for this folder layout. Change the paths at
the top of each script if yours differs:

```
E:\NSMBWPort\
  wiicompiled\        this repository
  game\               the translator's inputs (step 2)
  toolchain\          dotnet\, llvm-mingw\, CMake\, Ninja\ (step 1)
  build-nsmbw\logs\   build logs
```

## 1. Tools

- The same native toolchain as WiiCompiled: .NET 8 SDK, CMake, Ninja and llvm-mingw 20260616.
  [`Launcher/Prepare-PortableTools.ps1`](../../Launcher/Prepare-PortableTools.ps1) downloads the
  pinned versions.
- The pinned third-party sources in `Dependencies/`:
  [`Launcher/Prepare-Dependencies.ps1`](../../Launcher/Prepare-Dependencies.ps1). Android also needs
  `Dependencies/dawn_prebuilt_android` (Dawn `v20260603.191052`, `dawn-android-aarch64`).
- Python 3 for the scripts in `tools/` (standard library only).
- Android only: Android SDK (platform 34, build-tools 34), NDK 29.0.14206865, JDK 17, Gradle 8.7.

## 2. The game's code

```
python projects/nsmbw/tools/unpack_game.py <your extracted game folder>
python projects/nsmbw/tools/prelink_rels.py
```

`unpack_game.py` checks the disc (SMNE01 revision 1, clean `main.dol`), copies `sys/main.dol` and
unpacks the four boot RELs (`files/rels/*.rel.LZ`) into `..\game`. `prelink_rels.py` links the RELs
offline the way the game links them at boot, into the one `nsmbw_linked.rel` the translator reads
([`recomp.yml`](recomp.yml)).

## 3. Translate

```
dotnet build translator/Translator.sln -c Release
```

Then the four translator steps, as [`tools/rebuild-all.ps1`](tools/rebuild-all.ps1) runs them:
`translate-recursive`, `emit-base-manifest`, `generate-data-init` and `emit-build-shards`. The output
goes to `generated/` (ignored by git: it is the game's code).

## 4. Windows

```
projects/nsmbw/tools/configure-native.ps1
projects/nsmbw/tools/build-native.ps1
```

The game is `native-build/WiiCompiled.exe`, next to the DLLs and data files it needs. Name your
extracted game folder `game` and put it next to the exe, or let the exe ask for it.

## 5. Android

```
projects/nsmbw/tools/configure-android.ps1
projects/nsmbw/tools/build-android.ps1
projects/nsmbw/tools/stage-android.ps1 -NativeDirectory <this repository>\android\native
gradle -p android assembleRelease
```

`build-android.ps1` makes `android-build/libmain.so`; `stage-android.ps1` strips it and copies it,
with the runtime's data files, into the app (`android/native`, ignored by git). The release APK is
signed with the build machine's Android debug key (see `android/app/build.gradle`).
`deploy-android.ps1` builds, stages and packages a debug APK and installs it on a connected phone.

## 6. Linux

Steps 2 and 3 are the same on Linux; [`tools/build-linux.sh`](tools/build-linux.sh) runs step 3 and the
native build in one go. It needs:

- The .NET 8 SDK, on `PATH` or in `~/.dotnet` (no root needed:
  `dotnet-install.sh --channel 8.0 --install-dir ~/.dotnet`).
- The pinned clang, CMake and Ninja:
  `Launcher/prepare-portable-tools.sh --arch x86_64` puts them in
  `Launcher/artifacts/portable-tools/toolchain-x86_64` (about 2 GB to download, 556 MB kept).
- Python 3 and the X11, ALSA and Vulkan development headers (on Fedora `libX11-devel`, `alsa-lib-devel`,
  `vulkan-loader-devel`). Native Wayland, PipeWire and PulseAudio need `wayland-devel`,
  `libxkbcommon-devel`, `libdecor-devel`, `pipewire-devel` and `pulseaudio-libs-devel`; without them the
  game runs through XWayland and ALSA. The script switches off SDL's XScrnSaver and XTest support when
  their headers (`libXScrnSaver-devel`, `libXtst-devel`) are missing.

```
python3 projects/nsmbw/tools/unpack_game.py <your extracted game folder>
python3 projects/nsmbw/tools/prelink_rels.py
projects/nsmbw/tools/build-linux.sh --game <your extracted game folder>
```

The game is `../nsmbw-local-build/WiiCompiled`, with a `game` link to your extracted game beside it.
SDL3, Dawn and the other third-party sources are fetched by CMake on the first configure.

## 7. Newer Super Mario Bros. Wii

[Newer Super Mario Bros. Wii](https://newerteam.com/wii/) runs on top of the USA game. Its disc
(ID `SMNE03`) keeps the four RELs as they are, patches `main.dol` with a small loader, and loads its
own PowerPC code at boot: `files/NewerRes/SystemUS_1.bin` (raw patches), `DLCodeUS_1.bin` (the code)
and `DLRelocsUS_1.bin` (its relocations and its hooks into the game). A static recompilation never
runs that loader's result, so [`tools/newer_prelink.py`](tools/newer_prelink.py) does its work offline,
the way `prelink_rels.py` links the RELs, and the translator translates the patched game and Newer's
code together. The loader still runs in the game, so its data ends up in memory as on a Wii.

Build the base game's `../game` first (steps 2 and 6), then, with Newer extracted by Dolphin:

```
projects/nsmbw/tools/build-linux.sh --newer <extracted Newer folder> --game <extracted Newer folder>
```

That runs `newer_prelink.py` (it writes `../game-newer`: the patched `main.dol`, the synthetic REL
with Newer's code appended, and `MAP.txt` with Newer's function starts), translates
[`recomp-newer.yml`](recomp-newer.yml) and builds `native-build-newer` with `-DNSMBW_GAME_ID=SMNE03`,
so the game accepts the Newer disc. The game is `../nsmbw-newer-build/WiiCompiled`. By hand, on any
platform: run `newer_prelink.py <extracted Newer folder>`, then the step 3 commands with
`--project projects/nsmbw/recomp-newer.yml`, and configure with `-DNSMBW_GAME_ID=SMNE03`.

Both builds translate into the same `generated/` folder: building one after the other retranslates,
and the first build in each build folder after a switch recompiles most shards.

Things that are specific to Newer:

- Newer's code is placed where its loader's heap allocation lands at boot, `DLCODE_ADDR` in
  `newer_prelink.py` (0x80DF8FC0). The game logs the real address (`[OSReport] DLCode @ 0x80df8fc0`);
  if a runtime change ever moves it, update the constant, or Newer's first hook jumps into the wrong
  place.
- Hooks that call Newer code with `bl` but whose code returns for the hooked function are rewritten as
  `b` by `newer_prelink.py` (`rewritten_hooks` in `../game-newer/nsmbw_rel_layout.json`).
- Newer v1.30 is the version this was made with. Another version needs a new `DLCODE_ADDR` and may hook
  functions the runtime replaces natively; `newer_prelink.py` checks the files it expects.

## Other tools

- `build_function_map.py` made [`MAP.txt`](MAP.txt), the function map the translator follows.
- `ppcdis.py`, `symhash.py`, `nsmbw_versions.py`, `hook_size_check.py`, `verify_runtime_addrs.py`:
  the reverse-engineering helpers used to move WiiCompiled's Mario Kart Wii hooks to NSMBW.
- `capture-window.ps1`, `keyboard-run.ps1`, `capture-session.ps1`, `run-android.ps1`,
  `android-ui.ps1`, `gfx-test-android.ps1`: test drivers for the Windows and Android builds.
