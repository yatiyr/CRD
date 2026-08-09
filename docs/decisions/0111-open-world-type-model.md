# ADR-0111 — Open-world type model: dialect-defined type-classes beside the built-in `TypeKind`

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8a**. Implements the universality quest's U-§6
(open-world types) and pays no recorded IOU but opens the door every future domain type needs (`cad.BRep`,
`eda.Net`, `audio.Spectrum`, `geometry.HalfEdgeMesh`, `ml.KVCache`, …). Builds on ADR-0109 (crd-ceir host-only
core; §6 semantic identity) and mirrors ADR-0110's op-model registration precedent.
**Phase:** D-007 (CEIR programme). Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (the type gap),
mission `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` §16.
**Tags:** `[ceir]` `[types]` `[open-world]` `[serialization]` `[plugin]` `[substrate]` `[foundation]`

---

## 1. Context

The CEIR **op** layer is genuinely open-world: an op-kind is an interned `OpId` (FNV of "dialect.op"), registered
open-world via `Dialect::register_op`, and the core never switches on op kind (invariant I6). A new dialect adds
ops with **zero central edits**. The **type** layer is not: `TypeKind` (type.hpp) is a closed `u8` enum of ~30
built-in structural kinds, and every consumer (printer, parser, binary, verifier) switches on it. A domain that
needs a genuinely new type category (a CAD `BRep`, a PCB `Net`, a half-edge mesh) would have to edit that central
enum — the exact central-switch bottleneck the universality standard (U-§5, U-§125) forbids.

The fix is to apply the op model to types: a **dialect-defined type-class**, interned and registered open-world,
represented behind one appended `TypeKind::Extern` door — so the ~30 built-ins keep their fast jump-table path and
custom types ride the same interning / structural-equality / structural-hash / serialization machinery CEIR-3
already built, for free.

## 2. Decision

### 2.1 Representation — `TypeKind::Extern` + `TypeClassId`, reusing the existing param slots

- **One appended enum value, `TypeKind::Extern`** (at the END — the enum comment already reserves append-only
  growth; the binary out-of-range-kind reject makes appended kinds invisible-and-rejected to older decoders, the
  3b/3c precedent). This is the ONE-TIME `TypeKind` edit that opens the door. **`test.cad.TopoShape` and every
  custom type after it, forever, land with zero further enum edits.**
- **`TypeClassId`** (id.hpp) — the interned FNV-1a hash of "dialect.class", a distinct id type mirroring `OpId`.
  `Context::intern_type_class(dialect, class)` + `type_class_name(id)` reverse-lookup mirror `intern_op`/`op_name`.
- An `Extern` `Type` carries a **`type_class` field** (`TypeClassId`) plus its parameters in the **existing generic
  slots**: `members` (child `TypeId`s), `count`/`cols`/`is_signed`/`fkind` (scalar params), `name`/`labels`
  (strings). A type-class documents its slot usage in its reflection schema exactly as an op documents its operands.
  **Guidance (in this ADR, binding on class authors):** more than two integer parameters ⇒ carry them as a `Shape`
  member of static `Dim`s (semantically better, and it dodges the 2×`u32` count/cols ceiling). **No `AttrId` /
  data-payload slot on a type** — a type carrying data-that-is-not-a-parameter is a value/attribute concern
  (CEIR-8b at most). The ceiling is named honestly: if a real domain type ever overflows the slots, a versioned
  param-model v2 is the answer, not a bespoke escape hatch.

### 2.2 Registration — `Dialect::register_type_class(name, TypeClassSpec)`

Mirrors `register_op`. `TypeClassSpec` in 8a is deliberately **three fields only**: a **verify hook**
(`bool(const Context&, const Type&)` — is this Extern instance well-formed for its class?), a **version** (`u32`),
and a **reflection schema** (the class name + its documented slot usage, the `.ops.json` analogue). The registry
lives on the `Context` (`m_type_classes`, keyed by `TypeClassId.value`), host-only, no new module edge.

### 2.3 Equality, hashing, interning — the load-bearing invariant

`Type::operator==` and the intern hasher **MUST compare `type_class`**. Two different classes with identical param
slots are DIFFERENT types; omitting `type_class` from equality/hash would intern them to one `TypeId` — silent type
confusion, the worst bug this slice could ship. With `type_class` in both, custom types get structural interning,
dedup, and the content-pure hash for free (the 3a machinery, unchanged).

### 2.4 Serialization — string-keyed, generic-form-only (the U-§56 round-trip)

- **Binary** (the `TYPE` chunk): an `Extern` record stores its class **string** via the `STRP` pool (exactly as
  ops store "dialect.op" via STRP — binary.cpp), the class **version** (`u32`), and its param slots. The class
  string is what survives to an unregistered decoder; `TypeClassId` is a runtime cache only. **No binary version
  bump** — the extra trailing fields exist only on `kind==Extern` records, which older decoders reject anyway
  (out-of-range kind); pre-8a blobs decode byte-identically (tested).
