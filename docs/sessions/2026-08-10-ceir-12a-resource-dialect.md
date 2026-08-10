# CEIR-12a — the `ceir.resource` dialect (resource decl/view/range + import/export) — session log

> The autonomous grind ([[project_ceir_autonomous_loop_grant]]) opened CEIR-12 (Resource/memory subsystem) after band 11
> closed. 12a = the resource op vocabulary (§36 + §23). Recon + advisor design consult. ⛔ NO ADR (an op-vocabulary
> slice, the 11a/six-ops precedent); the tracker row + this log carry it.

## The settled design (advisor consult)

⭐ **The substrate already exists** (CEIR-3c): the resource/view TypeKinds (`Buffer`/`Image`/`View`/`ResourceTable`/
`ExternalResource`/`Tensor`) + the `ctx.type_*` factories + `view_combination_valid` + the `ViewRange{Byte,Element,Mip,
Layer,Aspect}` mask. ⛔ **REUSE it — mint no parallel vocabulary.** Resource-ness rides the TYPE, NOT a new trait
(`kKnownTraitsMask` is pinned); cross-op typing is enforced by a module-walk verifier.

**The op vocabulary** (`engine/ceir/ops/resource.ceirop.toml`, 4 ops):
- **`resource.declare`** → a GRAPH-OWNED resource (the planner owns its memory, 12d); result = a 3c resource kind; a
  `name` symbol attr (attr-only — NOT a SymbolTable entry; cross-module resolution is 13+) + an **`alias_group` symbol
  attr** (doc-only this slice — the advisor's addition: 12c's alias/lifetime analysis CONSUMES it, so it gets a syntactic
  home NOW rather than 12c minting vocabulary mid-analysis). `Pure`.
- **`resource.view`** (ONE op spans view/range/subresource — the 3c `View` type unifies them, the mask IS the taxonomy):
  operand(0) = the resource + a VARIADIC tail of runtime offset/size operands; result = `type_view(underlying, mask)`
  (the mask lives in the result View TYPE; offsets/sizes are runtime, ASCENDING ViewRange bit order). `Pure`.
- **`resource.import`** → an EXTERNALLY-OWNED resource (the provider binds the handle — §150 seam, named-forward to 13+);
  result = a 3c resource kind (often `external_resource`); a `name` symbol attr. `Pure` at the IR layer. ⛔ Distinct OP
  from declare — graph-owned vs externally-owned is the OP KIND, not a flag: the planner plans declare's, NEVER import's.
- **`resource.export`** → publish a resource externally (the boundary). ⛔ NOT Pure — a conservative
  **`MemoryReadWrite{operand=0}`** effect so the 12c hazard walk sees exports as ORDERING-RELEVANT (writes before export
  must complete; external may mutate) — getting this wrong is the WAR scar's IR edition. The direction is a 12b/provider
  refinement.

⛔ **Semantics are a NAMED-FORWARD** — an uninstalled resource op is a typed NoSemantics (the async precedent), landing
when the resource runtime + provider binding land (13+ / §150). Say this so nobody "fixes" it at 12b.

## ✅ Part 1 — the DIALECT (TOML + opgen) — DONE + gated (2026-08-10)

`resource.ceirop.toml` written; `python tools/ceir_opgen/ceir_opgen.py` regenerated the 5 artifacts
(`gen/resource_ops.{hpp,cpp}` — the interners, `ResourceXxxOp` wrappers, `build_*` builders, the per-op structural
`verify_*` (arity/attr-kind) + effect tables + `register_resource_ops(ctx)`; `resource.ops.{json,md}`;
`tests/ceir/generated/test_resource_gen_smoke.cpp`). ⛔ NO central edit — the library + smoke test are glob-collected;
consumers call `resource::register_resource_ops(ctx)` caller-side (no "register all" hub). **Gate:** the generated smoke
tests **3/3** (self-registers + reflects a coherent schema; every op builds through its builder + the structural verify
accepts; the verify rejects a malformed construction) on **win-debug + win-asan + linux-gcc-debug** + opgen
drift (`--check` clean — a fresh regen) + the opgen validator (`test_opgen.py` OK — schema-valid) + `crd-ceir-invariants`.
Full ceir **447/447** (444 + 3). NO recook/fuzz/version-bump.

## ✅ Part 2 — the VERIFIER + the rich test — DONE + gated (2026-08-10) → CEIR-12a CLOSED

