# Shared helpers, dot-sourced by the other tools. Works in Windows PowerShell 5.1 and PowerShell 7.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$script:BuildLog = $null

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan; Write-BuildLogLine "==> $Text" }
function Write-Ok([string]$Text)   { Write-Host "  [ok]   $Text" -ForegroundColor Green; Write-BuildLogLine "  [ok]   $Text" }
function Write-Warn([string]$Text) { Write-Host "  [warn] $Text" -ForegroundColor Yellow; Write-BuildLogLine "  [warn] $Text" }
function Write-Bad([string]$Text)  { Write-Host "  [miss] $Text" -ForegroundColor Red; Write-BuildLogLine "  [miss] $Text" }

function Stop-Tool([string]$Text) {
    Write-Host "error: $Text" -ForegroundColor Red
    Write-BuildLogLine "error: $Text"
    Close-BuildLog 1
    exit 1
}

# Lower-case project name from .recomp.json ("game"), else the repository folder; names the build logs.
function Get-ProjectName {
    $name = $null
    $manifest = Join-Path $script:RepoRoot '.recomp.json'
    if (Test-Path -LiteralPath $manifest) {
        try { $name = [string](Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).game } catch { $name = $null }
    }
    if (-not $name) { $name = Split-Path $script:RepoRoot -Leaf }
    $slug = ($name.ToLowerInvariant() -replace '[^a-z0-9]+', '_').Trim('_')
    if ($slug) { return $slug }
    return 'project'
}

# Where a build.ps1 run logs. A build alone writes build\logs\build\build_NNN.log; a build that runs the game writes the
# session log, build\logs\<project>_NNN.log, which the game then continues, so one invocation has one log.
function Get-BuildLogPlan([switch]$Run) {
    if ($Run) { return @{ Directory = (Join-Path $script:RepoRoot 'build\logs'); Name = (Get-ProjectName); Session = $true } }
    return @{ Directory = (Join-Path $script:RepoRoot 'build\logs\build'); Name = 'build'; Session = $false }
}

# The loader continues the session log given with --log; arguments that already choose the log are kept as they are.
function Get-SessionLoaderArgs([string]$SessionLog, [string[]]$Arguments = @()) {
    if ($Arguments -contains '--log' -or $Arguments -contains '--logs') { return $Arguments }
    return @('--log', $SessionLog) + $Arguments
}

# After the game ended: the runtime deletes its log after a clean exit when detailed logging is off, so the build
# section is written back before the exit code is recorded.
function Complete-SessionLog([string]$Path, [string]$BuildSection, [int]$ExitCode) {
    if (-not (Test-Path -LiteralPath $Path)) {
        [IO.File]::WriteAllText($Path, $BuildSection, (New-Object Text.UTF8Encoding($false)))
    }
    Add-Content -LiteralPath $Path -Value "[run] alienation.exe exit code $ExitCode"
}

# Reserves <name>_NNN.log (one more than the highest in the folder) with an exclusive create, so concurrent builds
# never share a number and an existing log is never opened for writing. -Session starts the log with a [build] line.
function Start-BuildLog([string]$Mode, [string]$Configuration, [string]$Directory = (Join-Path $script:RepoRoot 'build\logs\build'),
                        [string]$Name = 'build', [switch]$Session) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $pattern = '^' + [regex]::Escape($Name) + '_(\d+)\.log$'
    $stream = $null
    for ($attempt = 0; $attempt -lt 1000 -and -not $stream; $attempt++) {
        $last = 0
        foreach ($file in [IO.Directory]::GetFiles($Directory)) {
            if ([IO.Path]::GetFileName($file) -match $pattern) {
                $number = [int64]$Matches[1]
                if ($number -gt $last) { $last = $number }
            }
        }
        $path = Join-Path $Directory ('{0}_{1:D3}.log' -f $Name, ($last + 1))
        try { $stream = New-Object IO.FileStream($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read) }
        catch [IO.IOException] { $stream = $null }
    }
    if (-not $stream) { throw "Could not reserve a build log in $Directory" }
    $writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    $script:BuildLog = @{ Path = $path; Writer = $writer; Started = Get-Date; Warnings = 0; Errors = 0 }
    if ($Session) { $writer.WriteLine('[build]') }
    $writer.WriteLine("Project: $(Get-ProjectName)")
    $writer.WriteLine("Started: $($script:BuildLog.Started.ToString('yyyy-MM-dd HH:mm:ss zzz'))")
    $writer.WriteLine("Configuration: $Configuration")
    $writer.WriteLine("Mode: $Mode")
    $writer.WriteLine('')
    Write-Host "$(if ($Session) { 'Session log' } else { 'Build log' }): $path"
    return $path
}

