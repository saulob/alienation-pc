<#
.SYNOPSIS
  Starts isolated test instances of the game for local online testing on one PC: each has its own user folder
  (settings, saves, online identity), its own log folder and, with -Build, its own game build.
.DESCRIPTION
  Instance "alpha" uses user\test-client-alpha and build\logs\test-client-alpha, instance "bravo" the same with
  "bravo", and so on. The first start copies alienation.ini and the saves from the main user folder (-CopyFrom) so
  every instance can play right away; later starts keep the instance's own files. Each instance makes its own online
  identity (online_identity.ini) on first use and signs in as its own online id, so two instances never share an
  account. The Online name defaults to the callsign (a or alpha -> Alpha, b -> Bravo, c -> Charlie, d -> Delta; any
  other name as given): the lobby refuses two members with one name in a room, and the copied settings would otherwise
  give every instance the main profile's name.
  -Players 2, 3 or 4 starts Alpha, Bravo, Charlie and Delta in one call.
  Online play is on for the started processes only (ALN_ONLINE=1) unless -Offline; the main user folder, the instance's
  alienation.ini and the calling shell's environment are left as they were. Every instance uses the production lobby
  unless -Lobby names another one (ALN_LOBBY, for example the development lobby of tools\online-lobby.ps1); a second
  instance's P2P socket takes a free UDP port when 3658 is taken. -NoUpnp leaves the router alone (ALN_UPNP=0).
  Every instance runs the active game build unless -Build pins it to one prepared build (ALN_GAME_BUILD for these
  instances only); only instances of the same build ever meet.
  -Friends (with -Players) makes every pair of the started instances friends once they have their online identities:
  one sends a friend request by friend code and the other accepts it, through the same lobby and with the normal friend
  protocol (tools\online-friends.mjs, Node.js 22 or later). Being in the same squad or public lobby makes nobody a friend.
  Resource-light instances: three or four full games on one PC can exhaust Windows commit and video memory. With
  -Players 3 or 4 (or -Resources Light) every instance except the first (Alpha) runs a light test profile in its own
  alienation.ini: windowed, render resolution -LightResolution (720p; 480p is the lowest), window of that size, SMAA,
  FSR 1 and RCAS off, texture filtering from the game, and ALN_AUDIO=none (silent timer sink) for its process. The
  windows stay visible and playable. -Players 2, a single -Name and -Resources Normal leave the settings as they are, and
  an instance that was light comes back to its earlier values (only those it still has at the light values). The main
  user folder and the normal defaults are never touched.
.EXAMPLE
  tools\online-instance.ps1 -Name alpha -WhatIf      # prints the folders and starts nothing
  tools\online-instance.ps1 -Name alpha              # terminal 1, online as Alpha
  tools\online-instance.ps1 -Name bravo              # terminal 2, online as Bravo
  tools\online-instance.ps1 -Players 4               # Alpha, Bravo, Charlie and Delta
  tools\online-instance.ps1 -Players 2 -Lobby ws://127.0.0.1:8790/   # against the development lobby
  tools\online-instance.ps1 -Players 2 -Friends      # Alpha and Bravo, then a friend request from Alpha that Bravo accepts
  tools\online-instance.ps1 -Players 4 -Friends      # Alpha, Bravo, Charlie and Delta, all friends of each other
#>
[CmdletBinding(SupportsShouldProcess = $true, DefaultParameterSetName = 'One')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'One')][ValidatePattern('^[A-Za-z0-9-]{1,16}$')][string]$Name,
    [Parameter(Mandatory = $true, ParameterSetName = 'Squad')][ValidateRange(2, 4)][int]$Players,
    [ValidateSet('', 'CUSA00062-1.00', 'CUSA00062-1.06', 'CUSA04430-1.00')][string]$Build = '',
    [Parameter(ParameterSetName = 'One')][ValidatePattern('^[A-Za-z0-9_-]{0,16}$')][string]$OnlineName = '',
    [ValidatePattern('^(wss?://[^\s]+)?$')][string]$Lobby = '',
    [switch]$NoUpnp,
    [string]$CopyFrom = '',
    [string]$Executable = '',
    [switch]$Offline,
    [Parameter(ParameterSetName = 'Squad')][switch]$Friends,
    [ValidateSet('Auto', 'Light', 'Normal')][string]$Resources = 'Auto',
    [ValidateSet('480p', '720p')][string]$LightResolution = '720p'
)

