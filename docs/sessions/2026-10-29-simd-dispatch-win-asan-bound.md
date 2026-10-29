# 2026-10-29 — SIMD-DISPATCH: win-asan's CTest bound and the lanes' host record

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left: the guard's first green run on every lane; win-asan's
cap, the follow-up of [the ARM64 ASan cost](2026-10-29-simd-dispatch-arm64-asan-cost.md)).
Before: [the permute board's arms](2026-10-29-simd-dispatch-permute-arms.md).

## CI first

- No completed run has a failed job that an earlier batch had not traced. Push run 40869334591 (e496d16c) is still
  in progress: its win-asan job failed only on `crd-check-isa-isolation` (MSVC ASan's literal descriptors, fixed at
  5cf0e54e, after e496d16c) and its win-shipping job on the TickClock case (fixed at d3b0daf1);
  linux-gcc-arm64-asan started before a1901325 and runs into the cap. Every later push run was replaced while
  queued. Nothing was cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run. linux-gcc-asan, linux-clang-tsan and
  linux-clang-arm64-asan passed in 40869334591.

## The three questions

- **Memory.** "1.0 of 64.0 GiB available" is the contended-host fixture of `test-repository-tools.py` (the
  `build-profile` refusal case), printed into the tooling test's log. No CI step sampled a runner's memory, so
  nothing showed that anything held it. Every lane now records it (below).
- **B19-a4's swing.** The 280 s and 1,960 s case is `B19-a4 FULL GPU BINNING` in crd-kir-tests: the serial CPU
  interpreter `kir::eval_cpu_kernel` with fixed inputs, no GPU, no WARP, no lock. Over four win-asan jobs it took
  273, 279, 279 and 1,960 s (scheduled run 40849112570). In that run every case before and after it ran within 1 % of
  the other runs, so the runner was not slow around it: one process stalled for 28 min. The logs cannot attribute
  it (passing output is not kept); the host record now shows the memory at such a stall.
- **Growth.** Between f7304724 (2026-10-28, 16,639 s, green) and e496d16c (20,031 s) the shared cases grew 2,880 s
  and new cases added 547 s (the guard 371 s, the fft 1M arms 130 s). The growth sits in the pool-parallel hesap cases
  (AMG W-cycle 950 to 1,374 s, BLR, supernodal, `gemm_parallel`, eig, about 1.45x) and the forced arms: general code
  at the SSE2 baseline (49e4f622) and each forced arm running portable and its own level (5fbf494d), by design.

## The bound

At e496d16c win-asan ran every case serially: Test 5 h 34 min, build 18 min (warm cache), 7 min under the cap; a
green run then also runs the seeded faults, so serial cannot fit and it grows with the suite. The floor of a bounded
run is max(longest case 1,374 s, the `crd_gpu_device` chain 1,037 s over 781 cases, the serial guard 371 s, 20,031 s
/ jobs): 6,677 s at three jobs. `linux-clang-tsan` already ran 7,778 cases at four jobs, green, so the suite's cases
do not collide in one build tree; the temp-directory users name their files per case.

- **`CMakePresets.json`**: the `win-asan` test preset runs three cases at a time, one of the runner's four CPUs left
  for the pool cases. The seeded-fault step names its tests with `-R` and stays serial.
- **`ci-evidence.py watch`** runs a lane's test command with the host's available and committed memory sampled every
  5 s and passes the exit code on; it rewrites `Testing/Temporary/host-test.json` at every sample (`exit` null
  until the command returns), so a job stopped at the cap keeps the record. `bundle` records it with the
  machine (CPU, logical CPUs, memory) as `host`, the summary prints it, and a lane without it lists `host-test` as
  missing. The machine and memory readers and `PressureSampler` moved from `build-bench.py` into `ci-evidence.py`,
  which build-bench already imports. Every lane's Test step runs under `watch`.
- **Docs**: [ci-tiers](../design/ci-tiers.md#parallelism-and-shards) replaces "CTest still runs serially" with the
  rule and the bounds; REPO.DEV.5 records the follow-up.

## Checks

The tooling tests (`CiEvidence` and the board tests), the CI-tier check, actionlint, the plan and repository checks.

## Left

- The first win-asan run under the bound: all cases, green with the guard, the host record's least available memory.
- linux-gcc-arm64-asan's first run after a1901325 decides whether it needs a bound.
