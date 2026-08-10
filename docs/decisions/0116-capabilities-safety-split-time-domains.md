# ADR-0116 — Capability contracts + domain/safety split + typed time domains

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8f**. Builds the U-§57 capability model (finally OWNING the
§107 capability-contract field flagged ownerless since 7a), splits the safety axes out of `EvalDomain` (U-§23), and
establishes typed time domains (U-§17). Frameworks + vocabulary — a sandbox enforcing grants and an operand-type
checker enforcing time-domain mixing are named-forward.
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (rows "Capability contracts",
"Exec domain vs safety"); mission §57/§23/§17.
**Tags:** `[ceir]` `[capabilities]` `[safety]` `[time]` `[open-world]` `[foundation]`

---

## 1. Context

Three U-§ gaps: (a) programs have no way to say "I need `gpu.compute` / `file.write`" and hosts no way to grant it —
the TOML `[op.native] capabilities` field is PARSED by opgen (→ `.ops.json`) but wired to NOTHING (not on `OpInfo`,
not in the §107 interface hash; program_asset.hpp names it forward: *"capabilities have no owning row yet"*). (b)
`EvalDomain` (WHERE an op runs) is entangled with realtime-ness (`RegionExec` bundles domain+realtime); the orthogonal
SAFETY axes — may-allocate / may-block / may-IO / realtime-safe — have no representation. (c) time values (a frame dt,
a sim step, an audio sample count) are just numbers; mixing a wall-clock duration with a sim step is a silent bug.

## 2. Decision

### 2.1 Capabilities — an open-world named-permission model (owns the §107 field)

A `CapabilityId` is the FNV of a capability NAME (`scene.read`, `gpu.compute`, `file.write`, `external.process`, …) —
the `InterfaceId`/class-id model, **intern-only** (a capability is a named permission; no verify hook, no version, no
registry-with-metadata). `Context::intern_capability(name)→CapabilityId` + a reverse-lookup name table
(`capability_name`). An op-kind DECLARES its required set: `OpSpec.capabilities` (borrowed `StringView` names, the
opgen emits them from `[op.native] capabilities`) → `register_op` interns them into `OpInfo.required_capabilities`
(an arena-copied `CapabilityId` span, the effects precedent). `op_capabilities(kind)` reads them.

The PROGRAM's required set = **module-wide** (NOT "transitive" machinery — funcs ARE ops in the module body, so the
`StateW::go` pre-order walk already visits every private callee; no call-resolution, no cross-module calls exist), the
UNION of every op's required set, **sorted + deduped**. ⛔ **EMPTY≠UNKNOWN:** an UNREGISTERED op-kind contributes
`external.process` (the ExternalCall parallel — an unknown op must not read as requiring-nothing and smuggle past a
future sandbox). This set JOINS the §107 interface hash: a tagged `"caps:"` section, the **count pushed UNCONDITIONALLY
even when 0** (the 8c no-conditional-width lesson — cap-free modules recook too, named), then the sorted-unique u64 ids.
`program_capabilities(module)→sorted set` + `capabilities_satisfied(required, granted)` are the host-grant queries;
sandbox ENFORCEMENT (denying an ungranted program) is named-forward. ⛔ **Named-forward, explicitly:** a host can
compute the required set only from a LIVE Context today (`program_capabilities`); the hash folds it in but a hash is
opaque, so a host cannot read a COOKED program's required set before loading it. SURFACING the set as a cooked-`META`
field (so a sandbox pre-flights a grant off-disk) belongs to the sandbox/cook consumer slice — named forward so this
ADR's request/grant framing does not dangle.

**Recook:** `interface_hash` moves → **`kCeirCookSchema` 3→4**, a named recook (like 8c/8d). `stable_hash` and
`kBinaryVersion` are UNTOUCHED (capabilities are registration metadata, re-derived on load, exactly like effects — not
in the module blob). opgen emits `.capabilities` into the generated `OpSpec` (was `.ops.json`-only) → regen + drift
must be clean; `OpInfo`/`OpSpec` grow → the stale-.obj all-3-targets rebuild.

