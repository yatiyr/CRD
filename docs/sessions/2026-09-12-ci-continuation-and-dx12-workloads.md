# CI continuation and DX12 workload verification

<!-- doc-role: historical -->
> Dated direction/evidence. Live owners: [REPO.3c.4.a](../ROADMAP.md#slice-repo.3c.4.a),
> [REPO.3c.4](../ROADMAP.md#slice-repo.3c.4), [REPO.3c.5](../ROADMAP.md#slice-repo.3c.5). Rules: [AGENTS](../../AGENTS.md).

## Latest user direction

The user explicitly clarified: start with the earliest unfinished work unless instructed to a future slice; when CI
output is needed, state that and carry on available work without dropping gaps. This refines the earlier same-day
[strict-order correction](2026-09-12-strict-slice-order.md). The canonical rule remains in ROADMAP. Needs CI preserves
an unclosed evidence gate and allows the next available work; it never implies Done. Explicit future-slice scheduling
requires a real user instruction recorded and linked in the owning row, not an inferred agent preference.

## Publication observed

The human published `7201a7b818ee6b363aac6cfbf97f94fa96d8fb85` ("solidifying the repository.").
[Run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010), created 2026-09-12 19:25:49 UTC, was active
when checked. Its published diff includes the five classifier API/implementation/policy/census/test files (231
insertions, 18 deletions relative to 2ed89c4). Local evidence remains the
[classification proof](2026-09-12-dx12-adapter-classification.md#subsequent-qualification-and-completed-ci-census) and
[affected hardware/WARP checks](2026-09-12-atomic-abuffer-emitter-repair.md). The hosted tuple is not yet qualified.

Windows runtime jobs were still building and their Test steps had not completed. The existing workflow executes the
verbose native device census before the complete CTest selection. No CI output from the new classifier was available
at this observation. The Ubuntu repository-check job had passed, an incidental observation rather than closure of a
later slice. Synchronizer was watching with a finalized baseline, no incomplete transaction and no active generation.
Sandboxed `git status` emitted global-ignore access warnings; that instrument is not used to claim a clean checkout.

REPO.3c.4.a and the refreshed census REPO.3c.4 therefore need CI output. All already available local classifier/census
work and the older complete failure census are retained. The next available work is REPO.3c.5: B18 hair/fur/scattering,
RT-4 and impostor consumer failures. B18/RT local hardware/WARP proof already exists; the impostor case still needs a
discriminating reproduction and root cause. Keep precision/output/consumer assertions unchanged. No later row is
implemented, no geometry/physics algorithm change or renderer review acceptance is inferred, and no agent publishes.

## Execution and verification

Updated the existing loop and compact entry/quality/research documents to match this clarification. The table/query
distinguishes Needs CI from completed work and from an ordinary blocker. The validator requires local session evidence
and an actual published run for Needs CI. Referenced explicit user overrides are checked separately; none is used to
select this session's ordinary next row. Validation/results are recorded after the scoped work below.

## Actual impostor reproduction and diagnostic outcome

All envelopes below are under ignored `build/research-dev-workflow-20260912/`. Source/model identities were held
stable inside each qualification. Tests are correctness observations, not performance measurements; overlapping
selections must not be summed as unique coverage. Primary build: existing win-debug, MSVC 19.51.36246, two workers,
scoped CTest with a 180-second per-test bound. No full local sweep ran.

- `impostor-baseline-223337`: affected executables rebuilt; census plus actual Vulkan/DX12 impostor consumers
  **3/3 passed**, zero skips/failures, exit 0, on the NVIDIA RTX 4070 Ti SUPER (driver 32.0.15.9579).
- `warp-223434-2828ea`: default-device path forced through the three exact registered diagnostic executables;
  census passed, actual impostor consumer failed (1 executed, 0 skipped, CTest exit 8). CPU count 25, GPU count 0,
  centre/sky/delta all zero. Original app/device settings restored and verified. Local WARP is 10.0.26100.8972.
- `impostor-baseline-223616`: temporary DX12 debug-layer plus InfoQueue1 callback instrument initialized with
  HRESULT 0 for both enable/acquisition and registration. Hardware selection executed all three cases: census and
  Vulkan passed, DX12 failed with the new diagnostic; overall 2 pass/1 fail, zero skips, CTest exit 8. Retain this
  failure despite the ordinary non-instrumented path passing. The callback logged warnings/errors without filtering
  them out of the saved trace; this temporary probe is not the eventual bounded production capture.

Observed validation IDs included resource-state mismatch 527, invalid descriptor placement 646, incompatible stage
signature 660, missing attachment outputs 679 and resource/clear warnings 1328/820/821. Source inspection established
the frame heap is 256 descriptors and cursor writes have no capacity check. The color indexed-indirect function ends
after ExecuteIndirect without the buffer-state restoration present in its depth-only counterpart. These are concrete
source/validation defects; other warnings still require individual analysis and no full causal chain is claimed.

Automatic approval review rejected the first capacity-experiment command because it lacked guaranteed restoration
of its temporary tracked-source change. No action from that rejected command ran. The safer wrapper saves recovery
before mutation, bounds both nested runs, restores exact original bytes in finally, checks for concurrent source
edits and verifies restoration. It was accepted and completed:

- `capacity-counterfactual-224153`: temporarily changed only frame capacity 256→4,096. Hardware's three cases passed;
  WARP's impostor still failed with CPU 25/GPU 0 and black output (`warp-224237-3cef0a`, native CTest exit 8).
  The wrapper retained failure exit 8 and verified exact source restoration. The outer PowerShell invocation reported
  1 because it did not explicitly forward the native exit; nested bounded native-status evidence retains the actual 8.
  WARP settings restoration was independently recorded as verified. Capacity alone is not a sufficient repair.
- `impostor-baseline-224754`: after removing the debug callback and restoring the original 256-slot source, rebuilt
  all affected executables and passed the same **3/3 hardware cases**, zero skips/failures, exit 0. Source/model stable.
  `git diff --numstat` for both instrumented engine files was empty. No temporary diagnostic is left in engine source
  or in the current rebuilt executables. This does not erase either the WARP failure or the validation errors.

Original source snapshots and the diagnostic instrument are preserved in the ignored directory. No production C++
repair is claimed by this investigation; LLVM-20 qualification belongs to the coming persistent implementation.
The [repair contract](../design/dx12-workload-repair.md) records source owners, primary references, exact requirements
and REPO.3c.5.a–d in order: reliable validation → states → descriptors → complete consumer qualification. The parent
retains the original B18/RT/impostor scope. Context selects the first new child; no unrelated later work was started.

## Published CI and handoff

The later compact CI snapshot (`ci-7201-current.json`) showed both repository-check jobs passed and Windows ASan
in its Test step; other runtime jobs were still building. No classifier result from those unfinished jobs was claimed.
REPO.3c.4.a/4 continue to need CI output. Inspect changed results before the next implementation increment and return
verified failures to their earliest owner. Local hardware passing does not establish the hosted flag-0 provider.

The existing heartbeat was updated in place to retain CI waits while driving earliest available work. Its ID, thread,
15-minute cadence and quiet unchanged-state behavior were preserved. No second loop, tracker, agent, commit or push.
Updated-rule verification: **13/13** repository-tool regressions passed, including CI-wait and explicit-user-order
cases. Final documentation check passed **863 rows, 1,015 documents, 8,697 local links**, 210 D-007 and 16 v17 routes;
all orientation budgets passed. Repository hygiene passed all 96 module/registration/ignore contracts; `git diff
--check` passed. Synchronizer readback remained watching, finalized, with no incomplete transaction or generation.
Orientation rules were inspected and affected text corrected; PRINCIPLES/CODING remain applicable. This is a progress
turn, not a publication-only impasse. Suggested human commit: `docs(workflow): retain CI gates while continuing ordered work`.
