# crd-hesap-sparse

Elite, multi-threaded, deterministic sparse-matrix substrate. **Goal: beat
Eigen's `SparseCore`** (single-threaded, scalar spmv, SPA/`AmbiVector`
spgemm) on the kernels that matter — spmv (SELL-C-σ + AVX + `crd-jobs`
row-balanced parallel) and spgemm (parallel hash accumulator). Benched
head-to-head vs **Eigen 3.4** on the **SuiteSparse `.mtx` corpus**, gated
behind `CRD_BUILD_HESAP_VS_REFERENCE=ON`.

> Status: **v1a in progress** — storage + kernels cluster. This doc grows
> one section per subslice. v1a-1 (substrate) shipped the trinity + handles +
> format tag + topology hash + `'HSPM'` pin; v1a-2 (COO `TripletBuilder` →
> CSR `compress` + uncompressed insert mode + 12 CLI); v1a-3 (CSC + structural
> queries + smoke + assembly bench vs Eigen + 7 CLI).

## The trinity (the heart)

Structure, values, and the analysis plan are three decoupled objects so a
symbolic plan (ordering / exec-format choice / scratch layout) can be cached
across many value frames while the structure is unchanged:

- **`SparsePattern`** — `rows`, `cols`, `format` (`SparseFormat`),
  `block_size`, `outer_ptr`, `inner_idx`, cached `topology_hash`.
  Compressed-only in v1a-1 (one `outer_ptr` + tight `inner_idx`); the
  Eigen-style uncompressed/incremental form arrives with `TripletBuilder`
  in v1a-2.
- **`SparseValues<T>`** — flat `Array<T>` parallel to `inner_idx`, plus an
  opaque monotonic `frame_stamp`. The *consumer* bumps `frame_stamp` on
  value mutation; the library only reads it as a cache-invalidation hint.
  Raw `T` per ADR-0078 §5 (hesap is the numerical-kernel layer; the typed
  `Quantity` boundary lives where the engine consumes hesap).
- **`AnalysisHandle`** — cached `topology_hash` + recommended exec format +
  reserved `native` plan pointer. `is_valid_for(pattern)` is true iff its
  cached hash (non-zero) equals the pattern's current hash.

`SparseMatrix<T, Format>` owns one trinity (move-only, D15). `Format` is a
compile-time NTTP (D21 dense-Layout precedent); the runtime `pattern.format`
mirrors it for the analysis cache + v1c conversion graph. For sparse the
orientation IS the format (CSR row-oriented, CSC column-oriented), so there
is no separate `Layout` parameter.

## Determinism spec (ADR-0063) — pinned at v1a-1

Pinned up front so later kernels inherit it rather than discovering it:

1. **`topology_hash` is bit-reproducible across platforms.** FNV-1a-64 over
   `rows`, `cols`, `format`, `block_size`, then the length-prefixed *used*
   bytes of `outer_ptr` and `inner_idx`. Multi-byte integers are fed
   **little-endian via explicit byte shifts** (never `memcpy`), so a
   big-endian host produces the identical value. `format` + `block_size`
   are included so a CSR and a CSC pattern with identical index arrays —
   different matrices — hash differently. Only logical content is hashed;
   `Array` spare capacity is irrelevant. (Decision **D1**.)
2. **Row-parallel spmv = disjoint-row ownership.** Each worker owns a
   contiguous slice of output rows; no two workers write the same `y[i]`,
   so there is no order-dependent float reduction across threads. (v1b.)
3. **spgemm numeric output is canonical column-sorted before write.** The
   per-row private hash accumulator is sorted by column index before being
   emitted, so the result is identical regardless of accumulation order or
   thread count. (v1d.)
4. **No fixed-order-free cross-thread float reductions** anywhere. Any
   reduction that crosses threads uses a deterministic tree / disjoint
   partition, matching the dense KBN-pairwise contract.

## spgemm (v1d)

