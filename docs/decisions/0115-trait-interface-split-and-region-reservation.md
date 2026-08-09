# ADR-0115 — Trait/interface split + region-kind reservation

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8e**. Fixes the closed CORE-TRAIT vocabulary in place
(documented-why-closed + a factory guard), promotes the CEIR-1d `OpInterface` registry to the TYPED open extension
surface for plugin BEHAVIOR, and sets the RegionKind reservation policy. Frameworks only — no consumers beyond the
reservation (the 6c/7a/8g no-consumer rule).
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (rows "Traits vs interfaces",
"Region kinds"); mission §7/§8; U-§8/U-§10.
**Tags:** `[ceir]` `[traits]` `[interfaces]` `[open-world]` `[region]` `[foundation]`

---

## 1. Context

Three vocabularies decide how the compiler dispatches without a central `switch(op.kind)`: **OpTrait** (a `u32` flags
set — 8 of 32 bits used: Terminator/Symbol/SymbolTable/Pure/IsolatedFromAbove/StateEdge/TokenProducer/TokenConsumer),
**OpInterface** (the CEIR-1d registry: `intern_interface(name)→InterfaceId`, `register_interface(kind,id,void* impl)`,
`get_interface(kind,id)→const void*` — an opaque function-table pointer the analysis casts back), and **RegionKind**
(a `u8` enum {Graph, SsaCfg}, serialized in the BODY). The split between them was never given a policy: (a) nothing
stops a dialect from minting a trait bit (dialect.hpp's own comment reads that way) or passing a stray bit that
silently collides with a future core trait — `register_op` bounds effects but NOT traits; (b) the interface registry
is untyped (every caller hand-interns the id and hand-casts the `void*`) and `intern_interface` is a per-call linear
string scan — not acceptable for THE extension surface; (c) domains needing Reactive/StateMachine/Timeline/
Constraint/Device regions have no reservation, risking a later redesign.

## 2. Decision

### 2.1 OpTrait stays CLOSED — documented-why + a factory GUARD

Traits are the fixed REASONING AXES the core's verifiers switch on (the 5b CFG verifier reads Terminator; the 5d
state verifier reads StateEdge; the 6a token verifier reads TokenProducer/Consumer) — a closed set, exactly like the
effect FAMILIES (ADR-0113 §2.1): a flags word needs a fixed bit assignment, and "which structural properties the core
reasons about" is core policy, not plugin surface. ⛔ **Dialects SET core-minted trait bits; they never MINT new
ones** (the dialect.hpp comment is struck-in-place to say so — plugin behavior goes in an INTERFACE, §2.2, never a
new trait bit). Enforcement parallels `kLastEffectFamily`: a `kKnownTraitsMask` (the OR of the 8 defined traits) +
a `register_op` factory assert `(spec.traits & ~kKnownTraitsMask) == 0` — a stray bit 20 now REJECTS at
registration instead of silently colliding with whoever the core later assigns bit 20. Lockstep: `OP_TRAITS`
(ceir_opgen.py) is count-pinned in the validator (the EFFECT_FAMILIES precedent), so a C++/TOML trait append can't
drift.

### 2.2 The OpInterface registry PROMOTED to the TYPED open extension surface

Plugin BEHAVIOR lives in interfaces — a typed function table an op-kind implements, registered per kind, dispatched
without the core switching on kind or adding a trait bit. Two upgrades over the raw CEIR-1d registry:

