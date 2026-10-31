# docs/recipes — educative build-recipes

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

A **recipe** is a standalone teaching document for a technique, algorithm, or system we have studied and built.
Its job: **a human or an agent who reads it once understands the thing completely** — the physics/maths, every
parameter, the full assembly, the papers it comes from, and the traps that cost us time. Not a changelog, not an
API reference — a *lesson*.

This folder exists because we learn from many papers, and that knowledge is worthless if it lives only in code
comments and one engineer's head. A recipe is where a subject is taught end to end.

## When to write one

**Whenever we study something and build it** — an algorithm, a rendering technique, a numerical method, a data
structure, a device feature. If we spent real effort understanding a paper (or several) and turning it into
working code, that understanding gets a recipe. This is a standing rule; see `AGENTS.md`.

## What every recipe MUST contain, in this order

1. **PARAMETERS FIRST.** A complete table of every knob — name, meaning, units, sensible default, and the range
   over which it behaves. A reader should be able to drive the system from this table alone before reading a word
   of theory. What each parameter *physically is*, not just what it does.
2. **What it is / why it exists** — the problem, in plain language, and why the naive approach fails.
3. **The physics / maths** — the actual model, derived or cited, enough that the equations in the code are
   understood, not copied. Cite the papers precisely (author, year, section/equation).
4. **The full assembly** — every stage, in order, with the data that flows between them. A reader could rebuild
   the system from this section.
5. **The traps** — every scar. The bugs that cost us time, WHY they happened, and how to recognise the symptom.
   This is the most valuable section and the one only we can write.
6. **Measured numbers** — link the `docs/bench/` board; never quote perf from memory.
7. **Where the code lives** — the files, so the recipe and the implementation stay connected.

## Naming

DX12 [consumer contracts](2026-09-13-dx12-consumer-contracts.md) connects stage interfaces, indexed pipeline caches,
CEIR clear propagation and resource hints to the real renderer's validation and pixel gates.

Developer tooling: [Conservative affected-build selection](2026-09-12-affected-build-selection.md) explains CMake
ownership, generated aggregates, fixture expansion and safe fallback without automatic local sweeps.

DX12 [resource states and alias activation](2026-09-13-dx12-resource-states.md) explains native ownership across
indexed draws, repeated frames and placed memory, with validation and consumer evidence.
Its [frame descriptor allocator](2026-09-13-dx12-frame-descriptors.md) covers contiguous tables, bounded growth,
heap rebinding, explicit failure and retirement.

`YYYY-MM-DD-<subject>.md` (e.g. `2026-07-21-hair-offline-film.md`). A subject may have more than one recipe when
the offline and real-time forms are genuinely different systems (as hair does).

## Index

- [Workspace sync](workspace-sync.md) — the operator's daily NAS sync, an export for a friend and a restore on a new machine; what the NAS receives and how it is protected.
- [Setting up the Cerid CI platform](ci-platform-setup.md) — the user's four steps (project, billing, budget, go) and the operator's idempotent runbook (APIs, buckets, budget stop, identities, network, launcher, images, the IAP-tunnel UI) for ADR-0135, then the optional pieces.
- [Operating the Cerid CI platform](ci-platform-runbooks.md) — SLOs and runbooks: stuck build, lost VM, budget hit, coordinator down, flaky quarantine, first bad commit, upgrade, backup and disaster recovery.
- [Argument reduction for crd::math's log and trig](2026-10-29-scalar-log-and-trig-reduction.md) — log's carried quotient and exact head sum (≤0.52 ulp) and Payne–Hanek past Cody-Waite's exact range.
- [DX12 inner-coverage qualification](2026-09-13-dx12-inner-coverage.md) — exact interior/edge pixels, the native/barycentric route contract, one CPU corner oracle and SDK-only provider isolation.
- [Pinned WARP provider](2026-09-13-dx12-pinned-warp.md) — hash-verified app-local Microsoft WARP 1.0.20 staged beside test executables; hosted lanes qualify one software tuple.
- [DX12 validation and command lifetimes](2026-09-12-dx12-validation-and-command-lifetimes.md) — bounded startup-to-teardown capture, checked submission and truthful fence failures.
- [CKIR snapshots and bounded compute emission](2026-09-12-kernel-snapshot-ordering.md) — shared declaration ordering, live loads, Boolean value types and the atomic A-buffer regression.
- [DX12 adapter classification](2026-09-12-dx12-adapter-classification.md) — selected-LUID evidence, BasicRender and explicit unknown query outcomes.

- [2026-09-12-generated-codelet-stack-safety.md](2026-09-12-generated-codelet-stack-safety.md) — diagnose generated FFT stack frames, preserve optimized code generation and bound unoptimized leaf inlining.
- **2026-09-04-ceir-reverse-mode-autodiff.md** — reverse-mode autodiff as a CEIR compiler transform: `build_gradient` rewrites a forward module into a backward one (VJP rules for gemm/reduce/mlp), which then plans + runs device-resident like any program. The composite-vs-expansion reframe (why you differentiate `ml.mlp`, not the lowered resultless ReLU), write-through-declare, two-output readback, forward-liveness, readonly GLSL-vs-HLSL codegen, recompute-as-checkpointing. Board: `docs/bench/2026-09-04-ceir25-autodiff-scoreboard.md`.
- **2026-07-21-hair-offline-film.md** — physically-based path-traced hair, film/offline quality. The Marschner /
  Chiang fibre BCSDF, linear-swept-sphere geometry, multi-bounce GI, and the full renderer. The reference the
  real-time tier is measured against.
- **2026-07-21-hair-realtime.md** — the same hair in real time (games): the levers that turn the 194 ms/sample
  film path into a 29 ms/frame real-time path, plus temporal accumulation + denoising and the raster tier.
- [Recipe — 2D Gaussian Splatting (surfels): the geometrically-accurate radiance-field primitive](2026-07-21-2d-gaussian-splatting-surfels.md)
- [Recipe — Physically-based hair, film / offline quality](2026-07-21-hair-offline-film.md)
- [Recipe — Real-time hair (games)](2026-07-21-hair-realtime.md)
- [Recipe — mesh extraction: TSDF fusion + marching cubes](2026-07-21-mesh-extraction-tsdf-marching-cubes.md)
- [Recipe — Cinematic ocean sky + a seamless projected-grid horizon (CKIR)](2026-07-22-cinematic-ocean-sky-and-seamless-horizon.md)
- [Recipe — A neural material (2-D neural field) on the cooperative-vector tensor path](2026-07-22-neural-material-coopvec-field.md)
- [Recipe — Reverse-mode autodiff as a CEIR compiler transform](2026-09-04-ceir-reverse-mode-autodiff.md)
- [Conservative affected-build selection](2026-09-12-affected-build-selection.md)
- [DX12 consumer contracts: shader interfaces, caches and attachment clears](2026-09-13-dx12-consumer-contracts.md)
- [Bounded DX12 frame descriptor allocation](2026-09-13-dx12-frame-descriptors.md)
- [DX12 resource states across indirect draws, aliasing and repeated frames](2026-09-13-dx12-resource-states.md)
