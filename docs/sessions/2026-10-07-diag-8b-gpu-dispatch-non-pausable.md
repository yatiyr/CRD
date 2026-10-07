# DIAG.8b GPU dispatches are non-pausable at the execute_lowered seam, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8b](../ROADMAP.md#slice-diag.8b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8b pending jobs](2026-10-07-diag-8b-pending-jobs-and-detached-bodies.md).

## Goal

Third DIAG.8b batch: the contract's "distinguish ... nonpausable GPU/real-time work" made real at the seam that
records GPU work, instead of leaving it to the scope a session declares.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- `inspect::Session` had a `NonPausable` scope, but only as a property the host declares for the whole session. A
  `Task` session had no way to know that some of the work it covers runs on the device.
- crd-ceir-gpu's `execute_lowered` records each `compute.dispatch` into a `ComputeRecorder`; the work runs later on
  the device, after `submit_and_wait`, where no safe point exists. It took no session and could not be cancelled.
- Breakpoints on dispatch ops are reachable through the module form of `Session::bind` (a compiled plan holds no
  dispatch), because the lowered commands keep their source op.

## What changed

- **`Session::begin_device` / `device_point` / `end_device`** (`crd/ceir/inspect.hpp`): a recording attaches as an
  execution of the new private kind `Device`, under the same lock as its checks (`NotBound`, `StaleGeneration`, and
  `Busy` while any execution or recording is attached, so the breakpoint table stays read-only). `device_point` never
  waits and never touches the stepping or pause-request state: a breakpoint bound to the op increments
  `device_hits()` and `refused_pauses()` and records `NonPausable`, whatever the session's scope; it returns the
  session's cancel flag. `request_pause` while a recording is attached is refused `NonPausable`.
- **`execute_lowered(..., DispatchSites* sites, DeviceInspect* inspect)`** (crd-ceir-gpu): with a `DeviceInspect`
  (session, generation, refusal out) the call attaches before any work (a refusal returns the new
  `ExecuteError::InspectRefused` with the refusal and records nothing), calls `device_point` before recording each
  dispatch, and stops before recording a dispatch once the session is cancelled (the new `ExecuteError::Cancelled`,
  blamed on that dispatch through `DispatchSites`). A scope guard detaches on every return. `execute.hpp` only
  forward-declares the session, so its consumers do not pull in `<thread>` or the plan headers.
- **Decided gaps**, written into the header and the design: a recording is exclusive, so a host op that records
  device work in the middle of an interpreter execution is refused `Busy` (no such host op exists yet); only
  `execute_lowered` is classified, not `execute_rt_lowered`, `execute_work_lowered` or the render executor.

## Tests

The program is the two-dispatch fixture of `dispatch_provenance_fixture.hpp` (authored as text under a named file,
CSE, serialized, loaded into a fresh Context and lowered); the fixture now also keeps the loaded module so a session
can bind it. Breakpoint lines and expected positions come from scanning the text.

`tests/execution/ceir-gpu/test_dispatch_inspect.cpp` (3 cases, `[ceir][ceir-gpu][diag]`, device-free):

- **Task session, never stops**: a breakpoint on the second dispatch's line binds exactly that op. The recording runs
  on a second thread while this thread is the controller: `wait_for_stop` returns `Finished`, both dispatches were
  recorded, `device_hits() == 1`, one refused pause, last refusal `NonPausable`, and afterwards a pause request and a
  cancel find nothing running and the session rebinds. A backstop cancels a recording a broken seam left paused.
- **During a recording**, from inside the recorder after the first dispatch: a pause request is refused
  `NonPausable`; a second recording with the same session is refused `Busy` with nothing recorded; a cancel stops the
  recording before the second dispatch (`Cancelled`, one dispatch recorded, the fault resolving to the second
  dispatch's `file:line:col`), and the next recording starts uncancelled and records both.
- **Before any work**: an unbound session (`NotBound`) and a stale generation (`StaleGeneration`) record nothing and
  mint no site; the bound generation then records normally; without a session the recording is unchanged.

`tests/execution/ceir-gpu-vulkan/test_dispatch_provenance_vulkan.cpp` gains one case (real device; soft-skips without
Vulkan): the same program uploaded, recorded under a `Task` session with the breakpoint bound, submitted and read
back. The hit is counted and refused `NonPausable` (this thread is the controller, so a seam that tried to pause
would be refused `SameThread` instead, which the check tells apart), nothing stopped, validation reports no error, the
second dispatch was labelled, and all 64 outputs equal `a + 2b`.

## Teeth (win-debug)

Each break was applied, `crd-ceir-gpu-tests` rebuilt and the three new cases run; the source was then written back
(mtime moved), rebuilt and rerun green (125 assertions in 3 cases).

| Break | Result |
|---|---|
| `device_point` decides and holds like a CPU safe point | 2 cases fail (the recording paused; the backstop ended it), 6 assertions |
| `device_point` ignores the cancel | cancel section fails, 4 assertions |
| `begin_device` skips the generation and bind check | refusal case fails, 10 assertions |
| The hit is not counted | 2 cases fail, 4 assertions |
| `request_pause` accepted during a recording | 1 assertion fails |
| The recording is never detached | 2 cases fail, 9 assertions |
| A second recording is not refused `Busy` | 1 case fails, 9 assertions |

## Evidence

- **win-debug:** the whole tree builds. `crd-ceir-gpu-tests` 140 cases (2,658 assertions),
  `crd-ceir-gpu-vulkan-tests` 59 cases (4,930) on this machine's GPU, `crd-ceir-tests` 548 (13,094) and
  `crd-ceir-host-tests` 32 (1,026) pass. The 8 ctest guards pass.
- **win-shipping, win-clang-cl-shipping (clean thin-LTO links), win-asan (inside vcvars, no ASan report):** the same
  four suites build and pass with the same counts.
- **WSL (lavapipe):** linux-gcc-debug and linux-gcc-asan build and pass `crd-ceir-gpu-tests` (140),
  `crd-ceir-gpu-vulkan-tests` (59 cases, 4,896 assertions; the new case runs on lavapipe), `crd-ceir-tests` (548) and
  `crd-ceir-host-tests` (32) with no sanitizer report. linux-clang-tsan passes the same suites, with only the `[diag]`
  cases of `crd-ceir-gpu-vulkan-tests` (2 cases; the full suite has the lavapipe teardown reports recorded in the
  DIAG.8a GPU batch), and no TSan report; it covers the recording thread and the controller thread of the new case.
- **Checks:** strict tidy is clean on the changed C++ files; clang-format `--dry-run` shows only the repository's
  hand alignment and expanded special members on new code; the Allman check, check-master-plan and check-repository
  pass.

Hosted CI must show `crd-ceir-gpu-tests` (140 cases, 2,658 assertions) green on all six lanes with its three
`diag 8b` cases passing, `crd-ceir-gpu-vulkan-tests`' `diag 8b` case passing on the lavapipe Linux lanes (it skips on
hosted Windows), and `crd-ceir-tests` (548) and `crd-ceir-host-tests` (32) still green.

## Remaining on DIAG.8b (local)

- **Consumers**: no sandbox or headless consumer executes a plan or interpreter program today, so "inspect/step an
  authored program in headless and sandbox consumers" needs a consumer that runs one with a session attached.
