# Cerid Engine — Agent Rules

> Single source of truth for agents and humans working on Cerid.
> Auto-loaded by OpenCode every session. Concise by design — the master plan
> lives in `docs/ROADMAP.md`, the current state lives in `context.md`.

## What is Cerid

Cerid is a **general-purpose C++20 real-time engine substrate**. Games are
one consumer; **simulation (incl. robotics), medical visualization,
DAW-class creative tools, and offline cinematic pipelines** are equal-class
consumers. The architecture serves all of them; no domain is privileged.

Modules in scope: core, log, memory, containers, math, platform, app,
config, RHI (API-agnostic, Vulkan first), GPU memory, shader system,
renderer, jobs (thread pool + fibers), resources, asset cooker, physics
(PhysX backend → Cerid-native), scene, animation (skeletal + IK +
cinematic), audio (DAW-grade), scripting (hot-reload C++), advanced math
(dense / sparse / iterative solvers), retained-mode UI, node editor,
editor application.

## Engineering Principles (non-negotiable)

These are the architectural compass. Every slice respects them; deviations
are explicit, justified, and recorded in `docs/ROADMAP.md` Section 4.

- **Modular by default.** Every subsystem is a separable module with a
  clear public surface. Builds that don't need physics/animation must be
  able to omit them at link time.
- **Vertical slice over horizontal completeness.** Walk a small path
  end-to-end before widening. The first triangle gate is the canonical
  example.
- **Authoring text, runtime binary.** Human-edited data is text (TOML /
  JSON / glTF). Engine-consumed data is cooked binary. Runtime never
  imports source assets.
- **One-way module dependencies.** Cycles are bugs. Don't introduce a new
  edge without surfacing it for review.
- **Real workload before optimization.** No SIMD, fiber, GPU-allocator,
  ECS rewrite, or render-path swap without a measured baseline and target.
- **API stable across backends.** Public surfaces (RHI, physics, audio,
  render path) are designed assuming multiple implementations even when
  only one exists. Vendor types do not leak.
- **Tak-çıkar (plug-out) third-party.** External dependencies (PhysX,
  glslang, ImGui) are integrated as backends behind Cerid-owned interfaces.
- **Determinism is a first-class option.** Not the default, but reachable:
  fixed-step physics, deterministic random, replay-friendly event log.
- **Every shipped slice ends green on Debug + Release + ASan.** Three
  flavours. No exceptions.
- **The engine is allowed to be slow before it is allowed to be wrong.**

## Tech Stack

- C++20, no compiler extensions
- CMake 3.25+ with Ninja generator (CMakePresets.json)
- MSVC 2022 (primary), clang-cl (verified), Linux GCC (in CI)
- Test framework: Catch2 v3 (via CPM.cmake)
- Format: clang-format (`.clang-format` in repo root)
- Lint: clang-tidy (`.clang-tidy` in repo root)
- MSVC `/Zc:preprocessor` is required (for `__VA_OPT__` in log macros)
- Config substrate: `toml++` (single-header, exceptions-free mode)

## Build & Test

- Configure: `cmake --preset win-debug`
- Build: `cmake --build --preset win-debug`
- Test: `ctest --preset win-debug`
- ASan: `cmake --preset win-asan` then build + test
- Tidy: `cmake --preset win-tidy` then build
- Format: `clang-format -i <file>`
- clang-tidy: `clang-tidy -p build/win-debug <file>`

## Project Structureengine/<module>/
include/crd/<module>/    public headers (.hpp for C++, .h for C-only)
src/                     implementation
tests/<module>/            Catch2 tests + CMakeLists.txt
runtime/                   startup skeleton
runtime/examples/          per-module smoke demos (smoke_log, smoke_memory, ...)
runtime/configs/           authoring TOML configs (imgui_layer.toml, ...)docs/ROADMAP.md            phase plan, decision log, detour queue, open debt
context.md                 live "where we are now" state (project root)
docs/sessions/             one file per session (YYYY-MM-DD-<slug>.md)
docs/systems/              one short overview per shipped module
docs/research/             research logs
docs/decisions/            (future) per-decision ADR files
docs/<module>/<MODULE>_FILE.md  long-form deep-dive for major modules
docs/bench/                benchmark baselines

## Coding Standards

### Naming (enforced by clang-tidy)

| Element        | Style       | Example                        |
| -------------- | ----------- | ------------------------------ |
| Namespace      | lower_case  | `crd`, `crd::detail`           |
| Class / Struct | CamelCase   | `LogManager`, `Vec3`           |
| Enum / value   | CamelCase   | `LogLevel::Trace`              |
| Function       | lower_case  | `platform_name()`              |
| Variable       | lower_case  | `max_size`                     |
| Member         | lower_case  | `m_name` (m_ prefix)           |
| Constexpr var  | lower_case  | `default_capacity`             |
| Global const   | CamelCase   | `kMaxLogFiles` (k prefix)      |
| Template param | CamelCase   | `ValueType`                    |
| Macro          | UPPER_CASE  | `CRD_ASSERT`, `CRD_OS_WINDOWS` |

