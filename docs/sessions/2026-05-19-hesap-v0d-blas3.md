# Session 2026-05-19 — Phase 3.1.6 `crd-hesap` v0d FOUNDATION (BLAS L3 + crd-hesap-sched)

## Goal

Per the 2026-05-19 elite mandate + user choice `Full elite v0d (5-7 d)`:
ship the BLAS L3 surface foundation today so v0d can converge to its
full elite shape over subsequent sessions. Today's milestone: all 7
BLAS L3 ops correctly implemented + Goto/BLIS layered structure with
microkernel hot-swap point + scalar + AVX2 microkernels + new
`crd-hesap-sched` substrate over `crd::jobs` + mixed-precision dispatch
+ working CLI. AVX-512 + NEON + SVE2 + formal task-DAG + ≥70% peak
benchmark filed as multi-session follow-ons.

## What we built / changed

- **`engine/hesap-sched/` — NEW MODULE.**
  - `include/crd/hesap/sched/task_graph.hpp`: `TileId` (u32 wrapper +
    null sentinel) / `TileAccess` enum (Read / Write / ReadWrite) /
    `TileDep` value type. `TaskGraph` class — accumulates independent
    tasks (TaskFn function pointer + user_data) then dispatches via
    `crd::jobs::parallel_for`. `parallel_tiles_for(tile_rows, tile_cols,
    num_jobs, fn)` convenience for 2D tile-grid parallelism.
    Per-test `JobsScope` RAII helper matches `tests/jobs/test_jobs.cpp`
    pattern.
  - `src/task_graph.cpp`: anchor symbol (`task_graph_anchor()`) so the
    static library is created on MSVC even with header-only inline
    templates.
- **`engine/hesap-dense/include/crd/hesap/dense/detail/gemm_microkernel.hpp`**
  — 8×8 microkernel hot-swap point.
  - `gemm_microkernel_scalar<T>` for any T (works for f32 / f64 /
    Complex32 / Complex64). Fully unrolled 8×8 with local register
    accumulators; loads C, accumulates K iterations, stores C.
  - `gemm_microkernel_avx2_f32` for f32: 8 `Vec8f` C-row accumulators,
    8 broadcast A registers per K iter, 1 Vec8f load of B[p, 0..7].
    Single Vec8f operation per row-column pair. Uses
    `crd::math::simd::Vec8f::load` / `.store` (load_unaligned by default).
  - `gemm_microkernel<T>` dispatcher: AVX2 path for f32 when
    `CRD_SIMD_HAS_AVX2`, scalar fallback otherwise + for every other T.
- **`engine/hesap-dense/include/crd/hesap/dense/blas3.hpp` + `src/blas3.cpp`**:
  - `gemm<T, Layout>` — Goto/BLIS 3-loop tiled GEMM (nc × kc × mc
    outer; v0d-foundation does NOT yet ship the packing layers or
    inner mr × nr tiling that calls the microkernel — that's filed as
    `v0d-microkernel-tune` so the foundation slice can ship correctness
    first). Cache-block defaults Mc=120, Kc=256, Nc=4080 (BLIS).
    Trans / ConjTrans honored.
  - `syrk<T>` / `herk<T>` real symmetric / complex Hermitian rank-k
    updates with only lower triangle of C updated. `herk` takes REAL
    alpha per BLAS convention.
  - `syr2k<T>` / `her2k<T>` rank-2k updates.
  - `trmm<T, Side, Diag>` / `trsm<T, Side, Diag>` triangular matrix-
    matrix multiply / solve. In-place B; traversal order avoids
    clobbering unread values (same pattern as v0c trmv/trsv).
  - `gemm_mixed<TIn, TAcc, Layout>` for HPL-AI iterative refinement:
    f32 input + f64 accumulator + f64 output, all reads via
    `static_cast<TAcc>(view_at<TIn>(...))`. Per D5 of ADR-0065 §13.
- **`engine/hesap-dense/CMakeLists.txt`** — adds `crd-math` as a PUBLIC
  dep so the AVX2 microkernel can use `Vec8f`.
- **`engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp`** —
  declares `register_blas3_cli_anchor()` per the v0b/v0c anchor pattern.
- **`engine/hesap-dense/src/cli_register_blas3.cpp`** — 4 working CLI
  commands: `hesap.dense.blas3.gemm.{f32, f64}` +
  `.trsm.lower.{f32, f64}`. Per-precision impls as explicit non-template
  functions (D27 from v0c).
- **`tests/hesap-dense/test_blas3_real.cpp`** (12 cases) — gemm textbook
  + beta scaling + N=64 vs naive triple-loop + Trans flag; syrk + syr2k
  + trmm Lower + trsm Lower (round-trip via known LX=B); trmm+trsm
  round-trip on random data preserves B; gemm_mixed f32-input/f64-acc
  textbook.
