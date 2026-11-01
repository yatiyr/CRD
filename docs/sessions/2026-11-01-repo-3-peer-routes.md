# 2026-11-01 — REPO.3: routes for the Shewchuk reference, the MKL batched bench and the liquid-dsp peers

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [REPO.3](../ROADMAP.md#slice-repo.3), the `unrouted` sources the
[text encoding note](2026-11-01-repo-3-text-encoding-guard.md#remaining-on-repo3) left (27). Five get a checked route,
one is retired; 22 remain. Built and run on `cerid-track-b` (spot c4-standard-8, Intel Xeon with AVX-512, Ubuntu
24.04, GCC 13.3) in the `linux-gcc-debug` tree and a `linux-gcc-release` tree with the peer options on.

## CI first

No GitHub Actions run has finished for any commit since 03571fa3, so no failure can be traced to this track. Run
41269105413 (b0b1b245) and the push run 41262267004 (8a243a34) are still running and cover none of this track's
commits since then; the `workflow_dispatch` run 41274380605 at de5e10b0 is still queued; the push group's pending run
is now 41281400616 at c6c2729a (the previous batch's last commit). Every other run of the day was cancelled by a newer
push. The previous batch's only new Windows-facing code is the text encoding guard, which the Windows
repository-checks job runs: on this Windows PC `check-repository.py` passes, and of `test-repository-tools.py`'s 276
tests 272 pass, 3 skip and one fails (`test_generated_target_folders`, which configures a scratch tree and needs Ninja
on the PATH, which this PC's agent shell lacks; this PC must not configure trees anyway).

## Shewchuk's predicates: the oracle of a differential test

`bench/reference/shewchuk-predicates.c`, Shewchuk's public-domain `predicates.c` of 1996, sat unbuilt beside the
engine's port of it (`crd/geometry/primitives/predicates.hpp`, ADR-0076 §18). It moved, verbatim, to
`tests/geometry/geometry-primitives/reference/` (tests live with their module, `docs/design/repository-layout.md`) and
is now the oracle of `test_predicates_vs_shewchuk.cpp` in `crd-geometry-primitives-tests`, on every tree that builds
the tests, hosted lanes included.

- **The oracle build.** The `crd-geometry-shewchuk-oracle` object library compiles the file as C17 with GNU
  extensions (its definitions are K&R ones, which C23 removes, so a newer compiler default cannot break it), without
  LTO, under `crd-simd-flags`: the tree's floating-point contract (no contraction, no fast-math, SSE2 arithmetic on
  x64), which the expansions need, since a contracted product would lose the roundoff they carry. The test directory
  enables C. The Windows SDK has no `<sys/time.h>` (included, never used) and no `random()` (called only by the
  file's unused random-input generators): under MSVC and clang-cl, `reference/win32/sys/time.h` stands in for the
  header and `random` is defined as `rand`. The vendored file keeps its Allman-check exemption at the new path.
- **The comparison.** Each input goes to the engine's predicate and to Shewchuk's adaptive and exact ones; the three
  signs must agree, so an exact zero in one is an exact zero in all, and a disagreement is reported with the input in
  hexadecimal floating point. Per predicate, from a seeded SplitMix64 stream (`crd/core/seed_generators.hpp`): 2,000
  uniform inputs; 72,000 rounded ones (the last point computed on the degenerate locus of the others in floating
  point, so it misses by rounding only, moved by up to two units in the last place per coordinate, at scales 2^-40,
  1 and 2^40 and offsets up to 2^20 times the scale); 2,000 integer inputs exactly on the locus (a line, a plane, the
  108 lattice points of x^2 + y^2 = 1105^2 and those of x^2 + y^2 + z^2 = 45^2, translated by up to 2^20), each of
  which must be exactly zero; and the same 2,000 with one coordinate moved by one. Every predicate's uniform and
  rounded families, and incircle's moved lattice family, must show both signs. 312,000 inputs in all; in the debug tree the four cases take 2.9 s (insphere
  1.95 s, incircle 0.70 s, orient3d 0.18 s, orient2d 0.04 s).
- **Result.** Every sign agrees. Measured with a probe that was not kept: the engine's orient2d and orient3d return
  Shewchuk's adaptive value bit for bit on every input, and incircle and insphere on every uniform and lattice one;
  on the rounded families the magnitudes of incircle (47,543 of 72,000 identical) and insphere (39,402) differ where
  the two implementations' later stages compute different approximations, which the predicates' contract allows
  (the sign is exact, the magnitude approximate). So the test asserts signs only.
- **Teeth.** The first run found a defect of the test itself: the lattice enumeration skipped both points of each
  sign pair whose last coordinate is zero, so the circle had 106 points, not the 108 that 4 (2 + 1)^3 requires; the
  `REQUIRE` on the count caught it.

## bench_gemm_v0d.cpp retired

`scripts/bench_gemm_v0d.cpp`, the standalone f64 GEMM program of the 2026-07-03 v0d crush pass, is removed. Its board,
[docs/bench/2026-07-03-v0d-gemm-zeroinit-pass.md](../bench/2026-07-03-v0d-gemm-zeroinit-pass.md), is the record, and
`bench_hesap_gemm_vs_reference` (`scripts/run_bench_gemm.sh`, `CRD_BUILD_HESAP_VS_REFERENCE`) measures the same
one-thread f64 GEMM from 256 to 4096 against Eigen and OpenBLAS today.

## bench_batched and its MKL rows

`runtime/examples/bench_batched.cpp` (the 2026-07-05 batched linear-algebra board) joins the retained benches, so
`linux-gcc-debug` and every preset inheriting it compile and link its Cerid rows. Its MKL rows (MKL's
`cblas_dgemm_batch_strided` and the LAPACKE potrf, getrf and gesdd loops) had been built only by an untracked
`build/crd_batched_bench.sh`; the new option `CRD_BUILD_HESAP_VS_MKL` (requires `CRD_BUILD_RETAINED_BENCHES`) defines
`CRD_BENCH_MKL` and links MKL, and `scripts/run_bench_batched.sh` builds and runs it per CPU level. The MKL lookup is
shared with the fft boards' option now (one `find_path(mkl.h)` and `find_library(mkl_rt)` for either). On the track
machine, with `libmkl-dev` 2020.4.304-4 (`scripts/bench-host-setup.sh`'s package), the bench builds with the tree's
warnings as errors and prints all 40 rows, MKL beside Cerid (for example at n=4, b=10,000: GEMM 0.18 ms against
0.71 ms, small SVD 3.52 ms against 53.6 ms; at n=16, b=100,000 the SVD ratio is 1.72x). These are one-shot numbers
from a shared spot machine, a build check, not a board.

## The liquid-dsp peers

The three liquid-dsp peers with routes (comms, adaptive filter, multirate) were compiled by their scripts with an
inline `gcc ... -lliquid`, and a failed build printed a note and the script went on: no route checked them. The two
without one (sosfilt and windows) had no script at all. The new option `CRD_BUILD_HESAP_VS_LIQUID` finds liquid-dsp
(`liquid/liquid.h`, `libliquid`) and builds all five as C programs with the tree's warnings at the host's ISA
(`crd_peer_isa_flags`, the boards' `-march=native`), `CRD_ISA_ROLE peer`, so the ISA isolation guard leaves them out.
`run_bench_comms.sh`, `run_bench_dsp_adaptive.sh`, `run_bench_dsp_multirate.sh` and `run_bench_dsp_windows.sh` and the
new `run_bench_dsp_sosfilt.sh` run the Cerid bench per CPU level, then the peer once (`CRD_BENCH_LEVELS=detected`
through `run-lane-bench.sh`), and fail when a build fails. `scripts/bench-host-setup.sh` installs `libliquid-dev`
and records its version.

- The tree's warnings found 14 sites in the five programs, all mechanical: `timespec` fields and loop counters
  converted to floating point without a cast, a `rand()` result stored as unsigned, and f32 samples accumulated into
  f64 checksums (`-Wdouble-promotion`); and three deprecation reports, below. Each is now an explicit conversion; the one-line timing helpers that grew past
  120 columns are written out.
- liquid-dsp 1.6.0's `DEPRECATED(MSG, X)` expands to `X` followed by the attribute, after `X`'s semicolon, so each
  deprecation lands on the next declaration in `liquid.h`: `eqlms_*_get_weights`' on `eqlms_*_push` and
  `eqlms_rrrf_train`'s on the `eqlms_cccf` type. The comms and adaptive peers call neither deprecated function;
  `-Wno-deprecated-declarations` on those two sources only, with the reason in `runtime/CMakeLists.txt`.
- On the track machine (liquid-dsp 1.6.0-3 from Ubuntu's archive) all five build and run: iirfilt 231 ms per 10M
  samples, eqlms 100 ms per 1M, rresamp 30.9 and 16.6 ms, the comms set, and the window programs (liquid's per-sample
  Kaiser window and `firdes_kaiser` take about 6 s per 2^20 samples, the pathology the board's header describes).

## Evidence

- `linux-gcc-debug`: `crd-geometry-primitives-tests` and `bench_batched` build with the tree's warnings as errors;
  the `[shewchuk]` cases pass (4 cases, 312,000 inputs). CTest on that commit: the `crd-geometry-primitives` tests
  and their kernel-level arms, the guards (`^crd-check|^crd-no-|^crd-diag-coverage$|^crd-diag-seeded-faults$`),
  `crd-repository-hygiene`, `crd-repository-tools` and `crd-master-plan`, 55/55 pass.
- `crd-check-source-routes` on both trees: `linux-gcc-debug` 1,976 sources, 1,805 compiled, 132 gated off, 12 built
  outside, 5 included, 22 unrouted, PASS; the peers tree (`linux-gcc-release` with `CRD_BUILD_RETAINED_BENCHES`,
  `CRD_BUILD_HESAP_VS_LIQUID` and `CRD_BUILD_HESAP_VS_MKL`) 1,810 compiled, 127 gated off, PASS.
- Local validators: `check-repository.py`, `check-allman-braces.py`, `check-generated.py`, `check-master-plan.py`,
  `check-docs.py`; `test-repository-tools.py` `AllmanBraces` and `SourceRoutes`.

## Remaining on REPO.3

- Routes or retirements for the 22 `unrouted` sources left: the 21 CUDA and VkFFT programs under `bench/gpu-compute`
  and `bench/gpu-fft` (their generated kernels are included by the profile program) and
  `scripts/v14m_ggml_q8_bench.c` (ggml Q8_0, pinned and built by its own CMake like NLopt).
- The LunarG Qt line, from a cold Windows lane.
- Then REPO.3d (scoped configuration and consumer checks, then full remote CI evidence on the published revision once
  DIAG is Done) and REPO.3's close.
