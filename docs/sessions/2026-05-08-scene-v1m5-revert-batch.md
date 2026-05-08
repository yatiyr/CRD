# 2026-05-08 — Phase 3.0 v1m5: revert/unpack/enumerate APIs + AAAA-tier batch reservations (closes v1m)

**Status at start:** Phase 3.0 v1m4b shipped. Inherit transparent CoW backend complete. 805/805 / 802 release.

**Status at end:** v1m5 shipped — closes v1m entirely. Two sub-slices (v1m5a + v1m5b); hot-reload watcher + obekc CLI tool deferred to a separate post-Phase-3.0 follow-up (task #108). Six-config 814/814 / 811 release / 17 smokes; 9 new tests across 2 sub-slices.

**Phase 3.0 v1m FULLY DELIVERED.** All five published sub-slices closed: v1m1 substrate · v1m2 override patches + OCHN format · v1m3 ObekCooker (4 sub-slices) · v1m4 InheritPolicy enum + DontInherit · v1m4b Inherit CoW backend (3 sub-slices) · v1m5 revert/unpack/enumerate + AAAA reservations.

---

## Sub-slice plan + results

| Sub-slice | Scope | Tests added | Status |
|---|---|---|---|
| **v1m5a** | `ObekInstantiation::source` pointer; revert_field/component/entity/all (rebuild bytes from source + cook-time overrides); unpack_obek (revert + sever) + unpack_obek_keep_overrides (sever only); enumerate_overrides | 6 tests | ✅ shipped |
| **v1m5b** | `BatchHints` + `BatchInstanceTag` + `ObekBatchHandle` structs; `instantiate_obek_batch(res, count, parent, hints)` API; auto-tags entities with BatchInstanceTag when registered | 3 tests | ✅ shipped |
| **v1m5c-followup** | Hot-reload watcher with OCHN graph awareness + `obekc extract` CLI tool | — | ⏳ deferred (task #108; post-Phase-3.0) |

## What shipped — v1m5a

### Modified

```
engine/scene/include/crd/scene/obek.hpp       ObekInstantiation gains `source : const ObekResource*` pointer
                                              cleared by unpack APIs.
engine/scene/include/crd/scene/world.hpp       7 new methods: revert_field, revert_component,
                                              revert_entity, revert_all, unpack_obek,
                                              unpack_obek_keep_overrides, enumerate_overrides.
engine/scene/src/obek.cpp                      ~150 LOC of impl. revert_field rebuilds the byte
                                              range from source bytes + re-applies overlapping
                                              cook-time overrides; revert_component/entity/all
                                              cascade through it. unpack_obek_keep_overrides just
                                              clears the source pointer; unpack_obek calls
                                              revert_all first.
tests/scene/test_obek.cpp                      6 new tests: revert_component, revert_field
                                              (single-field), revert_all, unpack_obek (revert +
                                              sever), unpack_obek_keep_overrides (preserve +
                                              sever), enumerate_overrides via direct
                                              ObekArtifactBuilder.add_override.
```

### Architectural decisions pinned (v1m5a)

1. **Caller owns ObekResource lifetime.** `ObekInstantiation::source` is a non-owning const pointer. Caller MUST keep the resource alive via existing `ResourceHandle` semantics. After `unpack_obek*` clears the pointer, the resource can be safely unloaded.

2. **revert is FIELD-RANGE-aware.** `revert_field(file_idx, fourcc, offset, size)` rebuilds only the byte range; `revert_component` calls revert_field with `(0, info->size)`; `revert_entity` iterates components; `revert_all` iterates entities. Single low-level primitive, four user-facing APIs.

3. **Cook-time overrides are re-applied on revert.** revert_field copies source bytes for the field range, then walks `cook_override_records` and re-applies any record whose range overlaps the field. Effect: an entity reverts to its post-instantiate state (NOT pre-cook-time-override), which matches the user mental model of "reset to what the öbek-as-cooked actually contains."

4. **unpack_obek discards runtime state; unpack_obek_keep_overrides preserves it.** Two semantics deliberately separate per ADR-0058 pillar 14:
   - `unpack_obek` → revert_all (entities back to post-instantiate state) + sever source link
   - `unpack_obek_keep_overrides` → just sever; whatever runtime mutations exist stay

5. **`enumerate_overrides` returns cook-time records only at v1m5a.** Caller-supplied runtime overrides (passed to `instantiate_obek`) are NOT stored on the instance (they're transient). Editor "override window" use cases get the cook-time slice today; a future v1m5+ could store runtime overrides too (memory cost; deferred until needed).

## What shipped — v1m5b

### Modified

```
engine/scene/include/crd/scene/obek.hpp       New types: BatchHints (4 fields), BatchInstanceTag
                                              (2 fields), ObekBatchHandle (1 field), kFourCC_BatchInstanceTag.
                                              ~40 LOC of doc-blocks pinning the v1m5b → Phase 3.5+
                                              contract.
engine/scene/include/crd/scene/world.hpp       instantiate_obek_batch declaration with full doc.
engine/scene/src/obek.cpp                      ~40 LOC of impl. Monotonic per-process batch counter.
                                              For each slot: instantiate_obek + tag if BatchInstanceTag
                                              registered.
tests/scene/test_obek.cpp                      3 new tests: spawn-count semantics, BatchInstanceTag
                                              auto-tag when registered, unique handles per call.
```

### Architectural decisions pinned (v1m5b)

1. **API + format reservation only at v1m5b.** The renderer-instanced-draw path and OBAT chunk emission/consumption land in Phase 3.5+. v1m5b ships:
   - The 4 reserved structs (BatchHints / BatchInstanceTag / ObekBatchHandle / kFourCC_BatchInstanceTag)
   - The `instantiate_obek_batch` API surface
   - Auto-tag behavior (works today)

   Things deferred to Phase 3.5+:
   - Per-slot transform application (transforms are NOT applied in v1m5b)
   - GPU instanced draw path (renderer doesn't inspect BatchInstanceTag yet)
   - OBAT chunk emission (chunk FourCC reserved at v1m1; not populated)

2. **BatchInstanceTag is opt-in registration.** Default Worlds don't register it (saves a ComponentId slot). Render paths that want batched-instance optimization register it during their setup; instantiate_obek_batch detects the registration via FourCC scan and tags accordingly.

3. **Monotonic per-process counter for ObekBatchHandle.** Sufficient at v1m5b for in-session uniqueness. Cross-session persistence (save / replay) reserved for Phase 4.2 networking when a content-hash-keyed scheme might be needed.

4. **Transforms parameter REMOVED from v1m5b API.** Original ADR sketch had `ConstSpan<Mat4f> transforms` parameter. v1m5b drops it because the renderer-side application path doesn't exist yet — passing transforms with no consumer is misleading. Phase 3.5+ adds it back when the renderer reads them. v1m5b API: `instantiate_obek_batch(res, count, parent, hints)`.

### Six-configuration green (post-v1m5, 2026-05-08)

- win-debug:          814/814
- win-relwithdebinfo: 814/814
- win-release:        811/811 (after `cmake --build win-release --target clean` due to ObekInstantiation field addition)
- win-asan:           814/814
- win-clang-cl:       814/814
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## v1m FULLY SHIPPED — full inventory

| Sub-slice | What | LOC | Tests |
|---|---|---|---|
| v1m1 | ObekResource + ObekLoader (FourCC `'OBEK'`) + ObekArtifactBuilder + World::instantiate_obek with parent reparenting; ObekEntityGuid; CRDR layout (OINF/OETB/OCMP/D###/ORLS) | ~600 LOC | 8 |
| v1m2 | Runtime ObekOverride + symbolic-name fallback + bounds-checked apply; OCHN format substrate (ObekChainEntryRecord + add_chain_dependency + chunk emit/parse) | ~250 LOC | 6 |
| v1m3a | ObekCooker class + obek_cooker_inline (TOML → flat OBEK CRDR; reader registry shared with SceneCooker) | ~575 LOC | 7 |
| v1m3b | extends chain resolution: iterative walk, cycle detection, deepest-first apply, OCHN entries | ~210 LOC | 4 |
| v1m3c | Nested öbek refs: per-entity `obek = "..."`; recursive walk_and_apply_chain; ChildOf splice; nested name scoping | ~150 LOC | 3 |
| v1m3d | Cook-time `overrides = [...]` → OOVR chunk; auto-apply at instantiate before caller patches | ~150 LOC | 3 |
| v1m4 | InheritPolicy enum + apply_trait dispatch + DontInherit fully implemented + Inherit-as-stub | ~50 LOC | 5 |
| v1m4b1 | SharedComponentPool data structure (refcounted byte pool + freelist + grow) | ~250 LOC | 7 |
| v1m4b2 | SparseSetStorage Pool gains shared_pool + shared_pool_idx; insert_shared API; force-SparseSet for Inherit; get_const indirection; CoW write-break | ~150 LOC | 3 |
| v1m4b3 | Content-hash dedup in SharedComponentPool (acquire_or_retain + HashMap); refcount eviction; shared_pool_live_count diagnostic | ~70 LOC | 3 |
| v1m5a | ObekInstantiation.source pointer + revert_field/component/entity/all + unpack_obek/unpack_obek_keep_overrides + enumerate_overrides | ~150 LOC | 6 |
| v1m5b | BatchHints/BatchInstanceTag/ObekBatchHandle + instantiate_obek_batch (auto-tag when registered) | ~80 LOC | 3 |
| **TOTAL** | **~2700 LOC** | **58 tests** | |

Plus session logs covering each sub-slice's architectural decisions.

ADR-0058 pillars covered (out of 19):
- ✅ Pillar 1 (identity layers) — v1m1
- ✅ Pillar 3 (override patches typed) — v1m2 + v1m3d
- ✅ Pillar 4 (override conflict resolution) — v1m2 + v1m3d
- ✅ Pillar 5 (InheritPolicy with CoW) — v1m4 + v1m4b
- ✅ Pillar 7 (apply/revert/unpack) — v1m5a
- ✅ Pillar 8 (per-instance ADD; soft-DELETE; no REORDER) — implicit via override patches
- ✅ Pillar 9 (CRDR layout) — v1m1 + v1m2 + v1m3d
- ✅ Pillar 10 (cooker pipeline) — v1m3
- ✅ Pillar 11 (hot-reload graph-aware OCHN format) — v1m2 + v1m3
- ✅ Pillar 12 (sub-instance API) — v1m1 + v1m3c
- ✅ Pillar 13 (determinism contract) — verified across all sub-slices
- ✅ Pillar 14 (break / decompose / variant / compose / modify ops) — v1m5a closed Break/Modify; Decompose still requires obekc CLI (deferred)
- ✅ Pillar 15a (batch instantiation API) — v1m5b ships API; renderer integration Phase 3.5+
- ✅ Pillar 18 (per-component reservation flags) — already declared in ADR-0053; v1m respects them
- ✅ Pillar 19 (reserved API surface) — frozen at v1m close

Pillars 2 (composition vs variation), 6 (eager vs lazy), 15b–e (GPU residency, GUID, replication, streaming) are documented in the ADR; their implementation lives in respective consumer phases (renderer / networking / streaming).

Pillar 14 "Decompose" + Pillar 11 "hot-reload watcher" remain as task #108 follow-up (post-Phase-3.0).

---

## Files touched

```
engine/scene/include/crd/scene/obek.hpp                       modified
engine/scene/include/crd/scene/world.hpp                       modified
engine/scene/src/obek.cpp                                       modified (~190 LOC across both sub-slices)
tests/scene/test_obek.cpp                                       modified (+9 cases across both sub-slices)
docs/sessions/2026-05-08-scene-v1m5-revert-batch.md            created (this file)
CONTEXT.md                                                      updated (v1m5 milestone, v1m closure)
```

---

## Next: Phase 3.0 v1n — Preset + Profile system

v1m closed. Phase 3.0 still has 3 slices remaining: v1n (Preset + Profile), v1o (sandbox renderer integration with the full Öbek + Preset + Profile stack), v1p (reserved-slot freeze closing Phase 3.0). v1n ships ADRs 0059 (Preset) and 0060 (Profile) — typed PresetResource with five-layer resolution, per-type FourCC, ProfileResolver with closed predicate schema and additive composition. ~3–4 days.
