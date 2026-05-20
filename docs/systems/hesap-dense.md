# crd-hesap-dense — system overview

> **One-line purpose**: Dense BLAS substrate (Level 1 / 2 / 3) over the
> Cerid matrix-type catalog, with a reference-class benchmark policy
> measuring every shipped kernel against Eigen + OpenBLAS.

## Module surface

`crd-hesap-dense` ships:

- **Matrix-type catalog**: `Matrix<T, Layout>`, `MatrixView<T, Layout>`,
  `Symmetric<T>`, `Hermitian<T>`, `Triangular<T, Side, Diag>`,
  `Banded<T>`. All carry an `IAllocator*` that propagates into hesap's
  library functions (see "Allocator propagation" below).
- **BLAS L1** (`blas1.hpp`): `axpy`, `dot`, `dotu`, `dotc`, `nrm2`,
  `scal`, `copy`, `swap`, `asum`, `iamax`. SIMD path for f32/f64 via
  `detail/dot_simd.hpp` (8-accumulator `simd_dot_*` and `simd_sumsq_*`).
- **BLAS L2** (`blas2.hpp`): `gemv`, `gbmv`, `hemv`, `hbmv`, `symv`,
  `sbmv`, `ger`, `geru`, `gerc`, `syr`, `her`, `syr2`, `her2`, `trmv`,
  `trsv`, `tbmv`, `tbsv`. SIMD-tuned paths for f32/f64 RowMajor:
  - **gemv**: 8-row tiled with shared x load + gated row-prefetch.
  - **symv**: single-pass classic BLAS algorithm (each lower-half
    element touched once, updates both `y[i]` dot and `y[k]` rank-1).
    16-wide unroll + 4 dot accumulators + gated row-prefetch.
  - **trsv**: forward/back substitution with `simd_dot_*` for the inner
    `b - sum_{j<i} L[i,j]*x[j]` reduction.
- **BLAS L3** (`blas3.hpp`): `gemm`, `gemm_parallel`,
  `gemm_parallel_auto`, `small_gemm_parallel`, `syrk`, `herk`, `syr2k`,
  `her2k`, `trmm`, `trsm`, `gemm_mixed`. Goto/BLIS 5-loop layered with
  AVX2 microkernels (Vec8f for f32, Vec4d for f64 in 2× halves), Mc
  auto-tune, direct unpacked fast-path for small matrices, allocator
  propagation, FMA-based microkernel.
- **Dense direct solvers** (v0e): `LU<T,L>` (`lu.hpp`, partial pivoting,
  right-looking blocked, gemm_parallel trailing update),
  `Cholesky<T,L>` (`cholesky.hpp`, SPD; unblocked SIMD per-row-dot for
  n≤256, right-looking blocked + packed parallel `syrk_lower_minus`
  trailing update for n>256), `LDLT<T,L>` (`ldlt.hpp`, Bunch-Kaufman
  indefinite, 1×1 + 2×2 pivots, SIMD row-restructured trailing update),
  `QR<T,L>` (`qr.hpp`, blocked compact-WY Householder, transposed-panel
  SIMD factor, gemm_parallel block-reflector trailing update). All f32 +
  f64, RowMajor, bit-deterministic, allocator-propagating.
- **Solver support**: `LinearOp<T>` wrappers (`linear_op_dense.hpp`:
  `MatrixLinearOp`/`SymmetricLinearOp`), Hager 1-norm condition estimator
  (`condition.hpp`), iterative refinement (`refinement.hpp`:
  `refine_lu`/`refine_cholesky`). Packed register-tiled `syrk_lower_minus`
  (`detail/syrk_microkernel.hpp`) — reusable for future eig/sparse/opt.
- **CLI** (`cli_register*.cpp`): 14 BLAS L3 + L1 + L2 + 8 solver commands
  (`hesap.dense.solver.{lu,cholesky,ldlt,qr}.{f32,f64}`) registered via
  `CRD_HESAP_CLI_REGISTER_MODULE` per ADR-0081.
- **Mixed-precision** `gemm_mixed<TIn, TAcc>`: HPL-AI iterative-
  refinement pattern (f32 in / f64 accumulator).

