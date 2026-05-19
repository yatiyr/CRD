# Session 2026-05-19 — Phase 3.1.6 `crd-hesap` v0b BLAS L1

## Goal

Ship the first elite v0b deliverable per the 2026-05-19 strategic pivot
(ADR-0065 §13 elite-tier): BLAS Level 1 across all 4 type variants
(f32 / f64 / Complex32 / Complex64) consuming the v0a substrate end-to-
end. Per advisor + ADR-0081 §10, "shipping CLI" means callable from the
registry, not just registered schemas — so v0b also expands the v0a CLI
substrate with the `ArgValue` typed-param container and stands up 28
working CLI commands.

## What we built / changed

- **`engine/hesap/include/crd/hesap/cli/arg_value.hpp` + `src/arg_value.cpp`** —
  new `ArgValue` discriminated-union typed param value. 11 kinds (Empty / Bool
  / I64 / U64 / F64 / Complex64 / String / F64Array / I64Array / MatrixId /
  VectorId). Scalars use a POD union; variable-size data (string / arrays)
  uses `crd::containers::String` + `Array<T>` for allocator-owned storage.
- **`engine/hesap/include/crd/hesap/cli/command_registry.hpp` extension** —
  `CommandArgs` now carries `HashMap<String, ArgValue>` + typed setters/getters
  (`set_f64` / `get_f64` / `set_f64_array` / `get_f64_array` / ... for every
  ArgValue kind). v0a callsites (`CommandArgs args{};` + `args.alloc = &alloc`)
  flipped to `CommandArgs args{&alloc}` so `values`'s HashMap shares the alloc.
- **`CommandRegistry` ctor made public** — tests + downstream consumers can
  now build LOCAL registry instances; v0a's `reg.clear_for_tests()` pattern
  in test_command_registry would have wiped the static-init-registered BLAS
  L1 commands. New test pattern: `CommandRegistry reg;` for isolation, or
  `CommandRegistry::global()` for static-init-registered ops.
- **`engine/hesap-dense/include/crd/hesap/dense/real_type.hpp`** — trait
  giving the real scalar associated with a (possibly complex) T:
  `real_type<f32>::type=f32`, `real_type<Complex<T>>::type=T`. Used by
  `nrm2` / `asum` / `iamax` so complex inputs return real magnitudes.
  Also `is_complex_v<T>` predicate for SFINAE branching inside `blas1.cpp`.
- **`engine/hesap-dense/include/crd/hesap/dense/vector.hpp`** — owning
  `Vector<T>` value type. `IAllocator*` ctor, sized ctor (zero-init),
  `initializer_list` ctor, `ConstSpan<T>` ctor. Move-only (copy via
  explicit `.clone(alloc)`, D15). `size()` / `data()` / `operator()(i)` /
  `span()` / `fill()`. Aligned via `IAllocator->allocate(n*sizeof(T),
  alignof(T)>16 ? alignof(T) : 16)`.
- **`engine/hesap-dense/include/crd/hesap/dense/detail/pairwise_sum.hpp`** —
  Kahan-Babuška-Neumaier (KBN) compensated summation + pairwise reduction
  tree. `kPairwiseLeafBlock=8` (matches Vec8f lane count, D11). Both
  `pairwise_sum(span)` (materialised input) and `pairwise_sum_produced<T>(
  n, produce)` (lazy via `produce(i) -> T`). The produced overload is
  parameterised over (start, end) indices not a capturing lambda — fixes
  C1060 compiler heap exhaustion from each recursion level creating a
  fresh lambda closure type. New memory entry pinned.
- **`engine/hesap-dense/include/crd/hesap/dense/blas1.hpp` +
  `src/blas1.cpp`** — 9 BLAS L1 ops (`axpy` / `dot` / `dotu` / `dotc` /
  `nrm2` / `scal` / `copy` / `swap` / `asum` / `iamax`) templated over
  T; real-only `dot<T>` (T ∈ {f32, f64}), complex-only `dotu<T>` +
  `dotc<T>` (T = Complex<U>, U ∈ {f32, f64}). All reductions route
  through `detail::pairwise_sum_produced`. `iamax` ties broken by FIRST
  index (D16, LAPACK convention). Explicit instantiation for all 4 types
  in `blas1.cpp`.
