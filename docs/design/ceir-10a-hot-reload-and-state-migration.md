# CEIR-10a — Hot-reload lifecycle + state migration — DESIGN NOTE

> Status: **DESIGN LOCKED (2026-08-10), implementation pending.** This note is the substrate for **ADR-0120**
> (to be written at the start of the implementation tick). Authored autonomously (the [[project_ceir_autonomous_loop_grant]]
> grind) after two advisor consults resolved the scope forks. Master-map sections: **§108 hot-reload · §109 state
> migration** (§110 incremental / plan-cache is **CEIR-10b**). Resumes the paused **7c** contract verbatim on the
> closed 8d/8h/8i foundation.

## 0. What the slice is

The hot-reload **lifecycle** for CEIR programs, built in the **crd-ceir-cook bridge** on the 7b `RuntimeProgram` /
`ProgramSlot` foundation: `detect → cook → load → validate-set → (state migration decision) → atomic set-install →
generation bump → deferred destroy`, with **last-good on failure**. ⛔ **No mixed-generation call graph.** State
migration decides **CompatibleReuse / Migrate(fn) / Reject** and, where a live value store exists, **moves the real
§20 state cells** across generations keyed by 8d stable id.

Gate (the 10z host subset, one coherent DoD): **body-edit hot-swaps live · signature-edit REJECTS + keeps last-good ·
dep-edit recompiles exactly the affected region** (cache-hit counts are 10b/10z scope).

## 1. The three scope forks (advisor-resolved)

**A — migration is a structural DECISION *and* a live-value MOVE.** The decision engine (compare old/new state schemas →
`CompatibleReuse / Migrate / Reject`) is unambiguously 10a. A live value store **does exist**: the CEIR-5z reference
`exec::Interpreter` (`engine/ceir/include/crd/ceir/exec.hpp`) holds §20 cells (`m_cells`). "Never level down" → migrate
**real** cell values, not just decide structurally. Wrinkle: the Interpreter keys cells by **op pointer** (does not
survive round-trip / differs across generations) with no set-by-id surface → 10a adds a **snapshot/restore-by-stable-id**
surface to the Interpreter (a crd-ceir core change; §3).

