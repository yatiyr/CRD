# 2026-05-08 — Phase 3.0 v1m1: Öbek substrate + minimal round-trip

**Status at start:** Phase 3.0 v1l shipped 2026-05-08. SceneCooker ships. ADRs 0058/0059/0060 written and accepted. Phase 3.0 expanded from 14 to 17 slices: v1m (Öbek) → v1n (Preset+Profile) → v1o (sandbox integration) → v1p (reserved-slot freeze).

**Status at end:** v1m1 shipped — Öbek substrate (`ObekResource` + `ObekLoader` for FourCC `'OBEK'` + `ObekArtifactBuilder`) plus `World::instantiate_obek` with parent reparenting via ChildOf. 8 round-trip test cases. Six-config 766/766 / 763 release / 17 smokes. Scene tests up to 250 cases (was 242 post-v1l).

**v1m sub-slicing locked:** v1m1 (substrate) → v1m2 (extends + overrides) → v1m3 (nested composition) → v1m4 (InheritPolicy with CoW backend) → v1m5 (apply/revert/unpack + AAAA reservations + hot-reload + obekc CLI). Each sub-slice individually green-shippable.

---

## Goal of this session

Land the bottom of the Öbek stack (ADR-0058 pillars 1, 9, 10 partial, 12, 15c partial):
1. **Substrate types** — `ObekInfo`, `ObekComponentDescriptor`, `ObekEntityRecord`, `ObekRelationRecord`, `ObekResource`, `ObekLoader`, `ObekArtifactBuilder`, `ObekInstantiation`, `ObekEntityGuid`.
2. **CRDR layout** — chunks `OINF` / `OSTR` / `OCMP` / `OETB` / `D###` / `ORLS`. Reserved FourCCs declared for v1m2+ (`OOVR`, `OCHN`) and Phase 3.5+ (`OBAT`, `OLNK`).
3. **`World::instantiate_obek(const ObekResource&, EntityId parent)`** — spawn entities, restore components by FourCC lookup, install relations, **reparent öbek roots under `parent` via `Relation<ChildOf>`**.
4. **`ObekEntityGuid`** — stable 64-bit cross-machine identity (FNV-1a 64 of `obek_root_id` + `file_idx`).
5. **Six-config green + 8 round-trip tests.**

## What shipped

### New files

```
engine/scene/include/crd/scene/obek.hpp     ~205 LOC — substrate types + API
engine/scene/src/obek.cpp                    ~430 LOC — ObekArtifactBuilder, ObekLoader,
                                                       World::instantiate_obek
tests/scene/test_obek.cpp                    ~265 LOC, 8 cases
```

### Modified

