# Deploys the lobby Worker (server\lobby, Worker "alienation-lobby") with the Wrangler already installed and signed in on
# this PC. It installs nothing, never logs in, stores no token and never touches secrets or bindings: Wrangler's own
# configuration (server\lobby\wrangler.toml) decides where it goes.
#   .\tools\worker.ps1            wrangler deploy
#   .\tools\worker.ps1 -DryRun    wrangler deploy --dry-run (bundles, uploads nothing)
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("worker: $message")
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workerDir = Join-Path $repoRoot 'server\lobby'

if (-not (Test-Path -LiteralPath $workerDir -PathType Container)) { Fail "missing folder $workerDir" }
$config = Join-Path $workerDir 'wrangler.toml'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) { Fail "missing $config, the Wrangler configuration" }
$configText = Get-Content -Raw -LiteralPath $config
if ($configText -notmatch '(?m)^\s*name\s*=\s*"alienation-lobby"') { Fail "$config does not name the alienation-lobby Worker" }
if ($configText -match '(?m)^\s*main\s*=\s*"([^"]+)"') {
    $entry = Join-Path $workerDir $Matches[1]
    if (-not (Test-Path -LiteralPath $entry -PathType Leaf)) { Fail "missing $entry, the entry point named by wrangler.toml" }
}

# Only a Wrangler that is already there: `npx --no-install` refuses to download one.
$wrangler = Get-Command wrangler -ErrorAction SilentlyContinue
if ($wrangler) {
    $command = @($wrangler.Source)
} elseif (Get-Command npx -ErrorAction SilentlyContinue) {
    $command = @('npx', '--no-install', 'wrangler')
} else {
    Fail 'Wrangler was not found. Install it yourself (npm install -g wrangler) and sign in with wrangler login.'
}
$arguments = @('deploy')
if ($DryRun) { $arguments += '--dry-run' }

Push-Location -LiteralPath $workerDir
try {
    if ($command.Count -gt 1) { & $command[0] @($command[1..($command.Count - 1)] + $arguments) } else { & $command[0] @arguments }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -ne 0) {
    [Console]::Error.WriteLine("worker: wrangler exited with code $code")
    exit $code
}
Write-Host ($(if ($DryRun) { 'Worker bundle checked (dry run, nothing uploaded).' } else { 'Worker deployed.' }))
