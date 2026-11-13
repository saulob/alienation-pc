<#
.SYNOPSIS
The one pre-commit validation command: runs every headless suite and ends with one PASS, FAIL and NOT RUN summary. No
game, launcher window, controller, audio device, network or production service is used.

.DESCRIPTION
The suites are grouped in sections, each prints PASS, FAIL or NOT RUN, and a failing suite shows its real stdout and
stderr. Every suite runs even after one fails; the exit code is non-zero when any suite failed or a script or dependency
(the project .venv Python, Node.js with npm, the prepared game builds) is missing. Parts a suite skips are listed in the
summary.

Nothing is compiled or fetched by default. The C++ suites run the test binaries already in the build folder; a binary
that is missing, older than its sources or cannot be trusted fails its suite, with the exact command to make it current
(-Build). A C++ suite is never NOT RUN: NOT RUN is only for optional parts (PowerShell 7, the release artifacts without
-Release), does not fail the gate, and the last line says when anything did not run.

Local co-op (one to four controllers) runs on virtual SDL gamepads, online rooms of two to four players on a loopback
development lobby with modelled peers, the launcher's Accept and play on the launcher's own online client. Real
gameplay, real controllers, real network play and the visuals are still tested by hand.

The git state is never changed: the checks only read it, and the run fails if a suite leaves the tree different.

.PARAMETER Build
Builds the test targets first (incremental; the first build takes several minutes) and fetches the video core
dependencies in gpu\deps.txt when they are missing. This is the only mode that compiles or uses the network.

.PARAMETER BuildDir
Build folder of the C++ test binaries; default build\cmake\clang64 (tools\build.ps1 configures it). Without it the C++
suites fail.

.PARAMETER Packages
Folder with the player's real .pkg files (read only; also ALN_TEST_PACKAGES). Without it, Install game and Install
update run on synthetic packages only and the summary says so. A .pkg in game\ is used when this is not given.

.PARAMETER SkipGameData
Runs without the prepared game builds in game\prepared (the suites that read them skip those parts; the summary says so).

.PARAMETER Detail
Shows the output of passing suites too.

.PARAMETER List
Lists the suites by section and exits; nothing is built or run.

.PARAMETER Filter
Runs only the suites whose section, name or description contains this text. The run is marked as partial.

