# 2026-05-08 — Phase 3.0 v1m3a: ObekCooker substrate (TOML → OBEK, flat öbeks)

**Status at start:** Phase 3.0 v1m2 shipped. Runtime override patches + OCHN format substrate. 14 öbek tests; six-config 772/772 / 769 release.

**Status at end:** v1m3a shipped — `ObekCooker` substrate. TOML → OBEK CRDR pipeline riding on the v1l `SceneCooker` reader-registry pattern. Flat öbeks (no extends, no nested, no overrides) cook + load + instantiate cleanly. 7 cooker tests; six-config 779/779 / 776 release / 17 smokes.

**v1m3 sub-slicing locked:**
- ✅ **v1m3a** (this slice) — substrate.
- ⏳ **v1m3b** — `extends = "..."` chain resolution + cycle detection + deepest-wins merge + OCHN entries.
- ⏳ **v1m3c** — `obek = "..."` nested öbek refs + recursive cook + eager flatten + sub-instance tracking.
- ⏳ **v1m3d** — `overrides = [...]` cook-time patches → OOVR chunk emission.

---

## Goal of this session

Land the bottom of the öbek-side TOML cooker (the analogue of v1l's `SceneCooker`):
1. Public class `crd::cooker::ObekCooker` with `register_builtin_readers()`, `register_component_reader<T>()`, `register_relation_reader<Tag>()`, `cook_inline()`.
2. Free function `obek_cooker_inline(toml_text, ctx, errors_out)`.
3. `ObekCookContext` with `id`, `allocator`, `obek_root_id` (stamped into `OINF`).
4. Three-pass cook: collect entities (document order) → apply components (alphabetical key order, advisor pin #8 determinism) → install relations (FNV-keyed name resolver). Plus the `TransformPropagation::step()` matrix-bake pass.
5. Multi-error accumulation matching `SceneCooker`.
6. Reserved-key rejection at v1m3a: top-level `extends`, per-entity `obek`, per-entity `overrides` all emit a clear "reserved for v1m3X" diagnostic. Lighting these up as the corresponding sub-slices ship.

## What shipped

### New files

```
tools/asset_cooker/include/crd/cooker/obek_cooker.hpp     ~135 LOC
tools/asset_cooker/src/cook_handlers/obek.cpp              ~440 LOC
tests/scene_cooker/test_obek_cooker.cpp                    ~285 LOC, 7 cases
```

### Modified

- `tests/scene_cooker/CMakeLists.txt` — added `test_obek_cooker.cpp` to the discover list.

### Architectural decisions pinned

1. **`ObekCooker` is a parallel class, not a `SceneCooker` subtype.** The reader-registry shape matches (same `ComponentTomlReaderFn` typedef, same `register_*_reader` template grammar, same `register_builtin_readers()` body), but the cook pipeline diverges in v1m3b/c/d (extends chain, nested öbeks, OOVR). Rather than retrofit virtual hooks onto `SceneCooker`, ObekCooker is its own class with its own cook_inline. Code overlap with SceneCooker for the substrate (~80%) is the price; the clean divergence point is worth it.

2. **Built-in reader registration is identical to `SceneCooker`.** Transform + the six built-in relations register with the exact same names, FourCCs, versions, and reader function pointers. Content authored in either format (`.scene.toml` or `.obek.toml`) uses the same reader code — content teams don't re-wire anything to switch formats.

3. **Reserved-key rejection is explicit at v1m3a.** Top-level `extends`, per-entity `obek`, per-entity `overrides` all trigger an error message that names the sub-slice that will light them up ("reserved for v1m3b"). Better than silent ignore — an authoring user gets a helpful message, not a mystifyingly empty cook output.

4. **Per-entity reserved-key checks happen in pass 2 *before* the unknown-key error.** If a user writes `obek = "..."` on an entity, the "reserved for v1m3c" message fires; if they write some random unknown key, the "unknown component or relation key" message fires. Order matters because v1m3c's `obek` IS a known reserved keyword, and we want it to surface its own dedicated message.

5. **`obek_root_id` flows through the cooker into OINF.** The cooker's `ObekCookContext::obek_root_id` becomes the `ObekArtifactBuilder` constructor argument, ends up in `OINF.obek_root_id`, and `ObekResource::obek_root_id` exposes it on load. Stable cross-cook identity for `ObekEntityGuid` and the future hot-reload watcher.

6. **Determinism preserved across the pipeline.** Same TOML text + same `obek_root_id` → same OBEK bytes (verified by `Determinism: identical obek TOML produces bit-equal bytes` test). The cooker's iteration orders (entity insertion in TOML, alphabetical field order, registry-order relations, slot-iteration in builder) compose to keep this guarantee.

### Test matrix (7 cases in tests/scene_cooker)

| # | Case | What |
|---|---|---|
| 1 | Empty obek TOML cooks to a valid OBEK with zero entities | Sanity floor |
| 2 | Single-entity obek TOML round-trips Transform | Basic happy path through cooker → loader → instantiate_obek |
| 3 | ChildOf hierarchy round-trips and reparents under instantiate parent | Cooker emits ORLS → instantiate_obek installs ChildOf(root, anchor) |
| 4 | Top-level `extends` rejected with v1m3b reservation message | Reserved-key error path |
| 5 | Per-entity `obek` key rejected with v1m3c reservation message | Reserved-key error path |
| 6 | Per-entity `overrides` key rejected with v1m3d reservation message | Reserved-key error path |
| 7 | Determinism: identical obek TOML produces bit-equal bytes | FNV-equivalent verification (memcmp) |

### Six-configuration green (post-v1m3a, 2026-05-08)

- win-debug:          779/779
- win-relwithdebinfo: 779/779
- win-release:        776/776
- win-asan:           779/779
- win-clang-cl:       779/779
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## What's deliberately NOT in v1m3a (next sub-slices)

- TOML `extends = "..."` recursive resolution + cycle detection + deepest-wins merge + OCHN emission — **v1m3b**.
- TOML per-entity `obek = "..."` nested öbek refs + recursive cook + eager flatten into parent's entity table + sub-instance source_obek_root tracking — **v1m3c**.
- TOML `overrides = [...]` cook-time override patches → OOVR chunk emission — **v1m3d**.
- `ObekFileResolverFn` (caller-provided callback for loading referenced TOML files) — needed by both v1m3b and v1m3c; introduced in v1m3b.

---

## Files touched

```
tools/asset_cooker/include/crd/cooker/obek_cooker.hpp                  created (~135 LOC)
tools/asset_cooker/src/cook_handlers/obek.cpp                          created (~440 LOC)
tests/scene_cooker/CMakeLists.txt                                       modified
tests/scene_cooker/test_obek_cooker.cpp                                 created (~285 LOC, 7 cases)
docs/sessions/2026-05-08-scene-v1m3a-obek-cooker-substrate.md          created (this file)
CONTEXT.md                                                              updated (v1m3a milestone)
```

---

## Next: v1m3b — `extends` chain resolution

`ObekFileResolverFn` typedef: caller-provided `(path, alloc, out_text, ud) → bool` for loading referenced TOML files. Tests pass an in-memory map; production cooker passes a filesystem reader. Iterative chain resolution (parse → check `extends` → load parent → push to deepest-first chain → loop). Cycle detection via path hash set. Apply each chain step to the temp World in deepest-first order; later steps UPSERT-override earlier ones via the existing storage backend. OCHN entries emitted per chain link with content-hash. ~3 tests.
