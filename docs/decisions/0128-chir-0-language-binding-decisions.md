# ADR-0128 — CHIR-0: the Cerid high-level language, binding decisions against the corpus

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Status:** **ACCEPTED** (2026-09-06, advisor-approved under the CEIR gold-standard autonomous cadence) — the D-007
**CEIR band 32 (CHIR + language prototype)**, slice **CEIR-32a**. This ADR is the binding decision the CEIR-0e CHIR-0
design NOTE explicitly deferred ("the binding CHIR decisions are made by a future ADR at CEIR-32, *after* the CEIR
corpus exists to correct this design with real evidence"). The corpus now exists (§128–§143: renderers, hesap, ML,
`ceir.dist`, `ceir.audio`, UI effects), so this ADR BINDS — against cited proof programs, not re-derivation. The user's
commit ratifies (ADR-0108 §7 mold; no consent gate; NO AI co-author trailer).

**Phase:** D-007. **Law:** mission §4 (why CHIR is above CEIR), §98 (feature set), §19 (ownership), §10 (text/visual
projection), proof §143, DoD §180/§181. **Parents:** ADR-0108 (the owned language stack + non-negotiables), ADR-0109
(CHIR lowers/erases into CEIR; the one-way layer contract; the §6 semantic-identity model), CEIR-0e (the CHIR-0 design
note this binds), ADR-0114 (stable semantic identity), ADR-0116 (typed time domains + capability model), ADR-0110
(native intrinsics/FFI).
**Tags:** `[chir]` `[ceir]` `[language]` `[ownership]` `[module-edges]` `[substrate]` `[north-star]`

---

## 1. Context

CHIR is the **Cerid High-level IR / language layer** — the source-language semantics that must NOT be baked into the
execution IR (§4): the §98 feature set (modules, generics, ADTs, closures, traits, ownership, async/state/events,
reflection). It **lowers or erases into CEIR** (ADR-0109 §3, one-way). CEIR-0e weighed the load-bearing question
(ownership, §5), sketched a syntax direction (§6), and deferred every binding decision to *this* ADR so CHIR-1 would
start from reviewed design + corpus evidence rather than a blank page or syntax optimized for programs nobody wrote.

That corpus now exists. This ADR binds the CHIR-0 decisions the language prototype (CEIR-32b–32e) implements, and
sites the module. It is deliberately a **decisions** document: it fixes ownership, scope, module placement, lowering
targets, and the identity rule. It contains **no grammar** — the concrete syntax is CEIR-32c's parser + a
`docs/systems/chir.md`; 0e §6 already carries illustrative sketches. A grammar frozen in an ADR is a spec nobody
updates.

## 2. The runnable proof this ADR must make lowerable (§143)

```
on event:
    query entities
    parallel update
    await async task
    update state
```

Authored two ways — CHIR **text** and a **graph-schema document** — both lowering through the SAME CHIR→CEIR path to
the SAME CEIR module, then hot-reloaded. Every decision below is judged by whether it makes this program lower
*correctly and identically from both projections, and survive a reload*.

## 3. Decisions

### D1 — Scope: CHIR-0 v1 = the §143 five constructs ONLY

§181 says "defined," and 0e §3 says v1 need not ship §98. CHIR-0 v1 binds exactly five *semantic* constructs (stated
as lowering requirements, not syntax), plus real diagnostics and source spans on every node. Everything else in §98 →
the §7 deferral ledger, each with a trigger. **Gold-standard-narrow over broad-shallow: a full language is not a band.**

| §143 construct | CHIR-0 semantic requirement | CEIR lowering target | binding note |
|---|---|---|---|
| **`on event`** (event handler) | an event-handler declaration is a `func.func` carrying a **typed time-domain attr** (ADR-0116), host-invoked when the event fires | `func.func` + domain attr — **NO new `event` op** (grep-verified: no `event` dialect/op exists in CEIR; the §66/§67 `ceir.event`/`ceir.state` roadmap names are NOT built, and the handler needs neither) | zero-new-ops (the 31b-0 discipline) |
| **`query entities`** | an ECS query binds a typed entity view for the body | the ECS bridge (`register_ecs(ctx) → Systems`, `test_ecs.cpp`) | v1 uses the existing registration bridge; a richer query surface → ledger |
| **`parallel update`** | a data-parallel loop over the query result; **⛔ the body is STATE-FREE** (may not read/write a state cell) | `task.parallel_for` (host job provider, §38) | the state-free rule is **verifier-enforced** (`check_parallel_region` → `ExecError::ParallelBodyStateful`); see D2 |
| **`await async task`** | launch an async task, suspend until it completes | `async.launch` + `async.await` (§37) | v1 binds launch/await only; race/cancel/join → ledger |
| **`update state`** | write persistent state; **runs SEQUENTIALLY, outside any `parallel` body** | `core.state` StateEdge cell (§20) | the cell's stable id is **pinned from the source declaration** (D3) so it survives reload |

