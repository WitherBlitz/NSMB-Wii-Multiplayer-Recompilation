# Build targets of the NSMBW native build with the portable toolchain on a scrubbed PATH.
# -Newer: the Newer Super Mario Bros. Wii build (configure-native.ps1 -Newer), native-build-newer.
[CmdletBinding()]
param(
    [string[]]$Target = @('WiiCompiled'),
    [int]$Jobs = 10,
    [string]$BuildDirectory,
    [string]$LogPath,
    [switch]$KeepGoing,
    [switch]$Newer
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
if (-not $BuildDirectory) { $BuildDirectory = Get-NsmbwBuildDirectory $(if ($Newer) { 'native-build-newer' } else { 'native-build' }) }
if (-not $LogPath) { $LogPath = Join-Path (Get-NsmbwLogDirectory $BuildDirectory) 'build.log' }
$toolchain = Get-NsmbwToolchain
. (Join-Path $NsmbwRepo 'Launcher\NativeBuildFlags.ps1')
$env:PATH = Get-MkwToolchainPath $toolchain

$arguments = @('--build', $BuildDirectory)
foreach ($t in $Target) { $arguments += @('--target', $t) }
$arguments += @('--parallel', $Jobs)
if ($KeepGoing) { $arguments += @('--', '-k', '0') }

$ErrorActionPreference = 'Continue'
& (Join-Path $toolchain 'CMake\bin\cmake.exe') @arguments 2>&1 |
    ForEach-Object { "$_" } | Out-File -LiteralPath $LogPath -Encoding utf8
exit $LASTEXITCODE
