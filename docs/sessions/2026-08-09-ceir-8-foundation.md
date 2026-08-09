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