function Write-BuildLogLine([string]$Line) {
    if (-not $script:BuildLog) { return }
    if ($Line -match '(?i)\bwarning\b:') { $script:BuildLog.Warnings++ }
    if ($Line -match '(?i)(\berror\b:|^FAILED:)') { $script:BuildLog.Errors++ }
    $script:BuildLog.Writer.WriteLine($Line)
}

# Idempotent; the exit code is recorded as given and never changed.
function Close-BuildLog([int]$ExitCode) {
    $log = $script:BuildLog
    if (-not $log) { return }
    $script:BuildLog = $null
    $finished = Get-Date
    $log.Writer.WriteLine('')
    $log.Writer.WriteLine("Warnings: $($log.Warnings)")
    $log.Writer.WriteLine("Errors: $($log.Errors)")
    $log.Writer.WriteLine("Result: $(if ($ExitCode -eq 0) { 'SUCCESS' } else { 'FAILED' })")
    $log.Writer.WriteLine("Exit code: $ExitCode")
    $log.Writer.WriteLine("Finished: $($finished.ToString('yyyy-MM-dd HH:mm:ss zzz')) ($([int]($finished - $log.Started).TotalSeconds) s)")
    $log.Writer.Dispose()
}

# Console and build log, line by line.
filter Out-BuildLog {
    $line = "$_"
    Write-Host $line
    Write-BuildLogLine $line
}

# Runs a native tool with stdout and stderr merged into the console and the build log. The tool's exit code stays in
# $LASTEXITCODE for the caller; nothing here changes it.
function Invoke-Native([string]$File, [string[]]$Arguments = @()) {
    Write-BuildLogLine ("$ $File " + ($Arguments -join ' '))
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $File @Arguments 2>&1 | Out-BuildLog }
    finally { $ErrorActionPreference = $previous }
}

function Get-VenvPython {
    $path = Join-Path $script:RepoRoot '.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $path) { return $path }
    return $null
}

# Interpreter for the scripts: the project venv, else the py launcher, else the first python on PATH.
function Get-Python {
    $venv = Get-VenvPython
    if ($venv) { return @{ Exe = $venv; Args = @() } }
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) { return @{ Exe = $launcher.Source; Args = @('-3') } }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { return @{ Exe = $python.Source; Args = @() } }
    return $null
}

# Runs scripts/<name> with isolated-mode Python so nothing next to the data is imported.
function Invoke-Script([string]$Name, [string[]]$Arguments = @()) {
    $python = Get-Python
    if (-not $python) { Stop-Tool 'Python 3 not found. Run tools\setup.ps1.' }
    $script = Join-Path $script:RepoRoot "scripts\$Name"
    & $python.Exe @($python.Args) '-I' $script @Arguments | Out-Host
    return $LASTEXITCODE
}

function Test-PythonModule([string]$Module) {
    $python = Get-Python
    if (-not $python) { return $false }
    & $python.Exe @($python.Args) '-I' '-c' "import $Module" 2>$null
    return ($LASTEXITCODE -eq 0)
}

function Get-Msys2Root {
    $candidates = @($env:MSYS2_ROOT, 'C:\msys64', 'D:\msys64') | Where-Object { $_ }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'usr\bin\pacman.exe')) { return $candidate }
    }
    return $null
}

# The only supported toolchain: MSYS2 CLANG64, as in the reference ports (the video core needs it).
$script:Toolchains = [ordered]@{
    clang64 = @{ Dir = 'clang64'; Compiler = 'clang.exe'; Package = 'mingw-w64-clang-x86_64' }
}
# Needed to build the loader, runtime and video core.
$script:BuildTools = @('clang.exe', 'cmake.exe', 'ninja.exe', 'pkgconf.exe', 'SDL3.dll', 'glslang.exe', 'vulkan-1.dll', 'libfreetype-6.dll')
$script:Clang64Packages = 'mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,sdl3,freetype} ' +
    'mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} ' +
    'mingw-w64-clang-x86_64-{fmt,boost,robin-map,xxhash,libpng,zydis,spirv-tools,spirv-headers,spirv-cross,ffmpeg}'