- **`InterfaceId` becomes a `u64` FNV** of the interface name (aligning with `TypeClassId`/`AttrClassId`/
  `LocationClassId`), so an id is a **compile-time constant** and a query mints it with zero registry work. A small
  `constexpr` string-FNV (byte-identical to `containers::fnv1a_64`) computes `T::kId` at compile time;
  `intern_interface` keeps the runtime path + a reverse-lookup NAME table for diagnostics (the `intern_type_class`
  shape). This retires the per-query O(#interfaces) string scan.
- **Type-safe accessors**: an interface is a struct `T` carrying `static constexpr kName` + `static constexpr
  InterfaceId kId`; `register_op_interface<T>(ctx, kind, const T* impl)` and `get_op_interface<T>(ctx, kind) →
  const T*` do the id + cast internally (a caller can no longer pair the wrong id with the wrong cast type). A
  dialect adds a capability by registering a `T` impl — **ZERO central-enum edits** (the id is interned from a name,
  open-world like OpId).

**Catalog — one live proof, the rest reserved by name mapped to their existing home** (⛔ a fresh vtable for a family
that already has a home would FORK existing machinery — the third-dependency-graph anti-pattern):

| Reserved interface | Canonical name | Where the behavior lives TODAY (a future slice EXTENDS this, never forks it) |
|---|---|---|
| **Cost** | `crd.iface.cost` | ⭐ **NO existing home → the LIVE proof interface, built here** (`u64 (*cost)(Context,Operation)`) |
| MemoryEffect | `crd.iface.memory_effect` | `EffectRecord` + `EffectsFn` (CEIR-4a/8c) |
| Shape | `crd.iface.shape` | the opgen `type_inference`/`shape_inference` op-schema fields (CEIR-3d) |
| Lowering | `crd.iface.lowering` | the `IExecutionProvider` seam (ADR-0109 §69) |
| Timeline | `crd.iface.timeline` | the `region_exec` packed attr (EvalDomain/RealtimeClass, CEIR-4c) + 8f time domains |
| Incremental | `crd.iface.incremental` | CEIR-8h (the incremental-evaluation unification) |
| Constraint | `crd.iface.constraint` | the 8c `Constraint` effect family + a future constraint dialect |
| Serialization | `crd.iface.serialization` | the 8a/8b/8c class verify/serialize + the 8d STID machinery |

Only **Cost** ships a live typed interface (proven end-to-end: a dialect registers a `CostInterface`, an analysis
that knows nothing about the op-kind dispatches through it). The other seven reserve their canonical names so a
future slice binds behavior to the SAME name against its existing home.

⛔ **"Op first" is a LOUD contract (advisor pre-close).** An interface impl lives ON the op-kind's `OpInfo`, so it
cannot be bound before the op registers. `register_interface` (the base, so the manual path shares the guard)
**asserts** the kind is registered rather than silently dropping — on THE extension surface a silent drop is a
plugin footgun (a mysterious `nullptr` from `get_op_interface` far away). Re-registering an interface for a kind
overwrites its impl in place (last binding wins). Late-bind (a pending side-table) would be a redesign — named
forward if a real need appears.

### 2.3 RegionKind reservation — interface/attr-gated policy, NO enum widening

`RegionKind` stays `{Graph, SsaCfg}` — the STRUCTURAL execution-order tag (data/effect graph vs explicit CFG),
which is all the core scheduler/verifier needs. The reserved region SEMANTICS (Reactive/StateMachine/Timeline/
Constraint/Device) are **not** new `RegionKind` enum values — they follow the CEIR-4c precedent, where region
semantics ALREADY ride an attr on the owner op (`region_exec` = packed EvalDomain/RealtimeClass). A domain adds a
region semantic by an interface (§2.2) + an owner-op attr, zero format change. **The policy names the one future
path that WOULD touch the enum:** if a kind proves a *structural* need the graph/CFG tag cannot express, it appends
to `RegionKind` per the 8a pattern (append-at-end, decoder bound-raise, out-of-range reject on old decoders, no
version bump). Until then: no enum value, no serialized-format change.

## 3. The recook story — ZERO motion (the band's first)

Nothing 8e touches is serialized: traits + interfaces are registration metadata (re-derived on load, like effects),
and `RegionKind` is unchanged. So **no `kBinaryVersion` bump, no `kCeirCookSchema` bump, no interface-hash change, no
content-hash change — no recook of any kind.** (The full 4-config gate still runs: `InterfaceId` u32→u64 grows the
`OpInterface`/`OpInfo` layout, so all 3 targets rebuild against the new headers — the stale-.obj scar.)

## 4. Consequences

- A stray/minted trait bit now REJECTS at `register_op` (was a silent future collision).
- Interface ids are compile-time constants + type-safe accessors; the per-query string scan is gone.
- Plugin behavior has ONE home (interfaces) with a documented catalog mapped to existing machinery — a future slice
  extends, never forks.
- `InterfaceId`/`OpInterface`/`OpInfo` layout grew (a rebuild, not a recook).
- Region semantics are reserved via the existing attr+interface mechanism; the enum stays minimal.

## 5. Alternatives rejected

- **Open-world traits** — breaks the flags-word bit assignment + the verifier-switch model (§2.1); behavior belongs
  in interfaces.
- **Keep `InterfaceId` a u32 + linear-scan intern** — a per-query string scan on the declared extension surface; the
  FNV model is O(1) + compile-time + consistent with the class ids.
- **Fresh vtables for MemoryEffect/Shape/Lowering/…** — forks the effect/shape-inference/provider machinery that
  already owns those behaviors; the catalog reserves NAMES and maps to homes instead.
- **New `RegionKind` enum values for Reactive/StateMachine/…** — a serialized-format change for semantics the
  `region_exec` attr + an interface already express; reserved-via-policy avoids the churn.
