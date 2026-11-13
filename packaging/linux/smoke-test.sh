#!/usr/bin/env bash
# Starts the AppImage's launcher on a virtual display with no game files and checks that it stays open (a missing
# glibc version or library stops it at startup), and that its data lands next to the AppImage, never inside it.
# Three starts: the extracted AppRun, --appimage-extract-and-run, and the file itself (a FUSE mount where FUSE works,
# otherwise the patched runtime's extract-and-run fallback). Nothing is shown on a real display.
# Run as root in a fresh container of the distribution to check: it installs Xvfb, a font and a software Vulkan
# driver, as a desktop has them. Elsewhere it needs Xvfb already.
# Usage: smoke-test.sh path/to/AlienationPC-*.AppImage
set -euo pipefail
appimage=$(readlink -f -- "$1")
if [[ $(id -u) == 0 ]]; then
    if command -v apt-get >/dev/null; then
        apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq xvfb fonts-dejavu-core libvulkan1 mesa-vulkan-drivers >/dev/null
    elif command -v dnf >/dev/null; then
        dnf install -y -q xorg-x11-server-Xvfb dejavu-sans-fonts vulkan-loader mesa-vulkan-drivers >/dev/null
    elif command -v zypper >/dev/null; then
        zypper -nq install xorg-x11-server-Xvfb dejavu-fonts libvulkan1 Mesa-libVulkan-devel >/dev/null
    elif command -v pacman >/dev/null; then
        pacman -Sy --noconfirm --needed xorg-server-xvfb ttf-dejavu vulkan-icd-loader vulkan-swrast >/dev/null
    fi
fi
grep -m1 PRETTY_NAME /etc/os-release || true
ldd --version | sed -n 1p # head would end ldd with SIGPIPE (pipefail)

work=$(mktemp -d)
trap 'kill "${xvfb:-0}" 2>/dev/null || true; rm -rf "$work"' EXIT
mkdir "$work/home" "$work/beside" "$work/tmp"
cp "$appimage" "$work/beside/game.AppImage"
chmod 755 "$work/beside/game.AppImage"
cd "$work"
"$work/beside/game.AppImage" --appimage-extract >/dev/null
[[ -x squashfs-root/AppRun && -x squashfs-root/usr/bin/alienation ]] || { echo 'FAIL: --appimage-extract' >&2; exit 1; }
display=:$((90 + RANDOM % 9))
Xvfb "$display" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
# Only the virtual display: no Wayland socket (WSLg keeps one in XDG_RUNTIME_DIR) and no audio device.
mkdir -m 700 "$work/runtime"
export DISPLAY=$display HOME=$work/home TMPDIR=$work/tmp XDG_RUNTIME_DIR=$work/runtime SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy
unset WAYLAND_DISPLAY
sleep 2
find squashfs-root | sort > before.txt

# The launcher must still be running when timeout stops it (status 124), with its window shown and its data beside
# the AppImage file.
expect_launcher() { # label status log
    local label=$1 status=$2 log=$3
    cat "$log"
    if [[ $status != 124 ]] || grep -qE 'GLIBC_|GLIBCXX|CXXABI|error while loading|cannot open shared object' "$log"; then
        echo "FAIL ($label): the launcher exited ($status) instead of staying open" >&2
        exit 1
    fi
    grep -q 'Launcher: shown' "$log" || { echo "FAIL ($label): the launcher did not open its window" >&2; exit 1; }
    [[ -d $work/beside/logs ]] || { echo "FAIL ($label): no logs folder beside the AppImage" >&2; exit 1; }
    echo "OK ($label)"
    rm -rf "$work/beside/logs" "$work/beside/user"
}

status=0
APPIMAGE="$work/beside/game.AppImage" timeout 15 squashfs-root/AppRun > extracted.log 2>&1 || status=$?
expect_launcher 'extracted AppRun' "$status" extracted.log
if ! diff <(find squashfs-root | sort) before.txt >/dev/null; then
    echo 'FAIL: data was written inside the AppImage contents' >&2
    exit 1
fi

status=0
timeout 20 "$work/beside/game.AppImage" --appimage-extract-and-run > extract-and-run.log 2>&1 || status=$?
expect_launcher '--appimage-extract-and-run' "$status" extract-and-run.log

if [[ -e /dev/fuse ]] && { command -v fusermount || command -v fusermount3; } >/dev/null; then
    mode='FUSE mount'
else
    mode='no FUSE: the runtime falls back to extract-and-run'
fi
status=0
timeout 20 "$work/beside/game.AppImage" > direct.log 2>&1 || status=$?
expect_launcher "the AppImage file ($mode)" "$status" direct.log

sleep 1
if [[ -n $(find "$work/tmp" -mindepth 1 -maxdepth 1 -name 'appimage_extracted_*' 2>/dev/null) ]]; then
    echo 'FAIL: the runtime left its extraction folder behind' >&2
    exit 1
fi
echo "OK: the launcher stayed open in every start mode and kept its data beside the AppImage"
