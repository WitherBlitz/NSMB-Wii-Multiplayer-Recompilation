# Configure the NSMBW native build exactly like WiiCompiled's Launcher/LocalBuild.ps1 does:
# same canonical flags (NativeBuildFlags.ps1), the pinned Dependencies tree as FetchContent sources,
# the portable llvm-mingw/CMake/Ninja toolchain, and a scrubbed PATH so no other compiler leaks in.
# -Newer: Newer Super Mario Bros. Wii (BUILDING.md section 7) in native-build-newer, accepting its
# disc (NSMBW_GAME_ID=SMNE03).
[CmdletBinding()]
param(
    [int]$TranslatedJobs = 6,
    [string]$BuildDirectory,
    [string]$LogPath,
    [switch]$Newer
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
if (-not $BuildDirectory) { $BuildDirectory = Get-NsmbwBuildDirectory $(if ($Newer) { 'native-build-newer' } else { 'native-build' }) }
if (-not $LogPath) { $LogPath = Join-Path (Get-NsmbwLogDirectory $BuildDirectory) 'configure.log' }
$gameId = if ($Newer) { 'SMNE03' } else { 'SMNE01' }
$workspace = $NsmbwRepo
$toolchain = Get-NsmbwToolchain
if (-not (Test-Path -LiteralPath (Join-Path $workspace 'Dependencies\SDL'))) {
    throw "No pinned sources in $workspace\Dependencies: run Launcher\Prepare-Dependencies.ps1 -Destination Dependencies"
}
. (Join-Path $workspace 'Launcher\NativeBuildFlags.ps1')

$bin = Join-Path $toolchain 'llvm-mingw\bin'
$env:PATH = Get-MkwToolchainPath $toolchain

$configure = Get-MkwNativeConfigureArguments `
    -SourceDirectory (Join-Path $workspace 'runtime') -BuildDirectory $BuildDirectory `
    -Ninja (Join-Path $toolchain 'Ninja\ninja.exe') `
    -CCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang.exe') `
    -CxxCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang++.exe') `
    -ResourceCompiler (Join-Path $bin 'x86_64-w64-mingw32-windres.exe') `
    -DependenciesDirectory (Join-Path $workspace 'Dependencies') `
    -AdditionalArguments @("-DMKW_TRANSLATED_COMPILE_JOBS=$TranslatedJobs", "-DNSMBW_GAME_ID=$gameId")

# Windows PowerShell 5.1 turns a native program's stderr lines into error records; CMake prints
# ordinary status there, so run it with 'Continue' and keep the log as plain text.
$ErrorActionPreference = 'Continue'
& (Join-Path $toolchain 'CMake\bin\cmake.exe') @configure 2>&1 |
    ForEach-Object { "$_" } | Out-File -LiteralPath $LogPath -Encoding utf8
exit $LASTEXITCODE
