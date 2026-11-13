# The local input of a release that is not in Git: the reviewed AppImage runtime, kept in cache\ (ignored, never
# mirrored or packaged). Dot-sourced by tools\package-release.ps1 and tests\release_inputs_tests.ps1.

function Get-PinnedRuntimeSha256([string]$Root) {
    $baseline = Get-Content -LiteralPath (Join-Path $Root 'patches\appimage-runtime\baseline.env') -Raw
    if ($baseline -notmatch '(?m)^RUNTIME_SHA256=([0-9a-f]{64})\s*$') { throw 'RUNTIME_SHA256 not found in patches\appimage-runtime\baseline.env' }
    return $Matches[1]
}

function Test-PinnedRuntime([string]$File, [string]$Sha256) {
    (Test-Path -LiteralPath $File -PathType Leaf) -and (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha256
}

# cache\appimage-runtime\runtime-x86_64 when it has the pinned SHA-256. Otherwise $Import is asked to place a copy there
# (from a WSL packaging cache); a file with any other hash is deleted and $null returned, so nothing unreviewed is used.
function Resolve-ReviewedRuntime([string]$Root, [scriptblock]$Import) {
    $sha256 = Get-PinnedRuntimeSha256 $Root
    $file = Join-Path $Root 'cache\appimage-runtime\runtime-x86_64'
    if (Test-PinnedRuntime $file $sha256) { return $file }
    Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    if ($Import) {
        New-Item -ItemType Directory -Force (Split-Path $file -Parent) | Out-Null
        & $Import $file $sha256
        if (Test-PinnedRuntime $file $sha256) { return $file }
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    }
    return $null
}
