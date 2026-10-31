#!/usr/bin/env bash
# The benchmark machine's setup (ci/bench_vm.py, the optimization track SIMD-PERF): idempotent, run from its checkout
# after each `sync` that changes this script, and safe to rerun (the disk persists across stops, so most runs only
# print the census).
#   bash scripts/bench-host-setup.sh
# - perf for the running kernel (linux-tools-$(uname -r)) and kernel.perf_event_paranoid=1 kept in
#   /etc/sysctl.d, so perf stat, top-down metrics and perf record work for the user after every boot;
# - the pinned Ninja on /usr/local/bin and the pinned Vulkan headers as the lanes' VULKAN_SDK (~/cerid-deps/vulkan-sdk,
#   the wsl-lane.sh default), as the hosted Linux lanes install them;
# - the peers the boards find on the system (the apt packages below: FFTW 3 and Intel MKL for the fft boards); the
#   pinned peers (pocketfft, Eigen, OpenBLAS, Boost, HPTT, Highway) come from cmake/pins.json at configure time;
# - then the census a board records: the CPU and its vector features, the kernel, the tools and each peer's package
#   version (also written to ~/bench-host.txt).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
PACKAGES=("linux-tools-$(uname -r)" linux-tools-common libfftw3-dev libmkl-dev)
missing=()
for package in "${PACKAGES[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed'; then
        missing+=("$package")
    fi
done
if [[ ${#missing[@]} -gt 0 ]]; then
    sudo apt-get update -q
    # libmkl-dev asks whether MKL becomes the system BLAS; the boards link it by name, so the answer is the default.
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q "${missing[@]}"
fi
PARANOID=/etc/sysctl.d/90-cerid-perf.conf
if [[ "$(cat "$PARANOID" 2>/dev/null)" != kernel.perf_event_paranoid=1 ]]; then
    echo kernel.perf_event_paranoid=1 | sudo tee "$PARANOID" > /dev/null
fi
sudo sysctl -q -p "$PARANOID"
mkdir -p "$HOME/.cerid/ninja" "$HOME/.cerid/downloads"
ninja_dir=$(python3 scripts/install-ninja.py --destination "$HOME/.cerid/ninja" | tail -n 1)
sudo ln -sf "$ninja_dir/ninja" /usr/local/bin/ninja
if [[ ! -d "$HOME/cerid-deps/vulkan-sdk" ]]; then
    python3 scripts/install-vulkan-headers.py --destination "$HOME/cerid-deps/vulkan-sdk" \
        --download-dir "$HOME/.cerid/downloads/vulkan-headers"
fi
{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "logical cpus: $(nproc); memory: $(free -g | awk '/^Mem:/ {print $2}') GiB"
    features='avx2|fma|avx512f|avx512bw|avx512vl|avx512_vnni|avx512_bf16|avx512_fp16|amx_tile'
    echo "flags: $(grep -m1 '^flags' /proc/cpuinfo | grep -o -w -E "$features" | tr '\n' ' ')"
    echo "kernel: $(uname -r); perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
    echo "perf: $(perf --version)"
    echo "gcc: $(gcc --version | head -n 1); cmake: $(cmake --version | head -n 1); ninja: $(ninja --version)"
    dpkg-query -W -f='package: ${Package} ${Version}\n' libfftw3-dev libmkl-dev
} | tee "$HOME/bench-host.txt"
