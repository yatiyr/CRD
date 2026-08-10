# Phase 4.0 — Cerid Platform Layer (Reflection · Command Interface · Scripting · Agents)

**Status:** 📋 planned — **sequenced after Phase 3.1.6 `crd-hesap` (full) + Phase 3.1 `crd-eylem` (full)**.
**Detail level:** architecture-deep, **slices provisional** (this doc executes ~2 years out; the durable content is the pillar structure, decisions, dependency order, and open questions — fine-grained slice mechanics are sketched, not pinned, and get sliced concretely when execution nears).
**Consolidates / supersedes:**
- **ADR-0081** (Agent-Native Engine: CLI + RPC + MCP) — the command-interface decision; becomes **Pillar 2** here. Not re-decided; referenced.
- **ADR-0034** (C++ hot-reload DLL scripting) — already folded into ADR-0081's `crd-script`; becomes part of **Pillar 3**.
- The old **`phase-4-extensibility.md` §4.0** (pre-ADR-0081 scripting framing) — superseded by this doc.
- The old **`phase-4-extensibility.md` §4.1** (advanced math) — **obsolete**: entirely absorbed by Phase 3.1.6 `crd-hesap`.
- **Out of scope (stays separate):** networking (old `phase-4-extensibility.md` §4.2 → re-home as its own phase). This doc is the *platform/scripting/agent* layer only.

> **Why a phase doc now, when execution is ~2 years out?** Two reasons. (1) It **consolidates five scattered half-decisions** (ADR-0081, ADR-0034, the C++-scripting pivot, the reflection idea, the packaging idea) into one coherent picture so the vision stops drifting. (2) It is **load-bearing on how we build hesap and eylem in the meantime** — knowing reflection-codegen is coming keeps the command-registration pattern consistent and reflection-friendly *as we go*, so the future "CLI back-fill" pass is a cheap annotate-and-regenerate rather than a rewrite. **hesap's hand-written CLI commands literally become the codegen's correctness fixture later.**

---

## 1. The bet: Cerid stops being an engine and becomes a platform

This phase is the layer that turns Cerid from "the engine" into "the substrate other people build apps on, *and* that AI agents operate from outside." Three capabilities converge here, and the insight that makes them one phase is that **they share a single foundation — reflection metadata:**

1. **A command interface** (CLI + RPC + MCP) — *calling* exposed engine verbs. External, language-agnostic, agent-drivable, replayable.
2. **A C++ hot-reload script system** — *authoring* native logic/behavior, ECS-integrated, packable into a shipped app. The gameplay/app layer, in the same language as the engine.
3. **A reflection/codegen substrate** — *parses annotated, exposed, marshallable C++ (functions, structs, enums, handles) and generates the bindings* that the command interface, RPC, MCP, serialization, the editor inspector, and networking all consume.

**The unifying claim:** these are not three subsystems — they are three consumers of one reflection layer, plus the runtime that hosts authored code. Build the reflection once, and CLI + RPC + MCP + serialization + inspector + replication all get cheaper at once (the Unreal-UHT lesson: one metadata system underpins Blueprints, replication, serialization, and the editor).

### The three-tier mental model (a spectrum, not silos)

```
Engine C++ (compiled in)        — fastest, the substrate, rebuilt
      │
Hot-reload script C++           — native speed, fast iteration, full API, in-process   ← Pillar 3/4
      │   (authored logic; can REGISTER commands)
Command interface CLI/RPC/MCP   — marshalled, remotable, discoverable, automatable     ← Pillar 2
      ▲
      │ all marshalled surfaces generated from …
Reflection / codegen metadata   — annotation → parse → bindings                        ← Pillar 1 (foundation)
```

