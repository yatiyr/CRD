# Cerid Engine — Roadmap Hub

> **Cerid is a general-purpose C++20 real-time engine substrate.** Games are
> one consumer; simulation (incl. robotics), medical visualization,
> DAW-class creative tools, and offline cinematic pipelines are equal-class
> consumers.
>
> **This is a hub.** Don't read it end-to-end. Follow the link relevant to
> your task. The detailed plans live in `docs/phases/`, the architectural
> decisions live in `docs/decisions/`, and the live state lives in
> `context.md`.

---

## Core docs (read every session)

- **`AGENTS.md`** (project root) — project rules, agent roster, coding standards
- **`docs/PRINCIPLES.md`** — engineering principles + pinned cornerstones
- **`context.md`** (project root) — live "where we are now" state

## Status (snapshot)

| Phase | State        | Detail                                       |
| ----- | ------------ | -------------------------------------------- |
| 1.0   Foundations               | ✅ shipped       | `docs/phases/phase-1-foundations.md`     |
| 1.5   Application skeleton      | ✅ shipped       | `docs/phases/phase-1.5-app.md`           |
| 1.6   Configuration substrate   | 🚧 active        | `docs/phases/phase-1.6-config.md` (1.6a shipped; consumers next) |
| 2.0   RHI + Vulkan + triangle   | ✅ shipped (a–d) | `docs/phases/phase-2-graphics.md`        |
| 2.1   ImGui debug overlay       | ✅ shipped       | `docs/phases/phase-2-graphics.md`        |
| 2.2   GPU memory + streaming    | ✅ shipped       | `docs/phases/phase-2-graphics.md`        |
| 2.3   Shader system             | ✅ shipped (a–g) | `docs/phases/phase-2.3-shader.md`        |
| 2.4   Renderer v1               | 🚧 active        | `docs/phases/phase-2-graphics.md`        |
| 2.5   Jobs (threads + fibers)   | ⏳               | `docs/phases/phase-2-graphics.md`        |
| 2.6   Resources + asset cooker  | ⏳               | `docs/phases/phase-2-graphics.md`        |
| 3     Simulation foundation     | ⏳               | `docs/phases/phase-3-simulation.md`      |
| 4     Extensibility             | ⏳               | `docs/phases/phase-4-extensibility.md`   |
| 5     UI + node + advanced render | ⏳             | `docs/phases/phase-5-ui-rendering.md`    |
| 6     Native physics            | ⏳               | `docs/phases/phase-6-native-physics.md`  |
| 7     Editor                    | ⏳               | `docs/phases/phase-7-editor.md`          |

Legend: ✅ shipped · 🚧 active · ⏳ planned · ❌ blocked

## Decision log

All architectural decisions are individual ADRs under `docs/decisions/`.
Tag-indexed list: `docs/decisions/README.md`.

## Detour queue

Active detour, if any, is named in `context.md`. Queue and rules:
`docs/detours/README.md`.

## Open debt

`docs/debt.md`.

## Long-range outlook

Direction, not commitment.

### 6–12 months
- Stable Vulkan backend, GPU allocator strategy in place
- Shader system v1 with hot-reload
- Renderer v1 (Clustered Forward+) with multiple renderables, basic PBR
- Cooked asset pipeline + first real scenes
- ImGui debug tooling and basic profiling visibility

### 12–24 months
- Physics + scene + animation modules shipped (PhysX-backed)
- Hot-reload C++ scripting boundary
- DAW / simulation use-case prototypes built on Cerid to validate substrate
  claims
- Asset / scene / shader hot-reload across the board

### 24–36 months
- Retained-mode `crd-ui` mature enough to replace ImGui in editor surfaces
- Node editor for shaders (and optional scripts)
- Editor shell usable for real authoring
- Deferred + Visibility-Buffer render paths beside Forward+
- First Cerid-native physics path landing alongside PhysX
- Plugin ecosystem basics (versioned C ABI, sample plugins)
- Determinism / replay path for simulation/medical workflows

The goal: Cerid becomes a real-time substrate for interactive applications
across games, simulation, and creative tools — not "another Vulkan engine."

## Glossary

- **Channel** — a named log filter for one subsystem.
- **Sink** — a log destination (Console / File / Debugger / RingBuffer / Null).
- **RHI** — Render Hardware Interface; the API-agnostic graphics layer.
- **TLSF** — Two-Level Segregated Fit; an O(1) general-purpose allocator.
- **VMA** — Vulkan Memory Allocator (AMD library; Cerid writes its own).
- **ADR** — Architecture Decision Record; one decision per file.
- **SPSC** — Single Producer Single Consumer (lock-free queue class).
- **CSM** — Cascaded Shadow Maps.
- **IBL** — Image-Based Lighting.
- **TAA** — Temporal Anti-Aliasing.
- **TOI** — Time of Impact (continuous collision detection).
- **GJK / EPA** — narrowphase distance / penetration algorithms.
- **Forward+ / Clustered Forward** — forward shading with per-tile or
  per-cluster light lists; scales to many lights without a G-buffer.
- **Visibility Buffer** — render path that defers material evaluation by
  storing only triangle IDs in the framebuffer.
- **Hi-Z** — depth pyramid used for occlusion queries.

## Document conventions

- `docs/ROADMAP.md` (this hub) — navigation only. Doesn't grow.
- `docs/PRINCIPLES.md` — engineering compass. Stable.
- `docs/phases/<phase>.md` — one file per phase. Slices, decisions ref,
  open questions.
- `docs/decisions/<NNNN>-<slug>.md` — one ADR per decision. Append-only
  numbering. Index at `docs/decisions/README.md`.
- `docs/detours/<D-NNN>-<slug>.md` — one file per detour. Index at
  `docs/detours/README.md`.
- `docs/sessions/<YYYY-MM-DD>-<slug>.md` — one file per work session.
- `docs/systems/<module>.md` — short overview per shipped module.
- `docs/<module>/<MODULE>_FILE.md` — long-form deep-dive (only when
  explicitly requested).
- `docs/bench/` — benchmark baselines, one file per snapshot.
- `docs/debt.md` — open debt list.
- `context.md` (project root) — live state. Short. Updated by
  `@docs-keeper` at session end.

After a system has shipped, **prefer adding to its session log over
rewriting its overview**.
