# 2026-10-30 — SIMD-DISPATCH: the symmetric eigensolvers' crd-jobs levers as forced arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the dense factors' levers](2026-10-30-simd-dispatch-jobs-factor-picks.md).

## CI first

- No completed run has a failed job. Push runs 40933364118 to 40963529193 were cancelled before any job ran (the jobs
  API lists none). Push run 40929108987 (1ebe9dc4) still runs win-asan and linux-gcc-asan; every finished job passed.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has completed. Nothing
  cancelled or dispatched.

## What landed

The retune's third consumer, `eigvals_sym`'s multisection and `mrrr_compute_vectors`' segment chunking.
`crd/hesap/dense/detail/eig_arms.hpp` holds the picks; `eigvals_sym` is `eigvals_sym_arm(EigvalsPick{})` and
`mrrr_compute_vectors` takes `MrrrPick{}` by default, so production and the arms share one body.

- **Multisection:** `MultisectionPick {chunks, workers}` (the chunk count was the pool size) and
  `EigvalsPick::multisection_min` (512, dqds below it). `tridiag_multisection_arm` replaces the file-local driver,
  and `bench_hesap_eig_vs_reference` calls it in place of its own copy.
- **MRRR:** `MrrrPick {workers, gran_div 256, gran_max 8, serial_max 0}`; the scratch stays sized by the pool.

**A determinism defect, fixed.** Each multisection chunk bisected a root found from its own separators, and the
partition followed `num_workers()`, so `eigvals_sym`'s bits at n >= 512 followed the host's core count. ADR-0065
said the opposite. Observed before the fix: the arm test failed 540 of 762 assertions; the six spectra with a finite
bracket all differed between 1 chunk and 3 to 64 chunks. Every chunk now descends the one tree rooted at the
Gershgorin bracket and keeps only the nodes that meet its indices (ADR-0065 D(dense-eig)-MRRR-multisection-global-tree,
the false clause struck in place). One chunk was always the whole tree, so the one-job bits are unchanged. A
non-finite bracket now returns quiet NaNs; a NaN at `d[0]` used to repush one node forever (read from the code).

- **Tests** (`test_mrrr_eigenvalues.cpp`): `tridiag_multisection_arm` at n 64, 100 and 257, f32 and f64, over eight
  spectra (random, glued W21+, an n-fold eigenvalue, graded, -0 and a subnormal in e, NaN at d[0], NaN mid-d,
  infinity in e) and 15 chunk and job picks (1 to 64 chunks, 1 to 32 jobs), bit for bit against one chunk on one job.
  `eigvals_sym_arm`: dqds against the multisection at n 200 and 512 within a declared 4 n eps |A|_inf (c stated, not
  derived: sturm_count.hpp documents no Sturm backward-error constant), and production equal to its arm.
  `mrrr_vectors_arm`: n 2, 3, 100 and 300, random and glued W21+ (the cluster arena), 1 to 32 jobs times both
  granularities and serial cuts, bit for bit with ldz = n + 3 and guard words in the padding and after the buffer.
  MRRR's chunking was already bit-exact (it passed before the fix).
- **Board:** `bench_hesap_eig_parallel`, one process per `--threads N`, times the one-job eigenvalues and vectors and
  each arm round by round at n 128 to 8192 (vectors to 4096), f32 and f64: chunks 1, 2, 4 and 8 per job and dqds,
  MRRR's production granularity and its gran_max and gran_div arms. Bit gates, dqds by the bound; a miss exits 1.
  `scripts/run_bench_eig_parallel.sh` runs it at 4, 8, 16 and 32 workers at each level.
- **Tidy:** the pre-existing findings of the two touched headers are fixed (local constants lower case,
  `MrrrPArgs`' members, `mrrr_one_vector`'s const casts, a widening cast).

## Checks

win-debug builds of crd-hesap-dense-tests and the bench; every `[eig]` case passes (107 cases; the three new ones take
1.1 s, 9.5 s and 12.8 s); strict tidy (LLVM 20) clean on the six changed C++ files; clang-format on the added lines;
Allman braces; the ctest guards (the include budget gains `eig_arms.hpp`); `bash -n` on the driver.
`bench_hesap_eig_vs_reference` is not compiled here (its peers' tree is not configured); its call matches the
compiled bench's. No bench ran: the board runs at row close.

## Left

The retune's other consumers, one batch each: hesap-direct (supernodal, multifrontal, dense LU kernels), hesap-amg's
serial Galerkin product, hesap-iterative's operator threshold, hesap-autodiff's batch gradient and hesap-sched (a
measured consumer or the unused link removed).
