<#
.SYNOPSIS
  Checks a Windows release: only the files the program needs, no game files, packages, player data, logs, debug output
  or developer paths. Reads only; exits 1 and lists the problems when the release is not clean.
.DESCRIPTION
  -Folder checks a staged release folder. -Zip checks the ZIP itself: the entry names, the .sha256 file beside it, and
  the extraction into a temporary folder, which gets the same check as a staged folder.
  Binary checks (imports, the pinned ANGLE files, the version resource) need the MSYS2 CLANG64 llvm-objdump;
  -SkipBinaryChecks is for placeholder files (tools\test-release-check.ps1).
.EXAMPLE
  .\tools\check-release-folder.ps1 -Folder dist\AlienationPC-v0.1.0-windows-x64
  .\tools\check-release-folder.ps1 -Zip dist\AlienationPC-v0.1.0-windows-x64.zip
#>
[CmdletBinding(DefaultParameterSetName = 'Folder')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Folder')][string]$Folder,
    [Parameter(Mandatory, ParameterSetName = 'Zip')][string]$Zip,
    [switch]$SkipBinaryChecks
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$RootFiles = @('alienation.exe', 'trg-updater.exe', 'README.txt', 'LICENSE.txt')
$AssetFiles = @('assets\launcher\header-art.png', 'assets\launcher\sponsor-heart.png')
$RequiredLicenses = @('licenses\THIRD-PARTY-LICENSES.txt', 'licenses\MSYS2-PACKAGES.txt', 'licenses\NotoSansTC\LICENSE-NotoSansTC.txt')
$AngleLicence = 'licenses\angleproject\LICENSE'
# The licence folders of what libGLESv2.dll has compiled in.
$AngleLinkedLicences = @('spirv-tools', 'vulkan-loader', 'VulkanMemoryAllocator', 'xxhash')
# Names that mean game data, a package, player data, caches, logs or build output; the same list as tools/package-linux.sh.
$ForbiddenNames = @('game', 'user', 'user_data', 'user_data.migrating', 'logs', 'cache', 'build', 'prepared', 'extracted', 'image',
    'trophy', 'sce_sys', 'sce_module', 'online-instances', 'eboot.bin', 'param.sfo', 'alienation.ini', 'boot.bin', 'content.bin',
    'image.json', 'imports.json', 'symbols')
$ForbiddenWindowsNames = @('saves', 'settings', 'profiles', 'achievements')
$ForbiddenPatterns = @('*.pkg', '*.pdb', '*.debug', '*.log', '*.dmp', '*.ini', '*.bin', '*.self', '*.sprx', '*.prx',
    '*.elf', '*.cpk', '*.sfo', '*.json', '*.spv', '*.pipeline_cache', '*.obj', '*.o', '*.a', '*.lib', '*.tmp', '*.bak',
    '*.sav', '*.py', '*.ps1', '*.c', '*.cpp', '*.h', '*.pyc', '*.orig', '*.rej', '*.rdc', '*~',
    'trg-update-*', 'vulkan-1.dll', 'VkLayer_*', 'renderdoc*', 'TracyClient*', 'Thumbs.db', 'desktop.ini', '.git*')
# Licence texts have names such as COPYING.LIB; the licences folder is still checked for everything else.
$LicenseTextPatterns = @('*.lib', '*.a', '*.o', '*.c', '*.h')
# File signatures of PS4 packages, SELF/ELF executables and CRI archives.
$ForbiddenMagic = @{ '7F434E54' = 'PS4 package'; '4F153D1D' = 'PS4 SELF'; '7F454C46' = 'ELF executable'; '43504B20' = 'CPK archive' }
# The update helper runs from a temporary folder, so it may import nothing but Windows itself.
$HelperSystemDlls = @('kernel32.dll', 'shell32.dll', 'user32.dll', 'advapi32.dll', 'ntdll.dll', 'shlwapi.dll', 'ole32.dll', 'bcrypt.dll')
# Shapes of values that must never ship; matched, never printed.
$SecretPatterns = @('ADMIN_TOKEN', 'CLOUDFLARE_API_TOKEN', 'CF_API_TOKEN', 'ghp_[A-Za-z0-9]{30,}', 'github_pat_[A-Za-z0-9_]{30,}',
    'gho_[A-Za-z0-9]{30,}', '-----BEGIN [A-Z ]*PRIVATE KEY-----', 'AKIA[0-9A-Z]{16}', 'xox[baprs]-[A-Za-z0-9-]{10,}') |
    ForEach-Object { [regex]::new($_) }

$Latin1 = [Text.Encoding]::GetEncoding(28591)
$LocalText = New-Object System.Collections.Generic.List[string]
foreach ($path in @($root, $env:USERPROFILE)) {
    if ($path) { $LocalText.Add($path); $LocalText.Add($path.Replace('\', '/')) }
}
foreach ($word in @('VisualStudio', 'DEBUG_SAULO', 'gow3', 'God of War', 'referencias-ps4', 'build\cmake', 'build/cmake')) {
    $LocalText.Add($word)
}
$LocalNeedles = foreach ($text in $LocalText) { $text; $Latin1.GetString([Text.Encoding]::Unicode.GetBytes($text)) }

# Any user profile folder or MSYS2 location; matched against the file text with the NUL bytes removed, so UTF-16 text is found too.
$LocalPathRegexes = @('[A-Za-z]:[\\/]Users[\\/][^\\/\x00-\x1f"<>|*?]+', '[A-Za-z]:[\\/]msys64') |
    ForEach-Object { [regex]::new($_, 'IgnoreCase') }

$AnglePin = Read-AnglePin
$AngleLicenceSha256 = ($AnglePin.Files | Where-Object { $_.Name -eq 'LICENSE' }).Sha256
$objdump = $null
$dllDirs = @()
if (-not $SkipBinaryChecks) {
    $tc = Resolve-Toolchain 'auto'
    $objdump = Join-Path $tc.Bin 'llvm-objdump.exe'
    $dllDirs = @((Join-Path (Get-FFmpegPrefix) 'bin'), $tc.Bin)
}

# Adds what ANGLE's DLLs in $Dir load from $Dir.
function Add-AngleDlls($Found, [string]$Dir) {
    foreach ($file in Get-AngleDlls $AnglePin) {
        $path = Join-Path $Dir $file.Name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $closure = Get-DllClosure $path $Dir $objdump
        foreach ($dllName in $closure.Keys) { if (-not $Found.Contains($dllName)) { $Found[$dllName] = $closure[$dllName] } }
    }
}

# The DLLs a release folder may hold: what the executables and ANGLE load from it.
function Get-ShippedDlls([string]$Base) {
    $dlls = [ordered]@{}
    foreach ($file in Get-AngleDlls $AnglePin) {
        $path = Join-Path $Base $file.Name
        if (Test-Path -LiteralPath $path -PathType Leaf) { $dlls[$file.Name.ToLowerInvariant()] = $path }
    }
    if ($SkipBinaryChecks) { return $dlls }
    foreach ($exe in @('alienation.exe', 'trg-updater.exe')) {
        $path = Join-Path $Base $exe
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $closure = Get-DllClosure $path $Base $objdump
        foreach ($dllName in $closure.Keys) { if (-not $dlls.Contains($dllName)) { $dlls[$dllName] = $closure[$dllName] } }
    }
    Add-AngleDlls $dlls $Base
    return $dlls
}

function Test-BinaryContent([string]$Base, $Problems) {
    $exe = Join-Path $Base 'alienation.exe'
    $updaterExe = Join-Path $Base 'trg-updater.exe'
    $dlls = Get-ShippedDlls $Base
    foreach ($problem in Get-AngleRuntimeProblems $Base $AnglePin { param($path) Get-Imports $path $objdump }) { $Problems.Add($problem) }
    if ((Test-Path -LiteralPath (Join-Path $Base $AngleLicence) -PathType Leaf) -and -not (Test-Sha256 (Join-Path $Base $AngleLicence) $AngleLicenceSha256)) {
        $Problems.Add("stale: $AngleLicence differs from gpu\angle-runtime.txt")
    }
    foreach ($binary in @($exe, $updaterExe)) {
        if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { continue }
        foreach ($name in (Get-DllClosure $binary $dllDirs $objdump).Keys) {
            if (-not $dlls.Contains($name)) { $Problems.Add("missing DLL: $name, needed by $(Split-Path $binary -Leaf)") }
        }
    }
    if (Test-Path -LiteralPath $updaterExe -PathType Leaf) {
        foreach ($import in Get-Imports $updaterExe $objdump) {
            if ($import.ToLowerInvariant() -notin $HelperSystemDlls -and $import -notmatch '^api-ms-win-') {
                $Problems.Add("trg-updater.exe imports $import, which is not part of Windows")
            }
        }
    }
    if (Test-Path -LiteralPath $exe -PathType Leaf) {
        $leaf = Split-Path $Base -Leaf
        if ($leaf -match '^AlienationPC-v([0-9]+\.[0-9]+\.[0-9]+)-windows-x64$') {
            $built = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
            if ($built -ne $Matches[1]) { $Problems.Add("alienation.exe is version $built, the folder name says $($Matches[1])") }
        }
    }
}

function Test-ReleaseFolder([string]$Dir) {
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { throw "Not a folder: $Dir" }
    $base = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
    $dlls = Get-ShippedDlls $base
    if (-not $SkipBinaryChecks) { Test-BinaryContent $base $problems }
    foreach ($item in Get-ChildItem -LiteralPath $base -Recurse -Force) {
        $relative = $item.FullName.Substring($base.Length + 1)
        $parts = $relative -split '\\'
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { $problems.Add("reparse point: $relative") }
        if ($ForbiddenNames -contains $item.Name -or $ForbiddenWindowsNames -contains $item.Name) { $problems.Add("forbidden name: $relative") }
        foreach ($pattern in $ForbiddenPatterns) {
            if ($item.Name -like $pattern -and -not ($parts[0] -eq 'licenses' -and $LicenseTextPatterns -contains $pattern)) {
                $problems.Add("forbidden file type: $relative")
            }
        }
        if ($parts.Count -eq 1) {
            if ($item.PSIsContainer) {
                if ($item.Name -notin @('licenses', 'assets')) { $problems.Add("unexpected folder: $relative") }
            } elseif ($RootFiles -notcontains $item.Name -and -not $dlls.Contains($item.Name.ToLowerInvariant())) {
                $problems.Add("unexpected file: $relative")
            }
        } elseif ($parts[0] -eq 'assets') {
            if ($item.PSIsContainer) {
                if ($relative -ne 'assets\launcher') { $problems.Add("unexpected folder: $relative") }
            } elseif ($AssetFiles -notcontains $relative) {
                $problems.Add("unexpected item: $relative")
            }
        } elseif ($parts[0] -ne 'licenses' -or $item.Name -like '*.exe' -or $item.Name -like '*.dll') {
            $problems.Add("unexpected item: $relative")
        }
        if ($item.PSIsContainer) { continue }
        $bytes = [IO.File]::ReadAllBytes($item.FullName)
        if ($bytes.Length -ge 4) {
            $magic = -join ($bytes[0..3] | ForEach-Object { $_.ToString('X2') })
            if ($ForbiddenMagic.ContainsKey($magic)) { $problems.Add("$($ForbiddenMagic[$magic]): $relative") }
        }
        $text = $Latin1.GetString($bytes)
        foreach ($needle in $LocalNeedles) {
            if ($text.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $problems.Add("local path or stale name in $relative"); break
            }
        }
        $stripped = $text.Replace([string][char]0, '')
        foreach ($regex in $LocalPathRegexes) {
            if ($regex.IsMatch($stripped)) { $problems.Add("user or build machine path in file: $relative"); break }
        }
        foreach ($pattern in $SecretPatterns) {
            if ($pattern.IsMatch($text)) { $problems.Add("secret or token in file: $relative"); break }
        }
    }
    foreach ($leaf in $RootFiles + $RequiredLicenses + @($AngleLicence) + $AssetFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $base $leaf) -PathType Leaf)) { $problems.Add("missing: $leaf") }
    }
    foreach ($folder in $AngleLinkedLicences) {
        if (-not (Get-ChildItem -LiteralPath (Join-Path $base "licenses\$folder") -File -ErrorAction SilentlyContinue)) { $problems.Add("missing licence: licenses\$folder") }
    }
    return $problems
}

function Test-ReleaseZip([string]$Path) {
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Not a file: $Path" }
    $file = (Resolve-Path -LiteralPath $Path).Path
    $leaf = Split-Path $file -Leaf
    $wrapper = if ($leaf -match '^(AlienationPC-v[0-9]+\.[0-9]+\.[0-9]+-windows-x64)\.zip$') { $Matches[1] } else { $null }
    if (-not $wrapper) { $problems.Add("unexpected ZIP name: $leaf"); return $problems }

    $checksum = "$file.sha256"
    if (-not (Test-Path -LiteralPath $checksum -PathType Leaf)) {
        $problems.Add("missing: $leaf.sha256")
    } else {
        $line = Get-Content -LiteralPath $checksum -TotalCount 1
        if ($line -match '^([0-9a-f]{64})  (.+)$') {
            $expected = $Matches[1]
            $named = $Matches[2]
            if ($named -ne $leaf) { $problems.Add("$leaf.sha256 names $named") }
            elseif ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { $problems.Add("$leaf.sha256 does not match the ZIP") }
        } else {
            $problems.Add("$leaf.sha256 is not '<sha256>  <file name>'")
        }
    }

    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($file)
    try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
    foreach ($entry in $entries) {
        if ($entry -match '^[\\/]|\.\.|:|\\') { $problems.Add("unsafe ZIP entry: $entry") }
        elseif (-not $entry.StartsWith("$wrapper/")) { $problems.Add("ZIP entry outside $wrapper/: $entry") }
    }
    if ($problems.Count) { return $problems }

    $temp = Join-Path ([IO.Path]::GetTempPath()) ("aln-release-check-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        [IO.Compression.ZipFile]::ExtractToDirectory($file, $temp)
        foreach ($problem in Test-ReleaseFolder (Join-Path $temp $wrapper)) { $problems.Add($problem) }
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
    return $problems
}

$target = if ($PSCmdlet.ParameterSetName -eq 'Zip') { $Zip } else { $Folder }
$problems = @(if ($PSCmdlet.ParameterSetName -eq 'Zip') { Test-ReleaseZip $Zip } else { Test-ReleaseFolder $Folder })
if ($problems.Count) {
    Write-Host ''
    Write-Host 'RELEASE CHECK FAILED' -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host "  $problem" -ForegroundColor Red }
    exit 1
}
Write-Ok "Release is clean: $target"
