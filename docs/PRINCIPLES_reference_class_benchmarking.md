# Reference-Class Benchmarking Policy

> **Status**: Accepted 2026-05-20.
> **Scope**: every performance-critical numerical / parallel kernel Cerid
> ships from now on.

## The policy

For every numerical kernel that Cerid ships (GEMM, FFT, solvers,
factorizations, sparse SpMV, sorting, parallel reductions, etc.), we
maintain a **head-to-head benchmark vs the strongest open-source
references on the same dev box**.

The reference set, by class:

| Kernel class | Reference(s) |
|---|---|
| Dense BLAS (L1/L2/L3) | **Eigen-MT 3.4** + **OpenBLAS 0.3.27** |
| Sparse BLAS | **Eigen Sparse** + **SuiteSparse:CHOLMOD/UMFPACK** |
| FFT | **FFTW3** + **PocketFFT** |
| Dense solvers (LU/QR/Chol) | **Eigen LU/QR/LLT** + **LAPACK (via OpenBLAS)** |
| Sort / parallel-for | **oneTBB** + **std::execution::par_unseq (libstdc++/MSVC)** |

If a kernel does not yet have a reference, we **file the comparison as
a blocker on shipping that kernel.** No "future work" excuses.

## Why this is non-negotiable

Cerid is positioned as a general-purpose engine substrate that
competes with hand-tuned numerical libraries (per ADR-0081 agent-native
strategy + ADR-0082 hesap microkernel decision). "Faster than scalar"
is not a benchmark. "Beats reference class on the same silicon" is the
only acceptable performance claim.

This mirrors the BVH precedent: Cerid's GPU-LBVH beat KittenGpuLBVH on
a card with **less** memory bandwidth than the reference. That standard
applies everywhere now.

## The protocol

When shipping a new performance-critical kernel, the slice MUST include:

### 1. Rock-solid correctness FIRST

Tests must pass **before** the benchmark conversation starts:
- Bit-exact across SIMD widths (when ADR-0063 applies).
- ULP-tolerance validation against a known-good reference (naive triple
  loop, scipy/numpy, etc.) — the FIRST validation, before any perf work.
- Edge cases: zero-size, transpose, non-multiple-of-tile dimensions,
  alignment edges, ill-conditioned inputs where applicable.
- Concurrent / multi-thread bit-exactness.
- 5-config DoD (debug + ASan + shipping + release + tidy) — all green.

**A failing test is not "perf debt" — it blocks the perf comparison
entirely.** No optimization commits land while tests are red. No
exception.

### 2. Gated reference-class CPM fetch

Add a `CRD_BUILD_HESAP_VS_REFERENCE`-style option that's **OFF by
default**. References are fetched on-demand into `build/_deps/`
(git-ignored), NEVER vendored into the repo. The default build, every
preset, every CI config behaves identically with the option off.

### 3. Head-to-head bench

Build `bench_<module>_vs_reference.cpp` that:
- Runs **identical workloads** through Cerid and each reference. Same
  random seeds, same dimensions, same memory layout.
- Sets `Eigen::setNbThreads()` / `openblas_set_num_threads()` to match
  Cerid's `crd::jobs::num_workers()`.
- Validates: `max|result_cerid - result_reference| / max|result_reference|`
  per trial. Flags `!MISMATCH!` on > 1e-3 (or the kernel's documented
  tolerance). **Numbers reported without validation are noise.**
- Uses best-of-3 measurement with 3-4 warm-ups per trial to amortize
  thread-launch and first-call overhead.
- Reports per-trial GFLOPS, iters, and Cerid-vs-each-reference ratios.
- Honors hardware specificity (hybrid CPU: P-core affinity; NUMA:
  pin threads). Both Cerid and reference get the same constraint.

### 4. Exact numbers in the session log

Every shootout produces a markdown table in the slice's session log:

```
N      Cerid (GFLOPS,nw) Eigen-MT (GFLOPS) C/Eigen   max|err|
1024   501     (nw=16)   407.05            1.23x ✓   8.56e-7
...
```

No averaging. No best-of-N obscuration. Show the actual numbers from
the actual run. If we have to "explain away" a loss, that's an action
item, not a footnote.

### 5. Goal: beat the reference class

The bar is **Cerid >= reference at every workhorse N (>= 1024 for
GEMM-class kernels, equivalent thresholds for other classes)**. At
very small problem sizes where overhead dominates, we accept losses
to libraries with specialized fast-paths, **but only if the loss is
documented and filed as a follow-on with concrete remediation**.