.PARAMETER Release
Also checks the release artifacts of the CMakeLists.txt version in dist\ (tools\package-release.ps1 makes them): the
Windows folder again with tools\package.ps1 -VerifyOnly, the ZIP against that folder, both .sha256 files and the
AppImage's header.
#>
[CmdletBinding()]
param(
    [string]$Packages = $env:ALN_TEST_PACKAGES,
    [switch]$SkipGameData,
    [switch]$Detail,
    [switch]$List,
    [string]$Filter = '',
    [switch]$Release,
    [switch]$Build,
    [string]$BuildDir = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot
$buildDir = if (-not $BuildDir) { Join-Path $root 'build\cmake\clang64' } elseif ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$binDir = Join-Path $buildDir 'bin'
$hostExe = (Get-Process -Id $PID).Path
$pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
$gameBuilds = 'CUSA00062-1.00', 'CUSA00062-1.06', 'CUSA04430-1.00'

# ---------------------------------------------------------------------------------------------------------------------
# The suites
# ---------------------------------------------------------------------------------------------------------------------
$suites = New-Object System.Collections.Generic.List[object]
function Add-Suite([string]$Section, [string]$Kind, [string]$Name, [string]$Description, [hashtable]$Options = @{}) {
    $suites.Add([pscustomobject]@{ Section = $Section; Kind = $Kind; Name = $Name; Description = $Description; Options = $Options })
}
$notRunSuites = New-Object System.Collections.Generic.List[object]
function Add-NotRun([string]$Section, [string]$Name, [string]$Reason) {
    $notRunSuites.Add([pscustomobject]@{ Section = $Section; Name = $Name; Status = 'NOT RUN'; Seconds = 0; Checks = 0; Summary = $Reason })
}
function Add-Exes([string]$Section, [object[]]$Rows) { foreach ($row in $Rows) { Add-Suite $Section 'exe' $row[0] $row[1] @{ MinChecks = $(if ($row.Count -gt 2) { $row[2] } else { 0 }) } } }

$s = 'Build and parity'
Add-Suite $s 'check' 'prepared game builds' 'game\prepared holds the three supported builds the static suites read' @{ Run = 'Test-GameData' }
Add-Suite $s 'python' 'tests\game_profile_tests.py' 'build fingerprints, regions, DUPLEX alias, save and online identity: launcher and script tables agree'
Add-Suite $s 'python' 'tests\feature_parity_tests.py' 'feature matrix of the four builds against patch tables, tests and docs; Local co-op only where it exists'
Add-Suite $s 'python' 'tests\patch_site_tests.py' 'patch sites, byte fingerprints and overlap against the real executables and stream.dat'
Add-Exes $s @(, @('aln-image-format-tests', 'boot image and content profile contract'))
$scriptTests = 'build_script_tests.ps1', 'build_log_tests.ps1', 'angle_dependency_tests.ps1'
foreach ($name in $scriptTests) { Add-Suite $s 'script' "tests\$name" 'build script, log and ANGLE dependency contracts (stand-in commands, no network)' @{ Host = $hostExe } }
if ($pwsh -and $pwsh.Source -ne $hostExe) {
    foreach ($name in $scriptTests) { Add-Suite $s 'script' "tests\$name" 'the same under PowerShell 7' @{ Host = $pwsh.Source; Label = ' (PowerShell 7)' } }
} elseif (-not $pwsh) {
    Add-NotRun $s 'build script tests under PowerShell 7' 'PowerShell 7 (pwsh) is not installed: winget install Microsoft.PowerShell'
}

$s = 'Core runtime'
Add-Exes $s @(
    , @('aln-log-tests', 'log with closed standard streams')
    , @('aln-module-history-tests', 'crash report: module history, call sites, startup phase')
    , @('aln-crash-guard-tests', 'crash reporting in isolated crashing child processes')
    , @('aln-rtc-tests', 'libSceRtc through the import resolver')
    , @('aln-ime-keyboard-tests', 'libSceIme keyboard through the import resolver')
    , @('aln-json2-tests', 'libSceJson2 through the import resolver')
    , @('aln-thread-cancel-tests', 'thread cancellation')
    , @('aln-avplayer-thread-tests', 'AvPlayer thread join')
    , @('aln-runtime-overlay-tests', 'guest file overlay')
    , @('aln-sdl-compat-tests', 'SDL compatibility')
    , @('aln-trophy-tests', 'trophies')
    , @('aln-incident-tests', 'bug reports and incident report')
    , @('aln-paths-tests', 'data folder beside the program (Linux: beside the AppImage, XDG fallback)')
)

$s = 'Memory'
Add-Exes $s @(, @('aln-memory-windows-tests', 'Windows guest memory: SEC_RESERVE, commit, 1455 fallback, aliases, protections, GPU tracking, rollback, multi-process', 200))

$s = 'Graphics patches'
Add-Exes $s @(
    , @('aln-resolution-patch-tests', 'render resolution, aspect and frame rate patches per build')
    , @('aln-tiling-tests', 'GPU tiling')
    , @('aln-gds-lane-offset-tests', 'GPU GDS lane offset')
    , @('aln-gpu-crash-trace-tests', 'GPU crash trace and geometry output')
    , @('aln-gpu-recording-tests', 'GPU deferred command recording')
    , @('aln-renderer-selection-tests', 'renderer selection')
    , @('aln-opengl-backend-tests', 'OpenGL backend (no context)')
    , @('aln-opengl-perf-tests', 'OpenGL write watch and stream (no context)')
    , @('aln-cursor-tests', 'cursor visibility')
    , @('aln-window-tests', 'window fit')
    , @('aln-present-tests', 'presentation')
    , @('aln-frame-pacing-tests', 'Frame rate: presentation pacing, game speed and network work equal at every rate')
)
Add-Suite $s 'python' 'tests\display_aspect_tests.py' 'display size, aspect and ultrawide sites against the real executables'

$s = 'Controller and UI'
Add-Exes $s @(
    , @('aln-controller-prompts-tests', 'prompt style, patch identity and glyph table')
    , @('aln-menu-buttons-tests', 'Asian Menu Buttons: Confirm and Cancel per build')
)
Add-Suite $s 'python' 'tests\controller_prompts_static_tests.py' 'Xbox and PlayStation prompt data, mapping and callout scenes'
Add-Suite $s 'python' 'tests\prompt_language_tests.py' 'prompt glyph atlases and font cells per language and build'
Add-Suite $s 'python' 'tools\prompts\verify_prompts.py' 'generated controller artwork, mip chain and callout anchors'

$s = 'Game patches'
Add-Exes $s @(
    , @('aln-skip-intro-tests', 'Skip Intro: hook sites, state logic, input contract, startup video')
    , @('aln-exit-game-tests', 'Exit Game: bytecode patch, hook sites, localized rows')
    , @('aln-exit-game-menu-tests', 'Exit Game rows in the real menus of each prepared build')
    , @('aln-text-fix-tests', 'ASIA/KR Portuguese Voice-Over Language text fix')
    , @('aln-online-status-tests', 'network status box: states, colours, persistent text, texts per language, script patch per build')
)

$s = 'Save data'
Add-Exes $s @(, @('aln-savedata-tests', 'save create, overwrite, recovery, profile isolation, four local users mounted at once', 100))

$s = 'Local multiplayer'
Add-Exes $s @(, @('aln-local-pad-tests', 'one to four virtual gamepads: user and pad mapping, join order, disconnect, reconnect, slot reuse, fifth refused', 200))

$s = 'Online multiplayer'
Add-Exes $s @(
    , @('aln-np-tests', 'local NP identity, account checks and network state')
    , @('aln-online-view-tests', 'launcher Online page model: connection status, friends, invitations, public rooms')
)
Add-Suite $s 'script' 'tests\online_instance_tests.ps1' 'online instance script (static)' @{ Host = $hostExe }
if ($pwsh -and $pwsh.Source -ne $hostExe) {
    Add-Suite $s 'script' 'tests\online_instance_tests.ps1' 'the same under PowerShell 7 (long path scenario)' @{ Host = $pwsh.Source; Label = ' (PowerShell 7)' }
} elseif (-not $pwsh) {
    Add-NotRun $s 'online instance script under PowerShell 7' 'PowerShell 7 (pwsh) is not installed: winget install Microsoft.PowerShell'
}
Add-Suite $s 'hle' 'aln-online-hle-tests' 'Matching2 rooms of 2, 3 and 4 players with a full mesh of peers, four-player invitations, launcher Accept and play, sessions, relay and DGRAM_P2P on a loopback lobby'

$s = 'Online Worker'
Add-Suite $s 'node' 'server\lobby (npm test)' 'the whole Worker suite: protocol, friends, rooms, sessions, relay, edge and ingress abuse protection, hibernation, write budgets, diagnostics, admin data (in memory, no network)'
Add-Suite $s 'check' 'Worker protection wiring' 'every abuse-protection test file exists and npm test runs them; limiter binding, unique namespace, ping auto-response and edge gate in the sources' @{ Run = 'Test-WorkerWiring' }

$s = 'Packaging and install'
Add-Suite $s 'exe' 'aln-game-install-tests' 'Install game and Install update: EUR base and 1.06, DUPLEX repack, ASIA/KR base, refusals' @{ Packages = $true }
Add-Suite $s 'exe' 'aln-update-tests' 'updater (no network)'
Add-Suite $s 'exe' 'aln-update-network-tests' 'updater on a loopback server: no release yet (404), failures, stalls, redirects, retries, production address and asset names'
Add-Suite $s 'exe' 'aln-update-helper-tests' 'update helper'
Add-Suite $s 'python' 'tests\release_contract_tests.py' 'one version source, updater address and asset names, desktop entry, AppStream metadata, packager lists, LGPL FFmpeg pin, release notes'
Add-Suite $s 'script' 'tools\test-release-check.ps1' 'release checker: a clean folder and ZIP pass; game data, player data, logs, updater leftovers, debug markers and local paths are refused' @{ Host = $hostExe }
Add-Suite $s 'script' 'tests\release_inputs_tests.ps1' 'reviewed AppImage runtime: the pinned runtime from the cache or an import, never another file; same pins and patch as the sibling ports' @{ Host = $hostExe }

if ($Release) {
    $s = 'Release artifacts'
    Add-Suite $s 'check' 'Windows package' 'dist\ folder verified again, ZIP entries equal to it, .zip.sha256 matches' @{ Run = 'Test-WindowsRelease' }
    Add-Suite $s 'check' 'Linux AppImage' 'AppImage type 2 header, .AppImage.sha256 matches, symbols kept outside' @{ Run = 'Test-LinuxRelease' }
} else {
    Add-NotRun 'Release artifacts' 'Windows package and Linux AppImage in dist' 'run tools\package-release.ps1, then tools\test.ps1 -Release'
}

$s = 'Final static checks'
Add-Suite $s 'script' 'tests\gate_tests.ps1' 'this script: parses, derives every path from the repository, runs from any folder, propagates a failing npm test, reports a missing build folder' @{ Host = $hostExe }
Add-Suite $s 'check' 'repository hygiene' 'no markers, local paths, secrets, logs, saves, game data or build output in tracked or new files' @{ Run = 'Test-Hygiene' }
Add-Suite $s 'check' 'every test is wired in' 'each CMake test target, test script and test source is run by this script or excluded with a reason' @{ Run = 'Test-Coverage' }
Add-Suite $s 'check' 'git diff --check' 'no whitespace errors or conflict markers in the changes, staged or not, nor in new files' @{ Run = 'Test-GitWhitespace' }
Add-Suite $s 'check' 'working tree unchanged' 'the run changed no tracked or untracked file and no staged change' @{ Run = 'Test-TreeUnchanged' }

# Test targets the CMake generator may define that this Windows script deliberately does not run.
$excludedTargets = @{
    'aln-runtime-tests' = 'Linux only (memory and fiber layers); the Windows counterpart is aln-memory-windows-tests'
}

$allSuites = $suites.ToArray()
if ($Filter) {
    $suites = New-Object System.Collections.Generic.List[object] (, @($suites | Where-Object { "$($_.Section) $($_.Name) $($_.Description)" -like "*$Filter*" }))
}

if ($List) {
    foreach ($group in ($suites | Group-Object Section)) {
        Write-Host "==> $($group.Name)" -ForegroundColor Cyan
        foreach ($suite in $group.Group) { Write-Host ('  {0,-44} {1}' -f ($suite.Name + $suite.Options['Label']), $suite.Description) }
    }
    Write-Host ("{0} suites" -f $suites.Count)
    exit 0
}

# ---------------------------------------------------------------------------------------------------------------------
# Running things
# ---------------------------------------------------------------------------------------------------------------------
$tempDir = Join-Path ([IO.Path]::GetTempPath()) "aln-test-$PID"

function ConvertTo-CmdArg([string]$Text) {
    if ($Text -eq '' -or $Text -match '[\s"&|<>^()]') { return '"' + ($Text -replace '"', '""') + '"' }
    return $Text
}

# Runs a program with stdout and stderr merged in order into a file, a timeout and no window; returns the exit code and lines.
function Invoke-Process([string]$File, [string[]]$Arguments = @(), [string]$WorkDir = $root, [int]$TimeoutSec = 900) {
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    $out = Join-Path $tempDir ('out-{0}.txt' -f [guid]::NewGuid().ToString('N'))
    $command = (@($File) + $Arguments | ForEach-Object { ConvertTo-CmdArg $_ }) -join ' '
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    $info.Arguments = '/d /s /c "{0} > "{1}" 2>&1"' -f $command, $out
    $info.WorkingDirectory = $WorkDir
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $timedOut = -not $process.WaitForExit($TimeoutSec * 1000)
    if ($timedOut) {
        & taskkill.exe /PID $process.Id /T /F 2>&1 | Out-Null
        $process.WaitForExit()
    }
    $code = $process.ExitCode
    $process.Dispose()
    $lines = @()
    if (Test-Path -LiteralPath $out) {
        $text = [IO.File]::ReadAllText($out, [Text.Encoding]::UTF8)
        if ($text.Length -gt 0) { $lines = @($text -split "\r?\n") }
        if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = @($lines[0..($lines.Count - 2)]) }
        Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    }
    if ($timedOut) { $lines += "TIMEOUT: stopped after $TimeoutSec s"; $code = 124 }
    return [pscustomobject]@{ Code = $code; Lines = $lines }
}

