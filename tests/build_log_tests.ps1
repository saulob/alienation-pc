# Build log contract of tools\build.ps1 and tools\build.sh: numbered, never overwritten, exit codes preserved. Runs the
# helpers in a temporary folder with small stand-in commands; no build, game, launcher or network is involved.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools\_common.ps1')

$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name" -ForegroundColor Red
    }
}

$shell = (Get-Process -Id $PID).Path
$work = Join-Path ([IO.Path]::GetTempPath()) "aln-build-log-tests-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
    Check ((Get-ProjectName) -eq 'alienation') 'the log name comes from .recomp.json (alienation)'

    $dir = Join-Path $work 'ps'
    $first = Start-BuildLog 'incremental' 'RelWithDebInfo' $dir 'alienation' 6>$null
    Check ((Split-Path $first -Leaf) -eq 'alienation_001.log') 'the first log is alienation_001.log'
    Write-BuildLogLine 'a.c:1:1: warning: unused'
    Write-BuildLogLine 'b.c:2:2: error: broken'
    Close-BuildLog 0
    Close-BuildLog 9
    $text = Get-Content -LiteralPath $first -Raw
    Check ($text -match 'Project: alienation' -and $text -match 'Configuration: RelWithDebInfo' -and $text -match 'Mode: incremental') 'header names the project, configuration and mode'
    Check ($text -match 'Warnings: 1' -and $text -match 'Errors: 1') 'warnings and errors are counted'
    Check ($text -match 'Result: SUCCESS' -and $text -match 'Exit code: 0' -and $text -match 'Finished: ') 'footer holds result, exit code and end time'
    Check ($text -notmatch 'Exit code: 9') 'closing twice does not change the recorded exit code'
    $firstHash = (Get-FileHash -LiteralPath $first).Hash

    $second = Start-BuildLog 'complete (-Clean)' 'RelWithDebInfo' $dir 'alienation' 6>$null
    Close-BuildLog 1
    Check ((Split-Path $second -Leaf) -eq 'alienation_002.log') 'the second run gets alienation_002.log'
    Check ((Get-FileHash -LiteralPath $first).Hash -eq $firstHash) 'an earlier log is not touched'
    Check ((Get-Content -LiteralPath $second -Raw) -match 'Result: FAILED' -and (Get-Content -LiteralPath $second -Raw) -match 'Mode: complete') 'a failed complete build is recorded as such'

    Set-Content -LiteralPath (Join-Path $dir 'alienation_007.log') -Value 'existing'
    $third = Start-BuildLog 'incremental' 'RelWithDebInfo' $dir 'alienation' 6>$null
    Close-BuildLog 0
    Check ((Split-Path $third -Leaf) -eq 'alienation_008.log') 'numbers only advance past the highest existing log'
    Check ((Get-Content -LiteralPath (Join-Path $dir 'alienation_007.log') -Raw).Trim() -eq 'existing') 'an existing log is not overwritten'

    $jobs = 1..4 | ForEach-Object {
        Start-Job -ArgumentList $root, (Join-Path $work 'parallel') -ScriptBlock {
            param($repo, $logs)
            . (Join-Path $repo 'tools\_common.ps1')
            $null = Start-BuildLog 'incremental' 'RelWithDebInfo' $logs 'alienation' 6>$null
            Start-Sleep -Milliseconds 100
            Close-BuildLog 0
        }
    }
    $null = $jobs | Wait-Job -Timeout 120
    $jobs | Remove-Job -Force
    $parallel = @(Get-ChildItem -LiteralPath (Join-Path $work 'parallel') -Filter 'alienation_*.log')
    Check ($parallel.Count -eq 4) "four concurrent builds reserve four different logs (got $($parallel.Count))"

    $null = Start-BuildLog 'incremental' 'RelWithDebInfo' (Join-Path $work 'native') 'alienation' 6>$null
    Invoke-Native $shell @('-NoProfile', '-Command', "[Console]::Out.WriteLine('to-stdout'); [Console]::Error.WriteLine('to-stderr'); exit 7") 6>$null | Out-Null
    $nativeExit = $LASTEXITCODE
    Close-BuildLog $nativeExit
    $native = Get-Content -LiteralPath (Join-Path $work 'native\alienation_001.log') -Raw
    Check ($nativeExit -eq 7) "Invoke-Native keeps the tool's exit code (got $nativeExit)"
    Check ($native -match 'to-stdout' -and $native -match 'to-stderr') 'stdout and stderr of a tool both reach the log'
    Check ($native -match 'Exit code: 7' -and $native -match 'Result: FAILED') 'the log records the failing tool exit code'

    $stopDir = Join-Path $work 'stop'
    $stopScript = Join-Path $work 'stop.ps1'
    Set-Content -LiteralPath $stopScript -Value @"
