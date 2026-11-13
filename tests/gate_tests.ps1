# Contract of tools\test.ps1: it parses, takes every path from the repository, runs from any folder, and reports what
# npm, a missing npm and a missing build folder do. The gate runs in child PowerShell processes with -List, or with -Filter
# on the one Worker suite and a stand-in npm in a temporary folder owned by this test. No game, launcher, window, audio,
# compiler or network is involved.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
$gate = Join-Path $root 'tools\test.ps1'
$shell = (Get-Process -Id $PID).Path
$workerSuite = 'server\lobby (npm test)'

$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name" -ForegroundColor Red
    }
}

function Invoke-Gate([string[]]$Arguments, [string]$Path = $env:PATH, [string]$WorkDir = $root) {
    $saved = $env:PATH
    Push-Location -LiteralPath $WorkDir
    try {
        $env:PATH = $Path
        $text = & $shell -NoProfile -ExecutionPolicy Bypass -File $gate @Arguments *>&1 | Out-String
        return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
    } finally {
        $env:PATH = $saved
        Pop-Location
    }
}

function New-StandInNpm([string]$Folder, [string[]]$Lines, [int]$Code) {
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    $body = @('@echo off') + ($Lines | ForEach-Object { "echo $_" }) + "exit /b $Code"
    Set-Content -LiteralPath (Join-Path $Folder 'npm.cmd') -Value $body -Encoding ascii
}

$work = Join-Path ([IO.Path]::GetTempPath()) "aln-gate-tests-$PID"
try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null

    $errors = $null
    $text = Get-Content -Raw -LiteralPath $gate
    [void][System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$null, [ref]$errors)
    Check ($errors.Count -eq 0) 'tools\test.ps1 parses'
    Check ($text -notmatch '(?m)^[^#\r\n]*\b[A-Za-z]:\\\\?[A-Za-z]') 'no drive-letter path in the gate'
    Check ($text.Replace('DEBUG_SAULO', '') -notmatch '(?i)saulo') 'no person in the gate'
    Check ($text -notmatch '(?i)\b(Set-Location|Push-Location|Pop-Location)\b|^\s*cd\s') 'the gate never changes the caller''s folder'
    Check ($text.Contains("Invoke-Process 'npm' @('test')")) 'the Worker suite is the package''s own npm test'
    Check ($text -notmatch '(?i)\bwrangler\s+(deploy|publish|secret)|build\.ps1\s+-Run|\bgit\s+(push|fetch|pull|clone)\b') 'the gate deploys, runs and fetches nothing'
    Check ($text -match 'if \(\$Build\) \{\s+Sync-GpuDependencies') 'only -Build fetches the video core dependencies'

    $fromRoot = Invoke-Gate @('-List')
    $absolute = Invoke-Gate @('-List') $env:PATH $work
    Check ($fromRoot.Code -eq 0 -and $absolute.Code -eq 0) '-List works from the repository root and from another folder'
    Check ($fromRoot.Text -eq $absolute.Text) 'the suites do not depend on the caller''s folder'
    Check ($fromRoot.Text.Contains($workerSuite) -and $fromRoot.Text.Contains('Worker protection wiring')) 'the Worker suite and its wiring check are listed'
    Check ($fromRoot.Text.Contains('tests\gate_tests.ps1') -and $fromRoot.Text.Contains('repository hygiene')) 'the gate checks itself and the repository'

    $failing = Join-Path $work 'failing'
    New-StandInNpm $failing @('simulated failing tests') 1
    $run = Invoke-Gate @('-Filter', $workerSuite) "$failing;$env:PATH"
    Check ($run.Code -ne 0 -and $run.Text -match 'FAIL' -and $run.Text.Contains('simulated failing tests')) 'a failing npm test fails the gate and shows its output'

    $passing = Join-Path $work 'passing'
    New-StandInNpm $passing @('# tests 7', '# pass 7', '# fail 0', '# cancelled 0') 0
    $run = Invoke-Gate @('-Filter', $workerSuite) "$passing;$env:PATH"
    Check ($run.Code -eq 0 -and $run.Text -match '7 tests, 7 pass') 'a passing npm test passes the gate with its count'

    $lying = Join-Path $work 'lying'
    New-StandInNpm $lying @('# tests 7', '# pass 5', '# fail 2', '# cancelled 0') 0
    $run = Invoke-Gate @('-Filter', $workerSuite) "$lying;$env:PATH"
    Check ($run.Code -ne 0) 'a report with failed tests fails the gate even when npm exits 0'

    $node = Get-Command node -ErrorAction SilentlyContinue
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($node -and $git -and (Split-Path $node.Source) -ne (Split-Path $git.Source)) {
        $bare = "$(Split-Path $git.Source);$env:SystemRoot\System32"
        $run = Invoke-Gate @('-Filter', $workerSuite) $bare
        Check ($run.Code -ne 0 -and $run.Text -match 'Node\.js 22\.13 or later was not found') 'a missing Node.js is reported with how to install it'
    }

    $run = Invoke-Gate @('-Filter', 'aln-log-tests', '-BuildDir', (Join-Path $work 'no-build'))
    Check ($run.Text -match 'NOT RUN' -and $run.Text.Contains('.\tools\test.ps1 -Build') -and $run.Text.Contains('FAILED: no suite ran')) 'a missing build folder is NOT RUN with the command that fixes it, and a run with nothing run fails'
} finally {
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($work)
    if ((Split-Path $full -Parent) -eq $temp -and (Split-Path $full -Leaf).StartsWith('aln-gate-tests-')) {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "$script:checks checks, $script:failures failed"
exit $(if ($script:failures) { 1 } else { 0 })
