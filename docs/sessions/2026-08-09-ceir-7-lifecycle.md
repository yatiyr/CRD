# CEIR band 7 — Asset / cook / runtime lifecycle (§105–§110)

Band 7 makes CEIR programs ordinary Cerid assets: a `'CEIR'` cook (source → verified → binary chunk), a
generation-safe runtime handle model, hot reload, and an execution-plan cache. It REUSES hard — the CRDR container,
the ADR-0104 content-hash cook cache, the RAF cooked-asset envelope (`crd-render-asset-core`), and the RAF-11
hot-reload machinery — rather than rebuilding any of it.

## CEIR-7a — the `'CEIR'` cook: interface hash, dependency record, verified packaging (2026-08-09)

**Delivered.** The cook that turns a `crd::ceir::Module` into a self-describing CRDR blob carrying the CONTENT hash
(the cook-cache key), the §107 INTERFACE hash (so an implementation-only edit hot-swaps without invalidating
callers), and the §106 dependency record.

**The design (advisor-ruled).** The SEMANTIC computations are IR analysis and live in crd-ceir (ADR-0109 §6 makes
the interface hash a core concept — `KernelRef = {asset_id, interface_hash}`); only the CRDR packaging lives in a
bridge. So:

- **crd-ceir core (no new edge — I5 green as-is):**
  - `interface_hash(§107)` — FNV-1a over a CANONICAL projection: each EXPORTED (Public) func **sorted by name**
    (never body order — a function reorder is an impl edit), its param + result types encoded **structurally**
    (recursing `Type::members`, never a Context-local TypeId int — so the hash is cross-Context stable like the 1f
    blob), its **transitive effective effect set** (5c — a body edit adding a caller-visible effect IS an interface
    change), and the **module-wide §20 state schema** (every StateEdge cell, module pre-order — a private callee's
    cells are live state a 7c hot-swap must migrate; the safety asymmetry decides module-wide vs exported-only).
    Impl-only bodies / private funcs / constants are excluded, so an implementation-only edit leaves it unchanged.
  - `collect_dependencies(§106)` — external calls (an unresolved callee is an IMPORT, not a cook error) + intrinsics
    + providers, **schema/registry-driven** (`op_info.intrinsic`/`native_provider`), never dialect-name-sniffing.
  - `find_unregistered_op` — the strict cook check: EMPTY≠UNKNOWN, an unregistered op's verifiers pass vacuously.
- **The OpInfo/OpSpec promotion (a crd-ceir core change).** `intrinsic` + `native_provider` were on the generated
  `OpSchema` but NOT the runtime `OpInfo` — so the dep collector could not read them. Promoted to `OpSpec`/`OpInfo`
  (the determinism/domain precedent, 4b/4c) + `register_op` copies them (asserting `intrinsic ⇒ !provider.empty()`)
  + the 2a generator emits them ONLY for `[op.native]` ops (every prior generated file stays byte-identical — drift
  clean; only the `test` dialect, which declares a native binding, churned) + a validator test pins the emission.
  ⛔ `native_determinism` was NOT promoted — no runtime consumer yet (the deadline-attr anti-pattern) → CEIR-13c.
- **The `crd-ceir-cook` bridge** (new `engine/ceir-cook/` → crd-ceir + crd-resources + crd-render-asset-core):
  - `cook_program` VERIFIES first (unregistered → structure → domain → token → borrow → §34 recursion; mode-violation
    SKIPPED — session state, not module validity), then serialize + hash + deps + package into a CRDR container of
    type `'CEIR'`: a `'META'` render-asset `CookedHeader` (the §107 iface + content hash + type/schema a loader
    validates), a `'CEIR'` program blob, a `'CDEP'` §106 dependency chunk.
  - `cook_program_text` (§105 source) parses then cooks — text ≡ builder produce byte-identical hashes (the
    no-privileged-path property, §121; tested).
  - `read_program` round-trips + validates (magic/type/schema); a MISSING or unparseable `'CDEP'` is `BadDeps`, never
    a silent "no dependencies" (the default-empty scar — under-invalidation at 7c).
  - REUSES the RAF cooked-envelope framework (`CookedHeader` / `interface_hash_of` / `content_hash_of` /
    `RuntimeSlot`) + `AssetType::Program` (CLAIMED for the §105 CookedProgramAsset — no cooker produced it before).

**The headline — cross-Context purity.** Cook in Context A → read into a FRESH Context B → recompute BOTH hashes on
B's deserialized module → they equal the header fields (no intern-state leak — the 3a dirty-Context precedent +
the declared-header-words-validated scar). This is the test that a hash leaking intern state would fail.

**The four-edit §107 matrix (the crd-ceir test):** (a) a body-constant edit → content hash changes, interface hash
stable; (b) a signature edit → both change; (c) a function reorder → neither's interface changes (canonical by
name); (d) adding a state cell → the interface hash changes (the migration schema).

