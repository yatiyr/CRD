# CEIR-13d part 1a — the `crd-ceir-gpu` lowering bridge is born (scaffold + ADR-0125)

**Date:** 2026-08-10 · **Slice:** CEIR-13d (D-007 master spine) · **Status:** ✅ **CLOSED — parts 1a (scaffold + ADR),
1b (dispatch lowering + barriers), 2 (transfer + ambient-narrowing), 3 (view→hazard retrofit + FLIP) all DONE + gated
(481/481 × 4 configs)** · **ADR:** **0125**.

## Contract

D-007 CEIR-13d row: *"Lowering pass: CEIR region → canonical command stream with 4d-derived barriers (incl. the upload
barrier BY CONSTRUCTION)."* → §158 §79. The biggest opening of the detour: the `crd-ceir-gpu` bridge module **did not
exist** (checked: `engine/ceir-gpu` absent, not in root/tests CMake). Advisor-reviewed at the decomposition fork; split
into checkpoints (the 13c two-tick precedent).

## The decomposition (advisor)

- **Fork 2 (load-bearing):** the lowering is a PURE VALUE-LIST function `lower_region(ctx, block) → Array<LoweredCommand>` —
  a CEIR-side, inspectable command list; execution against a live device is a SEPARATE step (13z). ⛔ NEVER drive an
  `IComputeContext` from the lowering (compile ≠ run, §158; untestable without a GPU). Handles stay UNRESOLVED (the 13c
  KernelRef + CEIR `Value*`s) — they bind to a gpu-context `DispatchDesc` at execute (the CKIR-by-identity seam, 13z).
