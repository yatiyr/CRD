# CEIR band 8 — Foundation Closure (the universality quest, part 1)

Band 8 closes every U-§112 foundation item BEFORE any further feature band: open-world types/attrs, effect
widening + extensible locations, stable semantic identity, trait/interface split + region reservation, capability
contracts + domain/safety split + time domains, the analysis/pass/rewrite/diagnostic skeleton, incremental
evaluation, transactions. Nothing shipped is rewritten — every slice EXTENDS the live substrate with all prior
tests green (format changes are versioned recooks, named per slice). Cadence: ADR-quality design → advisor gate →
implementation → tests → 4-config gate, per slice, fully autonomous under the gold-standard mandate.

## CEIR-8a — the open-world TYPE model (ADR-0111) — 2026-08-09

**The gap.** The op layer is genuinely open-world (interned `OpId`, no central switch, unknown-dialect
preservation). The type layer was not: `TypeKind` a closed `u8` enum, every consumer switching on it — a domain
needing a genuinely new type (CAD `BRep`, PCB `Net`, half-edge mesh) would edit that central enum, the
central-switch bottleneck U-§5/§125 forbid.

**The design (ADR-0111, advisor-approved).** Apply the op model to types. `TypeKind::Extern` is the ONE-TIME enum
edit — the open door. A `TypeClassId` (interned FNV "dialect.class", a distinct id mirroring `OpId`) +
`Dialect::register_type_class(TypeClassSpec{verify hook, version})` → a `TypeClassInfo` on the Context. An `Extern`
`Type` carries `type_class` + `type_class_version`; its parameters ride the existing 8 slots
(`members`/`count`/`cols`/`is_signed`/`fkind`/`name`/`labels`) — so custom types inherit interning, structural
equality/hash, arena spans, and serialization for FREE (the 3a machinery). Every custom type after `Extern` lands
with zero further enum edits — proven by `testcad.toposhape` AND `testeda.net`.

**The advisor's load-bearing rulings, all implemented.**
- **`operator==` + the intern hasher + `type_is_canonical` include the class fields.** Two different classes with
  identical params are DIFFERENT types; omitting `type_class` from equality would intern them to one id (silent
  type confusion). The canonicality guard additionally rejects a class on a non-Extern kind (which would print as
  the plain type but intern distinctly — the prints-identically-but-differs hazard) and a class-0 Extern.
- **Generic canonical text form only — NO pretty print/parse hooks in 8a.** A pretty hook emits text an
  UNREGISTERED Context cannot re-parse, structurally breaking the U-§56 unknown-plugin round-trip and 1e's
  byte-exact property. The generic form (`!extern<CLASS,VER,COUNT,COLS,SIGNED,FKIND,"NAME",NMEM,m..,NLAB,"l"..>`)
  round-trips universally, registered or not. Pretty forms → language/editor bands, named-forward WITH the reason.
- **String-keyed serialization** (the class STRING via STRP, mirroring op-name encoding). Binary: an Extern
  record's trailing class-string + version, ONLY on `kind==Extern` → NO version bump (pre-8a decoders reject the
  out-of-range Extern kind; pre-8a blobs decode byte-identically — tested). The decode kind bound was raised from
  `Qualified` to `Extern` (the very check that makes older decoders reject new-kind blobs — validating the
  no-bump reasoning).
- **The verify TRIPLE:** the factory `type_extern` ASSERTS (builder misuse); the binary decoder + text parser
  REJECT (registered-invalid, hostile input); an UNREGISTERED class PRESERVES opaquely at all three (U-§56).
- **Version range-check:** a newer-schema record than the loader = reject; an older record = accept (migration
  named-forward). Consequence, stated honestly: an old-version record is a DISTINCT type from the current schema.
- **Interface-hash class emission is CONDITIONAL** (only Extern) — custom types discriminate by class (else 7b's
  registry-drift discriminator silently breaks), ZERO churn for existing content.

**Generics** (`substitute`/`type_has_params`) walk `members` generically — a generic custom type substitutes
through with its class preserved (the `Type nt = t` copy carries `type_class` automatically). No per-kind arm
needed. Shape/broadcast queries reach an Extern only by caller error → degrade to Unknown/false via existing
defaults.

**Advisor pre-close caught three blockers, all fixed + re-gated:** the junk-field canonicality gap (the new
fields must be default on non-Extern + an Extern must name a class — two lines + two assertions); the U-§56
headline was binary-only (added the text leg + the late-register-unify proof); the parser-reject leg of the verify
triple was unexercised (test named it but only ran the decoder — added the print→parse-registered→reject leg).

**Tests.** `test_extern_type.cpp` (10 `[extern-type]`): dedup + the different-class/same-params discriminator; the
canonicality guard; text+binary round-trip byte-exact; the U-§56 headline (both forms + late-register unify); the
verify triple; interface-hash class discrimination; version range (newer-reject + older-accept); substitute-
through-Extern; a pre-8a no-churn round-trip; and a single-byte-corruption fuzz sweep over the Extern decode path
(ASan-proven, both registered and unregistered).

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **242/242 ctest** (232 + 10) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh against the new `Type`
layout (the stale-.obj scar) — + LLVM-20 tidy (11 files) + GCC `-Werror=switch` (every `TypeKind` switch carries
the Extern arm; context.cpp:714 has a default) + `crd-ceir-opgen-{drift,validator}` (no TOML change) +
`crd-ceir-invariants` + invariants. No binary version bump. Next = CEIR-8b (open-world ATTRIBUTE model — mirrors
this whole shape: `AttrKind` extension, class registry, conditional serialization, the same canonicality guard;
plus the deferred aggregate attr kinds — the attr.hpp CEIR-2/3 IOU).

## Proposed commit — CEIR-8a (user commits; NO AI trailer; ADR-0111 ships in the same batch)

```
feat(ceir-8a): open-world type model -- dialect-defined type-classes (ADR-0111)

- TypeKind::Extern (the ONE-TIME enum edit) + TypeClassId (interned "dialect.class",
  mirroring OpId) + Dialect::register_type_class(TypeClassSpec{verify, version}) -> a
  Context type-class registry. An Extern Type carries type_class + type_class_version;
  parameters ride the existing slots, inheriting 3a interning/equality/hash/serialization.
  Every custom type after Extern lands with ZERO further enum edits (proven: testcad.toposhape,
  testeda.net).
- operator== / the intern hasher / type_is_canonical include the class fields (two classes
  with identical params are DIFFERENT types); the junk-fields-Extern-only canonicality guard
  closes the prints-identically-but-differs hazard.
- Generic canonical text form ONLY (no pretty hooks -- they break the U-56 unknown-plugin
  round-trip). String-keyed binary (class via STRP), a trailing class+version ONLY on Extern
  records -> NO version bump (pre-8a blobs decode byte-identically). The verify triple: factory
  asserts / decoder + parser reject (registered-invalid) / unregistered preserves. Version
  range-check on decode (newer reject, older accept). Interface-hash emits the class
  CONDITIONALLY (zero churn for existing content).
- ADR-0111 (open-world type model) ACCEPTED. tests: test_extern_type (10 [extern-type]) incl.
  the U-56 headline (text + binary), the verify triple, version range, substitute-through, and
  a single-byte fuzz sweep over the Extern decode path.

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 242/242 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh against the new Type layout) +
LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants. No binary
version bump.
```

## CEIR-8b — the open-world ATTRIBUTE model (ADR-0112) — 2026-08-09

**The gap.** The attribute layer had a fixed six-kind `AttrKind` (Int/Float/Bool/String/SymbolRef/Type) and no
dialect-extensible attribute vocabulary — and the `attr.hpp` header carried a CEIR-2/3 IOU for the aggregate kinds
(arrays/dicts/typed constants) that was never paid. A domain needing a units-tagged constant, a structured config,
or a plugin-defined attribute had nowhere to put it but a stringly-typed escape hatch (the U-§125 anti-goal).

**The shape (ADR-0112).** The 8a type-model surface applied to attributes: TWO wrappers — `TypedConst` (a value OF a
`wrapped_type`, the units story) and `Extern` (a dialect attribute-class + version) — over ONE aggregate vocabulary
— `Array` (an ordered `elems` span) and `Dict` (parallel byte-order-SORTED `keys` + `elems`). An `AttrClassId`
(interned FNV "dialect.attr", mirroring `TypeClassId`/`OpId`) + `Dialect::register_attr_class(AttrClassSpec{verify,
version})` → a `Context` attr-class registry. Custom + aggregate attributes ride reused `AttrValue` slots, inheriting
1c interning/equality/hash/serialization FREE. `AttrKind::{Array,Dict,TypedConst,Extern}` is the ONE-TIME enum edit
that opens the door — every dialect attribute-class after lands with ZERO further enum edits (proven:
`testunit.dimension`).

