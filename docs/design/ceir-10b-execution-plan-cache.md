# CEIR-10b — Execution-plan cache — DESIGN NOTE

> Status: **DESIGN LOCKED (2026-08-10, advisor consult).** Substrate for **ADR-0121** (at 10b close). The 7d contract:
> an execution-plan cache keyed by (content hash × target × compiler version) through the 8h model — compiled artifacts
> are caches, never truth. Master-map §110 (incremental) · §104 (ADR-0104 persistent-pipeline-cache precedent).

## 0. The shape — a STORE + VALIDATE-ON-HIT client of live truth (not a compile pipeline)

No compiler exists (CEIR-11) and the **compile→plan interface is explicitly deferred to CEIR-21/26** (`provider.hpp:36`).
So 10b does NOT produce plans and never names that interface. It is a generic **content-addressed cache**: the CALLER
compiles an (opaque) artifact and `put()`s it; the cache `get()`s it back — but only after **validating it against live
truth**, so the cache never trusts itself ("caches, never truth" made mechanical). The artifact is mock (opaque bytes)
this slice; the real producer is 21/26, the real consumer 11+. **The mechanism is real and has a live driver today** —
the 10a ReloadSet's reload events — which is the 9x pattern (mock payload, real mechanism), NOT the ownerless-field scar.

Home: **`ceir-cook/plan_cache.{hpp,cpp}`** (bridge; a separate file from `hot_reload` — a different concern). Bridge-only
→ the lighter gate. In-memory ONLY this slice.

## 1. The seven pins (advisor)

1. **No compile callback in the cache.** `get(key) → {Hit, Miss, StaleDeps}` + the artifact on Hit; the caller produces
   the artifact and `put()`s it. Fork-B reapplied — store/gate authority separate from production. This is what lets 10b
   be built now without defining what 21/26 owns.
2. **The dep record keys on `interface_hash`, NOT `contract_hash`.** Cache-validity wants CONSERVATISM (the 7a under/over-
   inclusion doctrine: a stale plan = corruption, a spurious recompile = cheap); `interface_hash` ⊇ `contract_hash` in
   sensitivity and matches KernelRef's ADR-0109 §85 definition verbatim. ⭐ Consequence (a test comment): a NeedsMigration
   install of a callee OVER-invalidates the caller's plan — deliberate + safe.
3. **No second dag.** The cache owns no graph — it validates via a **resolver** fn-ptr `u64 (*)(AssetId, void* user)` →
   the dep's CURRENT `interface_hash`. ⛔ EMPTY≠UNKNOWN: a resolver returning 0/unknown for a recorded dep is **STALE**
   (miss), never valid. (In the ReloadSet pairing, the resolver reads `set.program(id)->interface_hash`, 0 if absent.)