Scripts can register commands (Pillar 5), so the *author* surface and the *call* surface fuse. Agents have two gears: **call existing verbs** (safe, schema'd) or **author a script** (unbounded, the notebook path).

---

## 2. Pillars

In dependency order. Each: purpose · key decisions · dependencies · provisional slice sketch · open questions.

### Pillar 1 — Reflection & codegen substrate (`crd-reflect`) — THE FOUNDATION

**Purpose.** A build-time pass that parses *annotated* C++ declarations and emits the marshalling + schema + registration code that every other pillar consumes. The single source of truth: **the C++ signature *is* the schema** — no hand-maintained divergence.

**Key decisions (proposed, ADR-candidate ADR-0084 to mint at execution):**
- **Parser:** Clang LibTooling / libclang (understands real C++ — templates, the no-STL container types, `Quantity<D,T>`), run as a CMake codegen step. Custom header parser (Unreal UHT style) rejected as higher-maintenance. **C++26 static reflection (P2996) is the eventual native replacement** — when MSVC/our toolchain ships it, the external tool retires; design the annotation surface so that migration is mechanical.
- **What it generates:** `CommandSchema` registration, `ArgValue`→typed-args unmarshalling, result→`CommandResult` marshalling, struct/enum (de)serialization, MCP tool descriptors, and a queryable runtime metadata registry (`meta.list-types`, `meta.schema-of`).
- **Marshalling scope:** functions, structs, enums, and the engine's opaque handles (`MatrixId`/`VectorId`/`EntityId`/`ComponentId` — `[generation:32|index:32]`). Nested structs, arrays, optionals, `Quantity<D,T>` (units travel in the schema!).
- **Templates:** hesap is template-heavy (`gemm<T,Layout>`, the f32/f64/c32/c64 quartet). The annotation specifies the instantiation set; the generator loops it — mirroring hesap's existing explicit-instantiation lists.
- **Quality:** generated code must pass clang-tidy/format/the DoD (clean generator, or scoped NOLINT regions).

**Dependencies:** none upstream (it's the base). Validated *against* hesap.

**Validation gate (the elegant one):** **regenerate hesap's hand-written CLI commands and diff against the manual versions.** If the generator reproduces ~290 known-good commands byte-for-byte (modulo formatting), it's correct. hesap's manual toil today is the test oracle tomorrow.

**Provisional slices:** annotation scheme + attribute parser → primitive + struct marshalling → handle + `Quantity` marshalling → command-schema emit → MCP descriptor emit → metadata runtime registry → hesap regen-and-diff validation.

**Open questions:**
- Annotation **inline** (`[[crd::expose]]`, truth-next-to-code, pollutes headers) vs **sidecar manifest** (clean headers, drifts). Unreal chose inline; Godot chose explicit bind calls. → decide at execution.
- Full-API exposure vs a **curated reflected surface** (trust/safety; see cross-cutting §4).

### Pillar 2 — Command interface: CLI + RPC + MCP (`crd-cli` + `crd-rpc`) — ≈ ADR-0081

**Purpose.** The external/agent control surface. Already specified in detail by **ADR-0081** — this pillar *executes* that ADR, now fed by Pillar 1's codegen instead of hand-registration.

**Inherited from ADR-0081 (not re-decided here):** three-layer surface inversion (CLI is source of truth, GUI emits commands); versioned introspectable `CommandSchema`; structured `CommandResult`; capability-based security; dry-run/transactions/sandbox/replay/quotas/provenance safety set; JSON-RPC 2.0 + **exact MCP compatibility** (tools/resources/prompts/sampling/roots); stdio/TCP/Unix-socket/WebSocket transports; `meta.*` introspection; `.crds` (`'CRDS'` FourCC) deterministic replay artifacts.

**What this doc adds:** the binding layer is **generated by Pillar 1**, not hand-written. The hesap protocol-plumbing (registry types, `ArgValue`, `CommandResult`, MCP descriptors — already shipped in hesap v0a) is the seed `crd-cli`/`crd-rpc` formalize.

**Registry lifecycle — scoped removal (new requirement; surfaced by the script consumer 2026-05-22).** The hesap registry today is *register-only* (`register_command`/`find`/`all` — no `unregister`), which is correct for static-init commands that live the whole process. **Hot-reload scripts and per-app command sets break that:** a `CommandImpl` is a raw function pointer into a DLL/app module; if it isn't removed before the module unloads, `find()->impl()` dispatches into freed code. So the formal registry must support **owner-scoped removal:**
- Every record carries an `OwnerId` — static-init engine commands use a sentinel `OwnerId::kEngine` (never torn down); each script DLL / app gets a fresh owner id on load.
- **Removal-stable storage:** a slot-map with generation indices (reuse the `crd-scene` ECS `[generation:32|index:32]` pattern) or hashmap-keyed records — **not** the current position-indexed `Array` (erase shifts indices and dangles the `all()` pointer cache).
- `unregister_owner(id)` bulk-removes everything an owner added (robust even if a script forgets an individual command), exposed via an RAII `ScriptCommandScope` whose destructor calls it.
- **Thread-safety invariant:** dispatch and register/unregister never overlap (the reload quiesces — Pillar 3); *no thread holds a raw `CommandImpl` across a reload/unload boundary.*

**Dependencies:** Pillar 1 (codegen). **Provisional slices:** per ADR-0081 §9 (`crd-cli` registry+parser+REPL+structured output → `crd-rpc` JSON-RPC+MCP server+capabilities+transactions+sandbox) + scoped-removal registry + `ScriptCommandScope`.

### Pillar 3 — C++ hot-reload script runtime (`crd-script`)

**Purpose.** Author native logic in C++, hot-reload it live, no restart. Scripts ARE `.crds.cpp` files compiled to hot-reloadable DLLs (ADR-0081 §9 / ADR-0034). **~~The only scripting language~~ A first-class scripting surface — no Lua/Python/GDScript** (decision locked in ADR-0081; ⚠ **superseded in part by ADR-0108 (2026-08-07): C++ is no longer the ONLY authoring surface — Cerid owns the CEIR/CHIR + CR-D007-visual language stack; the no-third-party-VM rule stands.** Rationale for the C++ surface: one language, no marshalling, full type system + debugger, deterministic FP, AI-writable).

**Key decisions:**
- **Hot-reload mechanism:** `DynamicLibrary` load → suspend `crd-jobs` → swap DLL → re-bind symbols → resume. Per-type **state-migration callbacks** across reload (the hard part — preserving live state when a struct layout changes). Reference patterns: Live++, Anvil, JetBrains C++ hot-reload, RemedyBG.
- **Iteration loop is make-or-break.** The edit→compile-DLL→hot-swap latency *is* the gameplay-iteration experience. Tiny TUs, PCH, fast linker, only-recompile-changed-cell. This is where C++-scripting wins or loses against C#/Lua mindshare — invest accordingly.
- **C ABI boundary** (old 4.0b): stable versioned C facade; all persistent state lives in engine-owned memory (handles), so a reload can't strand state in a DLL.
- **Reload protocol — commands deregister *before* the DLL unloads.** The supervisor runs: suspend `crd-jobs` (quiesce — no command dispatch in flight) → destroy each script module (its RAII `ScriptCommandScope` dtor calls `unregister_owner`, Pillar 2) → unload old DLL (safe now: the registry holds no pointers into it) → load new DLL → reconstruct modules (re-register commands under a fresh owner id) → resume. **The dangling-`CommandImpl`-into-an-unloaded-DLL is the #1 hazard;** the quiesce + deregister-first ordering is what prevents it. Command de/re-registration is *part of* state migration, not separate from it.

**Dependencies:** `crd-jobs` (suspend/resume — note the fiber asm context-switch interaction), platform `DynamicLibrary`, Pillar 2 scoped-removal registry.

**Provisional slices:** DLL load/swap/resume → symbol re-binding → state-migration callbacks → fast-iteration tooling → scripting cookbook (gameplay tick, custom layers, asset hooks).

**Open question:** in a **WASM/browser build**, hot-reload-of-WASM is exotic — scripting is the hard part of the browser story (see cross-cutting §4 + memory `project_browser_wasm_deployment_goal`). Likely: native-only hot-reload; browser uses the command interface (Pillar 2) as its primary surface.

### Pillar 4 — ECS-integrated scripting model

**Purpose.** Scripts aren't just free functions — they integrate with `crd-scene`'s 8-layer ECS just like gameplay scripts in mainstream engines (the native-C++ analog of Unity `MonoBehaviour` / Unreal components). This is what makes "write a section of your app as a script" real.

**Key decisions (to design at execution):**
- Scripts register as **ECS systems** (operate over component queries each tick) and/or define **script-components** (per-entity state + lifecycle: spawn / enable / tick / disable / destroy).
- Lifecycle + scheduling integrate with the scene's existing system scheduler and `crd-jobs` (deterministic ordering — ADR-0063).
- Script-component fields exposed to the **editor inspector** and **serialization** via Pillar 1 reflection (the same metadata that powers CLI powers the inspector — the multiplier paying off).

**Dependencies:** Pillar 1 (reflection for fields), Pillar 3 (runtime), `crd-scene`.

**Open question:** full ECS API to scripts vs a curated "gameplay-safe" reflected surface (trust vs power).

### Pillar 5 — Scripts ↔ commands (the unification)

**Purpose.** A hot-reload script can **register CLI commands** via the same Pillar-1 reflection. You author a capability in a script, and it instantly becomes agent-drivable / RPC-callable / replayable — without touching the engine core.

**Why it matters:** this fuses the *author* surface (Pillar 3/4) and the *call* surface (Pillar 2). It's also the agent superpower: an agent authors a `.crds.cpp` that exposes a new verb, and every other agent/tool can then call it. The v18-notebook idea (a cell is either a command invocation *or* a `.crds.cpp` script) is the visible form of this fusion.

**Dependencies:** Pillars 1–4. **Provisional slices:** script-side `CRD_CLI_REGISTER` working through hot-reload (re-register on reload) → schema lifecycle across reload → notebook cell = command-or-script.

### Pillar 6 — App packaging & build pipeline (`crd-pack` / build tooling)

**Purpose.** "Pack your scripts and build an app with them." The dev→ship pipeline: hot-reload DLLs during development → **baked-in compiled scripts** for a shippable, distributable app (Unreal's editor-hot-reload → cooked-build model).

**Key decisions (to design at execution):**
- Release build statically links the scripts that were hot-reload DLLs in dev (no DLL-swap machinery shipped to players → smaller, faster, safer).
- Asset + script + config packaging into a distributable (CRDR pack format; reuse `crd-resources` cooker pipeline).
- Per-platform targets (native; **WASM** as a target — ties to the browser goal).

**Dependencies:** Pillars 3–5, `crd-resources` (cooker/pack), build system.

### Pillar 7 — Agents at authoring-time AND runtime (NPCs)

**Purpose.** Agents operate at **two scopes over the same command + capability surface.** This *extends* ADR-0081, which is authoring-focused → **candidate for an ADR-0081 amendment (or a small new ADR) when this pillar nears.**

**Scope 1 — Authoring-time agents (external / cloud).** Claude / GPT / Gemini via MCP/RPC drive the editor, generate content, act as dev helpers. Mostly specified in ADR-0081 §4–§5 (capabilities, dry-run, transactions, sandbox, replay, quotas, provenance) — this pillar *hardens and references*:
- Reference **MCP server** bundled (`cerid --rpc-stdio`); reference Claude Code agent harness; agent telemetry.
- **Capability/permission model** (`CommandSchema.required_caps`) governs both command calls *and* agent-authored scripts — an agent-authored script runs with the session's capability set, not unrestricted.
- Adversarial-agent threat model: each capability gets attack-surface analysis.

**Scope 2 — Runtime agents bound to entities (NPCs, companions, helpers).** *The* forward bet: the same surface that lets a dev-time agent drive the editor lets a runtime agent **inhabit the world.** An NPC-agent **perceives** via scene/ECS queries (exposed as MCP resources) → **reasons** (a local **micro-LLM** via `crd-ml-inference` / v14 tensors / v17 GPU for low latency, or a cloud model for high-capability companions) → **acts** via a *constrained command set that IS its capability/action space* (an NPC gets `npc.move` / `npc.speak` / `npc.interact`; never `scene.delete_entity`). **Full circle: hesap's ML/tensor layer powers the model, the command registry is the action API, the scene is the perception source.**

Honest caveats (designed-in, not afterthoughts):
- **Determinism vs LLM nondeterminism.** LLM output isn't bit-deterministic, so the replay logs the agent's *chosen commands*, not the model internals — the `.crds` session captures *what the agent did*, so it replays even with a black-box model (ADR-0063 determinism preserved at the command layer).
- **Latency.** Cloud round-trips are too slow for per-frame NPC decisions → agents decide on **events / intervals**, not every frame; micro/local models for the fast paths (same latency lesson as real-time audio).
- **Safety = gameplay design.** The capability set IS the NPC's action space; sandboxing becomes a design knob (what can this NPC do?), not only a security control.

**Dependencies:** Pillars 1–2 (command surface + capabilities) + `crd-ml-inference` / v14 / v17 (runtime model backend) + `crd-scene` (perception).

### The application model — Cerid as spine, editor/game/DAW as apps

The pillars combine into one model: **Cerid is a spine; the editor, a game, a DAW are *apps on top of it*** — not the engine itself (the Godot "the editor is a Godot app" inversion, vs Unity/Unreal's editor-monolith).

- **Command ownership is layered.** The **engine** registers domain verbs (ECS `scene.entity.spawn` / `scene.component.set` / `scene.query.run`, physics, geometry, hesap) — always present. **Each app** registers its own verbs as an **owner-scoped command set** (Pillar 2 `OwnerId`) that loads on app launch and `unregister_owner`s on teardown. The editor is just an owner whose verbs (`editor.material.create`, `editor.selection.frame`, …) appear when the editor launches.
- **The GUI is an *emitter*, not the logic (the Godot × Blender synthesis).** A button decomposes into three separable things: a **UI entity** (Godot scene-tree node — UI nodes coexist in the scene tree per ADR-0020), a **command verb** (Blender operator — in the registry), and a **click→command-name binding** (the glue). The button emits the command *by name*; it never contains the logic. **Agent-native invariant: every UI action is a standalone callable verb,** reachable by hotkey / console / agent with no UI present — which is exactly what makes the editor itself agent-drivable, and (via Scope-2 runtime agents) what makes an NPC's action space just a constrained set of verbs.
- **GUI emits *committed* commands.** Discrete actions emit a command on click; continuous interactions (slider drag, viewport orbit) update state live and **commit one command on release** (the granularity rule — per-event commands would be too chatty; same lesson as real-time audio/DAW).
- **Undo/redo falls out for free.** Because every action is a reversible command (`CommandSchema.reversible` + the transaction/journal of ADR-0081 §5), **the command log *is* the undo stack** — no separate editor undo system. This is why Blender / Photoshop-class tools structure around operators.
- **Precedents:** Godot (editor-as-app; UI-in-scene-tree), Blender (every action is a `bpy.ops` operator — a full DCC application proves the pattern scales).

> **The UI/tooling system that realizes this model** — retained `crd-ui` entities, gizmos, the document-vs-tooling two-worlds split, the Logic/Visual/Command triple, screen-space-vs-worldspace rendering — is specified in **`docs/phases/phase-ui-tooling.md`**. It *consumes* this command layer (every UI action / gizmo drag emits a committed verb).

---

## 3. Dependency order & sequencing

**Within the phase:** P1 (reflection) → P2 (command interface) → P3 (script runtime) → P4 (ECS scripting) + P5 (scripts↔commands) → P6 (packaging) → P7 (agent hardening). P1 is strictly first (everyone consumes it); P2 and P3 can proceed in parallel once P1 is up.

**Within the roadmap:** **after** Phase 3.1.6 `crd-hesap` (full v0–v17/18) **and** Phase 3.1 `crd-eylem` (full v0–v9). Rationale = **consumer-driven design**: reflection-codegen needs hesap's full command corpus as its correctness fixture, and ECS-scripting needs eylem's gameplay requirements to be designed right. Building either in a vacuum risks the wrong abstraction. (This matches `feedback_ship_at_consumer_template_from_day_one` — substrate ships proactively, but consumer-specific shapes wait for the consumer.)

**Meanwhile (the cheap-now investment):** every hesap and eylem slice keeps shipping CLI per the ADR-0081 §10 DoD rule, using a consistent registration pattern, so Pillar 1's later back-fill is annotate-and-regenerate, not rewrite.

---

## 4. Cross-cutting concerns

- **Capability/permission model** — one model governs CLI calls, RPC sessions, and agent-authored scripts (`required_caps`). Narrow caps for agents on shared machines.
- **Determinism & replay** — the command log is the session document (`'CRDS'`). Commands are serializable → recordable/replayable/undoable. Scripts are replay-safe only if they mutate state through deterministic engine APIs (ADR-0063). Pull: the more behavior flows through commands, the more of the session is reproducible — but hot-loop logic can't be command-marshalled (too slow), so the boundary is partly "does this need to be in the replay log?"
- **Schema versioning/stability** — ADR-0081 §2 policy: the *command schema* is the multi-year-stable contract (versioned, deprecation windows, CI back-compat gate); the *C++ API* can churn under it. External consumers + agents couple to the stable schema; C++ scripts couple to the churning API (recompile).
- **Full-API vs curated reflected surface** — recurring trust/power decision across Pillars 1, 4, 7. Likely answer: curated reflected surface for the *exposed/agent* path, full API for *in-process scripts authored by trusted developers*.
- **Browser/WASM portability** (memory `project_browser_wasm_deployment_goal`) — the command interface (P2) is browser-native (RPC over WebSocket); C++ hot-reload scripting (P3) is the hard part in WASM (likely native-only). Two deployment flavors: compile-to-WASM-in-tab vs thin-browser-client-over-RPC, chosen per module.

---

## 5. Resolved design questions (from the 2026-05-22 brainstorm)

- **"A scripting language for the CLI?"** → **No new Turing-complete language.** Orchestration is covered by: C++ scripts calling commands (in-process), external clients driving RPC in their own language (agents/Python/browser), and at most a **thin declarative command-sequence/batch format** (for replay/automation — ADR-0081 already names YAML batch + `.crds` replay). Inventing a bespoke CLI language fights the one-language-C++ mandate and is rejected.
- **CLI granularity differs per module.** hesap is naturally CLI-shaped (clean data ops → a command per op). Stateful/per-frame modules (renderer/rhi/app) expose a curated *verb + query + config + debug-capture* surface, not a 1:1 function wrap. "Every op a command" = every *capability worth exposing/automating*.
- **Which old modules get the CLI back-fill pass:** ops-heavy ones first (geometry, meshgen, math); stateful ones get thin verb surfaces. Codegen (P1) makes this tractable.

---

## 6. Open questions (deferred to execution)

- Annotation location: inline attributes vs sidecar manifest (P1).
- Full-API vs curated reflected surface for scripts (P4) and the exposed command set (P1/P7).
- State-migration ergonomics across hot-reload when struct layouts change (P3) — the hardest runtime problem.
- WASM scripting story (P3/P6) — native-only hot-reload, or a WASM-recompile path?
- Where the notebook (ADR-0081 §9 Phase 4.0+2; v18) lives — its own phase or a pillar here.

---

## 7. Definition of done (provisional)

- Pillar 1 regenerates hesap's CLI commands and diffs clean against the hand-written set.
- An off-the-shelf Anthropic agent (Claude Code, `cerid --rpc-stdio`) drives a non-trivial end-to-end workflow over MCP.
- A sample app is authored partly as hot-reload scripts, ECS-integrated, then **packed and built into a standalone distributable**.
- A script registers a CLI command that an external agent then calls.
- A full session replays bit-exactly from its `.crds` log (ADR-0063 determinism gate).
- ADR-0084 (reflection-codegen) minted + ADR-0081 moved Proposed→Accepted on `crd-cli` v0 ship.

---

## 8. References

- **ADR-0081** — Agent-Native Engine: CLI + RPC + MCP (the command-interface decision; Pillar 2).
- **ADR-0034** — C++ hot-reload DLL scripting (folded into ADR-0081 / Pillar 3).
- **ADR-0063** — Eylem determinism contract (makes replay reproducible).
- **ADR-0078** — Units substrate (typed command params; units travel in schemas).
- **ADR-0084** (to mint) — Reflection & codegen substrate (Pillar 1).
- `docs/research/cerid-agent-native-engine.md` — vision dossier.
- Memory: `project_agent_native_engine_strategic_direction`, `project_browser_wasm_deployment_goal`, `feedback_hesap_clean_structure_over_calendar`.
- Anthropic MCP — https://modelcontextprotocol.io ; JSON-RPC 2.0 — https://www.jsonrpc.org/specification.
- Reference scripting hot-reload: Live++ (Molecular Matters), Anvil (RAD), JetBrains C++ hot-reload, RemedyBG.
- Reflection precedent: Unreal Header Tool (`UFUNCTION`/`UPROPERTY`), Godot `ClassDB`, pybind11; C++26 static reflection (P2996).
