# ADR-0081 — Agent-Native Engine: CLI + RPC + MCP Substrate

**Date:** 2026-05-19
**Status:** **Proposed** (Accepted when `crd-cli` v0 ships)
**Tags:** [arch] [cli] [rpc] [mcp] [agent] [scripting] [substrate] [vision]
**Supersedes:** ADR-0034 (C++ scripting / DLL hot-reload — folded in
as the C++ hot-reload sub-aspect of the broader agent-native vision).
**Superseded-by:** ADR-0108 (2026-08-07) — §9's "C++ is the ONLY scripting path" clause ONLY
(in-file strike executed 2026-08-10 at the first CEIR vertical slice); §1-§8 reaffirmed verbatim.

## Context

Cerid's multi-domain mandate (games + robotics + medical viz + DAW +
cinematic + scientific computing) faces a convergent trend: **AI
agents are becoming peer collaborators in every domain.** Anthropic
Claude / OpenAI GPT-class agents now:

- Author URDFs and behavior trees in robotics workflows.
- Generate previs blocking + camera curves in cinematic pipelines.
- Synthesize shaders, scenes, and game logic on demand.
- Run scientific computing workflows end-to-end in Jupyter / MATLAB.
- Author audio plugin chains in DAW workflows.

Existing engines (Unity, Unreal, Godot, Bevy, Blender, Houdini) ship
**GUI-first** with CLI / scripting as second-class. Houdini's
`hscript` is the closest existing analog to "CLI as a first-class
control surface," but even there the parameter UI is the source of
truth and `hscript` mirrors it.

**The bet:** the next decade of creative + engineering + scientific
work happens with AI agents as peer collaborators. **The engine
substrate that's designed for that wins.** Cerid's bet is to build
**agent-native from the substrate up** — CLI/RPC is the source of
truth, GUI panels are visualization layers that emit CLI commands
when the human clicks.

Vision dossier: `docs/research/cerid-agent-native-engine.md`.

## Decision

Cerid ships an **agent-native engine architecture** with three
modules:

- **`crd-cli`** — Command registry + parser + REPL + structured output.
- **`crd-rpc`** — JSON-RPC 2.0 server + Anthropic MCP (Model Context
  Protocol) compatibility + capability-based security + transactional
  sessions.
- **`crd-script`** — C++ scripts via hot-reloaded DLLs. **No
  embedded interpreter language.** Cerid scripts ARE C++ files
  compiled into hot-reloadable DLLs (the "CrdScript" format —
  `.crds.cpp` files). Same language as the engine itself = no
  marshaling, full type system, full debugger, deterministic.
  Supervises hot-reload boundary: DLL load + symbol re-binding +
  per-type state migration callbacks. Reference patterns: Live++
  (Molecular Matters), Anvil engine (RAD Game Tools), JetBrains
  C++ hot reload. This is the architecture ADR-0034 originally
  pinned, now elevated to the *only* scripting path.

Every existing and future Cerid module ships a **CLI surface
alongside its C++ API**. From v11 (geometry) close 2026-05-19 forward,
every new slice ships CLI commands as a Definition-of-Done item.

### 1. Three-layer surface inversion

```
Layer 3 — GUI / panels (ImGui debug + future crd-ui editor)
            ▲
            │ emits commands
            ▼
Layer 2 — CLI / scripting (text + JSON-RPC + MCP)    ← THE SOURCE OF TRUTH
            ▲
            │ wraps
            ▼
Layer 1 — C++ engine API
```

**Inversion vs traditional engines.** Layer 2 is the source of truth
for "what operations can an external user request." The GUI emits
Layer-2 commands; agents call Layer-2 directly; C++ code wraps
Layer-1 behind Layer-2.

### 2. Command schema (typed, introspectable, VERSIONED)

Every CLI command has a typed declaration:

```cpp
struct CommandSchema
{
    StringView          name;             // "hesap.dense.solve"
    StringView          description;
    SchemaVersion       version;          // {major, minor}; bumps on breaking change
    ArrayView<ParamSchema> params;        // typed args
    OutputSchema        output;           // typed result
    Capability          required_caps;
    bool                idempotent;       // safe to re-run?
    bool                reversible;       // has an inverse?
    DeprecationStatus   deprecation;      // Active / Deprecated{since, replaced_by} / Removed{when}
};
```

The schema is **introspectable**: `meta.list_commands` returns full
JSON-Schema for every registered command. This matches MCP / OpenAI
Function Calling / Gemini Function Calling — agents emit valid calls
because the surface is self-describing.

**Schema versioning policy** (load-bearing for agent stability):

- **Major version bump** = breaking change to params or output. Old
  schema stays registered for ≥ 2 minor versions with `Deprecated`
  status before removal. Replacement command name is advertised in
  the deprecation metadata.
- **Minor version bump** = additive (new optional param, additional
  output fields). Old agent scripts continue to work.
- **Backwards-compat CI test**: any schema removed before its
  deprecation window expires triggers a CI failure.
- **Schema export**: `meta.export-mcp-tools` produces a versioned
  MCP tool catalog suitable for committing alongside agent prompts
  (so an agent's prompt references schema vN, and a Cerid update
  doesn't silently break that agent's scripts).

This is the discipline that lets AI agents author scripts they can
trust across Cerid versions.

### 3. Structured output

Commands return typed results:

```cpp
struct CommandResult
{
    bool                ok;
    Variant<Void, Scalar<f64>, Text, TableRows, EntityId,
            ComponentValue, BinaryBlob, StructuredError>
                        value;
    Span<Diagnostic>    diagnostics;     // warnings / hints
    ProvenanceTrail     provenance;
};
```

Renderers per transport:
- TTY → human-readable colored / tabular.
- JSON-RPC → typed JSON.
- YAML → declarative batch scripts.
- Binary → CRDR-format `.crds` replay artifacts.

### 4. Capability-based security

Sessions hold a capability set. Examples:

- `scene:read` / `scene:write`
- `assets:cook`
- `physics:simulate`
- `shaders:compile`
- `system:shell` (DENIED by default; admin grants explicitly)
- `fs:write:<path-prefix>`
- `network:connect:<host>`

AI agents on shared dev machines get narrow capabilities. The agent
must request `system:confirm` capability for blanket bypass of
confirmation prompts.

### 5. Safety mechanisms

| Mechanism | Purpose |
| --- | --- |
| **Dry-run** | `--dry-run` on every command shows what would happen without executing. |
| **Transactions** | `tx.begin` / `commit` / `rollback`. |
| **Sandbox session** | Engine state forked; agent operates on the copy. |
| **Replay** | Every session logged to `.crds`; replay reproduces bit-exactly. |
| **Resource quotas** | CPU / RAM / GPU / disk-write budgets per session. |
| **Diff preview** | Before commit, view state diff. |
| **Undo / redo** | Non-destructive ops journaled. |
| **Provenance** | Every change tagged with session/command/agent-id; `provenance.trace` queries the lineage. |

### 6. Protocols

#### JSON-RPC 2.0

```json
{"jsonrpc": "2.0", "method": "hesap.dense.solve",
 "params": {"A": "mat_42", "b": "vec_17"}, "id": 1}
```

#### Anthropic MCP (Model Context Protocol)

MCP is Anthropic's typed-tool-use protocol for AI agents. Cerid's RPC
layer **must be MCP-compatible out of the box** — every CLI command
exposed as an MCP tool with full JSON schema. **With MCP
compatibility, every Anthropic-aware agent (Claude Code, Claude
desktop, custom Claude SDK agents) can drive Cerid out of the box.**
OpenAI Function Calling + Gemini Function Calling are also trivial
adapters.

#### Transports

- **stdio** — `cerid --rpc-stdio`; agent spawns engine as subprocess.
- **TCP** — for remote / shared-team server (TLS + auth token).
- **Unix domain socket** — localhost daemon mode.
- **WebSocket** — browser-based agent UIs / remote dashboards.

### 6.4 MCP exact compatibility (mandatory)

