# 2026-10-27 — SIMD-DISPATCH: the serial f32 permute walk, next-tile prefetch, SSE2 transposes

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `06789737`.

## CI first

- Completed runs: push runs 40573536615 back to 40550831506 were cancelled with no failed job. Complete run
  40515770381 (`31e2b9ce`) finished: win-shipping, win-release, win-shipping-profile and linux-gcc-asan failed, all
  fixed earlier (`b306e2ea` disk, `14840123` the MSVC LTCG C1001 at `gemm_block.hpp:59`).
- New in that run: win-relwithdebinfo exceeded the 6-hour job limit inside Build (14:16:54 to 20:20:29; the
  `windows` job sets no `timeout-minutes`). Its log is unavailable (the API answers BlobNotFound). On complete run
  40307186916 the same Build took 202 minutes, and in this run win-release reached step 1747/2376 in 64 minutes
  before the C1001 linking `crd-hesap-direct-tests`. Candidate cause: the same LTCG defect, which hangs instead of
  crashing on RelWithDebInfo. Not proven from here (the local win-relwithdebinfo tree dates from 2026-09-14); complete
  run 40578927592, dispatched at `06789737`, decides it.
- Needs CI audit: DIAG.11c and DIAG.12 stay Needs CI. 40515770381 predates `7eec1bf7` and `14840123`, so it serves
  neither; 40578927592 is the candidate.

## What changed

- `permute_walk_f32` (new table entry, `PermuteWalkF32`): the serial tiled permute is one kernel call per transform
  (it was one call per plane: 4096 for the 4D 64^4 reversal). The plane odometer (`permute_walk_planes`) and
  `permute_outer_order` moved to `permute_tiles.hpp` (level namespace); general code walks the same template for the
  other element types.
- Prefetch: every tile prefetches the next tile into L2 (`_MM_HINT_T2`, HPTT's macro-tile order): the next tile of
  the region, after the last one the next region's first. `PermuteNext` carries the next origin and its extents, and
  only elements inside them are prefetched, so no pointer leaves the arrays (the 50a713c3 class). The serial walk
  passes the next plane; a multithreaded task passes the worker's next task.
- The vector tiles run blocks of 16 rows of a (one 64-byte source line per column visit), the micro transposes
  stacked along a. The SSE2 level gains a 4x4 `_MM_TRANSPOSE4_PS` body with non-temporal stores (it ran the scalar
  tiles); portable stays scalar and prefetch-free (NEON-clean).
- Bits: copies and one IEEE multiply per element on every level; prefetches, block order and stores change only the
  order of writes to disjoint addresses, so the family stays bit-exact.

## Tests (hosted CI runs the forced arms)

- `[kernels]`: the region test gains a-extents 15, 16, 17, 24, 33 and 40 with tiles that cut the 16-row block, each
  run with and without a next region. A new walk test compares every level with a scalar oracle over ranks 2 to 5
  (a 16^4 reversal, 17x9x11x24, 33x12x10x40 with tile 32, a source stride of 2, a flipped b, tiles 5 and 24, one
  plane), alphas 1, -0.5, 3, -0 and +inf over signed zeros, sentinels past the last element.
- `[permute]`: every order of 16^4, 17x9x11x24 and 33x12x10x40 (each extent past the row path) for f32 unscaled,
  -0.5 and -0, and f64; the worker-count case adds 3 and 7 workers (uneven chunks).
- Local, win-debug: `[kernels]` 13 cases pass; `[permute]` passes detected, forced portable and forced sse2.

## Board candidates (row-close pass)

The 4D 64^4 1T row (1.18x behind HPTT) and 3D 512^3 8T (level) against HPTT at `9425386`: the 1T tile edge (64 or
32), non-temporal stores at 1T for a DRAM-sized dst (never measured on 4D), a dst-aware outer order (HPTT's estimate
weights both strides), the prefetch hint and distance and the dst prefetch, and the 3D 8T staged full-column strip,
whose stage needs a non-heap per-worker buffer (`crd::jobs` offers only `worker_index()`).

## Brief corrections

The brief's half-line re-read is within the same tile, still in L1, so the 16-row block is a small lever; the real
gap was the missing prefetch at 1T. f64 and complex permutes run scalar tiles on every level and decomp permutes
f64: added to the row's Left.
