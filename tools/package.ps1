<#
.SYNOPSIS
  Packages the tested build as dist\AlienationPC-v<version>-windows-x64 (folder, ZIP and .zip.sha256). Local only:
  nothing is published, tagged or uploaded.
.DESCRIPTION
  Builds with tools\build.ps1 (the one RelWithDebInfo configuration) unless -SkipBuild, then stages alienation.exe
  and trg-updater.exe without their debug information, the MSYS2 DLLs it loads (found by walking its imports), README.txt, LICENSE.txt and
  the licence texts. The debug information goes to dist\<name>-symbols\, which is not packaged.
  The staging folder is checked by tools\check-release-folder.ps1 before the ZIP is written, and the finished ZIP and
  its .zip.sha256 are checked again through an extraction into a temporary folder. The version is the project VERSION in
  CMakeLists.txt.
.PARAMETER SkipBuild
  Packages the existing build\cmake\clang64\bin\alienation.exe and trg-updater.exe.
.PARAMETER Clean
  Passes -Clean to tools\build.ps1.
.PARAMETER VerifyOnly
  Only checks an existing staged folder or ZIP with tools\check-release-folder.ps1, then exits.
.EXAMPLE
  .\tools\package.ps1
  .\tools\package.ps1 -VerifyOnly dist\AlienationPC-v0.7.0-windows-x64
  .\tools\package.ps1 -VerifyOnly dist\AlienationPC-v0.7.0-windows-x64.zip
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$Clean,
    [string]$VerifyOnly
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$checker = Join-Path $PSScriptRoot 'check-release-folder.ps1'
$powershell = (Get-Process -Id $PID).Path

function Invoke-ReleaseCheck([string]$Target) {
    $kind = if ($Target -like '*.zip') { '-Zip' } else { '-Folder' }
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $checker $kind $Target
    return $LASTEXITCODE -eq 0
}

if ($VerifyOnly) {
    if (-not (Invoke-ReleaseCheck $VerifyOnly)) { exit 1 }
    return
}
$loader = Join-Path $root 'build\cmake\clang64\bin\alienation.exe'
$updater = Join-Path $root 'build\cmake\clang64\bin\trg-updater.exe'
$tc = Resolve-Toolchain 'auto'
$bin = $tc.Bin
$prefix = Split-Path $bin -Parent
$msys = Split-Path $prefix -Parent
$pacman = Join-Path $msys 'usr\bin\pacman.exe'
$pacmanDb = Join-Path $msys 'var\lib\pacman\local'
$objcopy = Join-Path $bin 'llvm-objcopy.exe'
$objdump = Join-Path $bin 'llvm-objdump.exe'
$packagePrefix = 'mingw-w64-clang-x86_64-'
# FFmpeg's DLLs come from the LGPL build of gpu\ffmpeg.txt (tools\build.ps1 builds it), never from MSYS2.
$ffmpegPin = Read-FFmpegPin
$ffmpegPrefix = Get-FFmpegPrefix
$ffmpegBin = Join-Path $ffmpegPrefix 'bin'
$ffmpegLicences = Join-Path $ffmpegPrefix 'share\licenses\ffmpeg'
$dllDirs = @($ffmpegBin, $bin)

# Licence texts of the code compiled into alienation.exe from this repository and its fetched dependencies.
$BuiltInLicenses = [ordered]@{
    'shadPS4'       = 'gpu\shadps4\LICENSE'
    'sirit'         = 'gpu\third_party\sirit\LICENSE.txt'
    'SPIRV-Headers' = 'gpu\third_party\sirit\externals\SPIRV-Headers\LICENSE'
    'half'          = 'gpu\third_party\half\LICENSE.txt'
    'SMAA'          = 'gpu\shadps4\video_core\host_shaders\smaa\LICENSE.txt'
    'DejaVu'        = 'gpu\third_party\fonts\LICENSE-DejaVu.txt'
    'NotoSans'      = 'third_party\fonts\LICENSE-NotoSans.txt'
    'NotoSansTC'    = 'third_party\fonts\LICENSE-NotoSansTC.txt'
    'imgui'         = 'gpu\third_party\imgui\LICENSE.txt'
    'magic_enum'    = 'gpu\third_party\magic_enum\LICENSE'
    'miniz'         = 'gpu\third_party\miniz\LICENSE'
    'nanosvg'       = 'launcher\third_party\nanosvg\LICENSE.txt'
    'xbyak'         = 'gpu\third_party\xbyak\COPYRIGHT'
}
# MSYS2 packages compiled in as headers or static libraries; spirv-tools and vulkan-loader only for their licences:
# ANGLE's libGLESv2.dll links its own copies.
$StaticPackages = @('fmt', 'boost', 'robin-map', 'vulkan-memory-allocator', 'vulkan-headers', 'compiler-rt',
    'spirv-tools', 'vulkan-loader')
