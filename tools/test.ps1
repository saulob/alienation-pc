<#
.SYNOPSIS
Builds and runs the focused headless tests. No game, launcher or window starts.

.DESCRIPTION
The tests are not part of the default build; tools\build.ps1 must have configured build\cmake\clang64 first. The script tests in
tests\*.ps1 and tests\*.py are static and need no build; the build log checks use stand-in commands only. Every test
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
    'aln-skip-intro-tests'         = 'Skip Intro hook site and startup video tests'
    'aln-exit-game-tests'          = 'Exit Game bytecode patch, hook sites and row text'
    'aln-crash-guard-tests'        = 'crash reporting (isolated crashing child processes)'
    'aln-controller-prompts-tests' = 'controller prompt style, patch identity and glyph table'
    'aln-runtime-overlay-tests'    = 'guest file overlay tests'
    'aln-savedata-tests'           = 'save data commit, recovery and profile isolation'
    'aln-np-tests'                 = 'local NP identity, account checks and network state'
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
    & $python.Exe @($python.Args) '-I' (Join-Path $script:RepoRoot 'tests\controller_prompts_static_tests.py')
    if ($LASTEXITCODE -ne 0) { $failed.Add("controller_prompts_static_tests.py (exit code $LASTEXITCODE)") }
    & $python.Exe @($python.Args) '-I' (Join-Path $script:RepoRoot 'tools\prompts\verify_prompts.py')
    if ($LASTEXITCODE -ne 0) { $failed.Add("verify_prompts.py (exit code $LASTEXITCODE)") }
}

Write-Step 'script tests (build script and log contract, ANGLE dependency, online instance script; no build, no network)'
$hostExe = (Get-Process -Id $PID).Path
foreach ($scriptTest in 'build_script_tests.ps1', 'build_log_tests.ps1', 'angle_dependency_tests.ps1', 'online_instance_tests.ps1') {
    & $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $script:RepoRoot "tests\$scriptTest")
    if ($LASTEXITCODE -ne 0) { $failed.Add("$scriptTest (exit code $LASTEXITCODE)") }
}

Write-Step 'online backend tests (node --test: lobby protocol, rooms, builds, friends, relay, limits, dashboard; loopback only)'
$node = Get-Command node -ErrorAction SilentlyContinue
$nodeOk = $node -and ([version]((& $node.Source --version).TrimStart('v')) -ge [version]'22.13.0')
if (-not $nodeOk) { $failed.Add('online backend tests (Node.js 22.13 or later not found)') }
else {
    Push-Location (Join-Path $script:RepoRoot 'server\lobby')
    try { & $node.Source --no-warnings --test 'test/*.test.js' } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { $failed.Add("online backend tests (exit code $LASTEXITCODE)") }
}

Write-Step 'online HLE tests (classic Matching2 and the DGRAM_P2P socket against a loopback development lobby)'
& ninja -C $buildDir aln-online-hle-tests
if ($LASTEXITCODE -ne 0) { $failed.Add('aln-online-hle-tests (build failed)') }
elseif (-not $nodeOk) { $failed.Add('aln-online-hle-tests (Node.js 22.13 or later not found)') }
else {
    # The development lobby in memory on a free loopback port, in a folder this script owns; it never reaches the
    # production service.
    $lobbyDir = Join-Path ([IO.Path]::GetTempPath()) "aln-test-lobby-$PID"
    $lobbyProcess = $null
    try {
        New-Item -ItemType Directory -Force -Path $lobbyDir | Out-Null
        $probe = [System.Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $probe.Start(); $port = $probe.LocalEndpoint.Port; $probe.Stop()
        $server = Join-Path $script:RepoRoot 'server\lobby\local\server.js'
        $lobbyProcess = Start-Process -FilePath $node.Source -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $lobbyDir 'out.log') -RedirectStandardError (Join-Path $lobbyDir 'err.log') `
            -ArgumentList @('--no-warnings', "`"$server`"", '--host', '127.0.0.1', '--port', $port)
        $ready = $false
        for ($i = 0; $i -lt 80 -and -not $ready -and -not $lobbyProcess.HasExited; $i++) {
            Start-Sleep -Milliseconds 250
            try { $ready = "$((Invoke-RestMethod -Uri "http://127.0.0.1:$port/" -TimeoutSec 2).service)" -like 'alienation-lobby*' } catch { }
        }
        if (-not $ready) { $failed.Add("aln-online-hle-tests (the development lobby did not start: $(Get-Content -Raw -LiteralPath (Join-Path $lobbyDir 'err.log') -ErrorAction SilentlyContinue))") }
        else {
            $env:ALN_TEST_LOBBY = "ws://127.0.0.1:$port/"
            & (Join-Path $buildDir 'bin\aln-online-hle-tests.exe')
            if ($LASTEXITCODE -ne 0) { $failed.Add("aln-online-hle-tests (exit code $LASTEXITCODE)") }
        }
    } finally {
        $env:ALN_TEST_LOBBY = $null
        if ($lobbyProcess -and -not $lobbyProcess.HasExited) { $lobbyProcess.Kill(); $lobbyProcess.WaitForExit(5000) | Out-Null }
        if (Test-Path -LiteralPath $lobbyDir) { Remove-Item -LiteralPath $lobbyDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

if ($failed.Count) {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'All focused tests passed.' -ForegroundColor Green
