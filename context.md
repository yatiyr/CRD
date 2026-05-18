# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`.
> This is a DASHBOARD, not an append-only changelog. Each milestone goes to a session log (`docs/sessions/YYYY-MM-DD-*.md`); "Last shipped milestone" below just summarises and points there. The session-log directory itself is the chronological index, not this file.

---

## Current focus

**As of 2026-05-18.** Phase 3.1.7 `crd-geometry` is the active phase. **10 of 11 sub-modules COMPLETE** (primitives ✅ + bvh ✅ + convex ✅ + v3 hull-ext ✅ + mesh ✅ + spatial ✅ + polygon ✅ + mesh-processing ✅ + delaunay ✅ + decomposition ✅). Phase 3.1.7.6 `crd-rhi-compute` substrate ✅ CLOSED 2026-05-17. v9-prereq-test-harness ✅ + v9c V-HACD cluster ✅ + v9a-a Morton + 4 follow-ons ✅ all SHIPPED 2026-05-18.

**🎯 NEXT = v9a-b1 CPU radix sort** — `sort_morton_pairs<KeyT>` templated with both `KeyT=u32` (30-bit) and `KeyT=u64` (60-bit) instantiated from day 1 (both backends already shipping code per v9a-a follow-ons). The correctness reference for v9a-b2 GPU radix. ~3 days · ~300 LOC engine + ~200 tests.

**Remaining Phase 3.1.7 calendar (~5 wk):** v9a-b/c/d + v9b GPU refit + v9e shader-helpers (~4 wk) → v10 `-curves` (5 slices, ~2 wk) → v11 transform-aware (~2 days) → Phase 3.1.7 fully closes. ADR-0076 amendments §25 (v9a/v9b) + §26 (v9e) lock at sub-cluster closes. After Phase 3.1.7 close → `crd-hesap-dense` v0 → Phase 3.1 eylem v1c resumes per Strategic Execution Plan locked 2026-05-15.

**ADR posture.** ADR-0076 has 12 amendments (§12-§24) all accepted; §25/§26 planned. ADR-0078 (units §1-§5), ADR-0079 (perf), ADR-0080 (`crd-rhi-compute`) all accepted.

---

## Coming up next (Phase 3.1 eylem — ⏸ PAUSED at v1b)

Phase 3.1 eylem is paused at v1b close per the ADR-0076 §12 sequencing pivot (2026-05-11): `crd-geometry` ships FULL before eylem v1c resumes, so eylem v1c+ consumes geometry from day 1 (no deferred-refactor debt). Resume order after Phase 3.1.7 close + `crd-hesap-dense` v0: v1c broadphase → v1c-sensor → v1d narrowphase + v1d-filter + v1d-callback + v1d-mesh + v1d-hf → v1e SI solver + v1e-material → v1f joints + v1f-articulation-filter + v1f-fields → v1g islands + v1g-contactmodify → v1h scene queries → v1i character controller → v1j snapshot/replay → v1k sandbox → v1l close. Full slice plan in `docs/phases/phase-3.1-eylem.md`.

---

## Active detour

_none — D-001 closed 2026-05-07; D-002 (concurrent containers) closed 2026-05-12; D-003 (crd-perf) closed 2026-05-15; D-006 (crd-time) closed 2026-05-15._

> When a detour opens, this section names it (e.g. "D-001: investigate shader-cache corruption") and the main roadmap pauses until it closes. Detour file: `docs/detours/D-NNN-<slug>.md`. Queue rules: `docs/detours/README.md`.

---

## Last shipped milestone

**2026-05-18 — Phase 3.1.7 v9a-a base + 4 follow-ons ✅** (5 slices in one day on the v9a `-gpu` LBVH cluster). New module `crd-geometry-bvh-gpu` ships full first-light surface: 30-bit + 60-bit Morton (CPU oracle + GPU dispatch), typed (`Length<T>`) + raw API entries, sync + async-compute dispatch paths. RHI gained 2 new virtuals (`Device::create_command_buffer_for_queue` + `supports_shader_int64`, both appended at END per D135). 5-config DoD PASS in 39 s. Cluster totals (base + 4 follow-ons): ~1170 LOC engine + ~880 LOC tests · 26 cases / 175 assertions · 9 pinned decisions D132-D140 for ADR-0076 §25. Decision-reversal recorded: original "defer follow-ons" was misapplied to substrate work; refined `feedback_ship_at_consumer_template_from_day_one` memory entry. Session logs `docs/sessions/2026-05-18-geometry-v9a-a-morton.md` + `2026-05-18-geometry-v9a-a-followons.md`.

