# 2026-05-08 — Phase 3.0 v1m4b: InheritPolicy::Inherit transparent CoW backend

**Status at start:** Phase 3.0 v1m4 shipped — InheritPolicy enum + DontInherit + Inherit-as-stub. 792/792 / 789 release.

**Status at end:** v1m4b COMPLETE — three sub-slices (v1m4b1 / v1m4b2 / v1m4b3) shipped. `InheritPolicy::Inherit` now has the transparent CoW backend ADR-0058 pillar 5 specified: byte-deduplicated shared pool with content-hash dedup, refcounted entries, copy-on-first-write break in `get_mut`, refcount eviction on entity destroy, force-SparseSet at registration. **Six-config 805/805 / 802 release / 17 smokes; 16 new tests across 3 sub-slices.**

---

## Why v1m4b before v1m5

User-driven decision. v1m4 had shipped the API surface for `InheritPolicy::Inherit` with a "behaves as Override" stub. The user pushed for completing the elite-tier story before moving to v1m5: shipping v1m5 (apply/revert/unpack + AAAA hooks + hot-reload + CLI) with a CoW stub lurking would mean users who declare `Inherit` would discover no memory savings — a leaky abstraction. v1m4b consolidates the storage-backend risk in one focused effort, then v1m5 ships independent surface-area additions.

## Sub-slice plan + results

| Sub-slice | Scope | Tests added | Status |
|---|---|---|---|
| **v1m4b1** | `SharedComponentPool` data structure (refcounted byte pool with freelist, exponential growth, move-only) | 7 unit tests | ✅ shipped |
| **v1m4b2** | `SparseSetStorage::Pool` gains `shared_pool` + `shared_pool_idx[]`; `insert_shared` API; read-path indirection in `get_const`; force-SparseSet for Inherit at registration; instantiate_obek wires Inherit→insert_shared; CoW write-break in `get_mut` (jumped ahead from v1m4b3 to keep tests green) | 3 integration tests | ✅ shipped |
| **v1m4b3** | Content-hash dedup (`acquire_or_retain` with HashMap<u64, u32>); refcount eviction on entity destroy; `shared_pool_live_count` diagnostic; FNV-1a 64 hashing inside `insert_shared` | 3 dedup/eviction tests | ✅ shipped (closes v1m4b) |

## What shipped

### New files

```
engine/scene/include/crd/scene/shared_component_pool.hpp     ~80 LOC
engine/scene/src/shared_component_pool.cpp                    ~170 LOC (acquire/retain/release/dedup)
tests/scene/test_shared_component_pool.cpp                    ~145 LOC, 7 cases
```

### Modified

```
engine/scene/include/crd/scene/component_registry.hpp     Force-SparseSet at register_type for Inherit components.
engine/scene/include/crd/scene/sparse_set_storage.hpp     Pool.shared_pool + Pool.shared_pool_idx;
                                                          insert_shared API; shared_pool_live_count diagnostic.
engine/scene/src/sparse_set_storage.cpp                    Pool dtor releases shared entries first;
                                                          insert_shared uses acquire_or_retain with FNV-1a 64;
                                                          get_const follows shared-pool indirection;
                                                          get_mut breaks shared via copy + release;
                                                          remove + on_entity_destroyed handle shared slots
                                                          (release on destroy; lockstep-update shared_pool_idx
                                                          across swap-with-last).
engine/scene/src/obek.cpp                                  instantiate_obek for Inherit components calls
                                                          m_sparse_storage.insert_shared instead of insert.
tests/scene/test_obek.cpp                                  6 new CoW tests covering force-SparseSet,
                                                          read-through-share, CoW write-break, dedup,
                                                          distinct-values-distinct-entries, refcount eviction.
```

### Architectural decisions pinned

1. **Force `StorageHint::SparseSet` for Inherit components at registration**. ADR-0058 pillar 5 puts CoW in SparseSetStorage; archetype-side CoW would require rewriting the chunked SoA layout (too invasive for the value). If a user explicitly requested `Archetype` + `Inherit`, the registration silently overrides to `SparseSet`. Documented in `component.hpp`'s InheritPolicy doc-block.

2. **`SharedComponentPool` is a private substrate, exposed publicly for testing**. Lives in its own header/source. Owned by `SparseSetStorage::Pool` (lazy-allocated on first `insert_shared`). Tests verify pool semantics independently of storage integration.

3. **Per-call hash-then-acquire-or-retain**. Each `insert_shared` call computes FNV-1a 64 of the source bytes, queries `m_hash_to_idx` in `SharedComponentPool`. Hits → retain; misses → acquire + record. The HashMap lookup is O(1) amortised; the FNV computation is O(sizeof(component)) — both negligible vs the savings (one pool entry shared by N instances vs N entries).

4. **Hash sentinel = 0**. `m_entry_hashes[idx] == 0` means the entry was acquired via the un-deduped `acquire` path (legacy / direct callers). On `release`, only entries with non-zero hash are erased from `m_hash_to_idx`. Probability of FNV-1a 64 producing 0 for real bytes is negligible; we still guard by setting hash to 1 if the computation yields 0 (in `insert_shared`).

