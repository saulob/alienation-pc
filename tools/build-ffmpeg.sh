#!/usr/bin/env bash
# Builds the FFmpeg pinned in gpu/ffmpeg.txt into PREFIX: the same configuration on both platforms. tools\build.ps1 runs
# it in MSYS2 CLANG64, packaging/linux/ci-deps.sh on Linux. Nothing is rebuilt while PREFIX holds the build of the
# current pin and script.
#
#   tools/build-ffmpeg.sh PREFIX [WORK]     WORK keeps the downloaded tarball (default: a temporary folder)
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }
to_unix() { if command -v cygpath >/dev/null; then cygpath -u "$1"; else printf '%s' "$1"; fi; }

prefix=$(to_unix "${1:?usage: tools/build-ffmpeg.sh PREFIX [WORK]}")
pin=$root/gpu/ffmpeg.txt
field() { awk -v key="$1" '$1 == key { $1 = ""; sub(/^ /, ""); print }' "$pin"; }
version=$(field version) url=$(field url) sha256=$(field sha256)
[[ -n $version && $url == https://* && $sha256 =~ ^[0-9a-f]{64}$ ]] || die "$pin needs version, an https url and sha256"
case $(uname -s) in
    MINGW*|MSYS*) platform=windows ;;
    Linux) platform=linux ;;
    *) die "Unsupported system: $(uname -s)" ;;
esac
cc=${CC:-clang}
read -r -a options <<<"$(field configure) $(field "$platform")"

licenses=$prefix/share/licenses/ffmpeg
stamp=$licenses/aln-build.stamp
expected=$(cat "$pin" "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)
if [[ -f $stamp && $(<"$stamp") == "$expected" ]]; then
    echo "FFmpeg $version is current in $prefix"
    exit 0
fi
for tool in curl tar xz make nasm sha256sum "$cc"; do
    command -v "$tool" >/dev/null || die "Missing $tool"
done

if [[ -n ${2:-} ]]; then
    work=$(to_unix "$2")
    mkdir -p "$work"
else
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
fi
tarball=$work/ffmpeg-$version.tar.xz
if ! echo "$sha256  $tarball" | sha256sum -c --status - 2>/dev/null; then
    echo "Fetching FFmpeg $version"
    curl -fsSL --retry 3 -o "$tarball.part" "$url"
    echo "$sha256  $tarball.part" | sha256sum -c --status - ||
        { rm -f "$tarball.part"; die "$url does not match the SHA-256 in gpu/ffmpeg.txt"; }
    mv -f "$tarball.part" "$tarball"
fi
rm -rf "$work/build" "$work/ffmpeg-$version"
mkdir -p "$work/build"
tar -xJf "$tarball" -C "$work/build"
mv "$work/build/ffmpeg-$version" "$work/build/src"
src=$work/build/src
cd "$work/build"

echo "Building FFmpeg $version ($platform) into $prefix"
# The libraries carry their configure line and source file names. Sources in ./src make configure use the relative
# path "src", and on Windows a neutral prefix keeps the install folder out of them.
configure_prefix=$prefix
[[ $platform == windows ]] && configure_prefix=/ffmpeg
src/configure --prefix="$configure_prefix" --cc="$cc" "${options[@]}" >configure.log 2>&1 ||
    { tail -40 configure.log >&2; die 'FFmpeg configure failed'; }
grep -qx 'License: LGPL version 2.1 or later' configure.log || die 'FFmpeg was not configured as LGPL 2.1 or later'
make -j"$(nproc)" >make.log 2>&1 || { tail -40 make.log >&2; die 'FFmpeg build failed'; }
if [[ $platform == windows ]]; then
    make install DESTDIR="$work/install" >install.log 2>&1 || { tail -20 install.log >&2; die 'FFmpeg install failed'; }
    rm -rf "$prefix"
    mkdir -p "$(dirname -- "$prefix")"
    cp -a "$work/install/ffmpeg" "$prefix"
    rm -rf "$work/install"
    # pkgconf and clang are Windows programs: they need the prefix as S:/...
    sed -i "s|=/ffmpeg|=$(cygpath -m "$prefix")|g" "$prefix"/lib/pkgconfig/libav*.pc "$prefix"/lib/pkgconfig/libsw*.pc
    ! grep -qE '(=|-[IL])/ffmpeg(/|$)' "$prefix"/lib/pkgconfig/*.pc || die 'pkg-config files still name /ffmpeg'
else
    make install >install.log 2>&1 || { tail -20 install.log >&2; die 'FFmpeg install failed'; }
fi
needles=("$work")
[[ $platform == windows ]] && needles+=("$(cygpath -m "$work")")
for lib in "$prefix"/bin/av*.dll "$prefix"/bin/sw*.dll "$prefix"/lib/libav*.so.* "$prefix"/lib/libsw*.so.*; do
    [[ -f $lib && ! -L $lib ]] || continue
    for needle in "${needles[@]}"; do
        ! grep -aqF -- "$needle" "$lib" || die "$lib names the build folder $needle"
    done
done
mkdir -p "$licenses"
cp -f "$src/LICENSE.md" "$src/COPYING.LGPLv2.1" "$licenses/"
printf '%s\n%s\n' "FFmpeg $version, $url (SHA-256 $sha256)" "./configure ${options[*]}" >"$licenses/BUILD.txt"
printf '%s' "$expected" >"$stamp"
echo "FFmpeg $version installed in $prefix"
