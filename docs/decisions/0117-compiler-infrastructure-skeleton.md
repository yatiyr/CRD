# ADR-0117 — Compiler-infrastructure skeleton: analysis/pass managers, rewrite/conversion, diagnostics

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8g**. Lands the MLIR-class compiler-infrastructure
FRAMEWORKS — an `AnalysisManager` (registered analyses + never-stale invalidation), a `PassManager`, a
RewritePattern/ConversionTarget skeleton, and a `DiagnosticEngine` — with ⛔ ZERO consumers beyond the skeleton
(optimization passes + real lowering stay in CEIR-26/27; the 6c/7a/8e no-consumer rule).
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (rows "Analysis/pass framework",
"Rewrite/legalization", "Diagnostics"); mission §65/§72–74/§102.
**Tags:** `[ceir]` `[compiler]` `[analysis]` `[pass]` `[rewrite]` `[diagnostics]` `[foundation]`

---

## 1. Context

CEIR has op/type/attr/effect/interface machinery but no compiler DRIVER layer: no way to register an analysis and
reuse its result, no pass sequencing, no rewrite/legalization framework, and only `ParseResult{ok, offset, msg}` for
errors — no structured diagnostics with stable codes, severity, provenance, notes, or fix-its. U-§72 also warns that
`Context` must not become a god-object; these frameworks must be SEPARATE, not more Context members.

## 2. Decision

### 2.1 One shared constexpr FNV; open-world FNV ids

⛔ **Hoist a single `fnv1a_ct(const char*)`** (id.hpp) — the compile-time FNV-1a byte-identical to
`containers::fnv1a_64` (which takes a `void*` and is not constexpr over a literal). `make_interface_id` (8e),
`make_analysis_id`, and `make_diagnostic_code` all call it — three copy-pasted constexpr FNVs would silently drift
from `hash_string` and drift means `T::kId != intern(name)` (the 8d one-shared-routine discipline applied to
hashing). `AnalysisId` and `DiagnosticCode` are FNVs of a name (open-world — a plugin registers its own analysis/code
with ZERO central edits), each with a `kId == intern(name)` pin test (the 8e shape). `DiagnosticCode` carries a
reverse-lookup NAME table (the `capability_name` shape) — a rendered diagnostic prints the NAME, not a u64.

### 2.2 AnalysisManager (U-§73) — preserved-sets only, ⛔ NO dependency tracking

