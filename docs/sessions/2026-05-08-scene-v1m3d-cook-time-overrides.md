# 2026-05-08 — Phase 3.0 v1m3d: Cook-time `overrides = [...]` → OOVR chunk

**Status at start:** Phase 3.0 v1m3c shipped. Nested öbek references (`obek = "..."` per-entity) with eager flatten + ChildOf splice. 12 cooker tests; six-config 784/784 / 781 release.

**Status at end:** v1m3d shipped — closes v1m3 entirely. Cook-time `overrides = [...]` block at top level of an `.obek.toml` file bakes into the OOVR chunk; runtime auto-applies these BEFORE caller-supplied patches (so caller patches still win — caller is deepest). 15 cooker tests (+3); six-config 787/787 / 784 release / 17 smokes.

**v1m3 ALL FOUR SUB-SLICES COMPLETE.** Full ObekCooker pipeline shipped. ADR-0058 pillars 3 (overrides), 10 (cooker), 11 (extends + nested chain), 12 (sub-instance composition) lit up.

---

## Goal of this session

Land the cook-time half of ADR-0058 pillar 3 (override patches):
1. `ObekOverrideRecord` (24 bytes) on-disk struct.
2. `ObekArtifactBuilder::add_override(file_idx, fourcc, field_offset, payload)` API + `PendingOverride` storage.
3. OOVR chunk emit (record array + payload pool).
4. `ObekLoader` parses OOVR into `ObekResource::cook_override_records` + `cook_override_payload_pool`.
5. `World::instantiate_obek` auto-applies OOVR records BEFORE caller-supplied overrides — caller wins on overlap.
6. Cooker parses TOML `overrides = [...]` array at top level: each entry `{entity, component, value}`. Entity name resolved via `accumulated_names`. Component name resolved via reader registry. Value parsed by the registered TOML reader (whole-component override).

## What shipped

### Modified

```
engine/scene/include/crd/scene/obek.hpp        ObekOverrideRecord struct (24 bytes) +
                                               ObekArtifactBuilder::add_override API +
                                               PendingOverride internal struct +
                                               ObekResource.cook_override_records / payload_pool spans.
engine/scene/src/obek.cpp                       ObekArtifactBuilder.add_override impl;
                                               OOVR chunk emit in build();
                                               OOVR chunk parse in ObekLoader.load();
                                               instantiate_obek Step D2: auto-apply cook-time
                                               overrides BEFORE Step E (caller patches).
tools/asset_cooker/src/cook_handlers/obek.cpp   cook_inline reordered to construct
                                               ObekArtifactBuilder earlier so override parsing
                                               can call builder.add_override; new override-array
                                               parser (entity name → file_idx via accumulated_names
                                               linear scan, component name → reader, value parsed
                                               by reader to whole-component bytes).
                                               Removed top-level `overrides` rejection from
                                               apply_table_to_world (silent skip).
tests/scene_cooker/test_obek_cooker.cpp         3 new cases (cook-time override applies,
                                               caller wins precedence, unknown entity error).
                                               15 cases total (was 12; +3).
```

### Architectural decisions pinned

1. **Whole-component overrides only at v1m3d.** TOML schema is `{ entity, component, value }`; `value` is the FULL component value parsed by the registered reader. `field_offset = 0`, `payload_size = sizeof(Component)` always. Field-level slicing (`{ component = "X", field = "translation", value = [...] }`) reserved for v1m5+ when the cooker can route field-name → offset/size through a registered schema. Trade-off: simpler v1m3d cook surface; cooked OOVR record is ~24+sizeof(Component) bytes vs ~24+sizeof(Field) for field slicing. Acceptable for v1m3d's primary use case ("override the wheel's tint").

2. **OOVR has its own self-contained payload pool** (does NOT reuse OSTR or any shared pool). Same pattern as OCHN — keeps each chunk parseable independently.

3. **Cook-time overrides apply BEFORE caller-supplied** in `instantiate_obek`. Caller-supplied patches win on overlap (caller is deepest in the resolution stack — ADR-0058 pillar 4). The two paths share the same memcpy logic; cook-time uses `ObekOverrideRecord` (struct on disk), caller uses `ObekOverride` (runtime view). Both end up writing `payload` bytes at `field_offset` of the target component.

4. **`overrides_applied` and `overrides_skipped` count BOTH cook-time and caller-supplied patches.** Single counters means the caller can know "1 cook-time + 1 caller = 2 applied" without separate tracking. Documented behavior; tests verify both axes.

5. **Top-level `overrides` parsing happens in `cook_inline`, not in `apply_table_to_world`.** Reason: only the CURRENT öbek's overrides count; ancestors' overrides (from extends chain) are part of THEIR cook output, not this one. By keeping override parsing at the cook_inline level (which has the original top-level TOML), it never accidentally pulls in ancestors' or nested öbeks' overrides.