function New-Result([int]$Code, [string[]]$Lines) { return [pscustomobject]@{ Code = $Code; Lines = @($Lines) } }

$script:VenvPython = $false
function Get-VenvPythonOrNull {
    if ($script:VenvPython -eq $false) {
        $script:VenvPython = $null
        $python = Get-VenvPython
        if ($python) {
            $statement = 'import ' + (@($script:PythonPackages.Keys) -join ', ')
            $probe = Invoke-Process $python @('-I', '-c', $statement) $root 120
            if ($probe.Code -eq 0) { $script:VenvPython = $python }
        }
    }
    return $script:VenvPython
}

function Get-GitLines([string[]]$Arguments) {
    $result = Invoke-Process 'git' (@('-C', $root) + $Arguments) $root 120
    return $result
}

# --- checks --------------------------------------------------------------------------------------------------------

function Test-GameData {
    if ($SkipGameData) { return New-Result 0 @('skipped: -SkipGameData, the suites that read game\prepared run without the real builds') }
    $missing = @()
    foreach ($build in $gameBuilds) {
        foreach ($file in 'eboot.bin', 'stream.dat') {
            if (-not (Test-Path -LiteralPath (Join-Path $root "game\prepared\$build\$file") -PathType Leaf)) { $missing += "game\prepared\$build\$file" }
        }
    }
    if ($missing) {
        return New-Result 1 (@('The prepared game builds these suites read are not installed:') + ($missing | ForEach-Object { "  $_" }) +
            'Install the games from the launcher (Install game...), or run with -SkipGameData to run without them.')
    }
    return New-Result 0 @("$($gameBuilds.Count) builds prepared")
}

