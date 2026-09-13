# Third-party defect register and the two-sided ASan gate

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3c.10](../ROADMAP.md#slice-repo.3c.10); register: [third-party defects](../third-party-defects.md).
> Rules: [AGENTS](../../AGENTS.md). Preceding batch: [route and pinned WARP](2026-09-13-inner-coverage-route-and-pinned-warp.md).

## User direction

After the pinned-WARP withdrawal the user asked what the ASan lane is and what fails there, and decided: a defect that
is truly not Cerid's is closed and documented fully in a separate document, and a known third-party defect must not
keep CI red. The user chose the register plus a two-sided gate over a permanently red lane and over splitting the RT
gates onto a pinned provider. Same session loop, same rules; no commit or push by the agent.

## What the lane is and what fails

`win-asan` is the Windows Debug preset compiled with MSVC AddressSanitizer, running the same tests as the other Windows
lanes; it exists to catch memory defects the other lanes survive silently. At `0b858a6` its failing set was the two
guard/coverage failures repaired in the earlier batches plus four raytracing-pipeline gates. Those four fail on one
report: an 8-byte read by `memmove` called from the inbox `d3d10warp.dll`, 0 bytes past a block allocated by
`D3D12Core.dll` whose size equals a DXIL library blob, before any GPU work. No Cerid frame is on the stack, a program
with no Cerid code reproduces it, and the same executables pass with Microsoft's signed 1.0.20 WARP beside them. The
[register entry TP-1](../third-party-defects.md#tp-1) carries the full evidence chain and the retirement trigger.

## Design

- **Register, not suppression.** `docs/third-party-defects.md` admits an entry only with a reproduction outside
  Cerid, the exact tests and lanes, the engine handling that keeps every oracle intact, and a retirement trigger.
  Four entries: TP-1 (this over-read), TP-2 (the false native inner-coverage bit, handled by the route contract),
  TP-3 (the 1.0.20 divergences), TP-4 (the workstation `d3dconfig --export` failure).
- **Two-sided gate.** `scripts/check-registered-failures.py` reads the register's fenced JSON block and CTest's
  `--output-junit` document for the lane. Exit 0 only when the failing set equals the registered set: an unexpected
  failure, a registered test that passed, was skipped or did not run, and a CTest exit that is not a plain test
  failure all fail the lane. Lanes without entries require zero failures and propagate CTest's exit code, so every
  other preset behaves exactly as before. The defect ids in the block must have anchors in the document.
- **Wiring.** The Windows matrix Test step writes JUnit beside the CTest logs, keeps the census probe's own exit, and
  ends with the gate. The diagnostics artifact now includes the JUnit file. ASan itself is unchanged and its reports
  stay in the lane log.
- **Tests.** Five `RegisteredFailures` cases in `scripts/test-repository-tools.py`: exact set passes and is reported,
  unexpected failure and unexpected pass both fail, skipped or absent registered tests fail, lanes without entries
  require zero failures and propagate the exit code, and the register and JUnit evidence are validated (including
  the real register: only TP-1 on `win-asan`, lane keys are configure presets).

## Verification

- Tooling tests **21 of 21** (16 existing plus the five `RegisteredFailures` cases, including the real register).
- End to end on retained local JUnit evidence: the OS-WARP ASan arm (`warp-134948-07eef4`, four gates failing)
  yields `gate: PASS` with exit 0; the pinned arm (`warp-135107-ac7699`, four gates passing) yields `gate: FAIL`
  naming each registered test as an unexpected pass, which is the retirement path working.
- Workflow parses (8 jobs); validator PASS (863 rows, 1,031 documents, 8,889 local links); `git diff --check` clean;
  orientation budgets kept (BUILDING 6,993 bytes, context 2,264 bytes); edited CRLF files keep uniform endings.

## Hosted outcome of run 34757652779 (`ae44264`, before the gate)

Fifteen of sixteen jobs green: both repository checks, all six Linux lanes including ASan, win-debug, win-release,
win-debug-sse2, clang-cl, both Windows shipping lanes and the clang-tidy lane, which completed for the first time in
three runs. The one red job is `win-asan`, and its `LastTestsFailed.log` holds exactly the four registered names and
nothing else. Its census reports `driver=10.0.26100.33296`, the software adapter; the four AddressSanitizer reports are
heap-buffer-overflow reads located 0 bytes after D3D12Core-owned regions of 2,568, 2,924, 2,924 and 3,788 bytes, the
TP-1 signature. The register's set is therefore the hosted failing set, which is what the gate will compare against.

## State

REPO.3c.3 through REPO.3c.9 are Done on that run. REPO.3c.10 stays Needs CI: the engine contracts are proven, the
residual is registered and confirmed on the hosted lane, and the remaining confirmation is the gate's first hosted run
after the next push. REPO.3c stays Needs CI behind it; the audited REPO.DEV rows are Done; the pointer is REPO.DEV.3b.3.