🎯 **NEXT = v9a-b1 CPU radix sort** — `sort_morton_pairs<KeyT>` templated over key width from day 1 (both `KeyT=u32` + `KeyT=u64` instantiated, since both Morton paths are shipping code). Correctness reference for v9a-b2 GPU radix.

### Recent slice history (one line per slice — full detail in linked session log)

- **2026-05-18 — v9c `-decomposition` cluster CLOSED** — 3 slices (v9c-a voxelize + v9c-b decompose + v9c-close) · ADR-0076 §24 amendment locks D123-D131 · 18-config full sweep PASS · eylem v1c stub integration smoke shipped · sub-module 10 of 11 ✅. `docs/sessions/2026-05-18-geometry-v9c-close.md`.
- **2026-05-18 — v9-prereq-test-harness** — `ValidationCapture` + `ulp_compare`/`bit_compare` + `measure_ms`/`CRD_PERF_BUDGET_LE` + `gpu_determinism_check` + `-IncludeRelease` 5-config DoD flag. Discipline locked for all v9 GPU slices. `docs/sessions/2026-05-18-v9-prereq-test-harness.md`.
- **2026-05-17/18 — Phase 3.1.7.6 `crd-rhi-compute` substrate CLOSED** — 6 slices in one day (v0a-v0e + v0-close); ADR-0080 Accepted (D1-D12 + revisions); 18-config full sweep 18/18 PASS in 53:06. Surfaced vtable-stability discipline + zstd LTCG ICE fix + Linux spirv-reflect API mismatch. `docs/sessions/2026-05-17-rhi-compute-v0-close.md`.
- **2026-05-17 — v8 `-delaunay` cluster CLOSED** — 11 algorithm slices + paydown + close; ADR-0076 §23 locks D73-D122; 18-config sweep PASS; sub-module 9 of 11 ✅. `docs/sessions/2026-05-17-geometry-v8-close.md`.
- **2026-05-17 — v7 `-mesh-processing` cluster CLOSED** — 8 algorithm slices + close; ADR-0076 §22 locks D1-D72; sub-module 8 of 11 ✅. `docs/sessions/2026-05-17-geometry-v7-close.md`.
- **2026-05-16 — v6 `-polygon` cluster CLOSED** — 6 slices; ADR-0076 §21; sub-module 7 of 11 ✅. `docs/sessions/2026-05-16-geometry-v6-close.md`.
- **2026-05-16 — v5 `-spatial` cluster CLOSED** — 8 slices (KdTree + LooseOctree + R*-tree + SpatialHash + UniformGrid + scene-index bringup + queries-extension); ADR-0076 §20; sub-module 6 of 11 ✅. `docs/sessions/2026-05-16-geometry-v5-close.md`.
- **2026-05-16 — v4 `-mesh` cluster CLOSED** — closest-point + Möller-Trumbore raycast + Jacobson winding number + SIMD + validate; sub-module 5 of 11 ✅.
- **2026-05-15 — Phase 3.1.7.5 `crd-units` CLOSED** — 19 sub-slices · ADR-0078 §1-§5 (two-layer typed architecture) · 31 locked decisions · `crd-no-untagged-physical-numeric` CI guard live. Mars Climate Orbiter is now a compile error.
- **2026-05-15 — D-006 `crd-time` + D-003 `crd-perf` shipped** — first units consumer + frame profiler with ImGui frontend. ADR-0079.
- **2026-05-14 — Phase 3.1.7 v2 `-convex` + v3 (Shewchuk predicates + 3D Quickhull + hull-extension) CLOSED**.
- **2026-05-13 — Phase 3.1.7 v1 `-bvh` cluster CLOSED** — v1a-v1g shipped (BvhTree + binned-SAH + parallel build + DynamicBvh + Bvh4 + Vec4f SIMD + closest-point) + v1h primitives hardening + v1i unified query facade + v1j viz companion.

For complete chronology, see `docs/sessions/`. For ADR index, see `docs/decisions/README.md`. For active phase plan, see `docs/phases/phase-3.1.7-geometry.md`.

> Older milestones intentionally truncated — they live in session logs.