C = A·B via **Gustavson with a per-row dense SPA accumulator**. Serial
(`spgemm`) is a fused single pass (O(flops); stamp-marker SPA-clear, no
per-row memset). Parallel (`spgemm_parallel`) is two-phase — symbolic
distinct-column count → prefix-sum `outer_ptr` → numeric accumulate + sorted
write into disjoint row slices — over a **flop-balanced** A-row partition
(`flop[i]=Σ nnz(B[k])`); per-worker SPA scratch (sized by `num_workers`,
indexed by `worker_index`; phase-2 stamps offset by +m to reuse the marker
across passes). Each C row is produced by one worker → **bit-exact vs serial
at any worker / job count**. `kMaxSpaCols` caps the dense SPA (hash
accumulator is a future refinement above that). Deterministic: fixed
A-row/B-row accumulation order + column-sorted output.

**Shootout vs Eigen** (C=A·A, win-release, `Eigen::setNbThreads(1)` — Eigen
does NOT multi-thread sparse×sparse; best-of-7) on **real SuiteSparse
matrices** (fetched at configure via `file(DOWNLOAD)`, gated
`CRD_BUILD_HESAP_VS_REFERENCE`; minimal bench-local Matrix-Market reader —
engine MM I/O + writer + CLI are v1g):

| matrix | nnz(A) | Cerid par | Eigen-ST | par/Eigen |
|---|---|---|---|---|
| bcsstk13 (FEM) | 84k | 3.06 ms | 8.74 ms | **2.86×** |
| bcsstk24 (FEM) | 160k | 3.37 ms | 9.49 ms | **2.81×** |
| bcsstk25 (FEM) | 252k | 7.14 ms | 18.81 ms | **2.63×** |
| gemat11 (power) | 33k | 1.63 ms | 3.28 ms | **2.02×** |
| sherman3 (reservoir) | 20k | 0.78 ms | 0.93 ms | **1.19×** |
| west2021 (circuit) | 7k | 0.35 ms | 0.30 ms | 0.92× |

**Median par/Eigen = 2.32×, 5/6 WIN** — gate (median ≥ 1.3×) crushed. Our
*serial* Gustavson already ties Eigen-ST on the FEM matrices; row-parallelism
(Eigen is single-threaded) lands the ~3×. Only the tiny 7k-nnz west2021 loses
(0.92×, sub-ms small-input regime where Eigen's constant factor wins by
0.03 ms) — user-accepted (the v1 anti-rabbit-hole edge). The fairness note:
Eigen `ec = ea*ea` (NOT `.pruned()`, which added an unfair filter pass).

## spmm + SDDMM (v1e)

**spmm** — `C = α·A·B + β·C`, A compressed CSR (m×k), **B (k×r) and C (m×r)
dense row-major** with leading dimensions `ldb`/`ldc` (≥ r) so a caller can
spmm into a strided block of a larger matrix (block-Krylov / batched solve).
Each A-row is scanned **once** and its scaled B-row is axpy'd into all r
columns of `C[i,:]` (contiguous, auto-vectorised). The C row is initialised
once (0 for β=0 → NaN-safe; else β·C[i,:]) then accumulated in place — no
per-row temp. Serial (`spmm`) + nnz-balanced row-parallel (`spmm_parallel`,
boundaries computed once, each job writes disjoint C rows) → **bit-exact vs
serial at any job count**. f32/f64/c32/c64.

Honest framing: Eigen's sparse×dense is *also* one-pass-over-A, so the opening
is **row-parallelism** (Eigen-MT is the meaningful gate), not multi-RHS reuse.
**Shootout vs Eigen on the v1d SuiteSparse matrices, r∈{1,4,32,128}**: crushes
Eigen-ST **5–12×** throughout; vs Eigen-MT wins the heavy-RHS regime (**r=32
1.32–1.33×, r=128 1.79–1.94×**) and ties small-RHS (gemat11 r=4 **0.96×**,
bcsstk25 r=1 0.93×) — small-r collapses to the spmv B-gather bandwidth wall
(the user-accepted regime; a per-job binary-search partition was tried and
*removed* after it regressed small-r). "Chased to the wall," user-accepted.

