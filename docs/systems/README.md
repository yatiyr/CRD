# Systems

One short overview per shipped engine module. Plain English. "What is it, what does it do, how do I use it."
Read this folder to remember what the engine *is*; read `docs/sessions/` to remember how it got that way.

> Refreshed 2026-08-07 (doc-hygiene pass). Statuses here are honest as of that date — a row's ✅ means the
> module is shipped and its overview describes reality; docs written mid-phase carry their own historical
> markers. When a new module ships, add a row; when a module dies, move its row to **Retired** (never delete
> the overview — it becomes history).

## The current GPU / rendering stack (read these, not the retired docs)

| Doc | What it is |
| --- | ---------- |
| [rendering-foundation.md](rendering-foundation.md) | **The RAF architecture** — the whole asset-driven rendering stack in one overview: the five authored declarations (`.frame.toml`/`.crdt`/`.crdv`/`.crdl`/`.crdm`), cookers, runtime, executors, backends. Start here. |
| [shader-ir-corpus-and-stages.md](shader-ir-corpus-and-stages.md) | The CKIR shader-IR corpus + stage model (ADR-0101/0103) — what the IR can express and how it lowers. |
| [rah-0-canonical-model-audit.md](rah-0-canonical-model-audit.md) | **Design note (pending user review)** — the post-RAF canonical-command-model audit driving RAH-1…8. |

⚠ **Known gap (reported, not hidden):** the live GPU modules — `gpu-context` (+ `-vulkan`/`-dx12`/`-cuda`),
`kir` (+ per-backend emitters), `render-graph`/`render-pass`/`render-program`/`render-material`/
`render-asset-core`, `draw`, the five cookers (`frame/technique/vertex/light/material-cook`), `scene-render`,
`anim`, `asset-io`, `lod`, `preset`, `timeline` — have **no per-module overview yet**; the docs above +
`docs/detours/D-007-gpu-program-system.md` are authoritative for them.

## Foundation

| System | Status | Overview |
| ------ | ------ | -------- |
| `crd-core`       | ✅ | [core.md](core.md) |
| `crd-log`        | ✅ | [log.md](log.md) — deep-dive: [`docs/log/LOG_FILE.md`](../log/LOG_FILE.md) |
| `crd-vm`         | ✅ | [vm.md](vm.md) |
| `crd-memory`     | ✅ | [memory.md](memory.md) — deep-dive: [`docs/memory/MEMORY_FILE.md`](../memory/MEMORY_FILE.md) |
| `crd-containers` | ✅ | [containers.md](containers.md) — deep-dive: [`docs/containers/CONTAINERS_FILE.md`](../containers/CONTAINERS_FILE.md) |
| `crd-math`       | ✅ | [math.md](math.md) — SIMD layer: [math-simd.md](math-simd.md) |
| `crd-platform`   | ✅ | [platform.md](platform.md) |
| `crd-app`        | ✅ | [app.md](app.md) |
| `crd-config`     | ◧ | [config.md](config.md) — 1.6a shipped; 1.6b hot-reload hook deferred |
| `crd-units`      | ✅ | [units.md](units.md) — `Quantity<D, T>` two-layer typed architecture (ADR-0078) |
| `crd-jobs`       | ✅ | [jobs.md](jobs.md) — fiber job system; run/wait/parallel_for; Cerid-owned semaphore/deques |
| `crd-perf`       | ✅ | [perf.md](perf.md) — per-thread sample rings + `ScopedRegion` + perf-ui |
| `crd-imgui`      | ✅ | [imgui.md](imgui.md) — debug-only overlay (forever, ADR-0023); backend on gpu-context since RET-5 |

## Scene & assets

| System | Status | Overview |
| ------ | ------ | -------- |
| `crd-scene`      | ✅ | [scene.md](scene.md) — Phase 3.0 closed 2026-05-10; 8-layer slot ECS (ADRs 0049–0061). Concurrency: [scene-concurrency.md](scene-concurrency.md) |
| `crd-resources`  | ✅ | [resources.md](resources.md) — handle table, sync/async/streamed loading, eviction, hot-reload, CRDR cooker. GPU-free resource types re-homed here at RET-3: [texture_resource.md](texture_resource.md) · [mesh_resource.md](mesh_resource.md) |
| `crd-meshgen`    | ✅ | [meshgen.md](meshgen.md) — procedural primitives producing the standard vertex layout |
| `crd-sandbox`    | ✅ | [sandbox.md](sandbox.md) — the interactive desktop app; runs entirely on gpu-context since RET-6/8 (overview partially historical, see its banner) |