function Test-Coverage {
    $problems = New-Object System.Collections.Generic.List[string]
    $runTargets = @($allSuites | Where-Object { $_.Kind -in 'exe', 'hle' } | ForEach-Object { $_.Name })
    $known = @()
    $skipped = @()
    if ($script:NinjaReady) {
        $targets = Invoke-Process 'ninja' @('-C', $buildDir, '-t', 'targets', 'all') $root 120
        if ($targets.Code -ne 0) { return New-Result 1 (@('ninja could not list the targets:') + $targets.Lines) }
        $known = @($targets.Lines | ForEach-Object { if ($_ -match '^(aln-[A-Za-z0-9-]+-tests):') { $Matches[1] } } | Sort-Object -Unique)
    } else {
        $skipped = @('skipped: no configured build folder or toolchain, so the CMake test targets were not compared (tools\build.ps1 configures it)')
    }
    foreach ($target in $known) {
        if ($runTargets -notcontains $target -and -not $excludedTargets.ContainsKey($target)) {
            $problems.Add("test target $target is not run by tools\test.ps1 (add it, or exclude it with a reason)")
        }
    }
    $listed = @($allSuites | ForEach-Object { $_.Name })
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $root 'tests') -File | Where-Object { $_.Extension -in '.py', '.ps1' })) {
        if ($listed -notcontains "tests\$($file.Name)") { $problems.Add("tests\$($file.Name) is not run by tools\test.ps1") }
    }
    $cmake = (Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')) + (Get-Content -Raw -LiteralPath (Join-Path $root 'launcher\CMakeLists.txt'))
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $root 'tests') -File | Where-Object { $_.Name -match '_tests\.(c|cpp)$' })) {
        if (-not $cmake.Contains("tests/$($file.Name)")) { $problems.Add("tests\$($file.Name) belongs to no CMake test target") }
    }
    if ($problems.Count) { return New-Result 1 @($problems) }
    $excluded = @($known | Where-Object { $excludedTargets.ContainsKey($_) })
    $summary = if ($known.Count) { "$($known.Count - $excluded.Count) CMake test targets, all run" } else { 'test scripts and sources all run' }
    return New-Result 0 (@($summary) + $skipped + ($excluded | ForEach-Object { "excluded $_ : $($excludedTargets[$_])" }))
}

function Test-WorkerWiring {
    $lobby = Join-Path $root 'server\lobby'
    $problems = New-Object System.Collections.Generic.List[string]
    $package = Get-Content -Raw -LiteralPath (Join-Path $lobby 'package.json') | ConvertFrom-Json
    if ($package.scripts.test -notmatch 'test/\*\.test\.js') { $problems.Add('server\lobby\package.json: "test" no longer runs test/*.test.js, so new test files would be skipped') }
    foreach ($name in 'abuse', 'ingress', 'worker', 'relay', 'lobby', 'sessions', 'write_budget', 'metrics', 'public_lobby') {
        if (-not (Test-Path -LiteralPath (Join-Path $lobby "test\$name.test.js") -PathType Leaf)) { $problems.Add("server\lobby\test\$name.test.js is missing") }
    }
    $toml = Get-Content -Raw -LiteralPath (Join-Path $lobby 'wrangler.toml')
    $namespace = $null
    if ($toml -notmatch 'name = "WS_UPGRADE_LIMITER"') { $problems.Add('wrangler.toml: the WS_UPGRADE_LIMITER rate limit binding is missing') }
    if ($toml -notmatch 'simple = \{ limit = 30, period = 60 \}') { $problems.Add('wrangler.toml: the upgrade limit is not 30 per 60 seconds') }
    if ($toml -match 'namespace_id = "(\d+)"') { $namespace = $Matches[1] } else { $problems.Add('wrangler.toml: the rate limit has no namespace_id') }
    if ($namespace -in '1001', '2002', '3003') { $problems.Add("wrangler.toml: namespace_id $namespace belongs to another project's lobby") }
    $worker = Get-Content -Raw -LiteralPath (Join-Path $lobby 'src\worker.js')
    if (-not $worker.Contains("new WebSocketRequestResponsePair('ping', 'pong')")) { $problems.Add('worker.js: the ping/pong Hibernation auto-response is missing') }
    if (-not $worker.Contains('handleUpgrade(request, env, stub)')) { $problems.Add('worker.js: upgrades no longer pass the edge limiter') }
    if ($problems.Count) { return New-Result 1 @($problems) }
    return New-Result 0 @("limiter 30 per 60 s, namespace $namespace, ping auto-response, edge gate and 9 protection test files present")
}

# Files that name the markers and paths on purpose: the release checkers and their tests, and docs of what they refuse.
$hygieneNamedBy = 'docs/LINUX.md', 'docs/release.md', 'tests/release_contract_tests.py', 'tools/check-release-folder.ps1',
    'tools/package-linux.sh', 'tools/test-release-check.ps1', 'tools/test.ps1', 'tests/gate_tests.ps1'

