# CEIR-12d — the memory planner (interval-coloring port; inspectable plan) — §78 / §162

**Date:** 2026-08-10 · **Slice:** CEIR-12d (D-007 master spine) · **Status:** ✅ CLOSED · **ADR:** **0124** (this slice — see
"ADR" below).

## Contract

D-007 CEIR-12d row: *"Planner interface + first planner (interval-coloring port); plan inspectable (§162 explainability:
'why 64 MB')."* → §78 §162. The consumer of CEIR-12c's live-range analysis: assign each graph-owned resource a physical
**slot**, pooling transients whose ranges don't overlap — the frame graph's greedy aliaser (frame_graph.cpp:1185–1242),
ported to the IR, with an inspectable output.

## ADR — 0124 (this slice took one; 12c did not)

The advisor's discriminating call: 12c was a pure *analysis* (mirrors CEIR-4d's no-ADR hazard pass); 12d is a
**plan-producing tier with a downstream-consumed contract** — the §15d row pins "Frame compile → CEIR scheduling + **the
CEIR-12 planner**", a cross-band interface. That is the CEIR-11b `CompiledExecutionPlan` shape, which took **ADR-0123**. So
12d takes **ADR-0124** (contract + profile table + poolable gates + named-forwards). ⛔ **README not updated:** the
`docs/decisions/README.md` table stops at 0110 — ADRs 0111–0123 (the whole D-007 detour, incl. 0123) are absent from it, so
the detour's convention is that the tracker row + the ADR file are the index, not the README. Backfilling 14 entries is
out-of-scope for this slice; 0124 follows 0111–0123's precedent. (Primary-source deviation from the advisor's "update
README" — the README does not track this detour's ADRs.)

## Design (advisor-reviewed at the fork; all four corrections integrated)

**Home** — struct+enum+decl in `context.hpp` near `ResourceLifetime`; impl in `context.cpp` next to
`compute_block_lifetimes`. ⛔ NOT `plan.hpp` (that's CEIR-11b's `CompiledExecutionPlan` — "plan" is overloaded here). Home
matches the 12c pattern.

**Output — the inspectable `MemoryPlan`** (§162): `slots[]` (`MemorySlot{kind, size_class, dedicated, first, last,
occupant_count, history_length}`), `assignments[]` (declaration order, `SlotAssignment{resource, slot, reason, prior}`), and
`transient_logical`/`transient_physical` counts.