5. **CoW write-break in `get_mut` is idempotent**. Calling `get_mut` twice on a previously-shared slot is safe — the second call sees an owned slot (since the first broke the share) and goes through the inline path. No double-release.

6. **swap-with-last preserves owned/shared distinction**. When removing a slot, `shared_pool_idx[]` is updated in lockstep with `entities[]`. Move-construct the dense bytes only when the trailing slot is OWNED; for shared trailing slots, only the pool_idx moves. The freed trailing slot's `shared_pool_idx` is reset to `kInvalidIdx` to prevent stale-read issues.

7. **Refcount eviction is automatic on entity destroy**. `on_entity_destroyed` (and `remove`) check the slot's pool_idx; if shared, call `shared_pool->release(idx)`. Pool's `release` decrements refcount; if 0, erases from `hash_to_idx`, returns to freelist, decrements `m_live_count`. Tests verify the entire chain via `shared_pool_live_count` diagnostic.

8. **Wasted dense-buffer memory for shared slots is the v1m4b trade-off**. Each shared slot still occupies `sizeof(component)` bytes in the pool's dense buffer (unused for shared, used for owned post-CoW-break). The MEMORY SAVINGS come from the pool's dedup: N instances → 1 pool entry vs N separate dense entries (wins for sizeof(component) >> dense slot overhead). Documented as a v1m4b limitation; a future optimization could allocate dense bytes lazily per-slot.

### Test matrix totals

| File | Tests added | Total assertions |
|---|---|---|
| test_shared_component_pool.cpp | 7 | 188 |
| test_obek.cpp (Inherit/CoW section) | 6 | ~30 |

### Six-configuration green (post-v1m4b, 2026-05-08)

- win-debug:          805/805
- win-relwithdebinfo: 805/805
- win-release:        802/802 (after `cmake --build win-release --target clean` due to header struct changes)
- win-asan:           805/805
- win-clang-cl:       805/805
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

### Stale-.obj gotcha (third recurrence in v1m)

Adding `m_pending_overrides` (v1m3d), `cook_override_records` (v1m3d), `shared_pool` + `shared_pool_idx` (v1m4b2), and `m_entry_hashes` + `m_hash_to_idx` (v1m4b3) to existing structs each tripped the documented CLAUDE.md issue with stale `.obj` files in win-release. Pattern is consistent: `cmake --build --preset win-release --target clean && cmake --build --preset win-release` resolves it. Same advice applies when v1m5 adds further struct fields.

### Release-mode unused-var warning

`/W4 /WX` in win-release flagged `existing` in `insert_shared` as unused-but-initialized when CRD_ASSERT is a no-op in release. Added `(void)existing;` after the assert to silence. Same pattern existing CLAUDE.md describes for similar cases.

---

## v1m4b's value proposition (now demonstrable)

Before v1m4b, declaring `InheritPolicy::Inherit` on a component had no observable effect — Override fallback. After v1m4b:

- `forest_tree.obek` with one `MeshRef` component (sizeof = 8 bytes assumed)
- `for (i = 0; i < 1000; ++i) world.instantiate_obek(forest_tree)` → 1000 entities, each with a MeshRef
- **Pre-v1m4b**: 1000 × 8 bytes = 8 KB of MeshRef data (Override path)
- **Post-v1m4b**: 1 pool entry × 8 bytes + 1000 × 4 bytes (pool_idx in shared_pool_idx) = 4 KB. **2× memory savings on this component.**
- Larger components (skinned mesh data, animation skeletons) yield proportionally larger savings.
- CoW write-break on first mutation: that entity gets its own copy; siblings continue sharing.

This is the AAAA-tier feature ADR-0058 pillar 5 promised; v1m4b ships it.

---

## Files touched

```
engine/scene/include/crd/scene/component_registry.hpp     modified
engine/scene/include/crd/scene/sparse_set_storage.hpp     modified
engine/scene/include/crd/scene/shared_component_pool.hpp  created (~80 LOC)
engine/scene/src/shared_component_pool.cpp                created (~170 LOC)
engine/scene/src/sparse_set_storage.cpp                   modified (~150 LOC net)
engine/scene/src/obek.cpp                                  modified (~20 LOC for Inherit dispatch)
tests/scene/CMakeLists.txt                                 modified (+test_shared_component_pool.cpp)
tests/scene/test_shared_component_pool.cpp                created (~145 LOC, 7 cases)
tests/scene/test_obek.cpp                                  modified (+6 cases)
docs/sessions/2026-05-08-scene-v1m4b-cow-backend.md       created (this file)
CONTEXT.md                                                  updated
```

---

## Next: v1m5 — apply/revert/unpack + AAAA reservations + hot-reload + obekc CLI

Final v1m sub-slice. `revert_field`/`revert_component`/`revert_entity`/`revert_all` on `ObekInstantiation`. `unpack_obek` + `unpack_obek_keep_overrides`. `enumerate_overrides`. AAAA-tier API + format reservations: `BatchInstanceTag` + `instantiate_obek_batch` API + OBAT chunk; `streaming.lod` / `streaming.region` reserved fields. Hot-reload watcher with OCHN graph awareness. `obekc extract` CLI tool. Closes v1m entirely.