function Test-Hygiene {
    $problems = New-Object System.Collections.Generic.List[string]
    $tracked = @((Get-GitLines @('ls-files')).Lines | Where-Object { $_ })
    $untracked = @((Get-GitLines @('ls-files', '--others', '--exclude-standard')).Lines | Where-Object { $_ })
    $files = @($tracked + $untracked | Sort-Object -Unique)
    if (-not $files.Count) { return New-Result 1 @('git listed no files: is this a git checkout?') }
    $forbidden = '(?i)^(game|user|cache|logs|build|dist|out)/|\.(log|pkg|prx|sprx|self|elf|cpk|psarc|trp|pem|pfx|sav)$|(^|/)(eboot\.bin|\.env|\.dev\.vars|stream\.dat)$|(^|/)(node_modules|\.wrangler)/'
    foreach ($relative in $files) { if ($relative -match $forbidden) { $problems.Add("${relative}: game data, logs, saves, build output, secrets or tool state do not belong in the repository") } }
    $textual = '.c', '.h', '.cpp', '.inc', '.py', '.ps1', '.sh', '.js', '.mjs', '.md', '.txt', '.json', '.cmake', '.toml', '.yml', '.in', '.desktop', '.xml'
    $markers = 'DEBUG_SAULO', 'PRIVATE KEY-----'
    $absolute = '(?i)\b[A-Z]:\\(Users|VisualStudio)\\'
    $secret = 'ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{20,}|sk-[A-Za-z0-9]{32,}'
    $checked = 0
    foreach ($relative in $files) {
        if ($textual -notcontains [IO.Path]::GetExtension($relative).ToLowerInvariant()) { continue }
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $checked++
        $named = $hygieneNamedBy -contains $relative
        $number = 0
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            $number++
            if (-not $named) {
                foreach ($marker in $markers) { if ($line.Contains($marker)) { $problems.Add("${relative}:${number}: $marker") } }
                if ($line -match $absolute) { $problems.Add("${relative}:${number}: a path of one machine: $($Matches[0])") }
            }
            if ($line -match $secret) { $problems.Add("${relative}:${number}: looks like a credential") }
        }
    }
    if ($problems.Count) { return New-Result 1 @($problems | Select-Object -First 40) }
    return New-Result 0 @("$checked text files and $($files.Count) paths clean")
}

function Get-ReleaseName {
    $cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')
    if ($cmake -notmatch '(?m)^project\(alienation_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { return $null }
    return "AlienationPC-v$($Matches[1])"
}

function Test-ChecksumFile([string]$Artifact) {
    $file = "$Artifact.sha256"
    if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) { return "missing $Artifact" }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return "missing $file" }
    $line = Get-Content -LiteralPath $file -TotalCount 1
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$' -or $Matches[2] -ne (Split-Path $Artifact -Leaf)) { return "$file is not '<sha256>  <name>'" }
    $expected = $Matches[1]
    if ((Get-FileHash -LiteralPath $Artifact -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { return "$file does not match" }
    return $null
}

function Test-WindowsRelease {
    $name = Get-ReleaseName
    if (-not $name) { return New-Result 1 @('project VERSION not found in CMakeLists.txt') }
    $dist = Join-Path $root 'dist'
    $folder = Join-Path $dist "$name-windows-x64"
    $zip = "$folder.zip"
    $lines = New-Object System.Collections.Generic.List[string]
    $problem = Test-ChecksumFile $zip
    if ($problem) { return New-Result 1 @($problem, 'run tools\package-release.ps1 (or tools\package.ps1) first') }
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { return New-Result 1 @("missing $folder") }
    $verify = Invoke-Process $hostExe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\package.ps1'), '-VerifyOnly', $folder) $root 600
    $verify.Lines | ForEach-Object { $lines.Add($_) }
    if ($verify.Code -ne 0) { return New-Result 1 @($lines) }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try { $entries = @($archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName } | Sort-Object) } finally { $archive.Dispose() }
    $staged = @(Get-ChildItem -LiteralPath $folder -Recurse -File | ForEach-Object { "$name-windows-x64/" + $_.FullName.Substring($folder.Length + 1).Replace('\', '/') } | Sort-Object)
    if (Compare-Object $entries $staged) { return New-Result 1 @($lines + 'the ZIP does not hold exactly the verified folder') }
    if (@($entries | Where-Object { $_ -match '(?i)\.(pdb|debug)$|symbols/' }).Count) { return New-Result 1 @($lines + 'the ZIP holds debug symbols') }
    $lines.Add("$($entries.Count) files in $(Split-Path $zip -Leaf), checksum matches")
    return New-Result 0 @($lines)
}

function Test-LinuxRelease {
    $name = Get-ReleaseName
    if (-not $name) { return New-Result 1 @('project VERSION not found in CMakeLists.txt') }
    $image = Join-Path $root "dist\$name-x86_64.AppImage"
    $problem = Test-ChecksumFile $image
    if ($problem) { return New-Result 1 @($problem, 'run tools\package-release.ps1 first') }
    $bytes = New-Object byte[] 11
    $stream = [IO.File]::OpenRead($image)
    try { [void]$stream.Read($bytes, 0, 11) } finally { $stream.Dispose() }
    if (-not ($bytes[0] -eq 0x7f -and $bytes[1] -eq 0x45 -and $bytes[8] -eq 0x41 -and $bytes[9] -eq 0x49 -and $bytes[10] -eq 2)) {
        return New-Result 1 @("$(Split-Path $image -Leaf) is not a type 2 AppImage")
    }
    return New-Result 0 @("$(Split-Path $image -Leaf): type 2 AppImage, $([math]::Round((Get-Item -LiteralPath $image).Length / 1MB, 1)) MB, checksum matches")
}

function Test-GitWhitespace {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return New-Result 1 @('git was not found: it is needed for git diff --check') }
    $lines = New-Object System.Collections.Generic.List[string]
    $code = 0
    foreach ($arguments in @('diff', '--check'), @('diff', '--cached', '--check')) {
        $result = Get-GitLines $arguments
        if ($result.Code -ne 0) { $code = 1; $lines.Add("git $($arguments -join ' ') reported problems:"); $result.Lines | ForEach-Object { $lines.Add($_) } }
    }
    $untracked = Get-GitLines @('ls-files', '--others', '--exclude-standard')
    $textual = '.c', '.h', '.cpp', '.inc', '.py', '.ps1', '.sh', '.js', '.mjs', '.md', '.txt', '.json', '.cmake', '.toml', '.yml'
    $checked = 0
    foreach ($relative in @($untracked.Lines | Where-Object { $_ })) {
        if ($textual -notcontains [IO.Path]::GetExtension($relative).ToLowerInvariant()) { continue }
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $checked++
        $number = 0
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            $number++
            if ($line -match '[ \t]$') { $code = 1; $lines.Add("${relative}:${number}: trailing whitespace (new file)") }
            elseif ($line -match '^(<<<<<<<|>>>>>>>) ') { $code = 1; $lines.Add("${relative}:${number}: conflict marker (new file)") }
        }
    }
    if ($code -eq 0) { $lines.Add("clean, plus $checked new text files") }
    return New-Result $code @($lines)
}

function Get-TreeFingerprint {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($arguments in @('diff'), @('diff', '--cached'), @('ls-files', '--others', '--exclude-standard'), @('ls-files', '--stage')) {
        $parts.Add((Get-GitLines $arguments).Lines -join "`n")
    }
    $untracked = @((Get-GitLines @('ls-files', '--others', '--exclude-standard')).Lines | Where-Object { $_ })
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($relative in $untracked) {
            $path = Join-Path $root $relative
            if (Test-Path -LiteralPath $path -PathType Leaf) { $parts.Add($relative + ':' + [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($path)))) }
        }
        return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($parts -join "`n"))))
    } finally { $sha.Dispose() }
}

