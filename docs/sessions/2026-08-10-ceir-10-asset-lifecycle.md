# CEIR-10 — Asset lifecycle completion (band-7 remainder, re-homed) — session log

> The autonomous grind ([[project_ceir_autonomous_loop_grant]]) resumed band 7 as CEIR-10 on the closed 8d/8h/8i
> foundation. Design substrate: `docs/design/ceir-10a-hot-reload-and-state-migration.md` (2 advisor consults).
> Master-map: §108 hot-reload · §109 state migration (§110 incremental = 10b).

## CEIR-10a — Hot-reload lifecycle + state migration

Staged internally (ONE tracker row, one coherent DoD): **(1) core exposure → (2) supervisor + set-install →
(3) migration wiring → (4) detect front-end.**

### Stage 1 — the crd-ceir CORE migration surfaces — ✅ DONE + gated (2026-08-10)

**What shipped (crd-ceir core + tests; no bridge code yet).**

- **`collect_state_schema(ctx, module, alloc)` → `Array<StateCell{id,type,depth}>`** (`program_asset.{hpp,cpp}`): the
  module-wide §20 state cells, keyed by 8d STABLE ID, sorted by id — the schema a hot-swap compares to decide migration
  and the list restore walks. Extracted from `interface_hash`'s inline walk (ONE source of truth — `interface_hash` now
  folds the same helper).