- **Text** — a **generic canonical form only**, no per-class pretty-print/parse hooks in 8a. A pretty hook
  (`!cad.nurbs<3x!f64>`) would emit text an *unregistered* Context cannot re-parse, structurally breaking the
  U-§56 unknown-plugin text round-trip and 1e's byte-exact property. The generic form is fixed-order, total, and
  injective — it round-trips **universally, registered or not**. Pretty forms have no consumer until the
  language/editor bands (CEIR-32/33) and are named-forward there WITH THE REASON (universal round-trip > cosmetics),
  not merely deferred.
- **Interface hash** (`program_asset.cpp`'s `encode_type`): emit the class string **conditionally, only for
  `kind==Extern`**. Unconditional emission would change every existing module's interface hash (a needless global
  recook); omission would make two custom types hash-equal, silently breaking 7b's registry-drift discriminator.
  Conditional ⇒ **zero hash churn for existing content**.

### 2.5 Verification & unknown-plugin policy (U-§56) — the three-boundary rule

A **registered** class's verify hook runs at each of the three construction boundaries, matching the 3a house style:
the **factory** (`type_extern`) runs the hook and **asserts** (builder misuse = programmer error); the **binary
decoder** **rejects** a registered-class-failing record (hostile input, declared-words-validated); the **text
parser** **rejects** into a `ParseResult` error. An **unregistered** class is **preserved opaquely** at all three
boundaries — you cannot verify what you do not know, and for a persisted document preservation beats rejection
(U-§56). `type_is_well_formed`/`type_is_canonical` are Context-free (they cannot reach the registry), so their
`Extern` arms **accept structurally** (documented: Extern arity/canonicality is the registered hook's layer, not
theirs; the junk-ignored-field canonicality rule cannot apply to a class whose slot usage the checker cannot know).

### 2.6 Why `TypeKind` stays otherwise closed

The ~30 built-in kinds are the universal structural vocabulary (scalars, aggregates, resources, shapes, quantities,
ownership) every domain shares; a closed enum gives them a fast jump-table path and a compact record. Custom types
do not need that fast path (they are domain-specific, not hot in the core), so they ride the one `Extern` door.
This is the U-§2 principle: universal ≠ one flat vocabulary — a small closed core + an open extension surface.

## 3. Scope fence — named-forward (each has no 8a consumer)

TOML `[[type]]` generator support (no generated dialect needs a custom type; 9h's external-plugin proof uses hand
registration by design) · per-class pretty print/parse hooks (§2.4 reason) · attr/data payloads on types (8b) ·
class-version **migration** hooks (the version is stored and range-checked — equal=accept, greater=reject — but the
migration transform is named-forward, no consumer) · substitution re-running a custom class's verify hook (subst
rebuilds an Extern generically and correctly; the hook re-validates at the next factory/decode/parse boundary).

## 4. Consequences

**Positive.** A new domain type is a hand-registration (or, later, a TOML edit) with zero central-enum edits;
custom types inherit interning, structural equality/hash, arena-stable spans, binary+text serialization, generics
(`substitute`/`type_has_params` walk `members` generically — verified), and content/interface hashing. Documents
with unknown plugin types round-trip losslessly and become fully usable on late registration.

**Negative / cost.** Custom-type text is verbose (the generic form) until pretty hooks land; the param model is the
8 fixed slots (a documented, versionable ceiling); `TypeKind::Extern` is a permanent (harmless) member of the enum.

**Risk.** The `operator==`/hasher `type_class` inclusion is airtight-or-silent-corruption — mitigated by a dedicated
different-class/same-params discriminator test + the fuzz corpus over `Extern` records landing WITH this slice.

## 5. Test obligations (the 8a gate)

(1) intern dedup + the different-class/same-params discriminator; (2) generic-form text round-trip byte-exact;
(3) binary round-trip byte-exact; (4) **the U-§56 headline** — serialize registered → decode into an UNREGISTERED
Context → re-serialize BYTE-EXACT → register late → the type is fully usable (text and binary); (5) the verify-hook
triple (factory asserts / decoder rejects / parser rejects — registered-invalid only); (6) interface-hash class
discrimination + zero churn on existing content; (7) the fuzz corpus over `Extern` (corrupt class-STRP index,
non-splitting class string, bad member refs, bad version → typed rejection, no crash); (8) substitute-through-Extern
(a generic custom type); (9) a pre-8a blob still decodes byte-identically.

## 6. References

- Mission §16 (types); the universality review §B (the gap matrix).
- ADR-0109 (crd-ceir host-only core; §5 finalized names; §6 identity) — this ADR adds `TypeClassId` to §5's family.
- ADR-0110 (native-intrinsic schema) — the op-registration precedent this mirrors for types.
- type.hpp (the `Type` struct + the ~30 built-in kinds); binary.cpp (string-keyed op encoding, the model for types).
