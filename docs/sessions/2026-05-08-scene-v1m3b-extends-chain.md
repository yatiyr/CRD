# 2026-05-08 — Phase 3.0 v1m3b: ObekCooker `extends` chain resolution

**Status at start:** Phase 3.0 v1m3a shipped. ObekCooker substrate (TOML → OBEK CRDR, flat öbeks). 7 cooker tests; six-config 779/779 / 776 release.

**Status at end:** v1m3b shipped — `extends = "..."` chain resolution at cook time with depth-unbounded recursive walk (max-depth = 64 hard cap), path-based cycle detection, deepest-first apply, and OCHN entry emission per chain link. 10 cooker tests (+3 net over v1m3a; one reservation test removed, four new positive tests added). Six-config 782/782 / 779 release / 17 smokes.

---

## Goal of this session

Light up the `extends = "..."` half of ADR-0058 pillar 11 ("hot-reload graph-aware") at the TOML cooker level:
1. Caller-provided file resolver (`ObekFileResolverFn`) — decouples the cooker from the filesystem so tests can use in-memory maps.
2. Iterative chain walk: parse → check `extends` → resolve parent → cycle-check → push to ancestor stack → loop.
3. Depth-first apply: walk ancestors deepest-first, then apply current TOML last. UPSERT semantics in the storage backend = deepest-extends-wins per field.
4. Single shared temp World across all chain steps — entity names dedupe by hash; subsequent steps re-mention names → reuse the existing EntityId.
5. OCHN entries emitted per ancestor (deepest first, matching apply order). `kind = Extends`. `content_hash` = FNV-1a 64 of the parent's TOML text.

## What shipped

### Modified

```
tools/asset_cooker/include/crd/cooker/obek_cooker.hpp     ObekFileResolverFn typedef +
                                                          ObekCookContext.file_resolver / .file_resolver_ud
tools/asset_cooker/src/cook_handlers/obek.cpp             refactored cook_inline + extracted
                                                          apply_table_to_world helper +
                                                          extends-chain walker (~210 LOC added net)
tests/scene_cooker/test_obek_cooker.cpp                   removed v1m3b reservation test (1 case);
                                                          added 4 positive tests (single, chain-3,
                                                          cycle, no-resolver). 10 cases total.
```

### Architectural decisions pinned

1. **`apply_table_to_world` is a free helper, not a member.** Extracted from cook_inline so the chain walker can call it once per step without re-parsing context. Takes `const Array<ReaderEntry>&` rather than `const ObekCooker::Impl&` to avoid coupling the helper to ObekCooker's private internals — cleaner architectural seam.

2. **Iterative chain walk, not recursive.** Linear stack of ancestors + visited path-hash set. No recursion, no stack risk on adversarial input. Hard cap at depth 64 (cycle detection + max-depth guard both fire; whichever first).

3. **Cycle detection by FNV-hashed path string.** Same hash family as `containers::fnv1a_64`; rolled inline so the cooker doesn't take a dep on the runtime String view overload. Visited set is a flat `Array<u64>` with linear search — chain depth is small (<32 typical, 64 max) so O(n²) is fine.

4. **Single shared temp World across all chain steps.** Cleaner than merging TOML tables — apply each chain step to the World, components UPSERT (later wins per field), entity names dedupe by hash (re-mention → reuse EntityId), relations install via `add_relation_via_id` (replaces existing target). Storage backend's UPSERT semantics ARE the "deepest wins" semantics — no separate merge logic needed.

5. **`accumulated_names` is a parallel data structure to `name_to_entity`.** Only first occurrence of a name pushes to `accumulated_names`; subsequent occurrences reuse the EntityId without growing the array. The post-cook matrix-bake pass walks `accumulated_names` to mark Transform-bearing entities dirty. Matches v1l's per-entity-mark-then-step pattern.

6. **OCHN entry order matches apply order: deepest first.** Reflects the dependency graph; hot-reload watcher (v1m5) can re-cook on any upstream change by walking the chain in arrival order. Determinism preserved: same chain → same OCHN bytes.

7. **`content_hash` is FNV-1a 64 of the source TOML text bytes**, not the cooked-bytes hash. Source-byte hash detects the change earliest (before any expensive cook); cooked-byte hash would force an extra cook just to compute the watcher key. A future optimisation could cache cooked-byte hashes in the ResourceManager metadata.

8. **Resolver signature copies the file text into a caller-allocated `String`.** Avoids ownership ambiguity (resolver returns owned bytes; caller owns lifetime). Simple, explicit, allocator-aware.

### Test matrix delta (v1m3a → v1m3b)

| # | Case (v1m3a) | Status |
|---|---|---|
| 1 | Empty obek TOML | unchanged |
| 2 | Single-entity Transform round-trip | unchanged |
| 3 | ChildOf hierarchy + reparent | unchanged |
| 4 | Top-level extends rejected (v1m3b reservation) | **REMOVED** (extends now works) |
| 5 | Per-entity `obek` rejected (v1m3c reservation) | unchanged |
| 6 | Per-entity `overrides` rejected (v1m3d reservation) | unchanged |
| 7 | Determinism: identical TOML → bit-equal bytes | unchanged |

| # | Case (v1m3b new) | What |
|---|---|---|
| 8 | Single extends merges parent components into child | Base + child. Child's translation = [10,20,30] wins over base's [1,2,3]. OCHN reports 1 dependency. |
| 9 | Chain of 3 extends resolves deepest-first | Grandparent (1,1,1) → parent (2,2,2) → child (3,3,3). Child wins. OCHN reports 2 dependencies. |
| 10 | extends cycle detection emits error | a.extends=b, b.extends=a. Cooker emits "cycle detected" error; bytes empty. |
| 11 | extends without resolver emits error | extends present but ctx.file_resolver = null. Cooker emits "file_resolver" error. |

### Six-configuration green (post-v1m3b, 2026-05-08)

- win-debug:          782/782
- win-relwithdebinfo: 782/782
- win-release:        779/779
- win-asan:           782/782
- win-clang-cl:       782/782
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## Files touched

```
tools/asset_cooker/include/crd/cooker/obek_cooker.hpp                      modified
tools/asset_cooker/src/cook_handlers/obek.cpp                              modified (refactored;
                                                                           net +210 LOC)
tests/scene_cooker/test_obek_cooker.cpp                                    modified (-1, +4 cases;
                                                                           net +3)
docs/sessions/2026-05-08-scene-v1m3b-extends-chain.md                      created (this file)
CONTEXT.md                                                                  updated (v1m3b milestone)
```

---

## Next: v1m3c — nested öbek references (`obek = "..."` inside an entity)

Per-entity `obek = "..."` field. Reuses the same `ObekFileResolverFn` substrate: when an entity's TOML has `obek = "..."`, recursively cook the referenced öbek into an in-memory ObekResource, then splice its entity table into the parent öbek's entity table at instantiation time. Eager flatten by default — the parent öbek's cooked OBEK bytes contain the full expanded entity graph. Sub-instance source_obek_root tracking via per-entity reserved-flags slot in OETB. OCHN entries emitted with `kind = Nested`. ~3 tests.
