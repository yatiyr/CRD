# CEIR Universality Review — foundation closure, domain-proof architecture, roadmap re-baseline

> **The standing quest document** (2026-08-09, user-directed). Section numbers from the quest prompt are cited as
> **U-§n** here and in the tracker. This is the U-§127 report: sections A/B/N are complete now; C–M accrue as the
> CEIR-8 (Foundation Closure) and CEIR-9 (Universality Validation) bands land; the final U-§126 answer is recorded
> at CEIR-9z. The live plan is `docs/detours/D-007-ceir-tracker.md` (re-baselined; mapping table there and in §N).
>
> **The standard (U-§0):** not "can CEIR run rendering and GPU compute" but *"can Cerid build an engine,
> MATLAB-class environment, DAW, Blender-class DCC, CAD/CAM, PCB/EDA, simulation platform, scientific environment,
> AI/ML system, media tool, game engine, automation platform, and agent-driven engineering environment on ONE
> coherent programmable substrate — and can unimagined future domains integrate through new dialects/types/ops/
> providers without redesigning CEIR core?"*

---

## A. Current architecture discovered (evidence, 2026-08-09, bands 1–7b shipped · 232 tests · 4 configs)

**Modules.** `crd-ceir` (host-only core; links core/log/memory/containers/units only — grep-gated I3/I5/I6) ·
`crd-ceir-host` (jobs execution-provider bridge) · `crd-ceir-cook` (asset cook/runtime bridge → crd-resources +
crd-render-asset-core). ADR-0109 dependency inversion holds: the core has never gained a backend edge.

