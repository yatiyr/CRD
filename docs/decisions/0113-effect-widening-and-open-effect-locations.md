# ADR-0113 — Effect family widening (u32→u64) + open-world effect-LOCATION model

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8c**. Widens the CEIR-4a effect-family bitmask past its
32-bit ceiling to admit the U-§19 domain families (document/CAD/EDA/transaction/UI/agent) and replaces the
position-only effect-target with an open-world, dialect-extensible effect-LOCATION vocabulary (U-§20). Mirrors
ADR-0111/0112 (the open-world TYPE/ATTRIBUTE models) applied to effect locations.
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (rows "Effect families",
"Effect targeting"); mission §26; U-§19/U-§20.
**Tags:** `[ceir]` `[effects]` `[hazards]` `[open-world]` `[plugin]` `[foundation]`

---

## 1. Context

The CEIR-4a effect system (effect.hpp) carries, per op-KIND, a list of `EffectRecord`s — a `EffectFamily` (a `u8`
ordinal in a **closed** 27-entry `enum`, §26) optionally targeting an operand/result **POSITION** (`EffectTarget`
∈ {None, Operand, Result} + an index + a `range_mask`). The compiler unions families into a **`u32` bitmask** for
the CEIR-5c callee-derived-effects walk (`collect_effective_mask`) and derives ordering hazards (CEIR-4d) by mapping
family→`ResourceClass` (`hazard.hpp::effect_access`, a **total switch, no default**) and comparing target identity +
range overlap. Two ceilings block universality:

- **The family bitmask is 5 bits from overflow.** 27 families use bits 0..26 of a `u32`. U-§19's document (DCC/CAD/
  EDA/notebook), constraint (CAD/EDA), transaction, UI (reactive), and agent (agent-driven editing) families are
  legitimate §26 access-pattern axes with no home — and adding them crosses bit 31. The gap-matrix verdict
  (`…review.md` §B): *"`EffectFamily` 27 ordinals, u32 mask — 5 spare bits; U-§19's document/CAD/EDA/transaction/UI/
  agent families overflow → CEIR-8c (u64 + widening recook)."*
- **Effect targeting is position-only.** An op can say "I write operand 0" but not "I write **this ECS component**",
  "**this document object**", "**this state slot**", "**this file**" — the extensible resource identities every
  non-buffer domain needs. `EffectTarget` is a closed 3-value enum; a plugin domain (a CAD sketch entity, an EDA
  net) has no way to name its resource class.

**Effects are NOT serialized in the module blob.** `EffectRecord`s live on `OpInfo` (registration metadata) and are
re-derived from the registered dialect on load (`binary.cpp` encodes zero effect bytes — verified). The **only**
serialized surface effects touch is the §107 **interface hash**: `program_asset.cpp` projects each exported func's
transitive effective family mask (`push_u32(proj, mask)`). This single fact decides the whole recook story below.

## 2. Decision

### 2.1 Effect FAMILIES stay CLOSED; the bitmask widens u32→u64

Families remain a closed, append-at-end `enum` — **documented-why-closed** per U-§5, three load-bearing reasons:

