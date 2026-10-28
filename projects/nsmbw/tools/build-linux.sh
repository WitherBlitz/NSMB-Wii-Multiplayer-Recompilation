#!/usr/bin/env bash
# Build New Super Mario Bros. Wii (or Newer Super Mario Bros. Wii on top of it) for Linux x86_64:
# translator -> translate-recursive / emit-base-manifest / generate-data-init / emit-build-shards
# -> CMake + Ninja -> a folder with the game and its data files. See projects/nsmbw/BUILDING.md.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-linux.sh [--newer NEWER_DIR] [--output DIR] [--game DIR] [--jobs N]

  (no --newer)     the base game: ../game (unpack_game.py + prelink_rels.py), recomp.yml, native-build/
  --newer DIR      Newer Super Mario Bros. Wii, extracted with Dolphin (DIR holds DATA/ or files/ + sys/):
                   newer_prelink.py into ../game-newer, recomp-newer.yml, native-build-newer/,
                   NSMBW_GAME_ID=SMNE03
  --output DIR     where the game is put (default ../nsmbw-local-build or ../nsmbw-newer-build)
  --game DIR       extracted disc to link as DIR/game beside the game (skips the folder question)
  --jobs N         parallel jobs (default: CPU count; translated shards capped by RAM)

Tools: dotnet (PATH or ~/.dotnet), and clang/cmake/ninja from Launcher/prepare-portable-tools.sh
(Launcher/artifacts/portable-tools/toolchain-x86_64), or set TOOLCHAIN_BIN to another bin folder.
EOF
}

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
newer=""
output=""
game=""
jobs=$(nproc)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --newer) newer=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --game) game=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; echo "build-linux.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

fail() {
    echo "build-linux.sh: error: $*" >&2
    exit 1
}

step() {
    printf '\n== %s\n' "$*"
}

if [[ -n "$newer" ]]; then
    project=projects/nsmbw/recomp-newer.yml
    build=$repo/native-build-newer
    game_id=SMNE03
    output=${output:-$repo/../nsmbw-newer-build}
else
    project=projects/nsmbw/recomp.yml
    build=$repo/native-build
    game_id=SMNE01
    output=${output:-$repo/../nsmbw-local-build}
fi

dotnet=$(command -v dotnet || true)
[[ -n "$dotnet" ]] || dotnet=$HOME/.dotnet/dotnet
[[ -x "$dotnet" ]] || fail "dotnet 8 SDK not found (PATH or ~/.dotnet); see BUILDING.md"
export DOTNET_ROOT=$(dirname "$(readlink -f "$dotnet")") DOTNET_CLI_TELEMETRY_OPTOUT=1
bin=${TOOLCHAIN_BIN:-$repo/Launcher/artifacts/portable-tools/toolchain-x86_64/bin}
[[ -x "$bin/clang" && -x "$bin/cmake" && -x "$bin/ninja" ]] ||
    fail "no clang/cmake/ninja in $bin: run Launcher/prepare-portable-tools.sh --arch x86_64"
[[ -f "$repo/../game/nsmbw_linked.rel" ]] ||
    fail "../game is not prepared: run unpack_game.py <extracted SMNE01> and prelink_rels.py first"

# Translated shards are large: about 2 GiB each while compiling.
mem_gib=$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo)
shard_jobs=$(( mem_gib / 2 < jobs ? mem_gib / 2 : jobs ))
(( shard_jobs >= 1 )) || shard_jobs=1

cd "$repo"
step "Building the translator"
"$dotnet" build translator/Translator.sln -c Release -v quiet -nologo | tail -3
translator=("$dotnet" translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll)

if [[ -n "$newer" ]]; then
    step "Linking Newer's code offline (newer_prelink.py)"
    python3 projects/nsmbw/tools/newer_prelink.py "$newer"
fi

logs=$build/logs
mkdir -p "$logs"
# Runs a step with its output in $logs/<name>.log; on failure shows the end of it.
logged() {
    local name=$1
    shift
    if ! "$@" > "$logs/$name.log" 2>&1; then
        tail -25 "$logs/$name.log" >&2
        fail "$name failed; full log: $logs/$name.log"
    fi
}

step "Translating ($project, logs in $logs)"
logged translate "${translator[@]}" translate-recursive 0x80004050 --project "$project" --outdir generated/functions \
    --output-metadata generated/base_translation_output.json \
    --production-source-bundle generated/base_translation_sources.bin \
    --no-function-files --prune-stale --threads "$jobs"
grep -E "Recursively translated|skipped|warning" "$logs/translate.log" || true
logged manifest "${translator[@]}" emit-base-manifest --project "$project" --out build/base \
    --functions-dir generated/functions --translation-output-metadata generated/base_translation_output.json --region E
logged data-init "${translator[@]}" generate-data-init --project "$project"
logged shards "${translator[@]}" emit-build-shards --project "$project" \
    --base-metadata generated/base_translation_output.json --base-functions-dir generated/functions \
    --native-source-dir runtime/src --out generated/build_shards

step "Configuring $build"
configure=(-S runtime -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER="$bin/clang" -DCMAKE_CXX_COMPILER="$bin/clang++" -DCMAKE_MAKE_PROGRAM="$bin/ninja"
    -DCMAKE_AR="$bin/llvm-ar" -DCMAKE_C_COMPILER_AR="$bin/llvm-ar" -DCMAKE_CXX_COMPILER_AR="$bin/llvm-ar"
    -DCMAKE_RANLIB="$bin/llvm-ranlib" -DCMAKE_C_COMPILER_RANLIB="$bin/llvm-ranlib"
    -DCMAKE_CXX_COMPILER_RANLIB="$bin/llvm-ranlib" -DCMAKE_LINKER="$bin/ld.lld" -DCMAKE_ASM_COMPILER="$bin/clang"
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DMKW_TRANSLATED_COMPILE_JOBS="$shard_jobs" -DNSMBW_GAME_ID="$game_id")
# SDL3 refuses to configure without these X11 extensions' headers unless they are switched off.
pkg-config --exists xscrnsaver 2>/dev/null || configure+=(-DSDL_X11_XSCRNSAVER=OFF)
pkg-config --exists xtst 2>/dev/null || configure+=(-DSDL_X11_XTEST=OFF)
logged configure "$bin/cmake" "${configure[@]}"

step "Compiling (a switch between the base game and Newer recompiles most of it)"
logged build "$bin/cmake" --build "$build" --target WiiCompiled --parallel "$jobs"
grep -E "Linking CXX executable" "$logs/build.log" || echo "nothing to recompile"

step "Publishing to $output"
mkdir -p "$output"
cp -f "$build/WiiCompiled" "$build/dsp_coef.bin" "$build/initial_pipeline_cache.db" "$build/cacert.pem" "$output/"
cp -rf "$build/wii_bootstrap" "$output/"
if [[ -n "$game" ]]; then
    ln -sfn "$(readlink -f "$game")" "$output/game"
fi
echo "Done: $output/WiiCompiled"