. (Join-Path $PSScriptRoot '_common.ps1')

$callsigns = [ordered]@{ a = 'Alpha'; b = 'Bravo'; c = 'Charlie'; d = 'Delta' }
function Get-Callsign([string]$Instance) {
    $key = $Instance.ToLowerInvariant()
    if ($callsigns.Contains($key)) { return $callsigns[$key] }
    foreach ($callsign in $callsigns.Values) { if ($callsign.ToLowerInvariant() -eq $key) { return $callsign } }
    return $null
}

function Get-DefaultLobbyUrl {
    $header = Get-Content -Raw -LiteralPath (Join-Path $script:RepoRoot 'src\net\online_config.h')
    return [regex]::Match($header, 'kDefaultLobbyUrl\[\] = "([^"]+)"').Groups[1].Value
}

function Get-LightValues([string]$Resolution) {
    $size = if ($Resolution -eq '480p') { @('854', '480') } else { @('1280', '720') }
    return [ordered]@{ display_mode = 'windowed'; window_width = $size[0]; window_height = $size[1]; render_resolution = $Resolution
        smaa = '0'; fsr1 = '0'; rcas = '0'; anisotropy = '0' }
}

function Get-IniValue([string[]]$Lines, [string]$Key) {
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$'
    foreach ($line in $Lines) { if ($line -match $pattern) { return $Matches[1] } }
    return $null
}

function Set-IniValue([System.Collections.Generic.List[string]]$Lines, [string]$Key, [AllowNull()]$Value) {
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -notmatch $pattern) { continue }
        if ($null -eq $Value) { $Lines.RemoveAt($i) } else { $Lines[$i] = "$Key = $Value" }
        return
    }
    if ($null -ne $Value) { $Lines.Add("$Key = $Value") }
}

# Applies or lifts the light test profile in one instance's settings file. $Restore remembers each key's earlier value
# and the value written; a key is only put back while it still holds the written value.
function Update-InstanceSettings([string]$Settings, [string]$Restore, [System.Collections.IDictionary]$LightValues, [bool]$Light) {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $lines = New-Object 'System.Collections.Generic.List[string]'
    if (Test-Path -LiteralPath $Settings) { foreach ($line in [IO.File]::ReadAllLines($Settings)) { $lines.Add($line) } }
    $saved = @{}
    if (Test-Path -LiteralPath $Restore) {
        $recorded = ConvertFrom-Json (Get-Content -LiteralPath $Restore -Raw)
        foreach ($entry in @($recorded)) { $saved[$entry.key] = $entry }
    }
    if ($Light) {
        $entries = @()
        foreach ($key in $LightValues.Keys) {
            $original = if ($saved.ContainsKey($key)) { $saved[$key].original } else { Get-IniValue $lines $key }
            $entries += [pscustomobject]@{ key = $key; original = $original; applied = [string]$LightValues[$key] }
            Set-IniValue $lines $key ([string]$LightValues[$key])
        }
        [IO.File]::WriteAllText($Restore, (ConvertTo-Json -InputObject @($entries)), $utf8)
        [IO.File]::WriteAllLines($Settings, $lines, $utf8)
        return 'light'
    }
    if ($saved.Count -eq 0) { return 'normal' }
    foreach ($entry in $saved.Values) {
        if ((Get-IniValue $lines $entry.key) -eq $entry.applied) { Set-IniValue $lines $entry.key $entry.original }
    }
    [IO.File]::WriteAllLines($Settings, $lines, $utf8)
    Move-Item -LiteralPath $Restore -Destination "$Restore.restored" -Force
    return 'restored'
}

