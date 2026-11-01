# 2026-11-01 — HW-RESILIENCE.3: known-answer self-checks of the geometry families

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the crd-math self-checks](2026-11-01-hw-resilience-3-math-self-checks.md).

## CI read

- No completed run has a failed job. Running complete-tier run 41254049879 (`31404ec4`) failed
  `crd-ci-orchestrator-e2e` on win-relwithdebinfo (job 124693382935), the orchestrator race the previous note
  diagnosed. The same failure kept every Windows CTest lane from a green complete-tier run, which DIAG.11c and DIAG.12
  need, so it was fixed here under the fix-on-the-spot order (the operator's 2026-10-30 note allows bug fixes in
  `ci/`): aa05dd37. `build_outcome` no longer calls a build with no job yet, or with a listed job it could not read,
  done (a canceled one still is), and `settle` decides the outcome again inside its compare-and-swap and leaves the
  active index alone unless the build finished. Two new `test-ci-orchestrator.py` cases (a watchdog tick between the
  build's creation and its plan upload; a settle whose first read misses a step uploaded since) and three
  `build_outcome` assertions failed before the fix and pass after it. The unit suite (161) and the platform suite
  pass. The end-to-end suite passed 70 of 71 local runs, 18 of them three at a time; the one other run ended
  `FAILED (errors=1)` and its output was not kept. No later run reproduced it, so it is recorded as the host's known
  fault under load (impossible CPython errors in native Windows validators), not chased.
- Needs CI audit: run 41254049879's commit `31404ec4` predates the AArch64 decoder (76e61d51), so it cannot meet
  HW-RESILIENCE.2's hosted clause; complete-tier run 41269105413 (`b0b1b245`, after the decoder) is queued. DIAG.11c
  and DIAG.12 are unchanged. Account-backed lanes: the FTL variables exist; no AMD or Mac names.
- DIAG.11b clause (8): no new recording under `build/diag-overhead-wpr/`.
- Track B's `test_standard_streams.cpp` (`03571fa3`) failed win-debug with C4996 on `fopen`; track B fixed it in
  77a60960 before this batch's push.

## Geometry self-checks

- `geometry.primitives` (`primitive_self_check`): every batch routine over three bundles (an avx512 pair and the odd
  last bundle on Vec8f) and the hull scan over 37 vertices padded to 40 (two sixteen-lane steps and an eight-lane
  tail at avx512). The inputs are small dyadic values, so every product, sum and quotient is exact, and each answer
  is a closed form of the lane's index evaluated at compile time: slab entries with the t0 and t1 cuts and a missing
  axis, packet entries through infinite inverse directions, Moller-Trumbore t, u and v with back-face culling and the
  tnear cut, closed-interval box and sphere overlaps that touch, segment distances on the clamped, the general and
  the point-segment paths (tiny = 0), and hull support with an exact tie (the lower index) and a maximum in the tail.
- `geometry.mesh` (`mesh_self_check`): a two-leaf BVH over 29 triangles whose leaves give a sixteen-lane step with
  nine valid, and a sixteen-lane step with an eight-lane tail of four valid, at avx512. The ray passes a triangle
  behind its origin, finds a hit in the first leaf, enters the second leaf nearer and ends on an equal-t tie in the
  tail, which the lower triangle index wins: triangle 1 at t = 13, weights (3/4, 1/8, 1/8). Culling back faces, the
  same ray hits nothing.

## Tests

- `primitive self-check:` and `mesh self-check:` cases: every table the host runs passes (a `[kernels]` case, so every
  forced arm and SDE runs it), and a table with one wrong result fails in that routine (each of the seven primitive
  routines with its last bundle's last lane wrong; the raycast with a weight one bit off, and one that ignores
  culling). The selection cases also require the family's resolution `Passed` on the table that runs.

## Evidence

- win-debug: crd-geometry-primitives-tests (207 cases) and crd-geometry-mesh-tests (44) pass; forced portable, sse2
  and avx2 `[kernels]` arms pass; SDE 10.13.1 `-icx` with `CRD_CPU_LEVEL=avx512` passes the self-check and selection
  cases (running avx512). linux-gcc-debug builds both programs and passes their self-check and `[kernels]` cases.
- Teeth: a segment answer one off on lane 6 failed every primitive table (`portable segment8_distsq`); the mesh
  answer's triangle changed to 3 failed every mesh table (`portable raycast_mt8`) and the selection case's `Passed`.
  Both restored, rebuilt and passing.
- Guards (`crd-no-*`, `crd-check*`, diag coverage and seeded faults): 21 of 21 after crd-platform-tests was rebuilt
  (`crd-check-isa-isolation` had judged the stale tree, which lacked the new `standard_streams` objects). Strict tidy
  and clang-format clean on the six changed C++ files.

## Left on the row

Known-answer checks for the other nine families (hesap autodiff, dense, direct, fft, sparse, special, stats and
tensor, eylem rigid3d), the device path, `ceridc doctor --hardware` and the state-hash divergence check.
