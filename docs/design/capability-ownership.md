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
| cpu.kernel-dispatch → core | `math/libm.hpp` | reads `__AVX2__` and `__FMA__` to choose the FMA instruction; the choice moves behind a crd-core kernel-level macro. |
| math.fixed-size-linear-algebra → math | `geometry-primitives/transform.hpp` | `det3` and the inverse-transpose; crd-math gains Mat2/Mat3 `determinant`, `inverse` and the 3x3 solve. By reading also: `circumcenter.hpp` and mesh-processing's `quadric.hpp` (Cramer's rule), which move onto the same routines with declared tolerances. |
| io.durable-saves → platform | `perf/bundle.cpp` | crd-perf's `write_bundle_atomic` replaces a file durably by hand; it moves onto `durable_file.hpp` once crd-perf, which ships in the relocatable package, can link crd-platform. |
| compression.codecs → compression (new) | resources' `deflate`, `crdr` zstd use | decision 1. |
| numerics.signal-processing → hesap-dsp | `audio/audio_kernels.hpp` | designs its biquad with hesap-dsp's RBJ formulas but runs its own transposed direct-form II loop; the stateful section step belongs to hesap-dsp. |
| geometry.robust-predicates | `hesap-interp/clough_tocher.hpp` `fast_orient` | a deliberate non-robust orientation for the walk with an exact fallback; it becomes geometry's or crd-math's cross product, not a second predicate. |

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