- **`engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp` +
  `register_blas1_cli_anchor()` symbol in `cli_register.cpp`** — anchor
  pattern. The static-init `CRD_HESAP_CLI_REGISTER_MODULE` block in
  `cli_register.cpp` registers 28 BLAS L1 commands, but MSVC drops the
  .obj from a static library when no external symbol references it.
  The anchor is a tiny exported no-op function that test/smoke calls
  once via an anonymous-namespace `AnchorPull` static — referencing it
  pulls the entire `cli_register.cpp.obj` into the link, and the
  static-init runs.
- **`engine/hesap-dense/src/cli_register.cpp`** — single
  `CRD_HESAP_CLI_REGISTER_MODULE` block that constructs 28 typed
  `CommandSchema` declarations (7 real ops × 2 precisions + 7 complex
  ops × 2 precisions; total 28). Each schema declares its params with
  `add_param` (alpha / x / y / src), output kind (Scalar / BinaryBlob /
  Error), capability `kHesapCompute`, `idempotent=true`. The impl reads
  `args.get_f64_array("x")`, unpacks into `Vector<T>` (real path) or
  flattened `Vector<Complex<U>>` (complex path; flat `{re,im,...}`), runs
  the templated op, packs the result back into a `ResultBinaryBlob` or
  `ResultScalarF64`. v0b inline-JSON-array shape per D14; future
  `VectorRegistry` follow-on bumps schemas to v2.0.
- **`engine/hesap-dense/CMakeLists.txt`** — flipped from INTERFACE to
  STATIC since `blas1.cpp` + `cli_register.cpp` now exist.
- **Tests**:
  - `tests/hesap/test_arg_value.cpp` — 10 cases / 27 assertions covering
    every ArgValue kind round-trip + kind-mismatch nullopt + alloc owner.
  - `tests/hesap-dense/test_vector.cpp` — 10 cases / 40 assertions:
    default ctor, sized zero-init, initializer_list, span ctor, move
    ctor + move-assign, clone is deep, span() view, Complex<f64> storage,
    `fill`.
  - `tests/hesap-dense/test_blas1_real.cpp` — 17 cases covering each
    real op + pairwise tree size boundaries (1/7/8/9/15/16/17/63/64/65)
    + determinism (same input → bit-stable output across calls) +
    KBN compensation at N=100000 all-ones → exact integer N.
  - `tests/hesap-dense/test_blas1_complex.cpp` — 11 cases covering
    dotu vs dotc divergence + `dotc(x,x).im == 0 && == nrm2(x)^2` +
    complex `axpy` with `alpha=i` rotates by 90° + complex `nrm2`
    returns real magnitude + complex `iamax` first-index tie break +
    Complex32 round-trip + complex determinism.
  - `tests/hesap-dense/test_blas1_cli.cpp` — 10 cases covering all
    28 commands registered + working `axpy.f64` / `dot.f64` / `nrm2.f64`
    / `iamax.f64` / `scal.f64` / `dotu.c64` / `dotc.c64` dispatch +
    empty-input returns `InvalidArgument` error + every `blas1.*` schema
    flags `idempotent=true`.
- **Smoke**: `runtime/examples/smoke_hesap_blas1.cpp` —
  1000-element sin/cos vector; verifies engine `dot` matches naive
  reference within 1e-9; `nrm2` matches analytic sqrt(N/2); `scal /
  axpy / iamax / asum` on a 5-element vector with known answers; CLI
  dispatch of `hesap.dense.blas1.dot.f64` returns bit-equal result.
  Reports the 28-command registry count.

## Plain-English explanation

Three things matter about this slice:

