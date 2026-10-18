#!/usr/bin/env bash
# Linux lanes on the WSL reference host, built and tested from the source mirror on the native ext4 disk
# (~/cerid-src) instead of the Windows checkout through drvfs.
# Contract: docs/design/build-performance.md#wsl-source-mirror.
#
#   wsl-lane.sh sync                              copy the checkout into the mirror, then check it
#   wsl-lane.sh check                             exit 3 when the mirror differs from the checkout
#   wsl-lane.sh configure <preset> [cmake args]   sync, check, configure the lane's tree from the mirror
#   wsl-lane.sh build <preset> [target ...]       sync, check, build (every target when none is named)
#   wsl-lane.sh test <preset> [ctest args ...]    check, then CTest in the lane's tree with the test preset's
#                                                 environment, jobs and timeout (as the hosted lane's --preset)
#   wsl-lane.sh run <preset> -- command ...       check, then run a command from a fresh scratch directory
#   wsl-lane.sh dev <preset> [check args ...]     check, then dev.py check in the lane's tree from the mirror, with the
#                                                 build's cache server and the test preset's environment (the focused
#                                                 plan's lane route: docs/design/developer-workflow.md#risk-routes)
#   wsl-lane.sh tree <preset>                     print the lane's build directory
#
# test, run and dev check without syncing: a source edited after the build is refused instead of being tested against
# binaries built from older sources. Run it through the checkout's path (bash /mnt/d/.../scripts/wsl-lane.sh) so the
# mirror's source is known. One heavy job at a time: configure, build, test, run and dev wait for
# ~/.cerid/wsl-lane.lock.
# Environment: CRD_LANE_ROOT (the trees, default ~/cerid-mirror-build), CRD_SOURCE_MIRROR (default ~/cerid-src),
# CRD_LANE_JOBS (default 16), CRD_LANE_TIMEOUT (seconds per heavy command, default 3500), CRD_LANE_KEEP_GOING (1: build
# past failures, so a change to a widely included header shows every unit it breaks in one round), CRD_SCCACHE, CRD_MOLD and
# the other compiler-cache.py settings (the measured lanes link with the pinned mold, its threads per link the CPUs
# divided by CRD_LANE_JOBS), VULKAN_SDK (default ~/cerid-deps/vulkan-sdk), CRD_VULKAN_VALIDATION (the pinned
# validation layer, default ~/.cerid/vulkan-validation). The lane exports S (the mirror) and B (the tree) to run's
# command.
set -euo pipefail
unset DEBUGINFOD_URLS
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MIRROR=${CRD_SOURCE_MIRROR:-$HOME/cerid-src}
ROOT=${CRD_LANE_ROOT:-$HOME/cerid-mirror-build}
JOBS=${CRD_LANE_JOBS:-16}
# compiler-cache.py sizes mold's threads per link from the lane's job count at configure time.
export CRD_LANE_JOBS=$JOBS
LIMIT=${CRD_LANE_TIMEOUT:-3500}
LOCK=$HOME/.cerid/wsl-lane.lock
export CRD_SOURCE_MIRROR=$MIRROR
# The Linux sccache server's own port: WSL2 relays a Linux localhost listener to Windows, so on the default port the
# Windows clients reached this server (compiler-cache.py server_port, 2026-10-18).
SCCACHE_SERVER_PORT=$(python3 "$HERE/compiler-cache.py" port)
export SCCACHE_SERVER_PORT
export VULKAN_SDK=${VULKAN_SDK:-$HOME/cerid-deps/vulkan-sdk}
export CRD_PLATFORM_HEADLESS=1
export TMPDIR=${TMPDIR:-/tmp} TEMP=${TEMP:-/tmp} TMP=${TMP:-/tmp}
# The pinned validation layer, as the hosted lanes install it (scripts/install-vulkan-validation.py --destination
# <dir>): VULKAN_SDK selects no runtime layer, and the system layer is older than the headers the engine uses.
VALIDATION=${CRD_VULKAN_VALIDATION:-$HOME/.cerid/vulkan-validation}
if [[ -d "$VALIDATION/share/vulkan/explicit_layer.d" ]]; then
    export VK_LAYER_PATH=$VALIDATION/share/vulkan/explicit_layer.d
    export LD_LIBRARY_PATH=$VALIDATION/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
fi