1. **The mask is the O(1) union type** in the recursive `collect_region_effective_mask` walk (a callee's families
   fold into a caller's with one `|=`, dedup free, no allocation). An open-world family set cannot be a fixed-width
   bitset — it would force a heap set on the hottest interprocedural path.
2. **`effect_access()` is a total switch with NO default** — appending a family without classifying its
   read/write/`ResourceClass` is a `-Werror=switch` **compile error**. That append-at-end guard is the safety
   mechanism, and it exists *only* for a closed enum. Open-world families would silently fall through to a
   conservative default and lose the forced-classification discipline.
3. **Families are access-pattern AXES** (read/write/alloc/IO/determinism/sync), not domains. Domain identity is
   exactly what the open **location** (§2.2) carries. A universal system needs a small closed set of *how a thing is
   touched* and an open set of *what is touched* — never the reverse.

The bitmask type widens `u32`→`u64` end-to-end: `effect_family_bit` (→`u64`), `collect_effective_mask` /
`collect_region_effective_mask` / the `EffectsFn` hook signature / the §107 projection (`push_u32`→`push_u64`). The
**8 new families** (derived faithfully from the U-§19 domains — §26 stops enumerating at `Debug`, so these follow
the established `<Domain>Read`/`<Domain>Write` convention, e.g. `Scene`/`Ecs`/`Physics`/`Audio`):

| Family | access | ResourceClass | judgment (documented, §4d style) |
|---|---|---|---|
| `DocumentRead` | read | `Document` | DCC/CAD/EDA/notebook document-object graph |
| `DocumentWrite` | write | `Document` | |
| `ConstraintRead` | read | `Constraint` | CAD parametric constraints + EDA design rules |
| `ConstraintWrite` | write | `Constraint` | a solver mutates the constraint system |
| `TransactionBoundary` | read+write | `Universe` | a begin/commit/rollback is a full barrier — orders every effect across it, like `Synchronization` |
| `UIRead` | read | `Ui` | reactive-UI signal reads |
| `UIWrite` | write | `Ui` | UI event/state mutation |
| `AgentAction` | read+write | `Agent` | an agent edit observes-then-mutates; its *specific* resource rides the open LOCATION, not the family |

4 new `ResourceClass`es (`Document`, `Constraint`, `Ui`, `Agent`); `TransactionBoundary` reuses `Universe`.
`kLastEffectFamily` → `AgentAction` (ordinal 34 of 64 — documented headroom); the effect.hpp static-assert +
`ceir_opgen.py::EFFECT_FAMILIES` update in lockstep (drift/validator enforce identical order).

⛔ **Two family consumers, both classified deliberately.** `effect_access` (hazards, a total switch — `-Werror=switch`
forces its 8 new arms) is one; the SECOND is `effect_legal_in_region` (§32 audio-RT legality, semantics.hpp) — a
DENYLIST predicate, NOT a switch, so no compiler guard covers it. Each new family is decided by hand there:
`TransactionBoundary` (a commit/rollback can block or run unbounded — the `Synchronization` rationale) and
`AgentAction` (unbounded external-ish work — the `ExternalCall` rationale) JOIN the audio-RT forbidden set; the six
bounded domain read/writes (`Document`/`Constraint`/`UI`) stay legal, exactly as `SceneWrite`/`EcsWrite`/
`HostStateWrite` already are (over-forbidding a bounded read/write adds no safety and breaks legitimate real-time
patterns). ⛔ The general rule this bakes in: **a closed vocabulary can have consumers a total-switch guard cannot
see (predicate/mask/range logic) — widening the vocabulary means auditing EVERY consumer, not just the ones GCC
flags.**

**Serialization consequence — a NAMED, universal interface-hash recook (NOT a binary-format bump).** The §107
projection now pushes a `u64` mask (8 bytes) where it pushed a `u32` (4 bytes) — *every* exported func's interface
hash changes, **even effect-free ones** (the mask is pushed unconditionally, so 4 extra zero bytes shift the FNV).
This is honest and budgeted (unlike 8a/8b, which were no-bump): a *conditional-width* push to dodge the recook is
rejected — a value-dependent projection width is exactly the cleverness that bites at 8h. `kCeirCookSchema`
(program_cook.cpp) bumps **1→2** so a stale cooked program **rejects cleanly** at the schema check instead of
mismatching on the hash. `kBinaryVersion` does **NOT** bump — the module blob is byte-unchanged (effects aren't in
it); bumping it would wrongly reject valid blobs. A pre-8c module blob round-trips byte-exact (tested).

### 2.2 Effect LOCATIONS become open-world (dialect-extensible)

`EffectTarget` extends **in place** (append after `Result`, preserving ordinals 0..2 so every generated
`constexpr` `EffectRecord` array and hand registration compiles unchanged) with the U-§20 built-in fast path +
**one** `Extern` door; `using LocationKind = EffectTarget` names the widened concept:

```
None=0, Operand, Result,                                    // the pre-8c position kinds (unchanged ordinals)
BufferRange, ImageSubresource, TensorSlice, EcsComponent,   // CEIR-8c built-in location vocabulary (U-§20)
DocObject, StateSlot, File, Net,
Extern,                                                     // the ONE open-world door — location_class names the class
```

A `LocationClassId` (interned FNV "dialect.location", mirroring `TypeClassId`/`AttrClassId`) +
`Dialect::register_location_class(LocationClassSpec{verify, version, resource_class})` → a `Context` registry
(`m_location_classes`). `EffectRecord` gains **append-only** a `LocationClassId location_class` (a `u64` struct —
the POD stays trivially-copyable for the generated arrays + the `register_op` arena copy). The **junk-field guard**
(the 8a/8b canonicality discipline applied to `EffectRecord`): `location_class` is valid **iff** `target == Extern`,
zero otherwise — asserted at `register_op` (the factory leg of the verify triple).

**Hazard consumption (minimal + conservative).** `op_access_at` derives the conflict `ResourceClass`:
- `target == Extern` → the registered `LocationClassInfo::resource_class`, **or `ResourceClass::Universe` when the
  class is UNREGISTERED** — the EMPTY≠UNKNOWN leg (an unknown plugin location conflicts with **everything**, never
  inert). This is the one open-world leg that applies here and it is tested.
- else (None/Operand/Result + the built-in kinds) → the family's `effect_access` class, unchanged. The built-in
  location kinds are the **named vocabulary**; their per-instance identity resolution (an ECS component id, a
  document-object id) resolves conservatively to whole-class today and is **named-forward to CEIR-8d** (stable
  semantic identity — the `@taa.history`-style state-slot ids), where per-instance location identity rides operand
  SSA (today) or a serialized 8b attribute (there). No parallel identity machinery is built here (the
  third-dependency-graph anti-pattern the band warns against).

**Why the 8a/8b serialization legs are N/A-by-design (not deferred).** Effects are *registration metadata*,
re-derived from the registered dialect on every load — precisely so they can never go stale against registered
semantics. There is therefore **no per-instance location serialization surface**: no string-keyed encode, no
canonical text form, no decode version-range-check for locations. This is U-§5 discipline (a deliberately-scoped
open surface), documented here, with per-instance identity named-forward to 8d. The location-class registry still
carries `verify` + `version` (the factory/verify legs run at registration) and the conservative-unknown leg.

## 3. The verify triple, restated for effect locations

- **Factory (`register_op`):** asserts family ≤ `kLastEffectFamily` (new width), target ≤ `Extern`,
  `location_class` valid ⇔ `target==Extern`; runs the registered location class's `verify` hook on each Extern
  effect record; a `Pure` op still asserts zero effects (at the new width).
- **Decoder / parser:** N/A — effects are not serialized (see §2.2). The one adjacent decode invariant — a
  pre-8c module blob decodes byte-identically — is preserved (no format bump) and tested.
- **Unregistered:** an Extern location whose class is not registered is **preserved** at registration and treated as
  `ResourceClass::Universe` in the hazard analysis (maximally conservative), never dropped.

## 4. Consequences

- **A one-time, universal interface-hash recook** of every effectful program (KernelRef changes). Named, budgeted,
  clean-rejecting via `kCeirCookSchema` 1→2. No `kBinaryVersion` bump; module blobs are byte-stable.
- **`EffectRecord` grows one `u64`** (still a trivially-copyable POD; generated arrays + arena copy unchanged; the
  stale-.obj rebuild across all 3 targets is the only build-side cost).
- **A domain adds an effect location by hand-registering a class — ZERO central-enum edits** for the location
  (proven in tests with a `testfs.file_handle` class); the closed family set is a deliberate, documented exception.
- **The built-in location kinds resolve conservatively (whole-class) until 8d** — more hazards, never fewer (the
  safe direction), documented as 8d's refinement.

## 5. Alternatives rejected

- **Open-world effect FAMILIES.** Breaks the O(1) bitmask union AND removes the total-switch forced-classification
  guard (§2.1). Rejected — families are closed by design.
- **Per-instance serialized effect locations (in the module blob).** That is CEIR-8d's deliverable (stable ids for
  state slots / visual nodes); building it here duplicates identity machinery the band explicitly unifies later —
  the third-dependency-graph failure mode. Rejected; named-forward.
