# Cerid — Agent-Native Engine: Strategic Vision + CLI/RPC Architecture

**Date:** 2026-05-19
**Status:** Vision document — informs upcoming ADR-0081 + cross-cutting
phase-plan additions.
**Author:** Research dossier per user direction 2026-05-19.

> **The thesis.** Cerid is designed for a future where AI agents are
> peer collaborators in robotics, aerospace, cinematic, and game-design
> workflows — alongside (and increasingly ahead of) the human user.
> Every engine operation an artist, engineer, or researcher would
> perform must be reachable from a CLI / RPC surface so agents can
> drive the engine end-to-end: write shaders, generate scenes, design
> physics setups, run simulations, analyze results, iterate on
> creative direction. **This is a foundational engine principle, not a
> bolt-on feature.**

---

## 1. Why this matters (the multi-domain agent argument)

Cerid already serves six equal-class domains: games + robotics +
medical viz + DAW-class creative tools + cinematic pipelines +
scientific computing (per PRINCIPLES.md Identity table). Each of those
domains has growing AI-agent integration:

| Domain | Current AI agent integration trend (2024-2026) |
| --- | --- |
| **Games** | Generative content (NPC dialog, asset variations, level design); AI playtesting; AI-driven QA. Unity Sentis, Unreal Neural Network Engine, Roblox Studio AI Assistant. |
| **Robotics** | LLM-driven task planning (PaLM-E, RT-2, Google DeepMind); Sim-to-real policy training; Agent-authored URDF / behavior trees. Mujoco MJX + JAX agents now standard. |
| **Aerospace** | Mission-design copilots (NASA Apollo workflow modernization); trajectory-optimization assistants; SE/aerospace requirements-to-code generators. |
| **Cinematic** | Generative previs (Wonder Dynamics, Runway); shot-layout copilots; LLM-driven storyboard → blocking. Houdini + ChatGPT integrations across studios. |
| **Medical viz** | DICOM segmentation copilots; reporting AI; volumetric annotation. Lights-out diagnostic pipelines (Radiologue, Aidoc). |
| **DAW** | LLM-driven sound design (Sonauto, Suno); mixing copilots; auto-mastering. |
| **Scientific computing** | Jupyter copilots (GitHub Copilot, Cursor); Wolfram + LLM; SciML + Julia + LLM-driven scientific workflow synthesis. |

**The unifying observation:** all six domains are converging on a
pattern where the human user is gradually replaced — or significantly
augmented — by AI agents writing the scripts, configuring the runs,
analyzing the outputs, and proposing the next iteration. The
**operational interface** every one of those domains needs is the
same: a programmable, introspectable, scriptable, replayable,
sandboxed engine surface that an AI agent can drive.

**Engines that ship GUI-first and call CLI/scripting an afterthought
will lose the agent-driven workflows.** Unity, Unreal, Godot, Bevy,
Blender, Houdini — all have CLI surfaces but they are second-class
(Houdini's `hscript` is the closest to first-class; even there, the
parameter UI is the source of truth and `hscript` mirrors it).

**Cerid's bet:** the CLI/RPC surface IS the source of truth, and the
GUI is the second-class — a visualization layer rendering the engine
state and producing CLI commands when the human clicks. Every engine
operation an artist would perform via the GUI is reachable via CLI;
every CLI command can be issued by an AI agent via RPC; every
operation is introspectable, reversible, replayable.

---

## 2. Industry landscape — agent-driven creative engines (2024-2026)

### 2.1 Existing CLI / scripting surfaces in DCC tools

