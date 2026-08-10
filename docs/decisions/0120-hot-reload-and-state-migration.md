# ADR-0120 — Hot-reload lifecycle + state migration (asset lifecycle completion)

**Status:** **ACCEPTED** (2026-08-10, under the standing autonomous loop grant [[project_ceir_autonomous_loop_grant]];
design + close advisor-reviewed — five consults across the slice) — the D-007 **CEIR band 10 (Asset lifecycle
completion)**, slice **CEIR-10a**. A `ReloadSet` supervises a set of live CEIR programs and runs the reload lifecycle:
source-in → verify+cook → validate-set → atomic install-or-reject → state migration, with last-good on every failure
and no mixed-generation call graph.
**Phase:** D-007. Resumes the **7c** contract (paused at the 2026-08-09 re-baseline) on the closed 8d/8h/8i
foundation; law: `docs/research/2026-08-09-ceir-universality-review.md` §I (Reactive-UI / Build-pipeline rows), mission
§108 (hot reload) · §109 (state migration). Design substrate: `docs/design/ceir-10a-hot-reload-and-state-migration.md`.
**Tags:** `[ceir]` `[hot-reload]` `[state-migration]` `[lifecycle]` `[cook-bridge]`

---

## 1. Context

Band 7 paused at 7b because 7c's state migration **consumes** stable state identity (a CEIR-8d output — the 7a close
itself recorded "cells are instance-keyed, no stable id") and 7d's plan cache consumes the 8h incremental model.
Building them first guaranteed rework; they resumed as CEIR-10 after foundation closure. 7b shipped the loaded form —
a `RuntimeProgram` `{Module*, content_hash, interface_hash}` in a generation-tagged `ProgramSlot` — but explicitly
BORROWED its Context and deferred ownership / deferred-destroy / the reload driver / state migration to this slice.

The whole lifecycle lives in the **crd-ceir-cook bridge** (crd-ceir stays asset-free, I5): the bridge already links
crd-ceir + crd-render-asset-core + crd-resources + crd-containers. Three foundation slices are the reused substrate:
**8d stable ids** (the migration key — op pointers do not survive a generation swap), **8h `IncrementalDag`** (the
program dependency graph + the §107 dirty rule), and the 7b `RuntimeSlot`/`RuntimeHandle` generation model.

## 2. Decision — a four-stage architecture on one supervisor

`ReloadSet` (`hot_reload.{hpp,cpp}`) owns a set of `Entry`, an `IncrementalDag`, and a `Registrar` callback. The slice
is staged internally (one tracker row, one DoD); each stage is a gated increment.

### 2.1 Stage 1 — the crd-ceir CORE migration surfaces (ADR-scope: crd-ceir, not the bridge)

- **`collect_state_schema(ctx, module, alloc)` → `Array<StateCell{id,type,depth}>`** — every §20 StateEdge cell keyed by
  its 8d STABLE ID, sorted. Extracted from `interface_hash`'s inline walk (ONE source of truth).
- **`contract_hash(ctx, module, scratch)`** — the §107 projection **minus** the state schema (signature + transitive
  effects + capability contract). Because `interface_hash ≡ contract projection + state schema`, `contract_hash`-equal
  WITH `interface_hash`-differ ⇒ ONLY the state schema changed. `interface_hash` was refactored into shared helpers
  **byte-identically** — the §107 four-edit matrix is the regression net, so the cooked format is untouched (no recook).
- **`Interpreter::snapshot_state_by_id` / `restore_state_by_id` + `StateSnapshot{id, ring, pos}`** — the live cell-value
  move by 8d stable id (an absent id or a depth mismatch is skipped → the new generation init-fills).

### 2.2 Stage 2 — the reload supervisor + atomic set-install

- **Ownership (the push-back-UAF scar):** each generation is a HEAP `Generation{Context* ctx; RuntimeProgram program;}`
  unit; the `ProgramSlot` keeps a raw `RuntimeProgram*`, so it must never live by value in a growable `Array<Entry>`
  (growth would dangle the pointer). Install: old `current → zombie`, previous zombie drained (one-deep grace). **A
  Reject destroys the candidate Generation immediately** (never installed → the leak path); last-good = NOT installing
  (no stored blob — persistence is 10b). The destructor + `drain()` free everything.
- ⭐ **The 8h dag IS the validate-set:** `set_revision(asset, content_hash, contract_hash)` — the dag's interface
  revision = **`contract_hash`**, NOT `interface_hash`. Callers depend on the contract, not on a program's internal state
  schema, so a state-schema-only edit must not recompute (reject) dependents. Then the §107 rule (`affected_by`) IS the
  "recompiles-affected" set, unmodified — "the engine is the mechanism" (paired with 9c/9f). The graph is rebuilt
  WHOLESALE on every membership change (edges are arena-strings that die with a retired Context).
- **The 3-way `ReloadDecision`** {NoChange / HotSwap / NeedsMigration / ContractChange}: content equal → NoChange;
  interface equal → HotSwap (install; dependents untouched); contract equal + interface differ → NeedsMigration; contract
  differ → ContractChange. **Reject-always on a contract change** — the supervisor cannot prove a leaf has no callers
  (the host is an unseen caller; EMPTY≠UNKNOWN); a contract change is a COLD reload (remove + re-add). Dup-symbol +
  AssetId-0 are loud rejects; a `Registrar` registers each fresh Context's dialects before `load_program`.

### 2.3 Stage 3 — CALLER-DRIVEN state migration (fork B)

