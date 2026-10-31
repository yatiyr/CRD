# 2026-11-01 — DIAG: the ASan lanes in shards, one job limit and the overhead recording's frame count

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (tiered sanitizer CI), for the complete-tier evidence of
[DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12). Before:
[linux-gcc-asan's bound](2026-11-01-hw-resilience-2-ci-hosted-identity.md).

## CI read

Complete-tier run 41247045494 (67977fda, still running) failed win-debug, win-asan and win-public-checks at the build:
`test_ceridc_screen.cpp` defaulted three `std::initializer_list` members, which the hosted MSVC 19.51.36260 refuses
(C2797) and this host's 19.51.36246 accepts. d9be0cfd gives each repeated flag the leading slots of a null-ended
array (and three declarations clang-format's spacing); the screen cases pass on win-debug. The push runs since the
last batch were cancelled by newer pushes before their jobs started.

## Why shards

The complete run 41215667488 (c2645814) cancelled linux-gcc-asan at GitHub's 360-minute cap after 6,811 of 8,766
cases (20,331 s of serial tests; about 26,000 s for all), and win-asan, already bounded at three jobs, spent 292
minutes in its Test step and 315 in the job: under an hour from the cap. Its bound alone gained little over the
serial sum (20,031 s, 334 minutes, at e496d16c); why is the open win-asan item below. A second runner halves both
the cases and the `crd_gpu_device` chain.

## What changed

- `.github/ci-tiers.json` gives `win-asan` and `linux-gcc-asan` `"shards": 2`. `ci-tier.py resolve` publishes every
  job's legs as matrix `include` entries: `{"preset"}`, or `{"preset", "shard", "shards"}` per shard; every preset job
  takes `include:` so whole lanes keep their job names.
- Leg K of N runs CTest's stride selection `-I K,,N` (`ci-evidence.py watch --shard=K/N`). On the win-debug tree
  the two selections hold 4,615 and 4,614 of 9,229 tests; the suite registers no fixtures and no `DEPENDS`.
- `ci-evidence.py inventory --shard=K/N` records the lane's inventory (`inventory-lane.json`) beside the shard's and
  fails the leg unless the N selections partition the lane (each test in exactly one shard; only fixture providers,
  which CTest adds to every selection needing them, in more). `bundle --shard` records `shard` in
  `conclusion.json` and the summary; `compare` reads a sharded lane from all its shards' directories.
- `check-registered-failures.py --selection --lane-inventory` holds a shard to the registered tests it runs and fails
  every shard when a registered test is in none, so the shards' gates together hold the lane to its register
  (linux-gcc-asan's TP-5 lands in exactly one shard).
- The seeded detector failures run on shard 1 only; each leg uploads `evidence-<preset>-shard-<K>-of-<N>`; the
  required aggregate already fails when any leg of a job fails.
- Every job declares `timeout-minutes` of at most 300 (GitHub's limit less the hour the parallelism rule keeps).
- `check-ci-tiers.py` holds all of it: the shard count, the leg's `CRD_SHARD` in the inventory, watch and bundle
  calls, the gate's lane inventory, the per-leg evidence name, shard-1-only preset steps and every job's limit.

## Verification

`test-repository-tools.py`: 226 cases pass. Teeth: a gate that keeps the whole register on a shard, a partition check
that never reports a missing test and a resolver that ignores `shards` each fail their new cases; the validator
cases mutate the workflow (no `CRD_SHARD`, a bundle without `--shard=`, one evidence name, the seeded step on every
shard, no lane inventory, a missing or 360-minute limit, a `preset:` matrix). `inventory --shard=1/2` and `2/2` on
the real win-debug tree verified the partition. actionlint 1.7.12 and `check-ci-tiers.py` pass;
`test-ci-platform.py`, `test-ci-orchestrator.py`, `test-diag-seeded-faults.py` and `test-diag-coverage.py` pass.

## DIAG.11b clause (8): the user's two recordings

The user recorded twice at b562ba6e (harness trees in a clean worktree, `record`'s currency
check passed); both run directories are in `build/diag-overhead-wpr`. `20261031T211608Z` ran beside a
build and read 0 DISAGREES with most cells unresolved on spread. `20261031T215648Z` read 0 DISAGREES: jobs full AGREES
(+40.8% measured, +41.4% external), jobs compiled-out and alloc basic, compiled-out and full and offline basic AGREE;
io and offline compiled-out and full are unresolved on spread (a build resumed as it started); jobs basic is
unresolved only on frame coverage, 0.8456 and 0.8461 against FRAME_FRACTION 0.85.

Every basic and full process spent about 0.36 s outside its frames against 0.03 to 0.04 s for off and compiled-out,
with near-identical CPU samples: a wait. It is not start or exit latency. The harness asks for three captures
after its frames, and the overhead policies close a capture's post window after 100 ms when nothing arrives
(`assets/diag/overhead_basic.flight.toml`, authored so). Timed on the win-debug harness, a basic alloc process with
0, 1 and 2 captures took 0.041, 0.142 and 0.246 s, each capture 102.6 ms (`longest_ns`). The script's sizing comment
had called this time setup.

What broke the coverage was the frame count: `ITERATIONS` fixed jobs at 9,000 frames when a jobs frame took 370 to
417 us (2026-10-19); it now takes 218 us, so the frames lasted 1.97 s instead of at least 3.2 s. `record` no longer
fixes counts: before the sampler starts it times 500 off frames of each workload and sizes every process to 3.2 s
of off frames (`calibrate`; the manifest keeps the counts). On the b562ba6e shipping harness here: alloc 37,759,
jobs 14,935, offline 8,971 and io 3,989. FRAME_FRACTION and every other limit are unchanged.
`test-sample-overhead-wpr.py` covers the calibration, its failure (refused before the sampler) and the measured
counts; a fixed count fails two cases.

## Left

DIAG.11b clause (8): the user's re-recording with calibrated frame counts (command in the batch summary).
Hosted: the next complete-tier run (dispatched once 41247045494 ends) shows each ASan lane's two legs with the
partition line, their own evidence artifacts, the gate passing on each leg and every job inside 300 minutes; DIAG.11c
and DIAG.12 are audited against it. Open from the 2026-10-29 operator note: what holds win-asan's memory (the host
sample read 1.0 of 64.0 GiB available) and whether B19-a4 GPU binning's 280 s against 1,960 s swing is WARP or
contention.
