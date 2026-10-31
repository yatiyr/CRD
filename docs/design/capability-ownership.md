# Capability ownership

<!-- doc-role: reference -->
> Contract for [OWN.1](../ROADMAP.md#slice-own.1) and [OWN.2](../ROADMAP.md#slice-own.2). Status lives only in the
> [ROADMAP](../ROADMAP.md#slice-own); the user decision is
> [2026-10-26](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md).

Purpose: every engine capability is implemented once, in its one correct module, and every other module calls that
module. crd-hesap is the computational base for every solver and matrix need, crd-math for fixed-size vectors and
matrices, the geometry modules for predicates and spatial queries, and the foundation modules for containers,
allocators, jobs, I/O, hashing, compression and logging. Mods, plugins, user projects and CHIR/CEIR scripts are outside
the rule: they may implement anything, and reach engine capabilities through the plugin and CHIR seams.

## The registry

[cmake/capability-owners.json](../../cmake/capability-owners.json) sits beside the module registry (the `crd_module`
lines of the root `CMakeLists.txt`, [module registry](module-registry.md)). Each capability is one object:

| Field | Meaning |
|---|---|
| `id` | lower-case words joined by `.` or `-`, unique |
| `summary` | what the capability covers |
| `owner` | the one module, by the name `DEPENDS` uses (`containers`, `hesap-fft`) |
| `status` | `landed` (default): the owner is registered and its public API exists; `assigned`: the owner is registered and `row` creates the API there; `planned`: `row` creates the owner module itself |
| `api` | a landed owner's public API files, inside the owner |
| `signatures` | `{regex, what}` over C-family code (comments and string literals blanked): the constants, intrinsics or definitions only an implementation contains |
| `pending` | `{path, row}`: an engine file that implements the capability outside the owner today, and the row that moves it onto the owner |
| `allowed` | `{path, why}`: a match that is not an implementation (a code generator writing the operation into device source) |
| `consumers` | rows that must consume the owner instead of building their own |
| `decision` | the reason for an owner the module layout alone does not make obvious |

A capability without a crisp textual marker (allocators, spatial indices) carries no signature: the audit reviews it
by reading, and the registry still names its owner so a reviewer has one answer.

## The guard

[scripts/check-capability-owners.py](../../scripts/check-capability-owners.py) is the `crd-check-capability-owners`
CTest and the repository step "One owning module for every engine capability". It fails when:

- the registry is malformed: an unknown key, a duplicate id, an owner that is not a registration (or a planned owner
  that now is one), a missing or foreign API file, a row that is not in the ROADMAP, a signature that does not compile;
- a signature of a landed or assigned capability matches a product source outside the owner's directory and the file
  is neither pending nor allowed. Product sources are `engine/`, the host tools under `tools/` (they ship) and the
  `sandbox/` application (it models engine use for real applications); `tests/` and the measurement programs under
  `runtime/examples` keep reference implementations as oracles and peers and are not scanned;
- a pending or allowed file no longer matches (the entry is stale and goes, so the lists only shrink).

`--audit` prints every match outside its owner, listed or not. The unit tests (`CapabilityOwners` in
[test-repository-tools.py](../../scripts/test-repository-tools.py)) plant a copy outside its owner and show it
refused, show comments, strings and the owner's own copy ignored, stale entries refused and every registry error.

When the guard fires on new code, call the owner's API. When the owner lacks what the code needs, extend the owner
(with its tests and boards) and call it; a new capability gets a registry entry in the same change.

## Decisions

OWN.1 records these owners (2026-10-31):

1. **Compression** goes to a new foundation module, crd-compression. crd-perf's bundle refuses
   compressed sections only because the codec sits in crd-resources, above it, and OWN.1's rule puts compression
   in a foundation module unless the decision records otherwise (the question of the publishing research's
   [P-4](../research/2026-10-27-publishing-packaging-and-app-management-2026-comparison.md#p-4)). Raw DEFLATE and
   inflate, the zstd binding (crdr v2's
   chunk codec, with trained dictionaries) and the GDeflate CPU paths live there; resources, perf and shader-cook
   consume it.
2. **The perceptual image metric** ([IMG-ORACLE](../ROADMAP.md#slice-img-oracle)) is a new `hesap-image` module beside
   hesap-dsp, on hesap-fft and hesap-stats, as the row recommends: tooling owns no numerics.
3. **Seed generators** (SplitMix64 and the 64-bit MMIX LCG step) live in crd-core: crd-core's workload probe,
   crd-perf's fault injection and the CEIR input generator cannot reach hesap-stats. hesap-stats keeps the engines
   (xoshiro, PCG64, Philox, ChaCha, ...) and the distributions, built on crd-core's steps.
4. **Constraint solving.** The row's "eylem constraint solver" does not exist yet: eylem ships data descriptors, mass
   properties and a null scene, and its rigid-body module integrates without constraints. The complementarity solve
   (projected Gauss-Seidel and its successors) is assigned to hesap-opt, where [EYLEM-v1e](../ROADMAP.md#slice-eylem-v1e)
   creates it; eylem owns contact generation, constraint rows and Jacobians, and
   [EYLEM-v1f](../ROADMAP.md#slice-eylem-v1f)'s joints reuse the solve. EYLEM-v7a's internal PCG is
   hesap-iterative's.
5. **Domain owners**: autodiff is hesap-autodiff, audio DSP hesap-dsp, analysis meshing a MESH-SIM module, the FE and
   FV cores their own modules, the ROS 2 bridge 4.2e's, robot-description import EYLEM-v6c's articulation module,
   aerodynamics and the atmosphere EYLEM-v6f-aero's, geometric path planning NAV.3's (trajectory optimization stays
   hesap-opt, CONTROL.8), plotting crd-sciviz, publishing crd-ship and the store crd-store (which consumes crd-ship and
   owns no entitlement, payment, updater or store-adapter code).
6. **Code generators** that write an operation into device source (CKIR's CUDA emitter writing a determinant) are
   allowed matches: the host routine is crd-math's; the generator emits the device's own copy by design.
7. **FNV-1a and the hash combiner** live in crd-core (`crd/core/fnv1a.hpp`), not crd-containers as first registered:
   crd-memory, below crd-containers, hashes allocation stacks, and crd-core already holds the dependency-free integer
   primitives (`wide_multiply`, the seed generators). crd-containers re-exports `fnv1a_64` for `hash_string`; the
   checksum family (CRC-32, CRC-32C, XXH3) stays in crd-containers on crd-core's kernel dispatch.

## Duplicate rows

These rows planned a second implementation; each now consumes the owner (the registry's `consumers`):
[8.2b](../ROADMAP.md#slice-8.2b) (autodiff in crd-math, onto hesap-autodiff), [8.0c](../ROADMAP.md#slice-8.0c) (a
second ROS 2 bridge, onto 4.2e), [8.0a](../ROADMAP.md#slice-8.0a) (a second URDF importer, onto EYLEM-v6c),
[8.1a](../ROADMAP.md#slice-8.1a) (a second aero and atmosphere model, onto EYLEM-v6f-aero),
[CONTROL.10](../ROADMAP.md#slice-control.10) and [CONTROL.13](../ROADMAP.md#slice-control.13) (path planning, onto
NAV.3 and CONTROL.8), [EYLEM-v7a](../ROADMAP.md#slice-eylem-v7a) (an internal PCG, onto hesap-iterative) and the
plotting of [CHIR-SCI.3](../ROADMAP.md#slice-chir-sci.3), [NOTEBOOK.2](../ROADMAP.md#slice-notebook.2) and
[SCIVIZ.2](../ROADMAP.md#slice-sciviz.2) (onto SCIVIZ-PLOT's crd-sciviz).

## Audit (2026-10-31)

Every engine module was searched by the registry's signatures and by reading for reimplementations. Moves done:
CRC-32 (three copies, [HW-RESILIENCE.1](../sessions/2026-10-31-hw-resilience-checksum-family.md)). Moves pending,
each a `pending` entry or a listed review, all on OWN.1:

| Capability → owner | Copies | Notes |
|---|---|---|
| hash.fnv1a → core | 33 engine files and 2 asset-cooker files, **moved** 2026-10-31 | see below; `sandbox`'s BVH-viewer fingerprint mixes the FNV constants into SplitMix64 and moves with the seed generators. |
| hash.combine → core | `lod/lod_asset.cpp`, `scene/archetype_graph.hpp`, **moved** 2026-10-31 | the golden-ratio combiner `h ^= v + 0x9E3779B97F4A7C15 + (h << 6) + (h >> 2)`, twice; see below. |
| random.seed-generators → core | 11 files and core's own workload probe, **moved** 2026-10-31 | SplitMix64 in ceir, perf, core, hesap-eigen, three hesap-stats headers, containers' `hash_u64` finalizer and the sandbox; the MMIX LCG step in hesap-dense's SVD, hesap-fft's sparse FFT and hesap-iterative's IDR(s); bit-exact, see below. |
| cpu.kernel-dispatch → core | `math/libm.hpp`, **moved** 2026-10-31 | read `__AVX2__` and `__FMA__` to choose the FMA instruction; it now reads crd-core's `CRD_TARGET_X64` and `CRD_TARGET_X64_FMA`, see below. |
| math.fixed-size-linear-algebra → math | `geometry-primitives/transform.hpp`, `circumcenter.hpp`, mesh-processing's `quadric.hpp`, **moved** 2026-10-31 | `det3`, the inverse-transpose, the 2D attributes' determinant, the symmetric cofactor inverse and both circumcentres' Cramer's rule; crd-math gained Mat2/Mat3 `determinant`, `inverse`, `try_inverse`, `inverse_transpose`, `solve` and `try_solve`, see below. |
| io.durable-saves → platform | `perf/bundle.cpp` | crd-perf's `write_bundle_atomic` replaces a file durably by hand; it moves onto `durable_file.hpp` once crd-perf, which ships in the relocatable package, can link crd-platform. |
| compression.codecs → compression | resources' `deflate`, `crdr`'s zstd calls and the resources fuzz target's, **moved** 2026-10-31; hesap-tensor's `io_zip.hpp` npz inflate (pending) | decision 1: crd-compression created; see below. |
| numerics.signal-processing → hesap-dsp | `audio/audio_kernels.hpp`, hesap-dsp's own `sosfilt_stream`, **moved** 2026-10-31 | the audio biquad designed with hesap-dsp's RBJ formulas but ran its own transposed direct-form II loop; both loops now run hesap-dsp's `biquad_step`, bit-exact, see below. |
| geometry.robust-predicates | `hesap-interp/clough_tocher.hpp` `fast_orient`, **moved** 2026-10-31 | a deliberate non-robust orientation for the walk with an exact fallback; it is now crd-math's 2D `cross`, not a second predicate, bit-exact, see below. |

Each move is recorded in the OWN.1 session that lands it, with its oracle (bit-exact or the declared tolerance) and
its before/after board.

### The FNV-1a move

`crd/core/fnv1a.hpp` gives FNV-1a-64 and -32 over bytes and over text (the text form is a constant expression, which
the `void*` form cannot be), little-endian integer folds and `fnv1a_64_word`, one step over a whole word for runtime
keys. Each site took the form that keeps its meaning:

- **Persisted or stable identities, bit-exact.** The cooked lighting, vertex-layout, CKIR variant, LOD-policy, sparse
  topology, tensor-artifact, CEIR program and plan-target ids, the obek entity GUID, spec-set and scene signatures, the
  frame executor ids, CEIR's compile-time ids and the asset cooker's content and path hashes fold the same bytes in the
  same order through the owner. Each local `hash_u64` helper keeps its `u64` parameter, so a narrower field still folds
  eight bytes. vertex-cook's string fold ended with an extra prime multiply, which is the string's terminating NUL: it
  now folds `size() + 1` bytes of `c_str()`. CHIR's node ids folded native bytes (identical on the little-endian
  targets) and now fold little-endian by shifts, so the stable id (ADR-0128) is the same on every host.
- **Runtime keys whose values change.** Nine sites started from 1469598103934665603, the offset basis with its last
  digit lost, and scene-render's cull-argument signature multiplied before it xored. None persists or is pinned by a
  test: the template bridge's alias size classes, the diagnostic allocator's stack dedup, the Vulkan descriptor-set
  memo, the CUDA module and Vulkan pipeline caches (in memory), CKIR and autodiff hash-consing, the GPU frame-graph
  fingerprint, the step-down log key and the cull upload signature. They now use the true offset and the word step.
  frame-cook's work extraction hashed resource names with its own truncated FNV while the frame runtime keys the same
  resources by `pass_param_id`; it now uses `pass_param_id`.
- **Not a hash.** hesap-eigen's seeds stride by the prime (`seed + j * prime`) and take the owner's named constant;
  CKIR's hair kernel bakes `0x1000193` into a device hash with xxHash32 constants (allowed, decision 6).

### The seed-generator and combiner move

`crd/core/seed_generators.hpp` gives SplitMix64 in three forms and the MMIX step, all constant expressions:
`splitmix64_mix` (the finalizer), `splitmix64_next` (the stateful generator: add the golden-ratio gamma, then mix),
`splitmix64_at(seed, n)` (output `n` of the stream seeded with `seed`, directly) and `mmix_lcg_next`; `kGoldenRatio64`
names the constant both SplitMix64 and the combiner use. `hash_combine` sits beside FNV-1a in `crd/core/fnv1a.hpp`
(decision 7). The copies had four SplitMix64 shapes, each now a composition of the owner's forms with the same bits:

- the stateful step (`stats::SplitMix64`, now the bit-generator form of `splitmix64_next`; ChaCha20's key expansion;
  the sandbox's box generator);
- one step from a value, `mix(x + gamma)` = `splitmix64_at(x, 0)` (CEIR's seeded input draws, crd-perf's fault draws,
  crd-core's workload probe, the count-min sketch's row seeds);
- output `i` of a stream, `mix(seed + (i + 1) * gamma)` = `splitmix64_at(seed, i)` (hesap-eigen's starting vectors);
- the bare finalizer after an xor, `mix(x ^ gamma)` (containers' `hash_u64`, whose value at 0 is SplitMix64's first
  output from seed 0, now a test).

`test_seed_generators.cpp` (crd-core-tests) writes each shape out as the copies had it and compares it with the
owner's composition over the edges and a 4096-value Weyl sweep of the 64-bit range, beside SplitMix64's published
seed-0 stream (checked at compile time) and the MMIX step's wrap. The LOD policy identity (cooked) and the archetype
mask hash fold through `hash_combine` with the same bits. The sandbox's BVH-viewer fingerprint, a runtime cache key,
mixed FNV and SplitMix constants by hand and is now FNV-1a over the slider tuple. The before/after board
(`runtime/examples/bench_hash.cpp`, through `ab-board.py`) times every moved function against the copy it replaced
and covers the FNV-1a move as well.

### The ISA-read and small-matrix move

`crd/core/kernel_level.hpp` reports the instruction set of the unit being compiled for code whose choice is not a
kernel table's: `CRD_TARGET_X64` (x64, ARM64EC excluded) and `CRD_TARGET_X64_FMA` (such a unit may issue FMA: GCC and
Clang's `-mfma`, which the avx2 and avx512 levels' x86-64-v3 and -v4 carry, or MSVC's `/arch:AVX2` and up, which
defines no `__FMA__`). `libm.hpp` chooses its fma body from them, and `CRD_MATH_SCALAR_DISPATCH` is now
`CRD_TARGET_X64 && !CRD_TARGET_X64_FMA` in general code. The two conditions it replaces differed only for GCC or Clang
code built with `-mavx2` but not `-mfma`, which no preset or level compiles (every x64 level is an x86-64-vN level).
A kernel unit takes `CRD_TARGET_X64_FMA` from its level rather than its compiler (FMA on exactly the avx2 and avx512
levels), because the level's define reaches clang-tidy and MSVC's `/arch` does not: win-tidy analysed the avx2 and
avx512 units without `__AVX2__` and the first form's probe refused them (2026-10-31). The oracle is the
preprocessor's: every kernel unit compiled outside the analysis refuses to compile when its compiler's instruction set
disagrees with its level, and the SIMD-type probe, compiled once per CPU level, checks that `libm.hpp` computes fma in
software on exactly the levels without FMA and on MSVC issues the intrinsic on exactly the FMA levels; CI's
forced-level lanes and its MSVC, clang-cl, tidy and ARM64 lanes compile them on every target. The selected code is
unchanged, so no board applies.

crd-math's `mat_types.hpp` gained the closed-form routines: `determinant` (Mat2, Mat3: the cofactor expansion along
the first column, in the written order), `inverse` (Mat2, Mat3, beside Mat4's), `try_inverse` (Mat3: refuses
`|det| < min_abs_det`, a NaN determinant passes, as `optimal_position` did), `inverse_transpose` (Mat3), and `solve` and
`try_solve` (Mat2, Mat3: Cramer's rule, gated like `try_inverse`). The inverses divide by the value `determinant`
returns, bit for bit, so a caller that tests the determinant tests the value the inverse uses. The 3x3 solve takes each
of its four determinants as `determinant` of the transpose (the expansion along the first row), in which the four share
the six 2x2 minors of the lower two rows of `[m | b]`: 27 multiplications where the adjugate takes 33, which the first
board measured as the difference between parity and a 9% loss. `try_solve` gates on the determinant it divides by. The
sites:

- `transform.hpp`'s `det3`, `inverse_transpose_3` and the 2D attributes' determinant: the same expressions,
  **bit-exact** (a 4096-draw sweep in f32 and f64 against the copies, `test_transform.cpp`).
- `quadric.hpp`'s `optimal_position`: `try_inverse` and the matrix-vector product reproduce the symmetric cofactor
  inverse's six cofactors, its gate and its summation order, **bit-exact**, refusals included (`test_quadric.cpp`).
- `circumcenter.hpp`: both centres solve the translated system `(v - a)·O' = |v - a|^2 / 2` with `try_solve`, the
  centroid when `|det|` is below the smallest normal f64 (zero, as the copies tested, or so small its reciprocal
  overflows, where they returned infinities). The 3D centre is **bit-exact** (the copy expanded along the first row
  too); the 2D copy squared absolute coordinates, and the translated centre is within a declared tolerance of 2^-40 of
  the extent plus the circumradius near the origin and keeps digits the copy lost away from it (`test_circumcenter.cpp`,
  the first direct tests of these functions; the Delaunay, Voronoi, Ruppert and tetrahedral refinement suites pass
  unchanged).

Reviewed and left: geometry-convex's sub-distance barycentrics (signed volumes from crd-math's `dot` and `cross`, a
geometric formula rather than a matrix routine); CKIR's evaluator `mat_det` (the IR oracle's determinant, rounded per
operation at the node's dtype, the IR's own semantics like decision 6) and its 2D-Gaussian-splat emitter (Cramer's rule
written into IR, decision 6).

### The biquad-step and walk-orientation move

hesap-dsp's `filter.hpp` holds the engine's one direct-form II transposed section step: `BiquadState<T>` (moved there
from `filtering.hpp`) and `biquad_step(bq, state, x)`, a `constexpr` multiply-add in scipy's order that returns the
output and advances the state. `sosfilt_stream` cascades it, and the audio module's `apply_biquad` runs it once per
channel with one `BiquadState<f64>` per channel in place of its `z1`/`z2` arrays (the graph renderer's per-node state
and the CEIR executor's per-op state with it). The registry's `numerics.signal-processing` entry gains the step's
signature, so a third hand-written DF2T update outside hesap-dsp is refused. Both moves are **bit-exact**: the step
against the recurrence written out, 64 drawn stable sections of 512 samples in f32 and f64, outputs and states
(`test_filtering.cpp`), with an impulse response checked at compile time; `apply_biquad` against the audio copy for
every filter type, three cutoffs and three Qs, in one pass and cut into blocks of 256, 7, 1, 113 and 64 frames, the
filtered bus and both channels' states (`test_audio_graph.cpp`); the graph and CEIR render suites pass unchanged.

The Clough-Tocher walk read the sign of its own `fast_orient`; it now reads crd-math's 2D `cross(b - a, q - a)`, the
same products in the same order, **bit-exact** over 65536 drawn triangles per precision, one query in eight on the edge
line (`test_clough_tocher.cpp`). It stays a non-robust sign on purpose: a misrouted step costs a step, and the exact
fallback scan is geometry's adaptive `orient2d`.

### The compression module

crd-compression (`engine/foundation/compression`, decision 1) holds the lossless codecs below every module that reads
or writes compressed bytes. `deflate.hpp` is resources' own RFC 1951 DEFLATE and inflate with the RFC 1950 zlib
wrapper and Adler-32, moved whole (namespace `crd::compression`, the code clang-formatted and otherwise unchanged).
`zstd.hpp` is the binding of the pinned zstd, whose header is now private to the module: `zstd_compress_bound`,
`zstd_compress` (one frame at a level, the output replaced, refused with an empty output when zstd fails),
`zstd_decompress_exact` (the frames decode to exactly the buffer's size or are refused: corrupt, truncated, longer and
shorter alike), and `zstd_max_decompressed_size` with `kZstdBlockMaxBytes` (the 3-byte block headers and 128 KiB
blocks a claim is bounded by, moved from `crdr.hpp`). The CRDR reader and writer call the binding with the semantics
they had (the claim checked before anything is reserved, a frame that decodes to any other size refused, a chunk stored
raw when zstd fails or does not shrink it), and the resources fuzz target restates the layout over the binding instead
of linking zstd itself. `crdr.hpp` names the writer's default level as `kZstdDefaultLevel`, so crd-resources links
crd-compression publicly; the registry's signatures (the DEFLATE length base table, the Adler-32 modulus, a direct zstd
call or include) refuse a second codec outside the module.

The move is **bit-exact**: the before/after board runs the engine before the move and after it as two builds of
`runtime/examples/bench_compression.cpp`, and `ab-board.py --same` holds both to the same XXH3-64 of every case's
output (raw and zlib deflate and inflate, Adler-32, a CRDR chunk compressed and read back) on every repetition. The two
deflate cases read 4% slower after the move; `deflate_raw`'s instructions are identical in both binaries and it starts
16 bytes earlier, and with every function aligned to 64 bytes in both builds (the same start address) the cases are
level, so the difference is the hot loop's placement, not the move.

Still on OWN.1: hesap-tensor's npz reader carries a second inflate (`io_zip.hpp`, exact-size and allocation-free, with
an over-subscription check resources' lacked); the two merge into one decoder in crd-compression with an exact-size
form. crd-perf's bundle reads and writes compressed sections through the module once crd-compression and its zstd are
in the relocatable package that ships crd-perf.