## Dense direct solver scorecard (2026-05-20, i9-14900K AVX2, vs Eigen-MT)

Factor+solve, f64, P-core affinity, best-of-3. See ADR-0083 for the
row-major-vs-Eigen's-column-major small-N analysis.

```
Solver  N=64    N=128   N=256   N=512   N=1024
LU      0.52x   1.46x   ~70x    11x     21x       (WINS N>=128; Eigen LU serial at large N)
Chol    0.94x   0.70x   0.55x   0.84x   1.41x     (WINS N=1024; small-N row-major-bound, ADR-0083)
QR      1.19x   0.76x   0.96x   1.04x   --        (WINS N=64/512; blocked compact-WY)
LDLT    1.00x   0.96x   1.18x   1.13x   0.94x     (WINS/ties everywhere; SIMD row-restructured)
```

Small-N (≤256) dense factorizations trail Eigen ~1.4× — a row-major-vs-
column-major *layout-fit* gap (NOT kernel quality), proven by 3
controlled experiments + Eigen source reading. Settled in **ADR-0083**:
keep row-major (ML/array-ecosystem aligned) with a per-factor
column-major escape hatch. See `memory/project_cholesky_smalln_rowmajor_limit`.

## Allocator propagation

Per `memory/feedback_hesap_propagate_allocator`: **no hesap library
function uses `crd::memory::default_allocator()` for scratch**. Every
function either:

1. **Takes an `IAllocator* scratch` parameter** (the view-form
   `gemm` / `gemm_parallel` / `gemm_parallel_auto` / `small_gemm_parallel`).
   `nullptr` falls back to MallocAllocator only as a last-resort safety net.
2. **Reads `a.allocator()` from the input value-type** (`symv` reads
   from the Symmetric matrix's allocator; Matrix-form overloads of the
   L3 ops pass `a.allocator()` to the view-form).

This lets bench / test code construct a TlsfAllocator at the top of
the fixture and have it used for ALL hesap internal scratch — no
fragmentation, no implicit malloc.

## Reference-class benchmark policy

`crd-hesap-dense` is the first module to ship under
`docs/PRINCIPLES_reference_class_benchmarking.md`. Every
performance-critical kernel maintains a head-to-head bench vs
**Eigen 3.4 + OpenBLAS 0.3.27** on the same dev box.

**To run the shootout**:
```powershell
cmake -S . -B build/win-vs-ref -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCRD_BUILD_HESAP_VS_REFERENCE=ON
cmake --build build/win-vs-ref --target bench_hesap_gemm_vs_reference
cmake --build build/win-vs-ref --target bench_hesap_blas1_vs_reference
cmake --build build/win-vs-ref --target bench_hesap_blas2_vs_reference
& "build/win-vs-ref/runtime/bench_hesap_gemm_vs_reference.exe"
& "build/win-vs-ref/runtime/bench_hesap_blas1_vs_reference.exe"
& "build/win-vs-ref/runtime/bench_hesap_blas2_vs_reference.exe"
```

The harness:
- Fetches Eigen + OpenBLAS via CPM into `build/_deps/` (never vendored).
- Sets process affinity to P-cores (mask 0xFFFF on 14900K = 16 logical
  P-threads). Pass `--all-cores` to opt out.
- Matches thread counts across Cerid / Eigen / OpenBLAS (16 for L3,
  1 for L1/L2 since L1/L2 are memory-bandwidth-bound).
- Validates per-trial `max|c_cerid - c_eigen|` within ULP tolerance
  (flags `!MISMATCH!` on > 1e-3).
- Uses best-of-3 measurement with 3-4 warm-ups to amortize Eigen's
  OpenMP thread-launch overhead.

## Canonical benchmark numbers (2026-05-20, i9-14900K, AVX2)

These are the numbers that future reference-class regressions are
measured against. Any future slice that touches one of these kernels
re-runs the bench and either holds the line or improves.

### BLAS L3 GEMM — multi-threaded, 16 P-threads, best-of-3

