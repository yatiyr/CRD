# ADR-0124 — The memory planner (CEIR-12d): interval-coloring over the §26/§78 lifetime analysis, an inspectable plan

**Status:** **ACCEPTED** (2026-08-10, under the standing autonomous loop grant [[project_ceir_autonomous_loop_grant]];
design + close advisor-reviewed — a consult at the design fork and pre-close) — the D-007 **CEIR band 12 (Resource/memory
subsystem)**, slice **CEIR-12d**. The memory planner: `Context::plan_block_memory` colors the CEIR-12c live-range analysis
into physical slots and returns an inspectable `MemoryPlan`.
**Phase:** D-007. Law: §78 (memory planning + profiles) · §162 (explainability) · §26/§4d (the effect-derived analysis it
consumes). Inbound: CEIR-12c (`compute_block_lifetimes` + `resources_may_alias`). Outbound (pinned): §15d "Frame compile →
CEIR scheduling + **the CEIR-12 planner**" — a cross-band, downstream-consumed contract (why this slice takes an ADR where
the 12c *analysis* did not — the CEIR-11b `CompiledExecutionPlan` precedent, ADR-0123).
**Tags:** `[ceir]` `[resource]` `[planner]` `[memory]` `[explainability]`

---

## 1. Context

CEIR-12c produces, per graph-owned resource, a live range `[first, last]` over a block's op order plus the
`resources_may_alias` predicate (both explicit Transient, non-exported, same non-zero `(size_class, kind)` bucket,
disjoint ranges). 12d is the *consumer*: it assigns each resource a **physical slot**, pooling transients whose ranges do
not overlap — the frame graph's greedy interval-coloring aliaser (frame_graph.cpp:1185–1242), ported to the IR. The output
must be **inspectable** (§162 "why did this resource allocate 64 MB?") — the plan is not a black box; every assignment
carries its reason.

## 2. Decision — the `MemoryPlan` contract + the first planner (one tracker row)

### 2.1 The output contract (inspectable — §162)

`plan_block_memory(const Block&, PlanProfile, MemoryPlan& out)` — the out-param house pattern of the hazard/lifetime
collectors. `MemoryPlan`:
- `slots[]` — each `MemorySlot{kind, size_class, dedicated, first, last, occupant_count, history_length}`. `dedicated` ⇒
  never shared. `history_length > 0` records a `history<T>` ring's depth (its memory MULTIPLE — the §162 answer for a
  history resource; the ring's *realization* is a provider concern, named-forward — the slot count is NOT multiplied).
- `assignments[]` — in resource DECLARATION order, `SlotAssignment{resource, slot, reason, prior}`. `reason` (`SlotReason`)
  EXPLAINS the decision — the negative reasons as much as the positive (§162 "explain decisions, not only failures"):
  `Pooled` (shares a prior disjoint resource's slot; `prior` = that resource), `NewPoolSlot` (poolable but opened a fresh
  shareable slot), `DedicatedLifetime` / `DedicatedExported` / `DedicatedUnsized` / `DedicatedProfile`.
- `transient_logical` / `transient_physical` — count ONLY poolable-eligible resources (Transient ∧ non-exported ∧ non-zero
  size_class): logical = the slots they'd need WITHOUT aliasing, physical = the slots they actually use. **`physical ≤
  logical`, strict iff any pooling happened** — the REN-1 "aliasing saves memory" proof, IR edition (asserted at 12z).

### 2.2 The first planner — greedy interval-coloring

Poolable-eligible resources (the `resources_may_alias` unary gates) are processed in `(first asc, decl-index asc)` order;
first-fit into a same-`(size_class, kind)`-bucket, non-dedicated slot whose current end `< first` (disjoint), else a fresh
shareable slot. Everything else gets a dedicated slot. First-fit on a **start-sorted** stream is provably minimal (colors =
max concurrent live = the interval graph's clique number). ⛔ **Not a claimed improvement over the reference:** 12c's
`first` is the declare position and the lifetimes array is appended in walk order, so the input is ALREADY start-sorted by
construction — the explicit sort is a GUARD for a future first-*use* semantics, not a scoreboard win over the frame graph.

**Consistency invariant (planner ↔ predicate):** every co-slotted pair satisfies `resources_may_alias` — holds by
construction (the poolable gates equal the predicate's unary gates; the fit test equals its interval check), pinned by a
test that walks all co-slotted pairs.

### 2.3 The §78 profile semantics (which are real TODAY)

| `PlanProfile` | This slice | Named-forward |
|---|---|---|
| `Memory` | ✅ aggressive interval-coloring (max pooling) | — |
| `Latency` | ✅ pooling DISABLED — a dedicated slot per resource (preserves parallelism; §78 "do not optimize memory if aliasing destroys parallelism") | — |
| `Balanced` | rides `Memory` | a parallelism-aware cost model (alias only where it doesn't serialize independent work) |
| `Deterministic` | rides `Memory` (already deterministic — sorted, stable tie-break, plan-twice-identical is tested) | a cross-run stability *guarantee* across input perturbations |

`aliasing = profile != Latency`; every profile produces defined behavior. The `PlanProfile`/`SlotReason` name switches are
total (⛔ `-Werror=switch` guards a future append). The plan records the *requested* profile.

## 3. Scope / named-forwards

- **Bytes** — `size_class` is an opaque planner bucket, not bytes; the "64 MB" figure is a provider concern (§150 / 13+).
  12d answers "N physical slots in bucket B; resource R shares slot S with Q because their ranges are disjoint."
- **Cross-block / loop-carried ranges** — per-block, like the hazard/lifetime analyses; the CFG lands with a later slice.
- **History realization** — the ring's storage/rotation is provider machinery; 12d records the depth, does not multiply slots.
- **Hazard-side view aliasing** — ✅ CLOSED in CEIR-13d part 3: the 4d `ops_hazard` walk now normalizes each captured
  resource through `Context::resource_root` (the 12c view→root chain), so `write(%buf)` vs `read(view(%buf))` hazards.
  Orthogonal to the planner; a view laundered through a region-yield/call-result still escapes (a deeper alias hole).

## 4. Consequences

The band's resource→memory pipeline is complete as an analysis+planner: dialect (12a) → intent attrs (12b) → live-range
analysis (12c) → **planner (12d)**. CEIR-12z (the band gate) asserts the lifetime+aliasing corpus green including the
ported scar cases and `transient_physical < transient_logical` on an aliasable module. §15d (the frame-graph band) consumes
`plan_block_memory` when frame compilation moves onto CEIR.

Gate: **460/460 × 4 configs** (win-debug/win-asan/linux-gcc-debug/linux-gcc-asan); opgen drift/validator, LLVM-20 tidy,
`crd-ceir-invariants`, `-Werror=switch` all clean. Tests: `tests/ceir/test_planner.cpp` (the physical<logical proof; the
minimal-coloring + consistency invariant; Latency == the un-aliased baseline; every dedicated reason + the history depth;
bucket isolation + plan determinism).