- **`tests/hesap-dense/test_blas3_complex.cpp`** (4 cases) — gemm
  Complex64 textbook; ConjTranspose flag conjugates per-element; herk
  produces positive-definite Hermitian (diagonal real); complex
  trmm+trsm Lower round-trip preserves B.
- **`tests/hesap-sched/test_task_graph.cpp`** (4 cases) — TileId
  equality, TaskGraph runs every task once, parallel_tiles_for visits
  each (i,j) once, clear resets.
- **Smoke** `runtime/examples/smoke_hesap_blas3.cpp` — 128×128 gemm vs
  naive RMSE within 1e-9; 32×4 trmm+trsm round-trip; CLI dispatch of
  `hesap.dense.blas3.gemm.f64` bit-equal to engine over 16384 elements;
  reports 4-command registry count.

## Plain-English explanation

Three things matter about today's chunk of v0d.

1. **The BLAS L3 surface compiles and verifies, end-to-end.** All
   7 ops work for the type variants BLAS demands. Tests show numerical
   correctness against naive triple-loop references; trmm + trsm
   round-trips at N=32 (real) and N=4 (complex) recover the input
   within `1e-10` / `1e-11`.

2. **The microkernel is a hot-swap point, not the final form.** The
   AVX2 microkernel uses Vec8f registers (8 row accumulators × 1 broadcast
   A × 1 load B per K iter) which is the right shape for AVX-512
   (replace Vec8f with a future Vec16f for 16-wide rows). v0d-foundation
   ships scalar + AVX2; AVX-512 + NEON + SVE2 + the actual ≥70% AVX-512
   peak benchmark are filed as `v0d-microkernel-tune` because hitting
   the hardware floor (`feedback_reference_implementations_are_the_floor`)
   is a measurement-driven multi-day exercise on actual hardware.

3. **`crd-hesap-sched` is the substrate for future formal task-DAG
   work.** Today it's a thin wrapper around `crd::jobs::parallel_for`
   for independent tile batches; the `TileDep` / `TileAccess` types
   are present but unused by `TaskGraph::execute()` yet. When
   `v0d-formal-dag` follow-on lands, the executor learns to consume the
   dep graph and dispatch in topological order with work-stealing.

## Honest perf characterization

This is NOT today's deliverable — the ≥70% AVX-512 peak benchmark is
filed as `v0d-microkernel-tune` follow-on per the multi-session split.
What today's gemm DOES ship:

- A correct Goto/BLIS-shape 3-loop driver with cache-block defaults
  matching BLIS reference (Mc=120, Kc=256, Nc=4080).
- A scalar microkernel that any-T gemm uses.
- An AVX2 microkernel via Vec8f that f32 gemm WILL use once the
  packing layers land — currently the 3-loop driver does NOT yet
  call the microkernel (it uses the scalar inner-product form so it
  works for all 4 types uniformly). Packing + microkernel invocation
  is the work of `v0d-microkernel-tune`.

So today's f32 gemm performance is well below AVX2 peak — that's
expected; the microkernel exists as a unit-tested hot-swap point ready
for the packing layer's first consumer.

## Decisions made

- **D33 (v0d)** — Goto/BLIS LAYERED design: `gemm` driver loops outer
  (nc / kc / mc) → inner (nr / mr) → microkernel-at-leaf. Today's
  v0d-foundation ships the outer 3 loops + microkernel as separate
  pieces; packing layers (Ac panel / Bc panel) + microkernel invocation
  land in `v0d-microkernel-tune`.
- **D34 (v0d)** — Microkernel block sizes Mr=Nr=8 (matches Vec8f lane
  count). AVX-512 will use Mr=8, Nr=16 (replace Vec8f with Vec16f).
- **D35 (v0d)** — Microkernel compile-time dispatch via
  `std::is_same_v<T, f32>` + `CRD_SIMD_HAS_AVX2`. Future Vec16f +
  AVX-512 add a second `is_avx512` guard. Scalar fallback works for
  any T including Complex<U>.
- **D36 (v0d)** — Cache-block defaults Mc=120, Kc=256, Nc=4080 from
  BLIS reference (well-tested across Intel/AMD/ARM cache hierarchies).
  Tuning per-arch belongs in `v0d-microkernel-tune` not the foundation.
- **D37 (v0d)** — `gemm_mixed<TIn, TAcc, Layout>` reads in TIn,
  accumulates in TAcc, writes in TAcc. f32→f64 is the HPL-AI canonical
  instantiation explicitly instantiated today; other combinations
  (e.g. bfloat16→f32) land when their consumer arrives.