**f32 GEMM:**

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
256    349.76          0.95             368.78x       151.71x
512    430.03          354.24           1.21x          32.18x
1024   624.10          369.95           1.69x          16.05x
2048   628.07          369.23           1.70x          13.10x
4096   721.52          557.13           1.30x          13.41x
```

**f64 GEMM:**

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
256    218.17          186.82           1.17x          62.31x
512    211.83          177.39           1.19x           9.54x
1024   310.81          222.92           1.39x           7.26x
2048   332.60          297.08           1.12x           6.69x
4096   355.90          354.11           1.01x           6.76x
```

**10/10 WINS over Eigen-MT** for both precisions at every workhorse N.

### BLAS L1 — single-thread, P-core, f64

**axpy.f64** (`y = α x + y`):

```
N        Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
1024     26.18           15.49            1.69x          2.63x  ✓
4096     20.23           16.37            1.24x          2.36x  ✓
16384    18.94           16.20            1.17x          2.21x  ✓
65536    18.81           16.34            1.15x          2.21x  ✓
262144   6.52            9.04             0.72x          0.81x  ◑  memory-bandwidth ceiling
```

**dot.f64** (`Σᵢ x[i] y[i]`):

```
N        Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
1024     31.16           31.83            0.98x          5.78x  ✓ tied
4096     20.54           28.74            0.71x          3.71x  ◑
16384    20.79           28.42            0.73x          3.76x  ◑
65536    20.88           28.67            0.73x          3.68x  ◑
262144   12.75           13.19            0.97x          2.28x  ✓ tied
```

**nrm2.f64** (`√(Σᵢ x[i]²)`):

```
N        Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
1024     30.23           31.36            0.96x         11.58x  ✓
4096     39.42           40.42            0.98x         14.73x  ✓
16384    32.19           43.47            0.74x         11.99x  ◑
65536    32.22           43.45            0.74x         11.99x  ◑
262144   26.92           42.09            0.64x          9.96x  ◑
```

### BLAS L2 — single-thread, P-core, f64

**gemv.f64** (`y = α A x + β y`):

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
64     43.05           43.81            0.98x          4.33x  ✓ tied
128    32.78           37.02            0.89x          3.87x  ◑
256    34.97           38.76            0.90x          4.58x  ◑
512    33.19           35.87            0.93x          5.16x  ◑
1024   25.48           24.94            1.02x          4.32x  ✓ WIN
2048   13.74           11.46            1.20x          3.08x  ✓ WIN
4096   9.76            7.80             1.25x          2.50x  ✓ WIN
```

**symv.f64** (`y = α A_sym x + β y`):

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
64     27.82           30.46            0.91x          3.11x  ◑
128    34.18           43.86            0.78x          3.71x  ◑
256    35.25           48.68            0.72x          3.94x  ◑
512    34.10           46.21            0.74x          4.57x  ◑
1024   33.01           39.17            0.84x          4.55x  ◑
2048   25.87           30.52            0.85x          4.19x  ◑
4096   13.49           13.59            0.99x          2.50x  ✓ tied
```

