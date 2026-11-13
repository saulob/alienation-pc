#!/usr/bin/env bash
# Builds alienation on Linux x86_64 incrementally (Clang, CMake + Ninja): the Linux counterpart of tools/build.ps1.
# The one build configuration, RelWithDebInfo, is what is tested and what an AppImage contains.
#
#   tools/build.sh            build build/cmake/linux/bin/alienation
#   tools/build.sh --clean    delete build/cmake/linux first
#   tools/build.sh --test     also build and run the portable focused tests (the Linux counterpart of tools/test.ps1)
#   tools/build.sh --run [args...]   start the loader that was just built (arguments after --run go to it)
#
# Missing tools or libraries are listed; packaging/linux/ci-deps.sh installs them on Ubuntu 20.04 (the release baseline).
# Every run writes one new log (never overwritten, kept when the build fails): build/logs/build/build_NNN.log, or with
# --run the session log build/logs/alienation_NNN.log, which holds the [build] section and then the game's [run] lines.
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
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This script builds for Linux x86_64 only (Windows: tools\build.ps1).'

# BEGIN build log
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
# With --run the game continues this log, so one invocation has one log.
if ((run)); then log_dir=$root/build/logs log_name=$project; else log_dir=$root/build/logs/build log_name=build; fi
reserve_build_log "$log_name" || die "Could not reserve a build log in $log_dir"
exec 3>&1 4>&2
exec > >(tee -a "$build_log") 2>&1
tee_pid=$!
build_log_open=1
trap 'close_build_log $?' EXIT
((run)) && printf '[build]\n'
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
tests=(aln-image-format-tests aln-resolution-patch-tests aln-frame-pacing-tests aln-runtime-tests aln-sigpipe-tests aln-runtime-overlay-tests aln-savedata-tests aln-np-tests aln-online-status-tests aln-online-view-tests aln-local-pad-tests aln-controller-prompts-tests aln-exit-game-tests aln-exit-game-menu-tests aln-text-fix-tests aln-menu-buttons-tests aln-skip-intro-tests aln-module-history-tests aln-rtc-tests aln-ime-keyboard-tests aln-json2-tests aln-thread-cancel-tests aln-avplayer-thread-tests aln-tiling-tests aln-gds-lane-offset-tests aln-gpu-crash-trace-tests aln-gpu-recording-tests aln-renderer-selection-tests aln-opengl-backend-tests aln-opengl-perf-tests aln-cursor-tests aln-window-tests aln-sdl-compat-tests aln-present-tests aln-trophy-tests aln-incident-tests aln-paths-tests aln-game-install-tests aln-update-tests aln-update-network-tests)
# Run with a loopback development lobby below. The Windows-only suites (memory, log, crash guard, update helper) are
# not defined here; tests/feature_parity_tests.py names their Linux counterparts.
lobby_tests=(aln-online-hle-tests)
((test)) && targets+=("${tests[@]}" "${lobby_tests[@]}")
ninja -C "$build_dir" "${targets[@]}"
loader=$build_dir/bin/alienation
[[ -x $loader ]] || die "Missing $loader"

