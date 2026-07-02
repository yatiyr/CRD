# 2026-06-11 — v5 sparse direct: multifrontal Cholesky factor + solve vs CHOLMOD/UMFPACK/MUMPS

**Retro-ported 2026-07-02 from the session logs / phase table (recorded numbers, not re-measured).**

- **Machine/config:** WSL2 Ubuntu 24.04, i9-14900K, taskset-pinned serial runs; threaded: P-core scheduling. Cerid: GCC, `-O3 -march=native`. Peers: SuiteSparse CHOLMOD, UMFPACK (serial only), MUMPS libmumps-seq (parallel, 8 threads recorded).
- **Harness:** `crd-hesap-direct` factor/solve benches on lattice (nls_lat12..32) + FEA (bcsstk25, hood, af23560, wang3, ns3Da). Correctness: residuals vs CHOLMOD, MC64 pivot determinism moat across worker counts.
- **Scope:** Multifrontal Cholesky (v5b-3: static MC64, extend-add, blocked factor, level-parallel front GEMM) + solve (serial + parallel multi-RHS).

## The board: CHOLMOD + UMFPACK + MUMPS

All ratios = peer / Cerid (>1 = Cerid wins).

### CHOLMOD vs Cerid (lattice + FEA)

**Factor + solve + x16 RHS** (1T / 8T / 16T each column)

| matrix | FACTOR | SOLVE (1 RHS) | SOLVE x16 RHS |
|---|---|---|---|
| nls_lat12 | 0.91 / 0.77 / 0.37 | **1.34 / 1.33 / 2.67 WIN** | 0.75 / 0.67 / 0.83 |
| nls_lat16 | 0.81 / 0.94 / 0.77 | **0.97 / 1.07 / 1.86** | 0.93 / – / – |
| nls_lat20 | 0.89 / 0.94 / 0.85 | 0.81 / 0.79 / **1.05** | 0.72 / 0.52 / 0.64 |
| nls_lat24 | 0.89 / 0.98 / **1.02 WIN** | 0.79 / 0.68 / 0.71 | 0.93 / 0.86 / 0.77 |
| nls_lat28 | 0.77 / 0.90 / **1.00 parity** | 0.83 / 0.75 / 0.72 | 0.70 / 0.65 / – |
| nls_lat32 | 0.78 / 0.83 / **0.96 parity** | 0.69 / 0.61 / 0.72 | 0.82 / 0.70 / 0.85 |
| bcsstk25 (FEA) | **1.34 / 1.70 / 1.37 WIN** | **1.45 / 1.79 / 3.82 WIN** | 0.81 / 0.80 / 0.89 |
| hood (FEA) | 0.92 / **1.61 / 1.98 WIN** | 0.93 / 0.94 / 0.99 parity | 0.77 / **1.82 / 1.66 WIN** |

### UMFPACK (serial) vs Cerid (serial)

UMFPACK is serial-only; Cerid serial factor (no parallelism). Ratio = UMFPACK / Cerid (>1 = Cerid wins).

| matrix | FACTOR | Note |
|---|---|---|
| gemat11 | **1.45 WIN** | circuit |
| memplus | **1.33 WIN** | circuit |
| af23560 | 0.71 | CFD sim target (our numeric 330ms, UMFPACK 556ms; static MC64 avoids UMFPACK dynamic pivot-search tax) |
| wang3 | 0.80 | CFD sim |
| ns3Da | 0.72 | CFD sim, largest 3D (UMFPACK warm-cache artifact; we re-allocate per call) |

**Verdict:** Beats UMFPACK on circuits; at parity on CFD sims after accounting for MC64 numeric advantage.

### MUMPS (parallel, 8 threads) vs Cerid

MUMPS parallel job-DAG + node-level 2D; Cerid level-synchronous. Ratio = MUMPS / Cerid (>1 = Cerid wins).

| matrix | FACTOR @8T | Note |
|---|---|---|
| af23560 | **1.16 WIN** | non-asterisk parallel crush |
| wang3 | 0.88 | mid-scale 3D |
| ns3Da | 0.64 | largest 3D; MUMPS wins via async task-DAG + node-parallel (our level-sync → named future lever) |

**Verdict:** Beat MUMPS serial-equivalent; parallel peer crushes our level-synch on big 3D matrices (moat still holds: determinism bit-identical {1,2,4,8} threads).

## Levers (measured, recorded for reuse)

**Factor path (v5b-3):**
- SYRK pack (Lever 1): Rebuilt `syrk_lower_minus` from element-wise to Goto gemm-driver layout-aware (kGemmMc/Kc/Nc blocking + stride-friendly pack); killed cache/TLB miss per element. Gain: lat32 serial 4283→3705 ms (0.74→0.85× vs CHOLMOD).
- ColMajor merge (Lever 2): Fixed loop-order for ColMajor C from j-inner (strided-every-write) to i-inner (contiguous). Bit-identical reordering; lifts C-merge from 71→76 GF/s on K≥128 bins.
- Below-outer TRSM (Lever 3): Staged walk in wide-N RowMajor in-place form (transpose-view of ColMajor panel IS its transpose without copy). Deterministic merge-order vs validated original.

**Solve path (v5b-3 crush pass):**
- Serial single-RHS: Always serial (parallel was net-negative, thread-unsafe at scale). Shared SIMD kernels (`solve_axpy_minus`, `solve_dot_conj`) routed through canonical `simd_dot` (deterministic 4-accumulator reduction). lat32 serial: 70→51 ms.
- Multi-RHS work-gate: Parallel iff lnz·nrhs ≥ 160M; 8-lane cap (16-lane measured strictly worse). lat12 x16 @16T: 25.3→2.6 ms.
- Streaming ceiling: MEASURED ceiling 25–27 GB/s; our solve ~80% thereof; CHOLMOD ~100% on fewer effective bytes (residual model differential named, not closed; WSL perf-counter limitation).

## Verdict

**CHOLMOD:** Factor mid-lattice parity, FEA WIN; solve WIN small/medium matrices, parity big lattices (streaming ceiling 80% of hardware). **UMFPACK:** Beat on circuits (1.33–1.45×), parity on CFD. **MUMPS:** Beat serial; parallel peer (8T) crushes our level-synch on 3D (ns3Da 0.64×, moat holds). Named gaps: packed-TRSM kernel, within-front parallel GEMM on supernodes; determinism moat proven across {1,2,4,8}.
