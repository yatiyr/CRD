# Hosted CI triage, 2026-10-05/06

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.3b.2](../ROADMAP.md#slice-repo.dev.3b.2). Rules: [AGENTS](../../AGENTS.md).

## A status-read retry count was asserted exactly

`crd-developer-workflow` failed on `win-release` at `a45059fc`:
`test_supervisor_retries_status_read_without_repeating_native_command` expected `status_read_retries == 3`, and the
hosted runner counted 4.

The supervisor (`scripts/cerid_dev/process.py`) retries every `PermissionError` on `native-status.json`. That is what
the retry is for: the child renames the completed status document while Windows may still deny a reopen. The fixture
injects exactly three denials, and a real sharing denial on the runner added a fourth. The production behaviour was
right; the test over-specified it.

The test now asserts what the code guarantees:
- all three injected denials were consumed;
- the retry count is at least three;
- the native command still ran exactly once (its marker file holds a single line).

The 54 tooling tests pass locally.

## Runner losses, not code

Most failures on `aabbc97a`, `2dea4e6a`, `4988a1ec`, `a45059fc` and `b3f5df56` carry runner-side annotations, with no
test or compile error in their logs:
- "The hosted runner lost communication with the server": `win-public-checks` three times, `win-relwithdebinfo`
  twice, and `win-shipping`.
- "The runner has received a shutdown signal" (exit 143): `linux-gcc-debug`.
- "The job was not acquired by Runner of type hosted even after multiple attempts": preflight on two runs.

The repository is public, so Actions minutes are not metered. Several complete-tier runs of about 25 jobs each were
queued at once, against the account's concurrent-job limit. Re-running the affected jobs is the remedy. Recurring losses
on one lane would point to memory pressure in its build and get their own investigation.