## Geometry (`crd-geometry-*`, Phase 3.1.7 ✅ CLOSED 2026-05-19, ADR-0076)

| System | Status | Overview |
| ------ | ------ | -------- |
| `-primitives` | ✅ | [geometry-primitives.md](geometry-primitives.md) — shapes, Shewchuk predicates, intersection corpus |
| `-bvh` | ✅ | [geometry-bvh.md](geometry-bvh.md) — binned-SAH / dynamic / quad-BVH + SIMD traversal |
| `-bvh-gpu` | ✅ | [geometry-bvh-gpu.md](geometry-bvh-gpu.md) — Karras LBVH on the compute context |
| `-convex` | ✅ | [geometry-convex.md](geometry-convex.md) — GJK/EPA/SAT, Quickhull |
| `-mesh` | ✅ | [geometry-mesh.md](geometry-mesh.md) — closest-point / watertight raycast / winding number |
| `-mesh-processing` | ✅ | [geometry-mesh-processing.md](geometry-mesh-processing.md) — QEM, Loop subdivision, remesh, repair |
| `-spatial` | ✅ | [geometry-spatial.md](geometry-spatial.md) — KD-tree, loose octree, R*-tree, spatial hash, uniform grid |
| `-polygon` | ✅ | [geometry-polygon.md](geometry-polygon.md) — ear-clip, CDT, Vatti Boolean, Bentley-Ottmann |
| `-delaunay` | ✅ | [geometry-delaunay.md](geometry-delaunay.md) — Delaunay/Voronoi 2D/3D, Ruppert, CVT |
| `-decomposition` | ✅ | [geometry-decomposition.md](geometry-decomposition.md) — voxelize + V-HACD |
| `-curves` | ✅ | [geometry-curves.md](geometry-curves.md) — Bezier/Hermite/Catmull-Rom/B-spline + arc-length + frames |
| `-viz` | ✅ | [geometry-viz.md](geometry-viz.md) — debug visualization emitters (now over crd-draw) |
| `-shader-helpers` | ✅ | [geometry-shader-helpers.md](geometry-shader-helpers.md) — cooker-emitted GLSL/HLSL helper corpus |

## Numerics (`crd-hesap-*`, Phase 3.1.6 — v0–v16 ✅, paused mid-v17/GPU → D-007)

