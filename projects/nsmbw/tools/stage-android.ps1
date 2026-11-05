# Stage the Android native build for the app: a stripped libmain.so in android/native/jniLibs and the
# runtime's data files (what a desktop build keeps beside its executable) in android/native/assets.
# -Newer: the Newer Super Mario Bros. Wii module instead (android-build-newer): its stripped library
# zipped as android/native/newer/NSMBW-<version>-newer-android-arm64.zip, the file to attach to that
# version's release, and its name, size and SHA-256 in assets/newer-module.properties, which the app
# (NewerModule.java) downloads and checks it by. Stage it before building the APK, and attach exactly
# that zip: another build of it has another SHA-256.
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$NativeDirectory,
    [switch]$Newer
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
if (-not $BuildDirectory) { $BuildDirectory = Get-NsmbwBuildDirectory $(if ($Newer) { 'android-build-newer' } else { 'android-build' }) }
if (-not $NativeDirectory) { $NativeDirectory = Join-Path $NsmbwRepo 'android\native' }
$ndk = Get-NsmbwNdk
$strip = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'

if ($Newer) {
    $lib = Join-Path $BuildDirectory 'libmain.so'
    if (-not (Test-Path -LiteralPath $lib)) { throw "libmain.so is missing: $lib" }
    $gradle = Get-Content -LiteralPath (Join-Path $NativeDirectory '..\app\build.gradle') -Raw
    if ($gradle -notmatch "versionName\s+'([^']+)'") { throw 'no versionName in android/app/build.gradle' }
    $version = $Matches[1]
    $out = Join-Path $NativeDirectory 'newer'
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
    New-Item -ItemType Directory -Force $out | Out-Null
    $stripped = Join-Path $out 'libnewer.so'
    & $strip --strip-unneeded -o $stripped $lib
    if ($LASTEXITCODE -ne 0) { throw 'llvm-strip failed' }
    $name = "NSMBW-$version-newer-android-arm64.zip"
    $zip = Join-Path $out $name
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $stripped, 'libnewer.so',
            [IO.Compression.CompressionLevel]::Optimal)
    } finally {
        $archive.Dispose()
    }
    Remove-Item -LiteralPath $stripped
    $size = (Get-Item -LiteralPath $zip).Length
    $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $assetsDir = Join-Path $NativeDirectory 'assets'
    New-Item -ItemType Directory -Force $assetsDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $assetsDir 'newer-module.properties'),
        "file=$name`nsize=$size`nsha256=$sha`n", (New-Object Text.UTF8Encoding $false))
    '{0:N1} MB  {1}' -f ($size / 1MB), $zip
    return
}

$lib = Join-Path $BuildDirectory 'libmain.so'
if (-not (Test-Path -LiteralPath $lib)) { throw "libmain.so is missing: $lib" }
$libDir = Join-Path $NativeDirectory 'jniLibs\arm64-v8a'
New-Item -ItemType Directory -Force $libDir | Out-Null
$staged = Join-Path $libDir 'libmain.so'
# Only the dynamic symbol table is needed at run time; JNI and SDL_main are found through it.
& $strip --strip-unneeded -o $staged $lib
if ($LASTEXITCODE -ne 0) { throw 'llvm-strip failed' }
# Shared libraries the build made for libmain.so's own dependencies (aurora's libpng16.so). Android
# resolves them from the app's library folder when libmain.so loads; system libraries stay out.
$gpuHookNames = 'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so'
Get-ChildItem -LiteralPath $BuildDirectory -Recurse -Filter '*.so' |
    Where-Object { $_.Name -ne 'libmain.so' -and $_.Name -notin $gpuHookNames -and $_.FullName -notmatch '\\libadrenotools\\' } | ForEach-Object {
        & $strip --strip-unneeded -o (Join-Path $libDir $_.Name) $_.FullName
        if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed for $($_.Name)" }
        "bundled $($_.Name)"
    }
# libadrenotools and its hooks, for a picked GPU driver (GpuDrivers.java). They are assets, unpacked only
# when a driver is picked: as native libraries they would make Android extract every library at install.
$gpuHooks = Join-Path $NativeDirectory 'assets\gpu_hooks'
if (Test-Path -LiteralPath $gpuHooks) { Remove-Item -LiteralPath $gpuHooks -Recurse -Force }
New-Item -ItemType Directory -Force $gpuHooks | Out-Null
foreach ($name in $gpuHookNames) {
    $hook = Get-ChildItem -LiteralPath (Join-Path $BuildDirectory 'libadrenotools') -Recurse -Filter $name | Select-Object -First 1
    if (-not $hook) { throw "$name is missing from the build" }
    & $strip --strip-unneeded -o (Join-Path $gpuHooks $name) $hook.FullName
    if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed for $name" }
}

$assets = Join-Path $NativeDirectory 'assets\runtime'
if (Test-Path -LiteralPath $assets) { Remove-Item -LiteralPath $assets -Recurse -Force }
New-Item -ItemType Directory -Force $assets | Out-Null
# initial_pipeline_cache.db: NSMBW's pipelines (projects/nsmbw/tools/make_pipeline_seed.py), built
# at the first launch so effects don't stutter the first time they appear.
foreach ($file in 'dsp_coef.bin', 'cacert.pem', 'initial_pipeline_cache.db') {
    Copy-Item -LiteralPath (Join-Path $BuildDirectory $file) -Destination $assets
}
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'wii_bootstrap') -Destination $assets -Recurse

'{0:N1} MB  {1}' -f ((Get-Item $staged).Length / 1MB), $staged
Get-ChildItem $assets -Recurse -File | Measure-Object Length -Sum |
    ForEach-Object { '{0} runtime files, {1:N1} MB' -f $_.Count, ($_.Sum / 1MB) }
