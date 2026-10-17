# DIAG.11b: scene and öbek packs at the trust boundary, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contracts: [ADR-0055 §5](../decisions/0055-scene-serialization-toml-scen-crdr.md)
> and [ADR-0058 §9](../decisions/0058-obek-system.md) (both amended in place), the
> [DIAG.3a partial-construction policy](../design/runtime-diagnostics.md#diag-3a). Previous batch:
> [allocation refusals in the scene and the resource manager](2026-10-17-diag-11b-unchecked-allocations.md), which
> queued follow-ups (i), (k), (l) and (m).

## Goal

Follow-up (i): the scene loaders trusted a loaded pack's descriptors in shipping. `World::instantiate_scene` and
`World::instantiate_obek` only asserted a component's size against the registered size and the 1024-byte staging
buffer, its alignment, its version and every record's entity index, and the loaders' payload bound
`record_count * d.size` was a `u32` product that wraps. The loaders were to refuse such descriptors, with
corrupt-pack scene tests. Taken with it, from (m), the scene module's `SharedComponentPool::grow`, whose `u32` doubling
wraps to zero past 2^31 entries and then never ends. Committed locally, not pushed.

Reading the two loaders and both instantiations end to end for (i) found five more defects, repaired here (each with a
test that failed before the repair, see Teeth):

1. **A loaded scene viewed freed bytes.** `SceneLoader` kept spans into `ctx.bytes` ("the resource handle keeps the
   bytes alive"), but `ResourceManager::load_sync`, the async and the streamed loads all free an artifact's bytes as
   soon as `load()` returns (`artifact_bytes` is a local), and a compressed chunk's decoded bytes lived in a local
   `CrdrFile`. Every scene loaded through the manager was a use after free. `ObekLoader` had been fixed for the same
   reason in v1o3 ("bug surfaced when the sandbox started calling instantiate_obek across the load boundary");
   `SceneLoader` never was, and no test loaded a scene through the manager.
2. **Relations restored as stale ids lost their reverse-index edge.** Both builders persist a relation component's
   bytes (a source-World `EntityId`) as an ordinary payload record, and both instantiations restored it raw in step C
   before step D re-added the relation from `RELS`/`ORLS`. A fresh World issues the source's entity ids again, so the
   raw payload already named the new target, `add_relation_impl` took the re-add as a no-op (`old_target ==
   target`), and the reverse index never got the edge: destroying a loaded parent did not cascade to its `ChildOf`
   children. In a World with other entities the raw id named an unrelated entity until step D replaced it.
3. **Records staged at 16-byte alignment in 1024 bytes.** Both instantiations staged every record in
   `alignas(16) u8 staging[1024]`: a registered component past 1024 bytes overflowed the stack in a build without
   asserts, and one aligned past 16 was deserialized and moved from a misaligned address.
4. **Patches and reverts wrote a relation's target.** A cook-time `OOVR` patch, a caller's `ObekOverride` and
   `revert_field` resolved any registered FourCC, a relation component's included, and `memcpy`'d bytes into it: the
   target changed without the reverse index (a revert wrote the source-World id back). The cook-time patch's pool
   bound and `revert_field`'s overlap arithmetic were `u32` sums that wrap, and `revert_field` bounded its write by the
   descriptor's size, not the registered type's.
5. **The batch counter was process-wide.** `instantiate_obek_batch` numbered batches with a function-local `static`
   (a data race between two Worlds batching on two threads), where the header promises a per-World counter.

## Contract settled

`world.hpp` said a size, alignment or version mismatch was `CRD_FATAL`; the code asserted; ADR-0055 §5 said an
incompatible pack fails the load, never crashes it. The ADR decides: a refusal, amended in place in ADR-0055 §5 and
ADR-0058 §9, and the `world.hpp` comments now state it.

- **Loaders refuse** (the resource is never published) a pack whose records do not lie inside it. Shared rules
  (`engine/world/scene/src/pack_validation.hpp`, one template per record kind for both formats): at most
  `kMaxComponents` descriptors (the payload chunk FourCC carries three hex digits); each descriptor with a non-zero
  FourCC unique in the pack, a non-zero size that is a multiple of its power-of-two alignment (`align_up` was handed
  any file value) and a known storage hint; each payload chunk present exactly when its descriptor has records and
  holding exactly them (the end computed with checked arithmetic: the `u32` wrap is gone), its entity indices
  strictly ascending below `entity_count`; the relation table present exactly when `relation_count` > 0, each record
  with a non-zero FourCC, a source below `entity_count`, a target null or below it, sources ascending and one record
  per source and relation (as the builders write them; with no repeated source a restore never replaces an edge, so
  an acyclic final graph is acyclic at every step). OBEK also: each `OETB` record's `file_idx` equal to its position
  (documented as "stored for invariant check" and never checked) and its parent hint null or in range; each `OOVR`
  record naming an entity of the öbek and a payload range inside the pool, the chunk ending with the pool; each `OCHN`
  path offset inside its pool, which ends in a terminator, and a known kind.
- **Typed copies.** The loaders copy every record table they read (descriptors, entity table, relations, override
  and chain records, and all payload chunks' entity indices) into typed arrays the resource owns, instead of
  reinterpreting chunk bytes: a compressed chunk decodes into a packed backing, so its records can sit at any offset.
  Payload records and the string and override pools stay views (copied out with `memcpy`). Both resources own a copy
  of the artifact.
- **Instantiation refuses before the first spawn**, returning `PackRefusal` and the deciding FourCC in
  `SceneInstantiation` / `ObekInstantiation` (`refused()`), nothing spawned, `source` left null:
  `MalformedPack` (views that contradict themselves, which only a hand-built resource can reach),
  `ComponentSizeMismatch`, `ComponentAlignmentMismatch`, `ComponentVersionMismatch` (no migration hook exists, so any
  version difference refuses; a `DontInherit` component, never restored, is not compared), `RelationCycle` (an
  Acyclic relation's records forming a cycle, a self-relation included: one walk of the functional graph per Acyclic
  relation present; before, a release build installed the cycle and an ancestor walk never ended).
  `instantiate_obek_batch` checks once, spawns nothing for a refused öbek and returns the null handle without
  consuming a number; batches are numbered per World and never 0.
- **Restore.** Relations come from the relation records alone; relation components' payload records are not read.
  A record is staged in place when its component fits 1024 bytes at 16-byte alignment, otherwise in one block from
  the World's allocator per component at the type's alignment (out of memory there is the named fatal "World: out of
  memory for a component's instantiation staging"). Patches skip a relation component (counted in
  `overrides_skipped`), reverts skip relation and `DontInherit` components and bound the write by the registered size,
  and every offset sum is `usize`. `find_by_fourcc(0)` names nothing (0 is the FourCC of every component registered
  without a serialize trait, which the old lookups matched).
- **Shared pool.** `SharedComponentPool::grown_capacity` doubles from 8 in `u64` and clamps to `kMaxEntries`
  (`kInvalidIdx`: every index below it), fatal when more entries than that are needed ("every entry index is in
  use"); `incremented_refcount` is fatal past `u32` (the header claimed an assert guarded it; nothing did).

## Tests

`tests/world/scene/test_scene_pack_trust.cpp` (14 cases, `[pack-trust]`, TLSF heaps), plus three scene fatal modes:

- Loader corruption tables: 25 corruptions of an honest SCEN and OBEK family pack (a parent and two children with a
  component and `ChildOf`), each breaking one rule (19 of them rules this batch added, 6 the loaders already had), and
  8 OBEK-only corruptions of the entity, override and chain tables. A pack edit goes through `CrdrWriter`, so the
  container stays valid and only the chunk under test changes; the untouched round trip loads. A full table of
  `kMaxComponents` descriptors loads (the cap is exact).
- A compressed descriptor table decoded 2 bytes past a 4-byte boundary (the scenario is asserted on the parsed file)
  loads and instantiates.
- Lifetime: the loaded scene instantiates correctly after the bytes it was loaded from are overwritten, and a scene
  mounted in a pack file and loaded through a real `ResourceManager` instantiates after the manager freed the
  artifact.
- Relations: in a fresh World, destroying a loaded parent cascades to both children (SCEN and OBEK); a cook-time and a
  caller patch on `ChildOf` are skipped, and `revert_component` / `revert_entity` leave the targets, in a World whose
  ids differ from the source's.
- Refusals: size, alignment and version mismatches, a `ChildOf` cycle and self-relation refuse with the FourCC and
  spawn nothing, a mutual `Targets` pair (not Acyclic) restores; a mismatched `DontInherit` component does not
  refuse; four hand-built `SceneResource` contradictions are `MalformedPack`; a refused batch spawns nothing and
  takes no handle, and two Worlds number their batches independently.
- Staging: a 1536-byte, 64-aligned component round-trips through both formats with `read_blob` seeing a 64-aligned
  staging address.
- Shared pool: `grown_capacity` from 8, across 2^31 and at the top; `incremented_refcount`.
- `crd-scene-fatal-staging-refused` (an allocator refusing exactly the staging block's shape),
  `crd-scene-fatal-shared-index-exhausted`, `crd-scene-fatal-shared-refcount-overflow` (15 scene fatal modes now).

## Teeth

Each set applied as marked edits from byte-exact backups, rebuilt, run, restored by copying back and touching, rebuilt
on the same lane and rerun green (`git diff --stat` unchanged across every restore).

| Set (lane) | Edit | Seen |
| --- | --- | --- |
| loader (win-debug) | every rule this batch added to the loaders disabled together: descriptor validity, distinct FourCCs, index range and order, the relation rules, the cap, the exact chunk end with `u32` payload products, the OBEK table rules, `RELS` presence | exactly the 19 new rules failed for each format (38 failed checks; the 6 older rules still refused), and all 8 OBEK-table corruptions failed; the hand-built `MalformedPack` case then indexed past the entity table (an `Array` assert) |
| fixes (win-debug) | relation payload records restored again, the scene parsing `ctx.bytes`, in-place staging only (the old 1024-byte assert), patches and reverts resolving relation components, a process-wide batch counter with no pre-check, the refcount check removed | the lifetime, reverse-index, overrides-and-reverts and batch cases failed, the wide-component case hit the staging assert, `shared-refcount-overflow` exited without its marker |
| staging-oom (win-debug) | the staging block from `allocate` instead of `allocate_or_fatal` | `staging-refused` crashed (SegFault) instead of the named fatal |
| instantiate (win-shipping, no asserts) | the layout check and the cycle check skipped, the descriptor-count check dropped, the old `u32` doubling with no index-range fatal | the size, alignment, version and both cycle cases failed (cycles installed silently), the batch case failed, the hand-built count case read past the descriptors, the shared-pool unit case and `shared-index-exhausted` hung to their timeouts |
| views (linux-gcc-asan, UBSan) | the descriptor table viewed in place instead of copied | the compressed-table case stopped on UBSan's "reference binding to misaligned address ... which requires 4 byte alignment" (x86 lanes without UBSan tolerate the read) |

## Verification

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | every target (`all`), no warning | crd-scene-tests (297 cases), the 15 `crd-scene-fatal-*` CTests, crd-scene-cooker-tests (32), crd-cooker-tests (30), the 9 `crd-no-*`/`crd-check*` guards |
| win-shipping | every target (`all`), no warning | the same suites and the 15 fatal CTests |
| win-clang-cl-shipping | the four touched targets (every link on the first try) | the same suites and the 15 fatal CTests |
| win-asan (under vcvars) | the four touched targets | the same suites and the 15 fatal CTests, no report |
| linux-gcc-debug | the four touched targets, 0 warnings | the same suites and the 15 fatal CTests |
| linux-gcc-asan (ASan and UBSan) | the four touched targets, 0 warnings | the same suites and the 15 fatal CTests, no report |
| linux-clang-tsan | the four touched targets, 0 warnings | the same suites (the lane's TSan options) and the 15 fatal CTests, no report |
| linux-gcc-shipping | every target (`cmake --build`, `-k 0`) under the GCC link-time warning set with `-Werror`: the first round failed three links (below), the second relinked the 22 consumers of crd-scene, exit 0, no `warning:` or `error:` line | the same suites, the 15 fatal CTests and the 8 guards |
| linux-gcc-release | every target, 23 links, exit 0, no `warning:` line | the same suites and the 15 fatal CTests |
| linux-gcc-relwithdebinfo (asserts compiled in) | every target, 23 links, exit 0, no `warning:` line | the same suites and the 15 fatal CTests |

The first linux-gcc-shipping round found one link-time `-Wnull-dereference` (four sites, three executables: the
specimen and both cooker suites): `read_payload_chunks`' second pass looked each chunk up again and dereferenced the
result, which the first pass had proven non-null but the compiler could not see. The first pass now keeps its chunk
pointers and layouts and the second reads those (no second lookup); the relinked trees are clean, and every lane above
was rebuilt and rerun after it.

Strict tidy (LLVM 20.1.8) is clean on the 12 changed C++ files; touching `obek.hpp` brought its older findings into
the gate, repaired here (the unused `kObekEntityFlag_*` constants renamed to the naming rule, `PendingOverride`'s
members default-initialized, `kFourCC_BatchInstanceTag` under the `kFourCC_` table convention's naming exception that
`serialize.hpp` and `crdr.hpp` already state). clang-format `--dry-run` reports nothing on any added line (the three new
files are written in its style; no file was reformatted). The Allman check, check-master-plan and check-repository
pass.

## Hosted evidence needed once pushed

crd-scene-tests' 14 `[pack-trust]` cases and the 15 `crd-scene-fatal-*` CTests pass on every lane (linux-gcc-asan's
UBSan run included, which is also where the typed copies are held to alignment), crd-scene-cooker-tests and
crd-cooker-tests pass, and linux-gcc-shipping, linux-gcc-release and linux-gcc-relwithdebinfo link every consumer of
the scene headers under the link-time warning set with no `warning:` line.

## Queued

- (k) and (l) as before; (m) without `SharedComponentPool::grow` (done here): the execution layer, hesap and kir
  sites listed in the [previous session](2026-10-17-diag-11b-unchecked-allocations.md#queued).
