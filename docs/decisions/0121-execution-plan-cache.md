# ADR-0121 — Execution-plan cache: a validate-on-hit store of live truth

**Status:** **ACCEPTED** (2026-08-10, under the standing autonomous loop grant [[project_ceir_autonomous_loop_grant]];
design + close advisor-reviewed) — the D-007 **CEIR band 10 (Asset lifecycle completion)**, slice **CEIR-10b**. A
`PlanCache` stores caller-produced (opaque) execution-plan artifacts keyed by (content hash × target × compiler
version) and returns them ONLY after validating them against live truth — the cache never trusts itself.
**Phase:** D-007. Resumes the **7d** contract (paused at the 2026-08-09 re-baseline). Law: mission §110 (incremental) ·
§104 (ADR-0104 persistent-pipeline-cache precedent). Design: `docs/design/ceir-10b-execution-plan-cache.md`.
**Tags:** `[ceir]` `[plan-cache]` `[incremental]` `[cook-bridge]`

---

## 1. Context

7d (the plan cache) paused because it consumes the 8h incremental model (CEIR-8h) and the interface-hash discriminators
(7b). It resumes as 10b on the closed foundation. Two facts shape the design: **no compiler exists** (CEIR-11) and the
**compile→plan interface is explicitly deferred to CEIR-21/26** (`provider.hpp:36`). So 10b cannot *produce* plans; a
fully-mock compile pipeline would prematurely name what 21/26 owns and risk a zero-consumer artifact (the third-graph
scar). But the 10z gate row asserts cache-hit counts, so the band contract requires the cache before 10z — deferral is
out. The resolution: 10b builds the cache MECHANISM (store + invalidation) whose invalidation has a real driver TODAY —
the 10a ReloadSet's reload events — and gives the twice-deferred KernelRef record its first consumer.

## 2. Decision — a STORE + VALIDATE-ON-HIT client of live truth

`PlanCache` (`ceir-cook/plan_cache.{hpp,cpp}`, bridge-only) is a content-addressed store; it does NOT compile.

### 2.1 No compile callback — the caller produces, the cache stores + gates

`get(key)` returns `{Hit, Miss, StaleDeps}` (+ the artifact on Hit); the CALLER compiles an opaque artifact and
`put(owner, key, artifact, deps)`s it. This is fork-B (ADR-0120 §2.3) reapplied — store/gate authority separate from
production — and it is what lets 10b exist now without naming the compile→plan interface deferred to 21/26. The artifact
is opaque bytes this slice; the real producer is 21/26, the real consumer 11+.

### 2.2 The key + validate-on-hit against live truth ("caches, never truth")