The ReloadSet does **not** own interpreters: live §20 cells are populated by EXECUTION (an `Interpreter::invoke`), which
the supervisor never does, and there may be N execution sessions — so it structurally cannot own "the" state (aligned
with 7b's "Context: BORROW not own" and the Interpreter's "ONE EXECUTION SESSION" contract). Migration is therefore
caller-driven: a **`MigrationFn`** (fn-ptr mutating a `StateSnapshot` array) is registered per-asset
(`register_migration`, last-wins); its PRESENCE flips a NeedsMigration reload from Reject → install (the fn runs
caller-side, never fake-invoked on an empty snapshot). A free **`migrate_state(old_in, new_in, new_module, fn, user)`**
= snapshot → (fn?) → restore; `fn == nullptr` is the verbatim HotSwap path; a refusing fn restores nothing → the new
session init-fills (no install-rollback — a refusing fn should have been unregistered).

### 2.4 Stage 4 — the source-in detect seam + the RAF-11 reentrant guard

- **`add_source` / `reload_source`** — a TRANSIENT cook Context (`cook_program_text`, per-call: construct → registrar →
  cook → destroy; §121 text≡builder) → delegate to `add` / `reload`. Not a separate watcher class — the ReloadSet IS the
  reload authority; source-in is its second entry. This closes the twice-deferred 7a CookDb/`register_cook_handler`
  routing in its host-subset form.
- **Cook failure is a DISTINCT typed outcome** — `AddError::CookFailed` + `cook_error` (never conflated with a load
  failure); a failed cook installs + destroys NOTHING → **last-good keeps running** (the most common real hot-reload
  event).
- ⛔ **The RAF-11 reentrant guard** (`m_reloading` + a RAII `GuardScope`): a mutation called re-entrantly — from inside a
  registrar or migration fn, which the `void* user` can smuggle the set into — is a LOUD typed reject
  (`AddError::Reentrant` / `ReloadResult::reentrant`), never a corrupted entry. Public entries set the guard + delegate
  to guard-free `_impl` cores.

## 3. Consequences

- The full lifecycle works at HOST-SUBSET scale: `test_hot_reload.cpp` (13 `[ceir][reload]`, stages 2–4) +
  `test_reload_migration.cpp` (6, stage 1) = **19** proves body-edit hot-swap + handle staleness, contract-change / needs-migration reject +
  last-good, a real state VALUE surviving a schema change (depth-1→depth-2), bad-source → CookFailed → last-good, the
  affected-set, dup-symbol / AssetId-0 / reentrant rejects.
- The cooked format is **untouched** — no `kBinaryVersion` / `kCeirCookSchema` bump, no recook, no new fuzz corpus.
- The 8d state schema being stable-id-keyed (ADR-0114 §2.7) is what makes cross-generation migration sound; this ADR is
  its first consumer.

## 4. Named-forwards (explicit, honest — validated non-blocking, not built)

- **The mtime/filesystem detect SIGNAL** (a ResourceManager `poll_hot_reload` or a raw watcher calling `reload_source`)
  → production I/O. The seam it invokes is real + tested. ⛔ CEIR owns the swap, never the ResourceManager — its
  per-resource unconditional swap vs CEIR's cross-program validate-set = two swap authorities.
- **`CookedHeader.id` validation** — the cook writes it but `read_program`/`load_program` do not surface it, so the
  supervisor trusts the caller-passed `AssetId`; validating passed-id == declared-id is a future refinement.
- **Leaf-allow for contract changes** (install a contract change with no dependents) → CEIR-11, when the executable form
  can recompile/validate callers.
- **The expected-callee-`interface_hash` serialized record (`KernelRef`)** → CEIR-10b (persisted plan cache) / CEIR-13
  (CKIR asset refs) — no consumer this slice. ⛔ **REFINED at 10b close (ADR-0121):** the IN-MEMORY record landed at 10b
  (the `PlanCache`'s `PlanDep` validity mechanism); the SERIALIZED/persisted form did NOT — it waits for the real plan
  payload (→ CEIR-11+ / CEIR-13), so the format is not designed blind.
- **Semantic call-site compatibility** (beyond a hash compare) → CEIR-3/4.

## 5. Alternatives rejected

- **Fork A — the ReloadSet owns an interpreter per generation** (execution authority): breaks down at N execution
  sessions, gives an idle interpreter with zero invocations (the third-graph scar), and violates the Interpreter's
  one-session contract. → caller-driven (§2.3).
- **`interface_hash`-keyed dependent-safety:** would wrongly REJECT a state-schema-only edit to a program with
  dependents — the exact case `contract_hash` exists to permit. → the dag is keyed on `contract_hash` (§2.2).
- **Full ResourceManager mount + CEIR ILoader:** its per-resource unconditional payload swap conflicts with CEIR's
  cross-program validate-then-install (two swap authorities). → reuse only the detect signal (§2.4).
- **Extending the CDEP serialized record with the expected-callee hash:** a field with no consumer this slice (the
  host-subset gate compares live-vs-new hashes in memory) — the `native_determinism` / third-graph scar. → named-forward.

## Gate

`test_reload_migration.cpp` (6 `[ceir][reload]`) + `test_hot_reload.cpp` (13 `[ceir][reload]`) = **19** `ceir 10a` cases. **377/377 ctest** across
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** + LLVM-20 tidy + GCC `-Werror=switch` + opgen
drift/validator + `crd-ceir-invariants` (U-§116 holds — the stage-2/3/4 work is bridge-only, crd-ceir core touched only
in stage 1, which was gated on that config). **No recook, no fuzz** (the cooked format is untouched). Stages 1–4 landed
as four gated increments (364 → 370 → 373 → 377).