| Tool | CLI/scripting surface | Agent-integration grade |
| --- | --- | --- |
| **Houdini** | `hscript` + Python + VEX + HDA networks | B+ (best in class for CLI-first DCC; not designed for agents but the closest). |
| **Maya** | MEL + Python + C++ API | C (Python is the practical CLI; MEL is dated; agent integration is bolt-on). |
| **Blender** | Python (full API) + command-line `--python` flag | B (Python API is comprehensive but tied to Blender's GUI event model; headless mode works). |
| **Unreal** | `Unreal Editor Subsystem` + Python + Blueprints + console commands | C+ (Python in editor only; runtime CLI is debug-console-grade). |
| **Unity** | Editor Scripting (C#) + UnityScript (deprecated) + batch mode CLI | C (batch mode is for CI builds; not an agent-driving surface). |
| **Godot** | GDScript + C# + headless CLI | C+ (GDScript is interpretable, headless mode works, no formal RPC). |
| **Bevy** | ECS reflection + scripting via `bevy_mod_scripting` (Lua/Rhai/Rune) | B- (ECS reflection is excellent; scripting is opt-in). |
| **Notch / TouchDesigner** | Python; web-based node API | C+ (Python is the de-facto CLI; designed for live performers, not agents). |
| **Cinema 4D** | Python + COFFEE + C++ API | C |
| **Modo** | Python + Modo SDK | C |
| **Mujoco MJX** | JAX + Python + XLA | A- (designed for ML / agent training, but not a creative engine). |
| **Roblox Studio** | Lua + AI Assistant (built-in chatbot) | B (AI assistant is integrated but limited to authoring suggestions, not full agent driving). |

**The gap:** no DCC engine has been designed from the substrate up
with **AI agent as a first-class user role**. Cerid would be first.

### 2.2 Agent-driven systems outside DCC (the reference pattern)

| System | Agent interaction model | Lesson for Cerid |
| --- | --- | --- |
| **VS Code + LSP** | Language Server Protocol — every editor feature is a typed JSON-RPC call. | Schema-driven RPC is the right baseline. |
| **MCP (Model Context Protocol)** | Anthropic's typed tool-use protocol for AI agents. JSON-Schema'd commands; capability-based. | Cerid's RPC layer SHOULD be MCP-compatible — instant agent integration with Claude, GPT, etc. |
| **Anthropic computer use** | Agent gets a screen-capture + mouse/keyboard interface. | Cerid agents work at the CLI level (10-1000× more efficient than pixel UI). |
| **Cursor / Claude Code / Aider** | Agent + git + filesystem + terminal. | Same model — Cerid's CLI is the agent's primary interaction surface. |
| **Jupyter kernels** | Stateful REPL + structured I/O + introspection. | The notebook model maps perfectly to a long-running engine session. |
| **OpenAI Function Calling / Gemini Function Calling** | LLM emits JSON conforming to declared function schemas. | Every Cerid CLI command should have a JSON schema. |
| **LangGraph / CrewAI / AutoGen** | Multi-agent orchestration over typed tool calls. | Cerid agent surface should support multiple agents acting in parallel (each with scoped capabilities). |
| **GitHub Copilot Workspaces** | Agent gets a virtual machine + IDE + project state. | Cerid sandbox isolation should be VM-class. |

### 2.3 Programming-language workbenches with agent integration

- **Wolfram Mathematica + Wolfram|Alpha LLM integration** — natural-language → Wolfram language is the closest existing analog to "natural language → engine CLI."
- **Julia + LLM-aware error messages** — error reporting designed for agent self-correction.
- **MATLAB + AI Assistant** — opt-in copilot inside MATLAB Online. Agent has access to MATLAB CLI.

---

## 3. The Cerid agent-native architecture

### 3.1 Three-layer surface

```
┌──────────────────────────────────────────────────────────────┐
│   Layer 3 — GUI panels (ImGui debug; future crd-ui editor)   │
│   Generates Layer-2 commands; renders Layer-1 state.         │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ uses
                              ▼
┌──────────────────────────────────────────────────────────────┐
│   Layer 2 — CLI / scripting (text + JSON-RPC + MCP)          │
│   Every engine operation reachable; AI agents drive here.    │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ wraps
                              ▼
┌──────────────────────────────────────────────────────────────┐
│   Layer 1 — C++ engine API (the substrate; what already      │
│   exists; the source of truth for state + capability).       │
└──────────────────────────────────────────────────────────────┘
```

**Inversion vs traditional engines:** Layer 2 is the *source of truth*
for "what operations can an external user request." Layer 3 (GUI)
emits Layer 2 commands as the user clicks. Layer 1 (C++ API) is the
implementation detail that the CLI wraps.

This means: **every engine feature is built CLI-first.** If a feature
can't be CLI-invoked, it doesn't exist as far as agents are concerned.

### 3.2 Module surface — `crd-cli` + `crd-rpc` + `crd-script`

Proposed three modules (peer of `crd-app`, `crd-platform`, etc.):

| Module | Responsibility | Slot |
| --- | --- | --- |
| **`crd-cli`** | Command parser + registry + REPL + structured-output renderer + tab completion + help system. Each engine module registers its commands via a static-init `cli::register_module_commands` hook. | Future major phase (Phase 4.5 candidate, slot reserved) |
| **`crd-rpc`** | JSON-RPC 2.0 server (over stdio + TCP + Unix socket); MCP server (Anthropic agent protocol); capability-based gating; structured-error reporting; transactional sandbox sessions. | Same phase as `crd-cli`; depends on it. |
| **`crd-script`** | **C++ scripts via hot-reloaded DLLs (CrdScript format).** Cerid scripts ARE C++ files (`.crds.cpp`) compiled into hot-reloadable shared libraries. **No embedded interpreter — no Lua, no Python, no GDScript.** Supervises the DLL boundary: load + symbol re-binding + per-type state-migration callbacks (CRDR pattern). Reference architectures: Live++ (Molecular Matters), Anvil engine (RAD Game Tools), JetBrains C++ hot reload, RemedyBG. Locked 2026-05-19 per user direction. | Same phase. |

### 3.3 Command schema — every command is typed

Every CLI command has a typed declaration:

```cpp
namespace crd::cli {

struct CommandSchema {
    StringView                  name;            // "scene.spawn"
    StringView                  description;     // "Spawn an entity with components in the active scene."
    ArrayView<ParamSchema>      params;          // Typed parameters (each has name + type + optional + default + description).
    OutputSchema                output;          // Typed return type (JSON-Schema'd).
    Capability                  required_caps;   // e.g. "scene:write" (capability-based security).
    bool                        idempotent;      // Safe to re-run with same args?
    bool                        reversible;      // Has an inverse / undo?
};

}
```

The schema is **introspectable**: an agent can issue
`meta.list_commands` and receive every command's full schema as JSON.
This is the MCP / OpenAI Function Calling pattern — agents emit valid
calls because the surface is self-describing.

### 3.4 Structured output

Every command returns a typed result:

```cpp
struct CommandResult {
    bool                        ok;
    Variant<                    // tagged union of typed outputs.
        Void,
        Scalar<f64>,
        Text,
        TableRows,              // tabular data — rendered as table in TTY, JSON in RPC.
        EntityId,
        ComponentValue,
        BinaryBlob,             // e.g. cooked asset.
        StructuredError
    >                           value;
    Span<Diagnostic>            diagnostics;     // warnings / hints / cross-references.
    ProvenanceTrail             provenance;      // every operation logs origin (agent id, command id, parent transaction).
};
```

- **Plain text (TTY)** — human-readable, color-coded, table-formatted.
- **JSON (RPC)** — machine-parseable, schema-conforming.
- **YAML (script)** — for declarative batch files.
- **Binary** — for asset blobs / replay artifacts.

### 3.5 Capability-based security

Agents (and human users) operate under **capability tokens**:

- `scene:read` — read scene/entity/component state.
- `scene:write` — create/modify/delete entities.
- `assets:cook` — invoke cookers.
- `physics:simulate` — run physics steps.
- `shaders:compile` — compile/hot-reload shaders.
- `system:shell` — execute arbitrary OS commands (DENIED by default; agent must explicitly request and admin must grant).
- `fs:write:<path>` — write to a path prefix.
- `network:connect:<host>` — connect to a host.

The capability set is **explicit per session.** An AI agent on a
shared dev machine doesn't get `system:shell` unless the operator
grants it.

### 3.6 Transactional / sandboxed sessions

- **Dry-run mode** — `--dry-run` on any command shows what would
  happen without executing. Critical for agents to preview destructive
  ops.
- **Transaction** — `tx.begin` → series of commands → `tx.commit` or
  `tx.rollback`. Mirrors database semantics.
- **Sandbox session** — entire engine state forked into a sandboxed
  copy; the agent's commands operate on the copy; merging back is
  explicit.
- **Replay** — every session is logged to `.crdr` replay artifact.
  Re-running the replay reproduces the session bit-exactly (cerid
  determinism contract ADR-0063).

### 3.7 Cross-domain command examples

```
# Robotics — agent designs a robot arm.
agent> robotics.urdf.import "kuka_kr10.urdf" --to scene
agent> robotics.joint.set_limits arm.j0 --min -2.5 --max 2.5
agent> physics.simulate --duration 5s --rtol 1e-6
agent> hesap.solve_ik --target [0.5, 0.2, 0.8] --tolerance 1e-4
agent> robotics.export.trajectory --to "plan.crdr" --format crdr

# Cinematic — agent blocks a shot.
agent> cinematic.camera.create "hero_shot" --type cinematic
agent> cinematic.camera.set_curve hero_shot --path b_spline:[(0,0,0), (1,1,1), (2,0,2)]
agent> render.preview --camera hero_shot --frames 0..120

# Aerospace — agent runs a trajectory opt.
agent> aerospace.body.create "rocket" --mass 50t --thrust 800kN
agent> hesap.opt.minimize --vars [u(0..T)] --objective "fuel" --constraints "altitude(T) >= 100km"
agent> aerospace.export.dvl --to "ascent.csv"

# Game — agent builds a level.
agent> scene.import "level_01.gltf"
agent> scene.entities.list --filter "tag:obstacle"
agent> ai.behavior.assign --entities [...] --tree "patrol"
agent> game.playtest --duration 30s --bots 4 --record "playtest.crdr"
```

### 3.8 Module-level CLI surface

Every engine module ships a CLI surface alongside its C++ API.
Example: `crd-hesap-dense` exports:

```
hesap.dense.matrix.create --rows N --cols M [--layout row|col]
hesap.dense.matrix.load --from <path> --format auto|csv|npy|crdv
hesap.dense.matrix.save --to <path> --format crdv|csv|npy|matlab
hesap.dense.matrix.fill --matrix M --value V
hesap.dense.matrix.random --matrix M --distribution normal|uniform --seed S
hesap.dense.solve --A <mat> --b <vec> [--method lu|cholesky|qr|auto]
hesap.dense.eig --matrix M [--method qr|jacobi|divide-conquer]
hesap.dense.svd --matrix M [--method golub-reinsch|randomized]
hesap.bench.gemm --size N --backends [scalar,avx2,avx512,sve2]
...
```

Every command is introspectable + scriptable + replayable.

---

## 4. Determinism + replay (the Cerid superpower)

The agent-native vision **requires** determinism. Without it, agent
sessions are non-reproducible — the agent thinks `X happened` but the
human can't reproduce. Cerid's existing determinism contract (ADR-0063
+ ADR-0078 units + bit-exact SIMD/scalar) makes agent workflows
**replayable across machines and time.**

Every agent session emits a `.crdr` replay artifact (existing format,
new `'CRDS'` FourCC for command-stream). A replay artifact contains:

- The initial engine state (or a reference to a starting snapshot).
- The full sequence of CLI commands with their structured args.
- The structured outputs from each command (for verification).
- A bit-exact checksum of the final state.

Replaying:
```
cerid replay session.crdr --verify-checksum
```

If the checksum diverges, the engine has a determinism regression —
a CI gate.

This is the **engineer-platform-leader pivot in action.** Cerid is the
only engine where an agent's session is provably reproducible.

---

## 5. Integration with hesap (the immediate concrete consumer)

The `crd-hesap` substrate (Phase 3.1.6) is the **first major agent-
native consumer.** Every hesap operation an MATLAB/NumPy/Julia user
performs has a CLI command:

```
hesap.dense.gemm --A <matA> --B <matB> --alpha 1.0 --beta 0.0 --out <matC>
hesap.sparse.pcg --A <K_csr> --b <f> --tol 1e-8 --max-iter 1000 --preconditioner jacobi
hesap.ode.solve_ivp --rhs "fn:lotka_volterra" --y0 [40, 9] --tspan [0, 50] --method dopri5
hesap.opt.minimize --vars [x] --objective "fn:rosenbrock" --method lbfgs
hesap.fft.fft --input <signal> --inverse false
hesap.stats.welch_psd --signal <sig> --window hann --nperseg 256
```

Each command returns structured output (the matrix as a `MatrixId`
referencing the engine's hesap state, plus metadata: condition number,
residual norm, iteration count).

The CLI is the **first user-facing surface for hesap** — before any
GUI editor exists. This means hesap ships **agent-driveable on day
one** (v0).

A future `crd-hesap-repl` (Phase 3.1.6 v18 in the original plan) layers
the MATLAB-class facade on top of the CLI:
- `A = randn(1000, 1000)` (REPL syntax) compiles to
  `hesap.dense.matrix.random --rows 1000 --cols 1000 --distribution normal`.
- `x = A \ b` compiles to `hesap.dense.solve --A A --b b`.

---

## 6. Beyond hesap — the engine-wide rollout

Every existing module gets a CLI surface as it matures:

| Module | CLI surface |
| --- | --- |
| `crd-scene` | `scene.create / load / save / entity.spawn / component.set / query.run` |
| `crd-eylem` | `physics.body.create / collider.attach / sim.step / sim.snapshot / sim.replay` |
| `crd-geometry-*` | `geometry.aabb / closest_point / raycast / sample_curve / mesh.validate` |
| `crd-renderer` | `render.frame / render.preview / camera.set / light.set / material.update` |
| `crd-resources` | `assets.import / cook / pack / load / unload / hot-reload` |
| `crd-shader` | `shader.compile / hot-reload / list-variants / inspect-spirv` |
| `crd-rhi` + `-vulkan` + `-compute` | `gpu.list / device.info / submit / wait / profile` |
| `crd-jobs` | `jobs.dispatch / wait / stats / profile` |
| `crd-perf` | `perf.profile.start / stop / save / load / display` |
| `crd-config` | `config.get / set / list / save / load` |
| `crd-units` | `units.convert / parse / preferences.set` |

**Per-module pattern:** when a module ships a slice, it ships a CLI
surface for that slice. Existing modules need a back-fill pass —
sized as a future major phase.

---

## 7. Agent protocol — JSON-RPC 2.0 + MCP

### 7.1 JSON-RPC 2.0 surface

Every CLI command is also a JSON-RPC method:

```json
// Request
{"jsonrpc": "2.0", "method": "hesap.dense.solve",
 "params": {"A": "mat_42", "b": "vec_17"},
 "id": 1}

// Response (success)
{"jsonrpc": "2.0", "result":
 {"x": "vec_18", "residual_norm": 3.2e-9, "iterations": 12},
 "id": 1}

// Response (error)
{"jsonrpc": "2.0", "error":
 {"code": -32600, "message": "Matrix A is singular",
  "data": {"condition_number_estimate": 1e16, "diagnostic_url": "..."}},
 "id": 1}
```

### 7.2 MCP compatibility

MCP (Model Context Protocol) is Anthropic's typed-tool-use protocol
for AI agents. Cerid's RPC layer should be **MCP-compatible** out of
the box:

- Each CLI command exposed as an MCP tool with full JSON schema.
- Capability gates map to MCP's permission model.
- Streaming responses (for long-running ops: `physics.simulate
  --stream-progress`) via MCP's streaming primitives.

**With MCP compatibility, every Anthropic-aware agent (Claude Code,
Claude desktop, custom Claude SDK agents) can drive Cerid out of the
box.** OpenAI Function Calling + Gemini Function Calling are also
trivial adapters.

### 7.3 Transports

- **stdio** — for `cerid --rpc-stdio`; the agent spawns the engine as
  a subprocess. Most common for local agent integration (Claude Code
  pattern).
- **TCP** — for remote agent / shared-team server. TLS + auth token.
- **Unix domain socket** — for localhost daemon mode.
- **WebSocket** — for browser-based agent UIs / remote dashboards.

---

## 8. Safety + sandboxing for agents

AI agents make mistakes. The engine surface must be safe against
adversarial / hallucinating / confused agents:

| Safety mechanism | Description |
| --- | --- |
| **Capability tokens** | Agent has explicit, narrow capabilities (see §3.5). |
| **Dry-run** | Every destructive command supports `--dry-run`. |
| **Transactions** | `tx.begin` / `commit` / `rollback`. |
| **Sandbox sessions** | Entire engine state forked; agent's commands operate on the copy. |
| **Replay** | Every session is logged + replayable. |
| **Resource quotas** | CPU / RAM / GPU / disk-write budgets per session. Agent hits the budget → soft-fail with a structured error explaining the limit. |
| **Diff preview** | Before commit, agent (or human) can preview the diff vs the pre-transaction state. |
| **Undo / redo** | All non-destructive ops are journaled; `undo` rewinds. |
| **Confirmation prompts** | High-impact ops (file delete, force-push, schema migrate) require explicit confirmation token; agent must request `system:confirm` capability for blanket bypass. |
| **Provenance trail** | Every state change is tagged with the originating session + command + agent-id, queryable via `provenance.trace`. |

---

## 9. Phasing — how this lands

This vision is large. Decompose by module + by phase:

| Phase | Work |
| --- | --- |
| **Phase 3.1.6 v0 hesap-dense** | Ship hesap with CLI surface from day 1. CLI for hesap-dense becomes the first agent-driveable substrate. |
| **Phase 3.1.6 v1-v17 hesap** | Each slice ships its CLI surface alongside the C++ API. |
| **Phase X (new) — `crd-cli` + `crd-rpc` substrate** | The actual CLI parser + RPC server + capability system + transaction model. Sized as ~12-week phase. Probably **Phase 4.0** (replacing the original Phase 4.0 = "C++ scripting / DLL hot-reload" which folds in). |
| **Phase X+1 — module CLI back-fill** | Every existing module (scene / eylem / geometry / renderer / etc.) gets its CLI surface. Sized per-module; can run in parallel with other work. |
| **Phase X+2 — Notebook / REPL** | Layer the MATLAB-class REPL + Jupyter-compatible notebook on top. Reuses crd-hesap-repl plus the new `crd-cli` infrastructure. |
| **Phase X+3 — agent integration** | Reference MCP server bundled with Cerid; reference agent harness for Claude Code; agent-specific safety policies + telemetry. |

**Order of priority** (locked here, refined when the phase plan
formalises):

1. **Hesap CLI surface from day 1** (Phase 3.1.6 v0) — bake in the CLI
   surface alongside every BLAS / LAPACK call.
2. **`crd-cli` substrate** (new Phase 4.0) — formalise the registry +
   parser + structured output + capability system.
3. **`crd-rpc` substrate** (Phase 4.0 continuation) — JSON-RPC server
   + MCP compatibility.
4. **Per-module CLI back-fill** (cross-cutting; each module's next
   slice).
5. **Reference Claude Code agent integration** — first vertical agent
   workflow demo (e.g. "Claude designs and simulates a robot arm").
6. **Notebook / REPL** — later, when UI substrate ships.

---

## 10. Open questions

These don't block the vision but need resolution as the phase plan
formalises:

1. **Scripting language choice — RESOLVED 2026-05-19.** **C++
   hot-reload via DLL is the only scripting path.** Cerid scripts
   ARE C++ files compiled into hot-reloadable shared libraries.
   Rationale: one language for engine + scripts + tools = no
   marshaling, full type system, full debugger, deterministic FP
   (ADR-0063), no FFI overhead for AI agents. Alternative paths
   (Lua, Python, GDScript) are explicitly rejected — they fragment
   the engine, introduce GC non-determinism, require separate
   debuggers, and force a marshaling layer that AI agents would
   have to navigate. The same C++ scripts that agents author run in
   the engine's hot path with zero overhead. Reference architectures:
   Live++, Anvil, RemedyBG, JetBrains hot reload.
2. **MCP server hosting model.** Bundled with Cerid (single binary,
   one `cerid --rpc-stdio` invocation), or separate server process
   (`cerid-rpc-server` + engine as library). Bundled is simpler;
   separate is more flexible.
3. **Streaming progress for long-running commands.** WebSocket + MCP
   streaming, or stdio progress lines. MCP supports both; pick the
   safer default.
4. **Session-state persistence.** Should an agent's engine session
   survive a daemon restart? Yes via the replay artifact + state
   snapshot.
5. **Multi-agent coordination.** Multiple agents sharing one engine
   instance with overlapping capabilities — conflict resolution? Use
   transaction model + optimistic concurrency.
6. **Privacy / on-premises agent.** For aerospace / medical / robotics
   secrets, the agent must run on-premises. MCP supports this trivially
   (stdio transport, local Claude desktop, on-prem inference).

---

## 11. Why this is Cerid's strategic advantage

**No other engine is being built agent-first.** Unity, Unreal, Godot,
Bevy, Blender, Houdini — every one of them is GUI-first with CLI as
afterthought. Cerid's bet: **the next decade of creative + engineering
+ scientific work happens with AI agents as peer collaborators, and
the engine substrate that's designed for that wins.**

The locked architectural cornerstones already align:

- **Determinism** (ADR-0063) → replayable agent sessions.
- **Units everywhere** (ADR-0078) → typed cross-domain commands.
- **Module isolation** (CLAUDE.md) → narrow capability surfaces.
- **Authoring text, runtime binary** (PRINCIPLES.md) → agents author
  TOML/JSON cooked to binary; runtime never imports source.
- **Tak-çıkar** (PRINCIPLES.md) → no third-party agent harness lock-in.
- **C++ static types + reflection** (Phase 3.0 work) → schema
  introspection comes free.

**The hesap substrate is the first concrete instance.** From v0
forward, every operation has a CLI command. By the time eylem v1c
ships, the substrate for "AI agent designs a physics scene" is ready.
By the time eylem v9 (differentiable physics) ships, "AI agent trains
a diff-physics-based policy" is one MCP call away.

This is the **engineering-platform-leader pivot** (Pathway E, locked
2026-05-15) brought to its full conclusion.

---

## 12. Action items

- **Mint ADR-0081** locking the agent-native engine + CLI/RPC
  architecture decisions. Status: Proposed → Accepted when `crd-cli`
  v0 ships.
- **Refine Phase 3.1.6 hesap plan** to bake in CLI surface from v0
  (see companion doc `cerid-hesap-2026-update.md`).
- **Add to PRINCIPLES.md** a new cornerstone: "Agent-native engine —
  every operation reachable from CLI/RPC; replayable; sandboxed."
- **Add to ROADMAP.md** the new agent-native phase line + cross-cuts.
- **Memory entry** flagging the user's 2026-05-19 strategic direction:
  agent-native as a load-bearing pillar.

## References

- Anthropic Model Context Protocol (MCP) — https://modelcontextprotocol.io
- JSON-RPC 2.0 spec — https://www.jsonrpc.org/specification
- Language Server Protocol (LSP) — Microsoft 2016+
- OpenAI Function Calling docs / Gemini Function Calling
- VS Code architecture docs (extension API + LSP)
- Houdini hscript reference
- Bevy ECS reflection (bevyengine.org)
- Mujoco MJX + JAX integration docs
- Roblox Studio AI Assistant launch (2023+)
- Jupyter Kernel Protocol spec
- LangGraph / CrewAI / AutoGen architecture docs (multi-agent orchestration)
