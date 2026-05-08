# 2026-05-08 — Phase 3.0 v1m4: InheritPolicy enum + DontInherit semantics

**Status at start:** Phase 3.0 v1m3 fully shipped. Full ObekCooker pipeline. 15 cooker tests; six-config 787/787 / 784 release.

**Status at end:** v1m4 shipped — `InheritPolicy` enum (Override / Inherit / DontInherit) + trait registration + DontInherit auto-skip during instantiate_obek. **Inherit is API-only at v1m4**: observable behavior matches Override; the transparent CoW backend optimization is deferred to v1m4b. 5 new tests; six-config 792/792 / 789 release / 17 smokes.

**v1m4b** (transparent CoW backend) is now a separately tracked task — deferred until either actual memory pressure justifies the storage-backend changes or an authoring use case (10k tree forest with shared mesh) appears.

---

## Goal of this session

Land the API surface and DontInherit semantics from ADR-0058 pillar 5:
1. `crd::scene::InheritPolicy` enum (Override / Inherit / DontInherit) in `component.hpp`.
2. `ComponentInfo::inherit_policy` field stamped at `register_component` time via a new `apply_trait` overload.
3. `World::instantiate_obek` skips DontInherit components — the entity spawns but the component is never copied. Used for runtime-only state (NetworkId, LoadState, EditorSelectionFlag, ChangeDetectVersion) that should never persist through cook → instantiate.
4. `Inherit` accepted at registration but currently behaves as `Override` (private copy per instance). The OBSERVABLE BEHAVIOR is identical to Override — read returns a value, write updates that entity's copy without affecting siblings. The CoW backend optimization (per-entity per-component owned/shared flag in storage; copy-on-first-write; shared backing pool with refcount) lands in v1m4b.

## What shipped

### Modified

```
engine/scene/include/crd/scene/component.hpp          InheritPolicy enum (3 values, doc-block
                                                      pinned). ComponentInfo.inherit_policy field
                                                      with default Override.
engine/scene/include/crd/scene/component_registry.hpp apply_trait(ComponentInfo&, InheritPolicy)
                                                      overload — stamps the field at registration.
engine/scene/src/obek.cpp                              instantiate_obek consults info->inherit_policy;
                                                      DontInherit components are counted in
                                                      components_skipped and their bytes never
                                                      copied to the spawned entity.
tests/scene/test_obek.cpp                              5 new tests covering the trait, defaults,
                                                      DontInherit skip, Inherit-as-Override
                                                      behavior, and mixed-policy registration.
```

### Architectural decisions pinned

1. **`InheritPolicy::Override` is the default** — matches all pre-v1m4 behavior. Existing components without an explicit policy get the default; no breakage.

2. **`InheritPolicy::DontInherit` skips the component during instantiate, not during cook**. The cooker still emits the component bytes into the öbek file (the source World had it; the OBEK CRDR records it). The skip happens at the LOADING side, in `instantiate_obek`, when the target World's registered policy says DontInherit. This means:
   - Same öbek file can be loaded by World A (DontInherit on RuntimeState) → instances without RuntimeState
   - Same öbek file loaded by World B (Override on RuntimeState) → instances WITH RuntimeState
   - Policy is a TARGET-WORLD concern, not an SOURCE-WORLD concern.

3. **`InheritPolicy::Inherit` is API-only at v1m4**. Documented contract: observable behavior matches Override. The CoW optimization is purely an implementation detail of v1m4b. Consumers can declare `Inherit` today (e.g. on `MeshRef`, `MaterialRef`) and migrate transparently when v1m4b ships.

4. **Defer transparent CoW to v1m4b for honesty**. Implementing per-entity per-component owned/shared flag bits in BOTH `ArchetypeChunkStorage` (chunked SoA) and `SparseSetStorage` (sparse → dense → T) plus write-path interception plus reference-counted shared backing is genuinely large invasive work — multiple days of careful coding. The pragmatic split: v1m4 ships the API contract; v1m4b ships the optimization when there's an actual consumer with memory pressure. The OBSERVABLE BEHAVIOR is unchanged across the split, so users can write Inherit-aware code today.

5. **Mixed policies on the same World are valid.** A target World can register one component as DontInherit, another as Override, another as Inherit. Each is consulted independently per (entity, component) pair during instantiate.

### Test matrix (5 cases / scene)

| # | Case | What |
|---|---|---|
| 1 | InheritPolicy enum default is Override | Trait is opt-in; default behavior preserved |
| 2 | Component registered with InheritPolicy::DontInherit stamps the trait | Trait dispatch wires correctly |
| 3 | InheritPolicy::DontInherit skips component during instantiate_obek | DontInherit semantic — entity spawns without component |
| 4 | InheritPolicy::Inherit behaves as Override at v1m4 | API stub contract: read + write semantics match Override |
| 5 | Mixed InheritPolicy registration: Override + DontInherit on same World | Multiple policies coexist |

### Six-configuration green (post-v1m4, 2026-05-08)

- win-debug:          792/792
- win-relwithdebinfo: 792/792
- win-release:        789/789  (after `cmake --build win-release --target clean` due to header struct changes)
- win-asan:           792/792
- win-clang-cl:       792/792
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config.

---

## What's deliberately NOT in v1m4 (now tracked as v1m4b task #102)

- **Transparent CoW backend for Inherit** — per-entity per-component owned/shared flag bit in `ArchetypeChunkStorage` and `SparseSetStorage`. Shared backing pool. Write-path interception that detects shared component → allocates private copy → marks owned. Reference counting on shared backing. ~4 tests covering CoW reads, CoW writes, share-break-on-write, refcount eviction. Lands when either:
  - An authoring use case appears (10k tree forest with shared mesh is the canonical example).
  - Profiling shows Override's per-instance copy cost dominates in a real Cerid workload.
- The contract is preserved: switching from Override to Inherit on a registered component will be a memory-only change at v1m4b — observable behavior stays identical.

---

## Files touched

```
engine/scene/include/crd/scene/component.hpp                  modified
engine/scene/include/crd/scene/component_registry.hpp          modified
engine/scene/src/obek.cpp                                       modified
tests/scene/test_obek.cpp                                       modified (+5 cases)
docs/sessions/2026-05-08-scene-v1m4-inherit-policy.md          created (this file)
CONTEXT.md                                                      updated (v1m4 milestone)
```

---

## Next: v1m5 — apply/revert/unpack + AAAA reservations + hot-reload + obekc CLI

Final v1m sub-slice. `revert_field`/`revert_component`/`revert_entity`/`revert_all` on `ObekInstantiation`. `unpack_obek` + `unpack_obek_keep_overrides` (sever instance↔source link). `enumerate_overrides` for editor "override window" UI. AAAA-tier API + format reservations: `BatchInstanceTag` component + `instantiate_obek_batch` API + OBAT chunk format-only (Phase 3.5+ runtime). `streaming.lod` / `streaming.region` reserved fields in OETB. Hot-reload watcher integration with OCHN graph awareness. `obekc extract` CLI tool. Closes v1m entirely.