**Invariants.** I5 stays green AS-IS — crd-ceir gained no link edge (the cook is entirely in the bridge). I3 was
HARDENED: `+crd/resources` in the forbidden-include regex, so crd-ceir may never include the CRDR container layer
either (symmetry with the existing `crd/renderasset` ban).

**Deferrals — pinned to real rows.** `ckir_refs` (KernelRef asset deps) → CEIR-10 (no CKIR op exists yet; the schema
slot is present). `native_determinism` → CEIR-13c. RuntimeSlot/handle model → CEIR-7b (its row). The persistent
CookDb / `register_cook_handler` file integration → CEIR-7c (its detect→cook loop is the first consumer). §105 asset
classes beyond CookedProgramAsset + the `engine://`/`app://`/`runtime://` namespaces remain band-7 scope (7b/7c).
`ResourceId{0, asset_id}` is provisional — 7b decides the real ResourceId↔AssetId mapping. ⚠ The §107 CAPABILITY
CONTRACT field has NO owning tracker row — this is FLAGGED for the user (a decision, not a route I can invent); the
interface hash folds it in when a home exists (a recook, honest).

**Build notes for the user (two bites this slice hit):**
- `engine/ceir/generated/` is UNTRACKED in git — the `test`-dialect regen churn will NOT appear in the commit; the
  `crd-ceir-opgen-drift` ctest (regenerate-and-compare) is the enforcement, not git tracking.
- `tests/ceir/CMakeLists.txt` lists its sources EXPLICITLY (not globbed) — a new test file compiles NOWHERE until
  added to the `add_executable` list (the "No test cases matched" symptom). `tests/ceir-cook` is a new subdir wired
  into `tests/CMakeLists.txt`; `engine/ceir-cook` into the top-level `CMakeLists.txt`.

**Gate.** crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests **226/226 ctest** (209 + 7 `[program-asset]`
+ 10 `[cook]`) on **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** + LLVM-20 tidy (all changed files)
+ GCC `-Werror=switch` (the `CookError`/`ReadError` switches) + `crd-ceir-opgen-{drift,validator}` (52 py) +
`crd-ceir-invariants` (I5 as-is, I3 +crd/resources) + a full **win-debug all-targets build** (no crdr.hpp consumer
broke) + invariants. No binary version bump. Next = CEIR-7b (RuntimeSlot/handle model over crd-resources —
generation-safe handles; §105).

## Proposed commit — CEIR-7a (user commits; NO AI trailer)

```
feat(ceir-7a): the 'CEIR' cook -- interface hash, dependency record, verified packaging

- crd-ceir core (no new edge): interface_hash (s107) = FNV over a canonical projection
  (exported funcs sorted by name; structural param/result types -- cross-Context stable;
  the 5c transitive effect set; the module-wide s20 state schema). An impl-only edit keeps
  it; a signature/effect/state edit changes it (the four-edit matrix). collect_dependencies
  (s106) = external calls + intrinsics + providers, schema-driven via op_info (never
  name-sniffing). find_unregistered_op = the strict cook check (EMPTY != UNKNOWN).
- OpInfo/OpSpec promotion: intrinsic + native_provider promoted from OpSchema to the runtime
  OpInfo (the determinism/domain precedent) so the dep collector reads them; the generator
  emits them only for [op.native] ops (drift-safe -- only the test dialect churned) + a
  validator test. native_determinism NOT promoted (no consumer) -> CEIR-13c.
- crd-ceir-cook (NEW bridge: crd-ceir + crd-resources + crd-render-asset-core): cook_program
  verifies (unregistered/structure/domain/token/borrow/recursion) then packages a CRDR
  container (type 'CEIR': a 'META' render-asset CookedHeader + a 'CEIR' program blob + a
  'CDEP' dependency chunk). cook_program_text: text == builder hashes (no privileged path).
  read_program round-trips + validates; a missing/unparseable 'CDEP' is BadDeps, never a
  silent no-deps. Reuses the RAF CookedHeader envelope + AssetType::Program.
- Invariants: I5 green as-is (crd-ceir gained no edge -- the cook is in the bridge); I3
  hardened (+crd/resources). Deferrals to real rows: ckir_refs -> CEIR-10; RuntimeSlot ->
  7b; CookDb/register_cook_handler -> 7c; the s107 capability-contract field has no owning
  row (flagged for the user).
- tests: test_program_asset (7 -- the four-edit s107 matrix + cross-Context + deps +
  unregistered) + test_program_cook (10 -- cross-Context round-trip, s107-through-the-blob,
  deps survival, text==builder, reject unregistered/structure/recursion, BadDeps, WrongType,
  BadContainer) + test_opgen +1 (native emission).

Gated: crd-ceir-tests + crd-ceir-host-tests + crd-ceir-cook-tests 226/226 ctest on win-debug
+ win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen
drift/validator (52 py) + crd-ceir-invariants + a full win-debug all-targets build. No binary
version bump.
```