function Get-ToolchainStatus([string]$Name) {
    $info = $script:Toolchains[$Name]
    $root = Get-Msys2Root
    $status = @{ Name = $Name; Ready = $false; Bin = $null; Missing = @() }
    if (-not $root) { $status.Missing = @('MSYS2'); return $status }
    $bin = Join-Path $root "$($info.Dir)\bin"
    $status.Bin = $bin
    foreach ($tool in $script:BuildTools) {
        if (-not (Test-Path -LiteralPath (Join-Path $bin $tool))) { $status.Missing += $tool }
    }
    $status.Ready = ($status.Missing.Count -eq 0)
    return $status
}

function Resolve-Toolchain([string]$Requested) {
    if ($Requested -and $Requested -ne 'auto') {
        $status = Get-ToolchainStatus $Requested
        if (-not $status.Ready) { Stop-Tool "Toolchain '$Requested' is incomplete (missing: $($status.Missing -join ', ')). Run tools\setup.ps1." }
        return $status
    }
    foreach ($name in $script:Toolchains.Keys) {
        $status = Get-ToolchainStatus $name
        if ($status.Ready) { return $status }
    }
    Stop-Tool 'MSYS2 CLANG64 toolchain or libraries missing. Run tools\setup.ps1 for what to install.'
}

# Provided by the graphics driver.
$script:SystemProvidedDlls = @('vulkan-1.dll')

function Get-Imports([string]$Path, [string]$Objdump) {
    $out = & $Objdump -p $Path
    if ($LASTEXITCODE -ne 0) { Stop-Tool "llvm-objdump failed on $Path" }
    foreach ($line in $out) { if ($line -match 'DLL Name:\s*(\S+)') { $Matches[1] } }
}