A registered analysis = an `AnalysisId` + a compute fn returning an arena result (`const T*`, the 8e cast pattern);
`get<T>()` computes-or-returns-cached. **Never-stale invalidation:** a pass declares a **preserved-analyses set**;
after it runs, the manager EVICTS every cached analysis NOT preserved. ⛔ **The slice's biggest trap, refused:** an
analysis that reads another analysis raises transitive staleness — the reflex fix is inter-analysis dependency edges,
which would be a NEW dependency graph ONE SLICE before CEIR-8h exists to UNIFY the ≥2 the tree already holds (the
band's own anti-pattern, timed maximally badly). So the skeleton is **preserved-sets ONLY**; inter-analysis
dependencies are a documented **pass-author contract** ("preserving B while invalidating an analysis B read is a
contract violation") + an explicit **named-forward to 8h**. (a) Results are arena-allocated; eviction LEAKS into the
arena — accepted (the grow-by-rebuild precedent), documented. (b) The pass's run fn returns `changed`; **unchanged ⇒
preserve-all regardless of the declared set** (a pass that did nothing invalidates nothing). The compute-COUNTER test
proves both directions: a non-preserving pass ⇒ recompute (never-STALE); a preserving/unchanged pass ⇒ no recompute
(never-REDUNDANT).

### 2.3 PassManager (U-§65) — thin sequencer

A `Pass` = a name (StringView, for diagnostics — no pass ids; nothing queries passes by id) + a run fn
(`bool(Context&, Module&, DiagnosticEngine&)`, returning `changed`) + a preserved-analyses set.
`PassManager::add_pass` + `run(ctx, module, analysis_manager, diagnostics)` sequences the passes, drives the §2.2
invalidation after each, and ⛔ **STOPS the pipeline when a pass emits a Fatal** (`diag.has_fatal()` — a later pass
must not run on a fatally-broken IR; this is the one real semantic hook the skeleton has, and it is why a pass takes
the `DiagnosticEngine`). A no-op/identity pass is the only non-fatal test consumer (FRAMEWORK).

### 2.4 Rewrite / ConversionTarget skeleton (U-§67/U-§74) — data + caller-driven apply, driver RESERVED

A `RewritePattern` = a `match` hook (does it apply to this op?) + a `rewrite` hook (apply it). A `ConversionTarget`
declares per-op-kind legality (`Legal` / `Illegal` / `Dynamic` + a predicate) with `is_legal(op)`; ⛔ an UNLISTED
kind ⇒ **Illegal** (the EMPTY≠UNKNOWN default — an unknown op must not read as legal). ⛔ **The apply is CALLER-DRIVEN
per-op** — `try_apply(pattern, op)` (match then rewrite on THIS op), NOT a block-walking loop: a walk that rewrites
while iterating is the iterator-invalidation trap, and solving it (worklists, traversal order, fixpoint) IS the
CEIR-26 greedy driver — a "trivial walk" would smuggle half of it in. The real driver is RESERVED (named-forward).

### 2.5 DiagnosticEngine (U-§102) — one surface, text COPIED

A `Diagnostic` = a `DiagnosticCode` (stable, §2.1) + a `Severity` (Note/Warning/Error/Fatal — a small CLOSED enum,
the reasoning axis; widen-audit applies if extended) + a `SourceLoc` (1c provenance) + a message + notes + fix-its.
`DiagnosticEngine::emit(...)` collects them; `render(...)` prints code-name + severity + `file:line:col` + message
(reusing `Context::file_path`). `Fatal` is WIRED, not just an enum value: `has_fatal()` drives the §2.3 pipeline stop.
⛔ The code's reverse-lookup NAME rides EACH diagnostic (a copied `code_name`), NOT a separate intern registry — that
meets the render-the-name requirement without a name table no consumer queries (an intern registry is unbuilt by
design; §2.1's "reverse-lookup" is per-diagnostic). ⛔ **The lifetime landmine:** messages/notes arrive as `StringView`s from callers who
may have built them in dying buffers — the engine **COPIES all text into its own storage** (alloc-outlives-borrowers,
the existing memory scar). Pinned by an ASan probe: emit from a scope-local buffer, read the diagnostic after the
scope dies. It is the ONE structured error surface for text/visual/agent/CLI (the fix-it + note structure is why —
`ParseResult`'s flat message cannot carry them).

## 3. The recook story — ZERO motion

Every framework is RUNTIME state (managers constructed over a Context/Module; diagnostics collected in-memory).
Nothing serializes: no `kBinaryVersion`, no `kCeirCookSchema`, no interface/content-hash change, no recook. ⛔ ZERO
Context members added (the managers are separate classes) — but the ceir.hpp umbrella gains the new headers, so all 3
targets rebuild and the full 4-config gate still runs.

## 4. Consequences

- `Context` stays lean (frameworks are separate classes); 8h's incremental model has an `AnalysisManager` to build on;
  CEIR-26/27 have a `PassManager`/rewrite skeleton to fill; every band gets one structured `DiagnosticEngine`.
- Analysis-result eviction leaks into the arena (documented; a Context is a bounded build lifetime).
- Inter-analysis dependency tracking is deliberately absent — named-forward to 8h (NOT a third dependency graph here).
- The rewrite DRIVER is reserved — CEIR-26 (no-consumer rule).

## 5. Alternatives rejected

- **Managers as Context members** — the god-object U-§72 forbids; separate classes.
- **Copy-pasted per-id constexpr FNVs** — silent drift from `hash_string`; one shared `fnv1a_ct`.
- **Inter-analysis dependency edges in the AnalysisManager** — a new dependency graph one slice before 8h unifies
  them; preserved-sets + a documented contract instead.
- **A block-walking rewrite driver** — the iterator-invalidation/worklist problem IS CEIR-26; caller-driven per-op
  apply keeps it reserved.
- **A closed diagnostic-code enum** — a plugin needs its own codes; FNV open-world (Severity stays a closed axis).
- **Extending `ParseResult` for diagnostics** — it cannot carry codes/severity/notes/fix-its; a dedicated engine.

## 6. Test matrix

Analysis/pass (`test_pass_manager.cpp`, `[analysis][pass]`): an analysis computes once + caches (a compute-COUNTER);
a non-preserving pass forces recompute (never-stale); a preserving pass — or one that returns unchanged — skips
recompute (never-redundant); `preserve_all`/`preserve_none`; a two-pass sequence drives invalidation correctly;
`make_analysis_id` FNV == intern. Rewrite (`test_rewrite.cpp`, `[rewrite]`): `try_apply` matches + rewrites one op,
declines a non-matching op; `ConversionTarget` legal/illegal/dynamic, and an UNLISTED kind is Illegal (EMPTY≠UNKNOWN).
Diagnostics (`test_diagnostic.cpp`, `[diagnostic]`): emit + collect by severity; `render` prints code-name +
`file:line:col` (via file_path); a code's FNV == intern + reverse-lookup name; ⛔ the text-COPY ASan probe (emit from
a scope-local buffer, read after it dies); a Fatal short-circuits `has_errors`.