**B — install atomicity is the SLOT, not 8i.** 8i is Module-mutation-scoped; a reload replaces whole Modules in
per-generation Contexts. The band-contract phrase "reload transactions ride 8i" gets a **documented in-place correction**
(strike-in-place discipline, not a silent drop): 8i is the **upstream authoring seam** (agent edit → 8i commit → content
hash changes → the reload's detect trigger), **not** the install mechanism. The genuinely new machinery is **SET-level
all-or-nothing install**: if program A imports B and B's interface changed, installing B alone would create a mixed
graph, so the whole **affected set installs atomically or not at all**. Last-good falls out free — `ProgramSlot::install`
is publish-only, so any failure *before* the install loop leaves every slot untouched.

**C — "compile-affected" = the 8h affected-SET only.** Build a program-level dep graph from CDEP edges into an 8h
`IncrementalDag` (program-granular content/interface revisions; §107 gives body-edit-doesn't-invalidate-callers free);
10a computes the affected set. **Actual compilation is named-forward** (nothing to compile until CEIR-11); cache-hit
assertions are 10b/10z.

## 2. The four pre-ADR verifications (all resolved from code)

**V1 — CDEP carries callee NAMES only; NO serialized-record change, NO schema bump.** `DependencyRecord`
(`program_asset.hpp`) holds `called_funcs`/`intrinsics`/`providers` (StringViews). It does **not** record an expected
callee `interface_hash`; the `KernelRef = {asset_id, interface_hash}` slot is named-forward (see V-notes). The advisor's
conditional ("no callee hash ⇒ extend the record ⇒ schema bump + recook + fuzz") **fired but does not apply here**: the
10z host subset compares **live-vs-new** interface hashes — the supervisor sees both sides in memory at reload time, so
no *recorded expectation* is needed. The dep graph (edges) + per-program `interface_hash` change-detection + the atomic
affected-set install (fork B) satisfy ⛔ no-mixed-generation **without** a recorded hash. Serializing an expected-callee
hash now would be **a field with no consumer this slice** — the exact `native_determinism` (7a) / third-graph (8h) scar.
⇒ **The cooked format is UNTOUCHED: no `kCeirCookSchema` bump, no recook, no new fuzz corpus.** Persistent cross-session
validation is named-forward to **10b** (the plan cache is keyed + persisted anyway). ⛔ **REFINED at 10b close (ADR-0121):
10b's `PlanCache` is IN-MEMORY** (validate-on-hit against live truth) — the persistent/serialized form moved to CEIR-11+/13.

**V2 — no `CookDb`/`register_cook_handler` exists; the detect front-end REUSES the shipped resource hot-reload.**
`crd::resources::ResourceManager` already ships `subscribe_reload(ResourceId, ReloadCallback, user)` +
`poll_hot_reload(debounce_ms)` (mtime watch + debounce + one-frame grace) + `reload_mount_now(MountId)` +
`register_loader(ILoader)`. crd-ceir-cook already links crd-resources (7a). ⇒ The "CookDb/register_cook_handler" is this
machinery **under a planned name**: reuse it for the **detect** signal (a CEIR `ILoader` + a `subscribe_reload` callback);
the CEIR supervisor owns cook→validate-set→migration→atomic-install over its own `ProgramSlot`s (ResourceManager's
per-resource *unconditional* swap cannot express CEIR's cross-program atomic-set-or-reject / keep-last-good). Building a
parallel file-watch would be the third-graph scar in a new costume. **Host-subset gate** drives the lifecycle via a direct
`reload_program(...)` API (the `reload_mount_now` precedent for tests); real mtime file-watch is the production path.

**V3 — the state schema IS stable-id-keyed (8d CONFIRMED); the header comment is the stale one.** `program_asset.cpp`
(the `interface_hash` impl) collects, per `StateEdge` cell, `(stable_id, result-type, §20 depth)`, **sorts by stable id**,
then hashes — CEIR-8d/ADR-0114 §2.7, explicitly anticipating "10a migration would silently lose the id-1 state" under an
order-only hash. So 8d's claim is correct. The **stale artifact** is `program_asset.hpp:24` ("State cells stay in BODY
order — layout order IS the migration schema") — a 9f-class in-place fix (done in the implementation rebuild, §5). The
walk is **inline** inside `interface_hash`; migration needs the list, so 10a exposes **`collect_state_schema()`**
returning the sorted `(stable_id, type, depth)` cells, and `interface_hash` folds that shared function (one source of
truth, no divergence).

**V4 — the Interpreter has live cells but keyed by op pointer.** `m_cells : HashMap<const Operation*, Cell>`; `Cell =
{ring, pos}`; `cell_value(op*, out)` inspects. Migration moves the **whole ring + pos** (not one value), matched by
stable id. An **absent** cell (never evaluated in the old generation) = skip silently → the new generation init-fills
(EMPTY≠UNKNOWN — not an error). 10a adds `snapshot_state_by_id()` / `restore_state_by_id()` to the Interpreter, bundled
with `collect_state_schema` exposure so there is **one** crd-ceir core rebuild + 4-config re-gate, not two.

## 3. The migration decision table (the advisor's key design gap)

Because the §20 state schema is **folded into** `interface_hash`, a single `interface_hash` inequality **cannot**
distinguish "signature/effects changed" (callers break — unfixable by migration) from "only the state schema changed"
(fixable by a migration fn). The decision therefore needs a **contract-sans-state comparison** — a `contract_hash`: the
§107 projection **minus** the state-schema segment (pure in-memory analysis; the cooked header is unchanged — **no format
motion**). Reuse the existing `interface_hash` projection builder, emitting the state segment separately.

| old vs new | `contract_hash` | state schema | Decision |
|---|---|---|---|
| identical | equal | equal | **CompatibleReuse** — reuse cells verbatim (body-only edit; the common hot-swap) |
| interface differs | **differs** | any | **Reject** (keep last-good) — the caller contract changed; a migration fn cannot save callers |
| interface differs | equal | **differs** | **Migrate(fn)** if a migration fn is registered for this program+schema-delta; else **Reject** (keep last-good) |

A **registered migration fn** is the explicit opt-in that upgrades `Reject → allowed-with-migration` for a pure
state-schema delta. All three outcomes are exercised by the gate.

## 4. Other pins (advisor)

- **Symbol→program resolution:** two loaded programs exporting the same symbol → **loud reject at set-validate** (the
  silent-drop scars), never last-write-wins.
- **Deferred destroy / generation-retire (D5):** `RuntimeHandle` has staleness detection but **no refcount**; the honest
  host-only floor is **keep-last-good-until-next-install + an explicit drain** (free the old generation's Context
  wholesale — arenas free per-Context — at the *start* of the next install, mirroring ResourceManager's one-frame grace).
- **RAF-11 reentrant scar applies verbatim:** the reload driver must tolerate a reload triggered *during* a reload
  (guard + reserve + drain) — [[feedback_reentrant_init_programs_reload_must_guard_registration_and_drain_queue]].
- **No-privileged-path:** a program reloaded from source text and the same program rebuilt in C++ migrate identically
  (the §121 property the cook already guarantees).

## 5. Internal staging (ONE tracker row — do NOT fragment)

10a stays a single tracker row with one coherent DoD (a live hot-swap works end-to-end); staged internally for
implementation order. Restructuring user-authored rows exceeds the grant's "write out subslices at 17+" scope.

1. ✅ **DONE (2026-08-10) — crd-ceir core exposure** (one rebuild + 4-config re-gate, 364/364): `collect_state_schema()`
   + `contract_hash`/split projection in `program_asset.{hpp,cpp}`; Interpreter `snapshot_state_by_id`/`restore_state_by_id`
   in `exec.{hpp,cpp}`; the three in-place stale-comment fixes. `test_reload_migration.cpp` (6 `[ceir][reload]`).
2. ◀ **NEXT — the reload supervisor + set-install** (crd-ceir-cook, `hot_reload.{hpp,cpp}`) — API locked in §8 below.
3. **State migration** wired into the install: the decision table (§3) + the Interpreter snapshot/restore + registered
   migration-fn registry.
4. **Detect front-end**: the CEIR `ILoader` + `subscribe_reload` reuse (V2) + the direct `reload_program` API the gate
   drives.

## 6. Named-forwards (explicit, honest)

- **Expected-callee-`interface_hash` serialized record (`KernelRef`)** → **CEIR-10b** (persisted plan cache) / **CEIR-13**
  (CKIR asset refs). Not this slice — no consumer here (V1). ⛔ **REFINED at 10b close (ADR-0121):** the in-memory record
  landed at 10b (`PlanCache`'s `PlanDep`); the SERIALIZED form → CEIR-11+/13 (10b's cache is in-memory).
- **Semantic call-site compatibility** (does A's call still typecheck against B's new signature, beyond a hash compare) →
  **CEIR-3/4** (semantic verifiers; the 9g structural-vs-semantic boundary).
- **Actual compilation of the affected set** → **CEIR-11** (executable form) / **CEIR-10b** (plan cache + cache-hit
  counts).
- **Real mtime file-watch detect in production** → wired via ResourceManager (V2); the host-subset gate uses the direct
  reload API.

## 7. Gate — DELIVERED across stages 1–4 (2026-08-10); the 10z full-matrix is the band-close gate (after 10b)

**RECONCILED at 10a close.** The 10a lifecycle shipped as four gated increments; the tags/terms below are the delivered
reality (tag `[ceir][reload]`, the 3-way `ReloadDecision`). `test_reload_migration.cpp` (6, stage 1: `collect_state_schema`
+ `contract_hash` decision split + Interpreter snapshot/restore) + `test_hot_reload.cpp` (16, stages 2–4): body-edit →
**HotSwap**, cells carry (migrate_state verbatim) + old handle stale; signature-edit → **ContractChange** reject +
last-good; state-schema-only → **NeedsMigration** (reject without a fn; install + a real value migration WITH a fn —
ring [7]→[7,7] survives); dep-edit → exact `affected_by` set (8h), atomic set-install, no mixed-generation graph;
**bad source → CookFailed + last-good**; dup-symbol / AssetId-0 / reentrant → loud rejects. Gate per stage: crd-ceir +
host + cook across **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** + LLVM-20 tidy + GCC `-Werror=switch`
(`ReloadDecision`/`AddError`) + opgen drift/validator + `crd-ceir-invariants`. **No recook, no fuzz** (the cooked format
is untouched; only stage 1 touched crd-ceir core → the all-3-targets rebuild). Progression: 358 → **364** (1) → **370**
(2) → **373** (3) → **377** (4).

⛔ The **§172 full-matrix gate is CEIR-10z** (the band close, after 10b's plan cache — cache-hit counts are 10b/10z
scope); 10a's own DoD is "a live hot-swap works end-to-end," delivered + gated above.

## 8. Stage 2 detailed design — the reload supervisor (API-LOCKED 2026-08-10, 3rd advisor consult)

`engine/ceir-cook/{include/crd/ceir/cook,src}/hot_reload.{hpp,cpp}`. crd-ceir-cook already links crd-ceir +
crd-render-asset-core + crd-resources + crd-containers (7a/7b) — no new module edge.

### Ownership (the push-back-UAF scar drives the shape)

`ProgramSlot::install` stores a RAW `RuntimeProgram*`. If a `RuntimeProgram` lived by value inside an `Entry` held in a
growable `Array<Entry>`, array growth would move it and every installed pointer + minted handle would dangle. So:

- `Generation { Context* ctx; RuntimeProgram program; }` — ONE heap unit per loaded generation, owned by the supervisor's
  allocator; the slot points at `&gen->program`; the Context is non-movable → pointer-owned, freed WHOLESALE on retire.
- `Entry { AssetId id; ProgramSlot slot; Generation* current; Generation* zombie; u64 content_hash, contract_hash,
  interface_hash; }` — may live by value in `Array<Entry>` (holds no interior-pointed payload). `zombie` is one-deep (the
  ResourceManager one-frame-grace model). Deps/exports are NOT stored as views (arena-strings die with a retired Context).

`ReloadSet` owns `Array<Entry>` + `IncrementalDag dag` + `IAllocator*` + the registrar. Generations are heap units:
`allocate(sizeof(Generation))` + placement-new; retire = `~Context()` + `~Generation()` + deallocate.
- **Install success:** old `current → zombie`; drain the previous `zombie` first; new candidate → `current`;
  `slot.install(&current->program, id)` (bumps generation → old handles stale).
- **Reject:** destroy the candidate `Generation` immediately (never installed, nothing references it — the leak path);
  `current`/`slot` untouched → **last-good = NOT installing** (no stored blob; persistence is 10b).
- **`drain()`** (shutdown/tests): free every `zombie`, and on teardown every `current`.

### The dep graph — `set_revision(asset, content_hash, contract_hash)` makes 8h the validate-set

The dag's INTERFACE revision = `contract_hash` (NOT `interface_hash`): callers depend on the contract, not on B's internal
state schema. Then the 8h §107 rule (`recompute_after_change`) IS the affected-set: a content-only change → `{self}`
(dependents stay valid — hot-swap or self-migrate); a contract change → `{self + transitive dependents}`. The 10z
"dep-edit recompiles exactly the affected region" falls out of `affected_by` UNMODIFIED (one more "the engine IS the
mechanism" claim, paired with 9c/9f). `set_revision` ONLY on a successful install — a Reject decides from hash comparison
against the Entry's stored hashes and never commits a revision.

Rebuild the graph WHOLESALE on every membership change (add / successful reload / remove): edges + the symbol→program map
are arena-`StringView`s living in each CURRENT generation's Context; a retired Context's strings dangle, and
`IncrementalDag` has no remove API. Sets are small — an O(n) rebuild from the live generations is correct + cheap. Edges:
for each Entry A, `collect_dependencies(A.ctx, A.module)` → each external `called_funcs` name → the Entry B that EXPORTS it
→ `dag.add_edge(A.id, B.id)`. Exports = walk B's module top-level Public `func` ops for `sym_name` (bridge-side; no core surface).

### The three-way decision (emit the full enum NOW; stage 3 only wires the fn onto NeedsMigration)

`enum class ReloadDecision : u8 { NoChange, HotSwap, NeedsMigration, ContractChange };` — comparing the reloaded program's
OLD Entry hashes vs the freshly-loaded candidate:
- `content_hash` equal → **NoChange** (no-op; candidate destroyed).
- `interface_hash` equal (⇒ contract & schema equal) → **HotSwap** → install; dependents untouched.
- `contract_hash` equal, `interface_hash` differ → **NeedsMigration** (only the state schema changed) → **stage 2: Reject**
  (no fn registry yet, candidate destroyed); **stage 3:** run the registered migration fn + `restore_state_by_id`.
- `contract_hash` differ → **ContractChange** → **Reject** (keep last-good).

**Reject-always on a contract change — no leaf-allow.** The supervisor cannot prove a leaf has no callers: the HOST is a
caller it cannot see (EMPTY≠UNKNOWN). Reject-always matches the 10z letter ("signature-edit REJECTS + keeps last-good").
Contract evolution is a COLD reload (remove + re-add). Leaf-allow is a NAMED-FORWARD relaxation for CEIR-11 (when the
executable form can recompile/validate callers).

### API surface

- `using Registrar = void (*)(Context& ctx, void* user);` — registers the module's dialects into a fresh Context; run
  immediately after Context construction, BEFORE `load_program` (its registration check is first).
- `enum class AddError : u8 { Ok, LoadFailed, DuplicateSymbol, InvalidAssetId, AlreadyPresent };`
- `struct AddResult { AddError error; LoadError load_error; };` (load_error valid iff LoadFailed)
- `struct ReloadResult { ReloadDecision decision; bool installed; LoadError load_error; };`
- `class ReloadSet`:
  - `ReloadSet(IAllocator* alloc, Registrar reg, void* user); ~ReloadSet();` (drains everything)
  - `AddResult add(AssetId id, ConstSpan<u8> blob);` — initial load; id==0 → `InvalidAssetId`
  - `ReloadResult reload(AssetId id, ConstSpan<u8> blob);` — the lifecycle
  - `void remove(AssetId id);` — cold-reload half; rebuilds the graph
  - `void drain();` — free zombies (tests/shutdown)
  - `ProgramHandle handle(AssetId id) const;` · `const RuntimeProgram* program(AssetId id) const;`
  - `bool affected(AssetId id, Array<AssetId>& out) const;` — `dag.affected_by` (the "recompiles-affected" set)
  - `Generation* generation(AssetId id) const;` — the CURRENT generation's Context/Module (the caller builds the NEW
    interpreter here). ⛔ **CORRECTED (stage 3):** the ReloadSet does NOT own interpreters — live §20 cells are populated
    by EXECUTION, which the supervisor never does. Migration is CALLER-DRIVEN (fork B): `reload` gates on a registered
    `MigrationFn`'s PRESENCE + installs the Module; the caller applies the value-move against ITS OWN interpreters via
    the free `migrate_state` helper (snapshot old → fn → restore new). See §9.

`add`/`reload` flow: alloc `Generation` → `new(…) Context(alloc)` → **registrar(ctx)** → `load_program(ctx, blob…)` (a
`LoadError` → destroy candidate, return) → `collect_dependencies` + `interface_hash`/`contract_hash` on the loaded module
→ decide → install-or-destroy → rebuild the dag. **Dup-symbol reject** at add: a candidate exporting a symbol another
Entry already exports → `DuplicateSymbol`, candidate destroyed (loud, never last-write-wins).

### Pins + boundaries

- **AssetId 0 → `InvalidAssetId`** at add (the dag silently ignores node 0 — the silent-drop scar).
- **Declared-header-id boundary:** the cook writes `CookedHeader.id`, but `read_program`/`load_program` do NOT surface it
  (ReadResult/RuntimeProgram carry no id). Stage 2 trusts the caller-passed `AssetId`; validating passed-id == declared-id
  is a future refinement (surface `CookedHeader.id` in `ReadResult`). Documented, not silently assumed.
- **Test scope (host subset):** cross-program calls do NOT execute (an import → `UnresolvedCall` at exec). Stage-2 tests
  assert **decisions, affected sets, handle staleness, dup-symbol reject, last-good** — never cross-program execution.
- **Gate at stage-2 close:** a crd-ceir-cook change (bridge only; crd-ceir untouched) → rebuild cook-tests + the 4-config
  `-R ceir`; LLVM-20 tidy (`hot_reload.{hpp,cpp}` + the new test); GCC `-Werror=switch` (the new enums). No recook, no fuzz.
