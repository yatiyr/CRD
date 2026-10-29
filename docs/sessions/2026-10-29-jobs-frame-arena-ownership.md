# 2026-10-29 — SIMD-DISPATCH follow-up: frame arena ownership

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); the follow-up the
[minimax note](2026-10-29-simd-dispatch-minimax-coefficients.md) left. The permute stage of the row's HPTT board
needs a per-worker buffer from the frame arena, which this makes safe.

## CI first

Push run 40826432863 (dd860168) still runs win-asan and linux-gcc-arm64-asan; its win-shipping-profile Test step
failed on `crd-check-isa-isolation` (`roundpd` in the CRT's `svml_dfloor2_sse4`), the finding b9e796e3's
`msvc-crt-svml` exception already covers. Every later push run was cancelled by the concurrency group before any job
started; scheduled run 40849112570 is queued. Nothing was cancelled by hand. DIAG.11c and DIAG.12 stay Needs CI.

The lint job's `check-generated.py` would fail at HEAD: 026d4ee2 and e0f539cb regenerated
`transcendental_refs.inc` without refreshing its manifest entry, which also named a generator path that does not
exist (`generator-absent`). The generator reproduces the committed bytes on this host and on the reference host
(WSL Python 3.12), so the entry is now `reproducible` with its real path and command, and the generator writes UTF-8
on every host (a Windows pipe had turned its em dash into one cp1254 byte); `--regenerate` reports it byte-identical.

## Defects

- 25 library sites (hesap-direct's factorisations, the preconditioners, the parallel sparse operators, the
  triangular solve, tensor reduce and permute) called `crd::jobs::frame_reset()`, which wipes every thread's arena:
  memory a caller held from the arena was handed out again under it.
- `blas3.cpp` and the sandbox's host-call wrapper restored a frame mark after a `wait()`. A job that waits may resume
  on another thread (worker_pool.cpp:39), so the restore could move another thread's cursor; the jobs this thread ran
  meanwhile own the memory above the mark.
- `parallel_reduce` kept its partials in the arena across its wait, the same hazard.
- `FrameArena::alloc` checked exhaustion only by assertion: a release build returned a pointer past the arena.
- The padding aligned the offset, not the address, so an alignment above malloc's (16) was not honoured.

## What landed

- `parallel_for`, `parallel_for_pcores` and `parallel_reduce` take a mark, build the job array, `run()` (which copies
  it and never suspends) and restore before returning or waiting. A loop of them never grows the arena, so every
  library reset and the three mark/restore pairs are deleted, with the test and bench resets and oversized arenas
  that only hid the leak.
- `parallel_reduce` takes the caller's `std::span<R>` slots (one per job) instead of `num_jobs`; a job's slots live on
  its own stack and move with it. It has no engine caller (the BVH builder's notes said it did; they now name
  `parallel_for`).
- `frame_try_alloc` returns nullptr and leaves the cursor; `frame_alloc` on it ends in the always-on fatal "frame
  arena exhausted". `crd-check-allocation-results` checks `frame_try_alloc` results.
- `frame_reset` asserts it is not called from a job and that no completion counter is held.
- The arenas are a DIAG.10c bounded resource: `JobsPressure::frame` (the fullest arena, highest peak, refusals), and
  `crd-diag-queue-full-specimen frame` proves the counted fatal.
- `crd-check-frame-resets` rejects a `frame_reset` call under engine/ outside the jobs module.
- ADR-0065 D(iter)-2 is amended.

## Checks

win-debug: crd-jobs-tests (209 cases; four new: refusal and address alignment, 2000 ranges on a 4 KiB arena with
a caller's bytes intact, nested reductions in sixteen jobs leaving every arena empty, the frame pressure reading),
the hesap-sparse `[parallel]` cases (new: 500 operator applies keep a caller's frame bytes), crd-stress-tests
`[jobs]`, crd-geometry-bvh-tests (now on the default 1 MiB arena), the sandbox host cases (the run now leaves the
arena where it found it), crd-ceir-host-tests, the Morton and spatial tests, crd-hesap-direct-tests; tooling tests for
the new guard and the allocation guard; the ctest guards; the repository validators. One guard run died with Python's "internal error in regular expression engine" in `check-isa-isolation.py` (this host's known fault); its rerun passed. crd-bench builds; the vs-reference benches lost only reset lines and were not compiled here
(the local win-vs-ref tree names a removed MSVC 14.50.35717 and no longer configures), so hosted CI builds them.

## Left

- pow is 2.4 ulp at |y·log2 x| ≈ 1000 (from the log note); its ≤1 ulp needs log2 to ~2^−68 and an exp2 that takes
  a pair.