4. **In-memory this slice; persistence → CEIR-11+.** No serialized cache file (a new record = fuzz + schema + versioning
   whose primary payload — real plans, variants, chunking à la ADR-0104 — doesn't exist yet; the format would be designed
   blind). "Never truth" is about AUTHORITY, not medium. ⛔ **This refines the 10a named-forward** ("KernelRef record →
   10b *persisted* plan cache"): the record lands NOW (in-memory validity); the serialized form moves to 11/13 —
   strike-in-place at close.
5. **Bridge-local `PlanDep{AssetId, u64 interface_hash}`, NOT a crd-ceir `KernelRef` mint.** A core type with no core
   consumer is the zero-consumer scar + forces a needless 3-target core rebuild; crd-ceir is asset-free (I5) so the
   eventual `KernelRef` holds a raw u64 (CEIR-13's problem, as `program_asset.hpp` already says). Name the correspondence
   in a comment.
6. **Hit/miss counters are first-class** (10z asserts "cache-hit counts"). ⭐ **The headline test — §107 extended to
   plans:** a body-only edit of callee B (HotSwap, B's interface unchanged) leaves caller A's plan a **HIT** (A's recorded
   `PlanDep` for B still validates); reload A itself → new content → new key → **MISS by construction** (content-addressing;
   no invalidation needed for a self-change). ⛔ **DELIVERED note:** the "reload B contract-change → rejected → A still HIT"
   scenario is a LIVE (ReloadSet-driven) property NOT asserted in 10b's tests — it is a row of the **CEIR-10z** matrix
   (hit-count deltas across the three edit classes), where it belongs; 10b proves the cache mechanism, 10z proves it live.
7. **Key = `{u64 content_hash, u64 target, u32 compiler_version}`**, `target` = interned FNV of the `IExecutionProvider::
   name()` (the CapabilityId pattern — two targets = two coexisting entries for one content). A `compiler_version` bump →
   all-miss (one cheap test). No LRU/budgets (ADR-0104-scale, no load here) — explicit `evict(AssetId)` (provenance-keyed,
   memory hygiene) + `clear()`; `clear()` mid-session → all-miss → recompute → re-hit is the "never truth" falsifiable test.

## 2. API (`plan_cache.hpp`)

```
struct PlanKey { u64 content_hash; u64 target; u32 compiler_version; };   // equality by all three
struct PlanDep { AssetId id; u64 interface_hash; };                        // bridge-local; ≡ ADR-0109 §85 KernelRef
using InterfaceResolver = u64 (*)(AssetId id, void* user);                 // current interface_hash; 0 ⇒ stale
enum class PlanStatus : u8 { Hit, Miss, StaleDeps };
struct PlanLookup { PlanStatus status; const crd::u8* artifact; crd::usize size; };  // artifact null unless Hit

u64 plan_target(containers::StringView provider_name);                     // FNV intern of IExecutionProvider::name()

class PlanCache {
  PlanCache(IAllocator* alloc, InterfaceResolver resolver, void* user);
  PlanLookup get(const PlanKey& key);          // key match + validate EVERY PlanDep vs resolver (0/mismatch ⇒ StaleDeps)
  void       put(AssetId owner, const PlanKey& key, ConstSpan<u8> artifact, ConstSpan<PlanDep> deps);  // stores a COPY
  void       evict(AssetId owner);             // drop entries for an asset (provenance; memory hygiene)
  void       clear();
  usize hits() const;  usize misses() const;   // misses counts Miss + StaleDeps (both ⇒ the caller recompiles)
};
```

`get`: find the entry whose `PlanKey` matches; none ⇒ **Miss** (misses++). Found ⇒ validate every `PlanDep`:
`resolver(dep.id) == dep.interface_hash && != 0` for all ⇒ **Hit** (hits++, return artifact); else **StaleDeps** (misses++,
no artifact; drop the entry). `put` copies the artifact + deps under `owner`+`key`, replacing any same-key entry.

## 3. Tests — DELIVERED (2026-08-10; reconciled to the shipped form)

**Homes:** the pure-cache cases live in `test_plan_cache.cpp` (mock resolver, no cook helpers); the ReloadSet PAIRING
rehearsal landed in `test_hot_reload.cpp` (to reuse its cook helpers). Both added to the EXPLICIT cook-tests list.

- `test_plan_cache.cpp` (3 `[ceir][plancache]`): **put→get Hit; miss on unknown key / compiler_version bump / new target;
  two targets coexist for one content.**
- ⭐ **§107 extended:** A's plan records `PlanDep{B, ifaceB}`; a body-only edit of B (interface unchanged) → A's plan still
  **HIT**; B's interface CHANGED (resolver returns a new hash) → A's plan **StaleDeps**; B removed (resolver 0) → StaleDeps.
- **clear()/evict() → all-miss → re-put → re-hit** (the "never truth" falsifiable test).
- ⭐ **The 10z REHEARSAL (`test_hot_reload.cpp` +1, pairing with the ReloadSet):** resolver = `set.program(id)->
  interface_hash`; cache A's plan (dep B), B's plan, an independent C's plan; cold-reload B with a NEW interface (remove +
  re-add signature change). ⛔ **The recompile set = `affected(B)` [StaleDeps] ∪ {B} [Miss BY CONSTRUCTION]:** A's plan →
  **StaleDeps** (recorded dep B drifted); B's OWN plan → **Miss** (B's new content = a new key — NOT StaleDeps; a
  self-change needs no invalidation); the independent C → still **HIT**. So A ∈ `affected(B)`, C ∉ — the recompile set
  matches `affected_by`, unmodified.

## 4. Gate + close

Bridge-only (crd-ceir core untouched) → rebuild cook-tests + 4-config `-R ceir`; LLVM-20 tidy (`plan_cache.{hpp,cpp}` +
both tests); GCC `-Werror=switch` (there is NO `PlanStatus` switch to guard — see below); opgen drift/validator;
`crd-ceir-invariants` (U-§116 holds). **No recook, no fuzz** (in-memory; nothing serialized). ADR-0121 at close, stating
the persistence named-forward (11/13) that refines the 10a KernelRef-record forward. Then CEIR-10z (the §172 band-close
matrix, which inherits the pairing rehearsal).

⛔ **DELIVERED note — no `plan_status_name` (deliberate).** The sibling enums shipped a `*_name` with an exhaustive
switch (`reload_decision_name`, `add_error_name`) because they surface in DIAGNOSTICS; `PlanStatus` is a PROGRAMMATIC
result (the caller branches on Hit/Miss/StaleDeps), not a diagnostic surface — a name fn would be a zero-caller function
(a needless rebuild + re-gate). It lands with a diagnostic consumer, if ever. So 10b adds no new `-Werror=switch` site.
