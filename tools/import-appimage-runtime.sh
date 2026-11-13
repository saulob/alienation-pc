#!/usr/bin/env bash
# Copies the reviewed AppImage runtime to TARGET from a WSL packaging cache that holds it: this project's, or the
# sibling projects' that pin the same runtime. Only a file with SHA256 is copied.
# tools\package-release.ps1 runs this when cache\appimage-runtime\runtime-x86_64 is missing.
#
#   tools/import-appimage-runtime.sh SHA256 TARGET
set -euo pipefail
sha256=${1:?usage: tools/import-appimage-runtime.sh SHA256 TARGET}
target=${2:?usage: tools/import-appimage-runtime.sh SHA256 TARGET}
[[ $sha256 =~ ^[0-9a-f]{64}$ ]] || { echo "ERROR: not a SHA-256: $sha256" >&2; exit 2; }
cache=${XDG_CACHE_HOME:-$HOME/.cache}
for candidate in "$cache/alienation-pc-build/build/appimage/runtime-aln-x86_64" \
                 "$cache/13-sentinels-aegis-rim-pc-build/build/appimage/runtime-x86_64" \
                 "$cache/dragons-crown-pro-pc-build/build/appimage/runtime-dcp-x86_64" \
                 "$cache/odin-sphere-leifthrasir-pc-build/build/appimage/runtime-osl-x86_64"; do
    [[ -f $candidate && $(sha256sum -- "$candidate" | cut -d' ' -f1) == "$sha256" ]] || continue
    cp -- "$candidate" "$target.part"
    [[ $(sha256sum -- "$target.part" | cut -d' ' -f1) == "$sha256" ]] || { rm -f -- "$target.part"; continue; }
    mv -f -- "$target.part" "$target"
    echo "AppImage runtime: the reviewed runtime copied from $candidate"
    exit 0
done
echo 'AppImage runtime: no WSL packaging cache holds the reviewed runtime' >&2
exit 1