**Genuinely open-world today (the substrate's proven strengths):**
- **Operations** — interned `OpId` (FNV of "dialect.op"), open registration (`Dialect::register_op` + OpSpec),
  NO central switch on op kind (I6 grep-gated), unknown-dialect ops preserved through text/binary round-trips
  (CEIR-1d tests). The 2a generator: an op = a TOML edit + regen, zero central edits (proven end-to-end at 2z).
- **Effects machinery** — kind-level `EffectRecord` declarations + instance-level `EffectsFn` hooks (5c
  callee-derived), EMPTY≠UNKNOWN discipline everywhere (unregistered = maximally effectful).
- **Verifier corpus** — structure (§115 layer), domain/realtime, token use-once, borrow escape, recursion policy —
  all pointing-diagnostic style, all first-offender-deterministic.
- **Serialization** — chunked FourCC binary v2, forward-skip unknown chunks, field-by-field LE (padding-scar-proof),
  hostile-input fuzz corpus (1h), content-pure (equal graphs serialize byte-equal regardless of Context history).
- **Identity/hashes** — `stable_hash` (content) + `interface_hash` (§107 canonical projection: sorted exported
  symbols, structural types, transitive effects, module-wide state schema) + `collect_dependencies` (§106) —
  cross-Context purity proven (7a/7b).
- **Reflection** — `OpSchema` static tables + committed `.ops.json` per dialect (agent/CLI discovery at op level).
- **Execution** — reference interpreter (open-world EvalFn install), `IExecutionProvider` seam, jobs-backed
  parallel_for/map_reduce with {1..16} bit-identity, cooperative cancellation, structured concurrency (band 6).
- **Types with real semantics** — units/dimensions (3e `Quantity`, mirrors crd-units), ownership/lifetime
  qualifiers (3f + borrow-escape check), shapes/tensors (3d), resources/views (3c), generics/traits (3b).
- **Determinism model** — §27 tiers on ops AND native bindings, compiler modes, §28 per-instance numerics attr.

**Closed-vocabulary or missing (verified in code — the gap matrix, §B).**

## B. Universality gap matrix

| Foundation concept | Current state (file:line evidence) | Open-world? | Gap → owning row |
|---|---|---|---|
| Operations | interned OpId, open registration, I6-gated | ✅ YES | — (the model to copy) |
| Types | `TypeKind` closed u8 enum (~30 kinds, type.hpp:22) | ❌ closed | **CEIR-8a** (dialect type-classes) |
| Attributes | `AttrKind` 6 kinds (attr.hpp:16); attr.hpp:6 PROMISED "CEIR-2/3 extend it" — never landed | ❌ closed + recorded IOU | **CEIR-8b** |
| Effect families | ✅ **CEIR-8c:** widened u32→u64, +8 U-§19 families (35 total, closed-documented-why); interface-hash recook (kCeirCookSchema 1→2) | ✅ closed | **CEIR-8c** ✅ (ADR-0113) |
| Effect locations | ✅ **CEIR-8c:** open `LocationClassId` registry + `EffectTarget::Extern` door; unregistered ⇒ Universe (EMPTY≠UNKNOWN) | ✅ open | **CEIR-8c** ✅ (ADR-0113) |
| Semantic identity | ✅ **CEIR-8d:** per-op `StableId` (one id space, monotone watermark); STID chunk (no format bump); content-hash id-independent; state schema re-keyed by id | ✅ closed | **CEIR-8d** ✅ (ADR-0114) |
| Traits vs interfaces | ✅ **CEIR-8e:** traits CLOSED + factory guard (kKnownTraitsMask); `OpInterface` promoted to the TYPED open surface (FNV InterfaceId, `register/get_op_interface<T>`); behavior lives in interfaces | ✅ closed | **CEIR-8e** ✅ (ADR-0115) |
| Region kinds | ✅ **CEIR-8e:** `RegionKind` stays structural {Graph,SsaCfg}; reserved semantics (Reactive/StateMachine/Timeline/Constraint/Device) interface/attr-gated via policy (no enum widening) | ✅ reserved | **CEIR-8e** ✅ (ADR-0115) |
| Capability contracts | ✅ **CEIR-8f:** open-world `CapabilityId` (FNV); op-kind required set on OpInfo; module-wide program set (unregistered⇒external.process) joins the interface hash (kCeirCookSchema 3→4); host-grant queries | ✅ owned | **CEIR-8f** ✅ (ADR-0116) |
| Exec domain vs safety | ✅ **CEIR-8f:** `EvalDomain` stays WHERE; safety axes (alloc/block/IO+realtime_safe) a total-switch projection of effects; the two RT oracles a documented subset | ✅ split | **CEIR-8f** ✅ (ADR-0116) |
| Time domains | ✅ **CEIR-8f:** typed time domains ARE 8a type-classes under the `time` dialect (wall/sim/frame/audio_sample/sequencer/logical; `time.wall<T> != time.sim<T>` as distinct TypeIds); a plugin `game.turn` domain with zero central edits; mixing-enforcement named-forward | ✅ owned | **CEIR-8f** ✅ (ADR-0116) |
| Analysis/Pass/Rewrite | ✅ **CEIR-8g:** AnalysisManager (cached + never-stale preserved-set invalidation) + PassManager (Fatal short-circuit) + RewritePattern/ConversionTarget skeleton (driver reserved for 26) | ✅ framework | **CEIR-8g** ✅ (ADR-0117) |
| Diagnostics | ✅ **CEIR-8g:** DiagnosticEngine — stable FNV codes + severity + SourceLoc + notes + fix-its, one surface, text COPIED | ✅ built | **CEIR-8g** ✅ (ADR-0117) |
| Incremental evaluation | ✅ **CEIR-8h:** ONE engine `crd::containers::IncrementalDag` (deterministic DAG + content/interface revisions + the §107 dirty rule); `DependencyGraph` is now a byte-stable WRAPPER over it (the absorb, real this slice) | ✅ unified | **CEIR-8h** ✅ (ADR-0118) |
| Transactions | ✅ **CEIR-8i:** atomic authored-mutation `Transaction` (commit/rollback) — eager mutation + an inverse-op journal (reverse-order replay) over 8d stable ids; touched/removed id-sets feed the 8h engine; graceful-reject+poison via 8g diagnostics; byte-identical rollback; zero format motion | ✅ owned | **CEIR-8i** ✅ (ADR-0119) |
| Provenance | `SourceLoc` on every op since 1c; ⚠ NO transform-preservation policy exists yet (verified: no ADR states one, incl. 0117) and few transforms exist to test it | ◧ adequate now | named-forward to CEIR-26 (proven when real transforms exist) — NOT a claimed 8g policy |
| Serialization/versioning | strong (v2, forward-skip, fuzzed); custom types/attrs extend it at 8a/8b | ✅ strong | extended at 8a/8b/8d (versioned recooks) |
| Unknown-plugin content | unknown-dialect ops + Extern types/attrs preserved opaquely (1d/8a/8b); the preserve-opaque policy IS documented (ADR-0111/0115, U-§56); sandbox EXECUTION-denial of unknown ops named-forward | ◧ policy set | 8a/8e ADRs (U-§56); enforcement → 8f-caps/9h |
| Providers | `IExecutionProvider` + advertises(); partitioning at CEIR-24/29 | ✅ seam correct | unchanged (U-§75 lands with partitioning) |
| Structured concurrency | band 6 complete (jobs-backed, bit-identical, cancellable) | ✅ | — |
| Units/ownership/shapes | 3e/3f/3d complete with verifiers | ✅ | — |
| Agent introspection | ✅**9g** the FULL agent loop (OpSchema discover-by-property + 8i authoring + verify at 2 granularities + 8g diagnostics by code); the richer semantic-QUERY surface (beyond arity selection) stays forward | ◧ loop proven | 8i,9g ✅ · rich-query → future |
| Reference execution | §118 interpreter, differential-ready | ✅ | grows at CEIR-11 |

**Multi-level architecture (U-§2):** intact and correct — CHIR reserved (ADR-0109; 0e design note), domain dialects
lower progressively, CKIR by identity, providers under the common model. The re-baseline changes sequencing, not
layering.

## C. Core changes made (accrues per CEIR-8 slice)

**CEIR-8a — open-world TYPE model (ADR-0111, 2026-08-09).** `TypeKind::Extern` (one appended enum value) + a
`TypeClassId` (interned FNV "dialect.class") + `Dialect::register_type_class(TypeClassSpec{verify, version})` →
a `Context` type-class registry (`m_type_classes`), the op model applied to types. An `Extern` `Type` carries
`type_class` + `type_class_version`; parameters ride the existing 8 slots (reusing 3a interning/equality/hash/
serialization). Files: `id.hpp` (+`TypeClassId`), `type.hpp` (+`Extern`, +2 fields, `operator==`/well-formed/
canonical arms + the junk-fields-Extern-only guard), `dialect.hpp/.cpp` (+`TypeClassSpec`/`TypeClassInfo`/
`register_type_class`), `context.hpp/.cpp` (+`intern_type_class`/`type_class_name`/`type_class_info`/`type_extern`/
`verify_extern`), `print.cpp`+`parse.cpp` (the generic canonical form — no pretty hooks, U-§56), `binary.cpp`
(conditional trailing class-string+version on Extern, no version bump; the kind bound raised to Extern),
`program_asset.cpp` (conditional class emission in the §107 interface-hash projection, zero churn). Extension
mechanisms delivered: a domain adds a custom type by hand-registering a class — ZERO central-enum edits (proven:
`testcad.toposhape`, `testeda.net`). Unknown-plugin types round-trip losslessly through an unregistered Context
(U-§56, both text and binary). Serialization/hash changes: none for existing content (Extern-conditional);
`Type` layout grew (a rebuild, not a recook — the binary format is unchanged for non-Extern).

**CEIR-8b — open-world ATTRIBUTE model (ADR-0112, 2026-08-09).** `AttrKind::{Array,Dict,TypedConst,Extern}` (four
appended enum values) + an `AttrClassId` (interned FNV "dialect.attr") + `Dialect::register_attr_class(AttrClassSpec
{verify, version})` → a `Context` attr-class registry (`m_attr_classes`), the 8a type-model surface applied to
attributes. TWO wrappers (`TypedConst` = a value OF a `wrapped_type`; `Extern` = a dialect attribute-class + version)
over ONE aggregate vocabulary (`Array` = an `elems` span; `Dict` = parallel SORTED `keys` + `elems`), each carrying a
single `payload`/span in reused `AttrValue` slots (reusing 1c interning/equality/hash/serialization). Files: `id.hpp`
(+`AttrClassId`), `attr.hpp` (+4 kinds, +6 fields, `operator==`/`attr_is_canonical` arms + the kind-scoped junk guard
+ the Dict sorted-unique invariant), `dialect.hpp/.cpp` (+`AttrClassSpec`/`AttrClassInfo`/`register_attr_class`),
`context.hpp/.cpp` (+`intern_attr_class`/`attr_class_name`/`attr_class_info`/`attr_array`/`attr_dict`/`attr_typed`/
`attr_extern`/`verify_attr_extern`; `intern_attr` now asserts canonicality + deep-copies aggregate spans; `attr_dict`
insertion-sorts keys), `print.cpp`+`parse.cpp` (the generic canonical forms `[..]`/`{"k":v}`/`#typed<>`/`#extern<>`
— no pretty hooks; the parser gained a depth guard + graceful reject of non-canonical dicts + the version range-check,
which was ALSO retrofitted into 8a's `parse_extern` for text/binary symmetry), `binary.cpp` (a child-first ATTR pool +
conditional new-kind encode/decode arms + the kind bound raised to Extern, no version bump). Extension mechanisms
delivered: a domain adds a custom attribute by hand-registering a class — ZERO central-enum edits (proven:
`testunit.dimension`); arbitrary nested aggregates round-trip via the child-first pool. The wrapper-composition rule
(no wrapper-on-wrapper) + the Dict canonical order close the two structural hazards. Unknown-plugin attributes
round-trip losslessly through an unregistered Context (U-§56, both forms). Serialization/hash changes: none for
existing content (new-kind-conditional; scalar encodings byte-identical; attrs feed `stable_hash` not `interface_hash`
— proven); `AttrValue` layout grew (a rebuild, not a recook).

**CEIR-8c — effect FAMILY widening (u32→u64) + open effect-LOCATION model (ADR-0113, 2026-08-09).** The effect
family bitmask widened `u32`→`u64` to admit 8 U-§19 domain families (`DocumentRead/Write`, `ConstraintRead/Write`,
`TransactionBoundary`, `UIRead/Write`, `AgentAction` — ordinals 27..34, crossing bit 31). Families stay a CLOSED
enum (documented-why-closed: the mask is the O(1) union type; the `effect_access` total-switch is the append guard;
families are access-pattern axes, domain identity lives in the location). Effect LOCATIONS became open-world: an
interned `LocationClassId` ("dialect.location") + `Dialect::register_location_class(LocationClassSpec{verify,
resource_class, version})` → a `Context` registry; `EffectTarget` extended in place with the U-§20 built-in kinds +
one `Extern` door; `EffectRecord` gained a `LocationClassId location_class` (POD-preserved, valid iff Extern — the
register_op junk-field guard). The hazard analysis reads an Extern location's declared `resource_class`; an
UNREGISTERED one is `ResourceClass::Universe` (EMPTY≠UNKNOWN, extended). Files: `id.hpp` (+`LocationClassId`),
`effect.hpp` (+8 families, u64 `effect_family_bit`, +`EffectTarget` kinds + `LocationKind` alias, +`location_class`),
`hazard.hpp` (+4 ResourceClasses + 8 `effect_access` arms), `semantics.hpp` (`effect_legal_in_region` — the SECOND
family consumer, classified by hand: Agent/Txn forbidden in audio-RT, domain read/writes legal), `dialect.hpp/.cpp`
(+`LocationClassSpec`/`Info`/`register_location_class`, u64 `EffectsFn`), `context.hpp/.cpp` (registry +
`effect_resource_class`/`effect_location_valid`, u64 masks), `func.cpp` (u64 `call_effects_fn`), `program_asset.cpp`
(+`push_u64`, the §107 mask projection widened), `program_cook.cpp` (`kCeirCookSchema` 1→2), `ceir_opgen.py`
(EFFECT_FAMILIES 27→35 lockstep). Extension mechanisms delivered: a domain adds an effect location by
hand-registering a class — ZERO central-enum edits (proven: `testfs.file_handle`). Serialization/hash changes: a
NAMED, universal interface-hash recook (the u64 mask projection; kCeirCookSchema 1→2 rejects stale blobs cleanly);
NO `kBinaryVersion` bump (effects aren't serialized in the module blob; `EffectRecord` layout grew — a rebuild).

**CEIR-8d — stable semantic identity (ADR-0114, 2026-08-09).** A `StableId` (u64, 0=invalid) rides `Operation` —
ONE id space (functions/state-slots/visual-nodes all key off the op id). `Context::assign_stable_ids` is
module-scoped, pre-order, one-time, idempotent (NOT a Context counter — that would break blob purity); a monotone
per-module WATERMARK (serialized) means an `erase()`d op's id is never reused (in-memory or across load). Serialized
as an additive, forward-skippable `STID` chunk (`count + watermark + per-op ids`) → NO `kBinaryVersion` bump; decode
is build-raw-graceful-reject (0/duplicate/count-mismatch/id>watermark reject; fuzz-swept). `stable_hash` builds the
blob WITHOUT STID → the content hash is id-independent → ZERO content-hash churn (cache hits survive). The TEXT form
stays the id-free content projection (documented-why; a fresh module's text round-trip reproduces ids). The §20
state schema in the interface hash is re-keyed by stable id (sorted, id value in the hash) → a StateEdge-cell reorder
is invariant (the 7a/8c false-incompatible fixed) while delete-id-1 + add-id-2 is correctly incompatible. Files:
`id.hpp` (+`StableId`), `ir.hpp` (+`Operation::m_stable_id` mutable, +`Module::m_stable_id_watermark`),
`context.hpp/.cpp` (+`assign_stable_ids`/`set_stable_id`/`set_stable_id_watermark`), `binary.cpp` (the STID chunk
encode/decode + `stable_hash` STID-skip), `program_asset.cpp` (state schema re-keyed), `program_cook.cpp`
(kCeirCookSchema 2→3). Serialization/hash changes: interface-hash recook (state schema); ZERO content-hash churn;
`Operation`/`Module` layout grew (a rebuild). Runtime `m_cells` stays `Operation*`-keyed (transient; migration → 10a).

**CEIR-8e — trait/interface split + region-kind reservation (ADR-0115, 2026-08-09).** OpTrait stays a CLOSED core
vocabulary (documented-why: the fixed reasoning axes verifiers switch on) + a factory guard — `register_op` rejects a
stray/out-of-vocabulary trait bit (`kKnownTraitsMask`, the kLastEffectFamily parallel; OP_TRAITS count-pinned in the
opgen validator). The `OpInterface` registry was promoted to the TYPED open-world surface (`interface.hpp`): plugin
BEHAVIOR lives in a typed function-table interface, dispatched with zero `switch(op.kind)` and zero new trait bits.
`InterfaceId` became a u64 FNV of the name (like the class ids), so `T::kId` is a compile-time constant (a constexpr
`interface_hash_ct` byte-identical to `fnv1a_64`) and the per-query string scan is gone; `register_op_interface<T>`/
`get_op_interface<T>` are id+cast-safe; `register_interface` asserts "op first" (loud, not a silent drop). Catalog:
ONE live proof interface (`crd.iface.cost`) + 7 reserved names mapped to their existing homes (MemoryEffect→effects,
Shape→shape_inference, Lowering→IExecutionProvider, …) so a future slice extends, never forks. RegionKind stays
`{Graph, SsaCfg}`; reserved region semantics (Reactive/StateMachine/Timeline/Constraint/Device) are interface/attr-
gated via a documented policy (the CEIR-4c region_exec precedent) — no enum widening. Files: `id.hpp` (InterfaceId
u32→u64), `dialect.hpp` (kKnownTraitsMask + the trait-closed comment), `interface.hpp` (NEW — the typed surface +
CostInterface + reserved catalog), `dialect.cpp` (the trait guard + FNV intern_interface + the op-first assert),
`context.hpp` (the name table + interface_name), `ceir_opgen.py`/`test_opgen.py` (the OP_TRAITS count-pin).
Serialization/hash changes: NONE — the band's first zero-motion slice (no kBinaryVersion/kCeirCookSchema bump, no
recook); `InterfaceId`/`OpInterface`/`OpInfo` layout grew (a rebuild).

**CEIR-8f — capabilities + domain/safety split + typed time domains (ADR-0116, 2026-08-09).** THREE gaps closed.
(1) The U-§57 capability model OWNS the §107 field ownerless since 7a: a `CapabilityId` (FNV of a name, intern-only);
an op-kind's required set on `OpInfo` (opgen-emitted from `[op.native] capabilities`, empty-name-rejected); the
program's module-wide sorted-unique required set (an UNREGISTERED op ⇒ `external.process`, EMPTY≠UNKNOWN) joins the
interface hash (`"caps:"`, count unconditional) → `kCeirCookSchema` 3→4; `program_capabilities`/
`capabilities_satisfied` queries; sandbox enforcement + a cooked-META surfacing named-forward. (2) The U-§23 safety
split: `EvalDomain` stays WHERE; the safety axes are a total-switch `effect_safety` PROJECTION of the effect families
(no new state) folded by `op_safety`; the two RT oracles (`effect_legal_in_region` vs `realtime_safe`) reconciled as
a documented subset (`realtime_safe ⟹ legal`). (3) U-§17 time domains ARE 8a type-classes under a hand-registered
`time` dialect (no `TimeDomainId`); `time.wall<T> != time.sim<T>` as distinct TypeIds (the type distinction a future
operand-type checker enforces — named-forward); a plugin `game.turn` works with zero central edits. Files: `id.hpp`
(+`CapabilityId`), `dialect.hpp` (+OpSpec/OpInfo capability fields), `context.hpp/.cpp` (+intern_capability/
capability_name/op_capabilities/program_capabilities/capabilities_satisfied/op_safety), `hazard.hpp` (+`SafetyBits`/
`effect_safety` total switch), `program_asset.cpp` (the caps interface-hash section), `program_cook.cpp`
(kCeirCookSchema 3→4), `time.hpp`/`time.cpp` (NEW — the time dialect), `ceir_opgen.py`/`test_opgen.py` (the
capabilities→OpSpec emission + empty-name arm). Serialization/hash: interface-hash recook (capabilities);
`stable_hash`+`kBinaryVersion` unchanged (registration metadata); `OpInfo`/`OpSpec` layout grew (a rebuild).

**CEIR-8g — compiler-infrastructure skeleton (ADR-0117, 2026-08-09).** Four thin FRAMEWORKS (no consumers — CEIR-26/27),
each a SEPARATE class over a Context/Module (zero new Context members, the U-§72 bound). ONE shared `fnv1a_ct` (id.hpp,
hoisted from 8e) behind `make_interface_id`/`make_analysis_id`/`make_diagnostic_code`. `AnalysisManager` (U-§73): cached
analyses + never-stale invalidation via preserved-sets (a `changed==false` pass preserves all; NO inter-analysis
dependency tracking — named-forward to 8h, not a new graph). `PassManager` (U-§65): sequences passes + drives
invalidation + stops on a Fatal (`has_fatal()`). `RewritePattern`/`ConversionTarget` (U-§67/§74): data + caller-driven
`try_apply` (the walking driver reserved for 26); unlisted kind ⇒ Illegal (EMPTY≠UNKNOWN). `DiagnosticEngine` (U-§102):
one structured surface (code/severity/SourceLoc/notes/fix-its), all text COPIED (alloc-outlives-borrowers, ASan-proven).
Files: `id.hpp` (fnv1a_ct + AnalysisId/DiagnosticCode), `interface.hpp` (make_interface_id → fnv1a_ct), `pass_manager.hpp`
+ `rewrite.hpp` + `diagnostic.hpp` (NEW), `ceir.hpp`. Serialization/hash: NONE — a zero-motion slice (no recook); every
band now has the analysis/pass/diagnostic frameworks to build on. (8h's incremental engine and the AnalysisManager
stay SEPARATE — a different invalidation paradigm; analyses become engine nodes only when analysis deps become explicit, CEIR-26+.)

**CEIR-8h — incremental-evaluation unification (ADR-0118, 2026-08-09).** ENGINE-FIRST: ONE dependency/dirty engine
(`crd::containers::IncrementalDag`) the tree's ≥2 models converge on. The LAYERING WALL forced the home: crd-ceir is
asset-free and render-asset-core does not link crd-ceir (independent siblings), so the engine lives in the LOW shared
module both link (crd-containers). A `u64`-keyed DAG with deterministic topo (deps-first, min-id tie-break) +
affected_by ABSORBED byte-identical from `DependencyGraph`, PLUS per-node content/interface revisions driving the §107
rule generalized (`recompute_after_change`: content-or-interface change recomputes the node; INTERFACE change also
recomputes dependents; content-only hot-swaps). ⭐ The absorb is REAL this slice: `render-asset-core::DependencyGraph`
is now a byte-stable thin WRAPPER over the engine (the RAF/scene-render/ceir-cook suites are the regression net).
Named-forward (NOT wired, with reasons): the 8g AnalysisManager (a different invalidation paradigm), the CookDb (a
semantic unification — it already produces the content/interface pair). Files: `containers/incremental_dag.hpp` (NEW),
`render-asset-core/dependency.hpp`+`.cpp` (the wrapper). Serialization/hash: NONE — zero-motion (the wrapper's §106
CDEP records are byte-stable). Every future notebook/DCC/build-pipeline incremental workflow + CEIR-10b's plan cache +
CEIR-9's proofs build on this ONE engine.

**CEIR-8i — the transaction model (ADR-0119, 2026-08-09).** The atomic AUTHORED-mutation surface, the last foundation
framework before the 8z gate. A `Transaction` records IR edits against a Module, applies them EAGERLY (so the body
sees its own edits), and either COMMITS atomically or ROLLS BACK to the byte-identical pre-transaction state. Model:
eager mutation + an inverse-op UNDO JOURNAL, reverse-order replay (rejected: a COW module snapshot fights the arena's
stable-pointer identity model; a deferred-apply journal can't let the body observe its own edits). ⛔ The transaction
RECORDS, Context APPLIES — six thin transaction-only Context primitives ride Context's existing friendships (ZERO
`ir.hpp` edit), so there is no second mutation implementation (the frame-graph-recording discipline). REUSES the band:
a pre-existing op is referenced by its 8d `StableId` (survives the edit); the committed touched/removed id-sets feed
the 8h `IncrementalDag` (the tx says WHAT changed; the consumer computes revisions — 8h's division of labour); a
rejected edit / failed commit reports through the 8g `DiagnosticEngine`. The id-under-rollback subtlety: `begin()`
settles pre-existing ids up front (= the canonical serialized form) + records the watermark; `rollback()` restores it
→ byte-identity is unconditional and the 8d delete/re-add discriminator holds across a committed tx. Graceful-reject +
poison (agent-facing) on every mutator; commit verifies (per-op + symbol re-sync) before the point of no return.
Advisor pre-close caught the region-bearing-erase subtree-boundary hole (nested IN/OUT SSA edges leaking across the
tombstone) — the erase mutator now walks the subtree and rejects both directions. Files: `ceir/transaction.hpp`+`.cpp`
(NEW), `ceir/context.hpp`+`.cpp` (the six primitives), `ceir.hpp` (umbrella). Serialization/hash: NONE — a transaction
is runtime edit-session state; a committed tx changes CONTENT (an ordinary content-hash cache miss), not the format
(no `kBinaryVersion`/`kCeirCookSchema` bump, no new fuzz corpus). Unblocks the CAD-parametric, agent-editing, and
reactive-UI domains (they are transaction-shaped) + CEIR-9's proofs + CEIR-10a reload transactions.

## The U-§121 foundation DoD — answered from evidence (CEIR-8z BAND-8 GATE, 2026-08-09)

> ⚠ **Framing (honest):** the verbatim twenty-item enumeration lives in the quest prompt; this table is its
> **operational form — the §B foundation matrix answered item-by-item**. §B carries **23 rows** (not exactly twenty);
> they are ALL answered below rather than consolidated to hit a number — the count is flagged here so the user can
> reconcile against their own list. Each row cites the SPECIFIC test (file + case) so a reader can run the evidence.

| # | Foundation item | DoD criterion (what "closed" means) | Slice | Evidence (test file :: case) | Verdict |
|---|---|---|---|---|---|
| 1 | Operations | open registration, no central op-kind switch, unknown ops preserved | 1d | `test_dialect.cpp` :: unknown-dialect round-trip; `crd-ceir-invariants` I6 | ✅ the model |
| 2 | Types | dialect type-classes behind one `Extern` door, zero central edits, U-§56 round-trip | 8a | `test_extern_type.cpp` :: U-§56 preserve/re-register/unify; verify triple | ✅ |
| 3 | Attributes | aggregate + wrapper + dialect attr-classes, zero central edits | 8b | `test_extern_attr.cpp` :: U-§56 both forms; wrapper-composition; sorted-dict | ✅ |
| 4 | Effect families | the vocabulary covers the U-§19 domains; closed-documented-why with an append guard | 8c | `test_effect_open.cpp` :: u64 mask, the ≥bit-32 interface-hash catcher | ✅ |
| 5 | Effect locations | open-world location classes, unregistered ⇒ Universe (EMPTY≠UNKNOWN) | 8c | `test_effect_open.cpp` :: Extern location → registered class / unregistered → Universe | ✅ |
| 6 | Semantic identity | content-independent stable ids, monotone erase watermark, id-independent content hash | 8d | `test_stable_id.cpp` :: watermark no-reuse; content-hash id-independence | ✅ |
| 7 | Traits vs interfaces | traits CLOSED + factory guard; behavior in a typed open interface surface | 8e | `test_interface_typed.cpp` :: kKnownTraitsMask; typed dispatch/unify | ✅ |
| 8 | Region kinds | structural `{Graph,SsaCfg}`; reserved semantics interface/attr-gated by policy | 8e | ADR-0115 §2 policy (no enum widening); `test_controlflow.cpp` region_exec precedent | ✅ reserved |
| 9 | Capability contracts | open-world caps, module-wide program set in the interface hash, host-grant check | 8f | `test_capability.cpp` :: required-set round-trip, external.process, satisfied | ✅ |
| 10 | Exec domain vs safety | `EvalDomain` (WHERE) split from a total-switch safety projection of effects | 8f | `test_safety.cpp` :: effect_safety fold; the two-RT-oracles subset | ✅ |
| 11 | Time domains | typed time domains as 8a type-classes (`wall != sim`); plugin domain, zero edits | 8f | `test_time_domain.cpp` :: wall≠sim distinct TypeIds; `game.turn` plugin | ✅ |
| 12 | Analysis / Pass / Rewrite | cached analyses + never-stale invalidation; a pass sequencer; a rewrite/conversion skeleton | 8g | `test_pass_manager.cpp`, `test_rewrite.cpp` :: fresh-after-invalidate; unlisted⇒Illegal | ✅ framework |
| 13 | Diagnostics | one structured surface (code/severity/loc/notes/fix-its), text copied | 8g | `test_diagnostic.cpp` :: emit/render/Fatal; the text-COPY ASan probe | ✅ |
| 14 | Incremental evaluation | ONE dependency/dirty engine with the §107 content/interface rule; ≥2 models absorbed | 8h | `test_incremental_dag.cpp` :: the §107 rule; render-asset-core suite (the absorb) | ✅ unified |
| 15 | Transactions | atomic authored-mutation, commit/rollback, byte-identical rollback, the 8h seam | 8i | `test_transaction.cpp` :: A/B rollback; watermark restore; the dirty seam | ✅ |
| 16 | Serialization / versioning | chunked forward-skip binary, versioned recooks, fuzz-hardened, extended over the open-world records | 1f/8a/8b/8d | `test_binary.cpp`; the extern/STID round-trips; the per-slice corruption sweeps | ✅ |
| 17 | Structured concurrency | jobs-backed, {1..16} bit-identical, cancellable | band 6 | band-6 suite (async/token verifier) | ✅ |
| 18 | Units / ownership / shapes | dimensioned quantities, ownership qualifiers + borrow-escape, shapes/tensors | 3d/3e/3f | `test_band3_gate.cpp` :: the four pointing diagnostics | ✅ |
| 19 | Reference execution | an open-world reference interpreter, differential-ready | band 5 | `test_exec.cpp`; `test_band5_gate.cpp` :: the pinned program executes byte-identically | ✅ |
| 20 | Providers | `IExecutionProvider` seam + advertises(); partitioning is a consumer band | seam | ADR-0109 dependency inversion; the seam is correct (partitioning → 24/29) | ✅ seam |
| 21 | **Provenance** | `SourceLoc` on every op; a transform-PRESERVATION policy | 1c / →26 | `test_attr.cpp` :: SourceLoc provenance. ⚠ No preservation policy exists yet | ◧ named-forward CEIR-26 |
| 22 | **Unknown-plugin content** | preserve-opaque policy + a FULL external plugin registers via public doors, zero core edits | 8a/8e / **9h** | `test_plugin.cpp` :: 9 surfaces via public APIs, U-§116 enum-pin, foreign preserve+fuzz. Sandbox EXECUTION-denial → 24/29 | ✅ registration proven (9h); exec-denial → 24/29 |
| 23 | **Agent introspection** | discovery + authoring + verify/diagnostics + a richer semantic-QUERY surface | 2/8i / **9g** | `test_agent.cpp` :: the FULL agent loop (discover-by-property + author + module-wide sweep + diagnostics by code) proven at 9g. The richer semantic-query surface (beyond arity selection) stays forward | ✅ agent loop proven (9g); rich-query → future |

**The honest verdict (U-§126, foundation scope).** **20 of 23 items are CLOSED** by a foundation slice or were already
open; the **3 remaining (21 Provenance, 22 unknown-plugin ENFORCEMENT, 23 agent QUERY introspection) are ◧
consumer-band-deferred, NOT foundation gaps** — each needs a real consumer to test against (transforms at CEIR-26, a
sandbox host at 9h, an agent query workflow at 9g), and each has its policy/seam set. The foundation is **universal by
open-world semantics, not a second architecture**: a new domain adds dialects / types / attributes / effect-locations /
interfaces / capabilities / providers with ZERO core edits (the 8a–8f "zero central-enum edits" proofs), reuses the
identity / serialization / incremental / transaction models (8d/1f/8h/8i), and reports through one diagnostic surface
(8g); a CORE change is warranted only for a genuinely new universal PRIMITIVE. The composed gate proves it in one
program (`test_band8_gate.cpp`): an Extern type + Extern/aggregate attr + capability-bearing op + settled stable ids
round-trip, preserve through an unregistered host, unify on re-register with a cross-Context-pure interface hash, and
take an atomic transaction (commit + byte-identical rollback) — the U-§52 agent-edits-plugin-content story end-to-end.

**Gate discipline evidence (CEIR-8z).**
- **Recook migrations executed + clean:** the versioned-format recooks (`kCeirCookSchema` 1→2 [8c] → 3 [8d] → 4 [8f];
  `kBinaryVersion` held at 2 throughout) are proven by `test_program_cook.cpp`'s SchemaMismatch rejection cases — a
  stale-schema cooked blob rejects cleanly; all 320 tests (incl. the cook suite at schema 4) green across 4 configs.
- **Fuzz corpus over the open-world SERIALIZED records:** extern types (`test_extern_type.cpp` single-byte sweep),
  extern/aggregate attrs (`test_extern_attr.cpp` sweep), the STID chunk (`test_stable_id.cpp` sweep), the general
  malformed corpus (`test_malformed.cpp`), and now the **COMPOSED blob** (`test_band8_gate.cpp` — cross-chunk: ATTR
  pool × STID × extern class strings, which no single-feature sweep reached). ⛔ Effect families/locations, safety,
  time domains, and capabilities are REGISTRATION metadata, NOT serialized in the module blob → **N/A-by-design** (no
  serialized record to corrupt).
- **No new invariant earns its keep (documented-why I3/I5/I6 suffice):** I6 already gates the open-world dispatch
  discipline (the core never `switch`es on op.kind — the 8i transaction's `switch(MutKind)` and the closed-value-enum
  switches are legitimately outside it); the Extern-arm COMPLETENESS of the `TypeKind`/`AttrKind`/`EffectFamily`
  switches is enforced by GCC `-Werror=switch` (a compiler-level gate STRONGER than grep); the single-mutation-path is
  an ADR-0119 discipline that construction paths (parser/builder/deserialize) legitimately use the raw primitives for,
  so a grep would false-positive. A new grep-invariant would be redundant or noisy — not added.
- **No ADR for 8z:** a gate COMPOSES existing decisions (ADR-0111…0119); it introduces no new architecture, so it
  ships no ADR (the 3z/4z/5z precedent).

## D–H. Architecture sections (accrued across CEIR-8)

- **D — Extension model completion.** Open-world TYPES (8a), ATTRIBUTES (8b), and effect LOCATIONS (8c) join the
  already-open OPERATIONS (1d) and the typed op-INTERFACE surface (8e) — every extension is a `dialect.register_*` call
  with a verify hook + version, zero central-enum edits, U-§56 preserve-opaque for the unregistered host. Effect
  FAMILIES stay closed-documented-why (the O(1) mask; the total-switch append guard). §C 8a/8b/8c/8e carry the detail.
- **E — State + transaction model.** 8d content-independent stable identity (the monotone erase watermark; the state
  schema re-keyed by id) + 8i atomic transactions (commit/rollback over those ids, the touched/removed seam into 8h) —
  the authored-mutation + undo/redo + agent-edit architectural home, zero format motion. §C 8d/8i.
- **F — Incremental model.** 8h `crd::containers::IncrementalDag` — ONE dependency/dirty engine (deterministic topo +
  content/interface revisions + the §107 rule); `render-asset-core::DependencyGraph` is now a byte-stable wrapper over
  it (the absorb, real). The cook + AnalysisManager converge by named-forward. §C 8h.
- **G — Compiler architecture.** 8g four thin frameworks (AnalysisManager / PassManager / RewritePattern+ConversionTarget
  / DiagnosticEngine) — the optimization/lowering CONSUMERS are CEIR-26/27 (the no-consumer rule). §C 8g.
- **H — Provider architecture.** `IExecutionProvider` + `advertises()` (ADR-0109 dependency inversion); provider
  PARTITIONING (multi-provider lowering) lands with its consumer bands (CEIR-24/29). Unchanged this band — the seam is
  correct, proven non-blocking by the 8-band designs.

## CEIR-9 universality proofs (accrue per 9x slice; U-§97 verdict each)

**CEIR-9a — U1 Notebook/incremental (2026-08-09, no ADR — a proof composes; no new serialized record → no fuzz).** A
notebook/reactive-recompute workload runs on the foundation with ZERO new machinery. The cells are REAL CEIR arith ops
(const/addi/muli); deps are SSA operands; 8d stable ids are cell identity; each cell's 8h **content** revision = a hash
of its FORMULA (op kind + operand DEP stable-ids + attrs — reorder/id-INDEPENDENT), its **interface** revision = a hash
of its computed VALUE. ⭐ That mapping makes the §107 rule the notebook **early-cutoff**: a formula edit that preserves
the value hot-swaps its dependents. Every edit rides an 8i **transaction** (the touched-set seeds the eval; a no-op
edit's unchanged content hash yields zero recomputes — the content-addressed memo, and the first real consumer of the
8i→8h seam). Proof: `test_notebook.cpp` (7 `[ceir][notebook]`) — initial eval once-each; edit an input → exactly its
value-dependents (a value-stable `A*0` node CUTS its branch — chained cutoff); edit a different input → a precise
partial set (a non-dependent mid-node cache-hits); a value-preserving operand swap → the single-level §107 cutoff; a
no-op tx edit → zero recomputes; identical formulas hash equal regardless of cell id/position. Exact counts, not "≥".

⛔ **The U-§97 verdict — NO second engine, as a DEMONSTRATED boundary (three parts):** (1) single-level cutoff IS the
engine — `recompute_after_change` with an unchanged interface returns no dependents; (2) the CHAINED driver is
necessarily consumer-side because chaining requires re-evaluation and the engine holds NO results **by the 8h division
of labour** — it structurally cannot chain; (3) the consumer's direct-dependents map is an inverted index that
**mirrors the engine's own dep edges** — no dependency information exists outside the engine. The generic machinery
(topo order, dep structure, revision storage, the §107 rule) is ALL `IncrementalDag`; the consumer is ~100 lines of
evaluate/compare/propagate glue.

⭐ **The content/interface generalization (honest, not a mismatch):** in the COOK reading interface is a *projection of*
content (8h's comment). In the NOTEBOOK reading a cell's interface = its computed value, which legitimately changes
while its content (formula) does not — during propagation the consumer calls `set_revision(C, sameContent,
newInterface)` routinely. The engine compares `u64`s and is AGNOSTIC to which reading a caller intends; **that
generality is itself U-§97 evidence** (one engine serves both) — stated here so it does not later read as a semantic
bug someone "fixes."

**Two near-misses (observations, NOT gaps):** (a) no per-op content-hash PRIMITIVE exists — the consumer computes the
formula hash from the public op surface (kind/operands/attrs), correct at cell granularity; (b) no direct-dependents
accessor on the dag — the consumer derives it from the same edges (`deps_at` / the operand walk). Neither is a
foundation gap; note them only if a later proof needs either hot. **Named-forward:** multi-cell edits in a SINGLE
transaction (touched-set > 1 seeding the eval) are untested here — that is 9d/9g territory where transactions are the
headline.

**CEIR-9b — U2 DAW/timeline (2026-08-09, no ADR — a proof composes; no new serialized record → no fuzz).** A DAW/audio
graph runs on the foundation with ZERO new machinery. The mock `audio` dialect is INLINE-registered (zero central
edits — the open-world proof): `source`/`gain`/`mix` (RT-safe), `delay` (carrying `OpTrait::StateEdge`), `load_sample`
(a FileIO effect), `scratch_alloc` (an Allocate effect), `graph` (the region-owner), each with EXPLICIT determinism +
domain axes. Four foundation surfaces compose: **8f time** (the sample clock is `time.audio_sample<T>`, a distinct
TypeId from wall + a plugin `tempo.beat` clock, zero central edits), **5d state** (the feedback edge is the plugin
`delay`'s last operand — the §20 `find_structure_error` cycles-only-through-state rule accepts it and rejects a
combinational `gain` loop as `FeedbackWithoutState`, offender pointed), **4c legality** (`find_domain_violation` flags
the disk load in an audio-RT region), **8e interfaces** (latency is a typed `LatencyInterface` the compiler queries).
Proof: `test_daw.cpp` (5 `[ceir][daw]`).

⛔ **The U-§97 verdict — NO second engine (three parts):** (1) the feedback discipline keys on the **trait, never a
kind name** — a plugin's `delay` is exactly as legal as `core.delay` (proven via the plugin-ONLY feedback path); (2)
offline-vs-realtime is a region-TAG property of ONE module object — flag → retag offline → clean → retag realtime →
flag, on the SAME pointer, is the no-second-graph proof (U-§38); (3) latency is a typed interface with EMPTY≠UNKNOWN
semantics (a missing interface ⇒ the chain latency is UNKNOWN, never silently 0). No new time model, no new state
model, no new scheduler, no new legality oracle. The two-RT-oracles subset is pinned by THREE data points
(`realtime_safe ⟹ legal`, the allocating op the witness that the converse fails — legal-but-unsafe).

⭐ **Multi-rate, honestly (the candidate gap, assessed not waved):** distinct sample rates are expressible NOW as
distinct time type-classes (or a rate param) — mixing rates is a type error the moment the operand-type checker looks
(the `wall != sim` property applied to `44.1k != 48k`); a **resample** op is an ORDINARY op whose operand and result
live in DIFFERENT time domains (no new construct); rate-match ENFORCEMENT rides the SAME operand-type checker already
named-forward from 8f. Sidechain routing is an ordinary extra operand; sub-sample timing is a fractional underlying.
**No new time model is needed** — the 8f typed-domain model covers multi-rate by construction.

**Honest boundaries (NOT gaps):** latency was proven at **op-KIND level** (querying each kind's interface with
unknown-propagation — the U-§39 substance); a graph-WALKING accumulator (`first_op`/`next_in_block`, summing each op's
kind) is trivial consumer glue, not exercised here. The delay fixture's `init` operand is a source result, not a
time-typed constant (structurally fine — the fixture is a legality/feedback probe, not a numeric evaluator).

**CEIR-9c — U3 DCC modifier graph (2026-08-09, no ADR — a proof composes; no new serialized record → no fuzz).** A
Blender-class modifier stack runs on the foundation with ZERO new machinery — the 8h `IncrementalDag::affected_by` IS
the depsgraph. The mock `dcc` dialect is INLINE-registered (base_mesh/subdivide/transform/boolean/deform/output, each
with explicit determinism+domain axes); a "mesh" is a mock poly count. The topology is a DAG with a JOIN over TWO
SEPARATE-source branches (base→subdivide and cutter→transform meet at boolean→deform→output). Proof: `test_dcc.cpp`
(5 `[ceir][dcc]`), exact recompute counts.

⭐ **The differentiator from 9a — and the U-§97 sentence this pair uniquely earns: TWO DRIVERS, ONE ENGINE, NO CODE
MOTION.** 9a PUSHED forward from a seed (Salsa-style: recompute, compare, push direct dependents); 9c PULLS —
`affected_by(M)` computes the full invalidation set UPFRONT (Blender's TAG pass), then ONE topo walk evaluates only
tagged nodes, early-outing any whose inputs all turned out unchanged (Blender's "no update needed"). The engine changed
by ZERO lines between the two proofs. That is stronger U-§97 evidence than either alone — a **9z synthesis input**.

⛔ **The U-§97 verdict — NO second depsgraph (three parts):** (1) the TAG pass IS the engine — `affected_by(edited)` is
the invalidation set, asserted EXACT both directions (on the subdivide and deform edits) and excluding the sibling
branch (on the cutter edit — precise upstream isolation); (2) the eval walk is necessarily consumer POLICY because
early-out requires re-evaluation and the engine holds NO results by the 8h division (per the 9a verdict); (3) the
consumer's `deps[]` mirrors the engine's own edges (the operand walk records the same edges `add_edge` did). The §107
early-out is proven by a topology-preserving deform edit: its formula changes, its output mesh does not, so its tagged
dependent (`output`) is the NAMED cut.

⭐ **Topology-vs-geometry facets (assessed, not demonstrated — the candidate gap):** the mock's "topology" is the whole
mesh value (one scalar poly count), so the early-out proves the §107 MECHANICS but not genuine facet separation. A
facet (a downstream that cares only about topology, not geometry) is an ORDINARY additional output node; dependency
granularity = which node you edge to (the same shape as 9b's resample-op and multi-rate answer); enforcement rides the
same named-forward operand checker. **No new engine is needed.** Modifier REORDER (a structural stack edit, not a
parameter edit) is named-forward — it needs the 8i structural-edit tx path 8i itself named-forward.

**CEIR-9d — U4 CAD parametric + transaction (2026-08-09, no ADR — a proof composes; no new serialized record → no
fuzz).** A parametric-CAD feature graph runs on the foundation with ZERO new machinery, and the **8i TRANSACTION is the
headline**. The mock `cad` dialect is INLINE-registered (sketch/extrude/assembly/fillet/body); a two-feature assembly
(sketch1→extrude1, sketch2→extrude2 join at assembly→fillet→body); a "geometry" is a mock scalar. The shared incremental
helpers were HOISTED into `tests/ceir/incremental_helpers.hpp` at this third consumer (9a/9c refactored onto it, proven
neutral at 337/337 before a line of CAD). Proof: `test_cad.cpp` (5 `[ceir][cad]`).

⛔ **The U-§97 verdict — NO second transaction-manager / depsgraph / constraint-solver (three parts):** (1) **multi-op
atomicity IS 8i, unmodified** — one transaction editing several dimensions returns a `touched()` set with multiple
stable ids; the eval seeds from the UNION of the affected subtrees (consumer glue over `affected_by`), so BOTH feature
branches re-evaluate where a single-dimension edit leaves the sibling branch cached. **The 9a multi-cell-per-transaction
named-forward is now CLOSED** (and a no-op edit inside a multi-op transaction recomputes only the real edit's subtree —
multi-op UNION × content-addressed memo, a composition no prior slice had). (2) **A CAD constraint is TWO existing
surfaces doing their own jobs, zero new mechanism** — the 8c `ConstraintRead` effect is *classification*, the sketch's
`VerifyFn` (run by the transaction's existing commit-verify) is *enforcement*; a constraint-violating commit is REJECTED
(diagnostic `ceir.transaction.verify_failed` → rollback → byte-identical, dimensions restored) while a satisfying one
succeeds. (3) A bidirectional constraint SOLVER is an ordinary fixpoint *node* (a solve is a computation, not a new
dependency-edge model); the feature graph stays one-way; enforcement granularity rides the same named-forward operand
checker. No new solver, no new time/state/transaction model.

⭐ **The CAD argument for transactions as the mutation surface (the insight this slice uniquely earns):** the satisfying
edit (width=height=4 → 6,6) necessarily transits a VIOLATING intermediate state (width=6, height=4 after the first
`set_attr`) and commits ANYWAY, because verification is ATOMIC at commit. A constrained model CANNOT be edited
dimension-by-dimension without passing through inconsistent states — so enforcement MUST be transactional, and 8i gives
exactly that for free (eager mutation, verify-at-commit, rollback-on-reject). That is why the transaction is the right
authored-mutation surface, demonstrated not asserted.

**Doc precision:** the single-dimension-edit case (test 489) pins behavior by recompute COUNTS; tag-set exactness is the
9c contract, cited not re-proven. The byte-identical rollback reuses the 8i money-test shape (serialize A/B) applied to
a feature graph.

**CEIR-9e — U5 PCB/EDA + external provider (2026-08-09, no ADR — a proof composes; no new serialized record → no
fuzz).** A PCB/EDA board pipeline (board_document → drc → route → gerber_export) runs on the foundation with ZERO new
machinery; its EXTERNAL TOOLS are ordinary capability-bearing ops. The mock `eda` dialect is INLINE-registered (zero
central edits). Proof: `test_eda.cpp` (4 `[ceir][eda]`).

⛔ **The U-§97 verdict — NO second provider / capability system (three parts):** (1) **an external tool is an ordinary
capability-bearing op** — 8f capabilities + 4a effects + 4b determinism + a content-addressed `tool` attr form the
complete TYPED CONTRACT, queried generically (`op_capabilities`/`op_effects`/`op_determinism`), with zero hard-coded op
knowledge; the host-grant check (`capabilities_satisfied`) is the sandbox boundary — a GPU-only host PROVABLY cannot run
the pipeline (it grants `gpu.compute`, the pipeline needs `external.process`+`file.write`). The runtime DISPATCH to the
external tool is the `IExecutionProvider` seam, named-forward to CEIR-24/29 — the contract is proven, the execution is
not claimed. The `tool` identity is content-addressed (two boards differing only in the router `tool` attr hash
differently — a re-route is a different program). (2) **⛔ The op-LOCAL commit-verify boundary (the finding this slice
earns, stated not softened):** 8i's commit-verify runs on TOUCHED ops only, so a design rule fires iff its carrier op is
edited — a rule ON the board catches board edits (the DRC here), but a whole-board DRC SWEEP (the real-EDA shape) needs
a `find_*_violation`-style walk or a pass over the committed module. That is CONSUMER-band machinery, named-forward to
9g (the agent's post-commit verifier sweep) and CEIR-26 (passes). Op-local is the CORRECT commit-time granularity;
module-wide is a different verification pass that COMPOSES on top — a boundary of what was proven, not a gap in 8i.
(3) **U-§34, DEMONSTRATED:** a complete EDA pipeline expressed itself in ZERO render-flavored vocabulary — a program-level
walk over every op unions Document/Constraint/ExternalCall/FileIO families and finds NO `GPUCommand` effect, NO
`DeviceTime` domain, NO `gpu.compute` capability. **The vocabulary was domain-neutral before this domain arrived.** (A
9z synthesis input, paired with 9c's "two drivers, one engine.") Gaps assessed (all expressible, none real): a typed
ERROR contract is an 8g Diagnostic emitted by the runtime dispatch (named-forward WITH the dispatch); streaming export
is an op/token; a bidirectional tool is Document read+write effects.

**CEIR-9f — U6 Game/ECS effects (2026-08-09, no ADR — a proof composes; no new serialized record → no fuzz).** ECS
system parallelism runs on the foundation with ZERO new machinery — the 4d hazard analysis (`ops_hazard` /
`collect_block_hazards`) over 4a `EcsRead`/`EcsWrite` effects IS the parallelism oracle. The mock `ecs` dialect is
INLINE-registered (movement/render/physics/input/spawn/coarse). Proof: `test_ecs.cpp` (4 `[ceir][ecs]`).

⛔ **The U-§97 verdict — NO second scheduler / parallelism analysis (three parts):** (1) **ECS parallelism inference IS
the 4d hazard analysis over declared effects** — `ops_hazard`/`collect_block_hazards` UNMODIFIED; no scheduler was built
(the scheduler CONSUMES these edges at CEIR-12d, named-forward). Two systems touching disjoint components (a Position
writer vs a Health writer) → NO hazard → parallel; a shared component with ≥1 write → ordered (exact kinds: RAW/WAR/WAW).
The compiler derives the FULL 6-edge ordering set from declared effects alone, and the four disjoint pairs have no edge.
(2) **Per-component identity IS the SSA Value** — each component is a distinct block-arg Value, a system's effect targets
an OPERAND (the canonical 4d resource model, not a workaround). The 8c `EcsComponent` location KIND was exercised ONLY
as the conservative whole-class fallback: a `coarse` system targeting `EcsComponent` resolves whole-class → conflicts
with EVERY Ecs access, including two systems that are mutually parallel — safe-but-pessimal (MORE hazards, never fewer;
EMPTY≠UNKNOWN applied to location identity). Precision is opt-in via the operand. (3) A structural mutation needs NO
special mechanism — a whole-store write (a null resource) is the barrier by the existing nullptr conflict rule.

⛔ **The one foundation change any proof has forced: correcting a STALE forward-claim.** The 4d source comment said the
whole-class location fallback was "named-forward to 8d" — but 8d shipped per-OP stable identity, NOT per-LOCATION
identity; the pointer was never fulfilled (the 8z "never inherit a stale forward-claim into a verdict" scar). The
comment was fixed IN PLACE to the honest unbound form (a deliberate conservative fallback; per-instance location
identity is an unbound future refinement, not bound to a fake band). A comment-only crd-ceir source edit, re-gated
across 4 configs.

⭐ **Running tally (a 9z synthesis input): SIX domains in (9a notebook, 9b DAW, 9c DCC, 9d CAD, 9e EDA, 9f ECS), the
per-slice U-§97 answer has been NO every time — zero foundation edits, zero engine-line changes; the ONLY foundation
change any proof has forced is correcting one stale comment.**

**CEIR-9g — U7 Agent transaction (2026-08-09, no ADR — a proof composes; no new serialized record → no fuzz).** The
CULMINATION of band 9: the FULL agent loop runs on the foundation with ZERO new machinery. A mock agent DISCOVERS ops
via the machine-readable `OpSchema` registry (the surface whose header declares itself "CLI/MCP/agent discovery
(section 122)"; `.ops.json` is its OUT-of-process serialization, and the in-process `<dialect>_op_schemas()` tables are
the same registry — no JSON reader is needed, and none exists in the house). ⛔ **ZERO hard-coded op knowledge:** the
agent SELECTS by property (arity + result count), interns via the discovered `schema.dialect`/`name`, and the
`op_name(kind) == schema.qualified` assertion closes the loop. Proof: `test_agent.cpp` (3 `[ceir][agent]`).

⛔ **The U-§97 verdict — NO second authoring / verification / diagnostic / discovery system (three parts):** (1) **the
agent loop is a thin composition over four EXISTING surfaces** — OpSchema discovery + 8i authoring (unmodified) +
verification at two granularities + 8g diagnostics (read by CODE, not string) — zero new systems, zero hard-coded op
names. (2) **The three-legged authoring contract, layered honestly:** the schema (arity + required attrs + attr kinds)
+ a chosen type + STRUCTURAL verify is DEMONSTRATED (test 3 proves discovery-by-arity-alone is insufficient — a lazy
agent authoring a source WITHOUT its schema-required attr is caught by the op-local commit-verify backstop). SEMANTIC
type validity is a FURTHER, BOUND named-forward: the generated verifiers are structural, as their own source stamps —
"Semantic verification (types/effects/domain) lands at CEIR-3/4"; the full type-CONSTRAINT language is CHIR. Faking a
type-checking verifier to fit the design-call assumption would have been the failure; the proof adapted on primary
evidence. (3) **The op-local / module-wide composition DEMONSTRATED, closing the 9e named-forward:** the agent runs a
MODULE-WIDE `find_structure_error` SWEEP MID-TX (8i's eager application makes the authored state visible before commit)
— it catches a defect NO op-local commit-verify can see (a same-block forward reference → `FeedbackWithoutState`, a
combinational feedback edge in a Graph region, exact kind pinned) → the agent emits its OWN 8g diagnostic
(`agent.sweep.structure_defect`, carrying the offender's loc), reads it by code, and rolls back byte-identically; the
op-local commit-verify is the BACKSTOP behind the sweep (belt-and-suspenders), catching what a lazy agent skips.

⭐ **Running tally: SEVEN domains in (9a–9g), the U-§97 answer has been NO every time — zero engine-line changes; the
ONLY foundation change any proof has forced is correcting one stale comment (9f).**

**CEIR-9h — U8 the MANDATORY external-plugin proof (2026-08-09, no ADR — a proof composes; a foreign-plugin blob DOES
serialize → fuzz-swept).** The band's heaviest slice and the ACID TEST of the open-world thesis: a genuinely EXTERNAL
plugin — a whole `plugin` dialect authored in a test-side namespace (`plugin_ext`) that includes ONLY the public
crd-ceir headers and edits NO engine source — registers its FULL surface through the PUBLIC open-world APIs. Proof:
`test_plugin.cpp` (5 `[ceir][plugin]`).

⛔ **The U-§97 / U-§116 verdict — the plugin needed ZERO core edits and hit NO private wall (three parts):** (1) **the
acid test passed, and the claim is now MECHANICAL, not narrative** — NINE surfaces registered through public doors (a
custom TYPE class [8a] + ATTRIBUTE class [8b] + effect-LOCATION class [8c] + op INTERFACE [8e] + custom OPS with
verifiers + a REWRITE + a lowering `ConversionTarget` + an `IExecutionProvider`); the "zero central-enum edits" is a
STANDING GATE in `crd-ceir-invariants` (U-§116: `TypeKind`/`AttrKind` end at `Extern` via the script pin; `EffectFamily`
via its existing `kLastEffectFamily` compile-time `static_assert` — two mechanisms, one gate), passing on both
platforms; the in-test assertions confirm the custom type/attr went through the `Extern` doors. A future slice quietly
widening a central enum to fit a plugin trips it forever. (2) **The 8g rewrite framework carried its FIRST real
pattern** — `A(B(x)) → C(x)` fired via `try_apply` (a genuine IR mutation: create-C-wired-to-x → RAUW A's result →
erase A then B-if-dead), authored ENTIRELY plugin-side, with the worklist driver still cleanly RESERVED (the pattern
needed only the caller-driven shape — evidence that 8g's skeleton boundary was drawn right); a non-matching op returns
false. (3) **Two honest ASYMMETRIES, named (neither a private wall):** (a) a plugin HAND-AUTHORS its `OpSchema` table
where a first-party dialect gets it GENERATED — a tooling asymmetry, unbound named-forward (a plugin-side schema
helper/generator mode); (b) the plugin's execution is a CONTRACT stub (`advertises` real, `execute` = `NoSemantics`) —
real dispatch is CEIR-24/29. The registration surface is COMPLETE. The plugin's custom type/attr round-trip
text⇔binary + preserve through an unregistered host + unify on re-register (U-§56 for a foreign plugin), and a
single-byte corruption sweep over the plugin blob is ASan-clean; the plugin is DISCOVERABLE by the 9g agent path over
its hand-built schema table (the U-§117 close).

⭐ **Running tally — EIGHT domains in (9a notebook, 9b DAW, 9c DCC, 9d CAD, 9e EDA, 9f ECS, 9g agent, 9h external
plugin), the U-§97 answer has been NO every time — ZERO engine-line changes across all eight; the only foundation
changes any proof has forced are one stale-comment correction (9f) and one STANDING GATE ADDED (the U-§116 enum-pin —
gate infrastructure, not engine). The strongest cross-slice claims: 9c "two drivers, one engine"; 9e "the vocabulary
was domain-neutral before the domain arrived"; 9h "a whole foreign dialect — type to provider — landed without one core
line."**

## I. Domain-proof matrix (U-§122 — skeleton; CEIR-9z fills every cell from evidence)

| Domain (proof) | Required semantics | Current representation | Missing concepts | 2nd runtime needed? | Resolution |
|---|---|---|---|---|---|
| Renderer/frame graph (CEIR-15) | resources·passes·history·present | planned: `ceir.frame` over the ONE runtime | — | **NO expected** — one runtime, `ceir.frame` dialect over 4d hazards + 5d history | ○ **future (CEIR-15)** — architectural space validated by the band (resources=state, passes=ops, barriers=4d), the domain unproven |
| General compute (CEIR-13) | dispatch·transfer·barriers | planned over canonical command model | — | **NO expected** — canonical command model + 4d hazards for barriers | ○ **future (CEIR-13)** — space validated (dispatch=op, transfer/barrier=4d effects), unproven |
| MATLAB-like workflow | high-level numerics·provider choice·lazy eval | CEIR-21/22 dialects | incremental (8h) | **NO expected** — 8h incremental (lazy eval) + provider/capability choice | ◑ **analogous-proven** — planned (CEIR-21/22); the incremental+lazy shape is the 9a notebook + 9c depsgraph proof |
| Notebook/incremental (9a) | dependency invalidation + early-cutoff | ✅8h IncrementalDag (content=formula/interface=value ⇒ §107 = early-cutoff) + 8i tx edits + 8d cell identity; a ~100-line test consumer | — | **NO** — 8h engine + thin consumer | ✅ **9a PROVEN** (`test_notebook.cpp`: exact counts, chained cutoff, no-op memo) |
| DAW/audio (9b) | sample time·latency·feedback·RT-safety | ✅8f time domains + 5d state/feedback + 4c region legality + 8e latency interface; offline=tag-flip; multi-rate=distinct domains | — | **NO** — 8f + 5d + 4c + 8e | ✅ **9b PROVEN** (`test_daw.cpp`: trait-keyed feedback, tag-flip offline, two-oracles subset) |
| DCC modifier graph (9c) | incremental cache·partial re-eval | ✅8h IncrementalDag `affected_by` AS the tag pass (Blender depsgraph); two-branch join, §107 early-out; facets=extra nodes | — | **NO** — 8h `affected_by` as the tag pass | ✅ **9c PROVEN** (`test_dcc.cpp`: tag-then-eval, upstream isolation, cut named) |
| CAD parametric (9d) | document state·constraints·transactions | ✅8i tx (multi-op + byte-identical rollback) + ✅8h affected_by + ✅8c ConstraintRead classification + verifier enforcement (constraint = commit-verify) | constraint solver/dialect (future) | **NO** — 8i + 8h + 8c + verifier | ✅ **9d PROVEN** (`test_cad.cpp`: multi-op union, rollback A/B, constraint reject) |
| CAM | geometry→toolpath→external post | provider/capability model + ✅8f capabilities (external.process grant) | provider partitioning (24/29) | **NO expected** — 8f caps (external.process/file.write) + provider model | ◑ **analogous-proven** — planned (CEIR-24/29); the external-post shape is the 9e EDA host-granted external-export proof |
| PCB/EDA/Gerber (9e) | doc objects·rules·external export | ✅8f caps (external.process/file.write, host-grant) + 4a/4b tool metadata + content-addressed tool id + 8c Document/Constraint; DRC=board verifier | external-provider dispatch (24/29) | **NO** — 8f caps + 4a/4b + 8c + verifier | ✅ **9e PROVEN** (`test_eda.cpp`: host-grant, typed contract, U-34 domain-neutral) |
| Game/simulation (9f) | ECS effects·legal parallelism | ✅4d hazards over 4a EcsRead/EcsWrite; per-component = SSA Value (Operand target); EcsComponent kind = pinned conservative fallback; whole-store write = barrier | scheduler consumes edges (12d) | **NO** — 4d hazards over 4a Ecs effects + Value identity | ✅ **9f PROVEN** (`test_ecs.cpp`: disjoint=parallel, shared-write=ordered, 6-edge set, barrier) |
| Physics/CAE | solver orchestration·determinism | determinism tiers ready | provider partitioning (24/29) | **NO expected** — determinism tiers + 4d hazards for solver-step orchestration | ◑ **analogous-proven** — planned (CEIR-24/29); the effect-ordering shape is the 9f ECS hazard proof |
| ML (CEIR-24) | high-level semantics until fusion | planned | — | **NO expected** — high-level ops until a fusion lowering (8g rewrite) | ○ **future (CEIR-24)** — space validated (semantics=ops, fusion=8g conversion), unproven |
| Media | mixed providers·timelines | CEIR-31 planned + ✅8f time domains (sequencer/logical) | CEIR-31 | **NO expected** — 8f time domains + provider/capability model | ◑ **analogous-proven** — planned (CEIR-31); the time-domain mechanism is the 9b DAW multi-rate proof; mixed-provider domain unproven |
| Build pipeline | content-addressed incremental | ✅8h engine + cook cache (content/interface keys unified semantically) | 10b plan cache | **NO** — 8h engine + cook cache (content/interface keys) | ◧ **foundation-validated** — the incremental core is proven (9a/9c); no dedicated build-pipeline proof; real domain at CEIR-10b |
| Reactive UI | signals·events·transactions | ✅8e reactive-region reservation policy + typed interface surface + ✅8i transactions (signal-graph edits atomic) | signal dialect (future) | **NO** — 8e reactive-region policy + 8i atomic edits | ◧ **foundation-validated** — the mechanisms are proven (8e reservation + 9b/9d transactions); no dedicated proof; signal dialect future |
| Agent-driven editing (9g) | discovery·transactions·introspection | ✅ the FULL agent loop: OpSchema discovery (zero hard-coded op names) + 8i authoring + verify at 2 granularities (op-local backstop + module-wide sweep) + 8g diagnostics by code | semantic type verify (3/4); rewrite driver (26) | **NO** — OpSchema + 8i + verifiers + 8g | ✅ **9g PROVEN** (`test_agent.cpp`: discover-by-property, sweep-vs-backstop, byte-identical rollback) |
| Distributed compute (CEIR-30) | device mesh·collectives | planned | — | **NO expected** — device mesh as location classes (8c) + collective ops | ○ **future (CEIR-30)** — space validated (mesh=location classes, collectives=ops+effects), unproven |

**Legend (evidence tier — CEIR-9z, honest).** The §I matrix has **17 domain rows.** ✅ **PROVEN** — a band-9 test consumer drives the domain through the substrate (**7 rows**: 9a–9g). ◧ **foundation-validated** — every mechanism the domain needs is proven by other band-9 slices, but no domain consumer was driven (**2 rows**: Build pipeline, Reactive UI) — a composition of already-proven parts, not a prediction. ◑ **analogous-proven** — planned for a consumer band; the domain's characteristic shape is already proven by an analogous 9x slice (**4 rows**: MATLAB, CAM, Physics/CAE, Media). ○ **future** — planned for a consumer band; the architectural space is validated by the band but the domain itself is unproven (**4 rows**: Renderer-15, Compute-13, ML-24, Distributed-30). A **ninth** proof — 9h, the foreign external plugin — validates the open-world *doors* (custom type→attr→location→provider) rather than a matrix domain, so it is not a matrix row: **8 proof slices in all** = 7 mock-domain rows (9a–9g) + 1 foreign plugin (9h). **In no row does the honest answer to U-§97 turn to YES** — every domain, proven or planned, resolves onto the one runtime + open-world doors. The **7 PROVEN + 2 foundation-validated** rows rest on shipped, exercised mechanisms; the **8 planned rows** (◑ + ○) are labelled as predictions, not results.

### The U-§97 verdict tally (every band-9 slice, one place)

The single question asked at each slice — *"did this domain need a second scheduler / compiler / runtime / depsgraph / transaction-manager / capability-system?"* — target answer **NO** for a universal substrate.

| Slice | Domain | U-§97 answer | What the domain reused (no new runtime) | Engine lines changed |
|---|---|---|---|---|
| 9a | Notebook/incremental | **NO** | 8h IncrementalDag (§107 early-cutoff) + 8i tx + 8d identity | 0 |
| 9b | DAW/audio | **NO** | 8f time domains + 5d state/feedback + 4c region legality + 8e latency | 0 |
| 9c | DCC modifier graph | **NO** | 8h `affected_by` AS the depsgraph tag pass (pull driver) | 0 |
| 9d | CAD parametric | **NO** | 8i tx (multi-op + byte-identical rollback) + 8h + 8c ConstraintRead + verifier | 0 |
| 9e | PCB/EDA/Gerber | **NO** | 8f caps (host-grant) + 4a/4b tool metadata + 8c Document/Constraint + board verifier | 0 |
| 9f | Game/ECS | **NO** | 4d hazards over 4a Ecs effects; per-component = SSA Value identity | 0 (1 stale-comment fix) |
| 9g | Agent-driven editing | **NO** | OpSchema discovery + 8i authoring + verify ×2 granularities + 8g diagnostics | 0 |
| 9h | External plugin (foreign dialect) | **NO** | 8a/8b/8c registration doors + serialization + unify-on-re-register | 0 (1 standing gate ADDED: U-§116 enum-pin) |

**Tally: 8 proof slices, U-§97 = NO ×8. Zero engine-line changes across all eight.** The only foundation deltas any proof forced: one stale-comment correction (9f `op_access_at`) and one standing gate *added* (the U-§116 enum-pin — gate infrastructure that mechanically enforces "zero central-enum edits", not an engine change). This is the band's falsifiable result: a universal substrate is one where a new domain adds its own types/attrs/effect-locations/interfaces/capabilities/providers through the open-world doors and reuses the one runtime, and eight independent domains + one foreign plugin did exactly that.

## J–M. (Agent-authoring · optimization architecture · tests · documentation — accrue with 8g/8i/9g/9h and each gate)

## N. Re-baselined roadmap (executed 2026-08-09 — the four user verdicts)

1. **Full renumber** (quest §113): closed bands 0–7 frozen; NEW CEIR-8 (Foundation Closure) + CEIR-9 (Universality
   Validation); 7c/7d/7z → CEIR-10; old 8–13 → 11–16; old 14–32 → 17–35. The mapping table lives in the tracker's
   re-baseline banner.
2. **Band 7 paused at 7b** — 7c consumed 8d (stable state identity), 7d consumed 8h (incremental): foundation first.
3. **Cadence** — foundation ADRs advisor-approved, fully autonomous; gold-standard mandate verbatim: *"The best and
   most world class and gold standard and elite ADR's will be approved by the Advisor or agent. EVERYTHING MUST BE
   GOLD STANDARD AND UNIVERSAL!"*
4. **CEIR-9 = 8 separate proof slices** + gate (attribution over consolidation).

**Recorded debts the re-baseline pays (not new scope):** ADR-0109 §6 stable identity (promised at 1c) → 8d;
attr.hpp:6 extended attr kinds (promised at CEIR-2/3) → 8b; capability contracts (flagged at 7a) → 8f; state cells
instance-keyed (recorded at 7a) → 8d.

## O. Remaining deliberate limitations (honest, current)

- **CHIR stays design-only until CEIR-32** — the corpus corrects the language design before it binds (0e).
- **Symbolic computation (U-§26), constraint dialect (U-§29), query/stream dialects (U-§44/45), e-graphs (U-§68)**
  — architectural SPACE is validated by 8e/8g/9x proofs; implementation intentionally unscheduled until a consumer
  band opens (the no-consumer rule).
- **Formal/certification hooks (U-§109), capture/replay (U-§104), distributed (U-§77)** — reserved seams, proven
  non-blocking by the 8-band designs; not built.
- **The U-§126 answer, FOUNDATION scope (post-CEIR-8, 2026-08-09):** the closed vocabularies and the missing
  incremental/transaction models are CLOSED (the DoD table above, 20/23; the 3 remainders are consumer-band-deferred,
  not foundation gaps). A new domain now reuses ops/effects/state/identity/serialization/incremental/transaction/
  diagnostics cleanly AND adds its own types/attrs/effect-locations/interfaces/capabilities/providers with zero core
  edits — proven composed in `test_band8_gate.cpp`. The remaining U-§126 work is EMPIRICAL, not architectural: CEIR-9
  drives 8 real mock-domains + the mandatory external-plugin proof (9h) through the substrate and fills the §I domain
  matrix from evidence; the final cross-domain answer is recorded at CEIR-9z. The FOUNDATION half of the answer is now
  affirmative.

- **The U-§126 answer, EMPIRICAL scope (CEIR-9z, 2026-08-10) — the final cross-domain verdict.** *Is CEIR a universal
  execution substrate?* **Yes, at proof scale — validated, not merely designed.** The band drove seven independent
  mock-domains (notebook · DAW · DCC · CAD · EDA · game/ECS · agent-editing) plus one **foreign external plugin** (9h,
  a whole dialect from custom type to provider contract) through the one runtime — **eight proof slices in all**. The U-§97 question — *did this domain
  need a second scheduler / compiler / runtime / depsgraph / transaction-manager / capability-system?* — was answered
  **NO all eight times**, with **zero engine-line changes** across the entire band (the only deltas: one stale-comment
  fix and one standing gate *added*, the U-§116 enum-pin, which mechanically enforces the zero-central-enum claim). Each
  proof is a *composition* of already-shipped foundation slices, not a bespoke domain runtime — which is the operational
  definition of universal. The §I matrix's other ten rows are labelled by evidence tier (◧ foundation-validated ·
  ◑ analogous-proven · ○ future), never as results: two reuse only already-proven mechanisms, four share a characteristic
  shape with a proven 9x slice, and four are genuine future domains whose architectural space the band validated but
  whose domain code is unbuilt. **The boundary, stated plainly:** this is validation at *proof / mock-domain* scale —
  each consumer is a ~100-line test driver, not a production DAW or renderer. Production-scale validation (real data
  volumes, real provider dispatch, real solver kernels) accrues as the consumer bands CEIR-10→35 build the actual
  domains on this substrate; each will re-ask U-§97 against reality. What CEIR-9 establishes is that the substrate is
  *ready* for them: no known domain in the seventeen requires a second runtime, and eight have been proven not to. **The
  answer is affirmative in both halves — foundation (architectural) and empirical (cross-domain) — bounded honestly to
  proof scale.**
