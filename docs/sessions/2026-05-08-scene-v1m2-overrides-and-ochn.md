# 2026-05-08 — Phase 3.0 v1m2: Runtime override patches + OCHN format substrate

**Status at start:** Phase 3.0 v1m1 shipped 2026-05-08. Öbek substrate (ObekResource + ObekLoader + ObekArtifactBuilder + World::instantiate_obek) green at 766/766 / 763 release. Original v1m2 scope: extends chain + override patches.

**Status at end:** v1m2 shipped — runtime override patches (ADR-0058 pillar 3) + OCHN chunk format substrate (pillar 11). Six-config 772/772 / 769 release / 17 smokes. 14 öbek tests (+6 over v1m1).

**Strategic re-slicing:** the original v1m2 plan bundled "extends chain at TOML cook time" with "override patches at runtime." The TOML cooker work consolidates more naturally with v1m3 (nested öbek composition) into a single full-featured `ObekCooker`. v1m2 scope was reshaped to ship the runtime + format substrates that the v1m3 cooker needs in place. v1m3 expands accordingly — see CONTEXT.md / phase-3.0-scene-ecs.md / task #99.

---

## Goal of this session

Land ADR-0058 pillars 3 (override patches) and 11 (chain dependency tracking) at the runtime + format level, so v1m3's ObekCooker has the API surface to write into.

**Phase A — Runtime override patches:**
1. `ObekOverride` struct — typed runtime patch (file_idx + name + component_fourcc + field_offset + payload).
2. `World::instantiate_obek(res, parent, overrides)` — overload accepting `ConstSpan<ObekOverride>`.
3. Validation: file_idx in range, component_fourcc registered, field_offset + payload.size() within component bounds.
4. Symbolic-name fallback when `file_idx == kObekOverrideUseName`.
5. `overrides_applied` / `overrides_skipped` counters on `ObekInstantiation`.

**Phase B — OCHN format substrate:**
1. `ObekChainKind` enum (Extends / Nested).
2. `ObekChainEntryRecord` (24 bytes) — path_strp_offset + content_hash + kind + reserved.
3. `ObekArtifactBuilder::add_chain_dependency(path, content_hash, kind)` API.
4. OCHN chunk emission with self-contained path pool.
5. `ObekResource::chain_dependencies` exposed on load.

## What shipped

### Modified

```
engine/scene/include/crd/scene/obek.hpp     ~290 LOC (was 205) — ObekOverride,
                                                    ObekChainKind/EntryRecord,
                                                    ObekArtifactBuilder.add_chain_dependency,
                                                    ObekInstantiation override counters,
                                                    ObekResource.chain_dependencies span.
engine/scene/src/obek.cpp                    ~570 LOC (was 430) — override apply loop with
                                                    file_idx + name fallback, OCHN emit/parse.
engine/scene/include/crd/scene/world.hpp     instantiate_obek signature accepts overrides.
tests/scene/test_obek.cpp                    ~440 LOC (was 280) — 14 cases (+6 over v1m1).
```

### Architectural decisions pinned

1. **Runtime override patches are non-owning views.** `ObekOverride::payload : ConstSpan<u8>` — caller owns the bytes; the patch is a value type for the duration of the `instantiate_obek` call. Rationale: the patch bytes typically come from a SCEN-loaded scene that already has them in mapped memory, or from a stack-allocated value at the call site. Forcing the öbek system to own the bytes would either bloat the struct or require an allocator round-trip per patch.

2. **`field_offset` is a byte offset within the component**, not the ADR's full 32-bit field_path encoding. v1m2 ships only the 16-bit-offset half of the spec; the 8-bit element_idx + 8-bit kind bits are reserved as zeros in the OOVR disk format and unused at runtime. Adequate for any flat or POD-like component layout. Composite components with array fields will exercise the element_idx bits in v1m3+ when they appear.

3. **`component_fourcc == 0` is reserved** for relation overrides. v1m2 only applies component-field overrides; relation overrides (instance overriding the ChildOf target of a particular entity) are reserved for v1m3+ where nested-öbek graph mutation matters.

4. **Bounds check is mandatory.** `field_offset + payload.size()` is checked against `info->size`; out-of-range is a programming error in calling code. Debug: assert (catches authoring bugs in tests); release: count + skip (graceful in the field for forward-compat-driven content).

5. **Symbolic-name fallback uses linear OETB scan.** O(N) over the entity table. Acceptable for v1m2 (typical öbek N ≪ 1000, override count typically << N). v1m3+ may build a HashMap if cookers emit a sorted name index in OETB.