# The portable Python suites of tools/test.ps1; they need the project .venv (cryptography, pillow, numpy).
python_tests=(tests/game_profile_tests.py tests/feature_parity_tests.py tests/release_contract_tests.py tests/patch_site_tests.py tests/display_aspect_tests.py tests/controller_prompts_static_tests.py tests/prompt_language_tests.py tools/prompts/verify_prompts.py)
if ((test)); then
    # Every suite runs even after one fails; a missing dependency fails its suites by name.
    export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    unset ALN_LOBBY ALN_ONLINE ALN_ONLINE_NAME ALN_UPNP ALN_UDP_PORT ALN_STUN ALN_TEST_LOBBY
    passed=() failed=() skipped=()
    checks=0
    out=$(mktemp)
    suite() { # name command...
        local name=$1 code=0 count
        shift
        step "Running $name"
        "$@" >"$out" 2>&1 || code=$?
        cat "$out"
        count=$(grep -oE '[0-9]+ checks' "$out" | tail -1 | grep -oE '^[0-9]+' || true)
        checks=$((checks + ${count:-0}))
        if ((code == 0)) && ! grep -qE '^\s*(FAIL|FAILED)\b' "$out"; then passed+=("$name"); else failed+=("$name"); fi
        grep -E '^\s*skipped\b' "$out" | sed "s|^ *|$name: |" >>"$out.skipped" || true
    }
    missing() { step "Running $1"; echo "MISSING: $2"; failed+=("$1"); }

    for t in "${tests[@]}"; do suite "$t" "$build_dir/bin/$t"; done

    step 'Every test target is run'
    unlisted=$(ninja -C "$build_dir" -t targets all | grep -oE '^aln-[a-z0-9-]+-tests:' | tr -d : | sort -u |
               grep -vxF -f <(printf '%s\n' "${tests[@]}" "${lobby_tests[@]}") || true)
    if [[ -n $unlisted ]]; then echo "Not run by tools/build.sh --test: $unlisted"; failed+=(coverage); else passed+=(coverage); fi

    venv_python=$root/.venv/bin/python
    if [[ ! -x $venv_python ]]; then
        for t in "${python_tests[@]}"; do missing "$t" 'the project .venv: python3 -m venv .venv && .venv/bin/pip install cryptography pillow numpy'; done
    elif ! "$venv_python" -I -c 'import cryptography, PIL, numpy'; then
        for t in "${python_tests[@]}"; do missing "$t" 'cryptography, pillow or numpy in .venv'; done
    else
        for t in "${python_tests[@]}"; do suite "$t" "$venv_python" -I "$root/$t"; done
    fi

    node_ok=0
    if command -v node >/dev/null &&
       node -e 'process.exit(process.versions.node.split(".").map(Number).reduce((a, n, i) => a || (n - [22, 13, 0][i]), 0) >= 0 ? 0 : 1)'; then
        node_ok=1
    fi
    if ((!node_ok)); then
        missing 'lobby backend tests' 'Node.js 22.13 or later (see docs/ONLINE.md)'
        for t in "${lobby_tests[@]}"; do missing "$t" 'Node.js 22.13 or later for the loopback lobby'; done
    else
        suite 'lobby backend tests' bash -c "cd '$root/server/lobby' && node --no-warnings --test 'test/*.test.js'"
        # The development lobby in memory on a free loopback port; it never reaches the production service.
        port=$(node -e 'const s = require("net").createServer().listen(0, "127.0.0.1", () => { console.log(s.address().port); s.close(); })')
        node --no-warnings "$root/server/lobby/local/server.js" --host 127.0.0.1 --port "$port" >/dev/null 2>&1 &
        lobby_pid=$!
        for _ in $(seq 1 40); do
            curl -fs "http://127.0.0.1:$port/" >/dev/null 2>&1 && break
            sleep 0.25
        done
        if curl -fs "http://127.0.0.1:$port/" >/dev/null; then
            for t in "${lobby_tests[@]}"; do suite "$t" env ALN_TEST_LOBBY="ws://127.0.0.1:$port/" "$build_dir/bin/$t"; done
        else
            for t in "${lobby_tests[@]}"; do missing "$t" 'the development lobby did not start'; done
        fi
        kill "$lobby_pid" 2>/dev/null || true
    fi

    if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        suite 'git diff --check' bash -c "git -C '$root' diff --check && git -C '$root' diff --cached --check"
    else
        echo "git diff --check: skipped, $root is not a git checkout (tools/test.ps1 checks the checkout)" >>"$out.skipped"
    fi

    step 'Platform parity (scripts/feature_parity.py)'
    if [[ -x $venv_python ]]; then "$venv_python" -I "$root/scripts/feature_parity.py" --summary || failed+=('platform parity'); fi

    step 'Summary'
    printf 'total suites: %d   passed: %d   failed: %d   checks counted: %d\n' \
        $((${#passed[@]} + ${#failed[@]})) "${#passed[@]}" "${#failed[@]}" "$checks"
    if [[ -s $out.skipped ]]; then echo 'parts the suites skipped (not failures):'; sed 's/^/  /' "$out.skipped" | sort -u; fi
    rm -f "$out" "$out.skipped"
    ((${#failed[@]} == 0)) || die "FAILED: ${failed[*]}"
    echo 'All portable suites passed.'
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
trap - EXIT
build_section=$(cat -- "$build_log")
run_args=("${loader_args[@]}")
if [[ " ${loader_args[*]} " != *" --log "* && " ${loader_args[*]} " != *" --logs "* ]]; then
    run_args=(--log "$build_log" "${loader_args[@]}")
fi
cd -- "$(dirname -- "$loader")"
code=0
"$loader" "${run_args[@]}" || code=$?
# The runtime deletes its log after a clean exit when detailed logging is off; the build section stays.
[[ -e $build_log ]] || printf '%s\n' "$build_section" > "$build_log"
printf '[run] alienation exit code %d\n' "$code" >> "$build_log"
echo "Session log: $build_log"
exit "$code"
