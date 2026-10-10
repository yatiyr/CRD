# DIAG.4b GenMC weak-memory model, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.4b](../ROADMAP.md#slice-diag.4b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-4b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [Needs CI audit, 2026-10-09](2026-10-06-needs-ci-audit.md#rows-closed-on-6bd1ba59-2026-10-09).

## Goal

The audit of 2026-10-09 returned DIAG.4b to Partial: its GenMC model
([counter_publish_reclaim.c](../../tests/support/diag/models/counter_publish_reclaim.c)) had never been run and no
GenMC was pinned. This session pins GenMC, builds it from the pinned source, runs the model, and adds the hosted lane.

**User decision (2026-10-10):** a GenMC CI lane in the complete/nightly tier, like the TSan lane; if GenMC cannot be
installed in CI in reasonable time, fall back to the recorded WSL run as declared evidence and say so in the row.
The install was measured as reasonable (below), so the lane was added and the row is Needs CI.

## Pin and helpers

- `cmake/pins.json` `tools/genmc`: GenMC **0.19.0** (2026-09-24), commit `9f6c4c0772d0c42b325681581acc6a0f3eb9b5f7`,
  the commit-addressed GitHub archive with its SHA-256 (downloaded twice, identical), license Apache-2.0 OR MIT (parts
  of `lli/Runtime` are LLVM 3.5 derived, NCSA as well; recorded in the entry's note). `llvm_major: "20"`: GenMC 0.18+
  needs LLVM 19-22, and 0.18 added the weak-CAS and CAS-failure-ordering support the model's orderings need, so an
  older GenMC on the hosts' LLVM 18 was not an option. LLVM comes from Ubuntu noble's apt (`llvm-20-dev`, `clang-20`,
  with `g++-14` for GenMC's C++23), so those four packages joined `unpinned.apt` and the lane records their exact
  versions with dpkg-query, as for the other OS packages. `check-pins.py` now also requires a tool that names a
  commit to be addressed by it.
- [install-genmc.py](../../scripts/install-genmc.py): fetch and verify through `pins.fetch`, extract with tarfile's
  `data` filter (one top-level directory, no escaping links), refuse an LLVM of another major, configure/build/install
  with CMake (Ninja when present), require `genmc --version` to report the pin, and write a stamp (pin digest and exact
  LLVM version) so a cached install is reused only while both match. Listed in `check-pins.py` HELPERS.
- [check-genmc-models.py](../../scripts/check-genmc-models.py): runs the three variants with `--disable-estimation`
  (the seeded ones with `-print-error-trace`) and holds each to its verdict; a seeded variant that verifies, or fails
  for another reason, fails the check. Writes JSON records and full transcripts for the lane evidence.

## The recorded run (WSL reference host)

The WSL host has no passwordless sudo and only LLVM 18 and g++ 13, so GenMC was built and run on the WSL reference
host inside an `ubuntu:24.04` container
(`ubuntu@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55`) limited to 4 CPUs, the hosted
runner's count, with the same apt packages a runner installs: `llvm-20-dev`, `clang-20` and `libllvm20`
1:20.1.2-0ubuntu1~24.04.3, `g++-14` 14.2.0-4ubuntu2~24.04.1, `cmake` 3.28.3.

| Step | Measured |
|---|---|
| apt install of the toolchain (home connection) | 30 s |
| `install-genmc.py --jobs 4`: verify, configure, build (149 Ninja steps), install | 69 s |
| model check, all three variants | under 1 s |

A second container installing only the workflow's apt line (`llvm-20-dev clang-20 g++-14 libedit-dev`, plus the
`cmake`, `ninja-build` and `python3` the runner already has) built the same GenMC in 68 s and passed the model check,
so `libffi-dev` and LLVM's other dependencies arrive with `llvm-20-dev`.

GenMC reports `GenMC v0.19.0, Built with LLVM 20.1.2`. Verdicts of the final model (RC11, no `-unroll`; bounds: one
counter slot, one Waiter, list length 1, four threads; GenMC times vary run to run, the counts do not):

| Variant | Exit | Verdict | Complete / blocked executions | GenMC time |
|---|---|---|---|---|
| repaired | 0 | No errors were detected | 6 / 2 | 0.13 s |
| `-DCRD_MODEL_BROKEN_HANDSHAKE` (park_finalized removed) | 42 | Safety violation: the generation assertion `... == snapshot` | 1 / 0 | 0.08 s |
| `-DCRD_MODEL_EARLY_DRAINED` (drained stored after the exchange) | 42 | Attempt to access freed memory | 4 / 2 | 0.38 s |

The install plus run costs well under the 20-minute rule of thumb even at a few times this host's speed, and the
built install is cacheable, so the hosted lane was added.

## The first run found an engine race

The model as authored never finished: its publish loop used `compare_exchange_weak`, which GenMC 0.19.0 warns it
"could not be strengthened" and then explores without end (the repaired variant ran past two minutes at full CPU
while `-mode=estimate` predicted about 7 executions); with `-unroll=1..6` GenMC itself crashes (exit 139) on the
repaired variant. Modelled as a strong CAS, the **repaired** model failed in 4 executions with an access to freed
memory: the decrement's claim CAS touched the Waiter after the waiting thread freed it.

That is a real engine race, separate from the zero-drain/release race the `drained` gate was added for:

1. `counter_decrement` (zero-decrement D) exchanges the waiters list, then stored `drained = 1`, then walked the
   stolen Waiters. D loads Waiter `w`'s claim as Pending and is preempted before its claim CAS.
2. `counter_finish_park` (scheduler S) sees value 0 in its ABA re-check, wins the claim CAS (Canceled), stores
   `park_finalized` and re-queues the fiber.
3. The fiber resumes, passes the `park_finalized` spin and the `drained` spin (D stored it before the loop), releases
   the counter and returns; `w` lived in `jobs::wait()`'s fiber branch, whose scope had already ended before the
   `drained` spin. The fiber's stack is reused.
4. D's claim CAS runs on the dead Waiter. Usually it fails harmlessly on stale bytes; if the reused slot holds 0
   (Pending), D writes Wakeup into the live stack and enqueues a resume of whatever `fiber` the bytes hold.

The fix (`engine/foundation/jobs/src/counter.cpp`, `jobs.cpp`): `drained` is stored after the drain loop, the
decrement's last touch of any stolen Waiter, and `jobs::wait()` declares its Waiter at function scope so it outlives
the `drained` spin. Woken Waiters are unaffected: their fibers resume only once the caller enqueues them, and
`run_job_in_fiber` reads each node before enqueueing it. The comments that said `drained` is set "right after the
exchange" were updated (counter.cpp, counter.hpp, jobs.cpp).

The model gained the `drained` gate (stored after the loop; the waiter spins on it before `free`) and a third seeded
variant, `CRD_MODEL_EARLY_DRAINED`, with the old placement: it must and does report the use-after-free, which is the
teeth for this fix. The weak CAS is modelled strong, with the reason in the header: a spurious failure only repeats
the relaxed store of the same head into the unpublished `w->next`, which no other thread can observe. The weak loop
was rerun on the fixed model: it still diverges without `-unroll` and crashes with `-unroll=3`, so this is a GenMC
limitation, not a model defect.

## Hosted lane

`.github/workflows/ci.yml`, job `linux-gcc`, on the `linux-clang-tsan` matrix entry only (complete tier), after the
TSan tests and regardless of their outcome: install `llvm-20-dev clang-20 g++-14 libedit-dev` and record their
versions into the lane census; restore `$RUNNER_TEMP/genmc` from `actions/cache` keyed on the LLVM package version and
on `cmake/pins.json` plus the helper; `install-genmc.py` (reuses a matching stamp, otherwise builds); then
`check-genmc-models.py`, writing `build/linux-clang-tsan/evidence/genmc/` into the uploaded evidence. The step is
declared in `.github/ci-tiers.json` under `preset_steps`, and `check-ci-tiers.py` now fails when a preset step is
missing from its preset's job, is not gated on that preset, or declares a different tier.

## Verification

- GenMC: the three verdicts above, through `check-genmc-models.py` (PASS), rerun on the committed model. The
  checker's own teeth: pointed at the pre-fix model (strong CAS, no `drained` gate) it fails the repaired variant
  with "Attempt to access freed memory" (exit 1).
- Engine, gate off: `crd-jobs-tests` win-debug 174 cases / 29729 assertions, win-shipping and win-clang-cl-shipping
  166 / 29706, win-asan 174 / 29729. Gate on: `crd-jobs-schedcheck-tests` win-debug 6 / 45, 50 repeated runs
  without a failure (it includes the perturbed multicore park/reclaim stress).
- Linux (WSL, one job at a time): `crd-jobs-tests` 173 / 29719 on `linux-clang-tsan` (under ThreadSanitizer with the
  lane's options), `linux-gcc-debug` and `linux-gcc-asan`; `crd-jobs-schedcheck-tests` 6 / 45 on `linux-gcc-debug`.
  On the final tree the TSan executable passed 6 of 7 runs without `setarch -R` and 2 of 2 with it; the one failure
  died at startup (`ThreadSanitizer: BUS on unknown address`, a high-address read, "nested bug", no test output),
  the WSL2 address-randomization failure the repository lessons record (TSan on WSL2 needs `setarch x86_64 -R`).
- `tidy-files.py` clean on the three engine files; clang-format `--dry-run --Werror` clean on every changed line and
  on the whole model; `check-allman-braces.py`, `check-pins.py`, `check-ci-tiers.py`, `gen_license_manifest.py
  --check`, `check-generated.py`, the tooling tests, the pinned actionlint, the ctest guards and both validators.

## Remaining

The first hosted complete run of the GenMC step. An engine-level replay of the new race (D parked at `dec.claim`
while S cancels and the fiber unwinds) needs a dead-Waiter sentinel the gate-only detector does not have; it is
queued as a follow-up rather than done here.