function Test-TreeUnchanged {
    $after = Get-TreeFingerprint
    if ($after -ne $script:TreeBefore) { return New-Result 1 @('a suite changed tracked, staged or untracked files: compare git status and git diff with the state before the run') }
    return New-Result 0 @('git state identical before and after')
}

# --- the dependency-bearing kinds ----------------------------------------------------------------------------------

function Start-DevLobby {
    $node = Get-Command node -ErrorAction SilentlyContinue
    $lobby = [pscustomobject]@{ Process = $null; Port = 0; Dir = (Join-Path $tempDir 'lobby'); Error = $null; Node = $node }
    New-Item -ItemType Directory -Force -Path $lobby.Dir | Out-Null
    $probe = [System.Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $probe.Start(); $lobby.Port = $probe.LocalEndpoint.Port; $probe.Stop()
    $server = Join-Path $root 'server\lobby\local\server.js'
    $lobby.Process = Start-Process -FilePath $node.Source -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $lobby.Dir 'out.log') -RedirectStandardError (Join-Path $lobby.Dir 'err.log') `
        -ArgumentList @('--no-warnings', "`"$server`"", '--host', '127.0.0.1', '--port', $lobby.Port)
    $ready = $false
    for ($i = 0; $i -lt 80 -and -not $ready -and -not $lobby.Process.HasExited; $i++) {
        Start-Sleep -Milliseconds 250
        try { $ready = "$((Invoke-RestMethod -Uri "http://127.0.0.1:$($lobby.Port)/" -TimeoutSec 2).service)" -like 'alienation-lobby*' } catch { }
    }
    if (-not $ready) { $lobby.Error = "the development lobby did not start: $(Get-Content -Raw -LiteralPath (Join-Path $lobby.Dir 'err.log') -ErrorAction SilentlyContinue)" }
    return $lobby
}

function Stop-DevLobby($Lobby) {
    if ($Lobby.Process -and -not $Lobby.Process.HasExited) { $Lobby.Process.Kill(); $Lobby.Process.WaitForExit(5000) | Out-Null }
}

function Get-NodeProblem([switch]$NeedNpm) {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { return 'Node.js 22.13 or later was not found. Install it (winget install OpenJS.NodeJS.LTS) and open a new terminal.' }
    if ($NeedNpm -and -not (Get-Command npm -ErrorAction SilentlyContinue)) { return 'npm was not found next to Node.js. Reinstall Node.js (winget install OpenJS.NodeJS.LTS) and open a new terminal.' }
    $version = ((& $node.Source --version) -replace '^v', '').Trim()
    if ([version]$version -lt [version]'22.13.0') { return "Node.js $version is older than 22.13. Update it (winget upgrade OpenJS.NodeJS.LTS)." }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'server\lobby\package.json'))) { return 'server\lobby\package.json is missing.' }
    return $null
}

# Test targets ninja would rebuild; nothing is compiled. A dry run stops at an unfinished manifest check (the
# CONFIGURE_DEPENDS globs make it always dirty) and plans nothing, so the manifest is brought up to date first (a real
# check that only reruns CMake when the globs or CMake files changed) and the plan is read from a copy of it that no
# edge regenerates. StaleKnown stays false when no plan could be made.
function Get-StaleTargets([string[]]$Targets) {
    $script:StaleKnown = $false
    if (-not $script:NinjaReady -or -not $Targets.Count) { return @() }
    $manifest = Invoke-Process 'ninja' @('-C', $buildDir, 'build.ninja') $root 600
    if ($manifest.Code -ne 0) { return @() }
    $planFile = Join-Path $buildDir 'build.plan.ninja'
    try {
        Copy-Item -LiteralPath (Join-Path $buildDir 'build.ninja') -Destination $planFile -Force
        $plan = Invoke-Process 'ninja' (@('-C', $buildDir, '-f', 'build.plan.ninja', '-n', '-k', '0') + $Targets) $root 300
    } finally {
        Remove-Item -LiteralPath $planFile -Force -ErrorAction SilentlyContinue
    }
    if ($plan.Code -ne 0 -or @($plan.Lines | Where-Object { $_ -match 'Re-running CMake' }).Count) { return @() }
    $script:StaleKnown = $true
    return @($plan.Lines | Where-Object { $_ -match '^\[\d+/\d+\]' } | ForEach-Object { [regex]::Matches($_, 'aln-[A-Za-z0-9-]+-tests') } | ForEach-Object { $_.Value } | Sort-Object -Unique)
}

function Get-BinaryProblem([string]$Name) {
    $exe = Join-Path $binDir "$Name.exe"
    $how = if ($Build) { 'the target did not build; see the build failure above' } else { 'build it with: .\tools\test.ps1 -Build' }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { return "no test binary $exe ($how)" }
    if (-not $Build -and -not $script:StaleKnown) { return "the build folder is out of date (CMake has to run again) or ninja could not plan it, so the binary cannot be trusted ($how)" }
    if (-not $Build -and $script:StaleTargets -contains $Name) { return "the binary is older than its sources ($how)" }
    return $null
}

