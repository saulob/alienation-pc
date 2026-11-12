<#
.SYNOPSIS
The one focused validation command: builds the test targets and runs every headless suite. No game, launcher window,
controller, audio device or production service is used.

.DESCRIPTION
tools\build.ps1 must have configured build\cmake\clang64 first. The suites are grouped in sections, each prints PASS or
FAIL, and a failing suite shows its real stdout and stderr. Every suite runs even after one fails; the exit code is
non-zero when any suite failed, a binary or script is missing, or a dependency (the project .venv Python, Node.js, the
prepared game builds) is not there. Nothing is silently skipped: parts a suite skips are listed in the summary.

Local co-op (one to four controllers) runs on virtual SDL gamepads, online rooms of two to four players on a loopback
development lobby with modelled peers, the launcher's Accept and play on the launcher's own online client. Real
gameplay, real controllers, real network play and the visuals are still tested by hand.

The git state is never changed: the checks only read it, and the run fails if a suite leaves the tree different.

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
#>
[CmdletBinding()]
param(
    [string]$Packages = $env:ALN_TEST_PACKAGES,
    [switch]$SkipGameData,
    [switch]$Detail,
    [switch]$List,
    [string]$Filter = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
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
}
Add-Suite $s 'hle' 'aln-online-hle-tests' 'Matching2 rooms of 2, 3 and 4 players with a full mesh of peers, four-player invitations, launcher Accept and play, sessions, relay and DGRAM_P2P on a loopback lobby'

$s = 'Backend'
Add-Suite $s 'node' 'server\lobby\test' 'lobby protocol, friends, presence, rooms, sessions, invitations, relay, rate limits, build isolation, admin data (in memory, no network)'

$s = 'Packaging and install'
Add-Suite $s 'exe' 'aln-game-install-tests' 'Install game and Install update: EUR base and 1.06, DUPLEX repack, ASIA/KR base, refusals' @{ Packages = $true }
Add-Suite $s 'exe' 'aln-update-tests' 'updater (no network)'
Add-Suite $s 'exe' 'aln-update-helper-tests' 'update helper'

$s = 'Final static checks'
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
    $targets = Invoke-Process 'ninja' @('-C', $buildDir, '-t', 'targets', 'all') $root 120
    if ($targets.Code -ne 0) { return New-Result 1 (@('ninja could not list the targets:') + $targets.Lines) }
    $known = @($targets.Lines | ForEach-Object { if ($_ -match '^(aln-[A-Za-z0-9-]+-tests):') { $Matches[1] } } | Sort-Object -Unique)
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
    if (-not (Test-Path -LiteralPath (Join-Path $root 'server\lobby\test\lobby.test.js'))) { $problems.Add('server\lobby\test has no tests') }
    if ($problems.Count) { return New-Result 1 @($problems) }
    $excluded = @($known | Where-Object { $excludedTargets.ContainsKey($_) })
    return New-Result 0 (@("$($known.Count - $excluded.Count) CMake test targets, all run") + ($excluded | ForEach-Object { "excluded $_ : $($excludedTargets[$_])" }))
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

function Get-NodeProblem {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { return 'Node.js 22.13 or later was not found. Install it (winget install OpenJS.NodeJS.LTS) and open a new terminal.' }
    $version = ((& $node.Source --version) -replace '^v', '').Trim()
    if ([version]$version -lt [version]'22.13.0') { return "Node.js $version is older than 22.13. Update it (winget upgrade OpenJS.NodeJS.LTS)." }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'server\lobby\package.json'))) { return 'server\lobby\package.json is missing.' }
    return $null
}

function Invoke-SuiteRun($Suite) {
    switch ($Suite.Kind) {
        'check' { return & $Suite.Options['Run'] }
        'exe' {
            $exe = Join-Path $binDir "$($Suite.Name).exe"
            if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { return New-Result 127 @("missing binary: $exe (the target did not build; see the build failure above)") }
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
            $problem = Get-NodeProblem
            if ($problem) { return New-Result 1 @($problem) }
            $node = (Get-Command node).Source
            return Invoke-Process $node @('--no-warnings', '--test', 'test/*.test.js') (Join-Path $root 'server\lobby') 600
        }
        'hle' {
            $exe = Join-Path $binDir "$($Suite.Name).exe"
            if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { return New-Result 127 @("missing binary: $exe (the target did not build; see the build failure above)") }
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
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 first: it configures the build.' }

# Nothing may open a window or an audio device, and nothing may reach a real lobby by environment.
$environment = [ordered]@{ SDL_VIDEODRIVER = 'dummy'; SDL_AUDIODRIVER = 'dummy'; ALN_LOBBY = $null; ALN_ONLINE = $null; ALN_ONLINE_NAME = $null;
    ALN_UPNP = $null; ALN_UDP_PORT = $null; ALN_STUN = $null; ALN_TEST_LOBBY = $null }
$savedEnvironment = @{}
foreach ($name in $environment.Keys) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }

$results = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
$clock = [System.Diagnostics.Stopwatch]::StartNew()

try {
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $script:TreeBefore = Get-TreeFingerprint

    Sync-GpuDependencies
    Build-TestTargets @($suites | Where-Object { $_.Kind -in 'exe', 'hle' } | ForEach-Object { $_.Name } | Sort-Object -Unique)

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
            $pass = @($lines | Where-Object { $_ -match '\bpass\s+(\d+)\s*$' })
            if ($pass.Count -and $pass[-1] -match 'pass\s+(\d+)\s*$') { $summary = "$($Matches[1]) tests"; $checks = [int]$Matches[1] }
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
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
$passed = @($results | Where-Object { $_.Status -eq 'PASS' })
$checkTotal = ($results | Measure-Object -Property Checks -Sum).Sum

Write-Host '==> Summary' -ForegroundColor Cyan
foreach ($group in ($results | Group-Object Section)) {
    $bad = @($group.Group | Where-Object { $_.Status -eq 'FAIL' }).Count
    Write-Host ('  {0,-24} {1,3} suites  {2}' -f $group.Name, $group.Count, $(if ($bad) { "$bad FAILED" } else { 'all passed' })) -ForegroundColor $(if ($bad) { 'Red' } else { 'Gray' })
}
Write-Host ('  total suites: {0}   passed: {1}   failed: {2}   checks counted: {3:N0}   time: {4:N0} s' -f $results.Count, $passed.Count, $failed.Count, $checkTotal, $clock.Elapsed.TotalSeconds)
if ($notes.Count) {
    Write-Host '  parts the suites skipped (not failures):' -ForegroundColor Yellow
    $notes | Select-Object -Unique | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
}
if ($Filter) { Write-Host "  PARTIAL RUN: only suites matching '$Filter' ran." -ForegroundColor Yellow }
if ($failed.Count) {
    Write-Host ("FAILED: {0}" -f (($failed | ForEach-Object { $_.Name }) -join ', ')) -ForegroundColor Red
    exit 1
}
if (-not $results.Count) { Write-Host 'FAILED: no suite ran.' -ForegroundColor Red; exit 1 }
Write-Host 'All focused suites passed. Real gameplay (online with 3 or 4 players, controllers, visuals) is still tested by hand.' -ForegroundColor Green
exit 0
