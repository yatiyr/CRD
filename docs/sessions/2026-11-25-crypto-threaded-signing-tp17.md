# 2026-11-25: the threaded ML-DSA signing refusal registered as TP-17, intermittent

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path)'s track; the case is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the layout arms](2026-11-25-crypto-threaded-signing-layout-arms.md) and
[the reordered train](2026-11-25-net-path-reordered-train-black-hole.md). User decision on record (2026-11-01): "I
WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"; third-party defects are registered once and let be.

**CI first:** nothing to fix. No job of CI 45559790834 (65e76ee7) has failed; 45580605875 (8a9f5d04, the first with
21794a44) has not started. Host check 45579952691 (ead0f6c1) drew one Xeon Platinum 8573C (leg 4): all nine arms,
the case and `control-copy-slim` included, clean 40 of 40. The 8573C model now has one failing machine in five.

## What landed

- [TP-17](../third-party-defects.md#tp-17), on leg 8 of 45562483404's Cerid-free `control-copy-slim` (22 of 40
  refused, slowest 16.95 s). The nonreproduced register's entry is `explained` and points to it.
- The register can hold an **intermittent** entry: a `signature` (a regular expression the failing test's JUnit
  failure and output must hold, with no other `FAILED:` or `==pid==ERROR` report outside its matches) and
  `"intermittent": true`. Its test may pass or fail with that signature, while a skip or an absence stays red.
  Retirement is by a Cerid-free check, not by a pass: `control-copy-slim` clean on ten affected legs. The policy text,
  the gate section and the runbook's Read cell say so.
- `check-registered-failures.py` holds signatures on every entry (any lane, sharded or not). Tests cover a pass, a
  matching failure, one or two refusals, a non-matching failure, a refusal beside a second report or an ASan report, a
  skip, an absence, a signed non-intermittent entry and the register's refusals. TP-17's own signature is tested
  against the hosted report of 45496952342 and four lookalikes it must refuse (under 5 s, a refused second signing, a
  signature that did not verify, a fast exit).
- win-asan gates `Provider: one verifier and one key serve several threads` as TP-17, intermittent.
- `provider-host-check.py` takes several `--build` trees and runs each arm in each tree in turn on one machine.
  host-check's `provider_presets` input (default `win-asan`) configures and builds each tree with the same steps.
  A tree without the sanitizer is read only on a leg where win-asan's `control-copy-slim` failed.

## Evidence

- `test-repository-tools.py` RegisteredFailures and CiTiers pass, as do the provider driver case,
  `test-diag-coverage.py`, `test-ci-platform.py` and `test-ci-orchestrator.py`. Every repository validator passes.
- Without the sanitizer, three dispatches at 3f59e200 ran `case,control-copy,control-copy-slim,cerid` in win-asan and
  win-release. 45584330729 drew no affected runner. 45586029444's 8573C (leg 10) was clean in both trees, which
  determines nothing. On 45586011023's Xeon 6973P-C (leg 6), win-asan failed `control-copy-slim` 28 of 40 (25
  refusals, 3 fast exits `0xC0000409`), the case 25 and `cerid` 29, with `control-copy` at 0. In the same rounds,
  win-release ran all four clean 40 of 40 (slowest 0.08 s). So on that machine a build without AddressSanitizer does
  not reach it, and TP-17 has a second machine and model.
- The fast exits are Cerid-free too, but the gate does not admit them, because a crash with no output has no
  signature.

## Next

Tracing stops here, by the let-it-be rule. NET-PATH's Remaining follows: acknowledgement evidence sooner after a
fall's first flight, then the LEO case hardened on candidate A and the single SipHash draw.
