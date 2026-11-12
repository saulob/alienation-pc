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

    Check ($plan.Text -match 'Online name: Bravo') 'an explicit -OnlineName is used'
    $own = Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($own.Code -eq 0 -and $own.Text -match "Online name: $([regex]::Escape($name))\b") 'a name that is no callsign is its own Online name'
    $bravo = Invoke-Instance @('-Name', 'b', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($bravo.Code -eq 0 -and $bravo.Text -match 'Online name: Bravo' -and
           $bravo.Text -match [regex]::Escape('user folder: ' + (Join-Path $root 'user\test-client-bravo'))) 'b is Bravo in user\test-client-bravo'
    $squad = Invoke-Instance @('-Players', '4', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    $names = [regex]::Matches($squad.Text, 'Online name: (\w+)') | ForEach-Object { $_.Groups[1].Value }
    Check ($squad.Code -eq 0 -and ($names -join ',') -eq 'Alpha,Bravo,Charlie,Delta') '-Players 4 plans Alpha, Bravo, Charlie and Delta'
    $logFolders = [regex]::Matches($squad.Text, 'logs:\s+(\S+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    Check ($logFolders.Count -eq 4) '-Players 4 gives every instance its own log folder'
    $pair = Invoke-Instance @('-Players', '2', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom, '-Build', 'CUSA00062-1.06')
    Check ($pair.Code -ne 0 -or ([regex]::Matches($pair.Text, 'Online name:')).Count -eq 2) '-Players 2 plans two instances'
    foreach ($bad in '1', '5') {
        Check ((Invoke-Instance @('-Players', $bad, '-WhatIf', '-Executable', $exe)).Code -ne 0) "-Players $bad is rejected"
    }
    Check ((Invoke-Instance @('-Players', '2', '-OnlineName', 'Zulu', '-WhatIf', '-Executable', $exe)).Code -ne 0) '-Players takes no -OnlineName'

    $friends = Invoke-Instance @('-Players', '2', '-Friends', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($friends.Code -eq 0 -and $friends.Text -match 'Friends: Alpha, Bravo \(a friend request and its acceptance, through wss://') '-Players 2 -Friends plans the friend request through the production lobby'
    Check ($friends.Text -match 'Make the test instances friends') '-Friends makes friends only behind ShouldProcess'
    $devFriends = Invoke-Instance @('-Players', '2', '-Friends', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom, '-Lobby', 'ws://127.0.0.1:8790/')
    Check ($devFriends.Code -eq 0 -and $devFriends.Text -match 'through ws://127\.0\.0\.1:8790/') '-Friends uses the -Lobby of the instances'
    Check ((Invoke-Instance @('-Name', $name, '-Friends', '-WhatIf', '-Executable', $exe)).Code -ne 0) '-Friends needs -Players'
    $offlineFriends = Invoke-Instance @('-Players', '2', '-Friends', '-Offline', '-WhatIf', '-Executable', $exe)
    Check ($offlineFriends.Code -ne 0 -and $offlineFriends.Text -match 'needs online play') '-Friends with -Offline is refused'
    Check ($source -match 'online-friends\.mjs' -and $source -notmatch 'seed') '-Friends uses the normal friend protocol, no seeding'

    function Get-ResourceLines($result) { @([regex]::Matches($result.Text, 'Resources:\s+(light|normal)') | ForEach-Object { $_.Groups[1].Value }) }
    $light4 = Invoke-Instance @('-Players', '4', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($light4.Code -eq 0 -and (Get-ResourceLines $light4) -join ',' -eq 'normal,light,light,light') '-Players 4: Alpha normal, Bravo, Charlie and Delta light'
    Check ($light4.Text -match 'light: windowed 1280x720, render 720p, SMAA, FSR 1 and RCAS off, audio off') '-Players 4 light instances run 720p in a 1280x720 window with audio off'
    $light3 = Invoke-Instance @('-Players', '3', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ((Get-ResourceLines $light3) -join ',' -eq 'normal,light,light') '-Players 3: Alpha normal, the others light'
    $normal2 = Invoke-Instance @('-Players', '2', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($normal2.Code -eq 0 -and (Get-ResourceLines $normal2) -join ',' -eq 'normal,normal') '-Players 2 keeps both instances at normal quality'
    $single = Invoke-Instance @('-Name', $name, '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ((Get-ResourceLines $single) -join ',' -eq 'normal') 'a single -Name instance stays normal'
    Check ((Get-ResourceLines (Invoke-Instance @('-Players', '4', '-Resources', 'Normal', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom))) -join ',' -eq 'normal,normal,normal,normal') '-Resources Normal keeps four instances normal'
    Check ((Get-ResourceLines (Invoke-Instance @('-Players', '2', '-Resources', 'Light', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom))) -join ',' -eq 'normal,light') '-Resources Light makes Bravo light even with two players'
    Check ((Get-ResourceLines (Invoke-Instance @('-Name', $name, '-Resources', 'Light', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom))) -join ',' -eq 'light') '-Resources Light lightens a single instance'
    $lowest = Invoke-Instance @('-Players', '4', '-LightResolution', '480p', '-WhatIf', '-Executable', $exe, '-CopyFrom', $copyFrom)
    Check ($lowest.Text -match 'light: windowed 854x480, render 480p') '-LightResolution 480p is the lowest light mode'
    Check ((Invoke-Instance @('-Players', '4', '-LightResolution', '360p', '-WhatIf', '-Executable', $exe)).Code -ne 0) 'a light resolution the launcher does not offer is rejected'
    Check ((Invoke-Instance @('-Players', '4', '-Resources', 'Max', '-WhatIf', '-Executable', $exe)).Code -ne 0) 'an unknown -Resources value is rejected'

    Check ($source -match 'ALN_AUDIO = if \(\$instance\.Light\) \{ ''none'' \} else \{ \$null \}') 'ALN_AUDIO=none is set for light instances only'
    Check ($source -match "'ALN_AUDIO'") 'ALN_AUDIO is restored with the rest of the environment'
    Check (([regex]::Matches($source, 'Update-InstanceSettings \$settingsFile')).Count -eq 1 -and $source -match "\`$settingsFile = Join-Path \`$user 'alienation\.ini'") 'settings are changed only in the instance folder, never in the main profile'
    $launcherSettings = Get-Content -LiteralPath (Join-Path $root 'launcher\settings.cpp') -Raw
    Check ($launcherSettings -match '\{kRenderResolutionKey, kNativeResolution\}' -and $launcherSettings -match '\{kDisplayModeKey, "windowed"\}' -and
           $launcherSettings -match '\{kAntiAliasingKey, "0"\}' -and $launcherSettings -match '\{kWindowWidthKey, "1920"\}') 'the launcher defaults the light profile departs from are unchanged'

    $scriptAst = [Management.Automation.Language.Parser]::ParseFile($target, [ref]$null, [ref]$null)
    $profileFunctions = @($scriptAst.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $n.Name -in 'Get-LightValues', 'Get-IniValue', 'Set-IniValue', 'Update-InstanceSettings' }, $true))
    Check ($profileFunctions.Count -eq 4) 'the light profile is four helper functions'
    if ($profileFunctions.Count -eq 4) {
      try {
        . ([scriptblock]::Create(($profileFunctions | ForEach-Object { $_.Extent.Text }) -join "`n"))
        $profiles = Join-Path $work 'profiles'
        New-Item -ItemType Directory -Force -Path $profiles | Out-Null
        $mainIni = Join-Path $profiles 'main.ini'
        $original = "# Alienation user settings`ndisplay_mode = borderless`nsmaa = 1`nrender_resolution = native`nwindow_width = 1920`nonline_name = Saulo`n"
        [IO.File]::WriteAllText($mainIni, $original)
        $values = Get-LightValues '720p'
        Check (($values.Keys -join ',') -eq 'display_mode,window_width,window_height,render_resolution,smaa,fsr1,rcas,anisotropy' -and
               $values['render_resolution'] -eq '720p' -and $values['window_width'] -eq '1280' -and $values['window_height'] -eq '720') 'the light values are the supported display, window, resolution and filter keys'
        Check ((Get-LightValues '480p')['window_width'] -eq '854' -and (Get-LightValues '480p')['window_height'] -eq '480') '480p light values use an 854x480 window'

        $bravoIni = Join-Path $profiles 'bravo.ini'
        $charlieIni = Join-Path $profiles 'charlie.ini'
        Copy-Item -LiteralPath $mainIni -Destination $bravoIni
        Copy-Item -LiteralPath $mainIni -Destination $charlieIni
        $bravoRestore = Join-Path $profiles 'bravo.json'
        $charlieRestore = Join-Path $profiles 'charlie.json'
        Check ((Update-InstanceSettings $bravoIni $bravoRestore $values $true) -eq 'light') 'applying the light profile reports light'
        $lines = [IO.File]::ReadAllLines($bravoIni)
        Check ((Get-IniValue $lines 'render_resolution') -eq '720p' -and (Get-IniValue $lines 'display_mode') -eq 'windowed' -and
               (Get-IniValue $lines 'smaa') -eq '0' -and (Get-IniValue $lines 'window_height') -eq '720' -and (Get-IniValue $lines 'fsr1') -eq '0') 'the light values are written into the instance settings'
        Check ((Get-IniValue $lines 'online_name') -eq 'Saulo' -and $lines[0] -eq '# Alienation user settings') 'other settings and comments stay'
        Check ([IO.File]::ReadAllText($mainIni) -eq $original -and [IO.File]::ReadAllText($charlieIni) -eq $original) 'the main profile and another instance are untouched'
        Check ((Update-InstanceSettings $bravoIni $bravoRestore (Get-LightValues '480p') $true) -eq 'light' -and (Get-IniValue ([IO.File]::ReadAllLines($bravoIni)) 'render_resolution') -eq '480p') 'a second light start changes the resolution'
        Check ((Update-InstanceSettings $charlieIni $charlieRestore $values $true) -eq 'light' -and (Get-IniValue ([IO.File]::ReadAllLines($charlieIni)) 'render_resolution') -eq '720p') 'each instance has its own profile'

        $deltaIni = Join-Path $profiles 'delta.ini'
        Copy-Item -LiteralPath $mainIni -Destination $deltaIni
        Check ((Update-InstanceSettings $deltaIni (Join-Path $profiles 'delta.json') $values $false) -eq 'normal' -and [IO.File]::ReadAllText($deltaIni) -eq $original) 'a normal start of an instance that never was light changes nothing'

        $edited = New-Object 'System.Collections.Generic.List[string]'
        foreach ($line in [IO.File]::ReadAllLines($bravoIni)) { $edited.Add($line) }
        Set-IniValue $edited 'window_height' '600'
        [IO.File]::WriteAllLines($bravoIni, $edited)
        $restored = Update-InstanceSettings $bravoIni $bravoRestore $values $false
        $after = [IO.File]::ReadAllLines($bravoIni)
        Check ($restored -eq 'restored' -and (Get-IniValue $after 'render_resolution') -eq 'native' -and (Get-IniValue $after 'display_mode') -eq 'borderless' -and
               (Get-IniValue $after 'smaa') -eq '1' -and (Get-IniValue $after 'window_width') -eq '1920') 'a normal start brings the earlier values back'
        Check ((Get-IniValue $after 'window_height') -eq '600') 'a value changed since the light start is kept'
        Check ($null -eq (Get-IniValue $after 'fsr1') -and $null -eq (Get-IniValue $after 'anisotropy')) 'keys the instance never had are removed again'
        Check (-not (Test-Path -LiteralPath $bravoRestore) -and (Test-Path -LiteralPath "$bravoRestore.restored")) 'the restore record is set aside, not deleted'
        Check ((Update-InstanceSettings $charlieIni $charlieRestore $values $false) -eq 'restored' -and ([IO.File]::ReadAllLines($charlieIni) -join "`n") -eq $original.TrimEnd()) 'an untouched light instance comes back to its exact earlier settings'
      } catch { Check $false "the light profile helpers failed: $_" }
    }

    $ast = [Management.Automation.Language.Parser]::ParseFile($target, [ref]$null, [ref]$null)
    $parameterNames = @($ast.ParamBlock.Parameters | Where-Object { $_.StaticType -ne [string] } | ForEach-Object { $_.Name.VariablePath.UserPath.ToLowerInvariant() })
    $assigned = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left -is [Management.Automation.Language.VariableExpressionAst] }, $true) |
        ForEach-Object { $_.Left.VariablePath.UserPath }
    $collisions = @($assigned | Where-Object { $_.ToLowerInvariant() -in $parameterNames })
    Check ($collisions.Count -eq 0) "no script variable is assigned under a parameter name (case-insensitive): $($collisions -join ', ')"

    $helper = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-FriendTool' }, $true)
    Check ($null -ne $helper) 'the friend tool call is one helper'
    if ($helper) {
        $script:RepoRoot = $root
        . ([scriptblock]::Create($helper.Extent.Text))
        $fakeNode = Join-Path $work 'fake-node.cmd'
        foreach ($code in 0, 3) {
            Set-Content -LiteralPath $fakeNode -Value "@echo off`r`necho fake friend tool %*`r`nexit /b $code"
            $result = @(Invoke-FriendTool -Node $fakeNode -Lobby 'ws://x/' -PlayerArgs @('--player', 'Alpha=a.ini', '--player', 'Bravo=b.ini'))
            Check ($result.Count -eq 1 -and $result[0] -is [int] -and $result[0] -eq $code) "the friend helper returns exactly the exit code $code as one integer"
        }
    }

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
