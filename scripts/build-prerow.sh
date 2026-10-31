#!/usr/bin/env bash
# Build programs of the pre-row engine (2f2aed72, the parent of SIMD-DISPATCH's first engine commit) on the benchmark
# machine: its tree is exported once from this checkout's history (git archive) and configured once with that commit's
# own linux-gcc-release preset and retained benches, then each call builds the named targets incrementally. Every board
# that judges a change against the pre-row engine runs these programs beside HEAD's (scripts/ab-board.py), on the same
# machine only (docs/bench/README.md#fast-ab-boards).
#   bash scripts/build-prerow.sh <target> [target ...]      prints the build tree on its last line
# Environment: CRD_PREROW_COMMIT (default 2f2aed72), CRD_PREROW_ROOT (default ~/prerow), CRD_LANE_JOBS (default: the
# CPUs), VULKAN_SDK (default ~/cerid-deps/vulkan-sdk, as scripts/bench-host-setup.sh installs it).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 1 ]]; then
    sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
commit=${CRD_PREROW_COMMIT:-2f2aed72}
root=${CRD_PREROW_ROOT:-$HOME/prerow}
source_dir=$root/src-$commit
tree=$root/build-$commit/linux-gcc-release
export VULKAN_SDK=${VULKAN_SDK:-$HOME/cerid-deps/vulkan-sdk}
export CRD_PLATFORM_HEADLESS=1
if [[ ! -f $source_dir/CMakeLists.txt ]]; then
    mkdir -p "$source_dir"
    git archive "$commit" | tar -x -C "$source_dir"
fi
if [[ ! -f $tree/CMakeCache.txt ]]; then
    (cd "$source_dir" && cmake --preset linux-gcc-release -B "$tree" -DCRD_BUILD_RETAINED_BENCHES=ON)
fi
cmake --build "$tree" -j "${CRD_LANE_JOBS:-$(nproc)}" --target "$@"
echo "$tree"
