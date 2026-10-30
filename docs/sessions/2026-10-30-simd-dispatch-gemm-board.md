# 2026-10-30 — SIMD-DISPATCH: the one-thread GEMM board, portable's 4 by 4 tile and a kept test pool

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards, after fft). Before:
[the ASan stack alignment](2026-10-30-simd-dispatch-asan-stack-alignment.md). Board:
[2026-10-30 GEMM board](../bench/2026-10-30-simd-dispatch-gemm-board.md).

## CI first

- Scheduled run 41034829979 (530d0f4e) completed with five failed jobs: win-release, win-relwithdebinfo, win-shipping
  and win-shipping-profile (the fft `SEGFAULT` stack frames, fixed at fa525bfe) and linux-gcc-asan (the AVX-512 probe,
  ff01ac85; batch_gradient's empty `memcmp`, 74717b85; mikktspace's registered TP-5). Nothing new.
- Push run 41059057789 (in progress, not cancelled): linux-clang-tsan failed on `-Wdouble-promotion` in `fma_f32`,
  fixed at 85cf68f5. The cancelled runs since have no failed job. Push run 41075532731 (1b0039bd) is pending.
- Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none dispatched (a push run is in
  progress). Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only);
  no AMD or Mac secrets.
- The win-asan follow-up of 2026-10-29 is closed: `docs/design/ci-tiers.md` names the bounded test jobs.

## Downloads

Under the user's grant of 2026-10-30 (~17:00, testing tools and board peers), the two pinned board peers of
`cmake/pins.json`, each hash checked before anything read it, into `%USERPROFILE%/.cerid/downloads/gemm-peers-20261030`
and then the archive cache `%USERPROFILE%/.cerid/archives`:

| Archive | Bytes | SHA-256 | Licence |
|---|---|---|---|
| eigen-3.4.0.tar.gz | 2,705,005 | 8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72 | MPL-2.0 |
| OpenBLAS-ce3f668c.tar.gz | 24,532,468 | fab73d732f1063b93d1aa96f024d7e8258c22a7eff86ca2a5a771614d54c6224 | BSD-3-Clause |

## The board and its decisions

- avx2 accepted against pre-row `2f2aed72`: f32 1.12x to 1.32x faster, f64 0.97x to 1.09x with every range overlapping.
  The 2026-07 OpenBLAS lead is gone (f32 0.94x to 1.01x, f64 0.94x to 1.08x); recorded, not chased.
- Portable's GEMM tile is the 4 by 4 (f64 1.18x to 1.28x, f32 1.02x to 1.03x the 2 by 4's, same bits). The arm table
  (`DenseGemmArms`, `kDenseGemmArmsPortable`) and the bench's 4 by 4 rows are gone.
- `kSerialResidentThreshold` is 512M (between the 768 and 1000 cubes; margins inside the noise).
- Change of plan against [the arms note](2026-10-29-simd-dispatch-gemm-board-arms.md): `GemmPath` and
  `detail::gemm_arm` stay. They are the only way to run the serial packed path on a row-major op(A) at test sizes, so
  the paths' bit test (`check_paths`) and the empty-product test need them, and QUAL.1-HW's hosts set avx512's and
  ARM64's thresholds with the same arms.
- The pre-row tree `~/prerow-tree-build/linux-gcc-release` (WSL) stays for the next families' boards.

## A bug found on the way: a failed case shut a fixture's pool down

Changing the paths' run count made the f64 `check_paths` case fail its count, and the f32 case after it stopped on
`crd::jobs::frame_get_mark: call init() first` (a debug assert that then hung the run). The f64 case was the first to
call `hesap_jobs_listener()`, so the jobs pool listener saw a pool that was down when the case began and up after its
failure, and shut it down; the fixture's function-static stayed constructed and never brought it up again. Every later
case of the run met a pool that was down.

- `tests/support/catch/jobs_pool_listener.hpp` declares `crd::test::keep_jobs_pool()`; the listener leaves a kept pool
  up. The hesap-dense and hesap-sparse fixtures call it after their `init`. `crd_discover_tests` adds the header's
  directory with the listener.
- `crd-jobs-tests` gains "a pool a fixture keeps stays up after a failed case", which re-enters the executable on two
  hidden cases. With the keep check removed it failed (`test cases: 2 | 2 failed`); restored and rebuilt, it passes.

## Checks

- win-debug: crd-hesap-dense-tests whole (382 cases), its `[kernels]` cases at detected, portable and sse2;
  crd-jobs-tests' listener cases; crd-hesap-sparse-tests built. The bench built in the WSL release tree and its
  portable run printed every arm `same`.
- Strict tidy clean on the ten changed C++ files; clang-format findings equal HEAD's on every changed file; the ctest
  guards (19); Allman, plan and repository checks.

## Left

The row-close boards of SELL, permute, the sse2 bodies and the jobs retune's thread sweep, from the same pre-row tree.