| System | Status | Overview |
| ------ | ------ | -------- |
| `crd-hesap-dense`| ✅ (v0) | [hesap-dense.md](hesap-dense.md) — BLAS L1/L2/L3 over Matrix/Symmetric/Triangular/Banded; GEMM wins vs Eigen-MT; FMA microkernels; allocator-propagating |
| `crd-hesap-sparse` | ✅ | [hesap-sparse.md](hesap-sparse.md) — sparse formats + kernels substrate |
| `crd-hesap-ordering` | ✅ | [hesap-ordering.md](hesap-ordering.md) — AMD/ND fill-reducing orderings |
| `crd-hesap-eigen` | ✅ (v6) | [hesap-eigen.md](hesap-eigen.md) — Lanczos/Arnoldi/LOBPCG/JD/FEAST/IRLBA sparse eigensolvers (ADR-0089) |
| `crd-hesap-opt` | ✅ (v7) | [hesap-opt.md](hesap-opt.md) — QP/LP/conic/NLP/DFO/CMA-ES/global/MIP (ADR-0090) |
| `crd-hesap-ode` | ✅ (v9 a→z, 2026-06-13) | [hesap-ode.md](hesap-ode.md) — ODE/DAE cluster (ADR-0091): stepper kernels + driver substrate, IMEX, Krylov, sensitivities, index reduction |
| `crd-hesap-fft` | ✅ (v10) | [hesap-fft.md](hesap-fft.md) — deterministic-plan Stockham + split-radix codelets + four-step + real FFT/NUFFT/DCT/DST |
| `crd-hesap-dsp` | ✅ (v11) | [hesap-dsp.md](hesap-dsp.md) — DSP cluster (ADR-0093): design/filtering/multirate/spectral/adaptive on a multi-threaded bit-identical FFT |
| `crd-hesap-wavelet` | ✅ (v11w) | [hesap-wavelet.md](hesap-wavelet.md) — 76 families, DWT/SWT/CWT/2-D/MODWT + denoising; beats pywt's C core |
| `crd-hesap-comms` | ✅ (v11c) | [hesap-comms.md](hesap-comms.md) — modulation/pulse/sync/equalizers/channels/OFDM; crushes liquid-dsp |
| `crd-hesap-special` | ✅ (v12) | [hesap-special.md](hesap-special.md) — special functions incl. Bessel; generated minimax rationals beat Boost |
| `crd-hesap-stats` | ✅ (v12) | [hesap-stats.md](hesap-stats.md) — RNG (Philox), distributions, samplers, MCMC, descriptive/regression |
| `crd-hesap-interp` | ✅ (v13) | [hesap-interp.md](hesap-interp.md) — PCHIP/splines/barycentric/RBF/gridded N-D/kriging (ADR-0095) |
| `crd-hesap-quadrature` | ✅ (v12-c+v13) | [hesap-quadrature.md](hesap-quadrature.md) — Gauss + QUADPACK-class adaptive + DE + oscillatory + cubature; crushes scipy/GSL |
| `crd-hesap-diff` | ✅ (v13) | [hesap-diff.md](hesap-diff.md) — Fornberg stencils, Richardson, complex-step (machine-exact), Savitzky-Golay |
| `crd-hesap-motion` | ✅ (v13) | [hesap-motion.md](hesap-motion.md) — SQUAD/clothoid/NURBS/min-jerk/S-curve + full Ruckig-class OTG, bit-exact and faster than Ruckig |
| `crd-hesap-tensor` | ✅ (v14, 2026-07-05) | [hesap-tensor.md](hesap-tensor.md) — Tensor/TensorView + dtypes (f16/bf16/FP8/quantized + SR converts), einsum, batched LA, npy/npz/safetensors/DLPack I/O, NN inference pack (ADR-0096) |
| `crd-hesap-autodiff` | ✅ (v15 fwd + v16 rev, 2026-07-07) | [hesap-autodiff.md](hesap-autodiff.md) — forward Dual/Jet/HyperDual + SIMD drivers, sparsity, Taylor-mode; reverse mode + implicit diff + ODE adjoints + KAN (ADR-0097). Deterministic `{1..16}` gradients |
| `crd-hesap-resources` | ✅ | [hesap-resources.md](hesap-resources.md) — the `crd-resources` ↔ hesap bridge (sparse matrices in CRDR; 'TNSR' tensor artifacts since v14-l) |

## Physics (paused)

| System | Status | Overview |
| ------ | ------ | -------- |
| `crd-eylem` | ⏸ paused at v1b (2026-05-11) | [eylem-allocators.md](eylem-allocators.md) — the allocator design note; full plan `docs/phases/phase-3.1-eylem.md` |

## Retired modules (overviews deleted — history lives in git + the ADRs)

The `crd-rhi` (+`-vulkan`), `crd-rhi-compute`, `crd-renderer`, and `crd-shader` modules were deleted at RET-8
(2026-07-23, ADR-0105); their overview docs were **deleted 2026-08-07** (user direction — recoverable via git
history). Successors: `crd-gpu-context` (ADR-0103) · `IComputeContext` (ADR-0099/0103) · gpu-context + RAF frame
graphs (ADR-0106) · CKIR (ADR-0101/0104). The retirement ledger (coverage-parity audit, what re-homed where):
D-007 RET band rows 89–96. Historical mentions of the deleted overview paths in old ADRs/phase docs are left as
written — they describe their own era.

When a new module ships, add a row here and link to its overview. If a module gets a long-form deep-dive
document (like `LOG_FILE.md`), link to that too — the overview here should stay short and stable.