Per user direction 2026-05-19, **Cerid environment must be exactly
MCP-compatible**, not "MCP-inspired" or "MCP-adjacent." Concretely:

- Every CLI command is exposed as an MCP **tool** with JSON Schema
  conforming to the latest stable MCP spec.
- Engine state (scenes, entities, components, assets) is exposed as
  MCP **resources** (URI-addressable; agents can read).
- Common workflows are exposed as MCP **prompts** (templated
  multi-step recipes; agents can replay).
- Long-running ops support MCP **sampling** (stream intermediate
  results).
- Capability set maps to MCP **roots** (per-session permission
  boundaries).

**`crd-rpc` ships a reference MCP server.** Stdio + TCP + Unix
socket + WebSocket transports. An off-the-shelf Anthropic agent
(Claude Code / Claude Desktop / Claude SDK) configured with `cerid
--rpc-stdio` as an MCP server gets full Cerid surface access.

MCP spec evolves (stabilized late 2024; ecosystem accelerating
through 2025-2026); `crd-rpc` keeps spec compliance in a thin adapter
layer, so spec drift is one-module work.

### 7. Module surface (registration pattern)

Every Cerid module registers CLI commands via a static-init hook:

```cpp
// engine/hesap-dense/src/cli_register.cpp
CRD_CLI_REGISTER_MODULE("hesap.dense", [](cli::Registry& r) {
    r.command("matrix.create")
        .description("Create a dense matrix")
        .param("rows", ParamKind::U32, "number of rows")
        .param("cols", ParamKind::U32, "number of columns")
        .param("type", ParamKind::Enum("real|complex"), "scalar type", "real")
        .param("precision", ParamKind::Enum("f32|f64"), "precision", "f64")
        .param("layout", ParamKind::Enum("row|col"), "layout", "row")
        .output<MatrixId>()
        .requires(Capability::HesapWrite)
        .idempotent(false)
        .impl([](const CommandArgs& args) -> CommandResult {
            // ... thin wrapper over the C++ API.
        });
    // ... more commands
});
```

Static-init registers automatically when the module's static library
is linked.

### 8. Determinism + replay (Cerid superpower)

Per ADR-0063 (units + bit-exact SIMD/scalar + deterministic FP) +
ADR-0078 (typed units): every CLI session is reproducible across
machines + time. Every session emits a `.crds` (CRDR `'CRDS'` FourCC)
replay artifact:

- Initial engine state snapshot (or ref).
- Full sequence of CLI commands with args.
- Structured outputs from each command (for verification).
- Bit-exact checksum of final state.

`cerid replay session.crds --verify-checksum` — if checksum diverges,
determinism regression flags a CI gate.

### 8.5 Engine-wide CLI surface catalog (the full scope)

The CLI is engine-native — every domain reachable. Categories below
inform per-module slice planning; each module owns its commands and
registers them via §7 pattern.

