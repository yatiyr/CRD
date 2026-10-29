# 2026-10-29 — SIMD-DISPATCH: every f32 forward the planner offers, as a board arm

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 1, fft's f32 forward: the board judges every size,
not only 1M). Before: [win-asan's CTest bound](2026-10-29-simd-dispatch-win-asan-bound.md).

## CI first

- Push run 40869334591 (e496d16c) completed: win-asan ran all 8,106 cases serially in 5 h 34 min and failed only
  `crd-check-isa-isolation` (fixed at 5cf0e54e), win-shipping only the TickClock case (fixed at d3b0daf1), and
  linux-gcc-arm64-asan reached the 6 h cap (it started before a1901325). Nothing new to fix.
- Push run 40929108987 (1ebe9dc4) is in progress, the first to run linux-gcc-arm64-asan after a1901325 and win-asan
  under the three-job bound; every other push run since was replaced while queued. Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 wait on a green complete-tier run; none has completed since.

## What landed

- **`FftAlgorithm`** (`fft_kernels.hpp`): each direction's algorithm is a field of `FftPlanState` (`forward`,
  `inverse`), and `Engine::execute` switches on it. Before, the kernels inferred the route from which tables the
  plan had filled (`ip_n`, `ds_b`, `tf_nt`, `use_four_step`, `sh_t`, in priority order), so a forced forward could not
  coexist with the inverse's tables. The deep-split and standalone-hier bodies moved into their own members.
- **The planner** (`fft.hpp` `detail`): `planned_forward`, `planned_inverse`, `four_step_planned`, `four_step_split`
  and `ip4_planned` hold every per-size choice that was spread over the constructor, with their measurements;
  `forward_builds` says where each algorithm's codelets reach (Stockham any size; standalone-hier and IP4-AoS 1K to
  64K; the three-pass deep-split 8K to 512K, the four-pass 8K to 8M; the four-step from 1K; the six-step on squares
  from 4K, its blocks being 64 rows; the tiled four-step f32 1M). The constructor builds the inverse's state and the
  forward's, nothing else: f64 1K to 64K no longer builds standalone-hier tables and Stockham buffers IP4-AoS
  shadowed, f32 16K to 64K no deep-split tables, and the 2M, 4M and 8M four-pass factors, unreachable under the old
  band, are arms. `FftPlan(alloc, n, forward)` replaces `detail::LargeForward` and its 1M-only assert; only the
  six-step changes the split its inverse shares, to the square one. The destructor releases each table through one
  null-checked `release`.
- **Tests** (`test_fft_kernels.cpp`): one case pins both directions' planned algorithm at every size to 8M for both
  types (one letter a size) and that every planned forward builds; three cases (1K to 64K, 128K to 1M, 2M and 4M) run
  each of the 51 other f32 forwards on every level the host runs, against the radix-2 reference and portable's bits
  (the random input, aligned), with the planner's inverse in the state. They replace the 1M-only case here and in
  `test_fft.cpp`, which ran two of them.
- **Fatal modes**: `plan-stockham-table-refused` is `plan-ip4-table-refused` (f64 64K's first raw table is
  IP4-AoS's; Stockham's buffers are containers), and `plan-four-step-table-refused` builds f64 2M, whose first raw
  table is the four-step's (at 512K it was the deep-split's).
- **`bench_fft_f32`**: every size's arms come from `forward_builds`, the planned one first, each with its winner
  line. CI does not build it (MKL and FFTW are WSL-only); a clang-cl syntax pass with stub peer headers was clean.

## Checks

win-debug build of crd-hesap-fft, its tests and the fatal specimen; the four new cases once (64 s: 0.04, 1.2, 10.8
and 51.6 s); the ctest guards and the eight fatal modes; strict tidy on the changed units; format against HEAD's
counts; Allman braces, the plan and repository checks. Watch on the first hosted run: the 2M and 4M case's seconds
under win-asan and the SDE avx512 arm.

## Left

The board (row close), then each losing size's lever as its own `FftAlgorithm` from the research's remaining list
(docs/research/fft-stockham-v2.md): a fully fused 1K and 2K kernel, S1-transpose elimination at 4K to 64K, a 2M
four-step retune against the four-pass factors.
