# CEIR-12b — resource planning-INTENT attribute vocabulary (§20 lifetime / §24 memory-domain / §25 residency)

**Date:** 2026-08-10 · **Slice:** CEIR-12b (D-007 master spine) · **Status:** ✅ CLOSED · **ADR:** none (attr-vocabulary
refinement, consistent with 12a's no-ADR op-vocabulary slice).

## Contract

D-007 CEIR-12b row: *"Lifetime classes + history (`history<T>` binds here) + memory-domain intent attrs + residency
hints"* → §24 §25. This is the ATTRIBUTE-vocabulary slice riding on 12a's `ceir.resource` OP-vocabulary: the §20 lifetime
classes, the §24 memory-domain INTENT, the §25 residency hints, and the export publish-direction — all as declaration-site
attributes on the resource dialect, plus their well-formedness verifier.

## Design (advisor-reviewed at the fork)

**Attributes (opgen TOML), all optional (`required = false`) so builder signatures are unchanged:**

- `resource.declare` (the GRAPH-OWNED, planner-planned resource — the only place planning intent belongs):
  - `lifetime` (string) — CLOSED vocabulary `{transient, persistent, history}` (§20). `history<T>` binds HERE.
  - `history_length` (int) — the history RING depth in frames (≥1; absent under `lifetime=history` ⇒ 1, the TAA
    prev-frame case). ONLY valid with `lifetime=history`.
  - `memory_domain` (string) — CLOSED §24 INTENT `{host, pinned_host, device_local, host_visible_device, unified,
    upload, readback, sparse, external, peer_visible, distributed}`. ⛔ §24's transient/persistent entries ride the
    `lifetime` axis (orthogonal) — NOT duplicated here (every §24 word has ONE named home). INTENT only; providers lower
    to Vulkan memory types / D3D heap flags (NEVER source semantics).
  - `residency` (string) — CLOSED §25 hint `{resident, streamable, evictable}`.
  - `streaming_priority` (int) + `budget_class` (symbol) — OPEN tags (alias_group-style: UNCHECKED by the walk; the 12d
    planner consumes them). The finer §25 machinery (sparse pages, virtual textures, budget classes, prefetch,
    feedback-driven residency) is named-forward to the 12d planner in the TOML dialect summary.
- `resource.export`:
  - `direction` (string) — CLOSED `{read, readwrite}`. ADVISORY: the EFFECT record stays a conservative
    `MemoryReadWrite` (TOML-static, always ordering-relevant); the attr only NARROWS what the 12c hazard walk MAY assume.
    This closes the stale "direction is a 12b/provider refinement" named-forward left in 12a's export TOML.

**Verifier — a SEPARATE module walk `Context::find_resource_intent_misuse` + `ResourceIntentMisuseKind`** (context.hpp /
context.cpp). Deliberately distinct from 12a's `find_resource_misuse` (the CEIR-3c TYPE contract): the intent attrs are a
different layer (§20/§24/§25 planning vocabulary vs typing), and a separate verifier leaves 12a's contractual check order +
its 8 pinned negatives UNTOUCHED (the widen-enum-audit scar). Pre-order walk, identifies ops by `op_name` string (I6-clean,
const-safe). 8 kinds:

- `LifetimeValueInvalid` — `lifetime` present but not a String in the vocabulary. **Wrong VALUE and wrong KIND fold into
  one kind** (the state-depth precedent `dv.kind != Int || dv.i < 1 → StateDepthInvalid` at the top of context.cpp).
- `HistoryLengthInvalid` — `history_length` present but not an Int ≥1.
- `HistoryLengthWithoutHistory` — `history_length` present while `lifetime` ≠ "history" (incl. absent — a distinct code
  path from present-but-transient; both tested).
- `MemoryDomainValueInvalid`, `ResidencyValueInvalid`, `DirectionValueInvalid` — same closed-vocabulary shape.
- `IntentAttrOnImport` — a `resource.import` carries any declare-only planning-intent attr. Imports are NEVER planned, so
  a planning-intent attr there is nonsense-by-construction (the view-of-view legal-by-accident shape — the advisor caught
  this class in 12a; here we forbid it up front).

Per-attr check order on declare is CONTRACTUAL (negatives pin the exact kind): lifetime → history_length(value) →
history_length(without-history) → memory_domain → residency.

## Advisor's five pre-write deltas (all integrated)

1. **export `direction`** minted now (the named-forward pointed at THIS slice) — TOML comment struck-in-place.
2. **§25 open tags** (`streaming_priority`/`budget_class`) pre-minted alias_group-style so 12d mints no vocabulary
   mid-analysis; prefetch/feedback/sparse-pages name-forwarded in the dialect summary.
3. **§24 accounting** — the `memory_domain` doc states transient/persistent ride the `lifetime` axis (no silent subset).
4. **intent-attr-on-import** is a contradiction, not a stray → its own kind + negative.
5. **tests** — wrong-kind fold negative; history_length-with-lifetime-ABSENT (distinct path); positive
   `lifetime=history` with NO `history_length` (defaults to 1).

## Tests (`tests/ceir/test_resource.cpp`, +2 TEST_CASEs, ASCII names)

- *"ceir 12b: declare+export intent attrs are well-formed and survive round-trip"* — a declare carrying all six intent
  attrs + an export with `direction`; both verifiers clean; **BINARY + TEXT round-trip** with **VALUE read-back on BOTH
  twins** (misuse-None alone is too weak — a serializer that dropped BOTH `lifetime` + `history_length` would also read
  clean; the read-back rules that out. The printer/parser is a DISTINCT code path from the binary serializer, so both get
  the read-back, covering all three attr kinds — string/int/symbol).
- *"ceir 12b: find_resource_intent_misuse rejects every malformed intent attr"* — 2 positives (history-defaults-to-1;
  no-attrs) + 9 negatives (one per kind incl. the wrong-kind fold and both HistoryLengthWithoutHistory paths).

## Gate (GREEN)

- **win-debug 451/451 · win-asan 451/451 · linux-gcc-debug 451/451 · linux-gcc-asan 451/451** (was 449; +2 TEST_CASEs).
- opgen regen + `--check` drift-clean + `test_opgen.py` validator OK. GCC clean (covers `-Werror=switch`: the 8-arm
  `resource_intent_misuse_kind_name` switch is exhaustive, no default).
- LLVM-20 tidy clean: `context.cpp`, `context.hpp`, `test_resource.cpp`.
- `crd-ceir-invariants` OK (I3/I5/I6/U-116 — verifier reads op identity via `op_name` string, no switch on op.kind).
- No recook/fuzz/version-bump (host-only IR vocabulary; no cooked-blob or wire-format change beyond additive attrs that
  round-trip through the existing attr machinery).

## Deliberate scope line

Misplacement enforcement is IMPORT-ONLY: a `lifetime` on `resource.view` or a `residency` on `resource.export` passes
the walk silently — only `resource.import` gets `IntentAttrOnImport`. This is a decision, not an oversight: import is
declare's confusable resource-*producing* twin (both define a resource value), so a planning-intent attr there is a
genuine confusion; view/export don't define resources, so a stray intent attr is inert noise rather than a contradiction.
12c/12d may tighten this if the analysis benefits.

## Named-forwards still open (for the band)

- **12c** — alias/lifetime ANALYSIS consumes `alias_group` + the export effect + (advisory) `direction`; ports the frame
  graph's interval model; the WAR + slot-size scars become IR-level ctest cases.
- **12d** — the memory planner consumes lifetime/memory_domain/residency + the open `streaming_priority`/`budget_class`
  tags + the finer §25 machinery (sparse pages, virtual textures, budget classes, prefetch, feedback-driven residency).
- **13+/§150** — providers lower `memory_domain` intent to API memory types / heap flags; bind import/export handles.

## Proposed commit (the USER commits — no AI co-author trailer)

```
feat(ceir): CEIR-12b resource planning-intent attribute vocabulary (§20/§24/§25)

Add the declaration-site intent attrs on the ceir.resource dialect: lifetime
{transient,persistent,history} + history_length (the history<T> ring depth) +
memory_domain (§24 INTENT) + residency (§25 hint) on resource.declare, plus
direction {read,readwrite} on resource.export; streaming_priority/budget_class
are open tags the 12d planner consumes. A separate module walk,
Context::find_resource_intent_misuse, enforces the closed vocabularies (wrong
value or wrong kind folds into one kind per attr) and rejects planning-intent
attrs on resource.import (never planned). Keeps 12a's find_resource_misuse type
contract and its pinned negatives untouched.

Gate: 451/451 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator, LLVM-20 tidy, crd-ceir-invariants all clean.
```