| Category | Representative commands |
|---|---|
| **Hesap (Phase 3.1.6 — first concrete consumer)** | `hesap.dense.matrix.{create,load,save,fill,random,eye,diag}`, `hesap.dense.{blas1,blas2,blas3}.*`, `hesap.solver.{lu,cholesky,qr,ldlt,solve,solve_iter_refine}`, `hesap.sparse.*`, `hesap.iter.{cg,pcg,bicgstab,gmres,minres,lsqr,idr}`, `hesap.eig.*`, `hesap.opt.*`, `hesap.ode.*`, `hesap.fft.*`, `hesap.dsp.*`, `hesap.stats.*`, `hesap.tensor.*`, `hesap.autodiff.*`, `hesap.bench.*` |
| **Asset pipeline** | `assets.import`, `assets.cook`, `assets.pack-crdr`, `assets.validate`, `assets.diff-cooked`, `assets.migrate-schema`, `assets.index`, `assets.search` |
| **Build / CI** | `build.cmake-configure`, `build.build`, `build.test`, `build.bench`, `build.sweep`, `build.per-slice-dod`, `build.format`, `build.tidy`, `build.package` |
| **Editor ops** | `editor.undo`, `editor.redo`, `editor.save-scene`, `editor.load-scene`, `editor.new-project`, `editor.open-project`, `editor.build-playable`, `editor.record-macro`, `editor.replay-macro` |
| **Profiler / debug** | `perf.start`, `perf.stop`, `perf.save-trace`, `perf.replay-trace`, `debug.dump-state`, `debug.inspect-entity`, `debug.inspect-component`, `debug.breakpoint-on-event` |
| **Logging** | `log.filter`, `log.tail`, `log.save`, `log.dump-channel`, `log.replay` |
| **Reflection / introspection** | `meta.list-commands`, `meta.list-types`, `meta.list-components`, `meta.list-events`, `meta.list-systems`, `meta.schema-of`, `meta.capabilities`, `meta.export-mcp-tools` (auto-generate the MCP tool catalog) |
| **Documentation** | `doc.generate-from-registry`, `doc.export-md`, `doc.export-html`, `doc.export-mcp-spec` |
| **AI agent ops** | `agent.session-start`, `agent.replay-session`, `agent.inspect-provenance`, `agent.grant-capability`, `agent.revoke-capability`, `agent.quota-set` |
| **Migration / versioning** | `schema.migrate`, `asset.upgrade`, `crdr.format-bump`, `replay.replay-old-format` |
| **Resource budgets** | `quota.set-cpu`, `quota.set-ram`, `quota.set-gpu`, `quota.monitor`, `quota.kill-on-exceed` |
| **Scripting / workflow** | `script.run`, `script.chain`, `script.loop`, `script.if-then-else`, `script.save-as-macro`, `script.compile`, `script.hot-reload` |
| **Notebook (later)** | `cell.eval`, `cell.restart-kernel`, `notebook.export-as-html`, `notebook.export-as-pdf` |
| **Scene / ECS** | `scene.create`, `scene.entity.spawn`, `scene.component.set`, `scene.query.run`, `scene.prefab.instantiate`, `scene.obek.unpack` |
| **Eylem (physics)** | `physics.body.create`, `physics.collider.attach`, `physics.sim.step`, `physics.sim.snapshot`, `physics.sim.replay` |
| **Geometry** | `geometry.aabb`, `geometry.closest_point`, `geometry.raycast`, `geometry.sample_curve`, `geometry.mesh.validate`, `geometry.transform.*` |
| **Renderer** | `render.frame`, `render.preview`, `render.camera.set`, `render.light.set`, `render.material.update` |
| **Resources** | `resources.import`, `resources.cook`, `resources.pack`, `resources.load`, `resources.unload`, `resources.hot-reload` |
| **Shaders** | `shader.compile`, `shader.hot-reload`, `shader.list-variants`, `shader.inspect-spirv` |
| **RHI / GPU** | `gpu.list`, `gpu.device.info`, `gpu.submit`, `gpu.wait`, `gpu.profile` |
| **Jobs** | `jobs.dispatch`, `jobs.wait`, `jobs.stats`, `jobs.profile` |
| **Config** | `config.get`, `config.set`, `config.list`, `config.save`, `config.load` |
| **Units** | `units.convert`, `units.parse`, `units.preferences.set` |
| **Network / multi-user (future)** | `net.lobby-create`, `net.join`, `net.sync-authority`, `net.replicate-state` |
| **Animation** | `anim.keyframe-edit`, `anim.retarget`, `anim.timeline.scrub` |
| **Audio / DSP** | `audio.mixer.set`, `audio.fx-chain.add`, `audio.master.set` |
| **Cinematic** | `cine.camera.create-curve`, `cine.shot.block`, `cine.render.queue` |
| **Robotics** | `robotics.urdf.import`, `robotics.joint.set-limits`, `robotics.sensor.config`, `robotics.traj.optimize` |
| **Aerospace** | `aero.body.create`, `aero.mission.plan`, `aero.traj.optimize`, `aero.export.dvl` |
| **Game-specific** | `game.ai.behavior-tree.edit`, `game.save.create`, `game.save.load`, `game.playtest.record` |
| **XR / VR** | `xr.rig.config`, `xr.hand-track.config`, `xr.room.setup` |
| **ML / training** | `ml.data-pipeline`, `ml.train`, `ml.checkpoint.save`, `ml.checkpoint.load`, `ml.eval` |

