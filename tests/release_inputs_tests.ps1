# The reviewed AppImage runtime (tools\release-inputs.ps1) on a scratch tree: only the runtime with the pinned SHA-256 is
# ever used, from the cache or an import. Also the pins, the build script's override, the embedded-runtime check and
# that tools\package-release.ps1 needs no argument and names no temporary path. Nothing is built, packaged or launched.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $root 'tools\release-inputs.ps1')

$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name" -ForegroundColor Red
    }
}
function Read-Source([string]$Relative) { Get-Content -LiteralPath (Join-Path $root $Relative) -Raw }

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("aln-release-inputs-" + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force $scratch | Out-Null

    $runtimeFile = Join-Path $scratch 'reviewed-runtime'
    [IO.File]::WriteAllText($runtimeFile, 'reviewed runtime')
    $pinned = (Get-FileHash -LiteralPath $runtimeFile -Algorithm SHA256).Hash.ToLowerInvariant()
    New-Item -ItemType Directory -Force (Join-Path $scratch 'patches\appimage-runtime') | Out-Null
    [IO.File]::WriteAllText((Join-Path $scratch 'patches\appimage-runtime\baseline.env'), "RUNTIME_TAG=x`nRUNTIME_SHA256=$pinned`n")
    Check ((Get-PinnedRuntimeSha256 $scratch) -eq $pinned) 'the pin is read from baseline.env'
    $cached = Join-Path $scratch 'cache\appimage-runtime\runtime-x86_64'

    $script:imports = 0
    $good = { param($File, $Sha256) $script:imports++; Copy-Item -LiteralPath $runtimeFile $File }
    $bad = { param($File, $Sha256) $script:imports++; [IO.File]::WriteAllText($File, 'a rebuilt runtime') }
    $none = { param($File, $Sha256) $script:imports++ }

    Check ($null -eq (Resolve-ReviewedRuntime $scratch $none)) 'no copy anywhere: no runtime'
    Check ($null -eq (Resolve-ReviewedRuntime $scratch $bad) -and -not (Test-Path -LiteralPath $cached)) 'an imported file with another hash is refused and removed'
    Check ((Resolve-ReviewedRuntime $scratch $good) -eq $cached) 'an imported reviewed runtime is cached'
    $script:imports = 0
    Check ((Resolve-ReviewedRuntime $scratch $bad) -eq $cached -and $script:imports -eq 0) 'a cached reviewed runtime is used without importing'
    [IO.File]::WriteAllText($cached, 'replaced by something else')
    Check ($null -eq (Resolve-ReviewedRuntime $scratch $none) -and -not (Test-Path -LiteralPath $cached)) 'a cached file with another hash is refused and removed'
    Check ($null -eq (Resolve-ReviewedRuntime $scratch $null)) 'without an import nothing is used'

    $baseline = Read-Source 'patches\appimage-runtime\baseline.env'
    foreach ($pin in 'RUNTIME_TAG=20251108', 'RUNTIME_COMMIT=dd6cebedcbddde9c82f89b011e8e1d40b6e43868', 'ALPINE_VERSION=3.21.0',
        'ALPINE_SHA256=55ea3e5a7c2c35e6268c5dcbb8e45a9cd5b0e372e7b4e798499a526834f7ed90',
        'RUNTIME_SHA256=9e458da0e30e2dd06a206af0f345d98a872bf7c1ce69eba31d44e139488e4ab8') {
        Check ($baseline -match "(?m)^$([regex]::Escape($pin))\s*$") "baseline.env pins $pin"
    }
    $patch = Join-Path $root 'patches\appimage-runtime\0001-extract-and-run-when-fuse-fails.patch'
    Check ((Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash.ToLowerInvariant() -eq '9abff6b867bb89a228df024035f28b229ca1149ea1cc172ebc10b2744b798a4e') 'the extract-and-run patch is the reviewed one'

    $release = Read-Source 'tools\package-release.ps1'
    Check ($release -notmatch '(?i)Mandatory') 'tools\package-release.ps1 takes no mandatory argument'
    Check ($release -notmatch '(?i)AppData\\Local\\Temp|\\Temp\\claude|scratchpad') 'tools\package-release.ps1 names no temporary folder'
    Check ($release -notmatch 'dragons-crown-pro-pc|odin-sphere|13-sentinels') "tools\package-release.ps1 names no other project's cache"
    Check ($release -match 'Resolve-ReviewedRuntime \$root' -and $release -match 'ALN_APPIMAGE_RUNTIME=\$runtimeWsl') 'tools\package-release.ps1 resolves the runtime and hands it to the Linux half'
    Check ((Get-Content -LiteralPath (Join-Path $root '.gitignore') -Raw) -match '(?m)^/cache/$') 'cache\ is ignored by Git'

    $import = Read-Source 'tools\import-appimage-runtime.sh'
    Check ($import -match 'sha256sum -- "\$candidate"' -and $import -match 'sha256sum -- "\$target\.part"') 'the WSL import checks the hash before and after copying'
    $build = Read-Source 'tools\build-appimage-runtime.sh'
    Check ($build -match 'not the reviewed \$RUNTIME_SHA256') 'a runtime built or given with another hash is refused'
    Check ($build -match 'ALN_APPIMAGE_RUNTIME' -and $build -notmatch 'TSR_|DCP_|OSL_') 'the build script takes the reviewed runtime through ALN_APPIMAGE_RUNTIME only'
    $package = Read-Source 'tools\package-linux.sh'
    Check ($package -match '(?s)--runtime-file "\$runtime".*--check "\$dist/\$name\.AppImage\.new"\s+verify_embedded_runtime "\$dist/\$name\.AppImage\.new" "\$runtime"') 'the written AppImage is checked against the pinned runtime file'
    Check ($package -match '\.digest_md5' -and $package -match '\.upd_info' -and $package -match '\.sha256_sig' -and $package -match '\.sig_key') 'only the sections appimagetool fills may differ'
    foreach ($file in 'tools\package-linux.sh', 'tools\package-release-linux.sh') {
        Check ((Read-Source $file) -match "--exclude '/cache'|\|cache\|") "$file keeps cache\ out of the mirror and the package"
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failures) {
    Write-Host "FAILED: $script:failures of $script:checks checks" -ForegroundColor Red
    exit 1
}
Write-Host "release inputs: $script:checks checks passed (the pinned AppImage runtime only, the same one as the sibling ports)"
exit 0
