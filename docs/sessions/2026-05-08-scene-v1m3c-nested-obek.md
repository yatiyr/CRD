# 2026-05-08 — Phase 3.0 v1m3c: Nested öbek references (`obek = "..."` per-entity)

**Status at start:** Phase 3.0 v1m3b shipped. ObekCooker `extends` chain resolution + cycle detection + OCHN entries. 10 cooker tests; six-config 782/782 / 779 release.

**Status at end:** v1m3c shipped — per-entity `obek = "..."` recursive cooking. Eager flatten by default: nested öbek's full entity graph splices into the parent's cooked OBEK bytes. Splice via `ChildOf(nested_root → parent_entity)` for nested entities that have no ChildOf in their own source. Two-level (and arbitrary-depth) nesting works recursively. Cycle detection unified across extends + nested. Six-config 784/784 / 781 release / 17 smokes; 12 cooker tests (+2 net: -1 stale reservation, +3 new).

---

## Goal of this session

Land the second half of ADR-0058 pillar 11 (extends + nested as the dependency graph) plus pillar 12 (sub-instance composition):
1. Per-entity `obek = "..."` field in TOML — entity becomes a placeholder for the referenced öbek's flattened entity graph.
2. Caller-provided `ObekFileResolverFn` (already in v1m3b) loads the referenced öbek's source TOML.
3. Recursive cook of nested öbeks — including their own extends chains, nested öbeks, etc.
4. Splice: nested entities without a ChildOf get `ChildOf(nested_root → placeholder_entity)` installed at cook time.
5. Nested entity names are scoped to the nested öbek (don't leak into parent's name resolution; parent's `name_to_entity` is saved and restored).
6. Cycle detection unified across extends + nested via single shared `visited_path_hashes` set.
7. OCHN entries with `kind = Nested` populated per nested reference.

## What shipped

### Modified

```
tools/asset_cooker/src/cook_handlers/obek.cpp     refactored cook_inline + apply_table_to_world.
                                                  Introduced RecCtx struct (bundles ctx, world,
                                                  name_to_entity, accumulated_names, ochn,
                                                  visited_path_hashes, readers, errors, depth) to
                                                  thread state cleanly through recursive descent.
                                                  walk_and_apply_chain() helper handles the
                                                  extends-chain-walk-then-apply for ONE öbek file;
                                                  apply_table_to_world calls it recursively for
                                                  per-entity `obek = "..."` references. Per-entity
                                                  obek path resolves via ctx.file_resolver, name
                                                  table is saved/restored to scope nested names,
                                                  splice via ChildOf installs after recursive
                                                  apply. ~150 LOC net added.
tests/scene_cooker/test_obek_cooker.cpp           removed v1m3c reservation test (1 case, stale);
                                                  added 3 nested-öbek tests (single splice,
                                                  two-level nesting, nested cycle detection).
                                                  12 cases total (was 10; net +2).
```

### Architectural decisions pinned

1. **Recursion via `RecCtx` struct rather than threaded parameters.** All shared state (world, name table, accumulated names, OCHN, visited hashes, readers, errors, depth) lives in one bundle passed by reference. Cleaner signatures + a single source of truth for recursion-spanning state.

2. **`walk_and_apply_chain` is the recursive primitive.** Handles ONE öbek file's complete cook: walks its extends chain (collecting ancestors), then applies each chain step + body via `apply_table_to_world`. Top-level cook calls it with depth=0; per-entity nested-öbek refs call it again from inside `apply_table_to_world` with `depth + 1`. Max depth = 64 hard cap (matches extends-chain max).

3. **Nested öbek's name table is scoped via save/restore.** Before recursing on a nested öbek, the parent's `name_to_entity` is moved into a local backup and replaced with an empty table. After the nested cook, the original is moved back. Means: nested entity names like `[entity.tire]` don't collide with parent's `[entity.tire]` if both author chose the same names. Names are purely for cook-time relation resolution within their own öbek scope.

4. **Splice via ChildOf at cook time, not load time.** After the nested öbek's entities spawn into the temp World, the cooker walks the newly-added entities (via `accumulated_names[before_count..]`) and installs `ChildOf(nested_e → placeholder_entity)` for any nested entity that has no ChildOf already. This places the nested öbek's roots under the parent's placeholder; the cooked OBEK bytes carry the correct hierarchy without runtime fix-up.

5. **Cycle detection unified across extends + nested.** Single `visited_path_hashes` set in `RecCtx`. If you `extends` something, OR `obek` something, OR transitively reach the same path via either, you cycle. Simple visited set semantics — doesn't pop on exit, so diamond sharing of the same dep path errors as "cycle" (acceptable for v1m3c; documented as a debt note for v1m5+ if real-world content needs DAG support).

6. **Per-entity `obek = "..."` is processed BEFORE component fields.** When an entity has both `obek = "..."` AND e.g. `Transform = {...}`, the cooker first recurses into the nested öbek (spawning its entities under the placeholder), THEN applies the parent's own components to the placeholder. This means the placeholder's `Transform` overrides any Transform the nested öbek's TOML might also try to put on the placeholder (it can't — placeholder is the parent entity, not part of nested öbek's table).

7. **OCHN entries are `kind = Nested` for nested refs**, `kind = Extends` for extends links. They're appended to the same `rc.ochn` array; their positions reflect the order the cooker encountered them. The hot-reload watcher (v1m5) walks both kinds when deciding whether to re-cook on a dependency change — no semantic difference, both are upstream dependencies.

8. **Top-level `walk_and_apply_chain` return value is discarded** (cast to void). Errors accumulate in `rc.errors`; the function returns false on hard failures (bad TOML, cycle, missing resolver) but cook_inline checks `errors.size()` for the final decision. Both signals point the same direction.

### Test matrix delta (v1m3b → v1m3c)

| # | Case | Status |
|---|---|---|
| Per-entity obek key rejected (v1m3c reservation) | **REMOVED** (lit up in v1m3c) |
| Nested obek reference splices entities and parents under placeholder | **NEW** — vehicle (body + wheel_fl placeholder) + wheel (hub + tire) = 4 entities; OCHN reports 1 Nested dependency; some entity is a child of another (verifies splice ChildOf installed) |
| Two-level nested obek vehicle wheel tire-pattern | **NEW** — body + wheel + tire + tread = 4 entities; OCHN reports 2 Nested entries (middle + inner) |
| Nested obek cycle detection emits error | **NEW** — a.obek references b.obek references a.obek; cycle detected, bytes empty, "cycle" in error |

### Six-configuration green (post-v1m3c, 2026-05-08)

- win-debug:          784/784
- win-relwithdebinfo: 784/784
- win-release:        781/781
- win-asan:           784/784
- win-clang-cl:       784/784
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## What's deliberately NOT in v1m3c (next sub-slice)

- `overrides = [...]` cook-time override patches → OOVR chunk emission — **v1m3d** (closes v1m3).
- Sub-instance `source_obek_root` per-entity attribution in OETB. Currently the cooker knows which entities came from which nested öbek (via the recursion stack), but the OETB doesn't store that attribution. v1m5 may add it via a reserved bit + content-hash side table.
- Diamond-graph DAG support (a includes b, a includes c, b includes c, c is shared). v1m3c's "visited as cycle" model rejects diamonds; future work could distinguish "currently in chain" (cycle) from "previously visited" (diamond OK).

---

## Files touched

```
tools/asset_cooker/src/cook_handlers/obek.cpp                      modified (~150 LOC net)
tests/scene_cooker/test_obek_cooker.cpp                            modified (-1, +3 cases; net +2)
docs/sessions/2026-05-08-scene-v1m3c-nested-obek.md                created (this file)
CONTEXT.md                                                          updated (v1m3c milestone)
```

---

## Next: v1m3d — cook-time `overrides = [...]` → OOVR chunk emission

Closes v1m3. TOML allows `overrides = [...]` at top level for cook-time-baked override patches — useful when the öbek source itself wants to customize a nested öbek (e.g. vehicle.obek says "wheel_fl is wheel.obek BUT override the wheel's color"). Cook-time overrides bake into the OOVR chunk in the OBEK CRDR; runtime applies them at instantiate time as if they were passed via `instantiate_obek(parent, overrides)`. Schema validation against the öbek's component manifest. ~3 tests.
