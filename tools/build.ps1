<#
.SYNOPSIS
  Builds alienation.exe and, on Windows, trg-updater.exe incrementally (MSYS2 CLANG64, CMake + Ninja). -Run launches it.
.DESCRIPTION
  Reuses what is current and updates only what is stale: the video core dependencies (gpu\deps.txt), the pinned ANGLE
  runtime (gpu\angle-runtime.txt, cached in cache\deps), the CMake configuration and the boot image of every
  installed build (build\image\<TITLE_ID>-<version>). Every DLL the game loads ends up beside it. The one build configuration,
  RelWithDebInfo (-O2 -g -DNDEBUG), is what is tested and what a package must contain; nothing else is built.
  The game is installed from the launcher (Play page, Install game...), as in a release.
.PARAMETER Run
  Launches the loader that was just built and stays attached until the game is closed. The launcher opens first
  when its rules say so (docs\LAUNCHER.md). Logs to build\logs\alienation_NNN.log.
.PARAMETER Clean
  Deletes build\cmake first (the installed game, the boot image and the logs are kept).
.PARAMETER CheckImports
  Diagnostic: with -Run, lists the PS4 imports the runtime lacks instead of running the game.
.PARAMETER TimeoutSeconds
  Diagnostic for automated tests only: with -Run, kills the loader after this many seconds (and skips the launcher).
.PARAMETER LoaderArgs
  Extra arguments for alienation.exe (with -Run).
.EXAMPLE
  .\tools\build.ps1
  .\tools\build.ps1 -Run
#>
[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$CheckImports,
    [int]$TimeoutSeconds = 0,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LoaderArgs = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')
$config = 'RelWithDebInfo'
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
$loader = Join-Path $buildDir 'bin\alienation.exe'
$markerName = '.aln_view.json'
$loadExecExit = 30 # sceSystemServiceLoadExec ended the process

# Controlled stops; a crash ends with its exception code, abort() with 3.
$stopExits = @{
    20 = 'STOP: the first unsupported PS4 import'
    21 = 'STOP: a runtime feature or limit the port does not have'
    22 = 'STOP: a guest fiber returned from its entry function'
    23 = 'STOP: a guest debug exception, unsupported unwinding or a video core assertion'
    24 = 'STOP: an Ajm decode request, which the port does not decode'
}

# The installed builds: game\prepared\<TITLE_ID>-<version> folders whose preparation finished.
function Get-PreparedBuilds {
    $prepared = Join-Path $root 'game\prepared'
    if (-not (Test-Path -LiteralPath $prepared)) { return @() }
    return @(Get-ChildItem -LiteralPath $prepared -Directory | Where-Object {
        $_.Name -match '^CUSA\d{5}-\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName $markerName))
    } | ForEach-Object { $_.Name })
}

function Test-Magic([string]$Path, [string]$Magic) {
    $bytes = New-Object byte[] 8
    $stream = [System.IO.File]::OpenRead($Path)
    try { $read = $stream.Read($bytes, 0, 8) } finally { $stream.Dispose() }
    return $read -eq 8 -and [System.Text.Encoding]::ASCII.GetString($bytes) -eq $Magic
}

# Rebuilt when the game was installed again, the boot image builder changed or the image has another format.
function Test-ImageCurrent([string]$View, [string]$Image) {
    $newest = (Get-Item -LiteralPath (Join-Path $View $markerName)).LastWriteTime
    foreach ($source in 'launcher\boot_image.cpp', 'launcher\game_profile.h', 'launcher\game_profile.cpp',
                        'src\aln_image_format.h') {
        $time = (Get-Item -LiteralPath (Join-Path $root $source)).LastWriteTime
        if ($time -gt $newest) { $newest = $time }
    }
    foreach ($name in 'boot.bin', 'content.bin', 'image.json') {
        $file = Join-Path $Image $name
        if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).LastWriteTime -lt $newest) { return $false }
    }
    $content = Join-Path $Image 'content.bin'
    return (Test-Magic (Join-Path $Image 'boot.bin') 'ALNBOOT1') -and (Test-Magic $content 'ALNCONT1') -and
        (Get-Item -LiteralPath $content).Length -eq 28
}

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
Write-Step "Toolchain clang64 ($($tc.Bin)), $config"

Sync-GpuDependencies
$angleRuntime = Sync-AngleRuntime $tc.Bin

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Step 'Cleaning build\cmake'
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}
$cache = Join-Path $buildDir 'CMakeCache.txt'
$configured = (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache) -and
    (Select-String -Path $cache -SimpleMatch "CMAKE_BUILD_TYPE:STRING=$config" -Quiet)
if (-not $configured) {
    Write-Step 'Configuring'
    & cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$config" '-DCMAKE_C_COMPILER=clang'
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'CMake configuration failed' }
}
Write-Step 'Building'
& ninja -C $buildDir alienation trg-updater
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Build failed' }
if (-not (Test-Path -LiteralPath $loader)) { Stop-Tool "Missing $loader" }
Install-GameRuntime $loader $angleRuntime $tc.Bin

$installed = @(Get-PreparedBuilds)
if ($installed.Count -eq 0) {
    Write-Warn 'The game is not installed yet: start the game and use Install game... on the launcher''s Play page.'
}
foreach ($folder in $installed) {
    $view = Join-Path $root "game\prepared\$folder"
    $image = Join-Path $root "build\image\$folder"
    if (Test-ImageCurrent $view $image) { continue }
    Write-Step "Updating the boot image of $folder"
    # The call operator waits for a Windows-subsystem program only when its output is piped.
    & $loader --prepare-image $view $image | Out-Host
    if ($LASTEXITCODE -ne 0) { Stop-Tool "Boot image build failed for $folder" }
}
Write-Host ''
Write-Host "Built $loader" -ForegroundColor Green
if (-not $Run) { return }

$running = @(Get-Process alienation -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $loader })
if ($running.Count -gt 0) {
    Write-Warn "alienation.exe is already running (PID $($running.Id -join ', ')); not launching another copy."
    return
}
# The loader finds the game, the boot image, user\ and build\logs by itself in this tree.
$arguments = @()
if ($CheckImports) { $arguments += '--check-imports' }
if ($TimeoutSeconds -gt 0) { $arguments += '--aln-skip-launcher' }
$arguments += $LoaderArgs
Push-Location (Split-Path $loader -Parent)
try {
    if ($TimeoutSeconds -gt 0) {
        $process = Start-Process -FilePath $loader -ArgumentList $arguments -NoNewWindow -PassThru
        $null = $process.Handle # keeps the exit code readable after the process ends
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Write-Warn "Stopping alienation.exe after $TimeoutSeconds seconds"
            $process.Kill()
            $process.WaitForExit()
        }
        $code = $process.ExitCode
    }
    else {
        # A Windows-subsystem program is not waited for by the call operator.
        $start = @{ FilePath = $loader; NoNewWindow = $true; Wait = $true; PassThru = $true }
        if ($arguments.Count -gt 0) { $start.ArgumentList = $arguments }
        $code = (Start-Process @start).ExitCode
    }
}
finally {
    Pop-Location
}
Write-Host "alienation.exe exit code $code"
if ($code -eq $loadExecExit) { Write-Host 'The game requested sceSystemServiceLoadExec (see the LoadExec lines above).' }
elseif ($stopExits.ContainsKey($code)) { Write-Host "$($stopExits[$code]) (see the STOP lines in the log)" }
elseif ($code -lt 0) { Write-Host ('CRASH: exception 0x{0:X8}; the report is in the log' -f $code) }
elseif ($code -eq 3) { Write-Host 'CRASH: abort(); the report is in the log' }
exit $code