**The `SlotReason` enum — explain the NEGATIVE decisions too** (the advisor's §162 point: §162 says "explain decisions", and
`aliases[]`-only would answer "why do R and Q share?" but not "why does X have its own slot?" — the actual "why 64 MB"):
`Pooled` (shares a prior disjoint resource's slot; `prior` set), `NewPoolSlot`, `DedicatedLifetime`, `DedicatedExported`,
`DedicatedUnsized`, `DedicatedProfile`. Folded the advisor's separate `aliases[]` into `SlotAssignment.prior`. (One
conscious divergence: the advisor listed `DedicatedNoFit`; in pure interval-coloring a poolable resource that fits no
existing slot opens a fresh SHAREABLE slot — it isn't *dedicated* — so that case is `NewPoolSlot`, named honestly.)

**Algorithm** — poolable-eligible resources (the `resources_may_alias` unary gates) processed in `(first asc, decl-index
asc)` order; first-fit into a same-`(size_class, kind)`-bucket non-dedicated slot whose end `< first`, else a fresh
shareable slot. Everything else → dedicated. **Optimality claim FIXED** (the advisor caught it): 12c's `first` = the
declare position, appended in walk order, so the lifetimes are ALREADY start-sorted by construction — the sort is a GUARD
for a future first-*use* semantics, NOT a scoreboard win over the frame graph. Source=scoreboard.

**`history_length`** (the advisor's conscious-decision point): a `history<T>` gets ONE dedicated slot (the ring's
realization is provider machinery, named-forward — don't multiply the slot count), but the ring DEPTH is recorded on the
slot (`MemorySlot.history_length`) — that IS its "why 64 MB" (a depth-2 ring is 2× memory).

**Profiles (§78)** — `aliasing = profile != Latency`. Real today: `Memory` (max pooling) vs `Latency` (no pooling —
preserve parallelism; its physical == Memory's logical, a built-in cross-check). `Balanced` (parallelism-aware) +
`Deterministic` (cross-run stability) ride Memory's already-deterministic behavior — named-forward. One name switch, all
four arms (`-Werror=switch`); the plan records the requested profile.

**Metrics** — `transient_logical`/`transient_physical` count ONLY poolable-eligible resources (per the advisor: exported/
unsized are dedicated in both worlds, belong in neither number). `physical ≤ logical`, strict iff pooling happened.

**Consistency invariant** — every co-slotted pair satisfies `resources_may_alias` (holds by construction: the poolable
gates equal the predicate's unary gates, the fit test equals its interval check) — pinned by a test walking all pairs.

**Out-param** — `plan_block_memory(const Block&, PlanProfile, MemoryPlan& out)` — the hazard/lifetime collector pattern.

## Tests (`tests/ceir/test_planner.cpp`, +5 TEST_CASEs, ASCII — the advisor's checklist)

- the **REN-1 proof** (aliasable module → `physical < logical`; `%b` Pooled with `prior==%a`).
- **minimal coloring** (`A=[0,5], B=[1,2], C=[3,4]` → exactly 2 slots, `C` Pooled with `prior==B`, `A`≠`B` slot) + the
  **consistency invariant** (all co-slotted pairs `resources_may_alias`).
- **Latency** disables pooling (physical == logical == Memory's logical; both `DedicatedProfile`).
- **every dedicated reason** (persistent→Lifetime, history→Lifetime+ring depth 2, **a default-depth history→depth 1**
  [12b's TAA contract], exported→Exported, size 0→Unsized) + the transient counters stay 0.
- **bucket isolation** (disjoint but different `size_class` → 2 slots) + **determinism** (plan twice → identical
  assignments) + **Balanced/Deterministic ride Memory** (byte-identical assignments — pins the ADR claim) + name smoke.

## Gate (GREEN)

- **win-debug 460/460 · win-asan 460/460 · linux-gcc-debug 460/460 · linux-gcc-asan 460/460** (was 455; +5).
- opgen regen (TOML dialect-summary "the memory planner is CEIR-12d" amended in place) + `--check` drift-clean + validator
  OK. GCC clean (`-Werror=switch`). LLVM-20 tidy clean: `context.cpp`, `context.hpp`, `test_planner.cpp` (fixed a
  local-constant `kNone`→`no_slot` naming nit). `crd-ceir-invariants` OK. No recook/fuzz/version-bump.

## Named-forwards

- **12z** (the band GATE, next) — the lifetime+aliasing corpus green incl. the ported scar cases; `transient_physical <
  transient_logical` on an aliasable module (12d already proves it — 12z asserts it across the corpus + closes the band).
- **§150 / 13+** — bytes: `size_class` is a bucket, the "64 MB" figure is a provider lowering; history-ring realization.
- **CFG** — cross-block / loop-carried live ranges (per-block today, like the hazard/lifetime analyses).

## Proposed commit (the USER commits — no AI co-author trailer)

```
feat(ceir): CEIR-12d memory planner — interval-coloring over the 12c analysis (§78/§162)

Add Context::plan_block_memory: color a block's resource live ranges into
physical slots (greedy interval-coloring within a (size_class, kind) bucket,
the frame graph's aliaser ported to the IR) and return an inspectable MemoryPlan
— per-slot occupancy + a per-assignment SlotReason (§162 explainability) +
transient_logical/physical counts (the REN-1 aliasing-saves-memory proof).
The §78 PlanProfile selects Memory (alias) vs Latency (dedicate, preserve
parallelism); Balanced/Deterministic ride Memory (named-forward). Records a
history<T> ring's depth on its dedicated slot. ADR-0124.

Gate: 460/460 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator, LLVM-20 tidy, crd-ceir-invariants all clean.
```
