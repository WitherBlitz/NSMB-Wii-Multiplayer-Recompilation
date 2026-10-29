# Where the Windows build scripts find things, dot-sourced by each of them. Everything is found from
# this repository's own location, so it can be checked out anywhere; environment variables override:
#   NSMBW_TOOLCHAIN   llvm-mingw\, CMake\, Ninja\ and optionally dotnet\. Default: a toolchain folder
#                     next to the repository if there is one, else Launcher\artifacts\portable-tools
#                     (where Launcher\Prepare-PortableTools.ps1 puts them).
#   NSMBW_LOGS        build logs. Default: logs\ in the build folder.
#   ANDROID_NDK_HOME  NDK 29.0.14206865. Default: ndk\29.0.14206865 in ANDROID_HOME, else in
#                     %LOCALAPPDATA%\Android\Sdk (Android Studio's SDK folder).

$NsmbwRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))

function Get-NsmbwToolchain {
    $toolchain = if ($env:NSMBW_TOOLCHAIN) { $env:NSMBW_TOOLCHAIN }
        elseif (Test-Path -LiteralPath (Join-Path $NsmbwRepo '..\toolchain\CMake\bin\cmake.exe')) { Join-Path $NsmbwRepo '..\toolchain' }
        else { Join-Path $NsmbwRepo 'Launcher\artifacts\portable-tools' }
    $toolchain = [IO.Path]::GetFullPath($toolchain)
    if (-not (Test-Path -LiteralPath (Join-Path $toolchain 'CMake\bin\cmake.exe'))) {
        throw "No toolchain in ${toolchain}: run Launcher\Prepare-PortableTools.ps1, or set NSMBW_TOOLCHAIN"
    }
    $toolchain
}

# The default build folders: native-build (vanilla), native-build-newer, android-build.
function Get-NsmbwBuildDirectory([string]$Name) { Join-Path $NsmbwRepo $Name }

# The log folder for a build folder, created if needed.
function Get-NsmbwLogDirectory([string]$BuildDirectory) {
    $logs = if ($env:NSMBW_LOGS) { $env:NSMBW_LOGS } else { Join-Path $BuildDirectory 'logs' }
    New-Item -ItemType Directory -Force $logs | Out-Null
    [IO.Path]::GetFullPath($logs)
}

function Get-NsmbwNdk {
    $ndk = if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME }
        elseif ($env:ANDROID_HOME) { Join-Path $env:ANDROID_HOME 'ndk\29.0.14206865' }
        else { Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk\29.0.14206865' }
    if (-not (Test-Path (Join-Path $ndk 'build\cmake\android.toolchain.cmake'))) {
        throw "NDK missing: $ndk (set ANDROID_NDK_HOME)"
    }
    $ndk
}
