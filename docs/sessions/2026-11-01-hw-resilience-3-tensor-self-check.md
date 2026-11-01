# 2026-11-01 — HW-RESILIENCE.3: known-answer self-check of hesap-tensor

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the fft self-check](2026-11-01-hw-resilience-3-fft-self-check.md).

## CI read

- Completed runs: push runs up to 41289725189 were cancelled by concurrency and have no failed jobs.
- Running complete-tier run 41274380605 (`de5e10b0`): win-asan shard 2 failed `filesystem: the last write time
  converts exactly ...` (track B's aa91b382) at `REQUIRE(stamp(older, at))`. It failed on every Windows build, not
  only ASan: `clock_cast<file_clock>` of a nanosecond `sys_time` shifts the count by the Microsoft file clock's 1601
  epoch in nanoseconds, overflows i64 and hands SetFileTime a negative time (error 87). A probe reproduced it with
  and without ASan. Fixed in cc5ee9e9 by narrowing to the file clock's tick before the cast; libstdc++'s
  nanosecond file clock is unchanged. win-asan: the case failed before, and all 64 platform cases pass after.
  The run's other failures (win-tidy, the GCC release and shipping links, clang-cl) were already fixed on main.
- Needs CI audit: both running complete-tier runs end red on causes now fixed, so HW-RESILIENCE.2, DIAG.11c and DIAG.12
  stay Needs CI. A complete-tier run on current main is dispatched once neither is in progress.

## Self-check

`hesap.tensor` (`tensor_self_check`, `src/tensor_self_check.cpp`) runs all 78 routines of a table:

- The permute regions (a copy whose tiles cut both extents, a real scale through a staged strip with a next region
  and HPTT's levers, a complex scale, non-temporal stores, the scalar tile) and walks (a rank-3 transpose, real and
  complex, non-temporal stores, the scalar tile), f32 and f64.
- The binned sum past every body's gate, merged into a non-empty accumulator; Q8_0 over four blocks and a guard
  block; a 96-value u8 row; Philox over a 2^32 position crossing and from 0.
- The six elementwise ops over stepping, reversed, broadcast and rank-3 walks; Tier D's block, rows (one of two
  blocks) and columns for each op; einsum's thin and small-m products (both layouts).
- The batched GEMM (beta 0 and 1/2), Cholesky and LU in the lane tier (19 matrices over two ranges) and the scalar
  tier (n 33), both solves, and the SVD at 30 and 2 sweeps and at n 17.
- The sparse TTM at every mode (scatter, and staged for the inner modes) over all nonzeros and a sub-range, the fold,
  and both MTTKRPs over a rank-4 tree built at compile time.
- nn: softmax, layernorm, the four activations (one in place), the Q8_0 and int8 linears (both int8 layouts), max and
  average pools, the direct convolution with and without the fused ReLU and pool, and an im2col shape.

The inputs are k / 7 for nonzero k, so sums and products round and another order changes bits. Each routine's answer
is one FNV-1a digest of the portable table's outputs, as for fft; the differential cases already hold every table to
the scalar oracles. The int8 row-major and packed linears give one digest, as their contract says. Every output buffer
starts from a guard and is digested whole, and NaNs fold as one. Each group has its own frame under 12 KiB.

## Tests and evidence

- `tensor self-check:` cases: every table the host runs passes. Eight wrong routines fail under their own names: a
  permute walk, ew div, Tier D columns writing past, the batched LU, the COO MTTKRP, the packed int8 linear (VNNI
  dots), Philox and the convolution. The process-level case requires `Passed`.
- win-debug: the whole test program passes, and so do the forced portable, sse2, sse4.2, avx2 and avx512 arms. The
  four tables here gave identical digests. SDE 10.13.1 `-spr` runs all six tables (avx2 and avx512 VNNI included),
  passes, and selects avx512+avx512-vnni; `-icx` passes. linux-gcc-debug: the cases and forced arms pass.
- linux-gcc-debug found a bug in the first draft: the Q8_0 guard block set only its scale, so its uninitialised
  bytes went into the digest. MSVC's debug stack fill had hidden this. The whole block is now filled first.
- Teeth: changing one bit of the f64 MTTKRP answer failed every table (`f64 sparse mttkrp_csf`) and the selection
  case's `Passed`. Restored, rebuilt and passing.
- Guards: 21 of 21. The first run failed `crd-check-capability-owners` with no output, then passed when run directly
  and on the rerun. This is the host's known fault in Python validators. Strict tidy and clang-format are clean on
  the changed lines.

## Left on the row

The device path, `ceridc doctor --hardware` and the state-hash divergence check.
