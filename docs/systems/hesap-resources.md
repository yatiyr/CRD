# crd-hesap-resources — sparse matrices as cooked engine resources

> One-line purpose: bridge `crd-resources` ↔ `crd-hesap-sparse` so a sparse
> matrix is a first-class cooked CRDR artifact (`'HMTX'`), loadable through
> `ResourceManager` like a texture or mesh. Phase 3.1.6 v4-corpus; ADR-0084.

## Why it exists

The v4 iterative-solver cluster benches against real **SuiteSparse** matrices.
This module makes those matrices first-class engine resources — cooked binary at
build/cook time, fast-loaded at runtime — instead of an ad-hoc bench-only
`.mtx` read. It honours *authoring-text/runtime-binary* (the `.mtx` is authoring
text; the engine consumes cooked binary CSR) and *agent-native* (an agent loads
a matrix by id, then runs a solver) at once.

**Dependency direction (one-way):** depends on `crd-resources` (CRDR/`ILoader`)
+ `crd-hesap-sparse` (`SparseMatrix`) + `crd-hesap` (CLI). Nothing depends on it.

## Public surface

- **`'HMTX'` CRDR** with chunks `MXHD` (header) · `MXOP` (CSR `outer_ptr`) ·
  `MXII` (CSR `inner_idx`) · `MXVL` (values, raw `T` bytes). Loader uses
  `crdr_find_chunk` (order-agnostic).
- **`MatrixFileInfo`** — pinned 40-byte header `{u32 rows, u32 cols, u64 nnz,
  u8 variant, u8 format, u8 reserved[6], u64 topology_hash, u64 frame_stamp}`.
- **`MatrixVariant`** — pinned APPEND-ONLY enum `{F32=0, F64=1, C32=2, C64=3}`.
- **`SparseMatrixResource`** — type-erased payload (header + CSR structure +
  value bytes). `build_csr<T>(alloc)` materialises a `SparseMatrix<T, Csr>`
  (asserts variant↔`T` + recomputed-topology-hash match = corruption detector).
- **Cooker** (`matrix_artifact_builder.hpp`, header-only): `cook_sparse_matrix<T>`
  and `cook_matrix_market<T>` (reuses v1g `read_matrix_market`). In-memory,
  cook-time; **no filesystem dependency** (the caller persists the bytes).
- **Loader** (`matrix_resource_loader.hpp`): `read_matrix_resource(bytes, out,
  scratch)` (free fn, testable without `ResourceManager`) +
  `register_hesap_matrix_loader(rm)` + anchor symbols.
- **CLI** `hesap.matrix.{info, cook.<T>, load.<T>}` (9 commands; stateless on
  inline `.mtx` text per D14). `info` is type-agnostic. `fetch` is **dev-time**
  (the build-time `file(DOWNLOAD)`; Cerid has no HTTP client).

## Integration notes

- **Corpus delivery** reuses existing CRDR machinery: cook → single-entry PACK
  (`manifest_write`) → `mount_manifest` → `load_sync<SparseMatrixResource>`.
  No new `ResourceManager` API. See `smoke_hesap_matrix_resource`.
- **Little-endian-host** CRDR posture (matches Profile/Mesh artifacts).
- **Measured payoff:** cooked binary load is 6–7× faster than re-parsing the
  `.mtx` on real SuiteSparse (`bench_hesap_matrix_resource_vs_reference`).
- The vs-reference **solver** benches through this path land with their
  consumers in v4a+ (ship-at-consumer).

## Tests / smokes / benches

- `tests/hesap-resources/test_matrix_resource.cpp` — cook+load byte-exact
  round-trip (f32/f64/c32/c64), MM-text cook, pinned-header static asserts,
  malformed rejection, and the `hesap.matrix.{info,cook,load}` CLI.
- `smoke_hesap_matrix_resource` — the real `ResourceManager` mount + `load_sync`
  path on a cooked matrix.
- `bench_hesap_matrix_resource_vs_reference` (gated) — real SuiteSparse matrices
  through cook+load, timed vs a direct read, each verified via mount+`load_sync`.
