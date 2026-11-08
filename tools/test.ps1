<#
.SYNOPSIS
Builds and runs the focused headless tests. No game, launcher or window starts.

.DESCRIPTION
The tests are not part of the default build; tools\build.ps1 must have configured build\cmake\clang64 first. Every test
runs even after one fails; the exit code is non-zero when any of them failed.
#>
[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$buildDir = Join-Path $script:RepoRoot 'build\cmake\clang64'
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 first: it configures the build.' }

$tests = [ordered]@{
    'aln-memory-windows-tests'     = 'Windows memory tests (sparse backing, commitment, aliases)'
    'aln-log-tests'                = 'log tests (closed standard streams)'
    'aln-module-history-tests'     = 'crash report tests (module history, call sites, startup phase)'
    'aln-rtc-tests'                = 'libSceRtc tests through the import resolver'
    'aln-ime-keyboard-tests'       = 'libSceIme keyboard tests through the import resolver'
    'aln-json2-tests'              = 'libSceJson2 tests through the import resolver'
    'aln-thread-cancel-tests'      = 'thread cancellation tests'
    'aln-avplayer-thread-tests'    = 'AvPlayer thread join tests'
    'aln-image-format-tests'       = 'boot image and content profile contract'
    'aln-resolution-patch-tests'   = 'render resolution and frame rate patch tests'
    'aln-crash-guard-tests'        = 'crash reporting (isolated crashing child processes)'
    'aln-runtime-overlay-tests'    = 'guest file overlay tests'
    'aln-tiling-tests'             = 'GPU tiling tests'
    'aln-gds-lane-offset-tests'    = 'GPU GDS lane offset tests'
    'aln-gpu-crash-trace-tests'    = 'GPU crash trace and geometry output tests'
    'aln-gpu-recording-tests'      = 'GPU deferred command recording tests'
    'aln-renderer-selection-tests' = 'renderer selection tests'
    'aln-opengl-backend-tests'     = 'OpenGL backend tests (no context)'
    'aln-opengl-perf-tests'        = 'OpenGL write watch and stream tests (no context)'
    'aln-cursor-tests'             = 'cursor visibility tests'
    'aln-window-tests'             = 'window fit tests'
    'aln-sdl-compat-tests'         = 'SDL compatibility tests'
    'aln-present-tests'            = 'presentation tests'
    'aln-trophy-tests'             = 'trophy tests'
    'aln-incident-tests'           = 'bug report tests'
    'aln-update-tests'             = 'updater tests (no network)'
    'aln-update-helper-tests'      = 'update helper tests'
}

Sync-GpuDependencies

Write-Step 'Building the focused tests'
& ninja -C $buildDir @($tests.Keys)
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }

$failed = New-Object System.Collections.Generic.List[string]
foreach ($exe in $tests.Keys) {
    Write-Step $tests[$exe]
    & (Join-Path $buildDir "bin\$exe.exe")
    if ($LASTEXITCODE -ne 0) { $failed.Add("$exe (exit code $LASTEXITCODE)") }
}

Write-Step 'game profile tests (launcher and script build tables agree)'
$python = Get-Python
if (-not $python) { $failed.Add('game_profile_tests.py (Python 3 not found)') }
else {
    & $python.Exe @($python.Args) '-I' (Join-Path $script:RepoRoot 'tests\game_profile_tests.py')
    if ($LASTEXITCODE -ne 0) { $failed.Add("game_profile_tests.py (exit code $LASTEXITCODE)") }
    & $python.Exe @($python.Args) '-I' (Join-Path $script:RepoRoot 'tests\patch_site_tests.py')
    if ($LASTEXITCODE -ne 0) { $failed.Add("patch_site_tests.py (exit code $LASTEXITCODE)") }
}

if ($failed.Count) {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'All focused tests passed.' -ForegroundColor Green