6. **OCHN has its own path pool, separate from OSTR.** Self-contained: parsing OCHN doesn't require parsing OSTR first. Trade-off: 2× pool storage if entity names and dep paths overlap (they typically don't). Decoding becomes trivially independent — useful for editor tools that only want to inspect dependencies without parsing the full öbek.

7. **Chain entries preserve cooker insertion order.** `ObekArtifactBuilder` records `m_pending_deps` in call order; the OCHN chunk emits them in that same order. Determinism: same cooker call sequence → same OCHN bytes (same as v1m1 builder).

8. **`ObekChainEntryRecord` is 24 bytes** (path_strp_offset + reserved + content_hash + kind + 7 reserved bytes). `path_strp_offset` is 32-bit (4 GB pool max), `content_hash` is 64-bit (FNV-1a 64), `kind` is one byte (Extends / Nested initially, six bits reserved for future kinds).

### Test matrix (14 cases now / scene-obek)

#### v1m1 baseline (8 cases)
Empty / single-entity / multi-entity / null parent / alive parent / independence / determinism / GUID stability.

#### v1m2 additions — Phase A (4 cases)
| # | Case | What |
|---|---|---|
| 9 | Override patch by file_idx replaces a Transform field | Happy path, byte memcpy at offsetof |
| 10 | Override skipped when file_idx out of range | Bounds-check forward-compat |
| 11 | Override skipped when component fourcc unregistered | Type-registry forward-compat |
| 12 | Multiple overrides applied in order | Multi-patch ordering + counts |

#### v1m2 additions — Phase B (2 cases)
| # | Case | What |
|---|---|---|
| 13 | OCHN chunk round-trips chain dependencies | 2 deps (Extends + Nested) survive cook → load |
| 14 | OCHN absent when no chain dependencies recorded | No-op path |

### Six-configuration green (post-v1m2, 2026-05-08)

- win-debug:          772/772
- win-relwithdebinfo: 772/772
- win-release:        769/769
- win-asan:           772/772
- win-clang-cl:       772/772
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## Why the re-slicing

The original v1m2 spec read "extends chain + override patches" as one slice. As I prepared the TOML cooker for öbek (riding on the v1l SceneCooker pattern), it became clear that:

- The cooker IS the natural home for `extends` chain resolution (TOML-level merge logic).
- Nested öbek references (`obek = "..."` in TOML) ALSO live in the cooker.
- Override patches authored in TOML (`overrides = [...]`) are NOT a runtime concern — they're cooker output cooked into OOVR chunks.
- The cooker is ~600+ LOC of TOML-specific work; bundling extends + nested + cook-time overrides into one full ObekCooker is cleaner than splitting them across two slices.

So v1m2 ships the **runtime + format substrates** the cooker will consume, and v1m3 grows to ship the **full TOML cooker** in one unified slice. The architectural seam is clean: Phase 3.0 ends with a complete authoring stack regardless of the slicing.

---

## What's deliberately NOT in v1m2 (now in v1m3)

- ObekCooker class — TOML → OBEK CRDR.
- TOML `extends = "..."` chain resolution + cycle detection at cook time.
- Deepest-extends-wins variant resolution.
- TOML `obek = "..."` nested öbek references + recursive cook + sub-instance source_obek_root tracking.
- TOML `overrides = [...]` cook-time override patch baking into OOVR chunks.
- Multi-error accumulation in the cooker (matches v1l SceneCooker pattern).

---

## Files touched

```
engine/scene/include/crd/scene/obek.hpp                   modified
engine/scene/include/crd/scene/world.hpp                   modified (instantiate_obek signature)
engine/scene/src/obek.cpp                                  modified
tests/scene/test_obek.cpp                                  modified (+6 cases)
docs/sessions/2026-05-08-scene-v1m2-overrides-and-ochn.md  created (this file)
CONTEXT.md                                                  updated (v1m2 milestone + v1m re-slicing note)
```

---

## Next: v1m3 — full ObekCooker (TOML + extends + nested + cook-time overrides)

`tools/asset_cooker/include/crd/cooker/obek_cooker.hpp` + `tools/asset_cooker/src/cook_handlers/obek.cpp`. Riding on v1l's SceneCooker pattern. TOML reads `extends = "..."` (variant chain resolution at cook time, depth-unbounded, cycle-detected); `obek = "..."` inside an entity entry (eager flatten; recursive cook of dependencies; sub-instance `source_obek_root` tracking); `overrides = [...]` block (cook-time baked into OOVR chunks; field validation against schema). Multi-error accumulation. ~10 test cases covering composition + variants + cycles + error paths.
