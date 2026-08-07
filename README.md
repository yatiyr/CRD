# Cerid Engine

[![CI](https://github.com/yatiyr/crd/actions/workflows/ci.yml/badge.svg)](https://github.com/yatiyr/crd/actions/workflows/ci.yml)

**Cerid** is a general-purpose C++20 real-time engine substrate. Games are one consumer;
simulation (robotics, aerospace, CFD/FEA), medical visualization, creative tools (DAWs),
and offline cinematic pipelines are equal-class consumers. The architecture is modular,
API-stable across backends, and built on vertical slices rather than horizontal layers.

## What makes it different

- **Determinism as a product feature.** Bit-identical results across thread counts
  (`{1..16}` workers), runs, and — where claimed — compilers: deterministic transcendental
  math (`crd::math`, no `std::` in engine numerics), counter-based RNG (Philox), fixed-order
  parallel reductions, deterministic stochastic rounding. Built for replay, certification
  (DO-178C / ISO 26262-class evidence), and reproducible science.
- **A MATLAB-class numerical substrate (`crd-hesap`)** benchmarked head-to-head against the
  strongest references (Eigen, CHOLMOD/UMFPACK, ARPACK, scipy, MATLAB toolboxes, Boost, GSL,
  SUNDIALS, FFTW/MKL, Ruckig, NumPy/PyTorch, …) with honest, reproducible scoreboards —
  see [`docs/bench/`](docs/bench/).
- **Safety-critical API contracts** in the numerical layer: caller-provided workspaces
  (zero heap on hot paths), bounded iteration (no unbounded recursion), status codes instead
  of exceptions, error estimates with labelled certification tiers.
- **Own-your-primitives engineering:** hand-rolled fiber job system (asm context switch,
  Chase-Lev deques, futex/WaitOnAddress semaphores), custom allocators (TLSF, virtual-memory,
  streaming), engine-native containers — no black-box dependencies in the core.
- **One IR for everything the GPU does.** Every shader and compute kernel — rendering,
  FFT/sort/reduction, neural inference, ray tracing, mesh shaders — is authored once as
  backend-neutral CKIR and lowered to Vulkan/DX12/CUDA (WGSL/MSL emitters in place), with
  bit-exact CPU oracles gating the kernels and rendering techniques shipped as cooked assets
  rather than engine code.
- **Agent-native by design:** every engine operation is planned to be reachable via CLI /
  JSON-RPC / MCP; the GUI is a visualization layer. C++ hot-reload is the only scripting layer.

## Modules (shipped)

| Area | Modules |
|---|---|
| Foundation | `core` · `log` · `vm` · `memory` · `containers` · `math` · `jobs` · `platform` · `app` · `config` · `units` · `time` · `perf` |
| GPU platform | `gpu-context` (one device facade; Vulkan · DX12 · CUDA backends) · `kir` (**CKIR** — the backend-neutral shader/kernel IR; GLSL/HLSL/WGSL/MSL/CUDA are emitter *outputs*, never authored) · the asset-driven **RAF** rendering stack (`render-graph`/`render-pass`/`render-program`/`render-material` + five asset cookers — every technique is a cooked asset, not C++) · `draw` (overlay/viz) · `imgui` (debug-only) |
| Scene & assets | `scene` (8-layer ECS substrate) · `resources` (CRDR pack format, hot-reload) · `asset-io` · `anim` · `profile` |
| Geometry | 13 sub-modules: primitives (Shewchuk predicates) · BVH (+GPU LBVH) · convex (GJK/EPA/Quickhull) · mesh · mesh-processing (QEM/remesh) · spatial · polygon (Boolean) · Delaunay/Voronoi · decomposition (V-HACD) · curves · shader-helpers · viz · meshgen |
| Numerics (`crd-hesap`) | dense (BLAS/LAPACK-class) · sparse · orderings · iterative+AMG · sparse-direct (supernodal Cholesky/LU/QR/LDLᵀ, HSS/BLR, mixed-precision IR) · eigensolvers · optimization (QP/LP/NLP/conic/MIP/global) · ODE/DAE · FFT · DSP/wavelets/comms · special functions · statistics (RNG/distributions/MCMC/regression) · interpolation · quadrature · differentiation · motion (Ruckig-class OTG) · tensors (incl. quantized dtypes + NN inference) · autodiff (forward + reverse, deterministic gradients) |

Current front: the post-RAF GPU-platform programme (rendering pipelines · UI/2D · GPU
compute/science/ML · media/editor) — live state in [`context.md`](context.md). Then: hesap-GPU
numerics, the notebook + MCP agent platform, the Cerid-native physics resume (`eylem`), and the
editor.

## Building

Requirements: CMake ≥ 3.25, Ninja, Vulkan SDK ≥ 1.4.305 (NV extension headers; 1.4.341 is the
CI pin), and MSVC 2026 (VS 18)/clang-cl (Windows) or GCC (Linux). CUDA Toolkit is optional
(enables the CUDA compute backend).

```powershell
cmake --preset win-debug
cmake --build --preset win-debug
ctest --preset win-debug
```

Linux presets (`linux-gcc-debug`, `linux-gcc-release`, …) mirror the Windows ones. The full
preset list, sanitizer configs, per-slice verification protocol, and troubleshooting live in
[`docs/BUILDING.md`](docs/BUILDING.md).

## Documentation

Start at **[`docs/README.md`](docs/README.md)** — the documentation map: canonical reading
order plus a map of every doc area. Quick links:

- **Status & roadmap** — [`docs/ROADMAP.md`](docs/ROADMAP.md); live state in [`context.md`](context.md)
- **Engineering principles** — [`docs/PRINCIPLES.md`](docs/PRINCIPLES.md); sanity doctrine — [`docs/SANITY.md`](docs/SANITY.md)
- **Subsystem overviews** — [`docs/systems/`](docs/systems/)
- **Architecture decisions (ADRs)** — [`docs/decisions/`](docs/decisions/)
- **Benchmark results** — [`docs/bench/`](docs/bench/)
- **Session history** — [`docs/sessions/`](docs/sessions/)