### Style (enforced by .clang-format)

- Allman braces (opening brace on its own line)
- 4-space indent, no tabs
- 120-char column limit
- Pointer alignment: left (`int* p`)
- `#pragma once` for headers
- Include order: project `"..."` first, `<crd/...>` second, `<...>` third

### Hard rules

- RAII only. No raw `new`/`delete`. Use smart pointers.
- `std::span`, `std::string_view`, `std::optional` over raw pointers.
- `noexcept` on moves and destructors.
- `[[nodiscard]]` on factories, accessors, and any function where ignoring
  the return is a bug.
- Concepts/`requires` over SFINAE.
- No `using namespace` in headers. Forward-declare where possible.
- No commented-out code. No TODOs unless explicitly asked.
- Performance-critical paths: data-oriented design, SIMD where measured
  beneficial.
- Use `CRD_ASSERT` / `CRD_VERIFY` for assertions.
- Containers take `IAllocator*` as a constructor argument, not a template
  parameter.

## Architectural Cornerstones

These are pinned decisions from `docs/ROADMAP.md` Section 4. Don't
re-litigate them in regular sessions; if circumstances change, open an ADR
or a `@heavy` escalation.

- **Render path:** Renderer v1 ships **Clustered Forward+** behind an
  `IRenderPath` interface. Deferred and Visibility-Buffer paths land later
  as additional implementations, not as replacements.
- **Culling:** Frustum culling in v1 → BVH-accelerated when scene grows →
  Hi-Z occlusion later. Per-light culling is part of clustered Forward+.
- **Scene + ECS:** **Hybrid model.** SoA component storage for cache-friendly
  iteration; hierarchical scene tree for traversal/authoring. Not pure ECS,
  not naive scene graph. Entity = id + components.
- **UI in the scene tree:** Godot-style. Spatial nodes (3D) and Control
  nodes (UI) coexist as children of the same scene root. UI is part of the
  scene, not an overlay. Composited at frame end via separate render
  layers.
- **Physics tak-çıkar:** PhysX is the first backend behind a Cerid-owned
  `crd-physics` interface. `Px*` types do not leak. Cerid-native backend
  arrives in Phase 6, alongside parity tests.
- **Authoring vs runtime:** Configs and scenes are authored in TOML;
  scenes are cooked to binary for runtime. Configs are parsed directly
  (small, not hot-path).
- **ImGui's role:** Debug-only forever. After `crd-ui` ships, ImGui never
  grows into editor or game surfaces. ImGui's `imgui.ini` is not replaced
  by Cerid config — Cerid TOML sits above it (theme, panels, toggles).
- **Reference counting split:** Generic intrusive ref-counting in
  `crd-memory`. Resource-facing shared references (eviction, lazy loading,
  hot-reload ownership) in `crd-resources`.

## Definition of Done

1. Compiles clean — no warnings (`/WX`, `-Werror`).
2. clang-tidy and clang-format clean for changed files.
3. New code has unit tests. All existing tests pass.
4. Three-flavour quality pass: Debug + Release + ASan all green.
5. Runtime/smoke path is checked when relevant. For graphics/platform slices,
   validation-layer errors, backend runtime errors, and avoidable warnings in
   the smoke path are treated as real quality issues, not ignorable noise.
6. Public API change → `context.md` updated; `docs/systems/<module>.md`
   updated when relevant.
7. Architectural decision → appended to `docs/ROADMAP.md` Section 4 with
   appropriate tag(s).
8. Commit message follows Conventional Commits
   (`feat(<module>): ...`, `fix(<module>): ...`, ...).

## Module Status (snapshot)

Authoritative table is in `docs/ROADMAP.md` Section 1. This snapshot is
informational only.

| Module           | Status |
| ---------------- | ------ |
| `crd-core`       | ✅     |
| `crd-log`        | ✅     |
| `crd-memory`     | ✅     |
| `crd-containers` | ✅     |
| `crd-math`       | ✅     |
| `crd-platform`   | ✅     |
| `crd-app`        | ✅     |
| `crd-rhi`        | ✅     |
| `crd-rhi-vulkan` | ✅     |
| `crd-config`     | ⏳     |

The module dependency graph is one-way and curated. Surface any new edge
before adding it.

## Session Re-entry Prompt (master prompt)

Use this when starting the next session and asking the assistant to pick up
from where the project left off:

```text
You are working on the Cerid Engine. Read context surgically.

# Mandatory reads (always, in this order):

1. AGENTS.md — project rules, agent roster, coding standards.
2. docs/PRINCIPLES.md — engineering principles + pinned cornerstones.
3. context.md — live state, last shipped, next up, active detour, pointers.
4. docs/ROADMAP.md — small hub. Read fully (it's tiny).

# Then ONE phase doc

From context.md "Current focus" you'll know the active phase. Open ONLY
that one phase file under docs/phases/. Do NOT read other phase files.

# Lazy-load everything else

- A specific past decision → docs/decisions/README.md tag index → fetch
  ONLY the matching ADR file(s).
- Last session detail → the file linked under "Last shipped milestone".
- Module surgery → docs/systems/<module>.md, then deep-dive only if needed.
- Open debt picking → docs/debt.md.
- Detour rules → docs/detours/README.md.

# Session start ritual

After mandatory reads:

1. Five-bullet summary:
   - Last shipped milestone (one line + session file ref)
   - Current focus (phase + slice from context.md)
   - Active detour, if any
   - Top 1–3 items in "Next up"
   - Open questions blocking progress

2. Ask, in priority order:
   a. "Continue with the planned next item: <name>?"
      Propose a concrete plan for THIS session. Wait for my approval
      before implementation.
   b. If I want a detour: ask for title/why/scope/exit, create
      docs/detours/D-NNN-<slug>.md, update context.md "Active detour".
   c. If undecided: surface 2–3 candidates from "Next up" or "Open debt".

3. Honor PRINCIPLES.md throughout. Don't re-litigate cornerstones — those
   are pinned. Cornerstone change = new ADR or @heavy escalation.

# Session end ritual

When I say session-end:
1. Write docs/sessions/YYYY-MM-DD-<slug>.md
2. Update context.md (last shipped, next up, test counts, active detour,
   session log entry)
3. Architectural decision → new ADR file under docs/decisions/, plus an
   entry in docs/decisions/README.md tag index AND chronological table
4. Slice flipped status → update docs/ROADMAP.md status table AND the
   phase file's slice status
5. Phase finished → archive note at top of phase file
6. NEVER run git commit / push. Propose Conventional Commits message in
   chat; I commit myself.
```

## Detour Queue

Side missions that interrupt the main roadmap. The detour queue is
`docs/ROADMAP.md` Section 5; the active detour is named in `context.md`.

Rules:

- A detour pauses the main roadmap; `context.md` records "Active detour: D-N".
- Each detour has: title, why, scope, exit criteria.
- Run detours as their own mini-pipelines (research → coder → tester →
  reviewer → docs-keeper). Same DoD applies.
- When done: `@docs-keeper` closes the detour. If it changed architecture,
  it moves to the decision log; otherwise just a session log entry. The
  main roadmap then resumes.
- Detours that grow beyond their exit criteria become real phase slices —
  promote them, don't let them quietly take over.

To open a detour: tell `@planner` "I want a detour for X" and provide the
four fields. It will register the detour and run it.

## Session expectations

- Keep compile warnings at **zero**.
- For graphics/platform slices, check runtime behavior and validation output,
  not just compile/test success.
- If a smoke/example reveals a real runtime issue, fix it or document exactly
  why it is intentionally deferred.

## Documentation Conventions

- **`docs/ROADMAP.md`** — Master plan. Phases, decision log (tagged),
  detour queue, open debt, glossary, conventions. Append-only decision log;
  targeted edits to status table and detour queue.
- **`context.md`** — Live "where we are now" state at project root. Short.
  Updated by `@docs-keeper` at session end. Old session details go to
  `docs/sessions/`, not here.
- **`docs/sessions/YYYY-MM-DD-<slug>.md`** — One per session. Format from
  `SESSION_TEMPLATE.md` if present.
- **`docs/systems/<module>.md`** — Short overview per shipped module.
  Plain English. Stable; sessions tell the story, overviews describe the
  result.
- **`docs/research/YYYY-MM-DD-<slug>.md`** — Research logs from
  `@researcher`. Format from `RESEARCH_TEMPLATE.md` if present.
- **`docs/decisions/`** — Future home for per-decision ADRs once the
  decision log in ROADMAP exceeds ~50 entries.
- **`docs/<module>/<MODULE>_FILE.md`** — Long-form deep-dive for major
  modules. Only updated when explicitly requested.
- **`docs/bench/`** — Benchmark baselines, one file per snapshot.

After a system has shipped, **prefer adding to its session log over
rewriting its overview**.

## Platform

- Windows 11, PowerShell 7.
- Use PowerShell-compatible commands. Avoid `cat`, `grep`, `sed`, `rm -rf`.
  Use `Get-Content`, `Select-String`, `Remove-Item -Recurse -Force`
  (cautiously).

## Git Policy

- **Agents NEVER run `git commit` or `git push`.** The user commits
  themselves.
- Agents may freely run `git status`, `git log`, `git diff`, `git show`,
  `git branch`.
- Conventional Commits: `feat(<module>): ...`, `fix(<module>): ...`,
  `refactor(<module>): ...`, `docs(<module>): ...`, `test(<module>): ...`,
  `build: ...`, `ci: ...`.
- When an agent finishes, it proposes a commit message in chat. The user
  runs commit themselves.