function Invoke-FriendTool([string]$Node, [string]$Lobby, [string[]]$PlayerArgs) {
    & $Node (Join-Path $script:RepoRoot 'tools\online-friends.mjs') --lobby $Lobby @PlayerArgs | Out-Host
    return [int]$LASTEXITCODE
}

if ($Friends -and $Offline) { Stop-Tool '-Friends needs online play: leave out -Offline.' }
$node = $null
if ($Friends -and -not $WhatIfPreference) {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node -or [version]((& $node.Source --version).TrimStart('v')) -lt [version]'22.4.0') {
        Stop-Tool '-Friends needs Node.js 22.4 or later (https://nodejs.org).'
    }
}
if (-not $Executable) { $Executable = Join-Path $script:RepoRoot 'build\cmake\clang64\bin\alienation.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { Stop-Tool "Not built yet: $Executable" }
if (-not $CopyFrom) { $CopyFrom = Join-Path $script:RepoRoot 'user' }
if ($Build) {
    $app0 = Join-Path $script:RepoRoot "game\prepared\$Build"
    $boot = Join-Path $script:RepoRoot "build\image\$Build\boot.bin"
    if (-not (Test-Path -LiteralPath $app0) -or -not (Test-Path -LiteralPath $boot)) { Stop-Tool "Build $Build is not prepared." }
}

$lightSquad = $Resources -eq 'Light' -or ($Resources -eq 'Auto' -and $PSCmdlet.ParameterSetName -eq 'Squad' -and $Players -ge 3)
$lightValues = Get-LightValues $LightResolution
$instances = @()
if ($PSCmdlet.ParameterSetName -eq 'Squad') {
    foreach ($callsign in @($callsigns.Values)[0..($Players - 1)]) {
        $instances += [pscustomobject]@{ Folder = $callsign.ToLowerInvariant(); OnlineName = $callsign; Light = ($lightSquad -and $callsign -ne 'Alpha') }
    }
} else {
    $callsign = Get-Callsign $Name
    $folder = if ($callsign) { $callsign.ToLowerInvariant() } else { $Name }
    $shown = if ($OnlineName) { $OnlineName } elseif ($callsign) { $callsign } else { $Name }
    $instances += [pscustomobject]@{ Folder = $folder; OnlineName = $shown; Light = ($Resources -eq 'Light') }
}

$instanceVariables = 'PATH', 'ALN_ONLINE', 'ALN_ONLINE_NAME', 'ALN_GAME_BUILD', 'ALN_LOBBY', 'ALN_UPNP', 'ALN_AUDIO'
$previous = @{}
foreach ($variable in $instanceVariables) { $previous[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
try {
    $env:ALN_ONLINE = if ($Offline) { $null } else { '1' }
    $env:ALN_GAME_BUILD = if ($Build) { $Build } else { $null }
    $env:ALN_LOBBY = if ($Lobby) { $Lobby } else { $null }
    $env:ALN_UPNP = if ($NoUpnp) { '0' } else { $null }
    if (-not $WhatIfPreference) {
        $tc = Resolve-Toolchain 'auto'
        $env:PATH = "$($tc.Bin);$env:PATH"
    }

    foreach ($instance in $instances) {
        $user = Join-Path $script:RepoRoot "user\test-client-$($instance.Folder)"
        $logs = Join-Path $script:RepoRoot "build\logs\test-client-$($instance.Folder)"
        if (-not (Test-Path -LiteralPath $user)) {
            if ($PSCmdlet.ShouldProcess($user, 'Create the instance folder')) {
                New-Item -ItemType Directory -Force $user | Out-Null
                $settings = Join-Path $CopyFrom 'alienation.ini'
                if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination $user }
                $saves = Join-Path $CopyFrom 'savedata'
                if (Test-Path -LiteralPath $saves) { Copy-Item -LiteralPath $saves -Destination $user -Recurse }
                Write-Ok "New instance folder $user (settings and saves copied from $CopyFrom)"
            }
        }
        if ($PSCmdlet.ShouldProcess($logs, 'Create the log folder')) { New-Item -ItemType Directory -Force $logs | Out-Null }
        $env:ALN_ONLINE_NAME = $instance.OnlineName
        $env:ALN_AUDIO = if ($instance.Light) { 'none' } else { $null }
        $settingsFile = Join-Path $user 'alienation.ini'
        $restoreFile = Join-Path $user 'light_profile.json'
        if ($PSCmdlet.ShouldProcess($settingsFile, $(if ($instance.Light) { 'Apply the light test profile' } else { 'Lift the light test profile if it was applied' }))) {
            $state = Update-InstanceSettings $settingsFile $restoreFile $lightValues $instance.Light
            if ($state -eq 'restored') { Write-Ok "$($instance.OnlineName): earlier settings restored (the light test profile is lifted)" }
        }

        Write-Step "Test instance $($instance.Folder.ToUpper()) as $($instance.OnlineName)"
        Write-Host "  Online:      $(if ($Offline) { 'off' } elseif ($Lobby) { "on, lobby $Lobby" } else { 'on, production lobby' })$(if ($NoUpnp -and -not $Offline) { ', UPnP off' })"
        Write-Host "  Online name: $($instance.OnlineName)"
        Write-Host "  Game build:  $(if ($Build) { $Build } else { 'the active build' })"
        Write-Host "  Resources:   $(if ($instance.Light) { "light: windowed $($lightValues.window_width)x$($lightValues.window_height), render $($lightValues.render_resolution), SMAA, FSR 1 and RCAS off, audio off" } else { 'normal: settings as in the instance profile' })"
        Write-Host "  user folder: $user"
        Write-Host "  logs:        $logs"
        if ($PSCmdlet.ShouldProcess($Executable, "Start test instance $($instance.Folder)")) {
            Start-Process -FilePath $Executable -ArgumentList @('--user', "`"$user`"", '--logs', "`"$logs`"", '--launcher') -WorkingDirectory (Split-Path -Parent $Executable)
        }
    }

    if ($Friends) {
        $url = if ($Lobby) { $Lobby } else { Get-DefaultLobbyUrl }
        $names = @($instances | ForEach-Object { $_.OnlineName })
        Write-Step "Friends: $($names -join ', ') (a friend request and its acceptance, through $url)"
        if ($PSCmdlet.ShouldProcess($url, 'Make the test instances friends')) {
            $playerArgs = @()
            foreach ($instance in $instances) {
                $identity = Join-Path $script:RepoRoot "user\test-client-$($instance.Folder)\online_identity.ini"
                # The launcher makes the identity when it starts.
                for ($i = 0; $i -lt 120 -and -not (Test-Path -LiteralPath $identity); $i++) { Start-Sleep -Milliseconds 500 }
                if (-not (Test-Path -LiteralPath $identity)) { Stop-Tool "No online identity for $($instance.OnlineName) yet ($identity): start it once, then run this again." }
                $playerArgs += @('--player', "$($instance.OnlineName)=$identity")
            }
            $friendCode = Invoke-FriendTool -Node $node.Source -Lobby $url -PlayerArgs $playerArgs
            if ($friendCode -ne 0) {
                Stop-Tool "Friends step failed: online-friends.mjs exited with code $friendCode. Add them by hand: Online page, Friends tab, each other's friend code."
            }
            Write-Ok "Friends: $($names -join ' and ') are now friends"
        }
    }
} finally {
    foreach ($variable in $instanceVariables) { [Environment]::SetEnvironmentVariable($variable, $previous[$variable], 'Process') }
}