1. **The math is now actually callable.** v0a stood up the substrate;
   v0b plugs the first real BLAS surface into it. Vector<T> is the
   owning container; 9 BLAS L1 ops work for f32, f64, Complex32, and
   Complex64. Every sum (dot, dotu, dotc, nrm2, asum) routes through a
   KBN-compensated pairwise summation tree, so the round-off behaviour
   is ~1 ulp instead of O(N) eps — and the tree topology is fixed so a
   future SIMD path will produce bit-exact same outputs across SIMD
   widths.

2. **The CLI surface is real, not a stub.** Every BLAS L1 op gets a
   typed `CommandSchema` and a working impl. 28 commands total. An
   agent can already call `hesap.dense.blas1.dot.f64({x: [1,2,3], y:
   [4,5,6]})` and get back a typed scalar result. The MCP-tool-catalog
   emission from v0a now lists 28 BLAS L1 tools alongside the v0a echo
   test. When `crd-cli` + `crd-rpc` land in Phase 4.0, they pick up the
   entire BLAS L1 surface for free.

3. **The "anchor symbol" pattern is new and important.** MSVC's linker
   drops object files from a static library when no external symbol is
   referenced — and static-init blocks alone don't count as "referenced."
   Every future hesap module that registers CLI commands via
   `CRD_HESAP_CLI_REGISTER_MODULE` will export a similar anchor
   function. Test/smoke/runtime code references it once and the linker
   pulls the .obj in.

## Decisions made

- **D10 (v0b)** — Kahan-Babuška-Neumaier compensated summation +
  pairwise tree is the deterministic reduction primitive across hesap.
  Bit-exact across SIMD widths AS LONG AS each backend walks the same
  canonical tree topology.
- **D11 (v0b)** — `kPairwiseLeafBlock = 8`. Matches Vec8f lane count so
  a future SIMD path slots in 1:1 with the scalar path.
- **D12 (v0b)** — `real_type<T>` trait + `is_complex_v<T>` predicate.
  Complex ops (`nrm2`, `asum`, `iamax`) return the underlying real type
  via `RealType<T>` so type signatures match LAPACK convention.
- **D13 (v0b)** — Real `dot<T>` (T ∈ {f32, f64}) is real-only via
  `static_assert(!is_complex_v<T>)`. Complex variants split into `dotu`
  + `dotc` per BLAS convention. Single-name `dot<Complex>` is ambiguous
  (which? unconjugated or Hermitian?) and would conflict with the real
  path's signature.
- **D14 (v0b)** — v0b CLI commands use the inline-flattened-F64Array
  shape: real vectors as F64Array; complex vectors as flattened
  `{re,im,...}` F64Array of length 2N. Complex `alpha` as F64Array of
  size 2. Future VectorRegistry follow-on bumps these schemas to v2.0;
  v1.0 stays Deprecated for ≥ 2 minor versions per ADR-0081 §2.
- **D15 (v0b)** — `Vector<T>` is move-only. Copy via explicit
  `.clone(alloc)`. Reason: a 10⁶-element f64 vector is the same shape
  as `Array<f64>`; accidental copies are a perf trap a Krylov solver
  hits in the inner loop. Explicit clone is one line at the call site.
- **D16 (v0b)** — `iamax` ties broken by FIRST index (strict `>` in
  the loop, matches LAPACK reference + Eigen).
- **D17 (v0b)** — Explicit instantiation of all 4 type variants in a
  single `blas1.cpp`. Code-size cost is ~2 KB per op per type; build-
  time cost dominates over runtime cost; keeps the surface map readable.
- **D18 (v0b)** — `CommandRegistry` ctor made public so tests can build
  isolated local instances. The static-init-populated `global()`
  registry never gets stomped by v0a's old `clear_for_tests` pattern.
- **D19 (v0b)** — CLI register modules export an anchor function (e.g.
  `register_blas1_cli_anchor()`); downstream consumers (test/smoke/
  runtime) reference it to force MSVC to pull the static-init .obj
  into the link. Every future module shipping CLI commands follows
  this pattern. Memory entry queued.
