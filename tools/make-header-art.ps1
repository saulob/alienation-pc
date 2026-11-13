# Regenerates assets/launcher/header-art.png from header-art-source.png (needs ffmpeg on PATH).
$ErrorActionPreference = 'Stop'
$assets = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\launcher'
$bandTop = 110
$bandRows = 300
$fadeStart = 0.22
$fadeEnd = 0.55
$u = "clip((X/W-$fadeStart)/($fadeEnd-$fadeStart),0,1)"
$graph = "[0]crop=iw:${bandRows}:0:${bandTop},format=rgba,scale=-2:432:flags=lanczos,pad=3840:432:ow-iw:0:color=0x00000000," +
         "geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*$u*$u*(3-2*$u)'[out]"
& ffmpeg -v error -y -i (Join-Path $assets 'header-art-source.png') -filter_complex $graph -map '[out]' -frames:v 1 `
    -compression_level 9 (Join-Path $assets 'header-art.png')
if ($LASTEXITCODE -ne 0) { throw 'ffmpeg failed' }