### D2 — The `parallel`-body rules: STATE-FREE + SELF-CONTAINED (verifier-enforced), and the query-view capture prerequisite

`task.parallel_for` pre-flight (`check_parallel_region`, exec.cpp) imposes TWO constraints the CHIR `parallel for`
inherits, both verifier-enforced today:
- **STATE-FREE:** the body AND its resolved callees must be StateEdge-free — a state cell inside the loop would make the
  result depend on the range split, destroying the {1..16}-worker bit-identity (the §118 oracle) → `ParallelBodyStateful`.
  So in §143, `parallel update` (pure per-entity compute) and `update state` (the cell write) are **two sequential
  statements**, never a state write inside the parallel body. CHIR-0 surfaces this as a checked rule (the ownership model
  D4 doing real work at a language boundary).
- **SELF-CONTAINED:** the body sees only its block-arg (`%iv`) + body-local defs; ⛔ **outer captures are NOT seeded** (a
  captured value is `UndefinedValue` at execution — task.ceirop.toml:19; "capture-seeding is a named later refinement").

⛔ **The query-view landmine this exposes:** §143's `parallel update` iterates the ECS **query result** — an outer value
(the ECS bridge passes component storages as block args/operands, `test_ecs.cpp build_world`). Because the parallel body
is self-contained, the entity view does **not** reach the body as lowered today. So the §143 `parallel update` lowering
**REQUIRES a way for the parallel body to read the query result** — either capture-seeding (the named refinement) or a
self-contained component-storage read keyed by `%iv`. **This is a NAMED CEIR-32d prerequisite**, not a free lowering: 32d
must land the seed/read path before the §143 parallel-update line lowers correctly. The ADR binds the requirement; 32d
picks capture-seeding vs an index-keyed storage read against what the ECS bridge exposes.

### D3 — The reload-survival rule: CHIR PINS stable ids from the source declaration

⛔ **The load-bearing correctness decision.** `Context::assign_stable_ids` assigns ids **sequentially in pre-order**
(`StableId{next++}`, monotone watermark; context.cpp:341-363) to any op whose id is unset — it does NOT derive an id
from content. So a re-lowered module after a text edit would *re-number* every node, and CEIR-10a's `contract_hash`
would see a different state schema and say **Reject** (callers break), not **Migrate** — the "same semantic program after
an edit" claim silently fails exactly at hot reload (§143's fifth requirement).

**Decision (the REQUIREMENT; the exact id-space mechanism is a bounded CEIR-32e call):** the reload-relevant unit is the
**state schema** — `(stable_id, type, depth)` per StateEdge cell, sorted by stable id (CEIR-10a `collect_state_schema`);
`contract_hash`/`interface_hash` key on it, and a body-only edit must leave it EQUAL (→ no Reject). So CHIR-0 binds:
**every `state` declaration's StateEdge cell gets a stable id derived DETERMINISTICALLY from its source identity** (the
declaration's name + lexical scope path), NOT from textual position — so a body-only or unrelated edit re-lowers the same
state schema → `contract_hash` equal → the CEIR-10a body-only/Migrate case, and the text and graph projections of the same
program (D5) pin the SAME cell ids → the same CEIR. Non-state ops may keep the sequential assignment (they do not enter the
state-schema hash; their re-numbering is reload-invisible).

