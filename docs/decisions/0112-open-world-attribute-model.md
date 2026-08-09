# ADR-0112 — Open-world attribute model: aggregate kinds + dialect-defined attribute-classes

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8b**. Pays the recorded `attr.hpp:6` IOU ("CEIR-2/3
extend it [arrays, dictionaries, typed constants]" — never landed) AND opens the open-world attribute door
(U-§7: `cad.tolerance`, `eda.impedance_constraint`, `audio.sample_rate`, `ml.quantization`, …). Mirrors ADR-0111
(the open-world TYPE model) applied to attributes.
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B; mission §7/§8.
**Tags:** `[ceir]` `[attributes]` `[open-world]` `[serialization]` `[plugin]` `[units]` `[foundation]`

---

## 1. Context

`AttrValue` (attr.hpp) is a closed set of six scalar kinds (Int/Float/Bool/String/SymbolRef/Type) — attr.hpp's own
header comment records the debt: *"CEIR-2/3 extend it (arrays, dictionaries, typed constants) behind the same
`AttrValue`."* Two gaps follow: (a) an engineering value like `cad.tolerance` or a parameter table has no TYPED
representation — it would degrade to a free-form String (the U-§7 stringly-typed anti-goal); (b) a domain cannot
define a semantic attribute-class the way it defines an op or (post-8a) a type. This ADR closes both, reusing the
ADR-0111 discipline: a small closed built-in set + an open-world extension surface, no central-enum edit per
plugin attribute.

## 2. Decision

### 2.1 Two wrapper kinds over one aggregate vocabulary (the composition insight)

Rather than parallel storage families, 8b is **one aggregate vocabulary** wrapped by **two discriminators**:

- **Aggregate kinds** (the payload vocabulary):
  - **`AttrKind::Array`** — an ordered `ConstSpan<AttrId>` (`elems`). Heterogeneous allowed.
  - **`AttrKind::Dict`** — a string→value map: parallel `ConstSpan<StringView> keys` + `ConstSpan<AttrId> elems`.
    ⛔ **Keys are canonicalized (byte-order sorted) at the factory** — one representation for dedup + content-hash
    stability; authored key order is NOT semantic (the printer-determinism doctrine).
- **Wrapper kinds** (each wraps a SINGLE `AttrId` payload, stored as `elems[0]`):
  - **`AttrKind::TypedConst`** — `{ wrapped_type: TypeId, payload }` — a value OF a declared type (the units story
    for attrs: `cad.tolerance` = a TypedConst float of type `!qty<!f64, L>`, U-§63).
  - **`AttrKind::Extern`** — `{ attr_class: AttrClassId, version, payload }` — a dialect-defined attribute-class.

⛔ **Composition rule (the `qty<qty<…>>` precedent, 3e):** a wrapper may wrap an aggregate or a scalar but **NOT
another wrapper** — no `TypedConst<TypedConst<…>>`, no `Extern<Extern<…>>`. Enforced at the verify triple.

### 2.2 Open-world attribute classes — their OWN registry, the 8a shape

`AttrClassId` (interned FNV "dialect.attr", a distinct id mirroring `TypeClassId`/`OpId`) +
`Dialect::register_attr_class(AttrClassSpec{verify, version})` → a Context `m_attr_classes` registry (⛔ NOT
overloading the type-class registry). Same three-boundary verify (factory ASSERTS · binary decoder + text parser
REJECT registered-invalid · UNREGISTERED PRESERVES opaquely, U-§56), same version range-check on decode
(newer-schema record = reject; older = accept, migration named-forward), same string-keyed serialization (the
class STRING via STRP; `AttrClassId` a runtime cache), same late-register-unify headline test — proven **text AND
binary from day one** (8a's blocker-2 pre-empted).

### 2.3 `AttrValue` layout + equality + the canonicality guard

`AttrValue` grows a tagged layout — `{union scalars} + StringView s + ConstSpan<AttrId> elems + ConstSpan<StringView>
keys + {AttrClassId attr_class; u32 attr_class_version} + TypeId wrapped_type`. It is intern-table-only (attr.hpp's
"size not hot-path"). `operator==` compares ONLY the active payload per kind (element-wise for aggregates; class +
version + payload for Extern; type + payload for TypedConst). ⛔ **`attr_is_canonical(const AttrValue&)`** (the 8a
canonicality analogue, pre-empting 8a's blocker 1): every field a kind does NOT use is zero/default, else two
values that `operator==`-differ in an inactive field would intern to one id / serialize divergently. **Asserted in
`intern_attr`** (the `intern_type` house pattern), **rejected** in the decoder/parser. The six existing scalar
kinds get the canonicality test FIRST (the factories zero-init, so they pass).

### 2.4 Serialization — byte-identical scalars, child-first pool, no version bump

- ⛔ **THE CONTENT-HASH INVARIANT:** `stable_hash` is FNV over the serialized blob, so the six scalar kinds MUST
  encode **byte-identical** to pre-8b (same kind byte, field order, widths) — new fields serialize **CONDITIONALLY,
  only on the new kinds**. Any leak into the scalar encoding is a silent global recook. The
  pre-8b-blob-decodes-byte-identically test is matrix item 1.
- **Child-first ATTR pool** (the `intern_type_pool` shape): an aggregate/wrapper's child `AttrId`s (and their
  transitive strings/types) are pooled BEFORE the record's index is assigned; the decoder rejects a forward/self
  ref by construction. The ATTR decoder's kind-range check is raised to the new max (the binary.cpp:438 analogue) —
  it both gates hostile input AND makes pre-8b decoders reject new-kind records → **no binary version bump**.
- ⛔ **Depth guard** (`kMaxTypeDepth` precedent) on aggregate recursion in text parse, binary decode, AND the
  encoder's child-first walk — attrs can now nest arbitrarily; the fuzz sweep will find an unguarded stack.

### 2.5 Text forms (generic; read against the existing attr-value grammar)

`[v0, v1, …]` (Array) · `{k0: v0, k1: v1, …}` (Dict — the `:` separator distinguishes it from the op-attribute
dict's `name = value`, which is parsed at the op level, not in value position) · `#typed<!TYPE, payload>` ·
`#extern<dialect.attr, VER, payload>` (the `#` sigil is free in the attr-value grammar). Aggregates ARE their own
canonical form (nothing class-keyed to be unregistered about); the wrappers use fixed-order generic forms — NO
per-class pretty hooks (the ADR-0111 §2.4 reason: an unregistered Context must re-parse it).

### 2.6 What stays deliberately closed / accepted

The six scalar kinds + Array/Dict/TypedConst/Extern are the built-in attribute vocabulary; further extension is via
attribute-classes (never a new `AttrKind`). ⛔ **`NamedAttr` (op name→value) is NOT the U-§7 stringly-typed
anti-goal:** attribute NAMES are schema-declared (the 2a generator / hand registration), and values are now fully
TYPED — a name→typed-value map is the correct model, distinct from an untyped free-form string dictionary as the
extension mechanism. Attributes feed the CONTENT hash (via the blob) but are NOT projected by the §107 interface
hash (interface_hash projects func signatures) — so, unlike types, there is no interface-hash landmine here.

## 3. Scope fence — named-forward

TOML `[[attr]]` generator support · per-class pretty forms · attr-class migration transforms (version stored +
range-checked, transform named-forward) · a fixed-precision/bignum scalar (mission §16 "where later useful"). None
has an 8b consumer.

## 4. Consequences

**Positive.** Typed structured attribute data (arrays/dicts/typed constants) + dialect-defined attribute-classes,
all with interning, structural equality/hash, serialization, and unknown-plugin round-trip — zero central-enum
edits per plugin attribute. The units story reaches attributes (TypedConst over `!qty<…>`). **Negative / cost.**
`AttrValue` grows (intern-table-only); a new canonicality assert in the intern path (existing factories pass);
five switches gain four arms each. **Risk.** The byte-identical-scalar-encoding invariant is airtight-or-silent-
recook — mitigated by the pre-8b-blob test landing first + the fuzz sweep.

## 5. Test obligations (the 8b gate)

The 8a nine transposed + (a) canonicality of the six EXISTING kinds; (b) pre-8b blob decodes byte-identical
(scalar encoding unchanged); (c) Array/Dict round-trip text+binary; (d) Dict key canonicalization (authored order
irrelevant → one id + one blob); (e) TypedConst + Extern round-trip; (f) the U-§56 headline (text AND binary) +
late-register-unify for attr-classes; (g) the verify triple; (h) version range (newer-reject + older-accept);
(i) the no-wrapper-on-wrapper composition rejection; (j) a nesting/single-byte fuzz sweep over the new records
(ASan-proven).

## 6. References

- ADR-0111 (open-world type model) — the template this mirrors for attributes.
- attr.hpp (the six-scalar starter set + the recorded IOU); binary.cpp (the child-first TYPE pool, the model for
  the ATTR pool); the universality review §B (the attribute gap).
- Mission §7/§8 (attributes/properties), §16 (types, for TypedConst), §63 (units, the TypedConst-over-quantity use).
