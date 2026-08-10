# CEIR-12z — the BAND-12 gate: the resource/memory subsystem closes

**Date:** 2026-08-10 · **Slice:** CEIR-12z (D-007 master spine) · **Status:** ✅ CLOSED · **Closes:** CEIR-12 (band). ADR:
none (a gate composes, it does not decide).

## Contract

D-007 CEIR-12z row: *"Gate: lifetime + aliasing corpus green incl. ported scar cases; `transient_memory <
transient_logical` asserted on an aliasable module (REN-1 proof, IR edition)."* → §36 §78. The band's falsifiable claim:
the resource→memory pipeline (dialect → intent attrs → analysis → planner) COMPOSES on one realistic module, and aliasing
provably saves memory.

## A COMPOSING gate (the band8 pattern), not a re-run of slice tests

`tests/ceir/test_band12_gate.cpp` — ONE curated frame-graph-shaped module (`build_band12`, a documented position table)
carrying every band axis at once: `resource.declare`/`view`/`import`/`export` (12a — the import pins "the planner plans
declare's, NEVER import's" on the flagship module) with the §20/§24/§25 planning-intent attrs
(12b); a WAR-lifetime resource `%war` declared FIRST but used LAST **through a `resource.view` chain** + a direct read
(the 12c use-based-lifetime scar, exercised on the composed module — the advisor's required addition, so the gate composes
12a's central op and the twin carries a View-typed result end-to-end); three disjoint same-bucket transients that pool;
`%big` in a different size bucket (the slot-SIZE scar); a `history` (depth 2), a `persistent`, and an exported resource
(the three dedicated classes). The band's guarantees are then composed IN SEQUENCE on that same module:

1. **12a + 12b** — `find_structure_error` / `find_resource_misuse` / `find_resource_intent_misuse` all `None`.
2. **12c** — `compute_block_lifetimes`: `%war` extends to its LAST use (pos 19, through the view chain), NOT declaration
   order; the short transients are disjoint.
3. **12d** — `plan_block_memory(Memory)`: `transient_logical == 5`, `transient_physical == 3` (⭐ the REN-1 proof — the
   `{%a,%b,%c}` collapse; **exact counts** asserted per the advisor, so a halved-pooling regression FAILS the gate, not just
   a bare `<`). `%war` (spans them) and `%big` (different bucket) keep their own slots — the two scars. `%hist` dedicated
   with `history_length == 2` recorded, `%persist` dedicated, `%out` `DedicatedExported`; the §162 SlotReasons (`NewPoolSlot`
   / `Pooled` + `prior`) pinned.
4. **Latency cross-check** — `plan_block_memory(Latency)`: `physical == logical == 5` (aliasing is what saved the memory).
5. **Consistency invariant** — every co-slotted pair satisfies `resources_may_alias`.
6. **Round-trip** — a serialize→deserialize twin re-runs both verifiers `None` and RE-PLANS in a fresh Context; the full plan
   SHAPE reproduces per declaration-INDEX (slot + `SlotReason` + `prior`-null-ness — ⛔ `prior` compared by null-ness only,
   never the cross-context `Value*`), which is strictly stronger than the `5/3` counts (⭐ the lifetime + size_class intent
   attrs survived — else the poolable set would collapse to 0). The import op excluded from planning in both contexts.

## Gate (GREEN)

- **win-debug 461/461 · win-asan 461/461 · linux-gcc-debug 461/461 · linux-gcc-asan 461/461** (was 460; +1 gate case).
- opgen drift/validator OK; LLVM-20 tidy clean (`test_band12_gate.cpp`); `crd-ceir-invariants` OK. Test-only slice — NO
  engine code changed (a gate that FORCED an engine change would be a 12a–d defect to surface, not patch — none did).

## CEIR-12 CLOSED — the band summary

The resource/memory subsystem is complete as an analysis+planner pipeline:
- **12a** — the `ceir.resource` dialect (declare/view/import/export) + `find_resource_misuse` (the CEIR-3c type contract).
- **12b** — the §20/§24/§25 planning-intent attrs (lifetime/history/memory_domain/residency/size_class/direction) +
  `find_resource_intent_misuse`.
- **12c** — `compute_block_lifetimes` + `resources_interfere`/`resources_may_alias` (the effect-derived live-range analysis).
- **12d** — `plan_block_memory` → the inspectable `MemoryPlan` (interval-coloring; ADR-0124).
- **12z** — this composing gate.

**Named-forwards out of the band** (for the consuming slices): §15d frame-compile consumes `plan_block_memory`; the CFG
lands cross-block/loop-carried ranges; providers (§150/13+) lower `memory_domain` intent to API memory + realize history
rings + turn `size_class` buckets into bytes ("why 64 MB").

**NEXT = CEIR-13** (compute + transfer — first GPU contact). Re-read the CEIR-13a row from the tracker at open time (it
likely reaches beyond crd-ceir core into provider/bridge work — the recon scope changes).

## Proposed commit (the USER commits — no AI co-author trailer)

```
test(ceir): CEIR-12z band gate — the resource/memory pipeline composes, aliasing saves memory

Add tests/ceir/test_band12_gate.cpp: ONE frame-graph-shaped module composes
12a-d in sequence — the type + intent verifiers are clean, the WAR-lifetime
resource extends to its last use through a resource.view chain, and the planner
pools three disjoint same-bucket transients into one slot (transient_physical 3 <
logical 5, the REN-1 proof) while the WAR resource, the differently-sized
transient, and the history/persistent/exported resources each keep their own
slot. Latency cross-check (physical == logical), the co-slotted consistency
invariant, and a serialize->deserialize re-plan (reproducible cross-context)
close the band. CEIR-12 (resource/memory subsystem) is complete.

Gate: 461/461 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator, LLVM-20 tidy, crd-ceir-invariants all clean.
```