**SDDMM** — `C = α·sample(X·Yᵀ, M)`: given a sparse mask M (only its **pattern**
is used), X (m×r) and Y (n×r) dense row-major, `C[i,j] = α·dot(X[i,:], Y[j,:])`
for each (i,j) ∈ pattern(M). C carries M's exact pattern; **only the masked dots
are computed** — the dense X·Yᵀ is never formed. Each output entry is one
fixed-order length-r dot; entries are independent → row-parallel writes hit
disjoint output slots → **bit-exact vs serial at any worker count**. Complex is
non-conjugating (plain X·Yᵀ). The dot is explicit SIMD with multiple lane
accumulators (Vec4d×2 / Vec8f, two-rounded mul+add — D(sparse)-3; reduced via a
store-then-scalar `horizontal_sum` so AVX2 and the scalar fallback are bit-exact
across SIMD widths) to break the FP-add latency chain and keep the random Y-row
gathers in flight.

**No single Eigen SDDMM op exists**, so the bench (`bench_hesap_sddmm_vs_reference`,
gated) compares against the two ways an Eigen user would compute it, on real
SuiteSparse masks (X,Y dense n×r, r∈{8,16,32}):

- **vs dense-then-mask** (form the full dense `X·Yᵀ`, O(n²r), then sample): we
  compute only nnz(M) dots → **26–434× faster** (the structural win — the point).
- **vs same-flops per-entry `X.row(i).dot(Y.row(j))`** (the hardest kernel race,
  identical FLOPs): **compute-bound matrices win** (gemat11 r=32 **1.31×**,
  sherman3 r=32 **1.49×**); the **high-nnz FEM matrix bcsstk24 (168k entries)
  trails at 0.62–0.76×** — provably the random Y-row gather/cache wall (its
  parallel barely scales, ~1.4×; swapping to single-rounded `fma`, half the FP
  ops, moves it 0% → not a compute/determinism tax, the memory wall). Same wall
  spmv/spmm hit when gather-bound; beating it needs ASpT-class column-reordering
  (a future perf-attack, not v1). User-accepted at the wall.

CLI: `hesap.sparse.spmm`/`sddmm` × {f32,f64,c32,c64} (dense operands passed as
F64Array, complex flattened {re,im,…}; spmm returns dense C, sddmm returns C
values at the mask pattern).

## Block + structured formats: BSR / ELL / DIA (v1f)

Three standalone format structs (like `SellMatrix`), each with serial + parallel
spmv (two-rounded, deterministic) and CSR↔X convert; f32/f64/c32/c64; 36 CLI
commands. All beat Eigen scalar-CSR decisively on their native patterns (Eigen
has no first-class BSR, so the honest gate is "same matrix, Eigen as scalar CSR").

