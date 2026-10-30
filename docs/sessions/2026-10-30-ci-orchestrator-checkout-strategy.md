# 2026-10-30 — The orchestrator's checkout strategy decides the fetch URL (the Linux end-to-end defect)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-ci-1).

Row: [CI-1](../ROADMAP.md#slice-ci-1). Contract: [CI platform, the Cerid orchestrator](../design/ci-platform.md#the-cerid-orchestrator).
Reported by the serial workflow: [orchestrator CTests, "Linux end-to-end defect"](2026-10-30-simd-dispatch-ci-orchestrator-ctests.md).
Earlier work: [the orchestrator session](2026-10-30-ci-orchestrator.md).

## The defect

`crd-ci-orchestrator-e2e` failed on Linux: the `plan` job's checkout fetched
`http://127.0.0.1:<port>/tmp/.../demo`. `ci/agent/agent.py` decided "this is the coordinator's mirror" from the URL's
shape (a leading `/`), but a `direct` checkout of a POSIX filesystem repository starts with `/` too. Windows passed
only because its repository path starts with a drive letter. The hosted repository job on `ubuntu-24.04` runs the
same end-to-end test, so it would have failed there as well.

## The fix

- The agent's `resolve_checkout()` keys on `checkout.strategy` alone: `mirror` puts the coordinator's `/git/...`
  path after the coordinator address the agent already uses; `direct` fetches the configured URL exactly as given,
  whatever its shape (https, ssh, a POSIX or Windows path, `file://`). An unknown strategy, or a mirror checkout whose
  path is not the coordinator's `/git/`, is refused instead of guessed.
- Every other place that inferred the strategy was made explicit the same way: the coordinator pinned every build's
  commit in its mirror whenever a mirror existed, even for `direct` repositories; it now pins only `mirror`
  repositories. An unknown `checkout` value was silently treated as `direct`; `scheduler.CHECKOUT_STRATEGIES` now
  bounds it, `ci/coordinator/main.py check` reports it, and an assignment with it is refused. (The other prefix tests
  in the orchestrator are not strategy signals: the artifact-path check refuses absolute paths, the store opener keys
  on its `gs://` and `file://` schemes, and the HTTP router keys on its own route prefixes.)
- Tests: `AgentProgram.test_the_strategy_alone_decides_where_a_checkout_fetches_from` runs the agent's checkout with
  git replaced by a recorder for `/srv/git/cerid.git`, https, ssh, a Windows path and `file://` (direct) and for the
  mirror; it feeds the path strings, so it runs on every host. Against the previous `agent.py` it fails with exactly
  the reported URL (`http://127.0.0.1:8780/srv/git/cerid.git`). `Core.test_checkout_strategy_is_explicit_and_validated`
  covers the coordinator's assignment, mirror pinning and configuration check; it fails on the previous coordinator
  (a direct repository was pinned, an unknown strategy was accepted).

## The unreproduced suite error recorded earlier

What was tried to reproduce the one `test-ci-orchestrator.py` error recorded in the orchestrator session: 36 runs of
the whole suite (78 tests) in shuffled orders (seeds 1000-1011, 2000-2011, 3000-3011), three processes at a time for
parallel load, every error and failure printed with its traceback; then 25 runs of the five concurrency-sensitive
tests (compare-and-swap across processes and threads, concurrent polls and uploads, the process-tree kill) in a loop.
No error or failure appeared. During this session's validator pass the same host produced another impossible,
non-reproducing error: `check-allman-braces.py` (unchanged, scanning unchanged C++ files) raised
`TypeError: 'tuple' object is not an iterator` from `enumerate()` over a string, and passed on the next two runs.
This host is recorded as misexecuting under load (MEMORY: "This host misexecutes"; non-reproducing local failures
are rerun and recorded, and hosted lanes are the evidence), which is the likeliest cause of the suite error too. It
stays recorded as unexplained rather than fixed; the hosted repository job prints the traceback if it ever recurs
there.

## Verification

Python only: `python scripts/test-ci-orchestrator.py` (78), `python scripts/test-ci-orchestrator-e2e.py` (3, Windows
host), `python scripts/test-ci-platform.py` (76), and the quick validators. The Linux end-to-end run is the hosted
repository job's and the serial workflow's `crd-ci-orchestrator-e2e` on its WSL lane.