# The packages whose DLLs may ship, each listed in packaging\licenses\THIRD-PARTY-LICENSES.txt. A DLL from any other
# package stops the packaging until its licence has been reviewed and added there.
$DllPackages = @('sdl3', 'freetype', 'libpng', 'zydis', 'xxhash', 'libc++', 'libwinpthread', 'zlib', 'bzip2',
    'harfbuzz', 'brotli', 'graphite2', 'pcre2', 'glib2', 'gettext-runtime', 'libiconv')
# ANGLE's own files come from gpu\angle-runtime.txt: SDL loads them by name, so no import table leads to them.
$AnglePin = Read-AnglePin
$AngleLicence = 'licenses\angleproject\LICENSE'

# Adds what ANGLE's DLLs in $AngleDir load from $Dir.
function Add-AngleDlls($Found, [string]$AngleDir, [string]$Dir) {
    foreach ($file in Get-AngleDlls $AnglePin) {
        $path = Join-Path $AngleDir $file.Name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $closure = Get-DllClosure $path $Dir $objdump
        foreach ($dllName in $closure.Keys) { if (-not $Found.Contains($dllName)) { $Found[$dllName] = $closure[$dllName] } }
    }
}

function Get-PackageInfo([string]$Short) {
    $name = $packagePrefix + $Short
    $query = & $pacman -Q $name 2>$null
    if ($LASTEXITCODE -ne 0) { Stop-Tool "MSYS2 package $name is not installed" }
    $version = ($query -split ' ')[1]
    $fields = @{}
    $key = $null
    foreach ($line in Get-Content -LiteralPath (Join-Path $pacmanDb "$name-$version\desc")) {
        if ($line -match '^%(\w+)%$') { $key = $Matches[1]; $fields[$key] = @() }
        elseif ($line -and $key) { $fields[$key] += $line }
    }
    $base = if ($fields.ContainsKey('BASE')) { $fields['BASE'][0] } else { $name }
    $licenses = @(& $pacman -Ql $name | ForEach-Object { ($_ -split ' ', 2)[1] } |
        Where-Object { $_ -match '/share/licenses/.+[^/]$' })
    return @{
        Short = $Short; Version = $version; License = ($fields['LICENSE'] -join ' ') -replace 'spdx:', ''
        Source = "https://mirror.msys2.org/mingw/sources/$base-$version.src.tar.zst"; Licenses = $licenses
    }
}

function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, ($Text -replace "\r?\n", "`r`n"), (New-Object Text.UTF8Encoding $false))
}