**BSR** (`BsrMatrix<T>`, CSR-of-blocks, dense b×b row-major) — the FEM/physics
format. Block-spmv reads each x-block once and reuses it across the b output
rows (vs scalar CSR's b² x-gathers), carries b independent accumulators (ILP),
stores one block-column index per b² values. **D(sparse)-6:** the block GEMV is
a dedicated fully-unrolled small-block kernel (compile-time b∈{1,2,3,4,6} +
runtime fallback), NOT the hesap-dense v0d GEMM microkernel — that leaf is sized
for large-N tiling and its prologue dominates a 3×3 block. **D(sparse)-7:**
CSR→BSR zero-pads partial blocks (a block is dense by definition); BSR→CSR emits
all stored block entries. **Gate:** parallel BSR spmv **3.45–6.72× Eigen-CSR /
3.20–6.46× our scalar-CSR**; even *serial* BSR beats Eigen 1.8–2.0×.

**ELL** (`EllMatrix<T>`, slot-major, global max-row-length padding; pad val 0) —
the **interop / base** regular format. CONTRACT (pinned v1f-2): ELL is NOT the
irregular-matrix perf path — that is SELL-C-σ (v1b), which pads per-slice and
σ-sorts. ELL pays full global padding by design; use it for genuinely regular
patterns + interchange. spmv vectorises over rows; per-row reduction is in
ascending-slot == CSR column order (bit-exact with CSR). **Gate:** **4.9–5.2×
Eigen-CSR / 4.4–5.4× our CSR** on uniform patterns.

**DIA** (`DiaMatrix<T>`, diagonal-major, ascending offsets) — the
banded/structured-grid format. The column-index array vanishes (the offset is
shared across a whole diagonal) and the spmv's `x[i+off]` access is contiguous
in i → streams + vectorises. α is applied per-term (`y = βy + Σ_d α·d_i·x`), so
DIA-vs-CSR matches within tolerance while parallel-vs-serial stays bit-exact.
Wasteful for unstructured sparsity (a near-empty diagonal still costs `rows`).
**Gate:** **4.8–5.9× Eigen-CSR / 3.7–5.1× our CSR** on banded patterns.

## Element-wise + structural + conversions (v1c)

All on compressed CSR, all `[[nodiscard]]` factories returning a new matrix,
all deterministic (canonical column-sorted, bit-exact across runs), 4 type
variants.

- **Conversions** (`convert.hpp`): `to_csc`(CSR→CSC), `from_csc`(CSC→CSR),
  `transpose`(CSR→CSR Aᵀ), `to_coo` — `to_csc`/`from_csc`/`transpose` share one
  `organize_by_inner` kernel (count + prefix + ordered scatter = real O(nnz)
  build; `transpose∘transpose == A` byte-exact). Conversion hub is CSR; BSR/
  ELL/DIA edges + a runtime `convert(from,to)` dispatch land at v1f when the
  format count (8+ edges) justifies the table.
- **Element-wise** (`element_wise.hpp`): `add`/`subtract` (D(sparse)-5: matched
  columns `a OP b` left-first, single-rounded; `topology_hash`-equal fast path,
  symbolic-union sorted merge otherwise), `scale`(αA), `hadamard`(A.*B,
  intersection).
- **Structural** (`structural.hpp`): `extract_diagonal` (dense), `scale_rows`
  (diagonal left-scale), `triu`/`tril` (k-diagonal offset), `submatrix`
  (reindexed block; row/col slices are special cases).

## On-disk

`'HSPM'` (Hesap SParse Matrix) CRDR FourCC, little-endian (ADR-0037), pinned
in `crd-hesap`'s `crdr_format.hpp` next to dense's `'HDV0'`. Matrix-Market
round-trip + cooked sparse bundles land in v1g.

## Assembly (v1a-2/v1a-3)

`TripletBuilder<T>` (COO) accumulates `(row,col,value)` triplets in any order;
`compress()` → canonical CSR, `compress_csc()` → canonical CSC. Algorithm:
counting-sort grouping by outer vector (stable, preserves insertion order
within an outer) → per-outer stable sort by inner index → dedup-merge
(duplicate `(outer,inner)` summed left-to-right in insertion order =
deterministic, bit-reproducible). Single counting pass preallocates exact
storage (PETSc/Eigen assembly lever — no incremental reallocation).

`SparseMatrix<T,Format>` also supports the **Eigen 4-array uncompressed mode**
for incremental build: `make_uncompressed(rows,cols,slack)` → `coeff_ref(r,c)`
find-or-insert (keeps columns sorted, grows storage when an inner vector fills)
→ `make_compressed()` (compacts slack; recomputes the topology hash). The
canonical slack-invariant hash means an uncompressed matrix and its compressed
form share an identity.

Structural inspectors (`queries.hpp`, type-independent): `structural_stats`
(rows/cols/nnz/density/n_outer/min+max inner nnz/is_compressed) and
`inner_indices(m,k)` (the sorted indices of inner vector k).

### spmv (v1b)

CSR scalar baseline (`spmv`, deterministic L-to-R two-rounded) is the
reference. **SELL-C-σ (`spmv_sell`) is the SIMD primary** and is bit-exact
with the baseline (2509 assertions across f32/f64/complex). The f64 kernel:
slice-pair interleaving (two independent accumulator chains for OoO overlap) +
hardware `_mm256_i32gather_pd` gather + prefetch + two-rounded `mul + add`
(NOT `_mm256_fmadd`, per D(sparse)-3).

**Parallel** (`spmv_sell_parallel`, `sell_parallel.hpp`): nnz-balanced slice
ranges over `crd::jobs`; disjoint ranges write disjoint original rows (perm is
a bijection) → no cross-thread writes → **bit-exact with serial at any worker
/ job count** (27 260 assertions). σ row-length sort (`to_sell`, default global)
groups similar-length rows so short rows don't pad up to a long slice-mate
(fixes power-law); for uniform/banded the sort is the identity permutation and
a fast path skips the `perm[]` indirection entirely.

**Shootout vs Eigen** (win-release, i9-14900K, `Eigen::setNbThreads(1)` for ST
/ default for MT — Eigen's spmv is multi-threaded by default, the `v0e-g`
fairness trap; best-of-15 + warmup, f64). **The result is regime-dependent and
honest:**

| pattern | ST N=2M | MT N=2M | ST N=1M | MT N=1M |
|---|---|---|---|---|
| uniform-16 | **1.21×** | **1.05×** | 0.92× | ~1.0× |
| uniform-4  | **1.27×** | ~1.0× | **1.90×** | ~0.9× |
| banded-5   | **1.24×** | **1.35×** | **1.16×** | **1.13×** |
| power-law  | **1.27×** | ~0.9× | 0.96× | ~0.9× |

**SELL wins decisively in the memory-latency-bound regime** (large matrices
whose x-gather spills L3 — the regime that matters for "sparse = big"): ST
**1.21–1.27×** across the board at N=2M. When **cache-resident** (N=1M, x fits
L3) Eigen's leaner scalar row-dot wins on the regular patterns (uniform-16
0.92×) — our per-slice/gather code overhead shows when memory isn't the wall.
**MT is DRAM-bandwidth-bound** — both kernels saturate the bus, so it is
tie-territory (we win banded + uniform-16@2M; ±10% run-to-run noise elsewhere).