- **Conditional-width mask push** (u32 when the mask fits, u64 when it doesn't) to avoid recooking effect-free
  modules. A value-dependent projection encoding is a latent hazard for 8h's content-addressed incremental model.
  Rejected — the recook is honest and budgeted.
- **A `kBinaryVersion` bump.** The module blob is unaffected; a bump would wrongly reject valid pre-8c blobs.
  Rejected — the recook lives at the interface-hash + cook-schema layer, where it belongs.

## 6. Test matrix (`test_effect_open.cpp`, `[effect-open]`)

Family width: two funcs differing only in a **≥bit-32 family** have **different interface hashes** (`AgentAction`
bit 34 vs `MemoryReadWrite` bit 2 — a u32 mask would alias 1<<34 onto 1<<2 and collide them; the strongest
truncation catcher); the u64 mask carries a ≥bit-32 family through `effective_effects` unaliased; a **PRIVATE**
callee's bit-34 family lifts **transitively** into an exported caller's interface hash (the `call_effects_fn` u64
path — the private callee is not projected itself, so the difference can only arrive via the caller's transitive
mask); `Pure`⇒zero-effects asserts at the new width (the register_op live arm); the effect.hpp/opgen lockstep
static-assert holds (35 == 35). **Both family consumers:** `AgentAction`/`TransactionBoundary` in a device/audio-RT
region ⇒ a domain violation; `DocumentWrite` in the same region ⇒ legal (the deliberate `effect_legal_in_region`
classification). Locations: register a `testfs.file_handle` class (`resource_class` + verify) — an op declaring it
hazards by its declared class; an **UNREGISTERED** Extern location refuses to reorder against **everything**
(Universe); the verify hook rejects a registered-invalid record (the factory leg); two effects with **different**
registered location classes do **not** hazard. Format: `kBinaryVersion == 2` (unchanged) and a module blob
round-trips byte-exact (no format bump); the cook-schema 1→2 bump rejects stale v1 blobs cleanly via the existing
`read_cooked_header` SchemaMismatch path. The fuzz/ASan leg rides the existing effect + hazard suites (no new
serialized surface to corrupt).
