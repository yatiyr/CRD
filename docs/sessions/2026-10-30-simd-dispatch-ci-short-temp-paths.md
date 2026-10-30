# 2026-10-30 — SIMD-DISPATCH: the hosted Windows short temp path and the SELL board's wait

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards, the SELL family). Before:
[the CI orchestrator CTests](2026-10-30-simd-dispatch-ci-orchestrator-ctests.md).

## CI first

The cancelled push runs 41118715232, 41118429795 and 41108944693 hold no failed job. Push run 41112483075
(`37ebb440`, in progress, not cancelled) failed `Repository checks (windows-2025-vs2026)` at `CI platform contracts`:
nine store tests raised `ValueError: '...\runneradmin\...\a.json' is not in the subpath of '...\RUNNER~1\...'`.

- Cause: the hosted runner's `TEMP` is an 8.3 short path. At `37ebb440` `LocalStore.list` took each file from a
  resolved `rglob` (long names) and made it relative to the unresolved root (the short name).
- Reproduced here: `37ebb440`'s suite with `TEMP` and `TMP` set to the short form of a new directory on C: (D: has
  no short names) gave the same nine errors.
- Already fixed on main: since the orchestrator merge (`e9841a6e`, `9ab78f61`) every `LocalStore` path is taken
  against `root.resolve()`. HEAD's `test-ci-platform.py` (76 tests), `test-ci-orchestrator.py` (76) and
  `test-ci-orchestrator-e2e.py` (3) pass with the same short temp path. The hosted Windows repository job sets
  it on every run, so that job keeps the case covered.

Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none dispatched (push runs pending).
Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only); no secrets,
so the AMD and Mac lanes wait on their accounts.

## The SELL board

The board is ready in WSL: `~/sell-board/board.sh` (five passes), the probes `sell_probe_prerow` and
`sell_probe_head` (built at `5afef104`; no engine or bench source has changed since), and `parse_board.py`. A dry run
of the parser on the single-pass outputs already on disk rendered every table.

`build-bench.py host` refused the host on every sample for 36 minutes (two bounded watches): other processes 4.7 to
9.7 cores together, dota2 2.3 to 3.3 cores and 3.5 GiB, Studio One 1.1 to 1.2 cores; 19.1 to 21.0 of 63.7 GiB
available. A contended run would widen the run-to-run noise that the acceptance rule judges against, so the board
did not run.

## Left

The SELL board's five passes on a quiet host, then permute, the sse2 bodies and the retune's thread sweep.