**The load-bearing invariants.** `operator==` + `attr_is_canonical` include payloads/spans/`attr_class`/
`wrapped_type` (a different class or wrapped-type with identical payload = a DIFFERENT attr — the landmine); the
kind-scoped junk guard (`i==0` reads the whole scalar union) closes the prints-identically-but-differs hazard; **Dict
keys byte-order SORTED + UNIQUE** (`attr_dict` canonicalizes at intern; a hand-built unsorted/duplicate dict is
REJECTED, never asserted); the **wrapper-composition rule** — a wrapper's payload must not itself be a wrapper (the
`qty<qty>` precedent), gated by `verify_attr_extern` for BOTH wrappers; a **child-first ATTR pool** in the binary
encoder (aggregates/wrappers pool their child `AttrId`s before their own index, mirroring `intern_type_pool` — the
decoder rejects a forward ref by construction). The new fields serialize ONLY on the new kinds → the six scalar
encodings are BYTE-IDENTICAL to pre-8b → NO version bump, ZERO recook (a module's `stable_hash` is unchanged). Attrs
feed the CONTENT hash (`stable_hash`) but NOT the interface hash — proven with a func whose body-op attr changes one
and not the other.

**Advisor pre-close caught three gaps, all fixed + re-gated:** (1) a BLOCKER — `parse_dict_attr` routed through
`attr_dict`, which sorts but does NOT dedup, so a duplicate-key TEXT input would hit `intern_attr`'s canonical
ASSERT (a crash on hostile input, violating reject-not-assert); fixed by building `of_dict` raw + a graceful
`attr_is_canonical` reject, mirroring the binary arm. (2) the parser depth guard + empty aggregates were untested;
added a 80-deep nesting reject + `[]`/`{}` round-trip. (3) the version range-check was binary-only in BOTH 8a and 8b
text parsers — a latent text/binary asymmetry; RETROFITTED the check into `parse_wrapper_attr` (attrs) AND 8a's
`parse_extern` (types), so text and binary now agree on validity in both slices, and added the text leg to the
version test. (Finding 4 — a binary depth guard — was correctly NOT added: the decoder is iterative over a
child-first pool with forward-ref rejection, so there is no recursion to bound; adding one would be dead code.)

**Tests.** `test_extern_attr.cpp` (13 `[extern-attr]`): dedup + the wrapper/class discriminator; the canonicality
guard (junk-on-scalar, unsorted/duplicate dict, classless Extern); nested text+binary round-trip (the child-first
pool); empty aggregates; the U-§56 headline (both forms + late-register unify); the verify triple; the
wrapper-composition rule; content≠interface hash; version range both forms; a hostile unsorted/duplicate-dict TEXT
graceful-reject (the assert landmine); the depth guard; a pre-8b no-churn round-trip; and a single-byte-corruption
fuzz sweep over the aggregate/wrapper decode arms (ASan-proven, registered + unregistered).

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **255/255 ctest** (242 + 13) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh against the new
`AttrValue` layout (the stale-.obj scar) — + LLVM-20 tidy (10 files) + GCC `-Werror=switch` (every `AttrKind` switch
carries the four new arms) + `crd-ceir-opgen-{drift,validator}` (no TOML change) + `crd-ceir-invariants`. No binary
version bump. Next = CEIR-8c (effect widening — the family mask u32→u64 + an OPEN effect-LOCATION model replacing
operand/result-position-only targeting).

## Proposed commit — CEIR-8b (user commits; NO AI trailer; ADR-0112 ships in the same batch)

```
feat(ceir-8b): open-world attribute model -- aggregates + wrappers (ADR-0112)

- AttrKind::{Array,Dict,TypedConst,Extern} (the ONE-TIME enum edit) + AttrClassId (interned
  "dialect.attr", mirroring TypeClassId/OpId) + Dialect::register_attr_class(AttrClassSpec{
  verify, version}) -> a Context attr-class registry. TWO wrappers (TypedConst = a value OF a
  wrapped_type; Extern = a dialect attribute-class + version) over ONE aggregate vocabulary
  (Array; Dict with SORTED+UNIQUE keys), riding reused AttrValue slots -> 1c interning/equality/
  hash/serialization inherited free. Pays the attr.hpp CEIR-2/3 aggregate-kinds IOU. Every custom
  attribute after Extern lands with ZERO further enum edits (proven: testunit.dimension).
- operator== / attr_is_canonical include payloads/spans/class/wrapped_type (a different class or
  type with identical payload is a DIFFERENT attr); the kind-scoped junk guard + the Dict
  sorted-unique invariant close the prints-identically-but-differs hazard; the wrapper-composition
  rule forbids a wrapper-on-wrapper payload (qty<qty> precedent).
- A child-first ATTR pool (mirroring intern_type_pool) so aggregate/wrapper records never
  reference a forward child index. New fields serialize ONLY on the new kinds -> scalar encodings
  byte-identical -> NO version bump, ZERO recook. Attrs feed stable_hash, not interface_hash.
- Generic canonical text/binary forms ([..], {"k":v}, #typed<>, #extern<>); the text parser gained
  a depth guard + a graceful non-canonical-dict reject (never the intern assert) + the version
  range-check, which was ALSO retrofitted into 8a's parse_extern for text/binary symmetry. The
  verify triple: factory asserts / decoder + parser reject / unregistered preserves (U-56, both
  forms + late-register unify).
- ADR-0112 (open-world attribute model) ACCEPTED. tests: test_extern_attr (13 [extern-attr]) incl.
  the U-56 headline, the verify triple, the wrapper-composition rule, content-vs-interface hash,
  version range both forms, a hostile-dict-text graceful-reject, the depth guard, and a fuzz sweep.

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 255/255 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh against the new AttrValue layout)
+ LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants. No binary
version bump.
```

## CEIR-8c — effect FAMILY widening (u32->u64) + the OPEN effect-LOCATION model (ADR-0113) — 2026-08-09

**The gap.** The CEIR-4a effect system had two closed ceilings: the family bitmask was a `u32` with only 5 spare
bits (27 of 32 families used), so the U-19 domain families (document/CAD/EDA/transaction/UI/agent) had no home; and
effect TARGETING was position-only (`EffectTarget` in {None, Operand, Result}) — an op could say "I write operand 0"
but not "I write this ECS component / document object / state slot / file", the extensible resource identities every
non-buffer domain needs.

**The shape (ADR-0113), advisor-approved on the design fork.** Families stay CLOSED (documented-why per U-5: the
mask is the O(1) union type on the hot `collect_region_effective_mask` path — an open set can't be a fixed-width
bitset; `effect_access` is a total switch with no default, so the append-at-end `-Werror=switch` guard IS the
safety, and it exists only for a closed enum; families are access-pattern AXES, domain identity lives in the
LOCATION). The bitmask widened `u32`->`u64` end-to-end (`effect_family_bit`, `collect_*_mask`, the `EffectsFn` hook,
the sec 107 projection). 8 families appended (ordinals 27..34, crossing bit 31), each classified in `effect_access`
(+4 `ResourceClass`es Document/Constraint/Ui/Agent; Transaction->Universe). LOCATIONS became open-world:
`EffectTarget` extended IN PLACE (append the U-20 built-in kinds + one `Extern` door after `Result`, ordinals 0..2
unchanged so every generated `EffectRecord` array compiles untouched; `using LocationKind = EffectTarget`); a
`LocationClassId` (interned "dialect.location") + `Dialect::register_location_class(LocationClassSpec{verify,
resource_class, version})` -> a Context registry; `EffectRecord` gained a `LocationClassId location_class` (a `u64`
-> the POD stays trivially-copyable; the junk-field guard: valid IFF `target==Extern`, asserted at register_op — the
factory leg). The hazard analysis reads an Extern location's declared `resource_class`; an UNREGISTERED one is
`Universe` (conflicts with everything — EMPTY!=UNKNOWN, extended).

**The recook story — the ONE serialized surface.** Effects are REGISTRATION metadata, re-derived from the registered
dialect on load (`binary.cpp` encodes zero effect bytes). So the open-location model has NO per-instance
serialization surface — the 8a/8b string-keyed/text/decode-version-range-check legs are N/A-by-design (documented;
per-instance location IDENTITY named-forward to 8d, NOT a parallel identity graph). The ONLY effect surface that
serializes is the sec 107 interface-hash projection, which pushes the family mask; widening it (`push_u32`->`push_u64`)
changed EVERY effectful program's interface hash (even effect-free ones — the mask is pushed unconditionally). This
is a NAMED, universal recook (unlike 8a/8b's no-bump): `kCeirCookSchema` bumped 1->2 so stale cooked blobs reject
CLEANLY at the existing `read_cooked_header` SchemaMismatch check. `kBinaryVersion` did NOT bump — the module blob is
byte-unchanged (a bump would wrongly reject valid pre-8c blobs).

**Advisor pre-close caught one real hole + one verify, both resolved:** (1) `effect_legal_in_region` (sec 32 audio-RT
legality, semantics.hpp) is a SECOND family consumer — a DENYLIST predicate, NOT a total switch, so `-Werror=switch`
could not flag the 8 new families as unclassified. Audited by hand: `TransactionBoundary` (a commit can block, the
Synchronization rationale) + `AgentAction` (unbounded external-ish, the ExternalCall rationale) JOIN the audio-RT
forbidden set; the 6 bounded domain read/writes stay legal (exactly as SceneWrite/EcsWrite/HostStateWrite already
are — over-forbidding a bounded read/write adds no safety). A test pins it. The general lesson (saved as a memory):
a closed vocabulary can have consumers a total-switch guard cannot see; widening it means auditing EVERY consumer.
(2) Verified `kCeirCookSchema` is validated on load — `read_cooked_header` rejects a schema mismatch (tested at
`test_cooked_forms.cpp:96`), so the 1->2 bump rejects stale blobs cleanly. No blocker.

**Tests.** `test_effect_open.cpp` (8 `[effect-open]`): the >=bit-32 interface-hash truncation catcher (AgentAction
bit 34 would alias MemoryReadWrite bit 2 under a u32 mask — the strongest test); the u64 mask through
`effective_effects`; a PRIVATE callee's bit-34 family lifting transitively into an exported caller's interface hash
(the `call_effects_fn` u64 path — the func dialect must be registered so `func.call` carries its EffectsFn hook, else
it degrades to a bare ExternalCall barrier identically); Extern location -> registered class / UNREGISTERED ->
Universe; the hazard matrix (same class conflicts, distinct classes don't, unregistered conflicts with everything);
the location verify hook rejecting a registered-invalid record; the audio-RT legality classification (Agent/Txn
forbidden, DocumentWrite legal); `kBinaryVersion==2` + a byte-exact round-trip.

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **263/263 ctest** (255 + 8) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh against the new
`EffectRecord`/family-width layout (the stale-.obj scar) — + LLVM-20 tidy (12 files) + GCC `-Werror=switch`
(`effect_access` carries the 8 new arms; the second consumer `effect_legal_in_region` audited by hand) +
`crd-ceir-opgen-{drift,validator}` (the EFFECT_FAMILIES lockstep 27->35 + its count test updated) +
`crd-ceir-invariants`. **Interface-hash recook (kCeirCookSchema 1->2); NO binary-format bump.** Next = CEIR-8d
(STABLE SEMANTIC IDENTITY — content-independent stable ids for ops/functions/state slots/visual nodes; the ADR-0109
sec 6 IOU, which also gives 8c's built-in location kinds their per-instance identity).

## Proposed commit — CEIR-8c (user commits; NO AI trailer; ADR-0113 ships in the same batch)

```
feat(ceir-8c): effect family widening (u32->u64) + open effect-location model (ADR-0113)

- The effect FAMILY bitmask widened u32->u64 (effect_family_bit, collect_*_mask, the
  EffectsFn hook, the sec107 interface-hash projection) to admit 8 U-19 domain families
  (DocumentRead/Write, ConstraintRead/Write, TransactionBoundary, UIRead/Write, AgentAction
  -- ordinals 27..34, crossing bit 31). Families stay CLOSED (documented-why: the O(1) mask
  union; the effect_access total-switch append guard; families are access axes, domains live
  in the location). Each new family classified in hazard.hpp::effect_access (+4 ResourceClasses)
  AND in semantics.hpp::effect_legal_in_region (the SECOND, non-switch-guarded consumer: Agent
  + Txn forbidden in audio-RT, domain read/writes legal).
- Effect LOCATIONS became open-world: EffectTarget extended in place (BufferRange..Net + one
  Extern door after Result; using LocationKind = EffectTarget) + a LocationClassId registry
  (Dialect::register_location_class(LocationClassSpec{verify, resource_class, version})).
  EffectRecord gained a u64 location_class (POD-preserved; valid iff Extern -- the register_op
  junk-field guard, the factory leg of the verify triple). The hazard analysis reads an Extern
  location's declared resource_class; an UNREGISTERED one is ResourceClass::Universe (conflicts
  with everything -- EMPTY!=UNKNOWN). A domain adds a location by hand-registering a class:
  ZERO central-enum edits (proven: testfs.file_handle).
- Effects are registration metadata (not serialized in the module blob), so the location model
  has NO per-instance serialization surface (N/A-by-design; per-instance identity -> 8d). The
  ONE serialized surface -- the sec107 interface-hash projection -- pushes the u64 mask, changing
  every effectful program's interface hash: a NAMED universal recook. kCeirCookSchema 1->2 so
  stale cooked blobs reject cleanly (read_cooked_header SchemaMismatch). NO kBinaryVersion bump
  (the module blob is byte-unchanged). opgen EFFECT_FAMILIES 27->35 (lockstep + its count test).
- ADR-0113 ACCEPTED. tests: test_effect_open (8 [effect-open]) incl. the >=bit-32 interface-hash
  truncation catcher, the transitive callee-lift, the Extern-location hazard matrix + Universe,
  the audio-RT legality classification, and kBinaryVersion==2 + byte-exact round-trip.

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 263/263 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh against the new EffectRecord
layout) + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
Interface-hash recook (kCeirCookSchema 1->2); no binary-format bump.
```

## CEIR-8d — content-independent STABLE SEMANTIC IDENTITY (ADR-0114) — 2026-08-09

**The gap.** ADR-0109 sec6 fixed the semantic-identity model "before the editor": stable semantic ids independent of
textual position, so a semantic diff is position-independent. Only SourceLoc landed at 1c. Ops/functions were
pointer + pre-order identified (position IS identity), and the sec20 state schema in the interface hash walked
StateEdge cells in BODY ORDER — so a reorder changed the hash (a SAFE false-incompatible, but a false one). Every
consumer that must survive an edit (state migration, transactions, semantic diff, an editor's node identity, 8c's
per-instance location identity) had no identity to hold.

**The shape (ADR-0114), advisor-approved on the design fork.** A `StableId` (u64, 0=invalid) rides `Operation`. ONE
id space: a function IS a func.func op, a state slot IS a StateEdge op, a visual node's identity IS its op's stable
id. `Context::assign_stable_ids` is module-scoped, pre-order, one-time, idempotent (max+1 for id-0 ops; assigned ops
never re-derived). ADVISOR CORRECTION on the fork: NOT a Context-global create-time counter — that would break the
"blob is a pure function of module content, not Context history" gate (a counter makes ids depend on Context
history); module-scoped pre-order keeps ids a pure function of content (identical content -> identical ids; two
machines migrating the same blob agree). The id field is `mutable` and assignment is memoization (logical-const), so
the const serialize/interface_hash can give a never-persisted op a deterministic id, and both agree (ONE routine).

**Serialization + the content-hash tension.** An additive, forward-skippable `STID` chunk (count + watermark +
per-op ids, body pre-order) -> pre-8d decoders skip it, a pre-8d blob decodes fine (ids re-seed pre-order) -> NO
kBinaryVersion bump. Decode is build-raw-graceful-reject (0/duplicate/count-mismatch/id>watermark reject, never
assert; fuzz-swept ASan). The central tension: stable ids are content-INDEPENDENT identity, but stable_hash (the
cook-cache key) must stay id-INDEPENDENT (else re-authoring identical content cache-misses). Resolution: stable_hash
builds the blob WITHOUT the STID chunk -> byte-identical to pre-8d -> ZERO content-hash churn, cache hits survive.
The principle: content projections are id-free (stable_hash AND the text form); identity/persistence surfaces (binary
STID + the in-memory Module) carry ids. Text stays the id-free content projection (documented-why: a per-op id token
would churn every print golden for no persistence gain; a test pins that a fresh module's text round-trip reproduces
its ids). The sec20 state schema is re-keyed by stable id (sorted, id VALUE in the hash) -> a StateEdge-cell reorder
is now invariant (the false-incompatible fixed), while delete-id-1 + add-id-2 is correctly incompatible (an
order-only hash would silently lose state at 10a migration). kCeirCookSchema 2->3, a named interface-hash recook.

**Advisor pre-close caught a BLOCKER + a coverage gap, both fixed:** (1) BLOCKER id-reuse after erase — a
max-of-LIVE-ids scan misses a tombstoned (erased) op, so a later append would draw the erased op's id and the
delete/re-add discriminator would silently pass (the exact state-corruption identity exists to prevent). Fix: a
monotone per-module WATERMARK (max id EVER assigned, serialized + restored); assignment draws from
max(scan_max, watermark)+1; the decoder rejects id>watermark. A freed id is never reused (in-memory OR across load).
Two tests pin it. (2) The pre-8d migration path (decode_stid's !m_has_stid early return) had no test that actually
DECODED a no-STID blob; added one that forges a pre-8d blob (strip the trailing STID chunk + patch chunk_count 6->5),
asserts it decodes with ids 0 and re-serializes BYTE-EXACT to the original (the fixpoint, stronger than hash-equal).

**Runtime scope.** The interpreter's m_cells stays Operation*-keyed — it is transient per-run state where pointers
are valid; cross-version cell MATCHING by stable id is migration (CEIR-10a), not 8d.

**Tests.** `test_stable_id.cpp` (12 `[stable-id]`): pre-order 1..N + idempotent; ids ride the op through serialize;
hostile-STID (0/duplicate) graceful reject; content-hash id-independence; blob purity; the reorder-invariant +
delete/re-add-sensitive state schema; erase-never-reuses-an-id (in-memory + across a serialize/load round-trip); a
genuine pre-8d blob decode -> re-serialize fixpoint; kBinaryVersion==2; a fresh-module text round-trip reproduces
ids; a single-byte-corruption fuzz sweep over a stable-id blob.

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **275/275 ctest** (263 + 12) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh against the new
`Operation`/`Module` layout (the stale-.obj scar) — + LLVM-20 tidy (8 files) + GCC + `crd-ceir-opgen-{drift,
validator}` + `crd-ceir-invariants`. **Interface-hash recook (kCeirCookSchema 2->3); ZERO content-hash churn; NO
binary-format bump.** Next = CEIR-8e (trait/interface split + region-kind reservation — the closed core-trait
vocabulary documented-why-closed; OpInterface promoted to the typed open extension surface; RegionKind reservation
policy for Reactive/StateMachine/Timeline/Constraint/Device regions).

## Proposed commit — CEIR-8d (user commits; NO AI trailer; ADR-0114 ships in the same batch)

```
feat(ceir-8d): content-independent stable semantic identity (ADR-0114)

- A StableId (u64, 0=invalid) rides Operation. ONE id space: a function is a func.func op,
  a state slot is a StateEdge op, a visual node's identity is its op's stable id. Pays the
  ADR-0109 sec6 IOU (only SourceLoc had landed).
- Context::assign_stable_ids is module-scoped, pre-order, one-time, idempotent (max+1 for id-0
  ops). NOT a Context counter (that breaks the blob-is-a-pure-function-of-content gate); ids are
  a pure function of content. A monotone per-module WATERMARK (serialized) means an erase()d op's
  id is NEVER reused (in-memory or across load) -- else the delete/re-add discriminator would
  silently pass. mutable field + logical-const memoization so const serialize/interface_hash agree.
- Serialized as an additive, forward-skippable STID chunk (count + watermark + per-op ids, body
  pre-order): pre-8d decoders skip it, a pre-8d blob decodes fine -> NO kBinaryVersion bump.
  Decode is build-raw-graceful-reject (0/duplicate/count-mismatch/id>watermark).
- stable_hash builds the blob WITHOUT STID -> the content hash is id-INDEPENDENT -> ZERO
  content-hash churn (cook-cache hits survive). Content projections are id-free (stable_hash AND
  text); identity surfaces (binary STID + in-memory) carry ids. Text stays the id-free content
  projection (documented-why + a fresh-module text-roundtrip-reproduces-ids test).
- The sec20 state schema in the interface hash is re-keyed by stable id (sorted, id value hashed):
  a StateEdge-cell reorder is now invariant (the 7a/8c false-incompatible fixed) while delete-id-1
  + add-id-2 is correctly incompatible. kCeirCookSchema 2->3 (a named interface-hash recook).
- ADR-0114 ACCEPTED. tests: test_stable_id (12 [stable-id]) incl. the erase-never-reuses-an-id
  watermark (in-memory + round-trip), content-hash id-independence, the reorder-invariant state
  schema, a genuine pre-8d blob decode->re-serialize fixpoint, and a fuzz sweep.

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 275/275 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh against the new Operation/Module
layout) + LLVM-20 tidy + GCC + opgen drift/validator + crd-ceir-invariants. Interface-hash recook
(kCeirCookSchema 2->3); zero content-hash churn; no binary-format bump.
```

## CEIR-8e — trait/interface split + region-kind reservation (ADR-0115) — 2026-08-09

**The gap.** Three vocabularies decide how the compiler dispatches without a central switch(op.kind): OpTrait (a u32
flags set, 8 of 32 bits), OpInterface (the CEIR-1d registry: intern by name, register/get an opaque void* impl), and
RegionKind (a serialized u8 enum {Graph, SsaCfg}). The split between them had no policy: nothing stopped a dialect
from minting a trait bit that silently collides with a future core trait (register_op bounds effects but NOT traits);
the interface registry was untyped (every caller hand-interns the id + hand-casts the void*) and intern_interface was
a per-call linear string scan — not acceptable for THE extension surface; and domains needing Reactive/StateMachine/
Timeline/Constraint/Device regions had no reservation.

**The shape (ADR-0115), advisor-approved on the design fork.** OpTrait stays CLOSED (documented-why: the fixed
reasoning AXES the core's verifiers switch on — 5b Terminator, 5d StateEdge, 6a Token*; like effect FAMILIES at 8c)
+ a FACTORY GUARD: register_op asserts (spec.traits & ~kKnownTraitsMask) == 0 (the kLastEffectFamily parallel), and
the dialect.hpp "a dialect ORs its own bits" comment is struck in place to say dialects SET core bits, never MINT.
OP_TRAITS is count-pinned in the opgen validator. The OpInterface registry is PROMOTED to the TYPED open-world
surface (a new interface.hpp): plugin BEHAVIOR lives in a typed function-table interface an op-kind implements,
dispatched with zero switch(op.kind) and zero new trait bits. Two upgrades: (1) InterfaceId became a u64 FNV of the
interface name (like TypeClassId/AttrClassId/LocationClassId), so T::kId is a COMPILE-TIME constant (a constexpr
interface_hash_ct byte-identical to containers::fnv1a_64) and a query mints the id with zero registry work — the
per-query linear string scan is gone; intern_interface keeps the runtime path + a reverse-lookup name table for
diagnostics. (2) Type-safe accessors register_op_interface<T> / get_op_interface<T> do the id + cast internally (a
caller can't pair the wrong id with the wrong type).

**The catalog — ONE live proof + reserved names mapped to homes.** Only Cost (crd.iface.cost) ships a live typed
interface — it is the one family with NO existing home. The other seven reserve their canonical names MAPPED to where
that behavior already lives: MemoryEffect -> EffectRecord/EffectsFn (4a/8c), Shape -> opgen type/shape_inference,
Lowering -> IExecutionProvider (§69), Timeline -> region_exec attr (4c) + 8f, Incremental -> 8h, Constraint -> 8c
Constraint family, Serialization -> 8a/8b/8c class serialize + 8d STID. So a future slice binds an impl to the SAME
name against its existing home rather than forking a fresh vtable (the third-dependency-graph anti-pattern).

**RegionKind reservation.** RegionKind stays {Graph, SsaCfg} — the structural execution-order tag. The reserved
region semantics (Reactive/StateMachine/Timeline/Constraint/Device) are NOT new enum values; they follow the CEIR-4c
precedent where region semantics already ride an attr on the owner op (region_exec = packed EvalDomain/RealtimeClass)
plus an interface. The policy names the one future path that WOULD touch the enum: a structural need the graph/CFG tag
can't express appends per the 8a pattern (append-at-end, decoder bound-raise, no version bump). No enum widening now,
no serialized-format change.

**The recook story — ZERO motion (the band's first).** Traits + interfaces are registration metadata (re-derived on
load, like effects); RegionKind is unchanged. So no kBinaryVersion bump, no kCeirCookSchema bump, no interface-hash
change, no content-hash change, no recook of any kind. (The full 4-config gate still ran: InterfaceId u32->u64 grew
the OpInterface/OpInfo layout, so all 3 targets rebuild against the new headers — the stale-.obj scar.)

**Advisor pre-close caught a footgun, fixed:** register_interface SILENTLY DROPPED when the op-kind was not registered
("if (slot == nullptr) return;") — tolerable for the raw 1d registry, but on THE typed extension surface it is a
plugin footgun of the silent-drop family (a mysterious nullptr from get_op_interface far away). True late-bind can't
work (the impl lives ON OpInfo — nowhere to park it before the op registers). So "op first" is now a LOUD contract:
CRD_ASSERT_MSG in the base register_interface (the manual path shares the guard, the factory-leg parallel to the trait
mask) + release-safe no-op. A test pins the overwrite branch (a second impl for the same interface wins, in place).
Per the advisor, no new memory scar this slice — the trait-guard lesson is a corollary of the existing
widen-enum-audit-every-consumer entry, carried by the ADR.

**Tests.** test_interface_typed.cpp (4 [interface]): an analysis dispatches through a typed interface knowing nothing
about the op-kind (zero central edits) + overwrite + missing/unregistered-kind -> nullptr; the compile-time T::kId
equals the runtime intern of its name (the FNV pin, which also guards against a hash_string swap) + distinct names ->
distinct ids + reverse lookup; the 7 reserved catalog names are all distinct; kKnownTraitsMask is exactly the 8
contiguous bits (no gap, no stray).

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **279/279 ctest** (275 + 4) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh against the new
InterfaceId/OpInterface layout (the stale-.obj scar) — + LLVM-20 tidy (7 files incl. the NEW interface.hpp) + GCC
(the first non-MSVC compile of the constexpr FNV path) + crd-ceir-opgen-{drift,validator} (the OP_TRAITS count-pin) +
crd-ceir-invariants. **ZERO recook (the band's first zero-motion slice).** Next = CEIR-8f (capability contracts +
domain/safety split + typed time domains — the U-§57 capability model, which OWNS the §107 capability-contract field
flagged ownerless since 7a; EvalDomain separated from may-allocate/may-block/may-IO/realtime-safe; typed time domains
wall/sim/frame/audio-sample/sequencer/logical).

## Proposed commit — CEIR-8e (user commits; NO AI trailer; ADR-0115 ships in the same batch)

```
feat(ceir-8e): trait/interface split + region-kind reservation (ADR-0115)

- OpTrait stays a CLOSED core vocabulary (documented-why: the fixed reasoning axes the core's
  verifiers switch on) + a FACTORY GUARD: register_op asserts (spec.traits & ~kKnownTraitsMask)
  == 0, so a dialect SETS core-minted bits and never MINTS one (the kLastEffectFamily parallel;
  the dialect.hpp comment struck in place). OP_TRAITS is count-pinned in the opgen validator.
- The OpInterface registry is PROMOTED to the TYPED open-world surface (new interface.hpp):
  plugin BEHAVIOR lives in a typed function-table interface an op-kind implements, dispatched
  with zero switch(op.kind) and zero new trait bits. InterfaceId -> u64 FNV of the name (like
  the class ids), so T::kId is a compile-time constant (a constexpr interface_hash_ct
  byte-identical to fnv1a_64) and the per-query string scan is gone. register_op_interface<T>/
  get_op_interface<T> are id+cast-safe; register_interface asserts "op first" (loud, not a
  silent drop). Catalog: ONE live proof (crd.iface.cost) + 7 reserved names MAPPED to their
  existing homes (a future slice extends, never forks).
- RegionKind stays {Graph, SsaCfg} (structural); reserved region semantics (Reactive/
  StateMachine/Timeline/Constraint/Device) are interface/attr-gated via a documented policy
  (the CEIR-4c region_exec precedent) -- no enum widening, no serialized-format change.
- ZERO recook: traits/interfaces are registration metadata, RegionKind unchanged -> no
  kBinaryVersion/kCeirCookSchema bump, no interface/content-hash move (the band's first
  zero-motion slice). InterfaceId/OpInterface/OpInfo layout grew (a rebuild).
- ADR-0115 ACCEPTED. tests: test_interface_typed (4 [interface]) incl. typed dispatch knowing
  nothing about the op-kind, the compile-time-kId == runtime-intern FNV pin, the reserved
  catalog distinctness, and the exactly-8-contiguous-bits trait mask.

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 279/279 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh against the new InterfaceId/
OpInterface layout) + LLVM-20 tidy (incl. the new interface.hpp) + GCC (the constexpr FNV path)
+ opgen drift/validator (the OP_TRAITS count-pin) + crd-ceir-invariants. Zero recook.
```

## CEIR-8f — capabilities + domain/safety split + typed time domains (ADR-0116) — 2026-08-09

**The gap (three U-§ items).** (a) The TOML `[op.native] capabilities` field was PARSED by opgen but wired to NOTHING
(program_asset.hpp named it forward: "capabilities have no owning row yet") — programs had no way to REQUEST a
permission, hosts none to GRANT. (b) EvalDomain (WHERE an op runs) was entangled with realtime-ness; the orthogonal
SAFETY axes (may-allocate/may-block/may-IO/realtime-safe, U-§23) had no representation. (c) Time values (a frame dt, a
sim step, an audio sample count) were just numbers — mixing wall-clock with sim-step was a silent bug (U-§17).

**The shape (ADR-0116), advisor-approved on the design fork.**

(1) CAPABILITIES — a `CapabilityId` = FNV of a name (`gpu.compute`/`file.write`/`external.process`/…; open-world +
INTERN-ONLY — a capability is a name, no verify/version, the intern_interface shape). `OpSpec.capabilities` (opgen
emits from the TOML) -> register_op interns them (rejects an EMPTY name — the declared-words-validated guard) into
`OpInfo.required_capabilities` (arena-copied, the effects precedent). The PROGRAM's required set = the MODULE-WIDE
(pre-order — funcs ARE ops in the body, so no "transitive" machinery, the StateW::go shape) sorted-UNIQUE union; an
UNREGISTERED op contributes `external.process` (EMPTY!=UNKNOWN — an unknown op must not read as requiring-nothing and
smuggle past a future sandbox). It JOINS the §107 interface hash (a "caps:" section, the count pushed UNCONDITIONALLY
even at 0 — the 8c no-conditional-width lesson; then the sorted-unique id VALUES) -> a NAMED interface-hash recook,
kCeirCookSchema 3->4. stable_hash + kBinaryVersion UNTOUCHED (capabilities are registration metadata — a cap-bearing
module round-trips byte-exact). program_capabilities / capabilities_satisfied are the host-grant queries; sandbox
ENFORCEMENT + a cooked-META surfacing of the set (so a host pre-flights a grant off-disk) are named-forward.

(2) DOMAIN/SAFETY SPLIT — EvalDomain stays WHERE. The safety axes are a PROJECTION of the effect families (no new
declared/hashed data — extend-not-fork): `effect_safety` a per-family TOTAL SWITCH (the effect_access shape,
-Werror=switch-guarded — a mask/denylist predicate would be a THIRD family consumer invisible to that guard, the 8c
hole), folded by `op_safety` over an op's effects (unregistered = maximally unsafe). `realtime_safe :=
!(may_allocate || may_block || may_io)`. THE two-oracles reconciliation: effect_legal_in_region (the HARD gate)
PERMITS Allocate in an audio-RT region (a soft cost, not a priority-inversion deadlock) while realtime_safe excludes
it -> realtime_safe => legal, never the converse — a documented SUBSET over ONE effect vocabulary, not two drifting
oracles, pinned side by side.

(3) TIME DOMAINS — a time domain IS an 8a TYPE-CLASS (advisor simplification: NO TimeDomainId — that would fork
TypeClassId). A hand-registered `time` dialect (the func precedent) registers the six built-ins (wall/sim/frame/
audio_sample/sequencer/logical) with a verify hook (exactly one Int/Float/Quantity/TypeParam member — the units-Time
seed + generics). `time.wall<T> != time.sim<T>` as distinct TypeIds (the ADR-0111 landmine) — the type distinction a
future operand-type checker rejects mixing on. ENFORCEMENT is named-forward honestly: NO operand-type-mix verifier
exists today (semantic type-checking lands at CEIR-3/4; the search found no same-type check), so 8f establishes the
DISTINCT TYPES and does not claim to enforce mixing now. A plugin `game.turn` domain works with ZERO central edits;
U-§56 round-trip inherited from 8a.

**Advisor pre-close caught four small findings, all fixed:** (1) an ADR/code mismatch — the ADR said the time hook
requires a numeric/quantity member but the code checked arity only; STRENGTHENED the hook to Int/Float/Quantity/
TypeParam (the TypeParam arm keeps generic time types verifying — the 8a substitute-through precedent) and aligned the
ADR. (2) added a test that a capability required by an op INSIDE a func body reaches the program set (the
module-wide-covers-func-bodies claim the whole simplification rests on). (3) an EMPTY capability name hashes to the FNV
offset basis (a phantom id) — added the register_op assert + the opgen validator arm. (4) two ADR touches: named-
forward the cooked-META surfacing of the required set, and replaced an untestable "cap-free stable_hash unchanged"
claim with the implemented cap-bearing serialize->deserialize byte-exact + same stable_hash test. Per the advisor, no
new memory scar — count-unconditional / EMPTY!=UNKNOWN / total-switch-not-predicate are applications of existing
entries; the two-oracles-subset is carried by the ADR.

**Tests.** test_capability.cpp (7 [capability]): required-set round-trip + external.process EMPTY!=UNKNOWN + func-body
reach + module-wide sorted-unique union + host-grant check + the interface-hash reorder-invariant/membership-sensitive/
unregistered-moves-it + cap-bearing byte-exact round-trip. test_safety.cpp (2 [safety]): effect_safety + op_safety fold
+ the two-oracles subset. test_time_domain.cpp (3 [time]): wall!=sim + plugin domain + verify-rejects-0/2-members +
U-§56 round-trip.

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **291/291 ctest** (279 + 12) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh against the new OpInfo/OpSpec
layout (the stale-.obj scar) — + LLVM-20 tidy (14 files incl. the NEW time.hpp/time.cpp) + GCC -Werror=switch (the
first non-MSVC exhaustiveness check of the 35-arm effect_safety) + crd-ceir-opgen-{drift,validator} (the
capabilities->OpSpec emission + the empty-name arm; regen clean) + crd-ceir-invariants. Interface-hash recook
(kCeirCookSchema 3->4); NO binary-format bump; ZERO content-hash churn. Next = CEIR-8g (the compiler-infrastructure
skeleton — AnalysisManager/PassManager/RewritePattern/ConversionTarget/DiagnosticEngine, FRAMEWORKS ONLY, no
optimization passes — the no-consumer rule).

## Proposed commit — CEIR-8f (user commits; NO AI trailer; ADR-0116 ships in the same batch)

```
feat(ceir-8f): capabilities + domain/safety split + typed time domains (ADR-0116)

- U-57 CAPABILITY model (owns the sec107 field ownerless since 7a): a CapabilityId (FNV of a
  name, intern-only). OpSpec.capabilities (opgen-emitted from [op.native]) -> register_op interns
  them (rejects an empty name) into OpInfo.required_capabilities. The program's required set =
  the MODULE-WIDE sorted-unique union (an UNREGISTERED op contributes external.process,
  EMPTY!=UNKNOWN) and JOINS the sec107 interface hash (a "caps:" section, count unconditional) ->
  kCeirCookSchema 3->4. stable_hash + kBinaryVersion UNTOUCHED (registration metadata). Host-grant
  queries program_capabilities / capabilities_satisfied; sandbox enforcement + cooked-META
  surfacing named-forward.
- U-23 domain/safety split: EvalDomain stays WHERE; the safety axes (may-allocate/block/IO +
  derived realtime_safe) are a PROJECTION of the effect families -- effect_safety a per-family
  TOTAL SWITCH (the effect_access shape, -Werror=switch-guarded, not a predicate that dodges the
  guard) folded by op_safety. The two RT oracles (effect_legal_in_region vs realtime_safe) are a
  documented SUBSET (realtime_safe => legal), not two drifting oracles.
- U-17 typed time domains: a time domain IS an 8a TYPE-CLASS (no TimeDomainId -- that would fork
  TypeClassId). A hand-registered `time` dialect registers six built-ins (verify: one Int/Float/
  Quantity/TypeParam member). time.wall<T> != time.sim<T> as distinct TypeIds -- the type
  distinction a future operand-type checker rejects mixing on (enforcement named-forward; no such
  checker exists yet). A plugin game.turn works with zero central edits.
- ADR-0116 ACCEPTED. tests: test_capability (7), test_safety (2), test_time_domain (3).

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 291/291 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh against the new OpInfo/OpSpec
layout) + LLVM-20 tidy (incl. new time.hpp/cpp) + GCC -Werror=switch (35-arm effect_safety) +
opgen drift/validator + crd-ceir-invariants. Interface-hash recook (kCeirCookSchema 3->4); no
binary-format bump; zero content-hash churn.
```

## CEIR-8g — the compiler-infrastructure skeleton (ADR-0117) — 2026-08-09

**The gap.** CEIR had op/type/attr/effect/interface machinery but no compiler DRIVER layer: no way to register an
analysis and reuse its result, no pass sequencing, no rewrite/legalization framework, and only
ParseResult{ok,offset,msg} for errors (no codes/severity/notes/fix-its). U-§72 also warns Context must not become a
god-object.

**The shape (ADR-0117), advisor-approved on the design fork.** Four THIN FRAMEWORKS, ⛔ FRAMEWORKS ONLY (no
optimization passes / no real lowering — those are CEIR-26/27; the no-consumer rule), each a SEPARATE class over a
Context/Module (ZERO new Context members — the U-§72 bound; the all-targets rebuild happens via the ceir.hpp umbrella
edit anyway).

- ⭐ ONE shared `fnv1a_ct` (id.hpp) — `make_interface_id`/`make_analysis_id`/`make_diagnostic_code` ALL call it (a
  copy-per-id would silently drift from hash_string, and drift means T::kId != intern(name); the 8e interface_hash_ct
  was hoisted here). AnalysisId/DiagnosticCode are open-world FNVs; kId is compile-time.
- AnalysisManager (U-§73): a registered analysis (kId + a compute fn returning an arena const T*); get<T> computes-or-
  caches. Never-stale INVALIDATION: a pass declares a PRESERVED set, the manager EVICTS the rest (+ invalidate_all);
  a pass reporting UNCHANGED preserves ALL regardless of its set. ⛔ THE slice's biggest trap, refused: NO
  inter-analysis dependency tracking — that would be a new dependency graph ONE SLICE before 8h exists to unify the
  >=2 the tree holds (the band's own anti-pattern); inter-analysis deps are a documented pass-author contract,
  named-forward to 8h. Eviction leaks into the arena (accepted, grow-by-rebuild).
- PassManager (U-§65): a Pass = name + run fn (bool(Context&, Module&, DiagnosticEngine&)) + preserved set; run
  sequences + drives invalidation + ⛔ STOPS on a Fatal (has_fatal()).
- Rewrite/Conversion (U-§67/§74): RewritePattern{match, rewrite} + ⛔ caller-driven per-op try_apply (NOT a block walk
  — the iterator-invalidation/worklist driver IS CEIR-26, reserved); ConversionTarget per-kind legality (Legal/Illegal/
  Dynamic) with ⛔ an UNLISTED kind => Illegal (EMPTY!=UNKNOWN).
- DiagnosticEngine (U-§102): one structured surface — DiagnosticCode + Severity (closed axis) + SourceLoc + message +
  notes + fix-its; render prints code-name + file:line:col + message. ⛔ ALL text COPIED into the engine arena
  (alloc-outlives-borrowers — a diagnostic outlives the dying buffer it came from). The reverse-lookup name rides EACH
  diagnostic (a copied code_name), NOT a separate intern registry no consumer queries.

**The recook story — ZERO motion.** Every framework is runtime state; nothing serializes: no kBinaryVersion, no
kCeirCookSchema, no interface/content-hash change, no recook.

**Advisor pre-close caught a blocker + a test gap, both fixed:** (1) BLOCKER — Severity::Fatal short-circuited
NOTHING (the ADR claimed it, but has_errors treated Fatal like Error and the PassManager loop had no stop condition,
so a Fatal in pass 1 didn't stop pass 2 — the exact overrun Fatal exists to prevent; source-must-match-scoreboard).
WIRED it: has_fatal() on the engine, threaded through the pass run signature + PassManager::run, which now breaks on
has_fatal(); a test with two passes proves pass 2 never runs after a Fatal. (2) the never-stale counter tests only
moved a compute-counter (a broken invalidation serving STALE results would still pass) — added a test that appends a
real op, invalidates, and checks the recomputed result reflects the new IR (op_count 1 -> 2). Per the advisor, no new
memory scar — Fatal-unwired is source-must-match-scoreboard, the counter gap is measure-the-property-not-the-proxy;
both existing entries.

**Tests.** test_pass_manager.cpp (6 [analysis][pass]): compute-once/cache (never-redundant); never-stale invalidation
+ preserved-analysis-not-recomputed; FRESH-data-after-invalidation; PassManager-drives-invalidation + unchanged-
preserves-all; Fatal-short-circuits-the-pipeline; the shared-FNV pin (analysis id == interface id of the same name).
test_rewrite.cpp (2 [rewrite]): try_apply match/rewrite/decline + match-only + null; ConversionTarget legal/illegal/
dynamic + unlisted=>Illegal + last-set-wins. test_diagnostic.cpp (4 [diagnostic]): emit/collect/has_errors + render
(code+loc+message, <unknown> for file 0) + Fatal + ⛔ the text-COPY ASan probe (emit from a scope-local buffer, read
after it dies) + the shared-FNV pin. ⛔ the text-copy probe is only meaningful under ASan — verified on
linux-gcc-asan (the money config), not just win-debug.

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **303/303 ctest** (291 + 12) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** — all 3 targets rebuilt fresh (the ceir.hpp umbrella edit)
— + LLVM-20 tidy (9 files incl. the 3 NEW headers pass_manager.hpp/rewrite.hpp/diagnostic.hpp) + GCC + opgen drift/
validator + crd-ceir-invariants. **ZERO recook (a zero-motion slice).** Next = CEIR-8h (the INCREMENTAL-EVALUATION
architecture — ENGINE-FIRST UNIFICATION: absorb the >=2 existing dependency graphs into ONE dirty-propagation/
memoization model keyed by the 7a content/interface hashes; a third graph is the failure mode; the AnalysisManager
built here is the seam it grows from).

## Proposed commit — CEIR-8g (user commits; NO AI trailer; ADR-0117 ships in the same batch)

```
feat(ceir-8g): compiler-infrastructure skeleton (ADR-0117)

- Four THIN frameworks, FRAMEWORKS ONLY (no optimization passes/lowering -- CEIR-26/27), each a
  SEPARATE class over a Context/Module (zero new Context members -- the U-72 god-object bound).
- ONE shared fnv1a_ct (id.hpp) behind make_interface_id/make_analysis_id/make_diagnostic_code
  (a copy-per-id would drift from hash_string); AnalysisId/DiagnosticCode are open-world FNVs.
- AnalysisManager (U-73): cached analyses + never-stale invalidation via preserved-sets (an
  unchanged pass preserves all). NO inter-analysis dependency tracking -- that would be a new
  dependency graph one slice before 8h unifies the >=2 the tree holds; named-forward to 8h.
- PassManager (U-65): a Pass = name + run fn + preserved set; run sequences + drives invalidation
  + STOPS on a Fatal (has_fatal()).
- Rewrite/Conversion (U-67/74): RewritePattern{match,rewrite} + caller-driven per-op try_apply
  (the walking driver is CEIR-26, reserved); ConversionTarget legality with an UNLISTED kind =>
  Illegal (EMPTY!=UNKNOWN).
- DiagnosticEngine (U-102): one structured surface (code/severity/SourceLoc/notes/fix-its); ALL
  text COPIED into the engine arena (alloc-outlives-borrowers, ASan-proven on the money config).
- ZERO-MOTION: nothing serializes (no kBinaryVersion/kCeirCookSchema/hash change, no recook).
- ADR-0117 ACCEPTED. tests: test_pass_manager (6), test_rewrite (2), test_diagnostic (4).

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 303/303 ctest on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (incl. 3 new headers) + GCC + opgen
drift/validator + crd-ceir-invariants. Zero recook.
```

## CEIR-8h — incremental-evaluation ENGINE-FIRST unification (ADR-0118) — 2026-08-09

**The gap.** Three pieces of an incremental model existed, scattered + unlinked: render-asset-core::DependencyGraph
(a generic AssetId DAG — deterministic topo + affected_by dirty/rebuild set + validate, NO memoization); the cook's
content_hash (cache key) + interface_hash (§107: interface change invalidates dependents, content-only hot-swaps);
the 8g AnalysisManager (per-Module memo + preserved-set invalidation). The ENGINE tying content-addressed
memoization to dependency-driven dirty propagation was missing — and U-§125 (never a third graph) makes building a
NEW one the failure mode.

**The design fork (advisor-decided): (B) in crd-containers, absorb REAL this slice.** ⛔ The LAYERING WALL is the
discriminator: crd-ceir is asset-free (I4/I5) and render-asset-core does NOT link crd-ceir — they are independent
SIBLING modules. So a crd-ceir engine is a graph render-asset-core can NEVER adopt without inverting the module DAG —
a permanent third graph BY CONSTRUCTION (not "a third graph until adoption"). The ONLY home both link with zero new
edges is the LOW shared module: a dependency DAG is a generic data structure -> crd-containers.

**The shape (ADR-0118).** `crd::containers::IncrementalDag`, a DAG keyed by NodeId=u64: a DETERMINISTIC topo
(deps-first, min-id tie-break — the RAF-11 reproducibility contract) + affected_by (transitive dependents),
ABSORBED byte-identical from DependencyGraph; PLUS per-node TWO revision hashes (content/interface) driving the §107
rule GENERALIZED — recompute_after_change recomputes the node on a content-OR-interface change and its transitive
dependents ONLY on an interface change (content-only hot-swaps). It holds STRUCTURE + REVISIONS + dirty propagation,
NOT the consumer's cached RESULTS (keyed by the content hash). ⭐ THE ABSORB IS REAL THIS SLICE:
render-asset-core::DependencyGraph keeps its 4-method API BYTE-STABLE and swaps its internals to an owned
IncrementalDag (AssetId <-> NodeId lossless + order-preserving — AssetId is a u64 value, valid()==value!=0,
value-ordered), so the emitted order is IDENTICAL; validate_against (needs AssetRegistry, a render-asset type that
cannot move down) stays wrapper-side; the existing RAF/render-asset/scene-render/ceir-cook suites are the regression
net (a canonical model with zero real consumers is how third graphs are born — the 7a capability field sat ownerless
six slices). Named-forward WITH reasons: the AnalysisManager (a DIFFERENT paradigm — preserved-sets vs
dependency-propagation; forcing it invents the inter-analysis edges 8g refused — CEIR-26+), the CookDb (already
PRODUCES the content/interface pair the engine consumes — semantic unification, plan-cache at 10b). hesap-sched's
DependencyGraph is a scheduling precedence graph (no revision model) — out of scope.

**The recook story — ZERO motion.** The engine is runtime state; DependencyGraph's serialized §106 CDEP records are
unchanged in SHAPE (the wrapper API is byte-stable). No kBinaryVersion, no kCeirCookSchema, no hash change, no recook.

**Advisor pre-close caught FIVE findings, all fixed:** (1) the `interface` member/param renamed `interface_rev` — the
Windows COM `#define interface struct` (combaseapi.h) landmine in a FOUNDATIONAL containers header a COM-pulling
module may include (green only because no current TU includes windows.h before it). (2) BLOCKER on "gate green": the
blast radius was enumerated BY SYMBOL (grep DependencyGraph|affected_by) -> render-asset + scene-render/reload
(RAF-11) + ceir-cook (which STATIC-LINKS render-asset-core, so the earlier ceir 303 ran STALE objects); ceir-cook-tests
was REBUILT + re-run (relinked), scene-render run. (3) parity: the existing render-asset dependency tests already
assert exact order + tie-break + cycle (the byte-identical pin); added a reverse-insertion-order engine test (the
"regardless of insertion order" contract). (4) recompute_after_change with interface-changed-but-content-same
returned dependents WITHOUT the changed node (incoherent under content-addressing); fixed to recompute self on EITHER
change (over-recompute, never stale) + a test. (5) the hesap-sched out-of-scope note.

**Tests.** test_incremental_dag.cpp (6 [containers][incremental]): topo determinism + min-id tie-break; affected_by
transitive dependents in topo order; insertion-order independence (reverse-insertion parity); cycle -> false; ⛔ the
§107 rule (interface change propagates to dependents, content-only does not, interface-only still recomputes self).
The render-asset-core suite (exact order + tie-break + cycle, unchanged) is the byte-identical regression pin.

**Gate.** crd-containers-tests + crd-render-asset-core-tests + crd-scene-render-tests + crd-ceir-cook-tests (RELINKED)
+ crd-ceir on **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** (the money config verifies the wrapper +
engine memory-safe) + LLVM-20 tidy (4 files incl. the NEW incremental_dag.hpp) + GCC + crd-ceir-opgen-{drift,
validator}. **ZERO recook; the >=2 graphs are now ONE.** Next = CEIR-8i (TRANSACTIONS — atomic multi-op edits with
commit/rollback, keyed by the 8d stable ids + 8h dirty propagation; the last framework before 8z the U-§121 gate).

## Proposed commit — CEIR-8h (user commits; NO AI trailer; ADR-0118 ships in the same batch)

```
feat(ceir-8h): incremental-evaluation unification -- one dependency/dirty engine (ADR-0118)

- ENGINE-FIRST UNIFICATION (never a third graph): crd::containers::IncrementalDag is the ONE
  dependency/dirty engine. The LAYERING WALL decided the home -- crd-ceir is asset-free and
  render-asset-core does not link crd-ceir (independent siblings), so a crd-ceir engine is a
  permanent third graph by construction; the low shared module both link is crd-containers.
- IncrementalDag: a u64-keyed DAG with a deterministic topo (deps-first, min-id tie-break) +
  affected_by ABSORBED byte-identical from DependencyGraph, PLUS per-node content/interface
  revisions driving the §107 rule generalized (recompute_after_change: content-or-interface
  change recomputes the node; an INTERFACE change also recomputes its transitive dependents; a
  content-only change hot-swaps). Holds structure+revisions+dirty propagation, not the cached
  results (keyed by content hash). The `interface` field is `interface_rev` (the Windows COM
  `#define interface struct` landmine in a foundational header).
- THE ABSORB IS REAL THIS SLICE: render-asset-core::DependencyGraph keeps its 4-method API
  BYTE-STABLE and delegates to an owned IncrementalDag (AssetId<->NodeId lossless); the existing
  RAF/render-asset/scene-render/ceir-cook suites are the regression net. AnalysisManager (a
  different invalidation paradigm) + CookDb (semantic-only) are named-forward.
- ZERO-MOTION recook: runtime state; the §106 CDEP records are byte-stable.
- ADR-0118 ACCEPTED. tests: test_incremental_dag (6) incl. the §107 rule + insertion-order
  independence; the render-asset-core suite is the byte-identical determinism pin.

Gated: crd-containers-tests + crd-render-asset-core-tests + crd-scene-render-tests +
crd-ceir-cook-tests (relinked) + crd-ceir on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (incl. new incremental_dag.hpp) + GCC + opgen drift/validator.
Zero recook; the >=2 graphs are now one.
```

## CEIR-8i — the TRANSACTION model (ADR-0119) — 2026-08-09

**The gap.** Every authored edit to a live program (a node-graph editor, an agent, a CLI `ceir edit`) is a MULTI-OP
mutation that must be all-or-nothing. CEIR had the 1a mutation PRIMITIVES (create+place, erase, set_operand, RAUW,
set_attr) but no atomic grouping, no rollback, and no seam feeding 8h. The last U-§ foundation gap before 8z; the
CAD-parametric / agent-editing / reactive-UI universality domains are all transaction-shaped and blocked on it.

**The design fork (advisor-first): eager mutation + an inverse-op UNDO JOURNAL, reverse-order replay.** Rejected: a
COW module snapshot (fights the arena's stable-pointer identity model — handles ARE identity; a snapshot invalidates
every external reference + breaks stable-id continuity); a deferred-apply journal (the tx body couldn't see its own
edits — unusable for an editor building op X then wiring Y->X). ⛔ Reverse-order replay IS the correctness proof: each
primitive's inverse telescopes back last-to-first. ⛔ The Transaction RECORDS, Context APPLIES — six thin
transaction-only Context primitives (reinsert_erased_op / detach_and_point_use / rauw_recording / restore_attr_dict /
resync_symbols / find_by_stable_id) ride Context's EXISTING friendships → ZERO ir.hpp edit, no second mutation
implementation (the frame-graph-recording discipline).

**Reuses the band.** A pre-existing op is referenced by its 8d StableId (survives the edit — not a pointer/pre-order);
the committed touched/removed id-sets feed the 8h IncrementalDag (the tx says WHAT changed, the consumer computes
revisions — 8h's division of labour); a rejected edit / failed commit reports through the 8g DiagnosticEngine.

**Advisor's four begin-time corrections (all landed + tested):** (1) restore payloads (the attr dict snapshot) live in
the CONTEXT arena, never tx-owned — a rolled-back attr dict BECOMES live module state and must outlive the Transaction
(the linux-asan UAF guard test destroys the tx then reads). (2) commit re-syncs the SymbolTable from ops via the
shared detail::register_symbol path (a duplicate sym_name is a commit-verify failure; rollback needs nothing — the
index was never touched during the edit). (3) begin() calls assign_stable_ids up front — settling pre-existing ids =
the canonical serialized form, so the ONLY tx-window assignments are to tx-created ops (all erased on rollback) → the
watermark record/restore makes byte-identity UNCONDITIONAL. (4) graceful-reject + POISON at every mutator (never
delegate straight to the asserting 1a primitives — hostile agent input would crash).

**Advisor pre-close caught the region-bearing-erase subtree-boundary hole.** The erase pre-check guarded only the OUTER
op's results, and Operation::erase doesn't recurse — so a nested op's cross-boundary SSA edge would leak (an IN-edge: a
nested operand defined outside stays threaded into that external value's use-list after the tombstone; an OUT-edge: a
nested result used outside becomes a live use into a dead subtree). Every op the suite erased was region-free, so it
couldn't see it. Fixed: the erase mutator walks the subtree once and rejects both directions; a CLOSED subtree commits
whole + rolls back whole (three tests added).

**ZERO format motion — the recook story.** A transaction serializes NOTHING (runtime edit-session state, like the 8g
AnalysisManager / 4b compiler mode). A committed tx changes the module's CONTENT → the ordinary content-hash cache
miss the existing mechanism handles, NOT a schema recook. kBinaryVersion=2, kCeirCookSchema=4 UNTOUCHED; no new
serialized record ⇒ no new fuzz corpus.

**Tests.** test_transaction.cpp (12 [ceir][transaction]): commit applies + touched/removed; the A/B money proof (a
poisoned edit → byte-identical rollback); a duplicate-symbol commit-verify failure → byte-identical; reverse-replay
exact block order (both interleavings); a replacement never reuses a freed id; watermark-restore under rollback with a
mid-tx serialize; erase-with-live-uses rejected not asserted; the attr-undo lifetime proof (destroy the tx, then read
— Context-arena UAF guard); the 8h seam (touched drives recompute_after_change, §107 rule); region-bearing erase
IN-edge reject / OUT-edge reject / closed-subtree accept+rollback.

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **315/315 ctest** (303 + 12) on win-debug +
win-asan + linux-gcc-debug + linux-gcc-asan (all 3 targets fresh vs the new context.hpp surface — the stale-.obj scar)
+ LLVM-20 tidy (6 files incl. the new transaction.hpp) + GCC -Werror=switch (the 5-arm rollback switch; GCC also
caught an MSVC-hidden unused-function) + opgen drift/validator + crd-ceir-invariants (I6: the rollback switch is on
MutKind, not op.kind). NEXT = CEIR-8z (the BAND-8 GATE — the U-§121 twenty-item foundation DoD).

## Proposed commit — CEIR-8i (user commits; NO AI trailer; ADR-0119 ships in the same batch)

```
feat(ceir-8i): transaction model -- atomic authored-mutation with commit/rollback (ADR-0119)

- The atomic AUTHORED-mutation surface: a Transaction records IR edits against a Module, applies
  them EAGERLY (the body sees its own edits), and COMMITS atomically or ROLLS BACK to the
  byte-identical pre-transaction state. Model: eager mutation + an inverse-op UNDO JOURNAL,
  reverse-order replay (the correctness proof). Rejected a COW module snapshot (fights the arena's
  stable-pointer identity) and a deferred-apply journal (the body couldn't see its own edits).
- The Transaction RECORDS, Context APPLIES -- six thin transaction-only Context primitives ride
  Context's EXISTING friendships (reinsert_erased_op / detach_and_point_use / rauw_recording /
  restore_attr_dict / resync_symbols / find_by_stable_id), so ZERO ir.hpp edit and no second
  mutation implementation (the frame-graph-recording discipline).
- REUSES the band: a pre-existing op is referenced by its 8d StableId (survives the edit); the
  committed touched/removed id-sets feed the 8h IncrementalDag (the tx reports WHAT changed, the
  consumer computes revisions); a rejected edit / failed commit reports via the 8g DiagnosticEngine.
- id-under-rollback: begin() settles pre-existing ids up front (= the canonical serialized form) +
  records the watermark; rollback() restores it -> byte-identity unconditional; a replacement never
  reuses a freed id (the 8d discriminator holds across a committed tx).
- Graceful-reject + POISON on every mutator (agent-facing); commit VERIFIES (per-op verifier +
  symbol re-sync) BEFORE assigning ids. Region-bearing erase walks the subtree and rejects a nested
  IN-edge or OUT-edge (Operation::erase doesn't recurse); a closed subtree commits + rolls back whole.
- ZERO format motion: a transaction serializes nothing (runtime edit-session state); a committed tx
  is an ordinary content-hash cache miss, not a schema recook. kBinaryVersion/kCeirCookSchema
  untouched; no new fuzz corpus. Scope: authored edits only (compiler rewrites adopt journaling at 26).
- ADR-0119 ACCEPTED. tests: test_transaction (12) incl. the A/B byte-identical rollback proof, the
  attr-undo Context-arena lifetime guard, the 8h dirty seam, and the subtree-boundary rules.

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 315/315 on win-debug + win-asan +
linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (incl. new transaction.hpp) + GCC -Werror=switch +
opgen drift/validator + crd-ceir-invariants. Zero recook.
```

## CEIR-8z — BAND-8 GATE (no ADR) — 2026-08-09 → BAND 8 CLOSED

**The gate.** Like 3z/4z/5z, a COMPOSING gate that PROVES the foundation is closed — it composes existing decisions
(ADR-0111…0119) and introduces no new architecture, so it ships NO ADR. Two deliverables: the U-§121 foundation DoD
answered item-by-item from evidence, and a composed end-to-end proof program.

**The U-§121 DoD (the review doc).** The verbatim twenty-item enumeration lives in the quest prompt; the §B foundation
matrix (23 rows) IS its operational form, so it is answered item-by-item — each row → its closing slice + the SPECIFIC
test that is its evidence (framed AS a reconstruction, count flagged for the user to reconcile). Verdict: **20/23
CLOSED**; the 3 remainders (Provenance transform-preservation, unknown-plugin EXECUTION-denial, agent QUERY
introspection) are ◧ consumer-band-deferred (CEIR-26 / 9h / 9g), NOT foundation gaps.

**Advisor pre-close caught the §B matrix contradicting itself** — three stale rows fixed DURING the DoD pass rather
than inherited into the answer: Time domains read "❌ missing" though 8f closed it; Unknown-plugin content read "no
policy doc" though ADR-0111/0115 landed the preserve-opaque policy; and ⛔ **Provenance's "policy at 8g" was an
UNVERIFIED forward-claim — I checked ADR-0117 and it states NO transform-preservation policy**, so the honest verdict
is named-forward-to-26, not a claimed 8g policy. The lesson: a gate that answers a status matrix must RE-VERIFY each row
against code/ADRs; matrices accrue stale rows and unverified forward-claims.

**The composing proof — `test_band8_gate.cpp` (5 [gate8]).** ONE curated program carrying every foundation axis at once
— an Extern TYPE (8a) in an exported func signature, an Extern + aggregate ATTR (8b) on a capability-bearing op (8f)
with EXPLICIT determinism/domain axes (the 4z Unspecified-default trap), settled stable ids (8d) — then the guarantees
composed IN SEQUENCE on that same module: (1) byte-identical text⇔binary; (2) decode into an UNREGISTERED host (core
substrate present, plugin absent) preserves the plugin content + re-serializes byte-exact (U-§56 × STID × caps × extern
in ONE blob — no slice test did this); (3) re-register UNIFIES the preserved plugin type; (4) the interface hash is
cross-Context pure (caps + extern composed); (5) ⭐ a TRANSACTION edits the preserved plugin content in the unregistered
host — commit succeeds (an unknown op verifies true), touched-set correct; a second edit rolls back BYTE-IDENTICALLY
(the U-§52 agent-edits-plugin-content story end-to-end); (6) a single-byte corruption sweep over the COMPOSED blob
(cross-chunk: ATTR pool × STID × extern class strings). A tidy catch: `static const` gate arrays → dropped to plain
const locals (register_op arena-copies), the money config re-confirmed no UAF.

**Gate discipline (confirmed, not skipped).** Recook migrations kCeirCookSchema 1→2→3→4 (kBinaryVersion held at 2)
executed + clean — test_program_cook SchemaMismatch rejects stale blobs, the cook suite green at schema 4. Fuzz corpus
covers every open-world SERIALIZED record (extern types 8a / extern+aggregate attrs 8b / STID 8d / the composed blob
8z); effects/locations/safety/time/caps are REGISTRATION metadata → N/A-by-design. NO new invariant earns its keep
(documented-why: I6 gates op-kind dispatch; GCC -Werror=switch gates Extern-arm completeness, stronger than grep; the
single-mutation-path is an ADR-0119 discipline construction paths legitimately use raw primitives for).

**Gate.** crd-ceir-tests + host + cook **320/320 ctest** (315 + 5) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan (the money config proving the composed-blob fuzz + plugin-content transaction memory-safe) + LLVM-20
tidy (test_band8_gate.cpp) + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants. **THE VERDICT: the
foundation is universal by open-world semantics, not a second architecture. → BAND 8 CLOSED (8a..8z).** NEXT = CEIR-9a
(the first universality-VALIDATION proof: notebook/incremental through the 8h model).

## Proposed commit — CEIR-8z (user commits; NO AI trailer; NO ADR — a gate composes ADR-0111..0119)

```
test(ceir-8z): BAND-8 GATE -- the foundation DoD answered + one composed universality proof

- BAND-8 GATE (no ADR -- a gate COMPOSES existing decisions ADR-0111..0119, the 3z/4z/5z precedent).
  The foundation is universal by OPEN-WORLD SEMANTICS, not a second architecture.
- The U-§121 foundation DoD answered ITEM-BY-ITEM from evidence in the universality review (the
  §B matrix, 23 rows, each -> its closing slice + the specific test): 20/23 CLOSED; the 3 remainders
  (Provenance transform-preservation, unknown-plugin execution-denial, agent QUERY introspection)
  are consumer-band-deferred (CEIR-26/9h/9g), not foundation gaps. Fixed 3 stale/self-contradicting
  §B rows during the pass (Time domains, unknown-plugin policy, and Provenance's UNVERIFIED
  "policy at 8g" -> corrected to named-forward-26 after verifying ADR-0117 states none).
- test_band8_gate.cpp (5 [gate8]) -- ONE composed program (Extern type + Extern/aggregate attr +
  cap-bearing op with explicit determinism/domain axes + settled stable ids) proving, in sequence:
  byte-identical text<->binary; PRESERVE through an unregistered host + byte-exact re-serialize
  (U-56 x STID x caps x extern in one blob); re-register UNIFY + cross-Context interface-hash purity;
  a TRANSACTION editing preserved plugin content (commit + byte-identical rollback, the U-52 story);
  a single-byte corruption sweep over the composed blob.
- Gate discipline: recook migrations (kCeirCookSchema 1->2->3->4, kBinaryVersion=2) clean; fuzz
  covers every serialized open-world record (locations/effects/caps are registration metadata,
  N/A-by-design); no new invariant earns its keep (I6 + -Werror=switch + the ADR discipline).
- Review doc: the DoD table + the expanded D-H sections + the U-126 foundation verdict.

Gated: crd-ceir-tests + host + cook 320/320 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
Zero recook. -> BAND 8 CLOSED (8a..8z).
```