$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(alienation_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$name = "AlienationPC-v$version-windows-x64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$symbols = Join-Path $dist "$name-symbols"
$zip = Join-Path $dist "$name.zip"
$shaFile = Join-Path $dist "$name.zip.sha256"

Write-Step "Alienation PC $version - Windows release package (local only)"
if (-not $SkipBuild) {
    $build = @{}
    if ($Clean) { $build.Clean = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @build
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Build failed' }
}
if (-not (Test-Path -LiteralPath $loader)) { Stop-Tool "Missing $loader (run without -SkipBuild)" }
if (-not (Test-Path -LiteralPath $updater)) { Stop-Tool "Missing $updater (run without -SkipBuild)" }
$built = (Get-Item -LiteralPath $loader).VersionInfo.ProductVersion
if ($built -ne $version) { Stop-Tool "alienation.exe is version $built, CMakeLists.txt says $version (run without -SkipBuild)" }

New-Item -ItemType Directory -Force $dist | Out-Null
foreach ($old in @($stage, $symbols, $zip, $shaFile, (Join-Path $dist "$name.sha256"))) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory $stage, $licenses, $symbols | Out-Null

Write-Step 'Staging alienation.exe without debug information'
$debugFile = Join-Path $symbols 'alienation.debug'
& $objcopy --only-keep-debug $loader $debugFile
if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --only-keep-debug failed' }
& $objcopy --strip-debug "--add-gnu-debuglink=$debugFile" $loader (Join-Path $stage 'alienation.exe')
if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --strip-debug failed' }

Write-Step 'Staging trg-updater.exe without debug information'
$updaterDebug = Join-Path $symbols 'trg-updater.debug'
& $objcopy --only-keep-debug $updater $updaterDebug
if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --only-keep-debug failed on trg-updater.exe' }
& $objcopy --strip-debug "--add-gnu-debuglink=$updaterDebug" $updater (Join-Path $stage 'trg-updater.exe')
if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --strip-debug failed on trg-updater.exe' }

Write-Step 'Staging the DLLs it loads'
$angleRuntime = Sync-AngleRuntime $bin
$dlls = Get-DllClosure $loader $dllDirs $objdump
$updaterDlls = Get-DllClosure $updater $dllDirs $objdump
foreach ($dllName in $updaterDlls.Keys) { if (-not $dlls.Contains($dllName)) { $dlls[$dllName] = $updaterDlls[$dllName] } }
Add-AngleDlls $dlls $angleRuntime $bin
$owners = [ordered]@{}
$ffmpegDlls = @()
foreach ($dll in $dlls.Values) {
    if ((Split-Path $dll -Parent) -eq (Resolve-Path -LiteralPath $ffmpegBin).Path) {
        if (-not (Test-LgplFFmpegDll $dll)) { Stop-Tool "$(Split-Path $dll -Leaf) is not the LGPL FFmpeg of gpu\ffmpeg.txt" }
        $ffmpegDlls += Split-Path $dll -Leaf
        Copy-Item -LiteralPath $dll $stage
        continue
    }
    $owner = & $pacman -Qqo $dll.Substring($msys.Length).Replace('\', '/') 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $owner) { Stop-Tool "No MSYS2 package owns $dll" }
    $short = $owner.Substring($packagePrefix.Length)
    if ($DllPackages -notcontains $short) {
        Stop-Tool "$(Split-Path $dll -Leaf) comes from $owner, which is not in packaging\licenses\THIRD-PARTY-LICENSES.txt"
    }
    if (-not $owners.Contains($short)) { $owners[$short] = @() }
    $owners[$short] += Split-Path $dll -Leaf
    Copy-Item -LiteralPath $dll $stage
}
foreach ($file in Get-AngleDlls $AnglePin) { Copy-Item -LiteralPath (Join-Path $angleRuntime $file.Name) $stage }

Write-Step 'Staging the licences'
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'packaging\licenses\THIRD-PARTY-LICENSES.txt') $licenses
foreach ($component in $BuiltInLicenses.Keys) {
    $source = Join-Path $root $BuiltInLicenses[$component]
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Stop-Tool "Missing licence file: $source" }
    New-Item -ItemType Directory -Force (Join-Path $licenses $component) | Out-Null
    Copy-Item -LiteralPath $source (Join-Path $licenses $component)
}
$manifest = New-Object System.Collections.Generic.List[string]
$manifest.Add("MSYS2 CLANG64 packages in alienation.exe $version and next to it. Source packages: the URL on each line.")
foreach ($short in @($owners.Keys) + $StaticPackages) {
    $info = Get-PackageInfo $short
    if ($info.Licenses.Count -eq 0) { Stop-Tool "MSYS2 package $short installs no licence file" }
    foreach ($file in $info.Licenses) {
        $relative = $file.Substring($file.IndexOf('/share/licenses/') + '/share/licenses/'.Length).Replace('/', '\')
        $target = Join-Path $licenses $relative
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath (Join-Path $prefix ($file.Substring($file.IndexOf('/share/licenses/') + 1))) $target
    }
    $use = if ($owners.Contains($short)) { $owners[$short] -join ', ' } else { 'compiled in' }
    $manifest.Add('')
    $manifest.Add("$short $($info.Version) ($use)")
    $manifest.Add("  Licence: $($info.License)")
    $manifest.Add("  Source:  $($info.Source)")
}
New-Item -ItemType Directory -Force (Split-Path (Join-Path $stage $AngleLicence) -Parent) | Out-Null
Copy-Item -LiteralPath (Join-Path $angleRuntime 'LICENSE') (Join-Path $stage $AngleLicence)
$manifest.Add('')
$manifest.Add("angleproject $($AnglePin.version) ($((Get-AngleDlls $AnglePin | ForEach-Object { $_.Name }) -join ', '))")
$manifest.Add('  Licence: BSD-3-Clause')
$manifest.Add("  Source:  https://repo.msys2.org/mingw/sources/mingw-w64-angleproject-$($AnglePin.version).src.tar.zst")
$manifest.Add("  Upstream revision: $($AnglePin.revision)")
if ($ffmpegDlls.Count -eq 0) { Stop-Tool 'alienation.exe loads no FFmpeg DLL from the gpu\ffmpeg.txt build' }
foreach ($text in 'LICENSE.md', 'COPYING.LGPLv2.1', 'BUILD.txt') {
    $source = Join-Path $ffmpegLicences $text
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Stop-Tool "Missing FFmpeg licence file: $source" }
    New-Item -ItemType Directory -Force (Join-Path $licenses 'ffmpeg') | Out-Null
    Copy-Item -LiteralPath $source (Join-Path $licenses 'ffmpeg')
}
$manifest.Add('')
$manifest.Add("ffmpeg $($ffmpegPin.version) ($($ffmpegDlls -join ', '); built from source, not an MSYS2 package)")
$manifest.Add('  Licence: LGPL-2.1-or-later')
$manifest.Add("  Source:  $($ffmpegPin.url) (SHA-256 $($ffmpegPin.sha256))")
$manifest.Add('  Configuration: licenses\ffmpeg\BUILD.txt')
Write-Text (Join-Path $licenses 'MSYS2-PACKAGES.txt') (($manifest -join "`n") + "`n")
$readme = (Get-Content -LiteralPath (Join-Path $root 'packaging\README.txt') -Raw).Replace('@VERSION@', $version)
Write-Text (Join-Path $stage 'README.txt') $readme

Write-Step 'Staging the launcher images'
$asset_dir = Join-Path $stage 'assets\launcher'
New-Item -ItemType Directory -Force $asset_dir | Out-Null
foreach ($image in @('header-art.png', 'sponsor-heart.png')) {
    $source = Join-Path $root "assets\launcher\$image"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Stop-Tool "Missing launcher image: $source" }
    Copy-Item -LiteralPath $source $asset_dir
}

