#!/usr/bin/env bash
# Builds alienation on Linux x86_64 incrementally (Clang, CMake + Ninja): the Linux counterpart of tools/build.ps1.
# The one build configuration, RelWithDebInfo, is what is tested and what an AppImage contains.
#
#   tools/build.sh            build build/cmake/linux/bin/alienation
#   tools/build.sh --clean    delete build/cmake/linux first
#   tools/build.sh --test     also build and run the focused tests
#   tools/build.sh --run [args...]   start the loader that was just built (arguments after --run go to it)
#
# Missing tools or libraries are listed; packaging/linux/ci-deps.sh installs them on Ubuntu 20.04 (the release baseline).
# Every run writes one new log, build/logs/build/alienation_NNN.log (never overwritten, kept when the build fails).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$root/build/cmake/linux
config=RelWithDebInfo
clean=0 test=0 run=0
loader_args=()
while (($#)); do
    case $1 in
        --clean) clean=1 ;;
        --test) test=1 ;;
        --run) run=1; shift; loader_args=("$@"); break ;;
        -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This script builds for Linux x86_64 only (Windows: tools\build.ps1).'

# BEGIN build log
log_dir=$root/build/logs/build
project_name() {
    local name
    name=$(sed -n 's/.*"game"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$root/.recomp.json" 2>/dev/null | head -n 1)
    [[ -n $name ]] || name=$(basename -- "$root")
    name=$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9][^a-z0-9]*/_/g; s/^_//; s/_$//')
    printf '%s' "${name:-project}"
}
# Reserves <project>_NNN.log with an exclusive create (noclobber), so concurrent builds never share a number.
reserve_build_log() {
    local name=$1 last file n attempt
    mkdir -p -- "$log_dir"
    for attempt in $(seq 1 1000); do
        last=0
        for file in "$log_dir/${name}_"*.log; do
            [[ -e $file ]] || continue
            n=${file##*_}
            n=${n%.log}
            if [[ $n =~ ^[0-9]+$ ]] && ((10#$n > last)); then last=$((10#$n)); fi
        done
        build_log=$(printf '%s/%s_%03d.log' "$log_dir" "$name" $((last + 1)))
        if (set -o noclobber; : > "$build_log") 2>/dev/null; then return 0; fi
    done
    return 1
}
build_log_open=0
close_build_log() { # exit code; idempotent, never changes the exit code
    local code=$1
    ((build_log_open)) || return 0
    build_log_open=0
    exec 1>&3 2>&4
    [[ -n ${tee_pid:-} ]] && wait "$tee_pid" 2>/dev/null || true
    {
        printf '\nWarnings: %s\n' "$(grep -ciE '(^|[^[:alpha:]])warning:' "$build_log" || true)"
        printf 'Errors: %s\n' "$(grep -ciE '(^|[^[:alpha:]])error:|^FAILED:' "$build_log" || true)"
        printf 'Result: %s\nExit code: %d\nFinished: %s\n' "$( ((code == 0)) && echo SUCCESS || echo FAILED)" "$code" "$(date '+%Y-%m-%d %H:%M:%S %z')"
    } >> "$build_log"
    echo "Build log: $build_log"
}
project=$(project_name)
reserve_build_log "$project" || die "Could not reserve a build log in $log_dir"
exec 3>&1 4>&2
exec > >(tee -a "$build_log") 2>&1
tee_pid=$!
build_log_open=1
trap 'close_build_log $?' EXIT
printf 'Project: %s\nStarted: %s\nConfiguration: %s\nMode: %s\n\n' "$project" "$(date '+%Y-%m-%d %H:%M:%S %z')" "$config" \
    "$( ((clean)) && echo 'complete (--clean)' || echo incremental)"
# END build log

cc=${CC:-clang} cxx=${CXX:-clang++}
missing=()
for tool in "$cc" "$cxx" cmake ninja pkg-config git; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
command -v glslang >/dev/null || command -v glslangValidator >/dev/null || missing+=(glslang)
if command -v pkg-config >/dev/null; then
    for module in sdl3 freetype2 libpng zlib openssl libxxhash vulkan; do
        pkg-config --exists "$module" || missing+=("pkg-config:$module")
    done
fi
if ((${#missing[@]})); then
    echo "Missing: ${missing[*]}" >&2
    echo 'packaging/linux/ci-deps.sh prepares the Ubuntu 20.04 release build environment (docs/LINUX.md).' >&2
    exit 1
fi

step "Toolchain $($cc --version | head -1), $config"

step 'Video core dependencies (gpu/deps.txt)'
patches_of() {
    local LC_ALL=C dir=$1 file
    PATCHES=()
    for file in "$dir"/*.patch; do
        [[ -f $file ]] || continue
        PATCHES+=("$file")
    done
}
patches_fingerprint() {
    local file hash
    (($#)) || return 0
    for file in "$@"; do
        hash=$(sha256sum < "$file" | cut -d' ' -f1)
        printf '%s:%s\n' "$(basename "$file")" "${hash^^}"
    done | sha256sum | cut -d' ' -f1 | tr 'a-f' 'A-F'
}
while read -r path url commit; do
    [[ -z ${path:-} || $path == \#* ]] && continue
    dir=$root/gpu/$path
    patches_of "$root/gpu/patches/$(basename "$path")"
    fingerprint=$(patches_fingerprint "${PATCHES[@]}")
    expected=$commit
    [[ -n $fingerprint ]] && expected=$commit$'\n'$fingerprint
    [[ -f $dir/.aln_commit && $(<"$dir/.aln_commit") == "$expected" ]] && continue
    echo "Fetching $path (${commit:0:7})"
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init --quiet
    git -C "$dir" fetch --quiet --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
    [[ -f $dir/.gitmodules ]] && git -C "$dir" submodule update --quiet --init --recursive --depth 1
    for patch in "${PATCHES[@]}"; do
        git -C "$dir" apply "$patch"
    done
    printf '%s' "$expected" > "$dir/.aln_commit"
done < "$root/gpu/deps.txt"

((clean)) && { step 'Cleaning build/cmake/linux'; rm -rf "$build_dir"; }
if ! grep -qs "CMAKE_BUILD_TYPE:STRING=$config" "$build_dir/CMakeCache.txt" || [[ ! -f $build_dir/build.ninja ]]; then
    step 'Configuring'
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=$config \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx"
fi
step 'Building'
targets=(alienation)
tests=(aln-image-format-tests aln-resolution-patch-tests aln-runtime-tests aln-runtime-overlay-tests aln-module-history-tests aln-rtc-tests aln-ime-keyboard-tests aln-json2-tests aln-thread-cancel-tests aln-tiling-tests aln-renderer-selection-tests aln-opengl-backend-tests aln-opengl-perf-tests aln-cursor-tests aln-window-tests aln-sdl-compat-tests aln-present-tests aln-trophy-tests aln-incident-tests aln-update-tests)
((test)) && targets+=("${tests[@]}")
ninja -C "$build_dir" "${targets[@]}"
loader=$build_dir/bin/alienation
[[ -x $loader ]] || die "Missing $loader"

if ((test)); then
    for t in "${tests[@]}"; do
        step "Running $t"
        "$build_dir/bin/$t"
    done
fi

image_current() {
    local view=$1 image=$2 source
    [[ -f $image/boot.bin && -f $image/content.bin && -f $image/image.json ]] || return 1
    [[ $view/.aln_view.json -nt $image/boot.bin ]] && return 1
    for source in launcher/boot_image.cpp launcher/game_profile.h launcher/game_profile.cpp src/aln_image_format.h; do
        [[ $root/$source -nt $image/boot.bin ]] && return 1
    done
    [[ $(head -c 8 "$image/boot.bin") == ALNBOOT1 && $(head -c 8 "$image/content.bin") == ALNCONT1 &&
       $(stat -c %s "$image/content.bin") == 28 ]]
}
installed=0
for view in "$root"/game/prepared/CUSA[0-9][0-9][0-9][0-9][0-9]-*; do
    [[ -f $view/.aln_view.json ]] || continue
    installed=1
    folder=${view##*/} image=$root/build/image/${view##*/}
    image_current "$view" "$image" && continue
    step "Updating the boot image of $folder"
    "$loader" --prepare-image "$view" "$image"
done
((installed)) ||
    echo 'The game is not installed yet: start the loader and use Install game... on the launcher'\''s Play page.'
printf '\nBuilt %s\n' "$loader"
((run)) || exit 0
close_build_log 0
cd -- "$(dirname -- "$loader")"
exec "$loader" "${loader_args[@]}"