function Invoke-SuiteRun($Suite) {
    switch ($Suite.Kind) {
        'check' { return & $Suite.Options['Run'] }
        'exe' {
            $exe = Join-Path $binDir "$($Suite.Name).exe"
            $unavailable = Get-BinaryProblem $Suite.Name
            if ($unavailable) { return New-Result 127 @($unavailable) }
            if ($Suite.Options['Packages']) {
                $folder = $Packages
                if (-not $folder -and (Test-Path -LiteralPath (Join-Path $root 'game') -PathType Container) -and
                    @(Get-ChildItem -LiteralPath (Join-Path $root 'game') -Filter '*.pkg' -File -ErrorAction SilentlyContinue).Count) { $folder = Join-Path $root 'game' }
                if ($folder -and -not (Test-Path -LiteralPath $folder -PathType Container)) { return New-Result 1 @("the real package folder does not exist: $folder") }
                $previous = $env:ALN_TEST_PACKAGES
                $env:ALN_TEST_PACKAGES = $folder
                try { return Invoke-Process $exe @() $root }
                finally { $env:ALN_TEST_PACKAGES = $previous }
            }
            return Invoke-Process $exe @() $root
        }
        'python' {
            $python = Get-VenvPythonOrNull
            if (-not $python) {
                return New-Result 1 @("the project Python environment is missing or incomplete (.venv with $((@($script:PythonPackages.Values)) -join ', ')); run tools\setup.ps1 -InstallPython")
            }
            $script = Join-Path $root $Suite.Name
            if (-not (Test-Path -LiteralPath $script -PathType Leaf)) { return New-Result 127 @("missing script: $script") }
            return Invoke-Process $python @('-I', $script) $root
        }
        'script' {
            $script = Join-Path $root $Suite.Name
            if (-not (Test-Path -LiteralPath $script -PathType Leaf)) { return New-Result 127 @("missing script: $script") }
            return Invoke-Process $Suite.Options['Host'] @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script) $root
        }
        'node' {
            $problem = Get-NodeProblem -NeedNpm
            if ($problem) { return New-Result 1 @($problem) }
            return Invoke-Process 'npm' @('test') (Join-Path $root 'server\lobby') 600
        }
        'hle' {
            $exe = Join-Path $binDir "$($Suite.Name).exe"
            $unavailable = Get-BinaryProblem $Suite.Name
            if ($unavailable) { return New-Result 127 @($unavailable) }
            $problem = Get-NodeProblem
            if ($problem) { return New-Result 1 @($problem) }
            $lobby = Start-DevLobby
            try {
                if ($lobby.Error) { return New-Result 1 @($lobby.Error) }
                $env:ALN_TEST_LOBBY = "ws://127.0.0.1:$($lobby.Port)/"
                return Invoke-Process $exe @() $root 600
            } finally {
                $env:ALN_TEST_LOBBY = $null
                Stop-DevLobby $lobby
            }
        }
        default { return New-Result 1 @("unknown suite kind $($Suite.Kind)") }
    }
}

# Incremental build of the test targets only; one failed target does not stop the others, and its suite then fails by name.
function Build-TestTargets([string[]]$Targets) {
    if (-not $Targets.Count) { return }
    Write-Host '==> Building the focused test targets (incremental; the first build takes several minutes)' -ForegroundColor Cyan
    $build = Invoke-Process 'ninja' (@('-C', $buildDir, '-k', '0') + $Targets) $root 3600
    if ($build.Code -ne 0) {
        Write-Host '  FAIL  build of the test targets' -ForegroundColor Red
        $build.Lines | ForEach-Object { Write-Host $_ }
        $results.Add([pscustomobject]@{ Section = 'Build'; Name = 'ninja test targets'; Status = 'FAIL'; Seconds = 0; Checks = 0; Summary = "ninja exit code $($build.Code)" })
        return
    }
    $steps = @($build.Lines | Where-Object { $_ -match '^\[\d+/\d+\]' }).Count
    Write-Host ('  built {0} target(s), {1} step(s)' -f $Targets.Count, $steps)
}

# ---------------------------------------------------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------------------------------------------------
$configured = Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')
if ($Build -and -not $configured) { Stop-Tool "Run tools\build.ps1 first: it configures the build ($buildDir has no build.ninja)." }
$script:NinjaReady = $false
if ($configured) {
    $tc = $script:Toolchains.Keys | ForEach-Object { Get-ToolchainStatus $_ } | Where-Object { $_.Ready } | Select-Object -First 1
    if ($tc) { $env:PATH = "$($tc.Bin);$env:PATH"; $script:NinjaReady = [bool](Get-Command ninja -ErrorAction SilentlyContinue) }
}
$script:StaleTargets = @()
$script:StaleKnown = $false

# Nothing may open a window or an audio device, and nothing may reach a real lobby by environment.
$environment = [ordered]@{ SDL_VIDEODRIVER = 'dummy'; SDL_AUDIODRIVER = 'dummy'; ALN_LOBBY = $null; ALN_ONLINE = $null; ALN_ONLINE_NAME = $null;
    ALN_UPNP = $null; ALN_UDP_PORT = $null; ALN_STUN = $null; ALN_TEST_LOBBY = $null;
    NPM_CONFIG_UPDATE_NOTIFIER = 'false'; NPM_CONFIG_FUND = 'false'; NPM_CONFIG_AUDIT = 'false' }
$savedEnvironment = @{}
foreach ($name in $environment.Keys) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }

$results = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
$clock = [System.Diagnostics.Stopwatch]::StartNew()

