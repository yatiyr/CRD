# 2026-05-16 — Phase 3.1.7 v5 `-spatial` cluster CLOSE

> 6th of 11 `crd-geometry` sub-modules complete. Final v5-close slice
> bundles the ADR amendment, system doc, tracker syncs, and the 18-
> config full sweep that gates push-to-git.

## What shipped

**v5-close** wraps the v5 `-spatial` cluster (8 slices total). Nothing
new under `engine/` — this slice is the final closure ritual.

1. **ADR-0076 §20 amendment** appended (~270 lines) — locks the v5 spatial
   substrate. 20 substrate decisions ledgered + slice ledger + cluster
   cross-validation + sub-module summary table.

2. **`docs/systems/geometry-spatial.md`** — new system overview for the
   6th sub-module. Matches the structure of `geometry-mesh.md`:
   when-to-use matrix, architecture diagram, API stencils, support
   matrix, thread-safety contract, determinism pins, performance pins,
   ECS integration template.

3. **Tracker syncs** — `context.md` Current Focus block updated;
   `docs/ROADMAP.md` Phase 3.1.7 row updated; `docs/phases/phase-3.1.7-
   geometry.md` Status block updated; `MEMORY.md` + `project_state.md`
   updated to mark cluster CLOSED + queue v6 `-polygon` as next.

4. **18-config full sweep** — `scripts/full-sweep.ps1`; every config
   green is the gate for the user's git push.

## Cluster summary (v5 `-spatial`)

| Slice | Day | Engine LOC | Tests | Cases | Highlight |
|---|---|---|---|---|---|
| **v5a** KdTree | 2026-05-16 | ~700 | ~600 | 24 | Widest-extent + leaf=8 + lex-tuple median + caller-heap k-NN + explicit-`k` |
| **v5b** LooseOctree | 2026-05-16 | ~700 | ~550 | 17 | Ulrich 2000; **fast-path AABB-fit 100% verified**; guaranteed-fit `(loose-1)·R/extent` formula |
| **v5c** RTree (R*) | 2026-05-16 | ~1000 | ~700 | 17 | Full Beckmann 1990 + STR + Hjaltason-Samet k-NN + indirection handles |
| **v5d** SpatialHash | 2026-05-16 | ~700 | ~700 | 28 | Teschner 2003 + Amanatides-Woo + per-query gen dedup + `find_overlapping_pairs` + fiber-jobified scratch |
| **v5e** UniformGrid | 2026-05-16 | ~700 | ~650 | 26 | Dense bounded grid + grid-bounds-clipped Amanatides-Woo + scratch from day 1 |
| **v5 thread-safety** | 2026-05-16 | — | ~350 | +3 | Locks scratch ↔ multi-location-storage rule; **2000 ASan fan-out tasks** cross-backend |
| **v5-index-bringup** | 2026-05-16 | ~350 | ~300 | 8 | `scene::SpatialBVHIndex` (LooseOctree-backed); FIRST non-reserved spatial index; IAabbExtractor pattern + UPSERT-only update |
| **v5-queries-extension** | 2026-05-16 | ~250 | ~250 | 16 | Unified `crd::geometry::*` facade over 5 v5 backends; support matrix locked |
| **v5-close** | 2026-05-16 | — | — | — | This doc + system doc + ADR §20 + 18-config sweep |
| **Totals** | | **~4900 LOC engine** | **~4330 LOC tests** | **~141 cases** | |

## Ctest growth

Full project ctest 1952 → **2093 win-debug** across the cluster (+141
cases).

| Anchor | Cases | Delta |
|---|---|---|
| v4 cluster CLOSED (2026-05-16 morning) | 1952 | — |
| v5a KdTree | 1977 | +25 |
| v5b LooseOctree | 1996 | +19 (incl. cross-backend hygiene tweaks) |
| v5c RTree | 2012 | +16 |
| v5d SpatialHash + v5d-fast | 2040 | +28 |
| v5e UniformGrid | 2066 | +26 |
| v5 thread-safety validation | 2069 | +3 (KdTree / LooseOctree / RTree fiber-jobified) |
| v5-index-bringup | 2077 | +8 |
| v5-queries-extension | 2093 | +16 |

## Locked decisions (ADR-0076 §20 — 20 in total)

The amendment captures the 20 cross-cutting design pins that emerged
across the cluster. Highlights (each has prose context in ADR §20):

**Algorithmic pattern**
1. Five backends with explicit "use the right one" guidance — don't
   paper over with a wrong-fit backend.
2. Two-camp split (one-object-one-location vs multi-location-storage)
   drives the scratch-overload-or-not decision.

**Thread safety**
3. Scratch overloads exist IFF per-query dedup state mutates during
   queries.
4. Fiber-jobified concurrent test mandatory for ALL backends —
   2000-task cross-backend ASan coverage.
