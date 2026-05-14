# Phase 6 — Platform expansion: console / mobile / web / VR / HPC

**Status:** 📋 planned (ADR-0077 §5)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.x rendering polish (Phase 3.9) close.

**Slot history:** the Phase 6 number was originally "Native physics" but Cerid ships native physics (eylem, Phase 3.1) from day 1, so the slot was free. ADR-0077 §5 repurposes it for platform expansion.

## Why this is late in the roadmap

Platform expansion amortises across a stable rendering / scene / physics stack. Doing console / mobile / web / VR before the substrate is solid means re-doing the work on every substrate change. The substrate-first principle (`docs/PRINCIPLES.md`) means: ship desktop Windows + Linux fully, then go wide.

## Scope

### Console support

NDA-gated, requires dev kits + licensing:
- **PlayStation 5** — `crd-rhi-ps5` backend (closer to Vulkan than DX12), Tempest 3D audio, DualSense haptics + adaptive triggers.
- **Xbox Series X|S** — `crd-rhi-d3d12` backend (X-specific extensions), Velocity Architecture (SSD streaming), sampler feedback.
- **Nintendo Switch** — `crd-rhi-nvn` backend (Nintendo proprietary low-level API), aggressive memory budgets.

### Mobile

- **iOS** — `crd-rhi-metal` backend, MetalFX upscaling, ARKit integration.
- **Android** — `crd-rhi-vulkan` (already exists), Adreno-specific tile-based optimizations, adaptive resolution.
- Mobile-specific considerations: thermal throttling, battery, smaller texture budgets, lower polygon counts, touch input.

### Web

- **WebGPU backend** for `crd-rhi` (Vulkan-class API now standard across browsers).
- **WASM toolchain** — emscripten or wasi-sdk; Cerid's C++20 compiles with both.
- Browser-specific: audio context, input handling, asset streaming via fetch API.
- Limitations: no native threads in older WASM (Web Workers needed for `crd-jobs`).

### VR / AR

- **OpenXR integration** — head tracking, hand tracking, controllers.
- **Per-eye render passes** — multiview or stereo rendering paths.
- **Foveated rendering** — variable-rate shading driven by eye tracker.
- **Reprojection** — async timewarp / spacewarp for low-latency redraws.
- Specific runtimes: SteamVR, Meta Quest (Horizon OS), Apple Vision (visionOS), PSVR2.

### HPC / cluster computing

For large-scale CFD / FEA / multi-player physics:
- **MPI integration** — message-passing for distributed simulation.
- **Domain decomposition** — for partitioned `crd-cfd` / `crd-fea` solves.
- **Distributed physics islands** — multi-machine `crd-eylem` sessions for VR-scale or planetary-scale simulations.
- **Cloud-native** — Kubernetes integration for batch CFD / training jobs.

## Dependencies

- All substrates that will run on the new platform (`crd-rhi`, `crd-renderer`, `crd-scene`, `crd-eylem`, etc.).
- Phase 3.x rendering polish (Phase 3.5–3.9) shipped.

## Sub-modules / sub-phases

- **6.0 console** — PS5 + Xbox + Switch (separately, each NDA-gated).
- **6.1 mobile** — iOS + Android.
- **6.2 web** — WebGPU + WASM.
- **6.3 vr** — OpenXR + per-eye + foveated.
- **6.4 hpc** — MPI + cluster integration.

Each can ship independently; ordering depends on consumer demand.

## Out of scope

- macOS / Linux desktop — already supported (Linux in CI; macOS Metal backend would be a `crd-rhi-metal` work).
- Console-specific game features (trophies, achievements, etc.) — application layer, not substrate.
- Mobile platform store integration (IAP, ads) — application layer.

## Open questions

- **macOS desktop** — Cerid's Vulkan-only path means macOS works through MoltenVK (Vulkan → Metal translation). Native `crd-rhi-metal` would be cleaner long-term; defer until consumer demand.
- **Console NDAs** — Cerid is open development; console support typically requires gated dev branches. The substrate-first principle means console backends are clean swap-ins; the NDA-gated work is mostly RHI implementation + asset format conversion.
- **WASM threading** — `crd-jobs` uses native threads; WASM-MT (multi-threaded WASM via SharedArrayBuffer) is the path. Requires browser support; degrade gracefully if unavailable.

## Revisit triggers

This stub becomes a full phase plan when:
- Phase 3.9 close (rendering polish stable).
- A specific platform demand (publisher requirement, partnership) makes a specific subsystem priority.
- WebGPU spec finalization (currently shipping in Chrome / Edge / Firefox / Safari — substantial but not 100%).