Write-Step 'Checking the release folder'
if (-not (Invoke-ReleaseCheck $stage)) { Stop-Tool 'The staging folder holds files that must not be released' }

Write-Step 'Writing the ZIP'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
    Sort-Object { $_.FullName.Substring($stage.Length + 1).Replace('\', '/') })
$stream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
try {
    $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $files) {
            $entry = "$name/" + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
$expected = @($files | ForEach-Object { "$name/" + $_.FullName.Substring($stage.Length + 1).Replace('\', '/') })
if (Compare-Object $expected $entries) { Stop-Tool 'The ZIP does not hold exactly the staged files' }
if ($entries | Where-Object { $_ -match '^[\\/]|\.\.|:|\\' }) { Stop-Tool 'The ZIP holds an unsafe entry name' }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Text $shaFile "$hash  $name.zip`n"

Write-Step 'Checking the ZIP through a temporary extraction'
if (-not (Invoke-ReleaseCheck $zip)) { Stop-Tool 'The ZIP does not hold a clean release' }

function Format-Size([long]$Bytes) { '{0:N1} MB' -f ($Bytes / 1MB) }
$dllBytes = ($dlls.Values | ForEach-Object { (Get-Item -LiteralPath $_).Length } | Measure-Object -Sum).Sum
$stageBytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ''
Write-Host 'RELEASE PACKAGE READY (nothing was published)' -ForegroundColor Green
Write-Host "  Folder:   $stage ($(Format-Size $stageBytes), $($files.Count) files)"
Write-Host "  ZIP:      $zip ($(Format-Size (Get-Item -LiteralPath $zip).Length))"
Write-Host "  SHA-256:  $hash"
Write-Host "  Symbols:  $symbols (not in the package)"
Write-Host "  Loader:   $(Format-Size (Get-Item -LiteralPath $loader).Length) built, $(Format-Size (Get-Item -LiteralPath (Join-Path $stage 'alienation.exe')).Length) packaged"
Write-Host "  DLLs:     $($dlls.Count), $(Format-Size $dllBytes)"
