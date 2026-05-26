# ADR-0084 — Sparse matrices as first-class cooked engine resources (`crd-hesap-resources`)

- **Status**: Accepted (2026-05-25)
- **Phase**: 3.1.6 `crd-hesap` v4-corpus (start of the v4 iterative cluster)
- **Tags**: arch, hesap, sparse, resources, cooker, crdr, corpus, agent-native

## Context

The v4 iterative-solver cluster benches every Krylov/preconditioner/AMG kernel
against real **SuiteSparse** matrices. Through v1–v3 those matrices were fetched
at build time (`file(DOWNLOAD)` into `_deps/suitesparse-mm`, gated by
`CRD_BUILD_HESAP_VS_REFERENCE`) and read by the benches with the in-memory
`read_matrix_market` reader — an **ad-hoc, bench-only** path that bypasses the
engine's resource system.

The user directive (2026-05-25) is that matrices be **loadable as resources from
`crd-resources`** — making the corpus a first-class engine asset, consumable by
the CLI/agents and the sandbox the same way textures/meshes are, and honouring
two pinned cornerstones at once: *authoring-text/runtime-binary* (the `.mtx` is
authoring text; the engine consumes cooked binary) and *agent-native* (an agent
loads a matrix by id through `ResourceManager`, then runs a solver).

Constraints:
- A loader that produces a `crd::hesap::sparse::SparseMatrix<T>` payload needs
  **both** `crd-resources` (CRDR/`ILoader`) and `crd-hesap-sparse`. Neither base
  module may depend on the other.
- `crd-resources` keys exactly **one `ILoader` per FourCC**, but a matrix has 4
  element-type variants (f32/f64/c32/c64).
- Cerid has **no HTTP client**; runtime download is out of scope.

## Decision

Add a one-way **bridge module `crd-hesap-resources`** (depends on
`crd-resources` + `crd-hesap-sparse` + `crd-hesap`; nothing depends on it). It
makes a sparse matrix a cooked CRDR artifact:

1. **`'HMTX'` CRDR type** with four chunks (CrdrWriter FourCC-sorts at finish →
   the loader uses `crdr_find_chunk`, never positional access):
   - `MXHD` — `MatrixFileInfo` header.
   - `MXOP` — CSR `outer_ptr` (`u32 × rows+1`).
   - `MXII` — CSR `inner_idx` (`u32 × nnz`).
   - `MXVL` — values, raw `T` bytes (`T` per the variant tag).

2. **`MatrixFileInfo` pinned at 40 bytes** (natural layout, no padding):
   `{u32 rows, u32 cols, u64 nnz, u8 variant, u8 format, u8 reserved[6],
   u64 topology_hash, u64 frame_stamp}`. `nnz` is **u64** (SuiteSparse approaches
   4G entries). `topology_hash`/`frame_stamp` are stored so the loader recomputes
   and **asserts-on-mismatch** (a free corruption detector). `format` reserves
   BSR/ELL/DIA/SELL for later (CSR = 0 today). `static_assert(sizeof == 40)`.

3. **`variant` enum pinned APPEND-ONLY** (`0=f32, 1=f64, 2=c32, 3=c64`;
   do-not-renumber). A reorder would silently mis-type every cooked `.crdr` on
   disk. New element types append at the end.

4. **Single loader, variant-in-header.** One `ILoader` for `'HMTX'`; the element
   type is read from `MXHD`. The payload (`SparseMatrixResource`) is type-erased
   (header + the always-`u32` CSR structure + raw value bytes); the consumer, who
   knows `T`, calls `build_csr<T>()` to materialise a concrete
   `SparseMatrix<T, Csr>` (asserts the variant tag matches `T`).

5. **Cooker is in-memory + cook-time** (`cook_sparse_matrix<T>` /
   `cook_matrix_market<T>`, reusing the v1g `read_matrix_market` verbatim). The
   module carries **no filesystem dependency**; file I/O lives in the
   bench/tool/sandbox layer (the cooker emits bytes, the caller persists them).

6. **Corpus delivery reuses existing machinery**: cook each matrix to a `.crdr`,
   assemble a single-entry PACK (`manifest_write`), `mount_manifest`,
   `load_sync<SparseMatrixResource>`. **No new `ResourceManager` API.**

7. **CLI `hesap.matrix.{info,cook,load}`** (stateless, on inline `.mtx` text per
   the D14 precedent). `fetch` is **dev-time** (the build-time `file(DOWNLOAD)`);
   runtime CLI works on the cooked/on-disk corpus. `info` is type-agnostic
   (structure-only); `cook`/`load` are `×{f32,f64,c32,c64}` (9 commands total).

8. **Little-endian-host CRDR posture** (matches Profile/Mesh cooked artifacts). A
   future ARM/big-endian target is a project-wide concern, not this module's.

## Consequences

- The corpus loads through the real `ResourceManager` (eviction / hot-reload /
  deterministic-replay for free); the CLI/agents and sandbox consume matrices the
  same way they consume textures/meshes.
- Measured payoff on real SuiteSparse (cooked binary vs re-parsing `.mtx`):
  **6–7× faster load** (bcsstk25 1.95 ms vs 14.76 ms) — the authoring-text /
  runtime-binary win quantified.
- The vs-reference **solver** benches that consume the corpus through this path
  land with their consumers in **v4a+** (ship-at-consumer); v4-corpus proves the
  pipeline via `smoke_hesap_matrix_resource` (real mount + `load_sync`), the
  `bench_hesap_matrix_resource_vs_reference` corpus bench, unit tests, and CLI.
- Header + variant enum are **pinned**; changes are append-only per the
  ADR-0081 §2 schema-versioning posture. `loader_version` participates in the
  cooker's incremental key.

## Alternatives considered

- **Runtime path-read shim** (wrap `read_matrix_market` on a downloaded `.mtx`,
  no cook/FourCC/`ResourceManager`): lighter, but bypasses
  authoring-text/runtime-binary and isn't a true engine resource. Rejected
  (user chose first-class cooked).
- **Four FourCCs (one per variant)**: forces four near-identical loaders.
  Rejected for single-loader + variant-in-header.
- **Typed `SparseMatrix<T>` payload**: impossible — `ILoader` is non-template;
  the type is only known at the consumer. Resolved by the type-erased payload +
  `build_csr<T>()`.
