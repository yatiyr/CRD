# 2026-10-30 — SIMD-DISPATCH: a lost runner, the fft codelet pool and the CI platform suite under CTest

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards). Before:
[the one-thread GEMM board](2026-10-30-simd-dispatch-gemm-board.md).

## CI first

- Push run 41059057789 (e0c70766, in progress, not cancelled): linux-clang-tsan failed on `-Wdouble-promotion` in
  `fma_f32`, fixed at 85cf68f5, after the run's commit. linux-gcc-relwithdebinfo was lost 79 minutes into Build ("The
  hosted runner lost communication with the server"); its log blob is gone. The cancelled runs since have no failed
  job; 41075532731 never started a job.
- The same run's other Linux builds were cold: e0c70766 sits after f70b933d, which edited `libm.hpp`, and release
  missed 989 of 1,981 compiles (50.08 % hits) against 98 % hits for relwithdebinfo in 41034829979.

## Why the runner was lost

Measured on the WSL `linux-gcc-relwithdebinfo` tree at HEAD (GCC 13.3, `-O2 -g`, Ninja 1.11.1), each step alone:

| Step | Peak RSS | Time |
|---|---|---|
| fft batched codelet unit 1, sse2 | 4.14 GiB | 93 s |
| fft batched codelet unit 6, avx512 | 1.66 GiB | 312 s |
| fft batched codelet unit 1, avx2 | 0.66 GiB | 18 s |
| fft hierarchical codelet unit 0, avx512 | 0.49 GiB | 104 s |
| fft batched codelet unit 1, portable | 0.33 GiB | 6 s |
| LTO link of `crd-hesap-fft-tests`, four ltrans jobs | 1.30 GiB | 7 s |
| LTO link of `crd-gpu-context-vulkan-tests` (the slowest hosted link) | 0.45 GiB | 26 s |
| LTO link of `crd-ceir-tests` | 0.29 GiB | 14 s |

The links are small: CMake spells IPO `-flto=auto`, which without a jobserver forks one ltrans job per online CPU
(the affinity mask is ignored, so the links were timed at `-flto=4`, the runner's count). The cost is the sse2 codelet
units: their vector fma is the exact SSE2 emulation of `fma_exact.hpp`, inlined at every site. The seven batched sse2
units sit in one target and ran side by side for 140 to 170 s each in the local 16-job build; at the runner's six
Ninja jobs that is about 25 GiB on a 15.6 GiB machine. The fft pool of d8475439 held only avx2 and avx512, sized from
MSVC's figures (2.2 and 4.2 GB). No other kernel unit took more than 5 s.

## Fix

`engine/numerics/hesap-fft/CMakeLists.txt`: every fft level (portable, sse2, avx2, avx512) compiles in the Ninja pool
`crd_fft_codelets`, `(total MiB - 4096) / 4300.8` deep and at least one: the build board's 4 GiB reserve, then one unit
per measured 4.2 GiB peak. Generated depth: 14 on the 64 GiB Windows host, 6 on the 31 GiB WSL host, two on a 16 GB
runner. Compile commands are unchanged (the WSL tree reconfigured with no work to do), so cache keys hold, and a
cached job never exercises the pool: the hosted check is the first Linux job whose fft codelets miss the cache (an
edit to `libm.hpp`, `codelet_lanes.hpp`, `fma_exact.hpp` or a codelet source) completing its Build.
`docs/design/build-performance.md` records the exception to "the runners run unbounded".

## CI platform suite under CTest

`tests/CMakeLists.txt` registers `scripts/test-ci-platform.py` as `crd-ci-platform` (timeout 300 s, `PYTHONUTF8=1`,
beside `crd-repository-tools`); the repository job already runs the script. 79 tests, 0.8 s locally. No defect found
in `ci/`.

## Audit

Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none dispatched (push runs pending).
Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only); no secrets,
so the AMD and Mac lanes wait on their accounts.