try {
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $script:TreeBefore = Get-TreeFingerprint

    $cppSuites = @($suites | Where-Object { $_.Kind -in 'exe', 'hle' } | ForEach-Object { $_.Name } | Sort-Object -Unique)
    if ($Build) {
        Sync-GpuDependencies
        Build-TestTargets $cppSuites
    } elseif ($configured -and $script:NinjaReady) {
        $script:StaleTargets = @(Get-StaleTargets $cppSuites)
    }

    $section = ''
    foreach ($suite in $suites) {
        if ($suite.Section -ne $section) { $section = $suite.Section; Write-Host "==> $section" -ForegroundColor Cyan }
        $label = $suite.Name + $suite.Options['Label']
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        try { $run = Invoke-SuiteRun $suite }
        catch { $run = New-Result 1 @("the suite could not run: $($_.Exception.Message)") }
        $watch.Stop()

        $lines = @($run.Lines)
        $status = if ($run.Code -eq 0) { 'PASS' } else { 'FAIL' }
        $problem = $null
        $checks = 0
        foreach ($line in $lines) { if ($line -match '(\d+) checks') { $checks = [int]$Matches[1] } }
        if ($status -eq 'PASS' -and @($lines | Where-Object { $_ -match '^\s*(FAIL|FAILED)\b' }).Count) { $status = 'FAIL'; $problem = 'exit code 0, but the output reports FAIL' }
        if ($status -eq 'PASS' -and $suite.Options['MinChecks'] -gt 0 -and $checks -lt $suite.Options['MinChecks']) {
            $status = 'FAIL'
            $problem = "only $checks checks ran; this suite has at least $($suite.Options['MinChecks']) (coverage shrank)"
        }
        $summary = ''
        if ($suite.Kind -eq 'node') {
            $count = @{}
            foreach ($line in $lines) { if ($line -match '^\D*\b(tests|pass|fail|cancelled)\s+(\d+)\s*$') { $count[$Matches[1]] = [int]$Matches[2] } }
            if ($count.ContainsKey('pass')) { $summary = "$($count['tests']) tests, $($count['pass']) pass"; $checks = $count['pass'] }
            if ($status -eq 'PASS' -and (-not $count.ContainsKey('tests') -or $count['tests'] -eq 0 -or $count['fail'] -ne 0 -or $count['cancelled'] -ne 0 -or $count['pass'] -ne $count['tests'])) {
                $status = 'FAIL'
                $problem = 'exit code 0, but the test report shows no tests, or tests that failed, were cancelled or were skipped'
            }
        } else {
            $last = @($lines | Where-Object { $_ -match '(\d+ checks?|\bOK\b|\bpassed\b|0 failures|0 failed)' })
            if ($last.Count) { $summary = $last[-1].Trim() }
            else {
                $text = @($lines | Where-Object { $_.Trim() })
                if ($text.Count) { $summary = if ($suite.Kind -eq 'check') { $text[0].Trim() } else { $text[-1].Trim() } }
            }
        }
        if ($summary.Length -gt 90) { $summary = $summary.Substring(0, 87) + '...' }
        $ranUnderPwsh = $pwsh -and $pwsh.Source -ne $hostExe
        foreach ($line in $lines) {
            if ($line -match '^\s*skipped\b' -and -not ($ranUnderPwsh -and $line -match 'PowerShell 7 required')) { $notes.Add("${label}: $($line.Trim())") }
        }

        $results.Add([pscustomobject]@{ Section = $section; Name = $label; Status = $status; Seconds = $watch.Elapsed.TotalSeconds; Checks = $checks; Summary = $summary })
        if ($status -eq 'PASS') {
            Write-Host ('  PASS  {0,-46} {1,6:N1} s  {2}' -f $label, $watch.Elapsed.TotalSeconds, $summary) -ForegroundColor Green
            if ($Detail) { $lines | ForEach-Object { Write-Host "        $_" } }
        } else {
            Write-Host ('  FAIL  {0,-46} {1,6:N1} s  exit code {2}' -f $label, $watch.Elapsed.TotalSeconds, $run.Code) -ForegroundColor Red
            if ($problem) { Write-Host "        $problem" -ForegroundColor Red }
            Write-Host "  ----- output of $label -----" -ForegroundColor Red
            $lines | ForEach-Object { Write-Host $_ }
            Write-Host '  ----- end of output -----' -ForegroundColor Red
        }
    }
} finally {
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------------------------------------------------
$clock.Stop()
if (-not $Filter) { foreach ($entry in $notRunSuites) { $results.Add($entry) } }
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
$passed = @($results | Where-Object { $_.Status -eq 'PASS' })
$notRun = @($results | Where-Object { $_.Status -eq 'NOT RUN' })
$checkTotal = ($results | Measure-Object -Property Checks -Sum).Sum

Write-Host '==> Summary' -ForegroundColor Cyan
foreach ($group in ($results | Group-Object Section)) {
    $bad = @($group.Group | Where-Object { $_.Status -eq 'FAIL' }).Count
    $skipped = @($group.Group | Where-Object { $_.Status -eq 'NOT RUN' }).Count
    $text = if ($bad) { "$bad FAILED" } elseif ($skipped) { "$skipped NOT RUN" } else { 'all passed' }
    Write-Host ('  {0,-24} {1,3} suites  {2}' -f $group.Name, $group.Count, $text) -ForegroundColor $(if ($bad) { 'Red' } elseif ($skipped) { 'Yellow' } else { 'Gray' })
}
Write-Host ('  PASS: {0}   FAIL: {1}   NOT RUN: {2}   checks counted: {3:N0}   time: {4:N0} s' -f $passed.Count, $failed.Count, $notRun.Count, $checkTotal, $clock.Elapsed.TotalSeconds)
if ($notRun.Count) {
    Write-Host '  NOT RUN (not failures; each says what it needs):' -ForegroundColor Yellow
    foreach ($entry in $notRun) { Write-Host "    $($entry.Name): $($entry.Summary)" -ForegroundColor Yellow }
}
if ($notes.Count) {
    Write-Host '  parts the suites skipped (not failures):' -ForegroundColor Yellow
    $notes | Select-Object -Unique | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
}
if ($Filter) { Write-Host "  PARTIAL RUN: only suites matching '$Filter' ran." -ForegroundColor Yellow }
if ($failed.Count) {
    Write-Host ("FAILED: {0}" -f (($failed | ForEach-Object { $_.Name }) -join ', ')) -ForegroundColor Red
    exit 1
}
if ($passed.Count -eq 0) { Write-Host 'FAILED: no suite ran.' -ForegroundColor Red; exit 1 }
if ($notRun.Count) {
    Write-Host ("PASSED, but {0} suite(s) did not run (see NOT RUN above). Real gameplay, controllers and visuals are still tested by hand." -f $notRun.Count) -ForegroundColor Yellow
    exit 0
}
Write-Host 'All suites passed. Real gameplay (online with 3 or 4 players, controllers, visuals) is still tested by hand.' -ForegroundColor Green
exit 0
