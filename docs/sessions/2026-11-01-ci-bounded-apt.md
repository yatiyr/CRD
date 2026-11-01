# 2026-11-01 — CI: every apt-get wait bounded after a five-hour index stall

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (tiered sanitizer CI), for the complete-tier evidence of
[DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12). Before:
[the ASan shards](2026-11-01-diag-ci-asan-shards.md).

## CI read

Complete-tier run 41254049879 (31404ec4) ended red. Five failed jobs were already fixed on main: win-tidy
(`test_arguments.cpp`, a40af1bf), linux-gcc-shipping and linux-gcc-release (the deflate test's null dereference,
95ec6965), win-relwithdebinfo (the coordinator race in `crd-ci-orchestrator-e2e`, aa05dd37) and win-asan shard 1 (the
sample-ring producer, a93cf0ca). The new one is linux-clang-tsan, cancelled at its 300-minute job limit with no test
run: its "Install system dependencies" step printed `Get:5 ... noble-security InRelease` at 23:50:25, after the azure
mirror answered `Ign` and apt fell back to archive.ubuntu.com, and nothing more until the cancel at 04:48:59. Nothing
bounded the fetch, so one stalled index download took the lane's TSan cases and the GenMC arm.
Run 41269105413 (b0b1b245, running): its early Windows failures are `checksum.cpp` C4324 (921e19cc) and the same
coordinator race, both fixed on main.

## Change

- [install-apt-packages.py](../../scripts/install-apt-packages.py) runs each `apt-get` as
  `sudo timeout --kill-after=30 <bound>` (the kill reaches the root-owned process), with `Acquire::Retries=3`,
  60-second http and https timeouts and a 120-second dpkg lock wait; an index refresh is bounded at 180 s and an install
  at 600 s, each tried three times, an interrupted install first running `dpkg --configure -a`; the last failure names
  every attempt's outcome. A stall now fails the step in about nine minutes with that message.
- Every apt step in `ci.yml` (three package installs and the GenMC toolchain) and `host-check.yml` (three) calls it,
  and each carries `timeout-minutes: 60`. Our own CI's lane (`ci/pipeline.py` `apt` and `genmc`) calls the same
  helper, as its parity test requires; `test-ci-platform.py` reads the package lists from the new form.
- `test-repository-tools.py` checks the bounds, options, retry, recovery and message, and fails any workflow that calls
  `apt-get` directly or runs the helper in a step without a step timeout; both teeth proven (no recovery, a step
  without its timeout). A WSL run of the helper's attempt loop with a stalled command read `timed out after 1 s`
  twice (coreutils exit 124). Pinned actionlint 1.7.12, `check-ci-tiers.py`, `check-pins.py`, the repository tools
  (272) and the CI platform suite (99) pass.

## Needs CI audit

- HW-RESILIENCE.2: no completed run at or after 921e19cc yet; queued complete-tier run 41274380605 (de5e10b0) is the
  first.
- DIAG.11c and DIAG.12: 41254049879 is red and has no TSan or GenMC result; 41274380605 (de5e10b0, which carries every
  fix above but not this one) is the next candidate. Nothing dispatched: one run is queued and one in progress.