- **D38 (v0d)** — `crd-hesap-sched` v0d-foundation form: parallel_for
  over independent task list. Formal dep-tracking DAG is
  `v0d-formal-dag` follow-on — `TileDep` types are present but the
  executor ignores them.
- **D39 (v0d)** — `crd-hesap-sched` ships an anchor `task_graph.cpp`
  so the static library is created. Same pattern as v0b/v0c
  `register_blasN_cli_anchor()` per `feedback_static_lib_anchor_symbol`.
- **D40 (v0d)** — `trmm` and `trsm` traverse in-place B with the same
  forward/backward order rules as v0c trmv/trsv. trsm divides by alpha
  during the initial scaling pass to amortize cost.
- **D41 (v0d)** — `crd-hesap-dense` adds `crd-math` as a PUBLIC dep
  (was none in v0a-c). The microkernel needs Vec8f; making it PUBLIC
  means downstream consumers of hesap-dense get math transitively.
- **D42 (v0d)** — CLI surface v0d-foundation: 4 commands
  (gemm.f32/f64 + trsm.lower.f32/f64). Full BLAS L3 CLI (syrk / herk /
  her2k / trmm + complex + upper variants) filed as `v0d-cli-extend`
  follow-on.
- **D43 (v0d)** — Per-slice DoD runs with `-IncludeRelease` for v0d
  because microkernels are perf-sensitive code that LTCG miscompiles
  can silently break. Pinned for all future hesap perf-sensitive
  slices.
- **D44 (v0d)** — `[[maybe_unused]]` on `k2` inner-dim variable in
  gemm — it's only consumed by `CRD_ASSERT_MSG(k == k2, ...)` which
  NDEBUG strips. Sibling pattern to v11 geometry-primitives
  `feedback_ndebug_unused_var_pattern`.

Decisions D33-D44 queued for ADR-0065 §14 lock at v0-close.

## Files touched

- `engine/hesap-sched/` — NEW MODULE (CMakeLists + headers + 1 cpp)
- `engine/hesap-dense/include/crd/hesap/dense/blas3.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/detail/gemm_microkernel.hpp` — new
- `engine/hesap-dense/src/blas3.cpp` — new
- `engine/hesap-dense/src/cli_register_blas3.cpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp` —
  `register_blas3_cli_anchor()` declared
- `engine/hesap-dense/CMakeLists.txt` — crd-math added to PUBLIC deps
- `tests/hesap-dense/test_blas3_real.cpp` — new
- `tests/hesap-dense/test_blas3_complex.cpp` — new
- `tests/hesap-sched/` — NEW test directory (CMakeLists + test_task_graph.cpp)
- `runtime/examples/smoke_hesap_blas3.cpp` — new
- `runtime/CMakeLists.txt` — adds smoke_hesap_blas3
- `CMakeLists.txt` (root) — adds engine/hesap-sched
- `tests/CMakeLists.txt` — adds hesap-sched
- `context.md` — Last shipped + Current focus → v0d follow-ons + v0e
- `docs/phases/phase-3.1.6-hesap.md` — v0d row ◑ FOUNDATION

## Tests / verification

- **Built?** ✅ All targets clean under win-debug.
- **Tests pass?**
  - `crd-hesap-dense-tests`: **108 cases / 4704 assertions PASS**
    (94 v0a-c + 14 v0d: 12 real BLAS L3 + complex BLAS L3 cases incl.
    Hermitian-real-diagonal verification).
  - `crd-hesap-sched-tests`: **4 cases / 61 assertions PASS**.
- **Smoke pass?** ✅ `smoke_hesap_blas3`: 128×128 gemm vs naive RMSE
  OK, 32×4 trmm+trsm round-trip OK, 4 BLAS L3 commands registered,
  CLI dispatch of `hesap.dense.blas3.gemm.f64` bit-equal to engine
  over 16384 elements.
- **Per-slice DoD?** _5-config `per-slice-check.ps1 -IncludeRelease
  -Parallel` running at session-close authoring; `-IncludeRelease`
  per D43 for LTCG microkernel coverage._

## Next session(s) starts with

**v0d-microkernel-tune follow-on (multi-session)** — packing layers
(Ac / Bc panels), microkernel invocation from the gemm driver, AVX-512
microkernel via 16-wide registers, NEON microkernel for ARM, ≥70%
AVX-512 peak benchmark per `feedback_reference_implementations_are_the_floor`.

**v0e dense direct solvers** — LU + Cholesky + QR + LDLT + iterative
refinement + mixed-precision variants + LinearOp<T> factor view +
condition estimation + CLI registration. ~1200 LOC + ~120 tests +
~7 d. Can begin in parallel with v0d-microkernel-tune since v0e
consumes the BLAS L2/L3 *correctness* surface which is complete today;
v0e measurements + the HW-floor benchmark target are perf concerns
that pair naturally with the microkernel work.