**`Context::find_resource_misuse(const Module&) const noexcept`** (`context.{hpp,cpp}`) — a pre-order module walk (the
`find_token_misuse` house shape) returning the FIRST resource-op misuse or `{None}`. ⛔ Identifies the resource ops by
`op_name(op->kind())` STRING (a const-safe reverse lookup — no interning in a const method); reads the CEIR-3c resource
TYPE (`type_of` + `view_combination_valid`). Enforces (8 `ResourceMisuseKind`s): **view** — result is a `View`
(`ViewResultNotView`), operand(0) is a **VIEWABLE** resource — a Buffer or Image, the only kinds `view_combination_valid`
admits (`ViewOperandNotViewable` — ⛔ the advisor pre-close catch: `View` was in the resource set, making view-of-view
*legal-by-accident* at the operand check but dead at the mask check with a MISLEADING `ViewMaskInvalid`; §36 views are
flat, so a view over a `View`/`Sampler`/`Tensor`/i32 is now rejected CLEARLY), the View's underlying == operand(0)'s type
(`ViewUnderlyingMismatch`), the mask is legal for the underlying (`ViewMaskInvalid`), `num_operands == 1 +
2*popcount(mask)` (`ViewRangeArity`); **export** — operand(0) resource-kinded (`ExportOperandNotResource`); **declare /
import** — result(0) resource-kinded (`DeclImportResultNotResource`, the advisor's "at minimum" widened to close the
constructor hole for those too). ⛔ The per-op check ORDER is CONTRACTUAL (the negatives pin the exact kind) — stated in
a `scan_resources` comment (advisor item 2). ⛔ A local `ceir_is_resource_kind(TypeKind)` classifies the §23 resource kinds (Buffer/
Image/Tensor/SparseTensor/Sampler/ResourceTable/AccelStruct/VideoFrame/AudioBuffer/ExternalResource/View) — a `default`
that a comment flags "a NEW resource TypeKind MUST be added here" (U-116 pins TypeKind's end, so widening is gated).

**`test_resource.cpp`** (+2 `[ceir][resource]`): a well-formed declare+view+import+export module — `find_structure_error`
+ `find_resource_misuse == None`; ⭐ a **BINARY + TEXT round-trip** (serialize→deserialize + print→parse into fresh
contexts; the loaded twins still verify clean — the `View`/`ExternalResource` TypeKinds flowing through op RESULTS survive
the serializers, the risk the advisor flagged); and all **8 malformed constructions** rejected with the exact kind (a
view over an i32 → NotViewable; ⭐ a **view-of-view** → NotViewable [the advisor's added case]; a non-View result; an
underlying≠operand; a `Mip` mask on a buffer — built via a raw `intern_type` to bypass `type_view`'s factory-assert; a
wrong operand count; an export of a non-resource; a declare of an i32).

**Gate.** crd-ceir-tests + host + cook **449/449 ctest** (447 + 2) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (`context.{cpp,hpp}` + `test_resource.cpp`, clean) + GCC `-Werror=switch`
(`resource_misuse_kind_name` + `ceir_is_resource_kind`) + opgen drift/validator + `crd-ceir-invariants` (I3/I5/I6/U-116 —
the walk is a string compare, NOT a switch on `op.kind()`; crd-ceir core jobs-free/asset-free). ⛔ **NO recook, NO fuzz,
NO version bump.** ⭐ **CEIR-12a CLOSED.** No ADR (an op-vocabulary slice). Named-forwards: resource-op SEMANTICS →
12b-or-later (typed NoSemantics today); PROVIDER handle binding → 13+ / §150; the alias_group CONSUMPTION + the
lifetime/hazard analysis → 12c; the memory planner → 12d.

## Proposed commit — CEIR-12a part 2 (the verifier + the rich test + close; user commits; NO AI trailer)

```
feat(ceir-12a): find_resource_misuse (the resource type-system verifier) + CLOSE 12a

Part 2 of CEIR-12a. The type-system ENFORCEMENT layer for the resource dialect + the rich test; closes
the slice. A crd-ceir CORE change (context.cpp).

- Context::find_resource_misuse(const Module&): a pre-order module walk (the find_token_misuse house
  shape) enforcing the resource ops' TYPING that opgen's structural verify + doc-only TOML types cannot.
  Identifies the ops by op_name string (const-safe); reads the CEIR-3c resource TYPE (type_of +
  view_combination_valid). 8 ResourceMisuseKinds: view result-is-View / operand-resource / underlying==
  operand / mask-valid / arity(1+2*popcount(mask)); export operand-resource; declare|import
  result-resource. Resource-ness rides the type, not a trait.
- test_resource.cpp +2 [ceir][resource]: a well-formed declare+view+import+export module (structure +
  find_resource_misuse==None) + a BINARY + TEXT round-trip (View/ExternalResource kinds in op results
  survive the serializers) + all 7 malformed constructions rejected with the exact kind.

Gated: 449/449 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator + crd-ceir-invariants (I6 clean -- a string compare, not a
switch on op.kind; core jobs-free/asset-free). No recook, no fuzz, no version bump. No ADR (op-vocabulary
slice). Named-forwards: semantics -> 12b+; provider binding -> 13+/sec-150; alias/lifetime analysis -> 12c.
```

## (superseded) Part 2 plan — see above for the shipped version

⛔ **`Context::find_resource_misuse(const Module&)`** — the type-system's ENFORCEMENT layer (NOT optional; the
`find_token_misuse` house shape — a deterministic pre-order module walk). The construction hole recurs: opgen's
structural `verify_*` checks arity/attr-kinds only + TOML `type` fields are doc-only, so `create_operation(view_kind,
{i32}, 1, type_view(...))` builds fine — the walk verifier closes it. Enforce, per op:
- **view:** operand(0) is resource-kinded (`type_of(operand(0)->type()).kind` ∈ the resource kinds); the result View's
  `members[0]` (underlying) == operand(0)'s type; the mask (`result.count`) passes `view_combination_valid(underlying,
  mask)`; `num_operands == 1 + 2*popcount(mask)` (the variadic range arity).
- **export:** operand(0) is resource-kinded.
- A `ResourceMisuse{value, op, ResourceMisuseKind}` + `ResourceMisuseKind{None, ViewOperandNotResource,
  ViewUnderlyingMismatch, ViewMaskInvalid, ViewRangeArity, ExportOperandNotResource}` (context.hpp), the impl in
  context.cpp (mirror `find_token_misuse`'s recursive walk), `-Werror=switch` name fn.

**The rich `test_resource.cpp`** (one CMakeLists line): a declare+view+import+export module passing `find_structure_error`
+ `find_resource_misuse == None`; the verifier NEGATIVES (a view over an i32; a view whose result-underlying ≠ operand
type; a mask failing `view_combination_valid`; a wrong operand count for the mask; an export of a non-resource); and a
⭐ **text + binary round-trip** of the module (the 5z/rich-graph precedent — the View/ExternalResource TypeKinds flowing
through op RESULTS is where a serializer gap would surface). Then the pre-close consult, the 12a row flip ◧→✅.

## Proposed commit — CEIR-12a part 1 (the dialect; user commits; NO AI trailer)

```
feat(ceir-12a): the ceir.resource dialect -- declare/view/import/export (sec-36 + sec-23)

Part 1 of CEIR-12a (the first slice of the Resource/memory subsystem band). The resource op vocabulary;
resource-ness rides the CEIR-3c resource TYPES (Buffer/Image/View/ExternalResource), NOT a new trait.

- engine/ceir/ops/resource.ceirop.toml (4 ops): declare (graph-owned resource; name + alias_group symbol
  attrs; Pure); view (view/range/subresource -- ONE op, the 3c View type unifies them; operand = resource
  + a variadic offset/size tail; result = type_view(underlying, mask); Pure); import (externally-owned;
  provider-bound handle = a sec-150 named-forward; a distinct OP from declare -- the planner plans
  declare's, never import's; Pure); export (the external-publish boundary; a conservative
  MemoryReadWrite{operand=0} so the 12c hazard walk orders it). Semantics = a typed NoSemantics
  named-forward (the async precedent). opgen regenerated the 5 artifacts (no central edit).
- The find_resource_misuse module-walk verifier + the rich test_resource.cpp (round-trip + negatives)
  are part 2.

Gated: the generated smoke 3/3 on win-debug + win-asan + linux-gcc-debug + opgen drift/validator +
crd-ceir-invariants; full ceir 447/447. No recook, no fuzz, no version bump. No ADR (op-vocabulary slice).
```
