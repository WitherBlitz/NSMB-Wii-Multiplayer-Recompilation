#!/usr/bin/env bash
# Builds the iOS app on a Mac: the counterpart of projects/nsmbw/tools/rebuild-all.ps1 and the
# configure/build/stage-android.ps1 trio. See ios/README.md.
#
#   ios/build-ios.sh --game <Dolphin's extracted folder>   first time: unpack the disc's code, then all below
#   ios/build-ios.sh                                       translate, build, package
#   ios/build-ios.sh --skip-translate --install            rebuild the runtime only, install on a phone
#
# Signing (optional; without it the result is an unsigned ios-build/NSMBW.ipa for a re-signing tool):
#   MKW_IOS_SIGN_IDENTITY  codesign identity, e.g. "Apple Development: you@example.com (TEAMID)"
#   MKW_IOS_PROFILE        the .mobileprovision for MKW_IOS_BUNDLE_ID
#   MKW_IOS_BUNDLE_ID      bundle identifier (default com.wither.nsmbw); must match the profile
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$repo/ios-build"
logs="$build/logs"
game=""
translate=1
install=0
device=""
threads="$(sysctl -n hw.ncpu)"
bundle_id="${MKW_IOS_BUNDLE_ID:-com.wither.nsmbw}"

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --skip-translate) translate=0; shift ;;
        --install) install=1; shift ;;
        --device) device="$2"; install=1; shift 2 ;;
        --threads) threads="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; usage 1 ;;
    esac
done

step() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 ($2)" >&2; exit 1; }; }
need cmake "brew install cmake"
need ninja "brew install ninja"
need python3 "Xcode's command line tools or brew install python"
xcrun --sdk iphoneos --show-sdk-path >/dev/null || { echo "Xcode with the iOS SDK is required" >&2; exit 1; }
mkdir -p "$logs"

# 1. The game's code, from the player's own disc (projects/nsmbw/BUILDING.md, step 2).
if [[ -n "$game" ]]; then
    step "Unpacking the game's code from $game"
    python3 -I "$repo/projects/nsmbw/tools/unpack_game.py" "$game"
    python3 -I "$repo/projects/nsmbw/tools/prelink_rels.py"
fi

# 2. Translate (rebuild-all.ps1's four steps). The output, generated/, is the game's code: it stays
# on this Mac and out of git.
if [[ $translate -eq 1 ]]; then
    need dotnet "the .NET 8 SDK: brew install --cask dotnet-sdk@8, or https://dot.net"
    export DOTNET_CLI_TELEMETRY_OPTOUT=1
    step "Building the translator"
    dotnet build "$repo/translator/Translator.sln" -c Release >"$logs/translator-build.log" 2>&1 ||
        { tail -20 "$logs/translator-build.log"; exit 1; }
    translator="$(ls "$repo"/translator/src/Translator.Cli/bin/Release/net*/Translator.Cli.dll | head -1)"
    project=projects/nsmbw/recomp.yml
    run() {
        local name="$1"; shift
        step "Translator: $name"
        (cd "$repo" && dotnet "$translator" "$@") >"$logs/translate-$name.log" 2>&1 ||
            { tail -20 "$logs/translate-$name.log"; echo "see $logs/translate-$name.log" >&2; exit 1; }
    }
    run translate translate-recursive 0x80004050 --project "$project" \
        --outdir generated/functions --output-metadata generated/base_translation_output.json \
        --production-source-bundle generated/base_translation_sources.bin \
        --no-function-files --prune-stale --threads "$threads"
    run manifest emit-base-manifest --project "$project" --out build/base \
        --functions-dir generated/functions --translation-output-metadata generated/base_translation_output.json \
        --region E
    run data-init generate-data-init --project "$project"
    run shards emit-build-shards --project "$project" --base-metadata generated/base_translation_output.json \
        --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards
fi
[[ -f "$repo/generated/build_shards/shards.cmake" ]] ||
    { echo "No translated game in generated/: run once with --game <folder>, without --skip-translate." >&2; exit 1; }

