# 2026-05-30 — hesap v5a-5: single-RHS parallel Cholesky solve (in progress)

## Goal
Close the remaining v5a solve gap: the single-RHS (x1) supernodal-Cholesky solve was
serial, losing to CHOLMOD at 8 threads (hood 0.74, ldoor 0.76, bmwcra 0.53). Parallelize it.

## Implementation (engine/hesap-direct/src/supernodal_cholesky.cpp)
- `fwd_one`/`back_one` (the level-parallel solve lambdas) gained an `nrhs == 1` hand-axpy
  specialization (no N=1 gemm — the failed-gemv lesson). Constructed to reduce in the SAME
  k-ascending per-descendant order as the dedicated serial path => bit-identical by construction.
- Guard `if (nrhs == 1)` -> `if (nrhs == 1 && nw <= 1)`: serial keeps the dedicated hand path;
  nw>1 single-RHS now takes the level-parallel path.
- **Size gate** in `solve()`: `nw = (nrhs==1 && m_lnz < kSolveParallelMinLnz) ? 1 : num_workers()`.
  `kSolveParallelMinLnz = 24'000'000`. solve_with_workers still honors nw (moat test forces the path).

## Why the gate (measured, CHOLMOD oracle, WSL, capped threads)
Cerid-vs-Cerid single-RHS solve, 1-thread serial vs 8-thread parallel:
| matrix | n | x1 1T serial | x1 8T parallel | verdict |
|---|---|---|---|---|
| bcsstk13 | 2003 | 0.149 ms | 5.880 ms | 39x SLOWER (regress) |
| bcsstk24 | 3562 | 0.165 | 3.151 | 19x slower |
| bcsstk25 | 15439 | 0.857 | 10.833 | 12x slower |
| bmwcra_1 | 148770 | 99.76 | 83.88 | 1.19x faster |
| hood | 220542 | 29.84 | 25.34 | 1.18x faster |
| ldoor | 952203 | 177.96 | 101.44 | 1.75x faster |

Per-level dispatch overhead (parallel_for+wait+frame_reset x many tiny levels) dwarfs the
single-vector work on small factors -> catastrophic regression -> gate small => serial.

## vs CHOLMOD @8T, single-RHS (with gate: small=serial, large=parallel)
- ldoor x1: 0.64 -> 1.13x WIN
- hood x1: 0.77 -> 0.90x (improved, still loses)
- bmwcra_1 x1: 0.47 -> 0.56x (improved, still loses — the known scaling wall)
- bcsstk25 x1: serial 1.33x WIN (gate avoids the parallel regression)
FACTOR + SOLVE x16 unchanged (all WIN where they won before).

## Verification
- Ungated: MSVC /W4 /WX clean + gcc -Werror clean; full suite 591348 asserts / 22 cases green
  (new [v5a-5][determinism] test: single-RHS bit-identical across nw {1,2,3,pool} + correct).
- Gated: rebuild + re-verify in progress.

## HONEST standing
Partial crush: ldoor x1 WINS; hood close (0.90); bmwcra walled (0.56); small protected.
NOT a full x1 crush. Deeper lever (future): per-level work gate to capture the mid-range and
reduce hood's overhead (may push hood >1.0); bmwcra needs the block-DAG/2D within-front scaling
lever (same wall as its factor).

## Tooling note
Severe intermittent tool-output corruption this session (fabricated read tails, dropped results,
fake bench numbers when a binary path was wrong, jumbled background-task IDs). All conclusions
cross-checked against git/grep ground truth and Windows-side log files. CHOLMOD bench binary is at
build/linux-gcc-release/runtime/ (NOT runtime/examples/); matrices at
build/win-relwithdebinfo/_deps/suitesparse-mm/<name>/<name>.mtx; run via `wsl` with hardcoded
paths (shell $vars got eaten through the wsl layer).

## GATED — CONFIRMED (WSL CHOLMOD oracle, capped 8T; gcc build green)
SOLVE x1 vs CHOLMOD @8T (gated): bcsstk13 1.61x WIN, bcsstk24 0.83x, bcsstk25 1.30x WIN (was
0.10x ungated => regression fixed), bmwcra_1 0.57x, hood 0.91x, ldoor 1.12x WIN.
vs pre-v5a-5 (serial x1 @8T): ldoor 0.64->1.12x (LOSE->WIN), hood 0.77->0.91x, bmwcra 0.47->0.57x;
small matrices bit-identical to serial (no regression). FACTOR + SOLVE x16 unchanged (WIN).
Regression check: gated-8T x1 == 1T-serial x1 for small (bcsstk13 0.149==0.149, bcsstk25 0.853~0.856).
Determinism moat held (591348 asserts/22 cases, MSVC+gcc clean).

## NET (honest): PARTIAL solve crush.
WIN: ldoor x1 (headline ~1M) 0.64->1.12x. IMPROVED but still lose: hood 0.91 (close), bmwcra 0.57
(scaling wall = same ADR-0082/block-DAG lever as its factor). Small: protected (serial), no regression.
Remaining levers (future): per-level work gate (cut hood's per-level dispatch overhead, may push >1.0,
+ capture the 1.5-26M mid-range the global 24M gate skips); bmwcra within-front/2D scaling (separate).