. '$(Join-Path $root 'tools\_common.ps1')'
`$null = Start-BuildLog 'incremental' 'RelWithDebInfo' '$stopDir' 'alienation'
Stop-Tool 'simulated early failure'
"@
    & $shell -NoProfile -File $stopScript *> $null
    $stopExit = $LASTEXITCODE
    $stopLog = Get-Content -LiteralPath (Join-Path $stopDir 'alienation_001.log') -Raw
    Check ($stopExit -eq 1) "Stop-Tool exits with code 1 (got $stopExit)"
    Check ($stopLog -match 'error: simulated early failure' -and $stopLog -match 'Exit code: 1') 'a build that fails early keeps its log'

    # One log per invocation: a build alone is build_NNN.log; with -Run the build section starts the session log
    # build\logs\alienation_NNN.log, which the game continues with --log.
    $buildOnly = Get-BuildLogPlan
    $session = Get-BuildLogPlan -Run
    Check ($buildOnly.Name -eq 'build' -and $buildOnly.Directory -eq (Join-Path $root 'build\logs\build') -and -not $buildOnly.Session) 'a build alone logs to build\logs\build\build_NNN.log'
    Check ($session.Name -eq 'alienation' -and $session.Directory -eq (Join-Path $root 'build\logs') -and $session.Session) 'a build with -Run logs to the session namespace build\logs\alienation_NNN.log'
    $plainDir = Join-Path $work 'plain'
    $plain = Start-BuildLog 'incremental' 'RelWithDebInfo' $plainDir $buildOnly.Name 6>$null
    Close-BuildLog 0
    Check ((Split-Path $plain -Leaf) -eq 'build_001.log' -and (Get-Content -LiteralPath $plain -TotalCount 1) -eq 'Project: alienation') 'a build-only log is build_001.log without a session marker'
    $loaderArgs = @(Get-SessionLoaderArgs 'X:\logs\alienation_004.log' @('--check-imports'))
    Check ($loaderArgs.Count -eq 3 -and $loaderArgs[0] -eq '--log' -and $loaderArgs[1] -eq 'X:\logs\alienation_004.log' -and $loaderArgs[2] -eq '--check-imports') 'the loader is given the session log with --log'
    $own = @(Get-SessionLoaderArgs 'X:\logs\alienation_004.log' @('--log', 'mine.log'))
    Check ($own.Count -eq 2 -and $own[1] -eq 'mine.log') 'a --log the user gives is kept'
    Check ((Get-SessionLoaderArgs 'X:\a.log' @()).Count -eq 2) 'the session log is passed when there are no other arguments'

    $sessionDir = Join-Path $work 'session'
    Set-Content -LiteralPath (Join-Path (New-Item -ItemType Directory -Force -Path $sessionDir).FullName 'alienation_069.log') -Value '[run] earlier session'
    $sessionPath = Start-BuildLog 'incremental' 'RelWithDebInfo' $sessionDir $session.Name -Session 6>$null
    Write-BuildLogLine 'ninja: no work to do.'
    Close-BuildLog 0
    $buildSection = [IO.File]::ReadAllText($sessionPath)
    Check ((Split-Path $sessionPath -Leaf) -eq 'alienation_070.log') 'the session log takes the next game log number'
    Check ((Get-Content -LiteralPath $sessionPath -TotalCount 1) -eq '[build]') 'the session log starts with its [build] section'
    Add-Content -LiteralPath $sessionPath -Value "[run] 2026-11-11T01:00:00`nRuntime: guest thread 'main' created"
    Complete-SessionLog $sessionPath $buildSection 0
    $sessionText = Get-Content -LiteralPath $sessionPath -Raw
    Check ($sessionText.IndexOf('[build]') -eq 0 -and $sessionText.IndexOf('Result: SUCCESS') -lt $sessionText.IndexOf('[run] 2026') -and
           $sessionText.TrimEnd().EndsWith('[run] alienation.exe exit code 0')) 'build section, then the game, then the exit code, in one file'
    Check (@(Get-ChildItem -LiteralPath $sessionDir -Filter 'alienation_*.log').Count -eq 2 -and -not (Test-Path (Join-Path $sessionDir 'build'))) 'no second log is made for the same invocation'
    Remove-Item -LiteralPath $sessionPath
    Complete-SessionLog $sessionPath $buildSection 0
    $restored = Get-Content -LiteralPath $sessionPath -Raw
    Check ($restored.StartsWith('[build]') -and $restored -match 'ninja: no work to do' -and $restored.TrimEnd().EndsWith('exit code 0')) 'a session log the runtime removed after a clean exit keeps its build section'

    $loaderSource = Get-Content -LiteralPath (Join-Path $root 'src\loader.c') -Raw
    Check ($loaderSource -match 'if \(log_file\) log_started=runtime_log_continue\(log_file') 'the loader continues an existing --log file instead of truncating it'
    $posixLog = Get-Content -LiteralPath (Join-Path $root 'src\runtime_log_posix.c') -Raw
    Check ($posixLog -match 'flags \| O_APPEND' -and $posixLog -notmatch 'O_TRUNC') 'on Linux a given log file is appended to'
    $buildScript = Get-Content -LiteralPath (Join-Path $root 'tools\build.ps1') -Raw
    $build = Get-Content -LiteralPath (Join-Path $root 'tools\build.sh') -Raw
    Check ($buildScript -match 'Get-SessionLoaderArgs \$sessionLog' -and $buildScript -match 'Complete-SessionLog \$sessionLog' -and
           $buildScript -notmatch "Filter 'alienation_\*\.log'") 'build.ps1 -Run uses its own session log, not the newest file in build\logs'
    $scripts = $buildScript + (Get-Content -LiteralPath (Join-Path $root 'tools\_common.ps1') -Raw) + $build
    Check ($scripts -notmatch '[A-Za-z]:\\\\?(VisualStudio|Users)') 'no machine-specific path is embedded'

    Check ($build -match 'build/logs/build' -and $build -match 'noclobber' -and $build -match "trap 'close_build_log") 'build.sh reserves the same numbered log'
    Check ($build -match '--log "\$build_log"' -and $build -notmatch '(?m)^exec "\$loader"') 'build.sh --run passes its session log to the game and records the exit code'
    $msys = Get-Msys2Root
    $bash = if ($msys) { Join-Path $msys 'usr\bin\bash.exe' } else { $null }
    if ($bash -and (Test-Path -LiteralPath $bash)) {
        $block = [regex]::Match($build, '(?s)# BEGIN build log.*?# END build log').Value
        $body = "echo out-line`necho 'x.c:1: error: boom' >&2`nexit 5`n"
        foreach ($mode in 0, 1) {
            $unix = $work.Replace('\', '/') + "/sh$mode"
            New-Item -ItemType Directory -Force -Path $unix | Out-Null
            Copy-Item -LiteralPath (Join-Path $root '.recomp.json') $unix
            $header = "set -euo pipefail`nroot=$unix config=RelWithDebInfo clean=0 run=$mode`ndie() { echo `"ERROR: `$*`" >&2; exit 1; }`n"
            [IO.File]::WriteAllText("$work\sh$mode.sh", ($header + $block + "`n" + $body).Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
            & $bash "$unix.sh" *> $null
            $shExit1 = $LASTEXITCODE
            & $bash "$unix.sh" *> $null
            if ($mode -eq 0) {
                $shLogs = @(Get-ChildItem -LiteralPath (Join-Path $work 'sh0\build\logs\build') -Filter 'build_*.log' | Sort-Object Name)
                Check ($shExit1 -eq 5) "build.sh keeps the failing exit code (got $shExit1)"
                Check ($shLogs.Count -eq 2 -and $shLogs[0].Name -eq 'build_001.log' -and $shLogs[1].Name -eq 'build_002.log') 'build.sh numbers its build-only logs and keeps the earlier one'
                $shText = Get-Content -LiteralPath $shLogs[0].FullName -Raw
                Check ($shText -match 'out-line' -and $shText -match 'error: boom' -and $shText -match 'Exit code: 5') 'build.sh log holds stdout, stderr and the exit code'
            }
            else {
                $shLogs = @(Get-ChildItem -LiteralPath (Join-Path $work 'sh1\build\logs') -Filter 'alienation_*.log' | Sort-Object Name)
                Check ($shLogs.Count -eq 2 -and $shLogs[0].Name -eq 'alienation_001.log' -and
                       (Get-Content -LiteralPath $shLogs[0].FullName -TotalCount 1) -eq '[build]' -and
                       -not (Test-Path (Join-Path $work 'sh1\build\logs\build'))) 'build.sh --run starts the session log alienation_NNN.log with its [build] section'
            }
        }
    }
    else { Write-Host 'skipped: build.sh log block run (MSYS2 bash not found)' }
}
finally {
    $script:BuildLog = $null
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failures -gt 0) {
    Write-Host "$script:failures of $script:checks build log checks failed" -ForegroundColor Red
    exit 1
}
Write-Host "build log tests passed ($script:checks checks)"