# 3. Configure and build (configure-android.ps1's settings, for the iPhoneOS SDK). Translated shards
# are the memory-hungry compiles: allow one per 2 GiB of RAM.
step "Configuring for iOS"
ram_gib=$(( $(sysctl -n hw.memsize) >> 30 ))
cmake -S "$repo/runtime" -B "$build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=vendor \
    -DBUILD_SHARED_LIBS=OFF \
    -DMKW_IOS_BUNDLE_ID="$bundle_id" \
    -DMKW_TRANSLATED_COMPILE_JOBS=$(( ram_gib / 2 > 1 ? ram_gib / 2 : 1 )) \
    >"$logs/configure.log" 2>&1 || { tail -30 "$logs/configure.log"; exit 1; }
step "Building (the first build compiles the whole game; expect a while)"
cmake --build "$build" --target WiiCompiled >"$logs/build.log" 2>&1 ||
    { grep -E "error:|FAILED" "$logs/build.log" | head -20; echo "see $logs/build.log" >&2; exit 1; }
app="$build/NSMBW.app"
[[ -d "$app" ]] || { echo "the build made no $app" >&2; exit 1; }
# An iOS app can only load system libraries and what it carries; aurora's dependencies default to
# shared libraries (BUILD_SHARED_LIBS above), and one left outside the bundle kills the app at launch.
if otool -L "$app/NSMBW" | tail -n +2 | grep -vE '^\s*/(System/Library|usr/lib)/' ; then
    echo "the app links libraries iOS can't load (above)" >&2; exit 1
fi

# The Home Screen icon (Info.plist's CFBundleIcons): ios/AppIcon.png at each size iOS asks for.
if [[ -f "$repo/ios/AppIcon.png" ]]; then
    step "Adding the app icon"
    icon() { sips -z "$2" "$2" "$repo/ios/AppIcon.png" --out "$app/$1" >/dev/null; }
    icon AppIcon60x60@2x.png 120
    icon AppIcon60x60@3x.png 180
    icon AppIcon76x76@2x.png 152
    icon AppIcon83.5x83.5@2x.png 167
fi

# 4. Sign. The profile's entitlements are the base (application-identifier, team); the app's own
# (ios/NSMBW.entitlements) are kept where the profile grants them.
if [[ -n "${MKW_IOS_SIGN_IDENTITY:-}" && -n "${MKW_IOS_PROFILE:-}" ]]; then
    step "Signing with $MKW_IOS_SIGN_IDENTITY"
    cp "$MKW_IOS_PROFILE" "$app/embedded.mobileprovision"
    profile_plist="$build/profile.plist"
    entitlements="$build/NSMBW.signing.entitlements"
    security cms -D -i "$MKW_IOS_PROFILE" >"$profile_plist"
    plutil -extract Entitlements xml1 -o "$entitlements" "$profile_plist"
    for key in com.apple.developer.kernel.extended-virtual-addressing com.apple.developer.kernel.increased-memory-limit; do
        if ! plutil -extract "$key" raw -o - "$entitlements" >/dev/null 2>&1; then
            echo "warning: the profile does not grant $key; enable it for $bundle_id in the developer portal" >&2
        fi
    done
    codesign --force --timestamp=none --sign "$MKW_IOS_SIGN_IDENTITY" --entitlements "$entitlements" "$app"
    codesign --verify --strict "$app"
else
    echo "(not signed: set MKW_IOS_SIGN_IDENTITY and MKW_IOS_PROFILE to sign here)"
fi

step "Packaging"
rm -rf "$build/Payload" "$build/NSMBW.ipa"
mkdir -p "$build/Payload"
cp -R "$app" "$build/Payload/"
(cd "$build" && zip -qry NSMBW.ipa Payload)
rm -rf "$build/Payload"
echo "app: $app"
echo "ipa: $build/NSMBW.ipa"

# 5. Install on a connected phone (Xcode's devicectl).
if [[ $install -eq 1 ]]; then
    if [[ -z "$device" ]]; then
        device="$(xcrun devicectl list devices --hide-default-columns --columns Identifier --hide-headers 2>/dev/null |
            awk 'NF {print $1; exit}')"
    fi
    [[ -n "$device" ]] || { echo "no connected device found; pass --device <identifier>" >&2; exit 1; }
    step "Installing on $device"
    xcrun devicectl device install app --device "$device" "$app"
fi
