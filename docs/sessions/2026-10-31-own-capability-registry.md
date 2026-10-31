# 2026-10-31 — OWN.1: the capability ownership registry, its guard and the audit

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [OWN.1](../ROADMAP.md#slice-own.1), first slice, and the guard half of [OWN.2](../ROADMAP.md#slice-own.2).
Track B of the [parallel tracks](2026-10-31-parallel-tracks.md); built and tested on its cloud machine
`cerid-track-b`.

## What landed

- [cmake/capability-owners.json](../../cmake/capability-owners.json): 62 capabilities, each with one owner. 48 are
  landed (registered owner, public API files checked), 2 assigned (registered owner, API created by a named row) and
  12 planned (owner module created by a named row). Every clause of the row is an entry: the hesap solver,
  decomposition, optimizer, ODE, FFT, statistics and interpolation owners; crd-math's fixed-size types and closed-form
  solves; the geometry predicates and spatial queries; the foundation containers, allocators, jobs, I/O, hashing,
  checksums, compression and logging; CPU detection and kernel dispatch in crd-core (ADR-0134); the domain owners
  (autodiff, audio DSP, analysis meshing, FE and FV cores, ROS 2 bridge, robot-description import, aerodynamics,
  path planning, plotting); IMG-ORACLE's image metric; crd-ship and crd-store.
- [scripts/check-capability-owners.py](../../scripts/check-capability-owners.py): validates the registry against the
  `crd_module` registrations and the ROADMAP rows, and scans every C-family source under `engine/` for each
  capability's signatures outside its owner. A match fails unless the file is `pending` (a row moves it) or
  `allowed` (with the reason); a stale entry fails. `--audit` lists every match. It is the
  `crd-check-capability-owners` CTest and the repository step "One owning module for every engine capability" (the
  GitHub workflow, `ci/pipeline.py`'s `REPOSITORY_CHECKS` and the CI-4 parity table in
  [CI platform](../design/ci-platform.md)).
- `CapabilityOwners` in [test-repository-tools.py](../../scripts/test-repository-tools.py), six cases: module names
  as `DEPENDS` spells them (NAME, source basename, tools excluded); a planted FNV-1a copy outside the owner refused
  by line while a comment, a string literal and the owner's own copy pass; pending and allowed entries pass until
  stale; every registry error (shape, duplicate id, unknown owner, foreign or missing API file, broken regex, unknown
  row, planned owner already registered or carrying signatures, assigned owner with an API); an assigned owner
  guarding before its API exists; and this checkout passing.
- The contract: [capability ownership](../design/capability-ownership.md), with the owner decisions and the audit.
  AGENTS links it from the reuse rule; CONTRIBUTING's module route adds the registry entry.

## Decisions (numbered in the design)

1. Compression goes to a new foundation module (crd-perf's bundle cannot compress only because the codec sits above
   it in crd-resources).
2. IMG-ORACLE's metric is a new `hesap-image` module, as the row recommends.
3. SplitMix64 and the MMIX LCG step live in crd-core: foundation modules cannot reach hesap-stats.
4. The row's "eylem constraint solver" does not exist: eylem has descriptors, mass properties and a null scene;
   eylem-rigid3d integrates without constraints. The complementarity solve is assigned to hesap-opt, created there
   by EYLEM-v1e and reused by EYLEM-v1f; the row text is corrected.
5. The domain owners and the duplicate rows (8.2b, 8.0c, 8.0a, 8.1a, CONTROL.10, CONTROL.13, EYLEM-v7a, CHIR-SCI.3,
   NOTEBOOK.2, SCIVIZ.2) are registry `consumers` of their owners.
6. CKIR's CUDA emitter writing determinants into device source is an allowed match.

## Audit findings

The scan found 45 files implementing a registered capability outside its owner, all now `pending` on OWN.1:

- FNV-1a in 33 files: byte-stream FNV-1a-64, FNV-1a-32 (profiler, frame runtime, LOD chain), word combiners and
  hesap-eigen's seed strides. Two frame-cook sites (`frame_template_bridge.cpp`, `frame_work.cpp`) start from
  1469598103934665603, the FNV-1a-64 offset basis with its last digit dropped, so they are not FNV-1a at all; the
  move decides from their consumers whether any value persists before it changes them.
- SplitMix64 and the MMIX LCG in 10 files (ceir, perf, containers' `hash_u64`, hesap-dense, hesap-eigen, hesap-fft,
  hesap-iterative, three hesap-stats headers).
- `math/libm.hpp` reads `__AVX2__`/`__FMA__` outside crd-core's kernel-level header.
- geometry-primitives' `det3` and inverse-transpose; by reading also `circumcenter.hpp` and `quadric.hpp` (Cramer).

By reading: the audio module's biquad runs its own section loop over hesap-dsp's coefficients, and hesap-interp's
Clough-Tocher walk has a non-robust orientation (deliberate, with an exact fallback) that becomes a crd-math cross
product.

## Tooling defect fixed on the way

`ci/bench_vm.py wait NAME` always failed: the `wait` subcommand's positional `name` overwrote the global `--name`
(the machine), so the machine-name check refused every wait. The positional is now `job`; a regression case in
`test-ci-platform.py` drives `main` with both names.

## Verification

- Locally: `check-capability-owners.py` (62 capabilities, 45 pending), `test-repository-tools.py CapabilityOwners`,
  `test-ci-platform.py` (the workflow and pipeline parity), `check-master-plan.py`, `check-repository.py`,
  `check-allman-braces.py`, `check-generated.py`.
- On `cerid-track-b` (Xeon, 8 vCPUs, Ubuntu 24.04, GCC 13.3): a clean linux-gcc-debug tree (2,414 steps), then the
  `crd-check-capability-owners`, `crd-repository-tools` and `crd-ci-platform` CTests from that tree.

The first build ran two `cmake --build` processes in one tree (a detached launch reported an SSH failure but had
started); sccache stored empty objects from the race and later links failed with "unknown file type". The cache was
wiped and the tree rebuilt clean. A remote command that carries double quotes or a local `~` goes through a base64
script on the machine (Git Bash expands `~`, and Windows' gcloud wrapper reparses quotes).

## What remains on OWN.1

Every pending move, each with its oracle and before/after board: FNV-1a (owner gains `fnv1a_32`, the constexpr text
form and the word step), the seed generators (crd-core gains them), `libm.hpp`'s ISA read, crd-math's Mat2/Mat3
determinant, inverse and solve with geometry moved onto them, the audio section loop onto hesap-dsp, the compression
module, and the reviewed Clough-Tocher orientation. OWN.2 needs OWN.1 Done.
