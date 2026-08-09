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
| Capability contracts | flagged ownerless at 7a; §107 interface-hash field unbuilt | ❌ missing | **CEIR-8f** (the user placed it here) |
| Exec domain vs safety | `EvalDomain` + `RealtimeClass` exist (4c); may-allocate/block/IO axes absent | ◧ partial | **CEIR-8f** |
| Time domains | none typed (RealtimeClass is an execution class, not a time domain) | ❌ missing | **CEIR-8f** |
| Analysis/Pass/Rewrite | ad-hoc `find_*` + one fold; no AnalysisManager/PassManager/patterns/conversion | ❌ missing | **CEIR-8g** (frameworks; passes stay in CEIR-26/27) |
| Diagnostics | per-verifier pointing structs (uniform but no codes/engine) | ◧ partial | **CEIR-8g** (DiagnosticEngine) |
| Incremental evaluation | content/interface hashes exist; NO dependency/dirty-propagation model; the tree already holds ≥2 dependency graphs (render-asset-core `DependencyGraph` dependency.cpp:115 + CookDb edges) | ❌ missing + duplication risk | **CEIR-8h** (engine-first unification) |
| Transactions | nothing, anywhere | ❌ missing | **CEIR-8i** |
| Provenance | SourceLoc on every op; transform-preservation policy untested (few transforms exist) | ◧ adequate now | policy at 8g; proven through CEIR-26 |
| Serialization/versioning | strong (v2, forward-skip, fuzzed); custom types/attrs extend it at 8a/8b | ✅ strong | extended at 8a/8b/8d (versioned recooks) |
| Unknown-plugin content | unknown-dialect ops preserved (1d); no execution-denial policy doc | ◧ partial | 8a/8e ADRs document the policy (U-§56) |
| Providers | `IExecutionProvider` + advertises(); partitioning at CEIR-24/29 | ✅ seam correct | unchanged (U-§75 lands with partitioning) |
| Structured concurrency | band 6 complete (jobs-backed, bit-identical, cancellable) | ✅ | — |
| Units/ownership/shapes | 3e/3f/3d complete with verifiers | ✅ | — |
| Agent introspection | .ops.json + OpSchema; no transaction/query surface yet | ◧ partial | 8i + 9g |
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

*(8f–8i append here as they land.)*

## D–H. (Accrue at CEIR-8b–8i: extension model completion · state+transaction model · incremental model · compiler architecture · provider architecture)

Filled slice-by-slice; each CEIR-8 close appends its section here.

## I. Domain-proof matrix (U-§122 — skeleton; CEIR-9z fills every cell from evidence)

| Domain (proof) | Required semantics | Current representation | Missing concepts | 2nd runtime needed? | Resolution |
|---|---|---|---|---|---|
| Renderer/frame graph (CEIR-15) | resources·passes·history·present | planned: `ceir.frame` over the ONE runtime | — | *(9z)* | *(9z)* |
| General compute (CEIR-13) | dispatch·transfer·barriers | planned over canonical command model | — | *(9z)* | *(9z)* |
| MATLAB-like workflow | high-level numerics·provider choice·lazy eval | CEIR-21/22 dialects | incremental (8h) | *(9z)* | *(9z)* |
| Notebook/incremental (9a) | dependency invalidation | — | 8h | *(9z)* | *(9z)* |
| DAW/audio (9b) | sample time·latency·feedback·RT-safety | StateEdge + RealtimeClass partial + ✅8c audio-loc + RT legality | 8f time domains | *(9z)* | *(9z)* |
| DCC modifier graph (9c) | incremental cache·partial re-eval | — | 8h | *(9z)* | *(9z)* |
| CAD parametric (9d) | document state·constraints·transactions | ✅8d stable identity for doc objects/state + ✅8c Constraint family | 8i·constraint dialect (future) | *(9z)* | *(9z)* |
| CAM | geometry→toolpath→external post | provider/capability model | 8f capabilities | *(9z)* | *(9z)* |
| PCB/EDA/Gerber (9e) | doc objects·rules·external export | ✅8c Document/Constraint families + doc-object locations | 8f capabilities | *(9z)* | *(9z)* |
| Game/simulation (9f) | ECS effects·legal parallelism | effect machinery ready + ✅8c EcsComponent location kind | — | *(9z)* | *(9z)* |
| Physics/CAE | solver orchestration·determinism | determinism tiers ready | provider partitioning (24/29) | *(9z)* | *(9z)* |
| ML (CEIR-24) | high-level semantics until fusion | planned | — | *(9z)* | *(9z)* |
| Media | mixed providers·timelines | CEIR-31 planned | 8f time domains | *(9z)* | *(9z)* |
| Build pipeline | content-addressed incremental | cook cache exists | 8h unification | *(9z)* | *(9z)* |
| Reactive UI | signals·events·transactions | ✅8e reactive-region reservation policy + typed interface surface | 8i transactions | *(9z)* | *(9z)* |
| Agent-driven editing (9g) | discovery·transactions·introspection | .ops.json partial | 8i·9g surface | *(9z)* | *(9z)* |
| Distributed compute (CEIR-30) | device mesh·collectives | planned | — | *(9z)* | *(9z)* |

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
- **The U-§126 answer today (pre-8/9), honestly:** a new domain would reuse ops/effects/state/providers cleanly but
  would hit the closed type/attr/location vocabularies and the missing incremental/transaction models — exactly the
  gaps CEIR-8 closes. The target answer ("new dialects + libraries + providers; core changes only for a truly new
  universal primitive") is recorded as the CEIR-9z gate criterion.