- **`contract_hash(ctx, module, scratch)` → u64** (`program_asset.{hpp,cpp}`): the §107 projection **minus** the state
  schema (funcs' signature + transitive effects + the §57 capability contract). The decision-table key: since
  `interface_hash ≡ contract projection + state schema`, `contract_hash`-equal WITH `interface_hash`-differ ⇒ ONLY the
  state schema changed → a migration fn may cover it; a `contract_hash` difference means callers break (Reject).
- ⛔ **`interface_hash` refactored BYTE-IDENTICALLY** into `push_funcs_projection` + `collect_state_cells` +
  `push_caps_projection` helpers. The cooked format is untouched — proven by the pre-existing §107 four-edit matrix
  (`test_program_asset.cpp`, 7 `[program-asset]`) still passing. **No recook.**
- **`Interpreter::snapshot_state_by_id` / `restore_state_by_id`** (`exec.{hpp,cpp}`) + `StateSnapshot{id, ring, pos}`:
  the live cell-value MOVE. Snapshot walks `m_cells` (keyed by op pointer) and emits by 8d stable id; restore walks the
  NEW module's StateEdge ops, matches by id, and seeds `m_cells` with the whole ring+pos — an absent id or a **depth
  mismatch** is SKIPPED (the new generation init-fills — EMPTY≠UNKNOWN). Bundled with the schema exposure so ONE core
  rebuild + re-gate covers both (the advisor's call).
- **3 stale in-place comment fixes** (the 8z/9f discipline — comments must match code, verified against the .cpp):
  `program_asset.hpp` (state schema "BODY order" → 8d stable-id; caps "unbuilt/no owning row" → folded in at 8f);
  `program_asset.hpp` (KernelRef "NAMED-FORWARD to CEIR-10" → **CEIR-13**, pre-renumber); `runtime_program.hpp`
  (executable form "CEIR-8" → **CEIR-11**, pre-renumber).

**The decision table this stage stands up** (used by the stage-2 supervisor):

| old vs new | `contract_hash` | Decision |
|---|---|---|
| `interface_hash` equal | equal | CompatibleReuse |
| `interface_hash` differ | **differ** | Reject (callers break; keep last-good) |
| `interface_hash` differ | equal | Migrate (state-schema-only) if a fn is registered; else Reject |

**Advisor.** Two consults on the design fork (design note). The pivotal call — reconciled AGAINST the advisor's initial
schema-bump prediction: the host-subset gate compares live-vs-new interface hashes in memory, so **no CDEP serialized
record is extended** (no schema bump / recook / fuzz); the `KernelRef` expected-callee-hash stays named-forward to
10b/CKIR-13. The advisor agreed and added the `contract_hash` (sans-state) requirement that stage 1 delivers.

**Tests.** `test_reload_migration.cpp` (6 `[ceir][reload]`): `collect_state_schema` extraction (sorted by id, type +
depth); the 3-way decision table (body-only → both equal · signature → both differ · state-schema-only [ring depth 1 vs
2] → interface differs, contract equal); the cell VALUE migrates across a generation swap by stable id (a control fresh
interpreter re-inits to 0, the restored one continues from the latched value); an absent id + a depth mismatch are
skipped (init-fill, not corruption).

**Gate.** crd-ceir-tests + host + cook **364/364 ctest** (358 + 6) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (all 3 targets rebuilt against the new crd-ceir core; win-asan clean on the new Array/snapshot handling)
+ LLVM-20 tidy (5 files, clean) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants` (the U-§116
enum-pin holds — **zero central-enum edits**). **NO recook, NO fuzz** (the cooked format is untouched — the §107 bytes
are preserved). **NO ADR yet** (ADR-0120 lands at 10a close, per the band-8 precedent; the design note is the substrate).

**Next = stage 2** — the reload supervisor + atomic all-or-nothing set-install (`ceir-cook/hot_reload.{hpp,cpp}`): the
ProgramSlot registry, the 8h program dep graph from CDEP, `validate_set` (dup-symbol reject, interface change-detection,
affected-set), last-good, deferred-destroy drain.

## Proposed commit — CEIR-10a stage 1 (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
feat(ceir-10a): state-migration core -- collect_state_schema + contract_hash + interpreter snapshot/restore

Stage 1 of the CEIR-10a hot-reload lifecycle (design:
docs/design/ceir-10a-hot-reload-and-state-migration.md): the crd-ceir CORE surfaces the reload
supervisor (stage 2) will consume. No bridge/supervisor code yet.

- collect_state_schema(ctx, module, alloc) -> the module-wide (stable_id, type, depth) state cells,
  keyed by 8d stable id, sorted -- the migration schema. Extracted from interface_hash's inline walk
  (one source of truth).
- contract_hash(ctx, module, scratch) -> the sec-107 projection MINUS the state schema (signature +
  transitive effects + capability contract). Distinguishes an unmigratable contract change (Reject)
  from a state-schema-only change (Migrate): interface_hash == contract projection + state schema, so
  contract-equal + interface-differ => only the state schema changed.
- interface_hash refactored BYTE-IDENTICALLY into push_funcs_projection + collect_state_cells +
  push_caps_projection. The cooked format is untouched -- the sec-107 four-edit matrix is the
  regression net. NO recook.
- Interpreter::snapshot_state_by_id / restore_state_by_id + StateSnapshot{id, ring, pos}: the live
  cell-value move by 8d stable id (snapshot m_cells; restore walks the new module's StateEdge ops,
  matches by id, seeds ring+pos). Absent id or depth mismatch => skip -> the new generation init-fills.
- 3 stale in-place comment fixes (comments must match code): program_asset.hpp (state schema BODY-order
  -> 8d stable-id; caps unbuilt -> folded in at 8f), program_asset.hpp (KernelRef CEIR-10 -> CEIR-13),
  runtime_program.hpp (exec form CEIR-8 -> CEIR-11).
- test_reload_migration.cpp (6 [ceir][reload]): schema extraction; the 3-way decision table;
  value migration across a generation swap; absent-id + depth-mismatch skip.

Gated: crd-ceir-tests + host + cook 364/364 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (5 files) + GCC -Werror=switch + opgen drift/validator +
crd-ceir-invariants (U-116 pin holds -- zero central-enum edits). No recook, no fuzz, no ADR (yet).
```

### Stage 2 — the reload supervisor + atomic set-install — ✅ DONE + gated (2026-08-10)

**API-locked first** (design note §8, 3rd advisor consult — the pivotal correction: dependent-safety keys on
`contract_hash`, NOT `interface_hash`, so a state-schema-only edit does not wrongly reject dependents; and
`set_revision(asset, content_hash, contract_hash)` makes the 8h dag the validate-set). Then pure execution.

**What shipped** (`engine/ceir-cook/{include/crd/ceir/cook,src}/hot_reload.{hpp,cpp}` — bridge only; crd-ceir core UNTOUCHED):

- **`ReloadSet`** — owns a set of loaded programs + an `IncrementalDag` + a `Registrar`. Lifecycle:
  `add` / `reload` / `remove` / `drain` + queries `handle` / `is_current` / `program` / `generation` / `affected`.
- **Ownership (the push-back-UAF scar):** each generation is a HEAP `Generation{Context* ctx; RuntimeProgram program;}`
  (placement-new from the supervisor's allocator; the ProgramSlot points at `&gen->program`, so array growth of the
  `Entry` vector never dangles it). Retire: on install, old `current → zombie`, previous zombie drained (one-deep grace);
  **a Reject destroys the candidate Generation immediately** (never installed → the leak path); the destructor + `drain`
  free everything. ASan-clean.
- **The 8h dag IS the validate-set:** `set_revision(asset, content_hash, contract_hash)` (interface rev = `contract_hash`)
  → `affected_by` gives the "recompiles-affected" set unmodified. The graph is rebuilt WHOLESALE on every membership
  change (edges are arena-`StringView`s that die with a retired Context). Edges: each program's `collect_dependencies`
  external `called_funcs` → the program that Publicly exports it.
- **The 3-way `ReloadDecision`** {NoChange / HotSwap / NeedsMigration / ContractChange}, decided by comparing the reloaded
  program's stored hashes vs the candidate's: content equal → NoChange; interface equal → HotSwap (install; dependents
  untouched); contract equal + interface differ → NeedsMigration (stage-2 Reject; stage-3 migrates); contract differ →
  ContractChange (Reject, last-good). **Only HotSwap installs in stage 2.** Reject-always on a contract change (the host
  is an unseen caller — EMPTY≠UNKNOWN; matches the 10z letter).
- **Loud rejects:** dup-symbol at `add` (a candidate exporting a symbol another program already exports); AssetId 0 (the
  dag silently ignores node 0). A `Registrar` callback registers each fresh Context's dialects before `load_program`.

**Boundary (documented, not silently assumed):** `read_program`/`load_program` do NOT surface `CookedHeader.id`, so stage 2
trusts the caller-passed `AssetId`; validating passed-id == declared-id is a future refinement.

**Tests.** `test_hot_reload.cpp` (6 `[ceir][reload]`): body-edit HotSwap + old-handle-goes-stale; signature =
ContractChange reject + last-good (old handle still current, program unchanged); state-schema-only = NeedsMigration
reject; identical reload = NoChange; the dep-graph affected-set + dup-symbol + AssetId-0 reject; `remove` drops from the
set + graph. Host subset — decisions/handles/affected-sets asserted, never cross-program execution.

**Gate.** crd-ceir-tests + host + cook **370/370 ctest** (364 + 6) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (win-asan clean on the heap-Generation ownership) + LLVM-20 tidy (`hot_reload.{hpp,cpp}` +
`test_hot_reload.cpp`, clean) + GCC `-Werror=switch` (the `ReloadDecision`/`AddError` name switches) + opgen
drift/validator + `crd-ceir-invariants` (U-§116 holds — crd-ceir core untouched, a bridge-only change). **NO recook, NO
fuzz** (hot_reload serializes nothing — it consumes existing cooked blobs). A post-tidy identifier rename
(`bSig`→`b_sig`, `bSchema`→`b_schema`) is behavior-preserving, reconfirmed green + tidy-clean on win-debug.

**Next = stage 3** (state-migration wiring: the migration-fn registry + `restore_state_by_id` onto NeedsMigration; the
old generation's Interpreter cells reached via `generation(id)`), then stage 4 (detect via ResourceManager reuse).

## Proposed commit — CEIR-10a stage 2 (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
feat(ceir-10a): reload supervisor -- ReloadSet, the 8h dag as validate-set, atomic set-install

Stage 2 of the CEIR-10a hot-reload lifecycle (design note sec 8). A bridge-only change (crd-ceir core
untouched): the supervisor consumes the stage-1 core surfaces.

- ReloadSet (ceir-cook/hot_reload.{hpp,cpp}): owns a set of loaded programs + an 8h IncrementalDag +
  a Registrar callback. add / reload / remove / drain + handle / is_current / program / generation /
  affected.
- Ownership: each generation is a HEAP Generation{Context*, RuntimeProgram} (the ProgramSlot keeps a
  raw RuntimeProgram* -> never by value in a growable array, the push-back-UAF scar). Install: old
  current -> zombie, previous zombie drained (one-deep). Reject destroys the candidate immediately
  (last-good = not installing). ASan-clean.
- The 8h dag IS the validate-set: set_revision(asset, content_hash, contract_hash) -- interface rev =
  contract_hash (dependent-safety keys on the contract, NOT the internal state schema) -> affected_by
  gives the recompiles-affected set unmodified. Graph rebuilt wholesale per membership change
  (arena-string edge lifetime).
- 3-way ReloadDecision {NoChange/HotSwap/NeedsMigration/ContractChange}: only HotSwap installs in
  stage 2; a contract change is Reject-always (the host is an unseen caller, EMPTY != UNKNOWN).
  Dup-symbol + AssetId-0 are loud rejects; a Registrar registers each fresh Context's dialects.
- test_hot_reload.cpp (6 [ceir][reload]): hot-swap + handle staleness; contract-change reject +
  last-good; needs-migration reject; no-change; affected-set + dup-symbol + AssetId-0; remove.

Gated: 370/370 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (3 files) +
GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants (U-116 holds; bridge-only). No recook,
no fuzz (hot_reload serializes nothing), no ADR (yet -- lands at 10a close).
```

### Stage 3 — state migration (caller-driven) — ✅ DONE + gated (2026-08-10)

**The architecture fork, advisor-resolved (fork B).** The crux: live §20 cells are populated by EXECUTION (an
`exec::Interpreter::invoke`), which the ReloadSet never does — so a lifecycle manager cannot own "the" live state (there
may be N execution sessions; the Interpreter's own contract says "ONE EXECUTION SESSION, construct a fresh one per run";
7b already pinned "Context: BORROW, not own"). Therefore migration is **caller-driven**: the ReloadSet decides + installs
the Module + gates on a registered fn's PRESENCE; the caller (which owns the live interpreter) applies the value-move. The
imprecise §8 design-note line ("reaches the old Interpreter's cells via generation(id)") was struck in place.

**What shipped** (`hot_reload.{hpp,cpp}` — still bridge-only; crd-ceir core UNTOUCHED):

- **`MigrationFn = bool (*)(Array<exec::StateSnapshot>& cells, void* user)`** — transforms the old snapshot IN PLACE to fit
  the new schema (touches no interpreter); returns false = REFUSED. (`exec::StateSnapshot` in a cook-bridge header is fine —
  crd-ceir-cook already links crd-ceir publicly.)
- **`register_migration(id, fn, user)`** (per-asset, last-registration-wins; dropped on `remove`) + **`migration(id)`** query.
- **The gating change is decision-preserving:** a NeedsMigration reload installs IFF a fn is registered (same zombie/drain
  path as HotSwap); `decision` stays `NeedsMigration`, `installed` names the outcome. ⛔ `reload` does NOT call the fn — its
  PRESENCE gates; the fn runs caller-side (no fake invocation on an empty snapshot — the false-green shape avoided).
- **`migrate_state(old_in, new_in, new_module, fn, user, scratch)`** (free fn) = `snapshot_state_by_id(old_in)` → (fn?) →
  `restore_state_by_id(new_in, new_module)`; returns the restored count. `fn==nullptr` = VERBATIM (the HotSwap /
  CompatibleReuse path — same helper). A refusing fn restores nothing → the new session init-fills (state lost but coherent;
  no install-rollback — a refusing fn should have been unregistered; documented).

**Tests** (+3 `[ceir][reload]`, driven by a MOCK RUNTIME that builds interpreters on the set's generation Contexts):
- **The DoD — a real value survives a SCHEMA change:** a depth-1 counter warmed to 7 → `register_migration(expand)` →
  reload the depth-2 version (NeedsMigration + fn present → INSTALLED) → `migrate_state` expands the ring `[7]`→`[7,7]` →
  the new interpreter continues from **7** (a control fresh interpreter re-inits to 0).
- **Refusal → init-fill:** a fn returning false installs (presence gates) but `migrate_state` restores 0 → the new
  generation init-fills to 0, coherent.
- **HotSwap verbatim:** a body-edit HotSwap + `migrate_state(…, nullptr, …)` carries the old cell value forward unchanged.

**Gate.** crd-ceir-tests + host + cook **373/373 ctest** (370 + 3) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (win-asan clean on `migrate_state` snapshotting a zombie-generation interpreter + restoring into the
current) + LLVM-20 tidy (3 files, clean) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants` (U-§116
holds — bridge-only). **NO recook, NO fuzz** (hot_reload serializes nothing). NO ADR yet (ADR-0120 at 10a close).

**Next = stage 4** (the detect front-end — a ResourceManager `subscribe_reload` adapter driving `reload`, the 7a
CookDb/`register_cook_handler` routing), then 10a CLOSE + ADR-0120 describing the settled 4-stage architecture.

## Proposed commit — CEIR-10a stage 3 (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
feat(ceir-10a): caller-driven state migration -- MigrationFn registry + migrate_state

Stage 3 of the CEIR-10a hot-reload lifecycle (design note sec 8-9). Still bridge-only; crd-ceir core
untouched. Architecture fork resolved (advisor): migration is CALLER-DRIVEN -- the ReloadSet does NOT
own interpreters (live sec-20 cells are populated by execution, which the supervisor never does).

- MigrationFn = bool(*)(Array<exec::StateSnapshot>&, void*): transforms the old cell snapshot in place
  to fit the new schema; returns false = refused. register_migration(id, fn, user) per-asset
  (last-wins, dropped on remove) + migration(id) query.
- Gating is decision-preserving: a NeedsMigration reload installs IFF a fn is registered (same
  zombie/drain path). reload does NOT invoke the fn -- its PRESENCE gates; the fn runs caller-side (no
  fake invocation on an empty snapshot).
- migrate_state(old_in, new_in, new_module, fn, user, scratch) [free fn]: snapshot -> (fn?) -> restore;
  fn==nullptr = verbatim (the HotSwap path, same helper). A refusing fn restores nothing -> the new
  session init-fills (coherent; no install-rollback).
- test_hot_reload.cpp +3 [ceir][reload], mock-runtime driven: the DoD (a real value survives a
  depth-1->depth-2 reshape -- 7 carries; control re-inits to 0); refusal -> init-fill; HotSwap verbatim.

Gated: 373/373 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (3 files) +
GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants (U-116 holds; bridge-only). No recook,
no fuzz, no ADR (yet -- lands at 10a close).
```

### Stage 4 — the source-in detect seam + the RAF-11 reentrant guard — ✅ DONE + gated (2026-08-10)

**Advisor-corrected scope.** The framing I brought — "thin ResourceManager adapter vs close with detect named-forward" —
was rejected on both ends. The tracker lifecycle string is "detect→**verify→cook**→…", and stages 1–3 only ever fed
*pre-cooked valid blobs* — the COOK step and the **bad-edit path** (the most common real hot-reload event) were never
exercised, and the 7a CookDb/`register_cook_handler` routing had been deferred TWICE (naming it forward a third time at
close = document-and-accept on a listed row item). So stage 4 is the honest cook step, not a wrapper.

**What shipped** (`hot_reload.{hpp,cpp}` — bridge only; crd-ceir core UNTOUCHED):

- **`add_source(id, source)` / `reload_source(id, source)`** — a TRANSIENT cook Context (`cook_program_text`, per-call:
  construct → registrar → cook → destroy; the cooked bytes are self-contained, §121 text≡builder) → delegate to
  `add` / `reload`. Not a separate watcher class — the ReloadSet *is* the reload authority; source-in is its second entry.
- **Cook failure is a DISTINCT typed outcome** — `AddError::CookFailed` + `cook_error` (and `ReloadResult::cook_error`),
  never conflated with a load failure. A failed cook installs + destroys NOTHING → **last-good keeps running**.
- ⛔ **The RAF-11 reentrant guard** (`m_reloading` + a RAII `GuardScope` clearing on every return path): a mutation
  (`add`/`reload`/`remove` + the source variants) called re-entrantly — from inside a registrar or migration fn, which
  the `void* user` can smuggle the set into — is a LOUD typed reject (`AddError::Reentrant` / `ReloadResult::reentrant`),
  never a corrupted entry. The public entries set the guard + delegate to guard-free `_impl` cores (so a source add/reload
  does not re-trip the guard on its internal call).
- **Named-forward, stated:** the mtime/filesystem SIGNAL that supplies `source` (a ResourceManager `poll_hot_reload` or a
  raw watcher calling `reload_source`) is production I/O — the seam it invokes is now real and tested. ⛔ CEIR owns the
  swap, NEVER the ResourceManager (its per-resource unconditional swap vs CEIR's cross-program validate-set = two swap
  authorities); that is the documented why.

**Tests** (+4 `[ceir][reload]`): source-in lifecycle (a body edit hot-swaps through the source path, old handle stale);
**the headline — a BAD source (syntax error) → `CookFailed`, last-good STILL current + unchanged** (+ `add_source` of bad
text adds nothing); a source signature edit → ContractChange reject; a REENTRANT reload from inside a smuggling registrar
→ rejected (the outer add still succeeds, state uncorrupted).

**Gate.** crd-ceir-tests + host + cook **377/377 ctest** (373 + 4) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (win-asan clean on the transient cook Context placement-new/destroy + the reentrant path) + LLVM-20 tidy
(3 files, clean) + GCC `-Werror=switch` (the widened `AddError`) + opgen drift/validator + `crd-ceir-invariants` (U-§116
holds — bridge-only). **NO recook, NO fuzz** (source cooking produces transient blobs consumed immediately; no new
persisted record). NO ADR yet.

⭐ **ALL 4 STAGES OF CEIR-10a ARE BUILT** — the full lifecycle (source-in → verify+cook → compile-affected [8h] →
validate-set [contract_hash dag] → atomic install → generation bump → deferred destroy → last-good → no mixed-generation
→ caller-driven state migration) + the RAF-11 guard. **Next = 10a CLOSE:** ADR-0120 (the settled 4-stage architecture,
fork B, contract_hash-keyed dependent-safety, the two named-forwards — mtime signal + CookedHeader.id validation); the §7
gate-plan reconcile; a fresh pre-close advisor with the full slice visible; tracker row → ✅; batch commit; then CEIR-10b.

## Proposed commit — CEIR-10a stage 4 (user commits; NO AI trailer; NO ADR yet — 10a close is next)

```
feat(ceir-10a): source-in detect seam + RAF-11 reentrant guard

Stage 4 (final) of the CEIR-10a hot-reload lifecycle (design note sec 8). Bridge-only; crd-ceir core
untouched. The honest cook step + the bad-edit path stages 1-3 never exercised (advisor-corrected scope:
not a wrapper watcher).

- add_source(id, source) / reload_source(id, source): a transient cook Context (cook_program_text,
  per-call construct/cook/destroy; sec-121 text == builder) -> delegate to add / reload.
- Cook failure is a DISTINCT typed outcome: AddError::CookFailed + cook_error (and
  ReloadResult::cook_error), never conflated with a load failure. A failed cook installs + destroys
  nothing -> last-good keeps running (the most common real hot-reload event, finally tested).
- The RAF-11 reentrant guard (m_reloading + RAII GuardScope): a mutation called re-entrantly (from a
  registrar / migration fn, which the void* user can smuggle the set into) is a loud typed reject
  (AddError::Reentrant / ReloadResult::reentrant), never a corrupted entry. Public entries set the guard
  + delegate to guard-free _impl cores.
- Named-forward (stated): the mtime/filesystem signal is production I/O; the seam it invokes is real +
  tested. CEIR owns the swap, never the ResourceManager (two-swap-authority).
- test_hot_reload.cpp +4 [ceir][reload]: source hot-swap; BAD source -> CookFailed + last-good;
  source signature -> ContractChange; reentrant reject via a smuggling registrar.

Closes the twice-deferred 7a CookDb/register_cook_handler routing in its host-subset form. All 4 stages
of 10a are now built (the full lifecycle works).

Gated: 377/377 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (3 files) +
GCC -Werror=switch (widened AddError) + opgen drift/validator + crd-ceir-invariants (U-116 holds;
bridge-only). No recook, no fuzz, no ADR (yet -- lands at 10a close).
```

---

## CEIR-10a — CLOSE (2026-08-10, ADR-0120)

**All four stages built + gated; the full hot-reload lifecycle works at host-subset scale.** The slice resumed the
paused 7c contract on the closed 8d/8h/8i foundation and shipped as four gated increments (**364 → 370 → 373 → 377**):
stage 1 the crd-ceir CORE surfaces, stage 2 the supervisor + the 8h-dag-as-validate-set, stage 3 caller-driven
migration (fork B), stage 4 the source-in seam + the RAF-11 guard. **ADR-0120** records the settled architecture, fork
B and its three priors, the `contract_hash`-keyed dependent-safety, the five rejected alternatives (each with the scar
that killed it), and the five named-forwards.

**The four cross-slice headlines** (the reusable lessons):
1. **The engine IS the mechanism, again:** `set_revision(asset, content_hash, contract_hash)` makes the 8h §107 rule
   the validate-set — `affected_by` is the recompiles-affected set unmodified (paired with 9c/9f). The one correction
   that made it sound: dependent-safety keys on `contract_hash`, not `interface_hash`, so a state-schema-only edit does
   not falsely reject dependents.
2. **A lifecycle manager must not own execution state** (fork B): live §20 cells are populated by execution, which the
   supervisor never does; migration is caller-driven — gate on the fn's PRESENCE, run the fn caller-side.
3. **Ownership follows the raw pointer:** `ProgramSlot` keeps a raw `RuntimeProgram*`, so generations are heap units,
   never by-value in a growable array (the push-back-UAF scar); a Reject destroys the candidate immediately.
4. **The bad-edit path is the real test:** stages 1–3 only ever fed valid blobs; stage 4's `CookFailed`→last-good is the
   most common real hot-reload event, and it (plus the reentrant guard) was the honest content the "close now" framing
   would have skipped.

**Advisor pre-close** caught two ADR misstatements — the Status line borrowing the CEIR-8 foundation-cadence clause (a
scope claim outrunning the grant; CEIR-10 is under the standing loop grant) and a test miscount (`test_hot_reload.cpp`
is 13, not 16; 19 total) — both fixed before the row flipped. (The 9z legend-miscount discipline, applied to the ADR.)

**Boundary, stated plainly:** this is the lifecycle at HOST-SUBSET / proof scale. The §172 full-matrix gate is CEIR-10z
(the band close, after 10b's plan cache — cache-hit counts are 10b/10z scope). The mtime/filesystem detect signal is
production I/O (named-forward). 10a's own DoD — a live hot-swap works end-to-end — is delivered + gated.

**Next = CEIR-10b** — the execution-plan cache keyed by (content hash × target × compiler version) through the 8h model
(compiled artifacts are caches, never truth); the likely home of the twice-deferred expected-callee `KernelRef` record.
Re-read ADR-0118's CookDb named-forward before designing; advisor on the design fork.

## Proposed BATCH commit sequence — CEIR-10a (user commits; NO AI trailer; ordered)

The four stage commits are drafted in their sections above; apply them in order, then a final docs/close commit:

1. `feat(ceir-10a): state-migration core -- collect_state_schema + contract_hash + interpreter snapshot/restore` (stage 1)
2. `feat(ceir-10a): reload supervisor -- ReloadSet, the 8h dag as validate-set, atomic set-install` (stage 2)
3. `feat(ceir-10a): caller-driven state migration -- MigrationFn registry + migrate_state` (stage 3)
4. `feat(ceir-10a): source-in detect seam + RAF-11 reentrant guard` (stage 4)
5. the close:

```
docs(ceir-10a): CLOSE -- ADR-0120, the settled 4-stage hot-reload architecture

- ADR-0120 (hot-reload lifecycle + state migration): the 4-stage architecture on one ReloadSet, fork B
  (caller-driven migration), contract_hash-keyed dependent-safety, 5 rejected alternatives + 5
  named-forwards. Accepted under the standing autonomous loop grant; design + close advisor-reviewed.
- D-007 tracker: CEIR-10a row -> CLOSED (stage progression 364->370->373->377, the named-forward list,
  the ADR pointer); band header 10a done / 10b NEXT.
- design note sec 7 reconciled to the delivered reality (tag [ceir][reload], the 3-way ReloadDecision,
  the sec-172 matrix re-homed to 10z); the imprecise stage-3 line struck in place.
- context.md + this session log close section.

CEIR-10a is CLOSED: the full lifecycle (source-in -> verify+cook -> validate-set -> atomic
install/reject -> caller-driven migration -> last-good -> no mixed-generation) + the RAF-11 guard, at
host-subset scale. 19 ceir-10a cases, 377/377 x 4 configs. No recook, no fuzz.
```

---

## CEIR-10b — Execution-plan cache — IMPLEMENTATION DONE + gated (2026-08-10); close (ADR-0121) pending

**Advisor fork (pre-design consult).** I brought a scope worry: no compiler exists (CEIR-11) and the compile→plan
interface is explicitly deferred to CEIR-21/26 (`provider.hpp:36`), so a fully-mock plan cache risks the zero-consumer
scar. The advisor's verdict: **build it — but as a STORE + VALIDATE-ON-HIT client of live truth, not a compile
pipeline** (drop the compile callback) and not deferral (the 10z gate row asserts cache-hit counts → the band contract
requires the cache before 10z; re-sequencing a user-authored row is never-defer). The artifacts are mock, but the
invalidation semantics have a REAL driver today — the 10a ReloadSet's reload events — and the twice-deferred KernelRef
record gets its first consumer as the validity mechanism (the 9x pattern: mock payload, real mechanism).

**What shipped** (`ceir-cook/plan_cache.{hpp,cpp}` — bridge-only; crd-ceir core UNTOUCHED):

- **`PlanCache`** — a content-addressed store keyed by `PlanKey{content_hash, target, compiler_version}` → an opaque
  artifact + recorded `PlanDep`s. `get(key)` → **Hit / Miss / StaleDeps**: a Miss when no entry; a Hit only when the key
  matches AND every recorded dep still validates against **live truth** (a resolver fn-ptr → the dep's current
  `interface_hash`); else StaleDeps (the stale entry is dropped — lazy eviction). `put(owner, key, artifact, deps)`
  copies both into cache-owned HEAP buffers (never interior pointers into the growable entry vector — the 10a UAF
  discipline). `evict(owner)` / `clear()`; first-class `hits()` / `misses()`.
- **The seven advisor pins, all honored:** (1) no compile callback — the caller produces + `put()`s (fork-B reapplied:
  store/gate ≠ production); (2) deps key on **`interface_hash`, NOT `contract_hash`** — cache-validity wants
  conservatism (a stale plan is corruption; a spurious recompile is cheap — the 7a under/over-inclusion asymmetry); a
  test comment notes a NeedsMigration callee install over-invalidates the caller, deliberate + safe; (3) **no second
  dag** — a client of the ReloadSet's 8h dag via the resolver; ⛔ a resolver 0 is STALE (EMPTY≠UNKNOWN); (4) **in-memory
  only** — no serialized cache file (the payload doesn't exist; the format would be designed blind), which ⛔ REFINES the
  10a named-forward ("KernelRef record → 10b *persisted* plan cache"): the record lands now (in-memory validity), the
  serialized form → 11/13 (strike-in-place at close); (5) bridge-local `PlanDep{AssetId, u64 interface_hash}`, NOT a
  crd-ceir `KernelRef` mint (I5 asset-free; ≡ ADR-0109 §85); (6) hit/miss counters first-class (10z); (7) key `target` =
  FNV of the `IExecutionProvider::name()` (the CapabilityId pattern).

**Tests.** `test_plan_cache.cpp` (3 `[ceir][plancache]`, mock resolver): put/get Hit + Miss on unknown-key / version-bump
/ new-target + two targets coexist for one content; ⭐ **§107 extended to plans** — a callee body-edit (interface
unchanged) leaves the caller's plan a HIT, a callee interface change → StaleDeps, a removed callee (resolver 0) →
StaleDeps; `clear()`/`evict()` drop everything (the "never truth" falsifiable test — all-miss → re-put → re-hit).
`test_hot_reload.cpp` +1 `[ceir][plancache]`: ⭐ **the 10z REHEARSAL** — resolver backed by a live ReloadSet; a callee B
cold-reloaded with a new signature stales exactly `affected(B) ∪ {B}` (A's plan → StaleDeps, B's plan → Miss by new
content, an independent C → still HIT).

**Gate.** crd-ceir-tests + host + cook **381/381 ctest** (377 + 4) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (win-asan clean on the PlanCache heap-buffer ownership — allocate/free, swap-remove, clear/evict) +
LLVM-20 tidy (`plan_cache.{hpp,cpp}` + both tests, clean after a `depA`→`dep_a` LocalConstant fix) + GCC `-Werror=switch`
+ opgen drift/validator + `crd-ceir-invariants` (U-§116 holds — bridge-only). **NO recook, NO fuzz** (in-memory; nothing
serialized). NO ADR yet.

**Next = 10b CLOSE:** ADR-0121 (the store+validate-on-hit design, the resolver-as-live-truth, the interface_hash-keyed
deps, the persistence named-forward that refines the 10a KernelRef forward); strike the 10a named-forward in place; a
fresh pre-close advisor with the full slice; tracker row → ✅; batch commit. Then CEIR-10z (the §172 band-close matrix,
which inherits the pairing rehearsal).

## Proposed commit — CEIR-10b implementation (user commits; NO AI trailer; NO ADR yet — close is next)

```
feat(ceir-10b): execution-plan cache -- PlanCache, a validate-on-hit client of live truth

CEIR-10b (design note ceir-10b-execution-plan-cache.md). Bridge-only; crd-ceir core untouched. A
STORE + VALIDATE-ON-HIT cache, NOT a compile pipeline (advisor fork): no compiler exists (CEIR-11) and
the compile->plan interface is deferred to 21/26, so the cache stores opaque caller-produced artifacts
and validates them against live truth on get().

- PlanCache (ceir-cook/plan_cache.{hpp,cpp}): content-addressed store keyed by
  PlanKey{content_hash, target, compiler_version} -> opaque artifact + PlanDeps. get() = Hit/Miss/
  StaleDeps: a Hit only when the key matches AND every recorded dep validates against a resolver fn-ptr
  (the dep's current interface_hash; 0 => stale, EMPTY != UNKNOWN) -> the cache never trusts itself.
  put() copies into cache-owned heap buffers (no interior pointers into the growable vector).
  evict(owner)/clear(); first-class hits()/misses().
- Deps key on interface_hash (NOT contract_hash) -- cache-validity wants conservatism (a stale plan is
  corruption; a spurious recompile is cheap). No second dag: a client of the ReloadSet's 8h dag via the
  resolver. In-memory only (persistence -> 11/13). Bridge-local PlanDep (== ADR-0109 sec-85 KernelRef,
  not a crd-ceir mint; I5 asset-free).
- test_plan_cache.cpp (3 [ceir][plancache]): put/get/miss + version + two-targets; sec-107 extended to
  plans (callee body-edit -> HIT, interface change -> StaleDeps, removed -> StaleDeps); clear/evict
  never-truth. test_hot_reload.cpp +1: the 10z rehearsal (a callee interface change stales exactly
  affected(B) union {B}).

Gated: 381/381 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator + crd-ceir-invariants (U-116 holds; bridge-only). No recook,
no fuzz (in-memory), no ADR (yet -- lands at 10b close).
```

---

## CEIR-10b — CLOSE (2026-08-10, ADR-0121)

**ADR-0121** records the settled design: a **store + validate-on-hit client of live truth** — the caller compiles an
opaque artifact + `put()`s it, `get()` returns it only after validating every recorded dep against the live resolver
(the cache never trusts itself). The §2.3 lesson is the headline: **cache-validity keys CONSERVATIVELY on
`interface_hash`; reload-reject keys PRECISELY on `contract_hash`** — the same 7a under/over-inclusion asymmetry applied
to a new pair (a stale plan is corruption; a spurious recompile is cheap). Five rejected alternatives, each with its
scar; the persistence named-forward (11/13) refines the 10a KernelRef forward.

**The 10a persistence forward, struck in place** (the superseded-clause discipline — annotate, never delete): ADR-0120
§4 + the 10a design note (V1 + §6) now state that 10b's cache is IN-MEMORY (the `PlanDep` validity mechanism landed; the
serialized/persistent form waits for the real plan payload → CEIR-11+/13, so the format is not designed blind).

**Advisor pre-close** confirmed ADR-0121 clean (counts verified: 3 `test_plan_cache.cpp` + 1 pairing = the 4/4 run,
377+4=381; Status line uses the corrected grant formula, not the CEIR-8 cadence clause; the three strikes follow the
in-place discipline) but blocked on **four doc-vs-reality drifts in the 10b design note** (the class the 10a §7 reconcile
existed for), all reconciled (doc-only, gate stays valid): (1) the pairing rehearsal lives in `test_hot_reload.cpp`, not
`test_plan_cache.cpp`; (2) the stale set is `affected(B)` [StaleDeps] ∪ {B} [**Miss** by new content, NOT StaleDeps] —
the delivered test was right, the note's phrasing wrong; (3) the "reload B contract-change → still HIT" scenario is a
LIVE property not asserted in 10b — handed to the 10z matrix; (4) there is **no `PlanStatus` switch** (the note claimed
`-Werror=switch` guards one). The missing `plan_status_name` is now DOCUMENTED as deliberate (PlanStatus is a
programmatic result, not a diagnostic surface — a name fn would be zero-caller), not silently absent.

**No memory file this close** — the conservative/precise validity-vs-reject lesson is fully in ADR-0121 §2.3; the memory
discipline says don't duplicate what the repo records. MEMORY.md untouched.

**Boundary:** the cache is IN-MEMORY at host-subset scale; its artifacts are opaque (the real producer is CEIR-21/26's
compile→plan interface, the real consumer CEIR-11+). What 10b establishes is the invalidation MECHANISM, driven live by
the 10a ReloadSet. **Next = CEIR-10z** — the §172 band-close matrix, a COMPOSING test (unlike 9z's doc-synthesis: the
band's lifecycle + cache guarantees only partially meet today) with hit-count-delta assertions across the three edit
classes; closing it closes BAND 10.

## Proposed BATCH commit sequence — CEIR-10b (user commits; NO AI trailer; ordered)

The implementation commit is drafted in the 10b implementation section above; apply it, then the close:

1. `feat(ceir-10b): execution-plan cache -- PlanCache, a validate-on-hit client of live truth` (implementation)
2. the close:

```
docs(ceir-10b): CLOSE -- ADR-0121, the validate-on-hit plan-cache design

- ADR-0121 (execution-plan cache): a store + validate-on-hit client of live truth; no compile callback
  (the compile->plan interface stays deferred to 21/26); deps key on interface_hash (conservative --
  cache-validity vs reload-reject's contract-hash precision, the 7a asymmetry); no second dag (a client
  of the ReloadSet's 8h dag via a resolver); in-memory (persistence -> 11/13); bridge-local PlanDep.
  5 rejected alternatives. Accepted under the standing autonomous loop grant; design + close
  advisor-reviewed.
- The 10a "KernelRef -> 10b PERSISTED plan cache" named-forward REFINED / struck in place (ADR-0120 sec-4
  + the 10a design note): the in-memory PlanDep record landed at 10b; the serialized form -> 11/13.
- design note (ceir-10b) reconciled to the delivered slice (4 doc-vs-reality fixes: the pairing test's
  home, the stale-set = affected(B)[StaleDeps] + {B}[Miss], the contract-change scenario handed to 10z,
  no PlanStatus switch); plan_status_name documented as deliberately omitted.
- D-007 tracker: CEIR-10b row -> CLOSED (ADR-0121 pointer, the refined persistence forward); band
  header 10a+10b done / 10z NEXT. context.md + this session log close section.

CEIR-10b is CLOSED: the plan-cache mechanism, driven live by the 10a ReloadSet. 381/381 x 4 configs.
No recook, no fuzz (in-memory).
```

---

## CEIR-10z — the BAND-10 GATE — ✅ CLOSED 2026-08-10 → ⭐⭐ BAND 10 CLOSED

**A COMPOSING gate, not doc-synthesis.** Unlike 9z (whose domains were each already a composition, so its gate was
synthesis), band 10's two guarantees — the 10a hot-reload lifecycle and the 10b plan cache — only MEET here: the PlanCache
validates against a resolver backed by the LIVE ReloadSet, so a reload DECISION and a cache VERDICT are checked in
lockstep. `test_band10_gate.cpp` (1 `[ceir][gate10]`, tag; TEST_CASE name "ceir 10z: BAND-10 GATE …") walks ONE program
graph — A (imports "bar") → B (exports bar()), plus independent C — through the three §172 edit classes:

1. **body-edit B → HotSwap installs LIVE, and caller A's plan stays a HIT** — the §107 hot-swap property proven at the
   PLAN layer against the live resolver (B's interface unchanged → A's recorded `PlanDep` still validates); B's OWN plan
   self-misses (new content = new key).
2. **signature-edit B → ContractChange REJECT + last-good, and A's plan STILL HITS** — a rejected reload leaves B's live
   interface unchanged, so the cache stays fully valid (a reject invalidates nothing).
3. **dep-edit (cold-reload B with a new signature) → B's interface changes → A's plan StaleDeps** (A must recompile),
   B self-misses = the recompile set `affected(B) ∪ {B}`; C stays a HIT throughout (∉ affected(B)).

⛔ **EXACT hit/miss counter deltas at every step** (the 9a "not ≥" discipline): `3/0 → 6/1 → 8/1 → 9/3` — these ARE the
§172 "cache-hit counts". The gate is the band's tightest artifact: every `get()` is enumerated, so a double-increment
counter bug fails it (a `>` would have passed).

**Re-verification (the 8z discipline):** the gate re-verifies the 10a/10b public surfaces against code by EXERCISING
them live (real `reload` decisions, real `PlanCache` verdicts) — the composing gate is itself the row re-verification.

**Advisor pre-close** caught two defects, both fixed before the flip: (1) the "∪ {B}" half of the dep-edit row was
unasserted (step 3 checked A→StaleDeps + C→Hit but never B's self-miss at new content — internally doc-vs-code
inconsistent on its own headline); (2) weak `hits() > hits0` / `misses() >= 2` violated the exact-counts discipline →
replaced with exact per-step assertions (I traced the counters against the code: baseline 3/0, step 1 6/1, step 2 8/1,
step 3 9/3 — all confirmed by the passing test). Also self-corrected mid-tick: the ctest name-vs-tag scar (`-R "gate10"`
found nothing — the tag isn't the name; `-R "ceir 10z"` is correct) and a LocalConstant tidy fix (`A/B/C/depA` →
`id_a/id_b/id_c/dep_a`). After both fixes: a FULL 4-config re-run (not win-debug-only — the band gate closes on the final
source).

**Gate.** crd-ceir-tests + host + cook **382/382 ctest** (381 + 1) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (clean) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants` (U-§116
holds — bridge-only). **NO recook, NO fuzz, NO ADR** (a gate composes ADR-0120 + ADR-0121 — the 3z/4z/5z/8z/9z precedent).

## BAND 10 — summary (Asset lifecycle completion)

The paused band-7 remainders (7c/7d/7z) resumed VERBATIM as CEIR-10a/10b/10z on the closed 8d/8h/8i foundation — the
2026-08-09 re-baseline's promise kept (foundation first, then the consumers that needed it). The arc:
- **10a (ADR-0120)** — the full hot-reload lifecycle on a `ReloadSet` (crd-ceir-cook bridge), 4 gated increments
  (364→370→373→377): core surfaces (`collect_state_schema` + `contract_hash`) → supervisor (the 8h dag AS the
  validate-set) → caller-driven migration (fork B) → source-in seam + the RAF-11 guard.
- **10b (ADR-0121)** — the execution-plan cache: a store + validate-on-hit client of live truth (381).
- **10z** — the composing gate (382).
**The two reusable lessons (both in the ADRs, [[feedback_lifecycle_manager_must_not_own_execution_state_fork_b]] in
memory):** (1) a lifecycle manager must not own execution state — gate on fn presence, run caller-side (fork B); (2)
cache-validity keys CONSERVATIVELY (`interface_hash`), reload-reject keys PRECISELY (`contract_hash`) — the 7a
under/over-inclusion asymmetry applied to a new pair. **Six named-forwards** landed honestly (mtime signal, CookedHeader.id
validation, leaf-allow, compile-affected, KernelRef in-memory-now/serialized-later, semantic call-site) — three converge
at CEIR-11. **Whole band bridge-only except 10a stage 1** (crd-ceir core touched once, gated on all 4 configs). NO recook,
NO fuzz across the entire band (nothing new serialized). **Next = CEIR-11** (reference executor + compiled host plan).

## Proposed BATCH commit — CEIR-10z + BAND 10 CLOSE (user commits; NO AI trailer; ordered)

```
test(ceir-10z): BAND-10 GATE -- lifecycle + plan cache composed live across the 3 edit classes

The band-10 close gate (sec-172 matrix, host subset). A COMPOSING test (not doc-synthesis): the 10a
hot-reload lifecycle and the 10b plan cache meet here -- the PlanCache validates against a resolver
backed by the LIVE ReloadSet, so a reload decision and a cache verdict are checked in lockstep.

- test_band10_gate.cpp (1 [ceir][gate10]): one program graph (A imports B; C independent) walked
  through the 3 edit classes: (1) body-edit B -> HotSwap live AND caller A's plan stays a HIT (sec-107
  at the plan layer; B self-misses by new content); (2) signature-edit B -> ContractChange reject +
  last-good AND A still HIT (a reject invalidates nothing); (3) dep-edit (cold-reload B) -> A StaleDeps,
  B self-miss = the recompile set affected(B) + {B}, C untouched.
- EXACT hit/miss counter deltas at every step (3/0 -> 6/1 -> 8/1 -> 9/3) -- the sec-172 cache-hit counts.

Gated: 382/382 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator + crd-ceir-invariants (U-116 holds; bridge-only). No recook,
no fuzz, no ADR (a gate composes ADR-0120 + ADR-0121).
```
```
docs(ceir-10z): BAND 10 CLOSED -- asset lifecycle completion

- D-007 tracker: CEIR-10z row -> CLOSED; CEIR-10 band header -> BAND 10 CLOSED (10a..10z); CEIR-11 NEXT.
  The band-contract "reload transactions ride 8i" phrase struck in place (corrected at 10a: install
  atomicity is the 7b slot; 8i is the upstream authoring seam).
- context.md + the session log band-10 summary (the arc, the two reusable lessons, the six
  named-forwards, three converging at CEIR-11).

BAND 10 CLOSED: the paused band-7 remainders (7c/7d/7z) resumed verbatim as 10a/10b/10z on the closed
8d/8h/8i foundation. 382/382 x 4 configs. No recook, no fuzz across the band.
```