# The DLLs that $Exe loads from $Dir, directly or through each other; Windows' own DLLs are not in $Dir.
function Get-DllClosure([string]$Exe, [string]$Dir, [string]$Objdump) {
    $found = [ordered]@{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($Exe)
    while ($queue.Count -gt 0) {
        foreach ($name in Get-Imports $queue.Dequeue() $Objdump) {
            $key = $name.ToLowerInvariant()
            if ($script:SystemProvidedDlls -contains $key -or $found.Contains($key)) { continue }
            $file = Join-Path $Dir $name
            if (Test-Path -LiteralPath $file -PathType Leaf) {
                $found[$key] = (Get-Item -LiteralPath $file).FullName
                $queue.Enqueue($found[$key])
            }
        }
    }
    return $found
}

function Test-Sha256([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ieq $Expected
}

function Read-AnglePin([string]$Descriptor = (Join-Path $script:RepoRoot 'gpu\angle-runtime.txt')) {
    $pin = @{ Files = @(); Needs = @() }
    foreach ($line in Get-Content -LiteralPath $Descriptor) {
        if ($line -match '^\s*(#|$)') { continue }
        $fields = -split $line
        if ($fields[0] -eq 'file') { $pin.Files += @{ Member = $fields[1]; Name = ($fields[1] -split '/')[-1]; Sha256 = $fields[2] } }
        elseif ($fields[0] -eq 'needs') { $pin.Needs += $fields[1] }
        else { $pin[$fields[0]] = $fields[1] }
    }
    foreach ($key in 'revision', 'version', 'package', 'url', 'sha256') {
        if (-not $pin[$key]) { throw "$Descriptor has no $key" }
    }
    if ($pin.Files.Count -eq 0) { throw "$Descriptor lists no files" }
    return $pin
}

function Get-AngleDlls($Pin) { @($Pin.Files | Where-Object { $_.Name -like '*.dll' }) }

function Test-AngleRuntimeCurrent($Pin, [string]$Runtime) {
    foreach ($file in $Pin.Files) {
        if (-not (Test-Sha256 (Join-Path $Runtime $file.Name) $file.Sha256)) { return $false }
    }
    return $true
}

function Get-AngleArchive($Pin, [string]$Dir) {
    $archive = Join-Path $Dir $Pin.package
    if (Test-Sha256 $archive $Pin.sha256) { return $archive }
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    Write-Step "Fetching ANGLE $($Pin.version)"
    $part = "$archive.part"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $client = New-Object System.Net.WebClient
    try { $client.DownloadFile($Pin.url, $part) }
    catch { throw "ANGLE could not be downloaded from $($Pin.url): $($_.Exception.Message)" }
    finally { $client.Dispose() }
    if (-not (Test-Sha256 $part $Pin.sha256)) {
        $actual = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
        Remove-Item -LiteralPath $part -Force
        throw "$($Pin.package) does not match its pinned SHA-256: expected $($Pin.sha256), got $actual"
    }
    Move-Item -LiteralPath $part $archive
    return $archive
}

function Expand-AngleArchive($Pin, [string]$Archive, [string]$Dir, [string]$Runtime, [string]$CMake) {
    $temporary = Join-Path $Dir 'extract'
    foreach ($folder in $temporary, $Runtime) {
        if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    }
    New-Item -ItemType Directory -Force -Path $temporary | Out-Null
    Push-Location $temporary
    try {
        & $CMake -E tar xf $Archive @($Pin.Files | ForEach-Object { $_.Member })
        if ($LASTEXITCODE -ne 0) { throw "cmake could not extract $($Pin.package)" }
    }
    finally { Pop-Location }
    New-Item -ItemType Directory -Force -Path $Runtime | Out-Null
    foreach ($file in $Pin.Files) {
        $extracted = Join-Path $temporary $file.Member.Replace('/', '\')
        if (-not (Test-Sha256 $extracted $file.Sha256)) {
            Remove-Item -LiteralPath $Runtime -Recurse -Force
            throw "$($file.Member) in $($Pin.package) does not match its pinned SHA-256"
        }
        Move-Item -LiteralPath $extracted (Join-Path $Runtime $file.Name)
    }
    Remove-Item -LiteralPath $temporary -Recurse -Force
}

# Files that already match their pinned hashes are the whole answer: no network, so an offline rebuild works.
function Update-AngleRuntime([string]$Descriptor = (Join-Path $script:RepoRoot 'gpu\angle-runtime.txt'),
                             [string]$CacheRoot = (Join-Path $script:RepoRoot 'cache\deps'), [string]$CMake = 'cmake') {
    $pin = Read-AnglePin $Descriptor
    $dir = Join-Path $CacheRoot "angle\$($pin.version)"
    $runtime = Join-Path $dir 'runtime'
    if (Test-AngleRuntimeCurrent $pin $runtime) { return $runtime }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Expand-AngleArchive $pin (Get-AngleArchive $pin $dir) $dir $runtime $CMake | Out-Null
    return $runtime
}

function Sync-AngleRuntime([string]$ToolBin) {
    try { return Update-AngleRuntime -CMake (Join-Path $ToolBin 'cmake.exe') }
    catch { Stop-Tool $_.Exception.Message }
}

function Copy-IfChanged([string]$Source, [string]$TargetDir) {
    $target = Join-Path $TargetDir (Split-Path $Source -Leaf)
    if ((Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $target).Hash -eq (Get-FileHash -LiteralPath $Source).Hash) { return }
    Copy-Item -LiteralPath $Source $target -Force
}

# What is wrong with ANGLE in $Dir: every import must be a pinned file beside it or part of Windows, so that nothing
# depends on what PATH happens to hold.
function Get-AngleRuntimeProblems([string]$Dir, $Pin, [scriptblock]$Imports) {
    $problems = @()
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($file in Get-AngleDlls $Pin) {
        $path = Join-Path $Dir $file.Name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $problems += "missing DLL: $($file.Name)"; continue }
        if (-not (Test-Sha256 $path $file.Sha256)) { $problems += "stale ANGLE file: $($file.Name) differs from gpu\angle-runtime.txt" }
        $queue.Enqueue($path)
    }
    foreach ($name in $Pin.Needs) {
        if (-not (Test-Path -LiteralPath (Join-Path $Dir $name) -PathType Leaf)) { $problems += "missing DLL: $name" }
    }
    $seen = @{}
    while ($queue.Count -gt 0) {
        $path = $queue.Dequeue()
        foreach ($name in & $Imports $path) {
            $key = $name.ToLowerInvariant()
            if ($seen.ContainsKey($key) -or $key -eq (Split-Path $path -Leaf).ToLowerInvariant()) { continue }
            $seen[$key] = $true
            $beside = Join-Path $Dir $name
            if (Test-Path -LiteralPath $beside -PathType Leaf) { $queue.Enqueue($beside); continue }
            if ($key -match '^(api-ms-win-|ext-ms-)' -or (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$name"))) { continue }
            $problems += "missing DLL: $name, needed by $(Split-Path $path -Leaf)"
        }
    }
    return $problems
}

# A development run needs nothing on PATH either: everything the game loads sits beside it, as in a release.
function Install-GameRuntime([string]$Exe, [string]$Runtime, [string]$ToolBin) {
    $objdump = Join-Path $ToolBin 'llvm-objdump.exe'
    $bin = Split-Path $Exe -Parent
    $pin = Read-AnglePin
    foreach ($dll in (Get-DllClosure $Exe $ToolBin $objdump).Values) { Copy-IfChanged $dll $bin }
    foreach ($file in Get-AngleDlls $pin) {
        $dll = Join-Path $Runtime $file.Name
        Copy-IfChanged $dll $bin
        foreach ($needed in (Get-DllClosure $dll $ToolBin $objdump).Values) { Copy-IfChanged $needed $bin }
    }
    $problems = @(Get-AngleRuntimeProblems $bin $pin { param($path) Get-Imports $path $objdump })
    if ($problems.Count -gt 0) { Stop-Tool ($problems -join '; ') }
}

# Content fingerprint of a dependency's patch set: empty when it has none.
function Get-PatchFingerprint($files) {
    if (-not $files -or $files.Count -eq 0) { return '' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $text = New-Object System.Text.StringBuilder
        foreach ($file in $files) {
            $hex = [System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($file.FullName))).Replace('-', '')
            [void]$text.Append($file.Name).Append(':').Append($hex).Append("`n")
        }
        $hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text.ToString()))
        return [System.BitConverter]::ToString($hash).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}

# Fetches the pinned sources in gpu\deps.txt that are missing, at another commit or with a different patch set, then
# applies gpu\patches. The stamp holds the commit and, when the dependency is patched, its patch fingerprint.
function Sync-GpuDependencies {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { Stop-Tool 'git is needed to fetch the video core dependencies (gpu\deps.txt).' }
    $gpu = Join-Path $script:RepoRoot 'gpu'
    foreach ($line in Get-Content -LiteralPath (Join-Path $gpu 'deps.txt')) {
        if ($line -match '^\s*(#|$)') { continue }
        $path, $url, $commit = -split $line
        $dir = Join-Path $gpu $path
        $stamp = Join-Path $dir '.aln_commit'
        $patch_dir = Join-Path $gpu ('patches\' + (Split-Path $path -Leaf))
        $patches = @(Get-ChildItem -LiteralPath $patch_dir -Filter '*.patch' -File -ErrorAction SilentlyContinue |
            Sort-Object Name)
        $fingerprint = Get-PatchFingerprint $patches
        $expected = if ($fingerprint) { "$commit`n$fingerprint" } else { $commit }
        if ((Test-Path -LiteralPath $stamp) -and (Get-Content -LiteralPath $stamp -Raw).Trim() -eq $expected) { continue }
        Write-Step "Fetching $path ($($commit.Substring(0, 7)))"
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Invoke-Native git @('-C', $dir, 'init', '--quiet')
        Invoke-Native git @('-C', $dir, 'fetch', '--quiet', '--depth', '1', $url, $commit)
        if ($LASTEXITCODE -ne 0) { Stop-Tool "git fetch failed for $url" }
        Invoke-Native git @('-C', $dir, '-c', 'advice.detachedHead=false', 'checkout', '--quiet', 'FETCH_HEAD')
        if (Test-Path -LiteralPath (Join-Path $dir '.gitmodules')) {
            Invoke-Native git @('-C', $dir, 'submodule', 'update', '--quiet', '--init', '--recursive', '--depth', '1')
            if ($LASTEXITCODE -ne 0) { Stop-Tool "submodules of $path failed" }
        }
        foreach ($patch in $patches) {
            Invoke-Native git @('-C', $dir, 'apply', $patch.FullName)
            if ($LASTEXITCODE -ne 0) { Stop-Tool "patch $($patch.Name) does not apply to $path" }
        }
        [System.IO.File]::WriteAllText($stamp, $expected, [System.Text.Encoding]::ASCII)
    }
}