This is the physics-bounded elite result (user call 2026-05-20): **dominate the
large/DRAM-bound regime, stay competitive at the cache + bandwidth walls.** The
v1b gate is **SELL ≥ Eigen in the DRAM-bound regime (ST) + parity-or-better vs
Eigen-MT** (replacing the original ≥60% STREAM-triad proxy — beating
multi-threaded Eigen is the stronger statement). v1b-2 absorbed v1b-3 (parallel)
into one slice.

### Assembly benchmark vs Eigen

`bench_hesap_sparse_assembly_vs_reference` (gated `CRD_BUILD_HESAP_VS_REFERENCE`)
times Cerid `TripletBuilder::compress()` against `Eigen::setFromTriplets`,
both from the same coordinate list (win-release, i9-class, best-of-3, f64):

| N | nnz/row | Cerid | Eigen | speedup |
|---|---|---|---|---|
| 50 000 | 16 | 12.84 ms | 13.21 ms | **1.03× WIN** |
| 200 000 | 16 | 52.13 ms | 74.97 ms | **1.44× WIN** |
| 1 000 000 | 8 | 122.13 ms | 214.42 ms | **1.76× WIN** |

**Beats Eigen at every size.** `assemble` scatters directly into the final
arrays (no intermediate AoS), sorts each inner vector in place (insertion sort
for small vectors; reused merge-sort scratch for large ones — dense-row
robustness preserved), and dedup-compacts in place. The two fully-scattered
arrays use `Array::resize_uninitialized` to skip the zero-init pass. The hard
sparse perf gates begin at v1b (spmv).

