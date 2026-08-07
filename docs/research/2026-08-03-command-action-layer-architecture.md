# Research — the Command / Action Layer: one agent-native backbone for every Cerid surface

> **Status:** **research only (direction pinned)** — the unified command/action layer is a strategic direction (memory `project_command_layer_unified_action_interface`); no implementation phase scheduled yet. *(stamped 2026-08-07, doc-hygiene pass)*

> Status: RESEARCH / DESIGN DOSSIER (pre-phase). User-directed 2026-08-03: build the command/action layer as its
> OWN phase (after crd-ui + REN + MED + the hesap-GPU return, before the D007 editor). It must be gold-standard,
> frontier-2026, and the single backbone that (1) drives the D007 editor AND every app built on Cerid, (2) exposes
> a **reflection-style command-generation system** so MCP tools/CLI/UI are derived from one declaration, and (3) is
> **extensible by Cerid devs AND downstream app developers** — anyone can register commands that ride engine parts
> so agents can do anything the engine can do.
>
> North-star cornerstone: **ADR-0081 (agent-native engine)** + PRINCIPLES.md — *the CLI / JSON-RPC / MCP surface is
> the source of truth; the GUI is a visualization layer that emits commands when a human clicks.* This dossier is
> the architecture that makes that sentence real. Sequencing memory: `project_phase_sequencing_pivot`,
> `project_command_layer_unified_action_interface`, `project_agent_native_engine_strategic_direction`.

---

## 1. The thesis (why this is one substrate, not four features)

The editor, the REPL (MATLAB-class hesap console), the CLI, the JSON-RPC server, and the MCP/agent surface are **the
same thing viewed five ways** — every one of them is a *front-end that issues commands and renders state*. Build
them as five features and you build (and debug, and version) five action vocabularies. Build **one command/action
layer** and:

- a human dragging a gizmo, a scientist typing in the REPL, `crd-cli scene.transform.set …`, an RPC call, and Claude
  issuing an MCP tool call **all hit the identical path**;