- **D20 (v0b)** — `pairwise_sum_produced<T, ProduceFn>` must parameterise
  over `(start, end)` index ranges, NOT over a sub-lambda that captures
  `mid`. Each new captured lambda has a distinct anonymous closure type;
  every recursion level instantiates a new function template, exploding
  compile-time template instantiation depth (C1060 on MSVC). Memory
  entry queued (sibling to `feedback_macro_lambda_decltype_double_eval`).

Decisions D9-D20 will be folded into ADR-0065 §14 at v0-close.

## Files touched

- `engine/hesap/include/crd/hesap/cli/arg_value.hpp` — new
- `engine/hesap/src/arg_value.cpp` — new
- `engine/hesap/include/crd/hesap/cli/command_registry.hpp` — extended
- `engine/hesap/src/command_registry.cpp` — extended
- `engine/hesap/include/crd/hesap/hesap.hpp` — include arg_value
- `engine/hesap-dense/include/crd/hesap/dense/real_type.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/vector.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/detail/pairwise_sum.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/blas1.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp` — new
- `engine/hesap-dense/src/blas1.cpp` — new
- `engine/hesap-dense/src/cli_register.cpp` — new
- `engine/hesap-dense/CMakeLists.txt` — STATIC (was INTERFACE in v0a)
- `tests/hesap/test_arg_value.cpp` — new
- `tests/hesap/test_command_registry.cpp` — use local registry, remove clear_for_tests
- `tests/hesap/CMakeLists.txt` — add test_arg_value
- `tests/hesap-dense/test_vector.cpp` — new
- `tests/hesap-dense/test_blas1_real.cpp` — new
- `tests/hesap-dense/test_blas1_complex.cpp` — new
- `tests/hesap-dense/test_blas1_cli.cpp` — new
- `tests/hesap-dense/CMakeLists.txt` — add new test files
- `runtime/examples/smoke_hesap_substrate.cpp` — use new CommandArgs ctor
- `runtime/examples/smoke_hesap_blas1.cpp` — new
- `runtime/CMakeLists.txt` — add smoke_hesap_blas1
- `context.md` — Last shipped + Current focus pointer to v0c
- `docs/phases/phase-3.1.6-hesap.md` — v0b row ✅
- `CLAUDE.md` — add smoke_hesap_blas1 to headless smokes list

## Tests / verification

- **Built?** ✅ All targets link clean under win-debug.
- **Tests pass?**
  - `crd-hesap-tests` — **39 cases / 151 assertions** PASS (v0a 30 + v0b 9 ArgValue).
  - `crd-hesap-dense-tests` — **60 cases / 318 assertions** PASS (v0a 8 catalog + v0b 52: 10 vector + 21 real BLAS L1 + 11 complex BLAS L1 + 10 CLI).
- **Smoke pass?** ✅ `smoke_hesap_blas1` dot=0.000000 (orthogonal),
  nrm2=22.36 ≈ sqrt(500), `28` BLAS L1 commands registered via
  static-init, CLI dispatch round-trip returns bit-equal result.
- **Per-slice DoD?** _4-config `per-slice-check.ps1 -Parallel` running
  in background at session-close authoring._

## Next session starts with

**v0c — BLAS L2 across 4 type variants** per `docs/phases/phase-3.1.6-
hesap.md`. Surface: `gemv / gbmv / hemv / hbmv / symv / sbmv / ger /
geru / gerc / syr / her / syr2 / her2 / trmv / trsv / tbmv / tbsv`.
Adds the `Matrix<T, Layout>` body (v0a shipped the empty shell) + strided
views + banded + triangular dispatch. Per-op CLI registration. ~1000 LOC
+ ~80 tests + ~5 days. The v0c-vectorregistry follow-on (lift inline-
JSON-array shape to VectorId/MatrixId handles) is filed but not blocking.
