<#
.SYNOPSIS
  Starts one isolated test instance of the game for local online testing on one PC: its own user folder (settings,
  saves, online identity), its own log folder and, with -Build, its own game build.
.DESCRIPTION
  Instance "a" uses user\test-client-a and build\logs\test-client-a, instance "b" the same with "b", and so on. The first
  start copies alienation.ini and the saves from the main user folder (-CopyFrom) so both instances can play right away;
  later starts keep the instance's own files. Each instance makes its own online identity (online_identity.ini) on
  first use and signs in as its own online id, so two instances never share an account.
  Online play is on for the started process only (ALN_ONLINE=1) unless -Offline; the main user folder, the instance's
  alienation.ini and the calling shell's environment are left as they were. Both instances use the production lobby
  unless -Lobby names another one (ALN_LOBBY, for example the development lobby of tools\online-lobby.ps1); the second
  instance's P2P socket takes a free UDP port when 3658 is taken. -NoUpnp leaves the router alone (ALN_UPNP=0).
  -Build pins the instance to one prepared build (ALN_GAME_BUILD for this instance only); only instances of the same
  build ever meet.
.EXAMPLE
  tools\online-instance.ps1 -Name a -WhatIf          # prints the folders and starts nothing
  tools\online-instance.ps1 -Name a                   # terminal 1
  tools\online-instance.ps1 -Name b -OnlineName Bravo # terminal 2
  tools\online-instance.ps1 -Name a -Lobby ws://127.0.0.1:8790/   # against the development lobby
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9-]{1,16}$')][string]$Name,
    [ValidateSet('', 'CUSA00062-1.00', 'CUSA00062-1.06', 'CUSA04430-1.00')][string]$Build = '',
    [ValidatePattern('^[A-Za-z0-9_-]{0,16}$')][string]$OnlineName = '',
    [ValidatePattern('^(wss?://[^\s]+)?$')][string]$Lobby = '',
    [switch]$NoUpnp,
    [string]$CopyFrom = '',
    [string]$Executable = '',
    [switch]$Offline
)

. (Join-Path $PSScriptRoot '_common.ps1')

if (-not $Executable) { $Executable = Join-Path $script:RepoRoot 'build\cmake\clang64\bin\alienation.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { Stop-Tool "Not built yet: $Executable" }
if (-not $CopyFrom) { $CopyFrom = Join-Path $script:RepoRoot 'user' }
if ($Build) {
    $app0 = Join-Path $script:RepoRoot "game\prepared\$Build"
    $boot = Join-Path $script:RepoRoot "build\image\$Build\boot.bin"
    if (-not (Test-Path -LiteralPath $app0) -or -not (Test-Path -LiteralPath $boot)) { Stop-Tool "Build $Build is not prepared." }
}

$user = Join-Path $script:RepoRoot "user\test-client-$Name"
$logs = Join-Path $script:RepoRoot "build\logs\test-client-$Name"
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

$instanceVariables = 'PATH', 'ALN_ONLINE', 'ALN_ONLINE_NAME', 'ALN_GAME_BUILD', 'ALN_LOBBY', 'ALN_UPNP'
$previous = @{}
foreach ($variable in $instanceVariables) { $previous[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
try {
    $env:ALN_ONLINE = if ($Offline) { $null } else { '1' }
    $env:ALN_ONLINE_NAME = if ($OnlineName) { $OnlineName } else { $null }
    $env:ALN_GAME_BUILD = if ($Build) { $Build } else { $null }
    $env:ALN_LOBBY = if ($Lobby) { $Lobby } else { $null }
    $env:ALN_UPNP = if ($NoUpnp) { '0' } else { $null }

    Write-Step "Test instance $($Name.ToUpper())$(if ($OnlineName) { " as $OnlineName" })"
    Write-Host "  Online:      $(if ($Offline) { 'off' } elseif ($Lobby) { "on, lobby $Lobby" } else { 'on, production lobby' })$(if ($NoUpnp -and -not $Offline) { ', UPnP off' })"
    Write-Host "  Game build:  $(if ($Build) { $Build } else { 'the active build' })"
    Write-Host "  user folder: $user"
    Write-Host "  logs:        $logs"
    if ($PSCmdlet.ShouldProcess($Executable, "Start test instance $Name")) {
        $tc = Resolve-Toolchain 'auto'
        $env:PATH = "$($tc.Bin);$env:PATH"
        Start-Process -FilePath $Executable -ArgumentList @('--user', "`"$user`"", '--logs', "`"$logs`"", '--launcher') -WorkingDirectory (Split-Path -Parent $Executable)
    }
} finally {
    foreach ($variable in $instanceVariables) { [Environment]::SetEnvironmentVariable($variable, $previous[$variable], 'Process') }
}
