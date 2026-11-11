# Contract of tools\online-instance.ps1: input validation, -WhatIf starts nothing and creates nothing, long paths.
# Every scenario runs the script in a child PowerShell with -WhatIf or with arguments it rejects, against stand-in files
# inside one temporary folder owned by this test. No game, launcher, window, audio or network is involved.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'tools\online-instance.ps1'
$shell = (Get-Process -Id $PID).Path

$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name" -ForegroundColor Red
    }
}

function Invoke-Instance([string[]]$Arguments) {
    $text = & $shell -NoProfile -ExecutionPolicy Bypass -File $target @Arguments *>&1 | Out-String
    [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
}

function Remove-OwnedFolder([string]$Path, [string]$Prefix) {
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $full = [IO.Path]::GetFullPath($Path)
    if ((Split-Path $full -Parent) -ne $temp.TrimEnd('\', '/') -or -not (Split-Path $full -Leaf).StartsWith($Prefix)) { return }
    Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
}

$prefix = 'aln-online-instance-tests-'
$work = Join-Path ([IO.Path]::GetTempPath()) "$prefix$PID"
$name = "wi-$PID"
$instanceUser = Join-Path $root "user\test-client-$name"
$instanceLogs = Join-Path $root "build\logs\test-client-$name"

try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $exe = Join-Path $work 'stand-in.exe'
    Set-Content -LiteralPath $exe -Value 'not an executable'
    $copyFrom = Join-Path $work 'user'
    New-Item -ItemType Directory -Force -Path (Join-Path $copyFrom 'savedata') | Out-Null
    Set-Content -LiteralPath (Join-Path $copyFrom 'alienation.ini') -Value '[video]'

    $source = Get-Content -LiteralPath $target -Raw
    Check (([regex]::Matches($source, 'Start-Process')).Count -eq 1) 'online-instance.ps1 has exactly one Start-Process'
    Check ($source -match '(?s)ShouldProcess\(\$Executable[^\r\n]*\{[^}]*Start-Process') 'Start-Process sits inside the ShouldProcess check'
    Check ($source -notmatch 'Remove-Item') 'online-instance.ps1 deletes nothing'
    Check ($source -match '(?s)finally \{[^}]*SetEnvironmentVariable') 'process environment is restored in a finally block'

    $missing = Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', (Join-Path $work 'absent.exe'))
    Check ($missing.Code -ne 0 -and $missing.Text -match 'Not built yet') 'a missing executable stops with Not built yet'

    foreach ($bad in 'a b', '..\x', 'a/b', ('n' * 17)) {
        $result = Invoke-Instance @('-Name', $bad, '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
        Check ($result.Code -ne 0) "instance name '$bad' is rejected"
    }
    Check ((Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-OnlineName', 'a b')).Code -ne 0) 'an online name with a space is rejected'
    Check ((Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-Build', 'CUSA00000-9.99')).Code -ne 0) 'an unknown game build is rejected'

    $plan = Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom, '-OnlineName', 'Bravo')
    Check ($plan.Code -eq 0) '-WhatIf succeeds with stand-in files'
    Check ($plan.Text -match [regex]::Escape("user folder: $instanceUser") -and $plan.Text -match [regex]::Escape("logs:        $instanceLogs")) '-WhatIf prints the instance and log folders under the repository'
    Check ($plan.Text -match 'Online:\s+on, production lobby') 'online play is reported on, with the production lobby, by default'
    Check (-not (Test-Path -LiteralPath $instanceUser) -and -not (Test-Path -LiteralPath $instanceLogs)) '-WhatIf creates no instance or log folder'

    $dev = Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom, '-Lobby', 'ws://127.0.0.1:8790/', '-NoUpnp')
    Check ($dev.Code -eq 0 -and $dev.Text -match 'Online:\s+on, lobby ws://127\.0\.0\.1:8790/, UPnP off') '-Lobby and -NoUpnp are reported'
    foreach ($bad in 'http://x', 'ws://a b', 'lobby') {
        Check ((Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-Lobby', $bad)).Code -ne 0) "lobby '$bad' is rejected"
    }
    Check ($source -match "ALN_LOBBY" -and $source -match "'ALN_UPNP'") 'the lobby and UPnP overrides are restored with the rest of the environment'

    $offline = Invoke-Instance @('-Name', $name, '-WhatIf', '-Offline', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($offline.Code -eq 0 -and $offline.Text -match 'Online:\s+off') '-Offline is reported'

    if ($PSVersionTable.PSVersion.Major -ge 7) {
        $long = $work
        while ($long.Length -lt 300) { $long = Join-Path $long ('segment-' + ('x' * 24)) }
        New-Item -ItemType Directory -Force -Path (Join-Path $long 'savedata') | Out-Null
        $longExe = Join-Path $long 'stand-in.exe'
        Set-Content -LiteralPath $longExe -Value 'not an executable'
        Set-Content -LiteralPath (Join-Path $long 'alienation.ini') -Value '[video]'
        $longPlan = Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $longExe, '-CopyFrom', $long)
        Check ($longPlan.Code -eq 0 -and $longPlan.Text -match [regex]::Escape("user folder: $instanceUser")) '-WhatIf accepts an executable and settings folder beyond 260 characters'
        Check (-not (Test-Path -LiteralPath $instanceUser)) 'long paths create no instance folder'
    }
    else { Write-Host 'skipped: long path scenario (PowerShell 7 required)' }
}
finally {
    Remove-OwnedFolder $work $prefix
}

if ($script:failures -gt 0) {
    Write-Host "$script:failures of $script:checks online instance checks failed" -ForegroundColor Red
    exit 1
}
Write-Host "online instance tests passed ($script:checks checks)"
