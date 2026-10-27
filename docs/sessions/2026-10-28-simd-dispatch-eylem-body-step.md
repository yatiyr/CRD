# 2026-10-28 — SIMD-DISPATCH: the eylem-rigid3d body step as a kernel family

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `90a87169`.

## CI first

- Push run 40581144513 (`59c79f2f`, still running): all eleven GCC and Clang jobs failed on the `GemmBlock` `beta`
  initializers that `90a87169` already fixed (every `GemmBlock{...}` site sets it now). Push run 40586446192 runs the
  fix. No completed run has another failed job.
- Needs CI audit: no new hosted evidence; DIAG.11c and DIAG.12 stay Needs CI.

## What changed

- `rigid3d_kernels.hpp` (data only): `kChunkLanes = 8`, the plain `BodyChunk` (26 `alignas(32) f32[8]` columns in the
  old member order, then flags, generation, live) and `Rigid3dKernels::step_bodies`. The `BodyChunkT<Lane>` template
  and both `put_lane` helpers are deleted; the sphere, box and capsule chunks are plain columns too. No public header
  of the module includes `vec8f.hpp` or `backend.hpp` now.
- `kernels/rigid3d_kernels.cpp`: one substep on eight lanes a step per level (portable, sse2, avx2). It copies every
  lane's pose to prev (the old `snapshot_state_to_prev`, deleted), then integrates the live lanes with
  `inv_mass > 0` in the scalar body's operation order: `v + g dt`, `* (1 - ld dt)`, `p + v dt`, `w * (1 - ad dt)`,
  `(w, 0) q` with its zero terms left to right, `q + (dt / 2) q_dot`, then `try_normalize`'s length test (at most
  1e-5 keeps q) and true divisions. No fma, so the family is bit-exact across levels and with the old body.
- Every lane computes and `select` keeps the inactive lanes' old values, so masked lanes may set sticky FP status
  flags the scalar body never set (nothing in the engine reads or unmasks them; the AoSoA trade-off). A kept lane
  divides by one, so no lane divides by a zero length.
- `EylemSystem::run`: one table call over the pool, then the entity loop only syncs Transform from `read_pose` (new;
  `PrevState` is now `Pose`). `RigidBodyInterpolationSystem` reads `read_pose` as well, instead of a full `read`.

## Decisions

1. The step runs over the whole pool (ADR-0062 §6: integrate is part of the solve; syncing Transform belongs to the
   post-physics step). Two behaviours change. A live body that no entity references now integrates (before, it froze
   while its prev pose still advanced). A body that two entities share integrates once per substep (before, it ran
   once per entity). A test covers each case.
2. The integrator still ignores `sleeping`, `gravity_enabled` and the locks, as v1b-c's step did, because changing
   them would be a physics change outside this row. They now belong to rows: EYLEM-v1e (gravity and the locks) and
   EYLEM-v1g (sleeping).

## Tests (hosted CI runs the forced arms)

- `[kernels]` in `test_rigid3d_kernels.cpp`, with `crd-eylem-rigid3d-kernels-at-<level>`:
  - Portable is checked against the old scalar body, which is kept as the oracle and runs on `RigidBody` and
    crd::math. Every level the host runs is checked against portable.
  - The corpus has 0 to 5 chunks plus two guard chunks, and five gravity and dt cases (dt of 1/60, 0, subnormal and
    random; gravity with NaN and infinity).
  - Lanes mix:
    - free and live slots;
    - inv_mass of 0, -0, negative, NaN, infinite and subnormal;
    - velocities that are signed zero, subnormal, infinite or NaN;
    - damping above 1/dt, and NaN damping;
    - zero and infinite quaternions, and lengths just below, at and above 1e-5.
  - The float columns must match bit for bit, except that two NaNs count as equal (ADR-0134 §7). The side bands must
    match byte for byte.
- A public-path case runs `EylemSystem` for three substeps over 20 bodies in 3 chunks (including a free lane, an
  unattached body and a shared body). The pool must equal the selected table run on a copy, and both entities of the
  shared body must hold its pose.
- Local, win-debug: the suite passes (35 cases), and all five `-kernels-at-` arms pass. `crd-eylem-viz-tests` passes.
  Tidy is clean on the changed sources.

## Board candidates (row-close pass)

- No eylem integrator board exists yet. One needs a program that runs one step over N bodies at every level, with
  HEAD's scalar entity loop as the control.
- No physics peer (Jolt, PhysX, Bullet, Box2D) is on disk. Using one would be a download, which needs the user's
  approval.
