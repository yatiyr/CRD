# CEIR-13a — the `ceir.compute` dialect: dispatch ops + the kernel binding contract (§42)

**Date:** 2026-08-10 · **Slice:** CEIR-13a (D-007 master spine) · **Status:** ✅ CLOSED · **ADR:** none (op-vocabulary +
verifier slice, consistent with 12a). Opens **CEIR-13** (compute + transfer — first GPU contact).

## Contract

D-007 CEIR-13a row: *"`ceir.compute`: direct/indirect dispatch ops, kernel binding records (against the RAH-era binding
model)."* → §42. The FIRST GPU-contact slice: the host-authorable dispatch op VOCABULARY. ⛔ core-only — the ops live in
`crd-ceir`; the `crd-ceir-gpu` bridge lowers them onto the canonical command model / `IComputeContext` at CEIR-13d (core
never links gpu-context — I3/I4/I5), exactly the `ceir.task` → `crd-ceir-host` precedent.

## Design (advisor-reviewed at the fork; all verdicts + deltas integrated)

**`engine/ceir/ops/compute.ceirop.toml`** — two ops:
- **`compute.dispatch`** — direct: operands `[grid_x, grid_y, grid_z` (index — dynamic dims, §42)`, bindings…` (variadic
  resource, SLOT order)`]`; attrs `kernel` (symbol — the @kernel identity; KernelRef `{asset_id, interface_hash}`
  resolution is 13c) + `access` (string). Resultless.
- **`compute.dispatch_indirect`** — indirect: operands `[args` (resource — the dims buffer)`, bindings…]`; same attrs.

**The binding record encoding (the design fork):**
- **`access` = a delimited string** `{r|w|rw}`, comma-separated, one token per binding in operand order (empty ⇔ zero
  bindings) — the arith.cmp.predicate precedent. ⛔ chosen over a CEIR-8b array attr (opgen can't declare it → invisible to
  the schema/§122 discovery/structural verifier) and over a bitmask (not authorable in the 13z TEXT proof).
- **Binding KIND is DERIVED from the operand's CEIR-3c type** (Buffer→storage, Image→texture, View→its underlying
  `members[0]`) — NO separate kind attr (ONE source of truth, 12a doctrine). **SLOT = positional index.**
- **Effects = a CONSERVATIVE BASELINE: `GPUCommand` + ambient `MemoryReadWrite`** (indirect also carries a static
  `MemoryRead{operand=0}` on args). ⛔ the advisor's Q4 correction: GPUCommand-ONLY was wrong — GPUCommand is the `Gpu`
  class, an export's is `Memory`, so a dispatch and the export of its own output would read as *freely reorderable* (the
  WAR-scar IR edition). The ambient makes a dispatch hazard any memory op; CEIR-13d NARROWS it to per-binding effects.

**`Context::find_dispatch_misuse`** (the `find_resource_misuse` house pattern) — 6 `DispatchMisuseKind`s, contractual order:
`GridNotIndex` (direct operands 0–2 Index-typed) / `AccessTokenInvalid` / `AccessArityMismatch` (token count == `n-3`
direct, `n-1` indirect) / `BindingNotResource` (the 12a `ceir_is_resource_kind` predicate) / `ArgsNotBuffer` (indirect
operand 0 a Buffer or View-of-buffer). Reuses the 12a resource-kind predicate; no StringView slicing in the access parser
(compares token bytes directly — r/w len 1, rw len 2). ⛔ **the wrong-KIND fold** (advisor pre-close): an `access` that is
absent or non-String folds into `AccessTokenInvalid` (the 12b fold doctrine) — NOT silently skipped, because a deserialized
module is built RAW and may reach the standalone walk before per-op verify (a false-clean would be a real path).

**NO-FOLLOW (named in the TOML, never silently subset):** §42's persistent kernels + cooperative launches + dispatch
groups → 13+/26; uniform-buffer bindings (BufferMode has no Uniform mode; the §129 proof binds storage) → 13d/render;
BindingFrequency (descriptor-update frequency, a lowering concept) → 13d/render. Resultless dispatch recorded as a decision
(ordering is effect-derived; async composition wraps at `ceir.async`).

## Tests (`tests/ceir/test_compute.cpp`, +3 TEST_CASEs, ASCII names — the advisor's checklist)

- well-formed direct + indirect, `find_dispatch_misuse` clean + BINARY & TEXT round-trip with `kernel`/`access` value
  read-back on both twins (the 12b standard).
- one negative per misuse kind (Grid/Access-token/Access-arity/Binding/Args) + the wrong-KIND fold (`access` = an int →
  `AccessTokenInvalid`, calling only `find_dispatch_misuse` — the standalone-robustness pin).
- the **conservative-baseline pins**: `ops_hazard(dispatch, export) != None` (the dispatch-vs-export hazard — the Q4
  regression guard) + a 12c integration case (a dispatch's ambient effect extends a prior transient's live range).

## Gate (GREEN)

- **win-debug 467/467 · win-asan 467/467 · linux-gcc-debug 467/467 · linux-gcc-asan 467/467** (was 461; +3 hand +3 generated
  compute smoke).
- opgen regen (5 compute artifacts) + `--check` drift-clean + validator OK. GCC clean (`-Werror=switch`: the 6-arm
  `dispatch_misuse_kind_name`). LLVM-20 tidy clean: `context.cpp`, `context.hpp`, `test_compute.cpp` (fixed a
  `s.data()[i]`→`s[i]` subscript nit). `crd-ceir-invariants` OK. No recook/fuzz/version-bump.

## Named-forwards (the rest of band 13)

- **13b** `ceir.transfer` (copies/upload/readback/clear/mip-gen, 4a effects). **13c** CKIR-by-identity (KernelRef resolution
  through the ADR-0104 cache; interface-hash vs dispatch signature at cook). **13d** the lowering pass (CEIR region →
  canonical command stream, the 4d-derived barriers that NARROW this slice's conservative ambient). **13z** the §129 proof
  (add/reduce/scan/FFT as CEIR assets, Vulkan+DX12) — and the ADR-0108 cornerstone flip.

## Proposed commit (the USER commits — no AI co-author trailer)

```
feat(ceir): CEIR-13a ceir.compute dialect — dispatch ops + the kernel binding contract (§42)

Add engine/ceir/ops/compute.ceirop.toml (compute.dispatch + dispatch_indirect):
grid as dynamic index operands, bindings as the variadic tail (kind derived from
type, slot positional), the kernel referenced by symbol identity (KernelRef
resolution is 13c), and the per-binding access as a {r|w|rw} string. Effects are
a conservative baseline (GPUCommand + ambient MemoryReadWrite; indirect reads its
args) so a dispatch hazards the export of its output; 13d narrows to per-binding.
Context::find_dispatch_misuse enforces the access tokens/arity, binding-is-resource,
grid-is-index, and args-is-buffer. Host-only vocabulary; the gpu-context lowering
is the crd-ceir-gpu bridge (13d).

Gate: 467/467 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator, LLVM-20 tidy, crd-ceir-invariants all clean.
```
