<#
.SYNOPSIS
DEBUG_SAULO: TEMPORARY developer-only fixture for testing the Bug Report crash dump attachment by hand. Delete this file, its
test (tests\debug_saulo_crash_fixture_tests.cpp) and their entries in launcher\CMakeLists.txt and tools\test.ps1 once the
manual test is confirmed.

.DESCRIPTION
Creates one unused alienation_NNN.log + alienation_NNN.dmp pair in the log folder, with a marker file beside them so -Remove
deletes only what this tool made. Nothing in the launcher or the game knows about it: the launcher recognises the pair with
its normal crash detection. No process is crashed and neither alienation.exe nor the launcher is started.

The dump is a real minidump of this PowerShell process (MiniDumpWriteDump, MiniDumpNormal). Only if that fails or is outside
the production size limits, a minimal structurally valid MINIDUMP is written instead.

.PARAMETER LogDir
The log folder of the build whose launcher you test; default build\logs of this repository.

.PARAMETER Remove
Removes the fixtures made by this tool, and only those: a file is deleted only while it is byte for byte what was created.
#>
[CmdletBinding()]
param(
    [string]$LogDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\logs'),
    [switch]$Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$MarkerSuffix = '.debug_saulo_fixture'
$MarkerPattern = '^alienation_(\d+)\.debug_saulo_fixture$'
$FewestDumpBytes = 32
$MostDumpBytes = 8MB

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

public static class DebugSaulo
{
    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpWriteDump(IntPtr process, uint processId, IntPtr file, uint type, IntPtr exception, IntPtr user, IntPtr callback);

    public static bool WriteMiniDump(FileStream stream)
    {
        using (Process self = Process.GetCurrentProcess())
        {
            return MiniDumpWriteDump(self.Handle, (uint)self.Id, stream.SafeFileHandle.DangerousGetHandle(), 0, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        }
    }
}
'@

function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') } finally { $stream.Dispose(); $sha.Dispose() }
}

function Get-NextSessionNumber([string]$Directory) {
    $last = 0
    foreach ($file in [IO.Directory]::GetFiles($Directory, 'alienation_*')) {
        if ([IO.Path]::GetFileName($file) -match '^alienation_(\d+)(?:_\d+)?\.(?:log|dmp)$') { $last = [Math]::Max($last, [int]$Matches[1]) }
    }
    return $last + 1
}

function New-ExclusiveFile([string]$Path) {
    return New-Object IO.FileStream($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
}

function Get-FixtureLogText([string]$DumpName) {
    $stamp = (Get-Date).ToString('s')
    return (@(
        "[run] $stamp",
        'DEBUG_SAULO crash report fixture for manual testing',
        'Launcher: closed, starting the game',
        'ALN: game build CUSA00062 1.06',
        'Crash diagnostics: crash thread ready, minidump next to this log',
        "Runtime: guest thread 'guest' created (stack=1048576, prio=700)",
        'Online: connecting to the lobby',
        'DEBUG_SAULO no process crashed; this log and its dump were written by tools\debug-crash-report-fixture.ps1',
        'CRASH: access violation (0xc0000005) read at 0x0000000000000010',
        "  minidump: $DumpName (next to this log)"
    ) -join "`n") + "`n"
}

function Write-SyntheticMiniDump([IO.Stream]$Stream) {
    $writer = New-Object IO.BinaryWriter($Stream)
    $writer.Write([uint32]0x504d444d)  # Signature "MDMP"
    $writer.Write([uint32]0x0000a793)  # Version
    $writer.Write([uint32]1)           # NumberOfStreams
    $writer.Write([uint32]32)          # StreamDirectoryRva
    $writer.Write([uint32]0)           # CheckSum
    $writer.Write([uint32][DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
    $writer.Write([uint64]0)           # Flags: MiniDumpNormal
    $writer.Write([uint32]7)           # SystemInfoStream
    $writer.Write([uint32]56)          # MINIDUMP_SYSTEM_INFO size
    $writer.Write([uint32]44)          # its Rva, after the directory
    $writer.Write([uint16]9)           # PROCESSOR_ARCHITECTURE_AMD64
    $writer.Write([uint16]6)
    $writer.Write([uint16]0)
    $writer.Write([byte]1)             # NumberOfProcessors
    $writer.Write([byte]1)             # VER_NT_WORKSTATION
    $writer.Write([uint32][Environment]::OSVersion.Version.Major)
    $writer.Write([uint32][Environment]::OSVersion.Version.Minor)
    $writer.Write([uint32][Environment]::OSVersion.Version.Build)
    $writer.Write([uint32]2)           # VER_PLATFORM_WIN32_NT
    $writer.Write([uint32]0)           # CSDVersionRva
    $writer.Write([uint16]0)
    $writer.Write([uint16]0)
    $writer.Write((New-Object byte[] 24))
    $writer.Flush()
}

function Test-MiniDump([string]$Path) {
    $length = (Get-Item -LiteralPath $Path).Length
    if ($length -lt $FewestDumpBytes -or $length -gt $MostDumpBytes) { return $false }
    $head = New-Object byte[] 4
    $stream = [IO.File]::OpenRead($Path)
    try { [void]$stream.Read($head, 0, 4) } finally { $stream.Dispose() }
    return [Text.Encoding]::ASCII.GetString($head) -eq 'MDMP'
}

function New-DumpFile([string]$Path) {
    $stream = New-ExclusiveFile $Path
    $real = $false
    try { $real = [DebugSaulo]::WriteMiniDump($stream) } catch { $real = $false }
    $stream.Dispose()
    if ($real -and (Test-MiniDump $Path)) { return 'MiniDumpWriteDump' }
    $stream = New-Object IO.FileStream($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { Write-SyntheticMiniDump $stream } finally { $stream.Dispose() }
    return 'synthetic'
}

function New-Fixture([string]$Directory) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        $number = Get-NextSessionNumber $Directory
        $name = 'alienation_{0:000}' -f $number
        $log = Join-Path $Directory "$name.log"
        $dump = Join-Path $Directory "$name.dmp"
        $marker = Join-Path $Directory "$name$MarkerSuffix"
        try { $logStream = New-ExclusiveFile $log } catch [IO.IOException] { continue }
        try {
            $text = New-Object IO.StreamWriter($logStream, (New-Object Text.UTF8Encoding($false)))
            $text.Write((Get-FixtureLogText "$name.dmp"))
            $text.Dispose()
        } finally { $logStream.Dispose() }
        try { $kind = New-DumpFile $dump } catch [IO.IOException] { Remove-Item -LiteralPath $log; continue }
        try {
            $when = [DateTime]::UtcNow
            [IO.File]::SetLastWriteTimeUtc($log, $when)
            [IO.File]::SetLastWriteTimeUtc($dump, $when)
            $record = [ordered]@{
                fixture = 'DEBUG_SAULO'; number = $number; dump_kind = $kind
                log_sha256 = (Get-Sha256 $log)
                dump_sha256 = (Get-Sha256 $dump)
            }
            [IO.File]::WriteAllText($marker, ($record | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
        } catch {
            Remove-Item -LiteralPath $log, $dump -ErrorAction SilentlyContinue
            throw
        }
        return [pscustomobject]@{ Number = $number; Log = $log; Dump = $dump; DumpKind = $kind }
    }
    throw 'no unused log number could be reserved'
}

function Remove-Fixtures([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { Write-Host "No such folder: $Directory"; return }
    $removed = 0
    foreach ($markerFile in Get-ChildItem -LiteralPath $Directory -File | Where-Object { $_.Name -match $MarkerPattern }) {
        $name = 'alienation_{0:000}' -f [int]([regex]::Match($markerFile.Name, $MarkerPattern).Groups[1].Value)
        $record = Get-Content -LiteralPath $markerFile.FullName -Raw | ConvertFrom-Json
        $kept = $false
        foreach ($item in @(@{ File = "$name.log"; Hash = $record.log_sha256 }, @{ File = "$name.dmp"; Hash = $record.dump_sha256 })) {
            $path = Join-Path $Directory $item.File
            if (-not (Test-Path -LiteralPath $path)) { continue }
            if ((Get-Sha256 $path) -eq $item.Hash) {
                Remove-Item -LiteralPath $path
                $removed++
                Write-Host "Removed $($item.File)"
            } else {
                $kept = $true
                Write-Warning "$($item.File) is no longer what this tool created; left in place"
            }
        }
        if (-not $kept) { Remove-Item -LiteralPath $markerFile.FullName }
    }
    if ($removed -eq 0) { Write-Host 'No DEBUG_SAULO fixture to remove.' }
}

if ($Remove) {
    Remove-Fixtures $LogDir
    return
}

$fixture = New-Fixture $LogDir
$log = Get-Item -LiteralPath $fixture.Log
$dump = Get-Item -LiteralPath $fixture.Dump
$difference = [Math]::Abs(($log.LastWriteTimeUtc - $dump.LastWriteTimeUtc).TotalSeconds)
$name = [IO.Path]::GetFileNameWithoutExtension($fixture.Log)
$command = ".\tools\debug-crash-report-fixture.ps1" + $(if ($PSBoundParameters.ContainsKey('LogDir')) { " -LogDir `"$LogDir`"" } else { '' })

Write-Host "DEBUG_SAULO crash report fixture created"
Write-Host "  number:          $($fixture.Number)"
Write-Host "  log:             $($fixture.Log)"
Write-Host "  dump:            $($fixture.Dump)"
Write-Host "  log size:        $($log.Length) bytes"
Write-Host "  dump size:       $($dump.Length) bytes ($($fixture.DumpKind))"
Write-Host "  time difference: $difference s"
Write-Host ''
Write-Host 'Manual test:'
Write-Host '  1. Open the Alienation launcher yourself.'
Write-Host '  2. Open Bug Report.'
Write-Host "  3. Find $name.log in the list of logs."
Write-Host '  4. Confirm "Crash detected" appears on that row.'
Write-Host '  5. Select "Send this log" on it.'
Write-Host '  6. Confirm "Attach crash report" appears and is checked by default.'
Write-Host '  7. Confirm the help text under it appears (crash dump, process memory used for diagnostics).'
Write-Host '  8. Describe the problem and send the report with the checkbox on.'
Write-Host '  9. Open the admin dashboard and the new report.'
Write-Host ' 10. Confirm "Download log" and "Download minidump" both appear.'
Write-Host ' 11. Download both.'
Write-Host " 12. Confirm the downloaded .dmp is $($dump.Length) bytes."
Write-Host ' 13. Repeat with "Attach crash report" unchecked: the new report must have only the log, no "Download minidump".'
Write-Host ''
Write-Host "Remove it afterwards: $command -Remove"