`PlanKey{u64 content_hash, u64 target, u32 compiler_version}` — `content_hash` = `stable_hash(module)` (the truth the
plan derives from); `target` = an interned FNV of the `IExecutionProvider::name()` (the CapabilityId pattern; two
targets coexist for one content); a `compiler_version` bump misses every entry. A `get` Hit requires the key to match
AND **every recorded dependency to still validate against live truth**: a **resolver** fn-ptr `u64 (*)(AssetId, void*)`
returns the dependency's CURRENT `interface_hash`; the cache compares it to the value the plan recorded at compile time.
Validate-on-hit IS "caches, never truth" made mechanical — the cache never trusts its own stored bytes. ⛔ EMPTY≠UNKNOWN:
a resolver return of 0 (dependency gone/unresolvable) is STALE, never valid. A key miss is a `Miss`; a key hit with a
drifted dep is `StaleDeps` (and the stale entry is dropped — lazy eviction). `hits()`/`misses()` are first-class (the
10z gate asserts hit counts). A self-change (the program's own content edit) needs no invalidation — the new content is
a new key, a `Miss` by construction.

### 2.3 Dependency records key on `interface_hash`, NOT `contract_hash`

CEIR-10a learned contract-keyed dependent-safety for reload-REJECT (precision — don't reject on a state-schema-only
change). Cache-validity is the OPPOSITE regime: a stale plan is CORRUPTION, a spurious recompile is CHEAP (the 7a
under/over-inclusion asymmetry), so validity wants CONSERVATISM. `interface_hash` ⊇ `contract_hash` in sensitivity and
matches KernelRef's ADR-0109 §85 definition verbatim. **Deliberate consequence:** a NeedsMigration install of a callee
(a state-schema-only change, which reload permits) over-invalidates the caller's plan. Safe by the asymmetry.

### 2.4 No second dag · in-memory · bridge-local record

The cache owns NO graph — the resolver makes it a CLIENT of the ReloadSet's 8h dag (`resolver(id)` reads
`set.program(id)->interface_hash`); `affected_by` gives which plans go stale on a callee change, unmodified. The cache
is **in-memory** this slice. The dependency record is a bridge-local `PlanDep{AssetId, u64 interface_hash}` — NOT a
crd-ceir `KernelRef` mint (crd-ceir is asset-free, I5; the eventual `KernelRef` holds a raw u64, CEIR-13's problem); it
corresponds to ADR-0109 §85 `KernelRef = {asset_id, interface_hash}`. Artifacts + dep records are cache-owned HEAP
buffers (never interior pointers into the growable entry vector — the ADR-0120 push-back-UAF discipline).

## 3. Consequences

- The plan-cache invalidation SEMANTICS are proven: `test_plan_cache.cpp` (3 `[ceir][plancache]`) + a pairing rehearsal
  in `test_hot_reload.cpp` (the 10z inheritor — a callee interface change stales exactly `affected(B) ∪ {B}`).
- ⭐ **The §107 hot-swap property is now observable at the PLAN layer:** a callee body-only edit (interface unchanged)
  leaves the caller's cached plan a HIT — the payoff §107 was built for.
- ADR-0118's "plan-cache wiring is CEIR-10b" named-forward is discharged; the KernelRef record has its first consumer.

## 4. Named-forwards (explicit)

- **The SERIALIZED / persisted plan-cache form** → CEIR-11+ (real plans) / CEIR-13 (CKIR/KernelRef serialized refs). ⛔
  This **refines ADR-0120 §4's "KernelRef record → 10b (persisted plan cache)"**: the record landed at 10b as the
  in-memory `PlanDep` validity mechanism; the *persistent/serialized* form did NOT — it waits until the real plan
  payload (variants, chunking à la ADR-0104) exists, so the format is not designed blind. (Struck in place in ADR-0120
  §4 + the 10a design note.)
- **The compile→plan interface** (what produces the cached artifact) → CEIR-21/26 (the partitioner band, per
  `provider.hpp`). 10b never names it.
- **LRU / memory budgets** → when there is real cache load (ADR-0104-scale); 10b has explicit `evict(owner)`/`clear()`
  only.

## 5. Alternatives rejected

- **A full mock compile pipeline in the cache** (a `compile` callback the cache invokes on a miss): prematurely defines
  the compile→plan interface that provider.hpp defers to 21/26, and fakes production the caller owns. → store + gate
  only (§2.1).
- **Deferring the plan cache past band 10:** the 10z gate row asserts cache-hit counts — deferral breaks the band
  contract; re-sequencing a user-authored row is never-defer without user direction. → build the mechanism now (§1).
- **`contract_hash`-keyed dependency validity:** under-conservative for a cache (a stale plan is corruption). →
  `interface_hash` (§2.3).
- **A serialized cache format now:** designed blind (the real plan payload doesn't exist) and redesigned when variants /
  chunking arrive. → in-memory; persistence named-forward (§4).
- **A crd-ceir `KernelRef` mint:** a core type with no core consumer (the zero-consumer scar) + a needless 3-target core
  rebuild; crd-ceir is asset-free (I5). → bridge-local `PlanDep` (§2.4).

## Gate

`test_plan_cache.cpp` (3 `[ceir][plancache]`) + `test_hot_reload.cpp` (+1, the pairing rehearsal). **381/381 ctest**
across **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** (win-asan clean on the heap-buffer ownership) +
LLVM-20 tidy + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants` (U-§116 holds — bridge-only, crd-ceir
core untouched). **No recook, no fuzz** (in-memory; nothing serialized).
