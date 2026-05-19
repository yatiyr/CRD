# Session 2026-05-19 — Phase 3.1.6 `crd-hesap` v0a substrate scaffolding

## Goal

Kick off Phase 3.1.6 `crd-hesap` — the MATLAB-class numerical substrate locked
2026-05-19 as "elite-and-big, ship the full v0-v17 elite scope before eylem
v1c-resume" (ROADMAP § Strategic Execution Plan Revision 2026-05-19, ADR-0065
§13). v0a ships the substrate scaffolding shared by every later slice:
`Complex<T>` value type, `LinearOp<T>` matrix-free interface, `MatrixId` /
`VectorId` opaque handles, `'HDV0'` CRDR FourCC pin, the dense-matrix-type
catalog headers (15 shells), and the **CLI protocol plumbing** (command
schema + result + registry + JSON writer + MCP-tool-descriptor emit) per
ADR-0081 §10 (every new slice ships CLI from v0).

## What we built / changed

- **`engine/hesap/`** — new substrate module.
  - `complex.hpp` — `Complex<T>` value type, f32 + f64 specialisations.
    Smith 1962 robust division (avoids overflow when one component is near
    the type's range bound); `abs` via `std::hypot`; `arg` via `std::atan2`.
    `sizeof(Complex<f32>) == 8`, `sizeof(Complex<f64>) == 16`, both
    trivially copyable. Scalar in v0a; SIMD bit-exact parity arrives v0b.
  - `linear_op.hpp` — `LinearOp<T>` matrix-free interface.
    Vtable shape LOCKED: 3 pure (`apply` / `n_rows` / `n_cols`) + 2 virtual-
    with-default (`apply_transpose` / `apply_adjoint`) returning false.
    PETSc / Trilinos pattern — matrix-free preconditioners (SPAI,
    polynomial) don't need transpose; Krylov solvers query
    `has_transpose()` / `has_adjoint()` at runtime. **New virtuals append
    at END forever** per `feedback_vtable_stability_append_at_end`.
  - `handles.hpp` — `MatrixId` + `VectorId` opaque 64-bit POD handles,
    `[generation:32 | index:32]` layout matching `scene::EntityId`.
    `DefaultHash` specialisations so they can key into `HashMap`.
  - `crdr_format.hpp` — `kHesapDenseFourCC = 'HDV0'` (0x30564448) pin
    for the v0f load/save round-trip.
  - `cli/command_schema.hpp` — typed `CommandSchema` declaration: name +
    description + version + params + output + capabilities + deprecation.
    `ParamKind` covers Bool / I32 / I64 / U32 / U64 / F32 / F64 / Complex32
    / Complex64 / String / Enum / MatrixId / VectorId / EntityId / Path.
    `SchemaVersion {major, minor}` per ADR-0081 §2.
  - `cli/command_result.hpp` — `CommandResult` typed return shape.
    `std::variant<ResultVoid, ResultScalarF64, ResultText, ResultTable,
    ResultMatrixId, ResultVectorId, ResultBinaryBlob, ResultError>`.
    `std::variant` allowed per advisor: value type, not heap-owning STL.
  - `cli/command_registry.hpp` + `src/command_registry.cpp` —
    `CommandRegistry::global()` Meyers singleton (defeats static-init-
    order fiasco). `register_command` / `find` / `all` / `size` /
    `clear_for_tests`. `CRD_HESAP_CLI_REGISTER_MODULE(lambda)` static-init
    macro — uses `auto` + `make_module_registrar<Init>` helper so the
    lambda type is materialised exactly once (MSVC bug encountered: naive
    `decltype(lambda)` in template-argument position evaluates the lambda
    expression twice, producing two distinct closure types).
  - `cli/json_writer.hpp` + `src/json_writer.cpp` — minimal write-only
    JSON emitter (~250 LOC). RFC 8259 string escaping; compact + pretty
    modes; fixed-depth stack (kMaxDepth = 32). Used by both
    `meta.export-mcp-tools` and the future JSON-RPC layer.
  - `cli/mcp_descriptor.hpp` + `src/mcp_descriptor.cpp` — emits an MCP
    tool-descriptor JSON object per command (`name`, `description`,
    `version`, `inputSchema` with `properties` + `required`,
    `outputSchema`, `capabilities`, `deprecation`). Canonical MCP-spec
    fields appear unchanged; Cerid-specific fields live alongside (MCP
    clients ignore unknown keys harmlessly) per ADR-0081 §6.4.
  - `hesap.hpp` — umbrella header.
- **`engine/hesap-dense/`** — new module with matrix-type catalog HEADERS
  (~15 dense type shells). Allocator-aware ctor + `rows()` / `cols()` /
  `is_square()` accessors. Bodies arrive v0b-v0e. Catalog: `Matrix<T, L>`
  / `MatrixView<T, L>` / `Diagonal<T>` / `Identity<T>` / `Permutation` /
  `Triangular<T, Side, Diag>` / `Symmetric<T>` / `Hermitian<T>` /
  `Banded<T>` / `BlockDiagonal<T>` / `BlockTridiagonal<T>` / `Toeplitz<T>`
  / `Hankel<T>` / `Circulant<T>` / `Vandermonde<T>`. INTERFACE library
  for v0a; v0b's first .cpp flips it to STATIC.
- **Sparse catalog** — DEFERRED to v1 per the v0a kickoff decision
  (cleaner separation than scattering empty shells across v0a + v1).
- **Tests**: `tests/hesap/` 30 cases / 121 assertions + `tests/hesap-dense/`
  8 cases / 30 assertions. ASCII-only `TEST_CASE` names. Smith division
  branch coverage (|re|≥|im| and |im|>|re|), conjugate involution +
  `z * conj(z) == |z|²`, identity-LinearOp round-trip, default-
  transpose/adjoint returns false, MatrixId/VectorId HashMap-keying,
  `'HDV0'` byte-layout, JsonWriter escape correctness, MCP-canonical
  fields presence + Cerid-extension fields presence.
- **Smoke**: `smoke_hesap_substrate` — registers `hesap.smoke.echo` via
  the static-init macro, invokes it, dumps the MCP tool-descriptor JSON
  to stdout. End-to-end protocol-plumbing verification.

## Plain-English explanation

What we shipped is the **scaffolding** that every later hesap slice (BLAS
L1 / L2 / L3, dense direct solvers, sparse, iterative, eig, opt, ODE, FFT,
DSP, stats, autodiff) will lean on. Three pieces in particular are load-
bearing:

1. **`Complex<T>` is Cerid-native.** Per the tak-çıkar principle, we don't
   wrap `std::complex`. The Cerid type is its own header with documented
   robust algorithms (Smith division, `hypot` abs, `atan2` arg) so when SIMD
   parity arrives in v0b, the algorithm shape doesn't change.

2. **`LinearOp<T>` is the matrix-free interface every Krylov solver
   eventually consumes.** The vtable shape locks today: 3 pure + 2
   virtual-default. The "virtual-default returns false" pattern means
   SPAI / polynomial / matrix-free preconditioners don't have to fake a
   transpose; GMRES / BiCGSTAB query `has_transpose()` at runtime.

3. **CLI protocol plumbing ships from v0** per ADR-0081 §10 + the
   strategic pivot 2026-05-19. From now on, every hesap slice registers
   typed `CommandSchema`s alongside the C++ API. The parser / REPL /
   JSON-RPC server lands later in Phase 4.0 `crd-cli` — but when it
   arrives it inherits the entire hesap surface for free because every
   slice already paid the schema-registration cost. The smoke proves
   the loop end-to-end: a fake `hesap.smoke.echo` command registered
   via the static-init macro, looked up by name from the registry,
   dispatched, emitting valid MCP-tool-descriptor JSON.

## Decisions made

- **D1 (v0a)** — `LinearOp<T>` vtable shape: 3 pure (`apply` / `n_rows` /
  `n_cols`) + 2 virtual-with-default (`apply_transpose` / `apply_adjoint`)
  returning `false`. PETSc / Trilinos pattern. Per `feedback_vtable_stability_
  append_at_end`, new virtuals append AT END forever.
- **D2 (v0a)** — `Complex<T>` is Cerid-native (per tak-çıkar in
  PRINCIPLES.md), not a wrap of `std::complex`. Smith 1962 division,
  `std::hypot` abs, `std::atan2` arg. f32 + f64 specialisations from v0.
- **D3 (v0a)** — `CommandRegistry` is a Meyers singleton (advisor
  2026-05-19; ADR-0081 §Consequences flags static-init-order fiasco as a
  known pitfall). The `CRD_HESAP_CLI_REGISTER_MODULE` macro uses
  `auto + make_module_registrar<Init>` so the lambda literal is
  materialised exactly once — naive `decltype(lambda)` in template-
  argument position evaluates the lambda expression twice on MSVC,
  producing distinct closure types and a deduction failure.
- **D4 (v0a)** — Hand-roll a minimal JSON writer in v0a (not pull a
  dependency). ~250 LOC; shared backbone for `meta.export-mcp-tools` +
  the future JSON-RPC layer in Phase 4.0.
- **D5 (v0a)** — `std::variant` is allowed for `CommandResult`. Value
  type, no heap-owning STL leaks; alternatives that need allocation
  (Text / TableRows / BinaryBlob) wrap `crd::containers::String` /
  `Array<u8>`.
- **D6 (v0a)** — Sparse-format catalog DEFERRED to v1. Cleaner separation
  than scattering empty shells across two slices.
- **D7 (v0a)** — `engine/hesap-dense/` ships as an `INTERFACE` library
  in v0a (header-only shells). v0b's first BLAS L1 `.cpp` flips it to
  `STATIC`.
- **D8 (v0a)** — `'HDV0'` (0x30564448) pins the v0 dense on-disk FourCC.
  Load/save lands v0f.
- **D9 (v0a)** — Per-slice DoD ran 4-config (no `-IncludeRelease`). Rationale:
  v0a introduces a fresh interface (`LinearOp<T>`) — `feedback_vtable_
  stability_append_at_end` documents the LTCG mis-dispatch failure mode that
  arises when virtuals are inserted into the MIDDLE of an existing vtable.
  Fresh-interface case is not in scope for that failure mode; win-shipping
  already provides LTCG coverage and passed. `-IncludeRelease` re-enables
  for any slice that modifies `LinearOp<T>` (or any other public interface)
  after v0a.

These will be folded into ADR-0065 §14 at v0-close (2-day slice at the end
of the v0a-f cluster); §14 is reserved for the v0a-f decision lock as the
phase doc specifies.

## Files touched

- `engine/hesap/CMakeLists.txt` — new
- `engine/hesap/include/crd/hesap/hesap.hpp` — umbrella
- `engine/hesap/include/crd/hesap/complex.hpp`
- `engine/hesap/include/crd/hesap/linear_op.hpp`
- `engine/hesap/include/crd/hesap/handles.hpp`
- `engine/hesap/include/crd/hesap/crdr_format.hpp`
- `engine/hesap/include/crd/hesap/cli/command_schema.hpp`
- `engine/hesap/include/crd/hesap/cli/command_result.hpp`
- `engine/hesap/include/crd/hesap/cli/command_registry.hpp`
- `engine/hesap/include/crd/hesap/cli/json_writer.hpp`
- `engine/hesap/include/crd/hesap/cli/mcp_descriptor.hpp`
- `engine/hesap/src/json_writer.cpp`
- `engine/hesap/src/command_registry.cpp`
- `engine/hesap/src/mcp_descriptor.cpp`
- `engine/hesap-dense/CMakeLists.txt` — new
- `engine/hesap-dense/include/crd/hesap/dense/dense.hpp` — umbrella
- `engine/hesap-dense/include/crd/hesap/dense/matrix_catalog.hpp`
- `tests/hesap/CMakeLists.txt`, `tests/hesap/test_*.cpp` (8 files)
- `tests/hesap-dense/CMakeLists.txt`, `tests/hesap-dense/test_matrix_catalog.cpp`
- `runtime/examples/smoke_hesap_substrate.cpp`
- `CMakeLists.txt` (root) — `add_subdirectory(engine/hesap)`,
  `add_subdirectory(engine/hesap-dense)`
- `tests/CMakeLists.txt` — `add_subdirectory(hesap)`, `add_subdirectory(hesap-dense)`
- `runtime/CMakeLists.txt` — `smoke_hesap_substrate` exe + link list
- `context.md` — Last shipped + Current focus pointer to v0b
- `docs/phases/phase-3.1.6-hesap.md` — v0a row ✅

## Tests / verification

- **Built?** ✅ `crd-hesap` + `crd-hesap-dense` + `crd-hesap-tests` +
  `crd-hesap-dense-tests` + `smoke_hesap_substrate` all link clean
  under win-debug.
- **Tests pass?**
  - `crd-hesap-tests` — **30 cases / 121 assertions** PASS.
  - `crd-hesap-dense-tests` — **8 cases / 30 assertions** PASS.
- **Smoke pass?** ✅ `smoke_hesap_substrate` dumps a valid 1-tool MCP
  catalog JSON to stdout: `[{"name":"hesap.smoke.echo",...}]`.
- **Per-slice DoD?** ✅ **4-config PASS in 0:35** via `scripts/per-slice-check.ps1
  -Parallel` (win-debug + win-asan + win-shipping + win-tidy all green;
  no `-IncludeRelease` per D9 above). ctest discovery confirmed: 38 hesap
  test entries registered at ctest IDs #1719-#1756 (Catch2 registers
  `TEST_CASE` strings as ctest names — they don't contain the word "hesap"
  so `ctest -R hesap` misses them; `ctest -N` shows them by `Complex` /
  `LinearOp` / `JsonWriter` / `CommandRegistry` / `Matrix` prefixes).

## Next session starts with

**v0b — BLAS L1 across 4 type variants** per `docs/phases/phase-3.1.6-
hesap.md`. Concrete first deliverable: `engine/hesap-dense/`'s first
`.cpp` (flips it from INTERFACE to STATIC). BLAS L1 ops:
`axpy / dot{u,c} / nrm2 / scal / copy / swap / asum / iamax` for
f32 / f64 / Complex32 / Complex64. Kahan-pairwise reduction tree for
sums (deterministic across SIMD widths per ADR-0063). Each op registers
a `CommandSchema` via `CRD_HESAP_CLI_REGISTER_MODULE`. Target: ~700 LOC
engine + ~80 tests + ~3 days.
