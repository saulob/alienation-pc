<#
.SYNOPSIS
  Checks that check-release-folder.ps1 accepts a clean release and refuses player data, game files, logs, updater
  leftovers, debug markers, developer paths and a damaged ZIP. Placeholder files in scratch folders only: nothing is
  built, launched or published.
#>
$ErrorActionPreference = 'Stop'
$ReleaseCheck = Join-Path $PSScriptRoot 'check-release-folder.ps1'
$Root = Split-Path $PSScriptRoot -Parent
$StageRoot = Join-Path ([IO.Path]::GetTempPath()) 'aln_release_check'
$PowerShell = (Get-Process -Id $PID).Path
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$RootLeaves = 'alienation.exe', 'trg-updater.exe', 'README.txt', 'LICENSE.txt'
$AssetLeaves = 'header-art.png', 'sponsor-heart.png'
$LicenseLeaves = 'THIRD-PARTY-LICENSES.txt', 'MSYS2-PACKAGES.txt', 'NotoSansTC\LICENSE-NotoSansTC.txt',
    'VulkanMemoryAllocator\LICENSE', 'xxhash\LICENSE'

# Each case adds one item to a clean release: a relative path, or path plus content.
$Cases = [ordered]@{
    'fresh extraction'      = $null
    'user folder'           = @{ Path = 'user\' }
    'settings file'         = @{ Path = 'user\alienation.ini' }
    'user_data'             = @{ Path = 'user_data\' }
    'migration leftover'    = @{ Path = 'user_data.migrating\' }
    'trophy data'           = @{ Path = 'user\trophy\NPWR00000_00\a.dat' }
    'saves'                 = @{ Path = 'saves\a.sav' }
    'settings folder'       = @{ Path = 'settings\a.txt' }
    'profiles'              = @{ Path = 'profiles\a.txt' }
    'achievements'          = @{ Path = 'achievements\a.txt' }
    'online instances'      = @{ Path = 'online-instances\a\alienation.ini' }
    'cache'                 = @{ Path = 'cache\a.txt' }
    'log'                   = @{ Path = 'logs\alienation_001.txt' }
    'log file'              = @{ Path = 'alienation_001.log' }
    'game folder'           = @{ Path = 'game\' }
    'prepared game'         = @{ Path = 'game\prepared\a.txt' }
    'extracted game'        = @{ Path = 'extracted\a.txt' }
    'boot image'            = @{ Path = 'image\a.txt' }
    'eboot'                 = @{ Path = 'eboot.bin' }
    'eboot mixed case'      = @{ Path = 'EBOOT.BIN' }
    'param.sfo'             = @{ Path = 'param.sfo' }
    'sce_sys'               = @{ Path = 'sce_sys\a.txt' }
    'ps4 package'           = @{ Path = 'game.pkg' }
    'ps4 module'            = @{ Path = 'libc.prx' }
    'self file'             = @{ Path = 'a.self' }
    'json'                  = @{ Path = 'image.json' }
    'pdb'                   = @{ Path = 'alienation.pdb' }
    'updater pdb'           = @{ Path = 'trg-updater.pdb' }
    'debug file'            = @{ Path = 'trg-updater.debug' }
    'symbols folder'        = @{ Path = 'symbols\a.txt' }
    'updater workspace'     = @{ Path = 'trg-update-1234\job.txt' }
    'updater log'           = @{ Path = 'trg-updater.log' }
    'renderdoc'             = @{ Path = 'renderdoc.dll' }
    'vulkan loader'         = @{ Path = 'vulkan-1.dll' }
    'vulkan layer'          = @{ Path = 'VkLayer_test.dll' }
    'crash dump'            = @{ Path = 'crash.dmp' }
    'backup'                = @{ Path = 'alienation.ini.bak' }
    'temp file'             = @{ Path = 'x.tmp' }
    'source file'           = @{ Path = 'main.cpp' }
    'script'                = @{ Path = 'run.ps1' }
    'unexpected file'       = @{ Path = 'notes.txt' }
    'unexpected folder'     = @{ Path = 'extra\a.txt' }
    'unexpected dll'        = @{ Path = 'other.dll' }
    'unexpected exe'        = @{ Path = 'tool.exe' }
    'extra asset'           = @{ Path = 'assets\launcher\other.png' }
    'extra asset folder'    = @{ Path = 'assets\game\a.png' }
    'license program'       = @{ Path = 'licenses\tool.exe' }
    'license dll'           = @{ Path = 'licenses\foo\x.dll' }
    'license pdb'           = @{ Path = 'licenses\foo\x.pdb' }
    'missing exe'           = @{ Path = 'alienation.exe'; Delete = $true }
    'missing updater'       = @{ Path = 'trg-updater.exe'; Delete = $true }
    'missing readme'        = @{ Path = 'README.txt'; Delete = $true }
    'missing license'       = @{ Path = 'LICENSE.txt'; Delete = $true }
    'missing header art'    = @{ Path = 'assets\launcher\header-art.png'; Delete = $true }
    'missing sponsor heart' = @{ Path = 'assets\launcher\sponsor-heart.png'; Delete = $true }
    'missing third party'   = @{ Path = 'licenses\THIRD-PARTY-LICENSES.txt'; Delete = $true }
    'missing packages'      = @{ Path = 'licenses\MSYS2-PACKAGES.txt'; Delete = $true }
    'ANGLE egl library'     = @{ Path = 'libEGL.dll' }
    'ANGLE gles library'    = @{ Path = 'libGLESv2.dll' }
    'ANGLE licence'         = @{ Path = 'licenses\angleproject\LICENSE' }
    'missing linked licence' = @{ Path = 'licenses\xxhash\LICENSE'; Delete = $true }
    'ps4 package magic'     = @{ Path = 'alienation.exe'; Bytes = [byte[]](0x7F, 0x43, 0x4E, 0x54, 0, 0) }
    'ps4 self magic'        = @{ Path = 'trg-updater.exe'; Bytes = [byte[]](0x4F, 0x15, 0x3D, 0x1D, 0, 0) }
    'elf magic'             = @{ Path = 'README.txt'; Bytes = [byte[]](0x7F, 0x45, 0x4C, 0x46, 0, 0) }
    'cpk magic'             = @{ Path = 'README.txt'; Bytes = [byte[]](0x43, 0x50, 0x4B, 0x20, 0, 0) }
    'debug marker'          = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes('[DEBUG_SAULO][ONLINE] x') }
    'debug marker 16'       = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::Unicode.GetBytes('DEBUG_SAULO') }
    'visual studio path'    = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes('x:/VisualStudio/repos/y/a.cpp') }
    'user profile path'     = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes('x ' + 'C:' + '\Users\someone\source\a.cpp') }
    'user path 16'          = @{ Path = 'trg-updater.exe'; Bytes = [Text.Encoding]::Unicode.GetBytes('x D:\Users\someone\a.cpp') }
    'msys path'             = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes('C:/msys64/clang64/include') }
    'repository path'       = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes("path $Root x") }
    'repository path 16'    = @{ Path = 'trg-updater.exe'; Bytes = [Text.Encoding]::Unicode.GetBytes("path $Root x") }
    'build tree path'       = @{ Path = 'alienation.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes('build\cmake\clang64') }
    'github token'          = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('ghp_' + ('a' * 36)) }
    'github fine token'     = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('github_pat_' + ('b' * 40)) }
    'private key'           = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('-----BEGIN RSA PRIVATE KEY-----') }
    'admin token name'      = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('ADMIN_TOKEN=x') }
}

