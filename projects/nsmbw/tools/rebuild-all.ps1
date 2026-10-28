# Re-run the NSMBW translation pipeline after runtime natives changed, then rebuild WiiCompiled.
# Mirrors Launcher/LocalBuild.ps1: translate-recursive -> emit-base-manifest -> generate-data-init ->
# emit-build-shards -> cmake --build. Ninja only recompiles shards whose content changed.
# -Newer <extracted Newer folder>: Newer Super Mario Bros. Wii instead (BUILDING.md section 7):
# newer_prelink.py writes ..\game-newer, recomp-newer.yml is translated, native-build-newer is built
# (configure it once with configure-native.ps1 -Newer). Both share generated\, so switching between
# them retranslates.
[CmdletBinding()]
param([int]$Threads = 10, [int]$Jobs = 10, [string]$Newer)

$ErrorActionPreference = 'Stop'
if ($Newer) { $Newer = (Resolve-Path -LiteralPath $Newer).Path }  # before Push-Location below
. (Join-Path $PSScriptRoot 'paths.ps1')
$ws = $NsmbwRepo
$logs = Get-NsmbwLogDirectory (Get-NsmbwBuildDirectory $(if ($Newer) { 'native-build-newer' } else { 'native-build' }))
# The toolchain's own .NET SDK if it has one (dotnet\), else the one on PATH.
$dotnetRoot = Join-Path (Get-NsmbwToolchain) 'dotnet'
if (Test-Path -LiteralPath (Join-Path $dotnetRoot 'dotnet.exe')) {
    $env:DOTNET_ROOT = $dotnetRoot
    $env:PATH = "$env:DOTNET_ROOT;$env:PATH"
} elseif (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw "No .NET 8 SDK: install it, or put it in $dotnetRoot"
}
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$translator = Join-Path $ws 'translator\src\Translator.Cli\bin\Release\net8.0\Translator.Cli.dll'
$project = if ($Newer) { 'projects/nsmbw/recomp-newer.yml' } else { 'projects/nsmbw/recomp.yml' }

function Invoke-Step([string]$Name, [string[]]$Arguments) {
    $log = Join-Path $logs "rebuild-$Name.log"
    $ErrorActionPreference = 'Continue'
    & dotnet $translator @Arguments 2>&1 | ForEach-Object { "$_" } | Out-File -LiteralPath $log -Encoding utf8
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { Get-Content $log -Tail 20; throw "translator step '$Name' failed ($code); see $log" }
    "$Name ok"
}

Push-Location $ws
try {
    # Stop any test instance so the link can replace WiiCompiled.exe.
    Get-Process WiiCompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$ws\*" } |
        ForEach-Object { Stop-Process -Id $_.Id -Force }
    if (-not (Test-Path -LiteralPath $translator)) {
        & dotnet build translator/Translator.sln -c Release -v quiet -nologo
        if ($LASTEXITCODE -ne 0) { throw "building the translator failed ($LASTEXITCODE)" }
    }
    if ($Newer) {
        # Newer's loader, done offline: its patches and its code linked into ..\game-newer.
        $log = Join-Path $logs 'rebuild-newer-prelink.log'
        $ErrorActionPreference = 'Continue'
        & python (Join-Path $ws 'projects\nsmbw\tools\newer_prelink.py') $Newer 2>&1 | ForEach-Object { "$_" } |
            Out-File -LiteralPath $log -Encoding utf8
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { Get-Content $log -Tail 20; throw "newer_prelink.py failed ($code); see $log" }
        'newer-prelink ok'
    }
    Invoke-Step 'translate' @('translate-recursive', '0x80004050', '--project', $project,
        '--outdir', 'generated/functions', '--output-metadata', 'generated/base_translation_output.json',
        '--production-source-bundle', 'generated/base_translation_sources.bin',
        '--no-function-files', '--prune-stale', '--threads', "$Threads")
    Invoke-Step 'manifest' @('emit-base-manifest', '--project', $project, '--out', 'build/base',
        '--functions-dir', 'generated/functions', '--translation-output-metadata',
        'generated/base_translation_output.json', '--region', 'E')
    Invoke-Step 'data-init' @('generate-data-init', '--project', $project)
    Invoke-Step 'shards' @('emit-build-shards', '--project', $project, '--base-metadata',
        'generated/base_translation_output.json', '--base-functions-dir', 'generated/functions',
        '--native-source-dir', 'runtime/src', '--out', 'generated/build_shards')
} finally {
    Pop-Location
}
& (Join-Path $PSScriptRoot 'build-native.ps1') -Target WiiCompiled -Jobs $Jobs -LogPath (Join-Path $logs 'rebuild-build.log') `
    -Newer:([bool]$Newer)
$code = $LASTEXITCODE
Get-Content (Join-Path $logs 'rebuild-build.log') | Select-String ' error' | Select-Object -First 10 | ForEach-Object Line
"build exit=$code"
exit $code