A slice ships GREEN only when this bar is met (or its loss is
explicitly accepted in the session log + filed follow-on).

### 6. Limitations get documented in code AND session log

OpenBLAS-on-MSVC is a real example (hardcoded `FORCE_GENERIC` upstream
+ GAS-syntax .S kernels incompatible with ml64). When a reference
cannot produce a fair comparison on a platform, the limitation is
documented in:
- The bench harness comments (so future readers know why numbers
  look weak).
- The CMake config comments (so the patch attempts are visible).
- The session log (with the path forward — vcpkg, MinGW, Linux CI).

## What "rock solid tests" means before reference comparison

Before the reference-class benchmark is in scope, the kernel needs:

| Test class | Required state |
|---|---|
| Bit-exact correctness (single SIMD width) | All pass. |
| Bit-exact across SIMD widths | All pass (or ADR-0063 exemption documented). |
| Concurrent / multi-thread bit-exact | All pass. |
| Transpose / non-multiple shapes | All pass. |
| 5-config DoD: debug + ASan + shipping + release + tidy | All green. |
| Memory: no leaks, no UAF, no OOB (verified by ASan ctest) | All clean. |

Only when this matrix is green do we open the reference-class
comparison. The bench is the **last** gate, not the first.

## Filed reference comparisons (as of 2026-05-20)

| Kernel | Status | Reference(s) | Session log |
|---|---|---|---|
| `crd-hesap-dense` GEMM | ✅ Cerid beats Eigen-MT at N >= 1024 (1.12-2.59×) | Eigen 3.4, OpenBLAS 0.3.27 (MSVC-generic limitation) | `2026-05-19-hesap-vs-reference-shootout.md` |
| `crd-hesap-dense` BLAS L1 (axpy / dot / nrm2) | OPEN | Eigen 3.4, OpenBLAS 0.3.27 | — filed |
| `crd-hesap-dense` BLAS L2 (gemv / symv / trsv) | OPEN | Eigen 3.4, OpenBLAS 0.3.27 | — filed |
| `crd-hesap-dense` v0e direct solvers (LU/QR/Chol) | BLOCKED on v0e shipping | Eigen LU/QR/LLT, LAPACK | — filed |
| `crd-jobs` parallel_for / parallel_reduce | OPEN | oneTBB | — filed |
| `crd-containers::sort` | OPEN | std::sort, oneTBB sort | — filed |

Every "OPEN" entry becomes BLOCKING the next time we touch that module
for non-trivial perf work.

## Where this applies

This policy applies to:

- `crd-hesap` (numerical computing) — every BLAS / LAPACK / solver / FFT
  slice from now on.
- `crd-jobs` — parallel primitives.
- `crd-containers` — sort, hash, dense storage layouts.
- `crd-math` — SIMD primitives where reference SIMD libraries exist
  (xsimd, highway).
- `crd-geometry-*` — already passing the BVH-vs-KittenGpuLBVH bar; the
  policy formalizes what was an implicit standard.
- `crd-eylem` (physics) when v1c+ resumes — vs Box2D / PhysX-CPU /
  Chaos / Jolt at equivalent feature sets.
- Future: `crd-renderer`, `crd-rhi` vs reference renderers when
  perf-critical.

## What this is NOT

This is NOT a "feature parity" policy. We don't have to match every
LAPACK routine, every Eigen template, every TBB primitive. **We have
to be at-or-better than the reference at the routines we ship.**

This is NOT a "vendor everything" policy. References live in
`build/_deps/`, fetched on-demand, never committed. The standard
Cerid build remains zero-dependency on these references.

This is NOT a "publish papers" policy. The numbers live in session
logs for the team's reference, not for external publication. They're
honest engineering measurements — including the losses.

## References

- `docs/sessions/2026-05-19-hesap-vs-reference-shootout.md` — the
  inaugural reference shootout. Establishes the bench harness pattern
  and the "hybrid CPU affinity + matched threads" methodology.
- `docs/decisions/0082-hesap-microkernel-intrinsics-strategy.md` —
  motivates the "intrinsics-class peer" target (Eigen / Faer / Highway).
- `memory/feedback_reference_implementations_are_the_floor.md` — the
  underlying engineering principle: "if someone hit X ms on this
  hardware, we hit X ms too."