- `engine/scene/include/crd/scene/serialize.hpp` — added FourCCs for `'OBEK'` container + `OINF`/`OETB`/`OCMP`/`ORLS`/`OSTR` chunks + reserved `OOVR`/`OCHN`/`OBAT`/`OLNK`. `make_obek_component_chunk_fourcc(file_local_id)` emits `'D000'`–`'D0FF'` (parallel to SCEN's `'C000'` range; separate FourCCs to keep tooling diagnostics unambiguous).
- `engine/scene/include/crd/scene/world.hpp` — declared `instantiate_obek(const ObekResource&, EntityId parent)`. Documented forward-compat + hard-fail policy at the API doc-block.
- `tests/scene/CMakeLists.txt` — added `test_obek.cpp`.

### Architectural decisions pinned

1. **CRDR chunk FourCCs distinct from SCEN's** — `'OINF'` not `'INFO'`, `'OETB'` not `'ETBL'`, `'D000'-'D0FF'` not `'C000'-'C0FF'`. The container `type_fourcc` already disambiguates, but explicit chunk-FourCC separation makes hex dumps and diagnostic tooling unambiguous about which container a stray chunk came from.

2. **`ObekInfo` is 24 bytes (not 16)** — adds `obek_root_id : u64` to OINF. v1k's `SceneInfo` was 16 bytes; öbek pre-emptively bumps because every öbek needs a stable root identity for `ObekEntityGuid` (ADR-0058 pillar 15c) and there's no benefit to forcing v1m2 to add a separate one-field chunk for it.

3. **Reparenting policy is "roots get ChildOf(parent)"** — a "root" is any entity with no `ChildOf` relation in the source öbek. Concretely: scan `relation_records` for `kFourCC_RelChildOf`; any `file_idx` that doesn't appear as `src_file_idx` in those records is a root. Each root gets `add_relation_via_id(childof_id, root, parent)` installed, but only when `Relation<ChildOf>` is actually registered in the target World (forward-compat skip otherwise). Passing `EntityId::null()` as parent disables reparenting — equivalent to `instantiate_scene`.

4. **`ObekEntityGuid = FNV-1a 64(obek_root_id, file_idx)`** — pure function, deterministic, no allocator. Hashes the 8 bytes of `obek_root_id` followed by 4 bytes of `file_idx`. Re-implemented inline (over `(u64, u32)`) rather than reusing `containers::fnv1a_64` because the identity computation should not depend on the StringView overload.

5. **`obek_root_id` is supplied by the cooker, not derived at build time** — `ObekArtifactBuilder` takes it as a constructor argument. v1m1 callers (tests) pass an arbitrary 64-bit; v1m2's TOML cooker will derive it from FNV-1a 64 of canonical-path + content version.

6. **Determinism preserved in entity-record table** — `ObekEntityRecord.parent_file_idx` is populated by scanning relation_records for ChildOf (same pass that builds the relations chunk). The authoritative parent relationship still lives in ORLS; OETB.parent_file_idx is a hint for v1m3 root detection. Same World → same OBEK bytes (verified by `Determinism: identical world produces bit-equal obek bytes` test).

### Test matrix (8 cases / scene)

| # | Case | What |
|---|---|---|
| 1 | Empty obek loads with zero entities | Sanity floor |
| 2 | Single-entity obek round-trips Transform values | Basic happy path |
| 3 | Multi-entity obek with TestComponent round-trips | Custom component round-trip |
| 4 | instantiate_obek(null parent) leaves roots top-level | Reparent semantics — null case |
| 5 | instantiate_obek(alive parent) installs ChildOf(root,parent) | Reparent semantics — main case (with sibling root pre-existing in target World) |
| 6 | Two instantiations of one obek don't share entities | Spawn independence |
| 7 | Determinism: identical world produces bit-equal obek bytes | FNV-equivalent verification (memcmp) |
| 8 | ObekEntityGuid is stable across (root_id, file_idx) | Pure-function identity check |

### Six-configuration green (post-v1m1, 2026-05-08)

- win-debug:          766/766
- win-relwithdebinfo: 766/766
- win-release:        763/763
- win-asan:           766/766
- win-clang-cl:       766/766
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. Scene tests: 250 cases (was 242 post-v1l).

---

## What's deliberately NOT in v1m1

These are explicitly v1m2+ scope. v1m1 ships the substrate; later sub-slices add the higher-level features that ride on it.

- **`extends` chain + variant resolution** — v1m2.
- **Override patches (`ObekOverride`) + cook-time validation + symbolic-name fallback** — v1m2. (`OOVR`/`OCHN` chunks reserved.)
- **Nested öbek references in TOML + sub-instance tracking** — v1m3.
- **`InheritPolicy` enum with full CoW backend** — v1m4. (Currently every component is implicit Override semantics — copy at insert time, no shared backing.)
- **`unpack_obek` + `revert_field/component/entity/all` + `apply_back_to_source` + `enumerate_overrides`** — v1m5.
- **`instantiate_obek_batch` + `BatchInstanceTag` + `OBAT` chunk** — v1m5 reserves; runtime backend Phase 3.5+.
- **`obekc extract` CLI tool** — v1m5.
- **Hot-reload watcher integration with `OCHN` graph awareness** — v1m5.

---

## Files touched

```
engine/scene/include/crd/scene/serialize.hpp       modified
engine/scene/include/crd/scene/obek.hpp            created (~205 LOC)
engine/scene/include/crd/scene/world.hpp           modified
engine/scene/src/obek.cpp                           created (~430 LOC)
tests/scene/CMakeLists.txt                          modified
tests/scene/test_obek.cpp                           created (~265 LOC, 8 cases)
docs/sessions/2026-05-08-scene-v1m1-obek-substrate.md  created (this file)
```

---

## Next: v1m2 — `extends` chain + override patches

`ObekCooker` extension for TOML `extends = "..."` recursive flattening with cycle detection at cook time; deepest-extends-wins resolution per field. `ObekOverride` struct (typed: `file_idx + component_fourcc + packed field_path + payload`); cook-time validation against öbek schema manifest; symbolic-name fallback when `file_idx` fails to validate. New `OOVR` and `OCHN` chunks populated. ~6 test cases.