⛔ **Two mechanism constraints this ADR pins for CEIR-32e (so 32d/32e implement the right thing, not the convenient one):**
(a) `Context::set_stable_id` is **deserialization-only** (the STID binary loader's restore path, context.cpp:365) — it is
NOT the lowering-time pin API; 32e adds a lowering-time pin (a CHIR id-map consumed by / alongside `assign_stable_ids`),
it does not repurpose the loader call. (b) The id space is a flat `u64` with **no reserved range or tag bit** and
`assign_stable_ids` takes `next = max(existing ids, watermark) + 1` — so a CHIR-pinned id must respect the **monotone
watermark** invariant ([[feedback_monotone_id_needs_watermark_not_live_max_scan]]): a raw hash pinned as an id would jump
the watermark to the hash and make every sequentially-assigned temporary `hash+1, hash+2…` (the scar in a new costume).
32e's pin therefore allocates state-cell ids from a deterministic source-ordered counter that co-exists with the
sequential space (the exact scheme — a per-module CHIR id table, or a reserved low-range — is 32e's, constrained here to
be deterministic-from-source AND watermark-safe). This makes the semantic node, not the character offset, the unit of
identity — ADR-0114's point extended UP into CHIR.

### D4 — Ownership: the CEIR-0e §5 COMPOSITE, confirmed against the corpus (cite, don't argue)

Not a single mechanism — the composite the engine already is. Each leg cites the proof program that exercised it:

- **Value semantics by default** — every committed `.ceir` asset's Tensor values (audio buffers, ML tensors) are values.
- **Generational handles** for shared/long-lived objects — `resource.declare`/`AssetRef`, the scene/resource system.
- **Arenas / region lifetimes** for transient bulk — the frame-arena idiom (31b's `blur_a`/`blur_b` ping-pong transients).
- **Explicit state stores** for persistence — StateEdge cells: audio biquad/delay z-state (§142), TAA history (§20).
  This is the leg §143's `update state` + hot reload lands on (D3).
- **A LIGHT borrow, NOT a full checker** — surfaced onto the EXISTING `TypeKind::Qualified` + `OwnershipKind::BorrowedView`
  (type.hpp:64-79, "a borrow may not outlive its region" — the allocator-outlives-borrowers scar, IR edition). No stored
  borrows, no borrow-returning functions without a region parameter.

⛔ **The full Rust-style borrow-checker is REJECTED** (0e §5's reason, confirmed): it is hard to express visually and hard
for agents to satisfy (lifetime errors are the #1 Rust friction), which fights constraint (b) — CHIR must be visually AND
agent authorable. The composite is the model Cerid programmers, Cerid-trained agents, and the visual editor already
understand.

### D5 — Text and graph are two projections of ONE source model; the graph is a SCHEMA, not an editor

CHIR has one canonical structured source model (semantic nodes + stable ids [ADR-0114, BUILT] + source spans + layout
side-table keyed by stable id). **Text** and the **CR-D007 graph** are projections (§10, §166 — the noodle graph is UI,
not semantics). The graph-schema document (nodes + typed pins + edges; layout in the side-table) **lowers through the
SAME CHIR→CEIR path as text** — there is no second lowering and no visual-only runtime (that IS §180 #13). CEIR-32
**defines** this CR-D007 inspection/authoring schema; **CEIR-33/D7E renders it** (this unblocks the capability-registry
L6 blocker, gpu-platform-capabilities.toml:19). The text/graph PARITY gate (CEIR-32d) asserts both projections of §143
produce the byte-identical CEIR module (printer-canonical, the anti-drift mold).

### D6 — Module placement: `crd-chir`, host-only, one-way into `crd-ceir`

A NEW module `engine/chir` (`crd-chir`), a host-only compiler frontend. Dependencies, and ONLY these: `crd-ceir` +
`crd-core`/`crd-log`/`crd-memory`/`crd-containers`/`crd-units` (the ADR-0109 §4 foundational set). It **lowers one-way**
into CEIR (never sideways, never up — CEIR does not call CHIR). This fills the ADR-0109 gap (that ADR sited `crd-ceir`
+ the `crd-ceir-host`/`crd-ceir-gpu` bridges but left CHIR "design-only until CEIR-29/32", unsited). ⛔ **The CR-D007
graph schema (D5) lives in `crd-chir`** — it is the source model's serialization — so CEIR-33's editor depends on
`crd-chir` (editor → crd-chir → crd-ceir). Acyclic by construction. No std containers (the project rule).

## 4. What the corpus corrected in the 0e design

- **Ownership §5 composite → CONFIRMED, not adjusted.** Every leg has a live corpus consumer (D4). The corpus produced
  no dangling-lifetime pain a light borrow can't catch, so 0e's tiebreak (reject the full checker) stands.
- **0e §8 "final ownership model"** → D4 (composite onto the BUILT `Qualified`/`BorrowedView`).
- **0e Q5 (generics × CKIR variants, the VART landmine)** → confirmed a real landmine by CEIR-18p (VART kernel-variant
  selection): a generic `reduce<T>` instantiated at `f32`/`f64` needs the matching CKIR variant, so CHIR
  monomorphization must agree with the VART cache. **Generics are DEFERRED** (§7) precisely because this interaction is
  not v1-simple; it is named, not silently dropped.
- **`core.match` is STRUCTURAL** (docs: "PATTERN semantics are CHIR's") → CHIR owns pattern-match *semantics* and lowers
  to `core.match` structural arms. Pattern matching is DEFERRED past v1 (not a §143 verb), but its lowering target is
  reserved and confirmed.
- **Runtime dims are NOT generics** — `ceir.audio`'s `[dyn,2]` (CEIR-31z) shows a runtime frame dimension is a Dynamic
  tensor dim, not a type parameter; a corpus fact that keeps generics out of v1's critical path.
- **Compile-time specialization exists WITHOUT generics** — 31b's spec-consts specialize a program at cook time; a data
  point for the eventual const-generics decision (deferred).

## 5. Constraints inherited (locked, not re-opened)

From ADR-0108/0109 (0e §2), CHIR-0 keeps: no mandatory GC in hot paths; deterministic time + RNG (ADR-0063 replay);
capability-secured (ADR-0116 §57); C++ via versioned intrinsics/FFI (ADR-0110); unit-aware quantities (ADR-0078);
Cerid-owned (no third-party VM); shares the semantic-identity model with CEIR (ADR-0114); lowers one-way into CEIR.

## 6. Consequences

- CEIR-32b builds the source model + the CR-D007 graph schema (in `crd-chir`); 32c the text parser; 32d the CHIR→CEIR
  lowering + the text/graph parity gate; 32e hot reload + state migration (CEIR-10a). 32z closes.
- CEIR-33/D7E gains a defined schema to render (the L6 unblock).
- `crd-chir` is a new module edge; ADR-0109's dependency diagram gains `crd-chir → crd-ceir` (host-only, one-way).
- The reload-survival rule (D3) makes CHIR responsible for stable-id pinning; `assign_stable_ids` stays the fallback.

## 7. Deferral ledger (each with a trigger — no speculative build)

The 0e §98 / §8 surface minus what D1 binds. Each deferred to its first corpus consumer or the named future work:

- **generics + traits/interfaces** → carries the 0e Q5 VART landmine (a generic over a CKIR kernel must agree with the
  variant cache); trigger = a corpus program whose duplication a monomorphized generic would remove, decided against
  measured code size (0e Q2).
- **ADTs / tagged unions / pattern-match SEMANTICS** → `core.match` structural arms are reserved; trigger = a corpus
  program needing sum-type dispatch.
- **modules / packages / imports / namespaces** (erased-before-CEIR) → trigger = multi-file CHIR programs.
- **reflection + serialization-metadata** → trigger = a tooling/serialization consumer.
- **full async surface** (race/cancel/join/generators/structured-concurrency beyond `parallel_for`) → each its first consumer.
- **the error-model surface** (Result/Option ergonomics, `?`, panic-vs-typed-failure boundary) → v1 lowers `await`/errors
  minimally; the full surface → a corpus program with real error propagation.
- **extension methods** → a NAMED omission (0e §3 already marked it deliberate), not a silent drop.
- **the stdlib boundary** (language vs `ceir.*` dialect vs intrinsic) → the CHIR-1 stdlib slice.
- **the full Rust borrow-checker** → REJECTED outright (D4), not deferred.
- **the visual EDITOR** (rendering the D5 schema) → CEIR-33/D7E.

## 8. Ratification

ACCEPTED under the CEIR gold-standard autonomous cadence (the ADR-0114 mold); the user's commit is the ratification
gate (ADR-0108 §7). No consent gate, no AI co-author trailer. Supersedes the DEFERRAL clause of CEIR-0e §8 (the note's
binding decisions now live here); the 0e note carries a pointer to this ADR.
