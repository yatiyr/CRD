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
| 2.4   Renderer v1               | 🚧 active        | `docs/phases/phase-2-graphics.md` (v1a–i shipped; v1j = GPU instancing, Phase 3.2 dep) |
| 2.5   Jobs (threads + fibers)   | 🚧 active        | `docs/phases/phase-2.5-jobs.md` (v1a–g shipped; v1h–k planned; ADR-0033) |
| 2.6   Resources + asset cooker  | ⏳               | `docs/phases/phase-2-graphics.md`        |
| 3     Simulation foundation     | ⏳               | `docs/phases/phase-3-simulation.md` (3.0 scene/ECS → 3.1 physics → 3.2 animation → 3.3 audio → 3.4–3.6) |
| 4     Extensibility + Networking | ⏳              | `docs/phases/phase-4-extensibility.md` (4.0 C++ scripting, 4.1 advanced math, 4.2 networking; ADR-0034, ADR-0035) |
| 5     UI + node + advanced render | ⏳             | `docs/phases/phase-5-ui-rendering.md`    |
| 6     Native physics            | ⏳               | `docs/phases/phase-6-native-physics.md`  |
| 7     Editor                    | ⏳               | `docs/phases/phase-7-editor.md`          |
| 8     Domain modules            | ⏳               | `docs/phases/phase-8-domain-modules.md` (robotics, aerospace, advanced math, cinematic, procgen) |

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

### Already achieved (as of 2026-05)
- Stable Vulkan backend, GPU allocator strategy in place
- Shader system v1 with hot-reload, variant keys, pipeline handoff
- Renderer v1a–i (Clustered Forward+, frame graph, descriptor system, material system)
- Jobs foundation v1a–g (fiber pool, work-stealing deque, scheduler, counter/wait, worker pool)
- ImGui debug tooling; crd-config TOML substrate

### 6–12 months (mid-2026 to early 2027)
- `crd-jobs` v1 complete (v1h–k: public API `run()`/`wait()`, SBO lambdas, `parallel_for`, per-frame allocator)
- Resource system + asset cooker (Phase 2.6): handle table, ref-counted assets, hot-reload notifications, cooked binary format
- Scene/ECS foundation (Phase 3.0): hybrid hierarchy + SoA component storage + TOML authoring → cooked binary
- Physics integration (Phase 3.1): PhysX 5 backend + scene transform sync + fixed-step option

### 12–24 months (2027–2028)
- Animation + audio complete (Phase 3.2–3.3): skeletal, blend trees, IK, spatialized audio, DAW plugin host scaffold
- PBR + post-FX + culling (Phase 3.4–3.6): IBL, CSM, ACES tonemap, bloom, TAA, BVH frustum culling
- C++ hot-reload scripting boundary (Phase 4.0): DLL reload supervisor, stable C ABI facade, cookbook patterns
- Networking foundation (Phase 4.2): transport layer, deterministic simulation, client-server sync + rollback netcode
- `crd-ui` + node editor foundations (Phase 5.0–5.1)

### 24–36 months (2028+)
- Editor shell usable for real content authoring (Phase 7)
- Advanced math module (Phase 4.1): dense/sparse solvers, iterative methods, robust geometry
- Domain modules Phase 8 begin: robotics substrate (URDF, SE(3), ROS2 bridge), cinematic tools, procedural generation
- Advanced rendering paths: deferred + Visibility-Buffer beside Forward+ (Phase 5.2–5.3)
- First Cerid-native physics backend alongside PhysX (Phase 6)
- Plugin ecosystem (versioned C ABI, sample plugins, SDK documentation)

The goal: Cerid becomes a real-time substrate for interactive applications
across games, simulation, creative tools, and engineering — not "another Vulkan engine."

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
- **Fiber** — a cooperative-switch execution context on an OS thread stack; suspends without blocking the thread. Fibers ensure threads are never idle while runnable work exists.
- **DLL hot-reload** — recompile a shared library and swap it at runtime without restarting the process; a stable C ABI boundary prevents name-mangling and vtable issues across the reload.
- **Rollback netcode** — client-side input prediction with server-authoritative correction; the server sends authoritative state, the client re-simulates diverged frames. Canonical for action/fighting games.
- **Deterministic replay** — engine state is reproducible from an initial snapshot plus an input log; prerequisite for rollback netcode, debugging, and simulation validation.
- **ROS2** — Robot Operating System 2; the industry standard message-passing middleware for robotics. Target integration for Cerid's robotics domain module.
- **Digital twin** — real-time synchronization between a physical system (robot, aircraft, medical device) and its Cerid simulation counterpart; driven by live sensor feeds.
- **SE(3)** — Special Euclidean group in 3D; the Lie group of rigid-body transforms (rotation × translation); core to robotics kinematics and aerospace flight models.

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