5. `GeometrySpatialJobsListener` Catch2 listener pattern (binary-wide).

**Determinism**
6. Lex-tuple comparators eliminate equal-key ordering hazards.
7. `crd::containers::nth_element/push_heap/pop_heap/sort` (not `std::`).
8. Lowest-payload tiebreak on equal distance (k-NN) / equal `t`
   (raycast) / equal distance² (closest-point).
9. NaN/Inf builders REJECT (debug `CRD_ASSERT`); queries TOLERATE.
10. Zero-direction ray short-circuits in raycast.
11. Update fast-path patterns: AABB-fit-only (LooseOctree); same-cell-
    range (SpatialHash + UniformGrid).
12. Amanatides-Woo voxel traversal — ALL-tied-axes-advance corner-
    grazing safety; grid-bounds-clipped variant for UniformGrid.

**Two-layer typed architecture**
13. Algorithm bodies stay `<MathScalar T>` raw f32.
14. Public surface ships `AABB3<Length32>` / `Vec3<Length32>` /
    `Length32` strip-compute-retag wrappers.

**Facade**
15. Compile-time overload polymorphism for `crd::geometry::*` — zero
    overhead; no virtual; ADL unifies BVH + spatial facades.
16. Support matrix is structural — don't fake k-NN over LooseOctree or
    overlap over KdTree.

**ECS integration**
17. Two-state `SpatialBVHIndex` (unconfigured = no-op shell;
    configured = real work) preserves ADR-0053 day-one promise.
18. Canonical `IAabbExtractor::extract(EntityId, ComponentId, const
    void* data)` — `data` is freshly-installed bytes per
    IStorageEventSink contract; avoids storage-migration-mid-commit
    `get_component`-null hazard.
19. UPSERT-only update contract — `world.add_component` fires
    `on_update`; `get_component_mut` only bumps chunk-version.
20. Pattern locked as the template for future spatial extensions
    (`LightInfluenceIndex` Phase 3.5, `OcclusionIndex` 3.5+,
    `AudioOcclusionIndex` 3.4).

## Cluster cross-validation

Each backend brute-force cross-validated against scalar reference:
- KdTree k-NN / radius — vs brute-force `nth_element` ranking over 500-
  1000 random points × 5 seeds × 5 radii / 4 k values.
- LooseOctree overlap — vs O(N) brute force × 20 query boxes.
- LooseOctree raycast — vs brute-force nearest + equal-`t` lowest-
  payload tiebreak.
- RTree overlap + k-NN — vs brute force × 20 boxes / k∈{1,5,20}.
- SpatialHash + UniformGrid overlap / radius / raycast — vs brute force.
- SpatialHash + UniformGrid `find_overlapping_pairs` — vs O(N²) brute
  force.
- Facade parity — same `crd::geometry::overlap` name across 4 AABB
  backends returns the SAME set on the SAME content.

Permutation determinism + insert/remove cycle stability + NaN
tolerance verified on every backend.

## Files touched

```
+ docs/systems/geometry-spatial.md         NEW system overview
~ docs/decisions/0076-geometry-substrate-architecture.md   §20 amendment appended
~ context.md                               v5-close + cluster-closed marker
~ docs/ROADMAP.md                          Phase 3.1.7 row → cluster closed
~ docs/phases/phase-3.1.7-geometry.md      Status block → v5 closed; v6 next
~ C:/Users/abici/.claude/projects/D--Dev-cerid/memory/MEMORY.md   v5-close entry
~ C:/Users/abici/.claude/projects/D--Dev-cerid/memory/project_state.md   description
+ docs/sessions/2026-05-16-geometry-v5-close.md   THIS FILE
```

## Definition of Done

- [x] ADR amendment ledgered + linked from `docs/decisions/README.md`
      (already linked from v4-close — same ADR file).
- [x] System overview written.
- [x] Trackers synced (context / ROADMAP / phase doc / MEMORY /
      project_state).
- [x] Session log written (this file).
- [ ] 18-config full sweep PASS (`scripts/full-sweep.ps1`) — running
      next. **Every config must be green** before user pushes to git.

## Next session

**Phase 3.1.7 v6 `-polygon` cluster.** Vatti general polygon clipping +
Constrained Delaunay Triangulation (CDT) + Sutherland-Hodgman + Bentley-
Ottmann sweep-line. Five slices per phase doc; ~2 weeks. Authoring/UI
surface (text rendering, vector-graphics export, navmesh polygon ops,
lightmap UV charting, decal projection).

Per the Strategic Execution Plan locked 2026-05-15 (`feedback_strategic_
execution_plan_2026_05_15.md`): geometry phase finishes in FULL before
eylem v1c resumes. Substrate-first; we'll consume into eylem v1c+ after
hesap-dense-v0.