function New-Stage([string]$Folder) {
    New-Item -ItemType Directory -Force $Folder | Out-Null
    foreach ($leaf in $RootLeaves) { Set-Content -LiteralPath (Join-Path $Folder $leaf) -Value 'placeholder' }
    New-Item -ItemType Directory -Force (Join-Path $Folder 'assets\launcher') | Out-Null
    foreach ($leaf in $AssetLeaves) { Set-Content -LiteralPath (Join-Path $Folder "assets\launcher\$leaf") -Value 'placeholder' }
    foreach ($leaf in $LicenseLeaves) {
        $target = Join-Path $Folder "licenses\$leaf"
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        Set-Content -LiteralPath $target -Value 'placeholder'
    }
}

function Test-Accepted([string[]]$Arguments) {
    $global:LASTEXITCODE = 0
    & $PowerShell -NoProfile -ExecutionPolicy Bypass -File $ReleaseCheck @Arguments -SkipBinaryChecks *> $null
    return $LASTEXITCODE -eq 0
}

function New-ReleaseZip([string]$Stage, [string]$Zip, [string]$Wrapper, [string[]]$ExtraEntries = @()) {
    $stream = [IO.File]::Open($Zip, [IO.FileMode]::CreateNew)
    try {
        $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in Get-ChildItem -LiteralPath $Stage -Recurse -File) {
                $entry = $file.FullName.Substring($Stage.Length + 1).Replace('\', '/')
                if ($Wrapper) { $entry = "$Wrapper/$entry" }
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry)
            }
            foreach ($name in $ExtraEntries) {
                $entry = $archive.CreateEntry($name)
                $writer = New-Object IO.StreamWriter($entry.Open())
                $writer.Write('placeholder')
                $writer.Dispose()
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
}

function Write-Sidecar([string]$Zip, [string]$Hash = $null, [string]$Name = $null) {
    if (-not $Hash) { $Hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant() }
    if (-not $Name) { $Name = Split-Path $Zip -Leaf }
    [IO.File]::WriteAllText("$Zip.sha256", "$Hash  $Name`n")
}

$failures = 0
$total = 0
try {
    New-Item -ItemType Directory -Force $StageRoot | Out-Null
    foreach ($case in $Cases.Keys) {
        $total++
        $stage = Join-Path $StageRoot ($case -replace '\W', '_')
        New-Stage $stage
        $extra = $Cases[$case]
        if ($extra) {
            $target = Join-Path $stage $extra.Path
            if ($extra.Delete) {
                Remove-Item -LiteralPath $target
            } elseif ($extra.Path.EndsWith('\')) {
                New-Item -ItemType Directory -Force $target | Out-Null
            } else {
                New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
                if ($extra.Bytes) { [IO.File]::WriteAllBytes($target, $extra.Bytes) } else { Set-Content -LiteralPath $target -Value 'placeholder' }
            }
        }
        if ((Test-Accepted @('-Folder', $stage)) -ne ($null -eq $extra)) {
            Write-Host "  FAIL $case"
            $failures++
        }
    }

    # The ZIP: its name, its .sha256 file, its entry names and what an extraction holds.
    $name = 'AlienationPC-v0.1.0-windows-x64'
    $zipStage = Join-Path $StageRoot 'zip_source'
    New-Stage $zipStage
    $zipCases = [ordered]@{
        'zip clean'              = @{ Accept = $true }
        'zip flat'               = @{ Wrapper = '' }
        'zip other wrapper'      = @{ Wrapper = 'Other' }
        'zip extra file'         = @{ Entries = @("$name/notes.txt") }
        'zip user data'          = @{ Entries = @("$name/user/alienation.ini") }
        'zip traversal'          = @{ Entries = @("$name/../evil.txt") }
        'zip backslash'          = @{ Entries = @("$name\evil.txt") }
        'zip drive path'         = @{ Entries = @("C:/evil.txt") }
        'zip no checksum'        = @{ NoSidecar = $true }
        'zip wrong checksum'     = @{ Hash = ('0' * 64) }
        'zip checksum names file' = @{ SidecarName = 'other.zip' }
        'zip other name'         = @{ ZipName = 'Alienation.zip' }
    }
    foreach ($case in $zipCases.Keys) {
        $total++
        $options = $zipCases[$case]
        $folder = Join-Path $StageRoot ($case -replace '\W', '_')
        New-Item -ItemType Directory -Force $folder | Out-Null
        $zipName = if ($options.ContainsKey('ZipName')) { $options.ZipName } else { "$name.zip" }
        $zip = Join-Path $folder $zipName
        $wrapper = if ($options.ContainsKey('Wrapper')) { $options.Wrapper } else { $name }
        $entries = if ($options.ContainsKey('Entries')) { $options.Entries } else { @() }
        New-ReleaseZip $zipStage $zip $wrapper $entries
        if (-not $options.ContainsKey('NoSidecar')) {
            $hash = if ($options.ContainsKey('Hash')) { $options.Hash } else { $null }
            $sidecarName = if ($options.ContainsKey('SidecarName')) { $options.SidecarName } else { $null }
            Write-Sidecar $zip $hash $sidecarName
        }
        if ((Test-Accepted @('-Zip', $zip)) -ne ($options.ContainsKey('Accept') -and $options.Accept)) {
            Write-Host "  FAIL $case"
            $failures++
        }
    }
} finally {
    Remove-Item -LiteralPath $StageRoot -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$total release check cases, $failures failures"
if ($failures) { throw 'Release check tests failed' }
$global:LASTEXITCODE = 0