**This catalog is non-exhaustive.** Each domain's exact command set
crystallizes when that domain's substrate ships. The discipline:
when a slice ships a feature, the slice also ships its CLI commands.

### 9. Phasing

| Phase | Work |
| --- | --- |
| **Phase 3.1.6 v0 hesap-dense** (immediate next slice — user direction 2026-05-19) | Hesap ships **CLI PROTOCOL PLUMBING ONLY** from v0: typed command schemas registered, structured-output infrastructure, MCP-tool-descriptor generation, `CommandRegistry` static-init pattern. **No actual CLI parser / REPL / RPC server in v0.** The protocol plumbing means crd-cli (when it lands later) inherits 100% of hesap's command surface "for free." |
| **Phase 3.1.6 v1-v17 hesap** | Same protocol-plumbing pattern per slice. Every BLAS / LAPACK / sparse / FFT / opt / AD entry registers its schema. |
| **Phase 4.0 — `crd-cli` + `crd-rpc` + `crd-script` substrate (sequenced later per user direction 2026-05-19)** | Formalize the registry + parser + REPL + structured output + capability system + JSON-RPC + **MCP exact compatibility** + transactional sessions + sandbox. `crd-script` ships C++ hot-reload supervisor (DLL boundary, symbol re-binding, state migration). ~12 weeks. Replaces the original Phase 4.0 (which was just C++ scripting / DLL hot-reload — now subsumed). |
| **Phase 4.0+1 — module CLI back-fill** | Every existing module (scene / eylem / geometry / renderer / etc.) registers its commands via the same protocol-plumbing pattern. Cross-cutting; per-module slices ship over time. |
| **Phase 4.0+2 — Notebook / REPL** | C++-hot-reload-cell notebook (`.cnb` CRDR format) on top of `crd-cli`. Cells contain C++ snippets; cell eval = compile to DLL → hot-reload → call → render output. Plot integration via crd-renderer. |
| **Phase 4.0+3 — agent integration reference** | Reference MCP server bundled with Cerid; reference Claude Code agent harness; agent telemetry; agent-specific safety policies + reference scripts ("Claude designs a robot arm", "Claude blocks a shot", "Claude solves a structural FEM problem"). |

**Order of priority (locked 2026-05-19 user direction):**

1. **Hesap v0-v17 with CLI protocol plumbing per slice** (Phase 3.1.6 — the immediate work).
2. **Eylem v1c-v9 resume** (Phase 3.1 — consumes geometry from day 1, may consume hesap-dense for v1f-articulation onward).
3. **`crd-cli` + `crd-rpc` + `crd-script` substrate** (Phase 4.0 — the formalization).
4. **Per-module CLI back-fill** (cross-cutting).
5. **Notebook + Claude Code agent reference** (later).

**Note on scripting language (locked 2026-05-19 user direction):**

> ⛔ **SUPERSEDED IN PART by ADR-0108** (Accepted 2026-08-07; this in-file strike executed 2026-08-10 at the first
> CEIR vertical slice, CEIR-13z). ONLY the "C++ is the ONLY scripting path / other paths explicitly rejected" clause
> is superseded: Cerid now owns a CEIR/CHIR + CR-D007-visual executable-program language stack, and C++ stays a
> first-class native + hot-reload authoring surface — no longer the *only* one. The third-party-VM rejection (no
> embedded Lua/Python/JS/GDScript runtime) **STANDS** — CHIR is Cerid's own language. §1-§8 of this ADR are untouched.

~~**C++ hot-reload is the ONLY scripting path.**~~ Cerid scripts may be C++
files compiled into hot-reloadable DLLs (still supported). No embedded Lua / Python / JavaScript
/ GDScript / WrenScript / Roblox-Luau (this third-party-VM rejection STANDS). Reasons the C++-path is valued:

- One language for engine + scripts + tools = no marshaling, full
  type system, full debugger, deterministic FP.
- C++ hot-reload via DLL boundary is mature (Live++, Anvil, JetBrains
  hot-reload, RemedyBG).
- Cerid's existing CRDR format + container-discipline + no-STL-types
  rules make the DLL boundary tractable.
- AI agents emit C++ scripts → `script.compile` → hot-reload DLL →
  observe results. Same loop as a human C++ engineer; agents
  benefit from the same compile-time errors + debugger.

~~Other scripting paths (Lua, Python wrapped by C-API, etc.) are~~
~~**explicitly rejected**. Future revisitation would require a new~~
~~ADR; not anticipated.~~ **[Superseded by ADR-0108 — this IS the "new ADR"
the last sentence required: Cerid-owned CEIR/CHIR + CR-D007 visual are now
first-class authoring surfaces alongside C++. Embedded third-party VMs stay rejected.]**

### 10. Per-DoD CLI surface requirement (cross-cutting)

From the v11 (geometry) close 2026-05-19 forward:

**Every new module slice's Definition of Done includes shipping a CLI
surface for its public operations.** No new slice closes without CLI
coverage. This back-fills the existing modules over time as they
ship new slices.

## Consequences

**Positive:**

- Every engine operation is reachable from CLI / RPC; AI agents can
  drive end-to-end.
- MCP compatibility = instant integration with Claude Code, Claude
  desktop, Anthropic SDK agents, OpenAI Function Calling agents,
  Gemini Function Calling agents.
- Cerid sessions are replayable across machines + time
  (deterministic).
- The GUI becomes a thin visualization layer; primary engine work
  happens via Layer-2.
- Capability-based safety + transactional sessions + sandbox
  isolation = safe agent execution.
- Cerid's strategic moat vs Unity / Unreal / Godot / Blender / Houdini
  who are all GUI-first with CLI as bolt-on.

**Negative:**

- **Phase 4.0 is a major investment** (~12 weeks for `crd-cli` +
  `crd-rpc`).
- **Existing modules need CLI back-fill** — per-module sized; spreads
  over many future slices.
- **Static-init registration order** can be tricky; mitigated by
  Cerid's discipline of explicit `register_module_commands(Registry&)`
  per module.
- **Output stability** — once an agent depends on a JSON schema, we
  can't break it. Schema versioning + backwards-compat policy
  required.
- **Security testing** — adversarial agents are a new threat model.
  Each capability needs documented attack-surface analysis.

**Insertion point:**

- **Immediate (Phase 3.1.6 v0):** hesap-dense ships with CLI surface
  from day 1. First proof point.
- **Phase 4.0:** the formal substrate (`crd-cli` + `crd-rpc`).
- **Cross-cutting:** every slice from now on ships CLI as part of DoD.

## References

- `docs/research/cerid-agent-native-engine.md` — vision dossier.
- `docs/research/cerid-hesap-2026-update.md` — hesap CLI surface as
  the first concrete consumer.
- Anthropic Model Context Protocol (MCP) — https://modelcontextprotocol.io
- JSON-RPC 2.0 spec — https://www.jsonrpc.org/specification
- Language Server Protocol (LSP) — Microsoft 2016+
- OpenAI Function Calling / Gemini Function Calling docs.
- VS Code architecture (extension API + LSP) — Microsoft.
- Houdini `hscript` reference — SideFX.
- Bevy ECS reflection — bevyengine.org.
- Mujoco MJX + JAX agent training.
- Roblox Studio AI Assistant (2023+).
- Jupyter Kernel Protocol spec.
- LangGraph / CrewAI / AutoGen multi-agent orchestration.
- ADR-0034 — C++ hot-reload DLL scripting (superseded; folded in).
- ADR-0063 — Eylem determinism contract (the substrate that makes
  agent sessions replayable).
- ADR-0078 — Units substrate (typed cross-domain command params).
