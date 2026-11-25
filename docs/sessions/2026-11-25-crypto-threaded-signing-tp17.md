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
  Retirement is by a Cerid-free check, not by a pass: `control-copy-slim` clean on ten Xeon 6 legs.
- The gate holds signatures on every entry, sharded or not. Its tests take every state: a pass, one or two matching
  refusals, a mismatch, an extra or ASan report, a skip and an absence. TP-17's signature passes 45496952342's
  hosted report and refuses four lookalikes.
- win-asan gates `Provider: one verifier and one key serve several threads` as TP-17, intermittent.
- `provider-host-check.py` takes several `--build` trees and runs each arm in each tree in turn on one machine.
  host-check's `provider_presets` input (default `win-asan`) configures and builds each tree with the same steps.
  A tree without the sanitizer is read only on a leg where win-asan's `control-copy-slim` failed.

## Evidence

- Script suites pass: test-repository-tools, test-diag-coverage, test-ci-platform and test-ci-orchestrator. Every
  repository validator passes.
- Three dispatches at 3f59e200 ran four arms in win-asan and win-release. On 45586011023's Xeon 6973P-C (leg 6)
  win-asan failed `control-copy-slim` 28 of 40 (25 refusals, 3 fast exits `0xC0000409`), the case 25 and `cerid` 29;
  `control-copy` passed 40 of 40. win-release (Release with LTO, so the layout differs too) went 0 of 160 in the same
  rounds. 45586029444's 8573C was clean in both trees; 45584330729 drew no affected runner.
- The fast exits are Cerid-free as well, but the gate does not admit them: a crash with no output has no signature.

## Next

Tracing stops here, by the let-it-be rule. NET-PATH's Remaining follows: acknowledgement evidence sooner after a
fall's first flight, then the LEO case hardened on candidate A and the single SipHash draw.
