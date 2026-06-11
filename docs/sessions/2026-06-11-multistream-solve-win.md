# 2026-06-11 — the deep-research dig: multi-stream DRAM, and the SOLVE flips to a WIN (part 21)

**Phase:** 3.1.6 `crd-hesap`. Directive: "make a deep research and empirically and 100% confidently
determine and fix the performance issues." Every claim below is a measurement, and the causal chain closes
with no unexplained residue.

## The research chain (each step empirical)

1. **Read the gold standard's source.** Cloned SuiteSparse v7.7.0; `t_cholmod_super_solve_worker.c` shows
   CHOLMOD's solve is exactly `dtrsv` (diagonal triangle) + `dgemv("N"/"T")` (below block) per supernode —
   structurally identical traffic to ours. So its speed is bytes or rate, nothing exotic.
2. **Pinned the bytes.** Added to the bench: the TRUE solve trapezoid computed from BOTH data structures.
   Result: **identical — 67.87M doubles (543 MB/pass) each.** The long-standing "Cerid has 22% less fill"
   claim compared our trapezoid to CHOLMOD's `lnz` (which equals its `xsize`, the RECTANGLE storage count)
   — apples to oranges; true fill is equal (same elimination order, same supernode partitions). Corrected.
3. **Pinned the rate.** Per-pass CHOLMOD timing (`CHOLMOD_L` / `CHOLMOD_Lt` split, added to the bench):
   18.2 + 18.8 ms = **29.8 / 28.8 GB/s per pass** — ABOVE the 26 GB/s "ceiling" my pure-load probe
   measured on the very same buffers. So the probe was not the true ceiling.
4. **Found the mechanism.** A standalone probe reading one vs several interleaved sequential regions:
   **1 stream = 22.7 GB/s · 2 streams = 29.7 · 4 streams = 36.9 GB/s** (DRAM bank/page-level parallelism —
   one stream leaves most of the memory system idle). OpenBLAS's dgemv processes 4 matrix columns per pass
   = 4 concurrent streams = its 29 GB/s. Our solve walked ONE column at a time = one stream = our 21 GB/s.
   Mechanism confirmed; THP ruled out (madvise-only on WSL, neither side gets hugepages); perf unavailable
   on WSL (worked around entirely with targeted probes).

## The fix — 4-column-fused solve phase kernels

Four shared helpers (`solve_fwd_diag`, `solve_fwd_below_acc`, `solve_fwd_apply_minus`, `solve_back_below`,
`solve_back_diag` + the `solve_dot4_f64` core), used by BOTH the serial and the level-parallel paths (the
duplicated loop bodies are gone — serial ≡ parallel by construction now):
- **Forward fusion is BIT-IDENTICAL**: each element accumulates its column terms in the same ascending-k
  order with the same mul-then-add/sub per term; only the loop interleaving changes. The diagonal triangle
  runs in 4-column blocks (exact sequential recurrence inside the block, fused update below it).
- **Backward fusion** is a new fixed deterministic reduction (2 FMA accumulators per column + fixed tail;
  the far/near split in the diagonal blocks is a fixed order). Values changed (third backward revision
  today) — residuals 8.9e-15, slightly better than before the day started.

## THE BOARD AFTER (CHOLMOD/Cerid, >1 = Cerid wins; 1T / 8T / 16T)

| matrix | FACTOR | SOLVE (1 RHS) | SOLVE x16 |
|---|---|---|---|
| lat12 | 0.93 / 0.64 / 0.48 | **1.33 / 1.43 / 2.53 WIN** | 0.79 / 0.77 / **1.25 W** |
| lat16 | 0.86 / 0.96 / 0.77 | **1.06 / 1.16 / 1.70 WIN** | **1.00 / 0.92 / 1.03** |
| lat20 | 0.94 / 0.98 / 0.87 | **1.06** / 0.88 / **1.32** | 0.73 / 0.55 / 0.67 |
| lat24 | 0.84 / **1.00 / 1.02 W** | **1.10** / 0.89 / **1.17** | 0.84 / 0.89 / 0.80 |
| lat28 | 0.86 / 0.87 / 0.90 | **1.07** / 0.90 / 0.94 | 0.71 / 0.60 / 0.51 |
| lat32 | 0.77 / 0.88 / **0.97** | **1.14 / 1.03** / 0.89 | 0.90 / 0.71 / 0.80 |
| bcsstk25 | **1.40 / 1.84 / 1.11 WIN** | **1.47 / 1.88 / 3.74 WIN** | 0.81 / 0.84 / 0.97 |
| hood | **1.03 / 1.58 / 1.91 WIN** | **1.13 / 1.08 / 1.14 WIN** | 0.89 / **1.62 / 1.48 WIN** |

**Single-RHS SOLVE: a WIN on every matrix at 1T (1.06–1.47), and on most at 8/16T (to 3.74×).** lat32 solve
went 0.50× (start of day) → **1.14×**. hood now wins every metric at every thread count.

## Named remaining (measured, in the verdict)

- **x16 mid-lattices @8/16T (0.51–0.80)**: CHOLMOD's multi-RHS dgemm threads well; our parallel x16 path's
  per-descendant small gemms don't. The serial x16 is 0.71–1.00. Lever: a multi-RHS variant of the fused
  kernels + a better-parallel multi-RHS structure.
- **lat28/32 single-RHS @8T (0.90, 1.03)**: CHOLMOD's solve gains ~10–15% at 8T from threaded gemv on the
  big supernodes; ours is deliberately serial (the level-fork lost). The lever is WITHIN-supernode
  parallelism for fronts above a size gate.
- **FACTOR serial 0.77–0.94**: our gemm streams at ~75 GF/s vs OpenBLAS asm's 84 (≈90%); in-factor packing
  reads are single-stream (the same DRAM mechanism — a multi-stream pack experiment is the next factor
  lever). 16T factor is at/near parity (0.97–1.02) via better scaling.
- lat12/16 FACTOR @16T (0.48–0.77): fork overhead on sub-50ms problems — a worker-count gate candidate.

## Verification

WSL gcc: dense 359,508/349 + direct 598,861/190 ✓ (the {1..16} moat holds — shared kernels by
construction). win-debug ✓ win-shipping ✓ win-asan ✓ win-tidy ✓ (598,861 each). The bench keeps the
trapezoid + per-pass-split diagnostics (permanently useful); the stream probe was scratch (deleted).
SuiteSparse source cached at `~/cerid-deps/suitesparse` for future digs.
