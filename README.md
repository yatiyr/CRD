# Cerid Engine

[![CI](https://github.com/yatiyr/crd/actions/workflows/ci.yml/badge.svg)](https://github.com/yatiyr/crd/actions/workflows/ci.yml)

**Cerid** is a general-purpose C++20 real-time engine substrate. Games are one consumer;
simulation (robotics, aerospace, CFD/FEA), medical visualization, creative tools (DAWs),
and offline cinematic pipelines are equal-class consumers. The architecture is modular,
API-stable across backends, and built on vertical slices rather than horizontal layers.

## Documentation

Start at **[`docs/README.md`](docs/README.md)** — the documentation map: canonical reading
order plus a map of every doc area. Quick links:

- **Status & roadmap** — [`docs/ROADMAP.md`](docs/ROADMAP.md); live state in [`context.md`](context.md)
- **Engineering principles** — [`docs/PRINCIPLES.md`](docs/PRINCIPLES.md)
- **Subsystem overviews** — [`docs/systems/`](docs/systems/)
- **Architecture decisions (ADRs)** — [`docs/decisions/`](docs/decisions/)
- **Session history** — [`docs/sessions/`](docs/sessions/)

## Build

Build presets, commands, and the full toolchain (CMake + Ninja; MSVC / clang-cl / GCC;
Vulkan 1.3) are documented in [`CLAUDE.md`](CLAUDE.md).
