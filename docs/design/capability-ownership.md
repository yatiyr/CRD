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
- a signature of a landed or assigned capability matches an engine source outside the owner's directory and the file
  is neither pending nor allowed;
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
| hash.fnv1a → containers | 33 files | byte-stream FNV-1a-64 (onto `fnv1a_64`), byte-stream FNV-1a-32 (profiler, frame runtime, LOD chain: the owner gains `fnv1a_32`), word combiners `h = (h ^ v) * prime` (the owner gains one word step), and hesap-eigen's seed strides `seed + j * prime`, which use the prime as a constant and take the owner's named constant. Several feed persisted identity (cooked assets, CRDR, LOD and tensor artifacts, obek), so each move keeps HEAD's bits, pinned by golden values computed before the edit. |
| random.seed-generators → core | 10 files | SplitMix64 in ceir, perf, hesap-eigen, three hesap-stats headers and containers' `hash_u64` finalizer; the MMIX LCG step in hesap-dense's SVD, hesap-fft's sparse FFT and hesap-iterative's IDR(s); bit-exact. |
| cpu.kernel-dispatch → core | `math/libm.hpp` | reads `__AVX2__` and `__FMA__` to choose the FMA instruction; the choice moves behind a crd-core kernel-level macro. |
| math.fixed-size-linear-algebra → math | `geometry-primitives/transform.hpp` | `det3` and the inverse-transpose; crd-math gains Mat2/Mat3 `determinant`, `inverse` and the 3x3 solve. By reading also: `circumcenter.hpp` and mesh-processing's `quadric.hpp` (Cramer's rule), which move onto the same routines with declared tolerances. |
| compression.codecs → compression (new) | resources' `deflate`, `crdr` zstd use | decision 1. |
| numerics.signal-processing → hesap-dsp | `audio/audio_kernels.hpp` | designs its biquad with hesap-dsp's RBJ formulas but runs its own transposed direct-form II loop; the stateful section step belongs to hesap-dsp. |
| geometry.robust-predicates | `hesap-interp/clough_tocher.hpp` `fast_orient` | a deliberate non-robust orientation for the walk with an exact fallback; it becomes geometry's or crd-math's cross product, not a second predicate. |

Each move is recorded in the OWN.1 session that lands it, with its oracle (bit-exact or the declared tolerance) and
its before/after board.