- **Fork 1 (split):** **1a** = scaffold + ADR (this); **1b** = dispatch lowering + hazard-derived barriers (barriers stay
  in tick 1 — a lowering with no ordering violates the band's headline scar; barriers = walk op pairs, emit one between any
  non-None `ops_hazard` — conservative, no transitive reduction [the §79 scheduler's job]); **2** = transfer lowering + the
  upload-barrier test + the 13a ambient-narrowing (coupled); **3** = the 12c view→hazard retrofit + flip.
- **Dependency narrowing (ADR-0125 §2.1):** ADR-0109 §4.2 lists 4 deps; tick-1 lowering needs only `crd-ceir` +
  `crd-gpu-context` — render-graph/kir edges added WHEN CONSUMED (a zero-include edge is the acyclic gate's dead-edge smell).

## part 1a — what landed

- **ADR-0125** — the bridge births; the pure-value-list contract (the plan-producing-tier shape — why 13d takes an ADR
  where 13a–c did not); unresolved handles bind at execute; conservative hazard-derived barriers; the dep narrowing.
- **`engine/ceir-gpu`** (`crd-ceir-gpu`) — a new static lib, the `crd-ceir-host` template, links `crd-ceir` +
  `crd-gpu-context` (+ core/containers/memory). Wired into root CMake (after ceir-cook) + `tests/CMakeLists`.
- **`crd/ceir/gpu/lower.hpp`** — the `LoweredCommand` value type (`LoweredKind{Dispatch, Barrier}`; a dispatch carries the
  resolved grid + `crd::gpu::DispatchKind` [the live bridge edge] + an `op` back-pointer for execute-time resolution; a
  barrier carries the `HazardKind` + the ordered ops) + the `lower_region` signature. ⛔ the exact field set is finalized
  WITH the lowering logic (1b) — this is the scaffolded contract.
- **`lower_region` stub** — clears `out` (an empty list); the module is born + links + the signature is pinned.
- **`tests/ceir-gpu/test_lower.cpp`** — the module links + `lower_region` yields the contract (empty list) + the value
  types are usable. ⛔ **`gate6b.sh` target list updated** (the advisor's warning — it hardcodes targets, else the Linux
  gate silently never builds `crd-ceir-gpu-tests`; the sandbox-false-green shape).

## Gate (part 1a, GREEN)

- **win-debug 476/476 · win-asan 476/476 · linux-gcc-debug 476/476 · linux-gcc-asan 476/476** (was 475; +1 smoke). The WSL
  gate RECONFIGURED + built `crd-ceir-gpu-tests` (now in gate6b.sh); `-R "ceir"` picks it up on both platforms.
- LLVM-20 tidy clean (`lower.hpp`, `lower.cpp`, `test_lower.cpp`). `crd-ceir-invariants` OK — ⭐ **I5 confirms crd-ceir
  CORE stays host-only** despite the new bridge (only `crd-ceir-gpu` link-edges gpu-context). opgen drift/validator OK.

## part 1b — the dispatch lowering + barriers (DONE + gated)

`lower_region` filled: it walks the block, emits a `Dispatch` command per `compute.dispatch`/`dispatch_indirect` and, before
each, ONE `Barrier` carrying the strongest incoming `ops_hazard` from any earlier dispatch (reverse scan, strict-greater keep
⇒ the NEAREST strongest-hazard source — deterministic + the best §162 diagnostic). Grid resolves ALL-OR-NOTHING from the
three `arith.const` operands (via `Value::defining_op()`); a zero group is lowered AS-IS; any non-const ⇒ `dynamic_grid` with
groups untouched; Indirect never resolves a grid (`dynamic_grid` stays false — it is not a failed resolution). Non-dispatch
ops (Pure consts/declares) emit nothing; ⛔ a walk comment marks that the `earlier` hazard set WIDENS to transfer commands
at part 2. Ops identified by `op_name` (const — keeps `lower_region` taking `const Context&`, the find_dispatch_misuse
precedent). `test_lower.cpp` +2 TEST_CASEs (structure + one-barrier-per-dispatch + the nearest tie-break + dispatch-free
empty; grid const/zero/dynamic/indirect; determinism).

⛔ **The `LoweredCommand`↔`DispatchDesc` fork — DECIDED (a primary-source divergence from the advisor's decomposition
checklist, surfaced + confirmed):** the `LoweredCommand` is **CEIR-native** (grid values + `op` back-pointer for the
unresolved kernel/bindings), NOT a `DispatchDesc` wrapper, and `validate_dispatch` runs at EXECUTE (13z), not as a lowering
test. Evidence: `validate_dispatch` returns `NullProgram` on a null `kernel` (command_model.cpp:253) — the lowering has no
device program, so a lowering-time desc could never pass it. The advisor's "validate_dispatch passes on every emitted desc"
line was self-inconsistent (it also said the pointer binds at execute); ADR-0125 §2.3 already recorded the correct design
("validate_dispatch runs on the resolved desc at execute"). The lowering is a faithful TRANSLATOR, not a verifier
(`find_dispatch_misuse` is the IR verifier; it checks grid TYPES, not values — so ZeroDraw is 13z's, not the lowering's).

## Gate (part 1b, GREEN)

- **win-debug 478/478 · win-asan 478/478 · linux-gcc-debug 478/478 · linux-gcc-asan 478/478** (was 476; +2 lowering tests).
- LLVM-20 tidy clean (`lower.hpp`, `lower.cpp`, `test_lower.cpp`); `crd-ceir-invariants` OK; opgen drift/validator OK.

## part 2 — transfer lowering + the ambient narrowing (DONE + gated)

`lower_region` now handles `ceir.transfer` ops (`LoweredKind::Transfer` + our OWN `LoweredTransferKind` — the 5 SHIPPED 13b
ops copy/upload/readback/clear/mip_gen, ⛔ NOT `crd::gpu::TransferKind` which subsets upload/readback/mip_gen; a Clear's fill
word resolved for §162) and, crucially, **narrows the dispatch barrier derivation**: a new bridge-side `gather(op) → precise
accesses` (a dispatch's bindings + `access` tokens; every other op's static per-operand effects via `ctx.op_effects` +
`effect_access`) + `precise_hazard` (mirrors `accesses_conflict`/`pair_hazard`/`strongest_hazard` — cited). Barriers now
derive from PRECISE accesses, so the **upload→first-read barrier is DISCRIMINATING** (present iff the dispatch binds the
uploaded buffer, absent for a disjoint resource) and disjoint dispatches stop barriering. `test_lower.cpp` rewritten (+3
TEST_CASEs; the part-1b binding-less "→ barrier" asserts correctly FLIPPED to no-barrier — the coupling's churn).

⛔ **Two rationale corrections (advisor pre-write, source=scoreboard):**
1. **The coupling reason is NOT "test churn" (as ADR-0125 §2.5 / the part-1a plan said) — it is FALSE-GREEN avoidance.**
   Decoupling (transfer lowering conservative, narrowing later) would ship the headline upload-barrier test passing via the
   dispatch's AMBIENT rw — a test that goes green whether or not the upload's `Write{buf}` is ever consulted (the
   sandbox-false-green / pixel-blind scar shape: zero discriminating power). The narrowing is what makes the scar test MEAN
   something. So they land together — for correctness of the test, not to save a rewrite.
2. **Two hazard notions BY DESIGN — do NOT "unify" them.** The core dispatch op's declared effects (GPUCommand + ambient
   MemoryReadWrite) and `Context::ops_hazard` stay CONSERVATIVE (any core consumer is safe; 13a's dispatch-vs-export ambient
   test still holds). ONLY the bridge's barrier derivation is narrowed. A future reader making core `ops_hazard` precise
   would break 13a's test — a comment at `precise_hazard` + this paragraph pin it.

**The four correctness pins (advisor, each tested):** (1) EMPTY≠UNKNOWN — an UNREGISTERED op ⇒ whole-Memory rw in `gather`;
(2) a MALFORMED/absent `access` ⇒ DEGRADE to ambient (conservative, never precise-but-wrong — standalone-robust below
`find_dispatch_misuse`); (3) GPUCommand is ordering-INERT in `gather` (submission/stream order is the executor's — disjoint
dispatches emit no barrier, "no barrier ≠ no order"); (4) the **12c VIEW HOLE** — `dispatch binds view(%buf)` vs
`upload(%buf)` = distinct Values → no conflict → no barrier, pinned as DOCUMENTED CURRENT BEHAVIOR (part 3 retrofits the
view→root map core + bridge in ONE move).

## Gate (part 2, GREEN)

- **win-debug 480/480 · win-asan 480/480 · linux-gcc-debug 480/480 · linux-gcc-asan 480/480** (was 478; +2 net TEST_CASEs).
- LLVM-20 tidy clean (`lower.hpp`, `lower.cpp`, `test_lower.cpp`); `crd-ceir-invariants` OK (crd-ceir CORE unchanged — the
  narrowing is bridge-only); opgen drift/validator OK.

## part 3 — the 12c view→hazard retrofit + FLIP (DONE + gated — the slice's END)

The 12c view hole (struck-in-place at 12c/13b/13d-part2) CLOSED across CORE + bridge in ONE move, as contracted:

- **`Context::resource_root(const Value*)`** (new `const noexcept` member) — follows `resource.view` chains to the underlying
  resource via `Value::defining_op()` (ONE hop per 12a: a view's `operand(0)` is the source), loop-guarded (64) against a
  malformed cycle. Returns `v` unchanged for a non-view or block arg (`defining_op()==nullptr`). ⛔ IDENTITY only — the view's
  byte RANGE is not resolved (disjoint views collapse to the root ⇒ conservative over-conflict, refinable). Mirrors the exact
  one-hop rule the 12c `compute_block_lifetimes` root map already uses (context.cpp:1765).
- **CORE normalize** — `op_access_at` now does `res = ctx.resource_root(res)` before capturing the access, so `ops_hazard` /
  `accesses_conflict` / `collect_block_hazards` all see through views. The context.hpp `ops_hazard` SUPERSEDED clause +
  the `accesses_conflict` comment struck in place (the hole is now a false-negative NO MORE).
- **BRIDGE normalize** — `gather` normalizes every captured resource (dispatch bindings, the indirect args buffer, and static
  per-operand effects) through `ctx.resource_root` too, so the bridge barriers see the same aliasing. The two hazard notions
  stay distinct BY DESIGN — only the *resource identity* is shared; the core op's ambient effect vs the bridge's precise
  gather are still two things (13a's dispatch-vs-export ambient test still holds).
- **Tests** — core: `test_hazard.cpp` +1 TEST_CASE (`write(buf)` vs `read(view(buf))` → RAW/WAR now; a view over a DIFFERENT
  buffer → None [precise, not blanket]; two distinct views of one buffer → hazard). Bridge: the part-2 view-hole pin in
  `test_lower.cpp` FLIPPED from "no barrier" to "Barrier(Raw)" and retitled (no longer a conservative pin — it's the
  closed-hole proof). ⛔ a view laundered through a region-yield/call-result STILL escapes (its `defining_op` is not a
  `resource.view`) — a deeper inter-op alias hole, named-forward (documented at context.hpp/lower.cpp/ADR-0124/ADR-0125).

**Gate (part 3, GREEN):** win-debug **481** · win-asan **481** · linux-gcc-debug **481** · linux-gcc-asan **481** (was 480;
+1 core hazard test, the bridge test flipped in place). LLVM-20 tidy clean (context.cpp, context.hpp, lower.cpp,
test_hazard.cpp, test_lower.cpp); `crd-ceir-invariants` OK (⭐ crd-ceir CORE stays host-only/jobs-free/asset-free — the
retrofit is a pure hazard-walk refinement, no new deps); opgen drift/validator OK; `-Werror=switch` clean (the retrofit is
switch-free — `resource_root` is a string-compare walk). **Advisor pre-close CONFIRMED** the design + the source=scoreboard
named-forward sweep (ADR-0124 §3, ADR-0125 §2.5/§3, the tracker 12c-row note all struck LANDED).

## 13d CLOSED — what comes next

CEIR-13d ✅ (all four checkpoints). Band-13 header stays ◧ — **13z** (the §129 execution proof: add/reduce/scan/FFT as CEIR
assets executed through the lowering, Vulkan+DX12 — ADR-0108 cornerstone flip) and any 13e remain. The lowering is the clean
seam 13z executes through.