## Decisions (queued for ADR-0065 §15 at v1g)

- **D(sparse)-1** `topology_hash` = FNV-1a-64, little-endian explicit-byte feed
  over `rows,cols,format,block_size` + canonical per-outer (count + sorted
  indices). Slack-invariant: compressed == uncompressed of the same matrix.
- **D(sparse)-2** Structural-query CLI commands (`nnz`/`density`/
  `structural_query`) are **type-agnostic** — registered once each, not ×4 —
  because they read structure only, never values. Typed ops (`from_triplets`/
  `to_csr`/`to_csc`/`build`) keep the ×4 {f32,f64,c32,c64} surface.
- **D(sparse)-4** SELL-C-σ: slice height `C` is **per-T** (`f32`→8 / `f64`→4 /
  complex→4 scalar; `Vec*` paths real-only — complex compiles + is correct, not
  fast). Column-major within a slice, unaligned `load`. **σ row-length sort
  shipped** (default global): rows stably sorted by nnz within a window so
  similar-length rows share a slice (cuts padding on irregular/power-law
  matrices). The sort is by ROW only — within-row columns stay ascending → spmv
  bit-exact with the CSR baseline; for uniform/banded (equal lengths) the sort
  is the identity permutation + a fast path skips the `perm[]` indirection. CLI
  keeps `spmv.{T}` on the CSR baseline (the determinism reference) and adds
  `spmv_sell.{T}` — no silent behavior swap (ADR-0081 versioning).
- **D(sparse)-3** spmv per-row reduction is **two-rounded and bit-exact across
  the CSR scalar baseline and the SELL SIMD primary**: CSR scalar computes
  `acc = acc + val * x[col]` (multiply-then-add, two roundings); SELL computes
  the same per lane via `crd::math::mul_add` (two-rounded under
  `-ffp-contract=off`, per ADR-0063) — **not** `simd::fma` (single-rounded).
  SELL padding slots are exactly `0` (real) / `{0,0}` (complex) at column 0;
  `a + 0 == a` for finite `a`, so padding is a reduction no-op. spmv requires a
  **compressed** CSR matrix (`CRD_ASSERT(is_compressed())`); CSC-spmv +
  uncompressed paths land with the v1c conversion graph. Reduction within a row
  is left-to-right in stored (column-ascending) order, identical scalar and
  per-lane → deterministic and width-independent.
- **D(sparse)-6** BSR block-spmv uses a **dedicated fully-unrolled small-block
  GEMV** (compile-time b∈{1,2,3,4,6} + runtime fallback), NOT the hesap-dense
  v0d GEMM microkernel — that leaf is sized for large-N register tiling and its
  packing/prologue cost dominates a 3×3 block (~9 FMAs). No hesap-dense
  dependency. Diverges from the phase note's "reuse v0d microkernel"
  (primary-source check beat the pre-commitment). b independent accumulators
  break the reduction chain; two-rounded; β=0 NaN-safe.
- **D(sparse)-7** CSR→BSR **zero-pads partial/edge blocks** (a BSR block is dense
  by definition); BSR→CSR emits every stored block's b² entries (structural, not
  a prune). CSR→DIA stores one diagonal per occupied offset; DIA→CSR / ELL→CSR
  drop stored zeros (a diagonal/slot zero is treated as absent) → round-trips
  exactly for zero-free matrices.
- **D(sparse)-8** ELL is the **interop/base** regular format (global padding);
  SELL-C-σ remains the irregular-matrix performance path. Both ship; ELL is not
  a redundant SELL flavour — it is the canonical unsorted-unchunked ELLPACK for
  interchange and uniform patterns.