usage() {
    sed -n '6,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

mirror() {
    python3 "$HERE/source-mirror.py" "$@" --mirror "$MIRROR"
}

tree_of() {
    [[ -n "${1:-}" ]] || usage
    printf '%s/%s\n' "$ROOT" "$1"
}

heavy() {
    # Re-run this script under flock(1): the lock stays with flock itself, and `-o` closes its descriptor in the
    # command, so nothing the lane starts inherits it. The sccache server outlives the build; holding an inherited
    # lock descriptor, it kept the next lane waiting until its idle timeout (2026-10-18).
    if [[ -z "${CRD_LANE_LOCKED:-}" ]]; then
        mkdir -p "$(dirname "$LOCK")"
        CRD_LANE_LOCKED=1 exec flock -o -w "$LIMIT" -E 75 "$LOCK" bash "${BASH_SOURCE[0]}" "$command" "$@"
    fi
}

configure() {
    local preset=$1
    shift
    local tree
    tree=$(tree_of "$preset")
    local cache_args
    cache_args=$(python3 "$MIRROR/scripts/compiler-cache.py" configure-args "$preset" --host linux)
    echo "[wsl-lane] configure $preset from $MIRROR into $tree${cache_args:+ ($cache_args)}"
    # shellcheck disable=SC2086  # the cache entries are separate words
    (cd "$MIRROR" && timeout "$LIMIT" cmake $cache_args --preset "$preset" -B "$tree" "$@")
}

preset_test_settings() {
    # The hosted lane runs `ctest --preset <preset>`; the preset's environment (sanitizer options, the TSan
    # suppressions under ${sourceDir}), job count and timeout apply here as well, in front of the caller's arguments.
    python3 - "$MIRROR/CMakePresets.json" "$1" "$MIRROR" <<'PY'
import json
import shlex
import sys

document, name, source = json.load(open(sys.argv[1], encoding='utf-8')), sys.argv[2], sys.argv[3]
for preset in document.get('testPresets', []):
    if preset['name'] == name:
        for key, value in (preset.get('environment') or {}).items():
            print('export %s=%s' % (key, shlex.quote(str(value).replace('${sourceDir}', source))))
        execution = preset.get('execution') or {}
        if 'jobs' in execution:
            print('PRESET_ARGS+=(-j %d)' % execution['jobs'])
        if 'timeout' in execution:
            print('PRESET_ARGS+=(--timeout %d)' % execution['timeout'])
        if (preset.get('output') or {}).get('outputOnFailure'):
            print('PRESET_ARGS+=(--output-on-failure)')
PY
}

command=${1:-}
[[ -n "$command" ]] || usage
shift
case "$command" in
    configure|build|test|run|dev)
        heavy "$@"
        ;;
esac
case "$command" in
    sync)
        mirror sync
        ;;
    check)
        mirror check
        ;;
    tree)
        tree_of "${1:-}"
        ;;
    configure)
        [[ $# -ge 1 ]] || usage
        mirror sync
        configure "$@"
        ;;
    build)
        [[ $# -ge 1 ]] || usage
        preset=$1
        shift
        tree=$(tree_of "$preset")
        mirror sync
        if [[ ! -f "$tree/CMakeCache.txt" ]]; then
            configure "$preset"
        fi
        # An assignment, not eval "$(...)": a server that cannot start must stop the build (set -e), never leave it
        # on whatever server answers.
        server_env=$(python3 "$MIRROR/scripts/compiler-cache.py" server "$tree" --shell sh)
        eval "$server_env"
        targets=()
        for target in "$@"; do
            targets+=(--target "$target")
        done
        keep=()
        if [[ "${CRD_LANE_KEEP_GOING:-}" == 1 ]]; then
            keep=(-- -k 0)
        fi
        echo "[wsl-lane] build $preset (${*:-all}) with $JOBS jobs${keep:+, keep going}"
        timeout "$LIMIT" cmake --build "$tree" -j "$JOBS" "${targets[@]}" "${keep[@]}"
        ;;
    test)
        [[ $# -ge 1 ]] || usage
        preset=$1
        shift
        tree=$(tree_of "$preset")
        mirror check
        # A parallel CTest otherwise runs N llvmpipe pools of nproc threads each (REN-38).
        export LP_NUM_THREADS=${LP_NUM_THREADS:-4}
        PRESET_ARGS=()
        eval "$(preset_test_settings "$preset")"
        cd "$tree"
        timeout "$LIMIT" ctest "${PRESET_ARGS[@]}" "$@"
        ;;
    run)
        [[ $# -ge 2 ]] || usage
        preset=$1
        shift
        [[ "$1" == "--" ]] && shift
        [[ $# -ge 1 ]] || usage
        tree=$(tree_of "$preset")
        mirror check
        scratch=$(mktemp -d "$HOME/.cerid/lane-scratch.XXXXXX")
        trap 'rm -rf "$scratch"' EXIT
        export S=$MIRROR B=$tree
        cd "$scratch"
        timeout "$LIMIT" "$@"
        ;;
    dev)
        [[ $# -ge 1 ]] || usage
        preset=$1
        shift
        tree=$(tree_of "$preset")
        mirror check
        [[ -f "$tree/CMakeCache.txt" ]] || { echo "[wsl-lane] configure $preset first (wsl-lane.sh configure)" >&2; exit 2; }
        # The check builds and tests in this tree as build and test would: the tree's cache server and the test
        # preset's environment (sanitizer options, suppressions); its own jobs, selection and evidence.
        server_env=$(python3 "$MIRROR/scripts/compiler-cache.py" server "$tree" --shell sh)
        eval "$server_env"
        export LP_NUM_THREADS=${LP_NUM_THREADS:-4}
        PRESET_ARGS=()
        eval "$(preset_test_settings "$preset")"
        scratch=$(mktemp -d "$HOME/.cerid/lane-scratch.XXXXXX")
        trap 'rm -rf "$scratch"' EXIT
        cd "$scratch"
        echo "[wsl-lane] dev.py check in $tree"
        timeout "$LIMIT" python3 "$MIRROR/scripts/dev.py" --root "$MIRROR" check --build "$tree" "$@"
        ;;
    *)
        usage
        ;;
esac