6. **Entity-name lookup at cook time uses `accumulated_names` linear scan.** O(N) per override entry; öbek N typically << 100 with handful of overrides → trivial cost. v1m5+ may build a HashMap if profiling shows otherwise.

7. **Component-name lookup uses the reader registry** (`find_reader_in(m_impl->readers, comp_view)`). The reader IS the schema for this v1m3d. Same registration path as inline component fields — no separate "override schema" to maintain.

8. **TOML scoping bites authors.** `overrides = [...]` MUST appear BEFORE any `[entity.NAME]` section, or TOML scoping rules turn it into a per-entity field (which is reserved for v1m5+ "per-instance overrides on nested öbek refs"). Tests verify both placements; documentation in the ADR will call this out.

### Test matrix (15 cases / scene-cooker)

| # | Case | What |
|---|---|---|
| 1 | Empty obek TOML cooks | Sanity floor |
| 2 | Single-entity obek round-trips Transform | Happy path |
| 3 | ChildOf hierarchy + reparent | Built-in relations |
| 4 | Per-entity overrides reserved (v1m5+) | Reserved-key error |
| 5 | Determinism: bit-equal bytes | FNV-equivalent verification |
| 6 | Single extends merges parent components | v1m3b |
| 7 | Chain of 3 extends resolves deepest-first | v1m3b chain |
| 8 | extends cycle detection | v1m3b cycle |
| 9 | extends without resolver error | v1m3b no-resolver |
| 10 | Nested obek splices entities + ChildOf | v1m3c happy path |
| 11 | Two-level nested obek | v1m3c deep |
| 12 | Nested obek cycle detection | v1m3c cycle |
| 13 | **Cook-time overrides bake into OOVR + apply** | **v1m3d happy path** |
| 14 | **Caller override wins over cook-time** | **v1m3d precedence** |
| 15 | **Override pointing at unknown entity emits error** | **v1m3d not-found** |

### Stale .obj gotcha (recurring CLAUDE.md issue)

When I added `m_pending_overrides` to `ObekArtifactBuilder` and `cook_override_records` / `cook_override_payload_pool` to `ObekResource`, the win-release config's incremental build kept stale `.obj` files compiled against the OLD struct sizes. Two pre-existing tests (v1m1's "Multiple overrides applied in order" and v1m2's "OCHN absent when no chain dependencies recorded") segfaulted / failed in release. **Fix: `cmake --build --preset win-release --target clean && cmake --build --preset win-release` →** all 784 tests pass. The CLAUDE.md guidance ("after header changes that add members to large classes, always Rebuild Solution / clean") applies.

### Six-configuration green (post-v1m3d, 2026-05-08)

- win-debug:          787/787
- win-relwithdebinfo: 787/787
- win-release:        784/784   (after clean rebuild)
- win-asan:           787/787
- win-clang-cl:       787/787
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## v1m3 closure — full ObekCooker pipeline now ships

```
.obek.toml ──┬── extends = "..." (v1m3b chain walking + cycle detection + OCHN Extends entries)
             │
             ├── [entity.NAME]
             │   ├── obek = "..." (v1m3c nested ref; eager flatten; ChildOf splice; OCHN Nested entries)
             │   ├── Transform = { ... } (v1m3a built-in reader)
             │   ├── ChildOf = "..." / Targets = ["..."] (v1m3a built-in relations)
             │   └── (user-defined components via register_component_reader)
             │
             └── overrides = [{ entity, component, value }, ...] (v1m3d cook-time → OOVR chunk;
                                                                  applied at instantiate before
                                                                  caller-supplied patches)
                                                                                ↓
                                                              cooker → OBEK CRDR bytes
                                                                                ↓
                                                                         ObekLoader
                                                                                ↓
                                                       World::instantiate_obek(parent, overrides)
                                                                                ↓
                                                      live ECS entities, hierarchies, components
```

All four sub-slices shipped:
- ✅ v1m3a — substrate (TOML → flat OBEK CRDR)
- ✅ v1m3b — extends chain
- ✅ v1m3c — nested öbek refs
- ✅ v1m3d — cook-time overrides

---

## Files touched

```
engine/scene/include/crd/scene/obek.hpp                           modified
engine/scene/src/obek.cpp                                          modified
tools/asset_cooker/src/cook_handlers/obek.cpp                      modified
tests/scene_cooker/test_obek_cooker.cpp                            modified (+3 cases; 15 total)
docs/sessions/2026-05-08-scene-v1m3d-cook-time-overrides.md       created (this file)
CONTEXT.md                                                          updated (v1m3d milestone, v1m3 closed)
```

---

## Next: v1m4 — InheritPolicy with full CoW backend

ADR-0058 pillar 5. `InheritPolicy` enum (Override default, Inherit transparent CoW, DontInherit). Per-entity per-component "owned vs shared" flag bit added to ArchetypeChunkStorage and SparseSetStorage. Write paths intercept and copy-on-first-write. Reference counting on shared backing. DontInherit skips the component on instantiation. ~6 tests covering all three policies including the CoW interception.
