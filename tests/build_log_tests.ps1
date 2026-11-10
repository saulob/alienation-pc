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

    $build = Get-Content -LiteralPath (Join-Path $root 'tools\build.sh') -Raw
    Check ($build -match 'build/logs/build' -and $build -match 'noclobber' -and $build -match "trap 'close_build_log") 'build.sh reserves the same numbered log'
    $msys = Get-Msys2Root
    $bash = if ($msys) { Join-Path $msys 'usr\bin\bash.exe' } else { $null }
    if ($bash -and (Test-Path -LiteralPath $bash)) {
        $unix = $work.Replace('\', '/') + '/sh'
        New-Item -ItemType Directory -Force -Path $unix | Out-Null
        Copy-Item -LiteralPath (Join-Path $root '.recomp.json') $unix
        $block = [regex]::Match($build, '(?s)# BEGIN build log.*?# END build log').Value
        $header = "set -euo pipefail`nroot=$unix config=RelWithDebInfo clean=0`ndie() { echo `"ERROR: `$*`" >&2; exit 1; }`n"
        $body = "echo out-line`necho 'x.c:1: error: boom' >&2`nexit 5`n"
        [IO.File]::WriteAllText("$work\sh.sh", ($header + $block + "`n" + $body).Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
        & $bash "$unix.sh" *> $null
        $shExit1 = $LASTEXITCODE
        & $bash "$unix.sh" *> $null
        $shLogs = @(Get-ChildItem -LiteralPath (Join-Path $work 'sh\build\logs\build') -Filter 'alienation_*.log' | Sort-Object Name)
        Check ($shExit1 -eq 5) "build.sh keeps the failing exit code (got $shExit1)"
        Check ($shLogs.Count -eq 2 -and $shLogs[0].Name -eq 'alienation_001.log' -and $shLogs[1].Name -eq 'alienation_002.log') 'build.sh numbers its logs and keeps the earlier one'
        $shText = Get-Content -LiteralPath $shLogs[0].FullName -Raw
        Check ($shText -match 'out-line' -and $shText -match 'error: boom' -and $shText -match 'Exit code: 5') 'build.sh log holds stdout, stderr and the exit code'
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
