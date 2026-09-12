# Cerid Engine

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

**New here?** [START_HERE](START_HERE.md) gives the agent/human entry route. The
[system review](docs/research/2026-09-12-cerid-whole-system-review.md) separates existing substrate from planned products.

[![CI](https://github.com/yatiyr/crd/actions/workflows/ci.yml/badge.svg)](https://github.com/yatiyr/crd/actions/workflows/ci.yml)

**Cerid** is a general-purpose C++20 real-time engine substrate. Games are one consumer;
simulation (robotics, aerospace, CFD/FEA), medical visualization, creative tools (DAWs),
and offline cinematic pipelines are equal-class consumers. The architecture is modular,
API-stable across backends, and built on vertical slices rather than horizontal layers.

## What makes it different

- **Determinism as a product feature.** Bit-identical results across thread counts
  (`{1..16}` workers), runs, and — where claimed — compilers: deterministic transcendental
  math (`crd::math`, no `std::` in engine numerics), counter-based RNG (Philox), fixed-order
  parallel reductions, deterministic stochastic rounding. Built for replay and reproducible science;
  domain certification requires its own evidence and is not established by these primitives.
- **A MATLAB-class numerical substrate (`crd-hesap`)** benchmarked head-to-head against the
  strongest references (Eigen, CHOLMOD/UMFPACK, ARPACK, scipy, MATLAB toolboxes, Boost, GSL,
  SUNDIALS, FFTW/MKL, Ruckig, NumPy/PyTorch, …) with honest, reproducible scoreboards —
  see [`docs/bench/`](docs/bench).
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
  JSON-RPC / MCP; the GUI is a visualization layer. Programs are authored as Cerid-owned
  CEIR/CHIR (text or CR-D007 visual) or via a C++ builder; C++ hot-reload stays a first-class
  authoring surface, no longer the only one (ADR-0108).

## Source map and current work

The [module/source map](docs/systems/README.md) lists the actual engine modules and their manifests.
Read current public source before relying on a historical system example or a dated test count.

Current sequence: finish the entire retained renderer library → native `crd-ui` and **CR-D007**
(game/engineering editor and game publishing) → hesap GPU and the notebook → media and remaining
programmes. The notebook shares one workspace implementation between CR-D007 and a standalone host.
Windows/Linux qualify first; macOS/web explicitly follow. Track every slice in the single
[master table](docs/ROADMAP.md#master-table); [context](context.md) is the current pointer.

CEIR is the shared execution IR; CHIR is its high-level authoring layer and CKIR its device-program
form. The recorded CEIR execution foundation is complete; that does not mean renderer quality,
full CHIR, UI widgets or CR-D007 have shipped. [Module inventory](docs/systems/README.md) and
[system audit](docs/research/2026-09-12-system-audit.md) distinguish these boundaries.

## Building

Use [BUILDING](docs/BUILDING.md) for scoped local targets, CTest, tidy and CI qualification.
The helper scripts and CMake presets define actual tool paths and available configurations.

## Documentation

Start at **[`docs/README.md`](docs/README.md)** — the documentation map: canonical reading
order plus a map of every doc area. Quick links:

- **Status & roadmap** — [`docs/ROADMAP.md`](docs/ROADMAP.md#master-table); live state in [`context.md`](context.md)
- **Engineering principles** — [`docs/PRINCIPLES.md`](docs/PRINCIPLES.md); sanity doctrine — [`docs/SANITY.md`](docs/SANITY.md)
- **Subsystem overviews** — [`docs/systems/`](docs/systems)
- **Architecture decisions (ADRs)** — [`docs/decisions/`](docs/decisions)
- **Benchmark results** — [`docs/bench/`](docs/bench)
- **Session history** — [`docs/sessions/`](docs/sessions)