- **undo/redo falls out** of command inversion (you don't design it per-feature);
- **agent-drivability is automatic** — the whole ADR-0081 bet;
- **everything is testable headless** (a command has a test without a window — decisive for a zero-defect engine);
- **determinism/replay** comes along (ADR-0063): a session IS a replayable command log;
- **N front-ends over 1 backbone** — a game scene editor, a DAW timeline, a medical/CAD inspector are *panels over
  the same commands*, which is exactly what a five-domain general-purpose engine needs.

The command layer is therefore the **editor backbone**. The GUI is thin.

## 2. Requirements (non-negotiable)

1. **One declaration → every surface.** A command is declared once; CLI parser, JSON-RPC method, MCP tool schema,
   editor UI hints, REPL binding, and undo inverse are DERIVED. (This is the "reflection-style generation" ask.)
2. **Anyone can register commands.** Cerid core modules, downstream app developers, and hot-reload C++ scripts
   (ADR-0081: C++ is the only scripting language) all register commands that *ride existing engine public APIs* —
   no engine change needed to give an agent a new ability.
3. **Typed by construction (ADR-0078 two-layer).** Command params are `Quantity<D,T>` at the surface; the generated
   schema carries UNITS. An agent cannot send `mph` where `m/s` is wanted; the dimensional check is compile-time on
   our side and schema-declared on the wire. This is a genuine frontier differentiator over every prior art below.
4. **Invertible / transactional / replayable.** Undo/redo, transactional sessions (all-or-nothing), deterministic
   replay of a command log.
5. **Capability-scoped + sandboxed (ADR-0063/0081).** Every command declares the capabilities it needs; an agent
   session runs under a capability set + sandbox; destructive ops are flagged (MCP `destructiveHint`).
6. **Schema-versioned + backwards-compatible (ADR-0081 §2).** Major/minor per command; deprecation window; a CI
   guard fails the build if a schema is removed before its window expires; `meta.export-mcp-tools` produces a
   committable, versioned MCP catalog.
7. **Portable across MSVC / clang-cl / GCC** — see §5, the reflection-mechanism decision (this is the hard part).
8. **Zero owning STL, `IAllocator*`, `crd::containers` only** (engine rule) — the registry and command records live
   in engine containers.

## 3. Prior art studied (reference implementations are the floor)

| System | What it gets right (adopt) | What we diverge from |
|---|---|---|
| **Blender operators (`bpy.ops`)** | THE model: every user action is an `Operator` with a namespaced `bl_idname` (`mesh.subdivide`), RNA-registered **properties used as args AND saved for undo/redo AND auto-added to the UI**; an `'UNDO'` flag makes Blender auto-create the undo step; `execute`/`invoke`/`modal` split (headless vs interactive vs drag). This is exactly "one declaration → args + undo + UI." | Blender's undo is largely global-state snapshotting; we prefer **command inversion** first, snapshot as the fallback (§4e). Python-first; we are C++-native. |
| **Unreal reflection (UHT + `UFUNCTION`/`UPROPERTY`)** | Compile-time **harvest** of annotated declarations powers detail panels, serialization, GC, network replication, and Blueprint/C++/Python exposure — one annotation, many consumers. Python decorators mirror the macros 1:1. Proof that codegen-from-annotation scales to a AAA engine. | UHT is a bespoke pre-build tool + a heavy macro layer; we want the *lightest* portable mechanism (§5), not a custom header parser we maintain forever. |
| **Godot `ClassDB` / `bind_method`** | **Pure-C++, no custom preprocessor**: `ClassDB::bind_method(D_METHOD("name","arg"), &Class::fn)` registers a callable + arg names + defaults at static-init, and GDScript/C#/GDExtension all ride it. This is the portable reflection model that works on MSVC TODAY. | Arg types are Variant (dynamic); we keep **static `Quantity<D,T>` typing** and generate the schema from the C++ types, not a dynamic Variant. |
| **Qt meta-object (`moc`)** | Signals/slots/properties via a codegen step; introspectable at runtime. | Another bespoke codegen tool; same reason as UHT. |
| **VS Code commands + contribution points** | A flat **command registry** (`commands.registerCommand(id, fn)`) + a `package.json` contribution manifest; the command palette, keybindings, and menus are all *views over the registry*; extensions add commands. The "editor is a thin shell over a command registry" proof. | JS/JSON; no typing/undo guarantees. We add typing, undo, capabilities. |
| **LSP / JSON-RPC** | A versioned, transport-agnostic request/response + notification protocol with capability negotiation — the wire shape for our RPC surface. | — (adopt largely as-is for the RPC transport). |
| **Anthropic MCP** | The agent surface: a tool = `name` + `description` + `inputSchema` (JSON Schema, `type:object`, `properties`, `required`) + **annotations** (`readOnlyHint`/`destructiveHint`); `additionalProperties:false` for safety; "80% of server quality is the schema." This is the TARGET our reflection layer emits. | MCP has no units, no undo, no transactions — we *enrich* the schema (units in `description`/format, effect + capability annotations) and keep the richer semantics engine-side. |
| **CQRS / event-sourcing** | Separate **commands (intent, mutating)** from **queries (read-only)**; persist the command/event log as the source of truth → replay, audit, time-travel. Directly gives us determinism/replay + the read/write split. | Full event-sourcing is heavy; we take the command-log + read/write split, not a mandatory event store for all state. |
| **Command pattern + Memento** | The textbook: a command object with `execute`/`undo`; memento for state capture when inversion is impractical. | — (this IS the atomic unit; §4a). |
| **ROS 2 actions / services** | **Long-running, cancellable, progress-reporting** operations (actions) vs immediate (services) — the model for a bake, a long render, a solve, a sim step (robotics is a first-class Cerid consumer). | ROS-specific transport; we take the *action semantics* (async + progress + cancel), not the middleware. |
| **ECS command buffers** | Deferred, ordered, batched structural mutations applied at a sync point — how engine-internal command application stays data-race-free with the fiber-jobified scene. | An implementation detail of application, not the public vocabulary. |

**The synthesis:** Blender's operator ergonomics + Godot's portable-C++ registration + Unreal's one-annotation-many-
consumers + MCP's schema-as-contract + CQRS's command-log + ROS's async actions + Cerid's `Quantity<D,T>` typing and
capability/determinism cornerstones. Nobody in the reference set has *units in the agent schema* or *deterministic
replay + capability sandbox on the same command layer that also generates the MCP catalog* — that's the frontier gap
Cerid fills.

## 4. The Cerid architecture

### 4a. The Command — the atomic unit
A `Command` is a typed, serializable, capability-scoped intent that **rides an existing engine public API** (it never
contains business logic; it calls the module). It carries:
- **id** — namespaced, Blender-style: `scene.entity.transform.set`, `hesap.solve.lu`, `render.frame.capture`.
- **params** — a POD-ish struct of `Quantity<D,T>` (+ enums, ids, handles). The param struct IS the schema source.
- **kind** — `Query` (read-only, no undo, `readOnlyHint`) vs `Action` (mutating). CQRS split at the type level.
- **effect** — declared: what it reads / writes / creates / destroys (drives ordering, `destructiveHint`, and the
  editor's dirty-tracking) — mirrors how our frame graph already declares reads/writes.
- **capabilities** — the set required (e.g. `scene.write`, `fs.write`, `gpu.dispatch`).
- **inverse** — how undo happens: (a) an *inverse command* (preferred — e.g. `transform.set(old)`), or (b) a
  *memento* (snapshot) when inversion is impractical, or (c) `NonUndoable` (explicit, e.g. a capture-to-disk).
- **apply(ctx) / undo(ctx)** — the executors, receiving a session context (allocator, capability set, target world).

### 4b. The registry + the reflection mechanism (the portable core)
Commands register at static-init or on module/script load into a central `CommandRegistry` (Godot ClassDB model),
keyed by id, holding the reflected schema. Registration is one macro/one call:

```cpp
CRD_COMMAND(scene_transform_set)                       // id "scene.entity.transform.set"
    .summary("Set an entity's world transform")
    .capability(Cap::SceneWrite)
    .param<EntityId>("entity", "target entity")
    .param<Vec3<Length<f32>>>("position", "world position")   // ← units ride the type
    .param<Quat<f32>>("rotation", "world rotation")
    .action(&scene_transform_set_apply, &scene_transform_set_undo);
```

⛔ **Reflection mechanism decision (the crux).** C++26 static reflection (P2996) was voted into C++26 (Sofia,
Jun 2025) — but **MSVC, our primary compiler, has no support in 2026** (only GCC trunk + Bloomberg's clang-p2996
fork), and P3294 code-injection is later still. Therefore the portable-NOW mechanism is **macro/builder registration
(Godot/Unreal-lite)**: the `.param<T>(name, doc)` calls capture name + C++ type + doc, and a small compile-time
trait maps each `T` → (JSON-Schema type, unit string, range) — no bespoke header-parser tool (unlike UHT/moc),
works on MSVC/clang-cl/GCC today, and stays `crd::containers`-only. **Migration path:** when MSVC ships P2996, the
`.param<T>()` list can be *derived* from the param struct via static reflection (drop the redundant names), and the
same registry/schema-emitter downstream is unchanged. Design the registry API so P2996 is a *source-of-declarations*
swap, not a rewrite. (Study→recipe candidate: `docs/recipes/` when built.)

### 4c. One declaration → every surface (the generation)
From the reflected command the layer emits, with no duplicate authoring:
- **MCP tool** — `name`=id, `description`=summary(+units), `inputSchema`= JSON Schema from the params
  (`additionalProperties:false`), `annotations`= `{readOnlyHint: kind==Query, destructiveHint: effect.destroys}`.
  `meta.export-mcp-tools` (ADR-0081) writes the versioned catalog.
- **CLI** — `crd-cli <id> --param value` parsed against the same schema (crd-cli parser substrate, Phase 4.0).
- **JSON-RPC** — method `<id>`, params validated against the schema; LSP-style capability negotiation.
- **Editor UI hints** — param widgets (a `Length` field shows the user's unit preset; an `EntityId` shows a
  picker) — Blender's "properties auto-added to the UI," but unit-aware via ADR-0078's `UnitPreferences`.
- **REPL binding** — the hesap MATLAB-class console calls commands as functions; tab-completion from the registry.
- **Undo inverse** — from the declared inverse/memento.

### 4d. Extensibility (the "anyone can add commands" requirement)
Three producers, one registry:
1. **Cerid core** — each module registers its commands in its own TU (scene, render, hesap, eylem, …).
2. **Downstream app developers** — link a Cerid app, call `CRD_COMMAND(...)` in their code; their commands appear in
   the same CLI/RPC/MCP/editor/REPL. Namespacing prevents collisions (`myapp.foo.bar`).
3. **Hot-reload C++ scripts** (ADR-0081) — a `.crds.cpp` registers commands on load and *unregisters on unload*
   (the registry supports live add/remove; the MCP catalog re-exports). This is how an agent gains a NEW ability at
   runtime without an engine rebuild: a script (possibly agent-authored) rides engine parts and exposes them.

⛔ A command rides PUBLIC engine surfaces only; the capability system + sandbox bound what a downstream/scripted
command can touch, so "agents can do whatever they like" is *within a declared, revocable capability set* — power
with a leash (ADR-0063 determinism + ADR-0081 capability security).

### 4e. Execution model
- **Sessions + transactions** — commands run in a session; a transaction groups N commands all-or-nothing; commit
  pushes ONE undo entry (Blender-style grouping).
- **Undo/redo** — inverse-command first; memento fallback; a bounded undo stack per session.
- **Deterministic replay** — the session's command log is the source of truth (CQRS); replay reconstructs state
  bit-exactly under the FP contract (ADR-0063) — the basis for tests, bug repro, and agent-session audit.
- **Async actions** — long ops (bake, render, solve, sim) are ROS-style actions: submit → progress notifications →
  result, cancellable; they ride crd-jobs (fibers). MCP long-running/streaming maps here.
- **Application safety** — structural mutations apply through an ECS-command-buffer-style deferred sync point so the
  fiber-jobified scene stays race-free.
- **Capability gate + sandbox** — the session's capability set is checked per command; a violation is a typed
  refusal, never a silent no-op.

### 4f. The picture
```
   D007 editor panels ─┐   REPL (hesap) ─┐   crd-cli ─┐   JSON-RPC ─┐   MCP (agents) ─┐
                       └──────────────┬──┴────────────┴────────────┴──────────────────┘
                              CommandRegistry  (id → reflected schema + apply/undo)
                                             │  (one declaration, generated surfaces)
                              Session / transaction / undo / replay / capability
                                             │
                              engine public APIs: scene · render · hesap · eylem · media · …
```

## 5. Cerid-specific divergences (pin as Dxxx in the phase ADR)
- **D-A: macro/builder registration now, P2996 later.** MSVC has no C++26 reflection in 2026; the builder API is the
  portable source-of-declarations, swappable to static reflection without downstream change.
- **D-B: units in the wire schema.** Param types are `Quantity<D,T>`; the emitted MCP/CLI/RPC schema carries the SI
  unit + accepts unit-tagged input (`"65_mph"` → normalized to SI at the boundary, ADR-0078). No prior art does this.
- **D-C: inversion-first undo, memento fallback** (vs Blender's snapshot-first) — smaller undo entries, replayable.
- **D-D: capability-scoped + deterministic by construction** — the same layer that generates the agent catalog also
  enforces the sandbox and produces the replay log; agent power is leashed and auditable.
- **D-E: C++-only extension** — no scripting VM; downstream + hot-reload C++ scripts register commands (ADR-0081).

## 6. Open questions / risks (decide in the phase plan; call `advisor`)
1. **The command/raw-API boundary** — what earns a command vs staying a plain C++ call? Heuristic: anything a human,
   agent, CLI, or undo needs is a command; hot-path inner math is not. Needs a written rule to avoid registry bloat.
2. **Reflection portability cost** — the trait mapping `T → schema/units` must cover the type zoo (Quantity, Vec/Mat,
   ids, enums, handles, arrays); getting P2996-migration-clean without over-engineering the builder now.
3. **Auto-inverse coverage** — which commands get a mechanical inverse vs must hand-write `undo` vs are `NonUndoable`;
   a gate that a mutating command declares one of the three (no silent non-undoable).
4. **Dispatch performance** — command dispatch is not a hot path (per-interaction/per-RPC), but async actions on
   crd-jobs and the ECS-command-buffer application need care; measure, don't assume.
5. **Schema-version churn + the CI backwards-compat guard** (ADR-0081 §2) — how strict, how the deprecation window is
   enforced, how the committed MCP catalog is diffed.
6. **Security depth** — capability granularity, sandbox boundaries for hot-reload/agent-authored commands, and the
   destructive-op confirmation UX (editor) vs `destructiveHint` (agent).

## 7. Phasing & prerequisites
- **Sequence (user-confirmed 2026-08-03):** crd-ui → finish REN band → MED band → **return to hesap, close the GPU
  part** (harvests D-007's CKIR/GPU-compute for its original purpose) → **Command/Action Layer (this dossier, own
  phase)** → **D007 editor** (thin front-end over the layer) + the hesap REPL panel.
- **Prereqs:** `crd-cli` parser substrate (Phase 4.0, ADR-0081); `crd-ui` retained-mode (ImGui is debug-only forever,
  ADR-0023 — the real editor cannot grow on it); a stable set of engine public APIs to ride (REN/MED/hesap done).
- **Deliverables of the layer phase:** `crd-command` module (registry + schema emitter + session/undo/replay +
  capability gate); the MCP-catalog exporter + the CI backwards-compat guard; CLI + JSON-RPC front-ends; the schema
  ADR + the "what is a command" rule doc; a study→recipe in `docs/recipes/`.
- **Own new ADRs:** the command-layer architecture (this dossier → an Accepted ADR), plus the schema-versioning +
  capability model if not fully covered by ADR-0081.

## 8. First vertical slice (make it tangible)
`scene.entity.transform.set` end-to-end: select an entity → drag a gizmo → the drag emits the command → **undo**
restores it → the **identical** command is issuable from `crd-cli`, JSON-RPC, and as an MCP tool (with units in the
schema) → a hot-reload C++ script registers `myapp.transform.nudge` and it appears in all five surfaces without an
engine rebuild. That one slice exercises registry, reflection→schema, units, gizmo→command, inversion/undo,
capability gate, and agent-drivability — the whole backbone in miniature. (Buildable on ImGui for the throwaway UI
while `crd-ui` matures.)

---

## Sources
- C++26 static reflection (P2996) status: [Glaze P2996 docs](https://stephenberry.github.io/glaze/p2996-reflection/) · [bloomberg/clang-p2996](https://github.com/bloomberg/clang-p2996) · [Reflection in C++26 (P2996) — Learn Modern C++](https://learnmoderncpp.com/2025/07/31/reflection-in-c26-p2996/) · [C++26 Reflection for large codebases](https://www.wholetomato.com/blog/cpp26-reflection-large-codebases/)
- Blender operators / RNA / undo: [bpy.types.Operator](https://docs.blender.org/api/current/bpy.types.Operator.html) · [bpy.ops](https://docs.blender.org/api/current/bpy.ops.html)
- Unreal reflection: [Unreal Property System (Reflection)](https://www.unrealengine.com/en-US/blog/unreal-property-system-reflection) · [UFunctions](https://dev.epicgames.com/documentation/unreal-engine/ufunctions-in-unreal-engine) · [Reflection System in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/reflection-system-in-unreal-engine) · [UE Blueprint libraries in Python](https://medium.com/@joe.j.graf/building-ue4-blueprint-function-libraries-in-python-746ea9dd08b2)
- MCP tool schema: [MCP Tool Schema Design Guide 2026](https://kansei-link.com/en/insights/mcp-tool-schema-design-guide-2026.html) · [MCP Tools concept](https://modelcontextprotocol.info/docs/concepts/tools/) · [MCP overview (Codilime)](https://codilime.com/blog/model-context-protocol-explained/)
- (From training, not re-searched: Godot ClassDB/bind_method, Qt moc, VS Code command registry, LSP/JSON-RPC, CQRS/event-sourcing, Command+Memento patterns, ROS 2 actions/services, ECS command buffers.)