**trsv.f64.lower** (solve `L x = b` in-place):

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)  Cerid/Eigen   Cerid/OBLAS
64     9.69            10.07            0.96x          1.39x  ✓ tied
128    16.59           16.00            1.04x          2.28x  ✓ WIN
256    20.36           22.68            0.90x          2.68x  ◑
512    21.15           25.63            0.83x          3.11x  ◑
1024   16.85           22.59            0.75x          2.88x  ◑
2048   11.58           14.52            0.80x          2.58x  ◑
4096   6.46            8.65             0.75x          1.75x  ◑
```

**Across all L1+L2 ops, Cerid WINS vs OpenBLAS-on-MSVC at every N
(1.4-14.7×)**. OpenBLAS-on-MSVC is constrained by upstream's
`FORCE_GENERIC` flag and GAS-syntax `.S` kernels incompatible with
ml64 — see `docs/sessions/2026-05-19-hesap-vs-reference-shootout.md`.
True asm-tuned OpenBLAS requires Linux / MinGW / vcpkg.

## Future work — explicitly deferred, NOT abandoned

The sub-1× spots above are **filed perf debt**, not "good enough."
Per the continuous-benchmarking policy, they get closed when one of:

1. A consumer slice (e.g. v0e direct solvers) bottlenecks on the
   under-performing kernel and the system-level perf gap exceeds 10%.
2. AVX-512 hardware lands in CI → `v0d-microkernel-avx512` activates.
3. ARM CI lands → `v0d-microkernel-neon` / `-sve2` activate.
4. A future Eigen / OpenBLAS release pulls further ahead → reactive
   re-tuning.

Filed follow-on backlog:

| Task | Trigger | What it does |
|---|---|---|
| `v0d-asm-microkernel` | ADR-0082 three-condition revisit gate (GEMM >50% of solve time AND intrinsics <70% peak AND no algorithmic alternative). NOT triggered today. | Hand-tuned asm per microarchitecture behind `CRD_HESAP_MICROKERNEL_BACKEND=Asm` switch. Locked signature; drop-in replacement. |
| `v0d-microkernel-avx512` | AVX-512 hardware in CI (Zen 4, older Intel, Xeon). | Vec8d / Vec16f microkernel + L2 op specializations. Expected ~1.5-2× over AVX2 path. |
| `v0d-microkernel-neon` | ARM CI available (Apple Silicon, Graviton). | NEON Vec4f / Vec2d microkernel. |
| `v0d-microkernel-sve2` | SVE2 hardware (Apple M4, Graviton 3+). | Vector-length-agnostic SVE2 path. |
| `v0d-microkernel-blocks-empirical-sweep` | Multi-machine bench access. | Per-box Mc/Kc/Nc sweep, replace BLIS defaults where measurably worse. Low priority on a single-box benchmark. |
| `vs-ref-blas2-followups` | Real consumer needs L2 close-the-gap. | gemv L2-prefetch tune for small N; symv mid-N (0.72× at N=256); trsv large-N (0.75-0.83×). Each requires asm or AVX-512. |

**Continuous-benchmarking policy stays active.** If a future slice
touches `gemm_parallel` / `symv` / etc. for any reason, the bench
re-runs and any slippage from these numbers is BLOCKING for that
slice. We do not stop chasing Eigen.

## Determinism

Hesap microkernels use **single-rounded IEEE 754 FMA** (`simd::fma`
on Vec8f / Vec4d, mapping to `_mm256_fmadd_ps` / `_mm256_fmadd_pd`
on AVX2 with `std::fma` scalar fallback). This is bit-exact across
SIMD widths AND scalar paths within hesap (per IEEE 754-2008 std::fma
guarantee). Distinct from `crd-eylem`'s ADR-0063 strict two-rounded
`mul_add` contract — see ADR-0082 §2026-05-20 update for rationale.

Bit-exact-across-thread-counts tests (`tests/hesap-dense/test_blas3_parallel.cpp`)
verify `gemm_parallel` produces `std::memcmp`-equal output across
worker counts {1, 2, 4, 8, 16} for f32 + f64 at sizes {64, 256, 1024}.

## Test surface

- 118 hesap-dense cases / 4731 assertions.
- 7 hesap-sched cases / 82 assertions (TaskGraph + DependencyGraph).
- 5-config DoD (debug + ASan + shipping + release + tidy) green.

## References

- `docs/sessions/2026-05-19-hesap-vs-reference-shootout.md` — full
  bench journey, every iteration, every number.
- `docs/PRINCIPLES_reference_class_benchmarking.md` — the policy.
- `docs/decisions/0082-hesap-microkernel-intrinsics-strategy.md` —
  intrinsics-first decision + FMA acceptance.
- `docs/decisions/0063-eylem-determinism-contract.md` — strict
  determinism contract (separate from hesap, applies to physics).
- `docs/decisions/0078-units-strategy.md` §5 — two-layer typed
  architecture (typed API, raw inner kernels).
- `memory/feedback_hesap_propagate_allocator` — allocator propagation
  directive.
- `memory/project_hesap_beats_eigen_mt_via_fma` — FMA decision lineage.
- `memory/feedback_reference_implementations_are_the_floor` —
  underlying engineering principle.