### 2.2 Domain/safety split — a total-switch effect projection, not new declared data

`EvalDomain` stays (WHERE). The safety axes are a **PROJECTION of the existing effect families** (extend-not-fork) —
NO new declared/hashed data. ⛔ Implemented as a per-family **TOTAL SWITCH** `effect_safety(EffectFamily)→SafetyBits`
(the `effect_access` shape, `-Werror=switch`-guarded — a mask/denylist predicate would be a THIRD family consumer
invisible to the compiler, the exact 8c hole the widen-enum memory exists for; a total switch forces every future
family to classify or fail to build). `op_safety(ctx, kind)` folds it over the op's declared effects (an UNREGISTERED
op is maximally unsafe — may-allocate+block+IO). Documented judgment arms: `Allocate`/`Deallocate`/`ResourceResidency`
→ may-allocate; `FileIO`/`NetworkIO`/`DeviceIO` → may-IO; `ExternalCall`/`Synchronization`/`TransactionBoundary`/
`AgentAction` → may-block (the 8c audio-RT rationale); `GPUCommand` → may-block (a submit can stall); `RandomRead`/
`TimeRead`/`Logging`/`Debug`/memory read-write → none. `realtime_safe := !(may_allocate || may_block || may_io)`.

⛔ **The two-oracles reconciliation (advisor).** `effect_legal_in_region` (the §32 HARD gate — what is *illegal* in an
audio-RT region) permits `Allocate` (8c deliberately), but `realtime_safe` excludes it — two "is this RT-safe" answers
that must not drift. Resolution: they answer DIFFERENT questions over the SAME effect vocabulary, in a **subset
relationship**: the hard gate forbids only the priority-INVERSION families (block + unbounded IO — a deadlock risk on
a RT thread); `realtime_safe` is the STRICTER advisory axis (additionally flags allocation, an RT-discouraged-but-not-
fatal cost). So `realtime_safe ⟹ legal_in_RT`, never the converse — documented + pinned by a test that shows `Allocate`
is legal-in-an-audio-RT-region yet NOT realtime_safe, side by side. (`effect_legal_in_region` keeps its curated
explicit list — e.g. bounded `DeviceIO` is RT-tolerable where unbounded `File`/`Network` are not — rather than a blind
`may_io` fold that would silently change DeviceIO's legality.)

### 2.3 Typed time domains — 8a type-classes under a `time` dialect (NO new vocabulary)

⛔ **There is no `TimeDomainId`** (the advisor's simplification): a time domain IS an 8a **type-class**. A
hand-registered `time` dialect (the `func` precedent) registers the six built-ins — `time.wall`, `time.sim`,
`time.frame`, `time.audio_sample`, `time.sequencer`, `time.logical` — as `TypeClassSpec`s whose verify hook requires
exactly one member of a NUMERIC/QUANTITY kind — `Int`/`Float`/`Quantity`, or a `TypeParam` so a generic time type
(`f<T>(x: time.wall<T>)`) verifies (the 8a substitute-through-Extern precedent); `time.wall<!string>` rejects. `time::time_type(ctx, class, underlying)` builds an
`Extern` type of that class over the underlying quantity. Because two different type-classes with identical params are
DIFFERENT `TypeId`s (the ADR-0111 landmine, PROVEN by 8a's own test), `time.wall<Time> != time.sim<Time>` — which IS
"mixing is a type error" the moment a checker looks. A parallel `TimeDomainId` vocabulary would FORK what `TypeClassId`
already does (the anti-pattern 8e's catalog table exists to prevent). A plugin domain (`game.turn`) works with ZERO
central edits (the open-world proof). U-§56 round-trip is inherited from 8a (pinned).

⛔ **Enforcement is named-forward, honestly:** NO operand-type-mix verifier exists today (semantic type-checking lands
at CEIR-3/4; the search found no `same-type-operands` check). So 8f establishes the DISTINCT TYPES (`wall != sim` as
`TypeId`s) — the foundation a future checker rejects mixing on — and does NOT claim to enforce it now. Zero
serialization motion for time domains (8a Extern types already round-trip; no format/hash change beyond §2.1).

## 3. The recook story

- **Capabilities (§2.1):** `interface_hash` moves → **`kCeirCookSchema` 3→4** (named). `stable_hash`, `kBinaryVersion`
  UNCHANGED (registration metadata). Cap-free modules recook too (count pushed unconditionally — honest, budgeted).
- **Safety (§2.2):** a pure projection/query — nothing serialized, no hash, NO recook.
- **Time (§2.3):** 8a Extern types round-trip on the existing machinery — no new format/hash motion.

## 4. Consequences

- The §107 capability contract is OWNED; a program's required-capability set is part of its swap-compatible identity;
  hosts can query `capabilities_satisfied`.
- `OpInfo`/`OpSpec` grew (a rebuild + a recook); the opgen emits capabilities into `OpSpec` (regen).
- Safety is a query over effects (no new state); the RT-safe advisory axis and the RT-legality hard gate are a
  documented subset, not two drifting oracles.
- Time domains are distinct 8a types (open-world); the mixing-is-an-error enforcement is named-forward to the checker.

## 5. Alternatives rejected

- **A capability registry with verify/version** — a capability is a name, not a class; intern-only (the `intern_interface`
  shape) is right.
- **"Transitive" capability collection via `collect_region_effective_mask`** — module-wide pre-order already covers
  private callees (funcs are ops in the body); no call-resolution machinery needed.
- **Safety as a declared `OpInfo` field / a mask predicate** — duplicates effects (fork) and a predicate dodges the
  `-Werror=switch` guard; a total-switch projection is the extend-not-fork, drift-proof choice.
- **A `TimeDomainId` vocabulary** — forks `TypeClassId`; a time domain IS an 8a type-class.
- **Claiming 8f enforces time-domain mixing** — no verifier exists; the honest deliverable is the type distinction +
  named-forward enforcement.

## 6. Test matrix

Capabilities (`test_capability.cpp`, `[capability]`): a declared required set round-trips (`op_capabilities`); the
module-wide program set is the sorted-unique union; an UNREGISTERED op contributes `external.process` (EMPTY≠UNKNOWN);
`capabilities_satisfied` (required ⊆ granted) true/false; a capability required by an op INSIDE a func body reaches the
program set (the module-wide-covers-func-bodies pin); two modules whose ops require DIFFERENT caps have DIFFERENT
interface hashes, a REORDER of the ops does NOT (sorted-unique — the 8d discriminator), and an UNREGISTERED op's
`external.process` moves the hash (EMPTY≠UNKNOWN); a cap-bearing module serialize→deserialize→serialize is BYTE-EXACT
with the SAME `stable_hash` (capabilities are registration metadata, never in the blob); `intern_capability` FNV ==
`capability_name` round-trip. Safety (`test_safety.cpp`, `[safety]`): `effect_safety`
classifies every family (the total switch); `op_safety` folds (a `MemoryWrite`+`Allocate` op is may-allocate; a
`FileIO` op is may-IO+not-realtime-safe; an UNREGISTERED op is maximally unsafe); the two-oracles subset — `Allocate`
is legal in an audio-RT region YET not realtime_safe, side by side. Time (`test_time_domain.cpp`, `[time]`):
`time.wall<Time> != time.sim<Time>` (distinct TypeIds); a plugin `game.turn` domain works (zero central edits); the
verify hook rejects a 0-member / 2-member time type; a `time.wall` type round-trips text+binary byte-exact (U-§56).
