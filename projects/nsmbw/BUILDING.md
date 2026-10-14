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

`unpack_game.py` verifies the SMNE01 revision 1 header, an allow-listed SHA-256,
and the DOL section layout; it copies `sys/main.dol` and unpacks the four boot RELs
(`files/rels/*.rel.LZ`) into the sibling `game` directory. `prelink_rels.py` then
joins the RELs into `nsmbw_linked.rel` for the translator.

**A second, structurally validated DOL:** A user's private SMNE01 rev 1 extraction had
SHA-256 `166eb6b7cca21418e316a6cfbf1bfbf0a213eb8f6396d42126a1482bb764a955`,
which differs from the SHA pinned in `recomp.yml`. The difference alone does **not**
establish that their disc is modified. To prepare this input, run:

```sh
python projects/nsmbw/tools/unpack_game.py /path/to/private-extracted-game
python projects/nsmbw/tools/prelink_rels.py
python projects/nsmbw/tools/prepare_input_profile.py
```

Before translating, audit the private export against the project's public function map:

```sh
python projects/nsmbw/tools/audit_rebuild_input.py /path/to/NSMBW-private-build-inputs.zip
```

This verifies the disc revision, DOL SHA-256 and executable layout, four REL
executable sections, and every address in MAP.txt. The supplied rev 1 export
was checked against the 39,582 public function entries: each address is in
an executable range, with no duplicate starts. This does not establish that
instructions and HLE hooks are equivalent; test before shipping.

The helper selects `recomp.yml` for the reference hash or creates a git-ignored
`recomp.local.yml` for the second hash. Use the selected file for **each**
Translator.Cli `--project` invocation, not the hardcoded `recomp.yml`.
**This only passes input hash verification:** the existing function-address
map, native HLE hooks and generated translations still need validation against
the second DOL before distributing a new `libmain.so`.

**ARM64 performance A/B testing:** After a full private native rebuild becomes
available, configure and compile the same translated source once with
`-DMKW_TRANSLATED_OPT_LEVEL=2` (existing Release behavior) and once with
`-DMKW_TRANSLATED_OPT_LEVEL=3` (experimental). Keep all other flags identical,
including the PowerPC floating-point semantics and disabled SLP vectorizer.
Benchmark the same scenes at 1x and 1.5x with profiling OFF. Revert if the
game regresses, crashes or produces graphical/physics errors. The alternate
input hash does not itself improve performance.

**Privacy and native builds:** `main.dol` and the RELs contain copyrighted
game code. Keep them and any translated output in a private workspace and
never upload them to the public fork or public GitHub Actions artifacts.
The public Android CI reuses the upstream precompiled `libmain.so`; changes
under `runtime` and `aurora-main` only change gameplay performance **after**
the full native library has been rebuilt separately.

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

## Other tools

- `build_function_map.py` made [`MAP.txt`](MAP.txt), the function map the translator follows.
- `ppcdis.py`, `symhash.py`, `nsmbw_versions.py`, `hook_size_check.py`, `verify_runtime_addrs.py`:
  the reverse-engineering helpers used to move WiiCompiled's Mario Kart Wii hooks to NSMBW.
- `capture-window.ps1`, `keyboard-run.ps1`, `capture-session.ps1`, `run-android.ps1`,
  `android-ui.ps1`, `gfx-test-android.ps1`: test drivers for the Windows and Android builds.
