# DIAG.9a host records through the replay commands, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a run records of the host provider](2026-10-08-diag-9a-host-provider-records.md).

## Goal

The fourth DIAG.9a batch: a diagnostic command and a ceridc consumer for host-provider records, so a host record made
by one process is reproduced in another after the program file is edited. DIAG.9a stays Partial.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- `record_host_run` and `replay_host_record` (crd-ceir-host) were library calls only; `replay.run` refused a host
  record and `replay.record` ran the compiled plan only.
- crd-ceir-cook owns the replay commands, their argument parsing, bounded file reads, the program loader and the
  answer layout; it must stay free of crd-jobs, so it cannot call the host provider.
- `HostProvider` lowers parallel ranges onto `crd::jobs::parallel_for`, which takes its job array from the calling
  thread's frame arena (`frame_alloc`). The arena exists only on threads the pool enrolled (the thread that called
  `jobs::init`, and the workers); an unenrolled thread asserts there.
- ceridc started no jobs pool and registered only the arith, core and func dialects.
- crd-sandbox's diagnostic panel runs every request on its own worker thread (DIAG.8c), which the pool never enrolls.

## What changed

- **Host executor table** (`crd/ceir/cook/replay_diag.hpp`): `ReplayHostExecutor` (a `record` and a `replay`
  function), their request and answer types (`HostRecordRequest`, `HostReplayRequest`, `HostReplayAnswer`) and
  `HostExecutorStatus` (`ok`, `unavailable`, `failed`, `bad-argument`); `ReplayCommands::host` points at one (null by
  default). `cook::OwnedReplaySite` (`replay_record.hpp`) is a position that owns its file name; crd-ceir-host's
  `HostSite` is now that type.
- **crd-ceir-host** (`crd/ceir/host/host_replay_diag.hpp`, `host_replay_executor()`): the table, backed by
  `record_host_run` (which can now return the fault's position) and `replay_host_record` (which now also locates the
  recorded fault in the record's own program, `HostReplay::recorded_fault`). Refusals map to `unavailable` (plan
  record, other build, missing inputs), `failed` (not loaded, content mismatch) and `bad-argument` (schedule).
- **`replay.record`**: `executor` (`plan` default, `host`), `jobs` (1 to 256, default 8) and `sub_fuel` (1 to 2^32,
  default 2^20); `jobs` or `sub_fuel` without `executor=host` is `bad-argument` in the service's argument step. With
  no executor bound, `executor=host` is `unavailable` before the file is read. The artifact is cooked exactly as for a
  plan record; the record, encode, exclusive write and the items are shared (`write_and_answer`). Both summaries now
  name the `executor`; a host record's also names `jobs` and `sub_fuel`, and its `error` is the interpreter's.
- **`replay.run`**: after the decode, a host record with no executor bound is `unavailable` ("made by the host
  executor and this host binds no host executor"); `jobs=` on a plan record is `bad-argument`; the build and input
  checks are shared; a host record then goes to the executor (with `program=` cooked by the same loader), and its
  answer has the plan replay's items plus `recorded_jobs`, `jobs` and `sub_fuel`.
- **ceridc**: `bind_diag_commands` sets the host executor; the dialects now include task and async (the CLI's and MCP
  services' program and replay commands all use them); `main` owns a 4-thread pool (16 MiB frame arenas) for the
  `diag` and `mcp` verbs, and the MCP loop resets the arenas after each request (every job of it was joined).
  crd-ceridc declares and links crd-ceir-host, `ceridc` crd-jobs; the module declaration names both.
- **crd-sandbox** is unchanged: binding the executor there would run the host provider on the panel's unenrolled
  worker thread, so its replay commands keep refusing host records `unavailable`.
- **`assets/ceir/host_replay_demo.ceir`**: the host replay test program as authored text (a pooled `async.launch`
  calling `@square(5)`, an `async.await`, a looping `task.map_reduce`, a state cell and a loop stepped by the
  argument). `main(0)` fails `bad-for-step` at that loop; `main(1)` returns 166.

## Tests

- `tests/execution/ceir-host/test_host_replay_diag.cpp` (4 cases, `[ceir][host][diag]`; lines and columns from
  scanning the text):
  - `replay.record executor=host jobs=3` on `main(0)`: summary `executor` host, `jobs` 3, `sub_fuel` 2^20, error
    `bad-for-step` at the guard loop's scanned line and column, `replayable`, the schedule input needed and
    `recorded`. The file decodes as a host record; its blob equals the text cooked under the same relative path, and
    its encoded bytes equal `record_host_run`'s record of that blob. `replay.prepare` reads it.
  - After the launched constant is edited from 5 to 6, a fresh service reproduces the record from its own artifact
    (recorded and replayed `bad-for-step` at the scanned loop) on the recorded 8 jobs and on `jobs=16`; with
    `program=` the edited file diverges (`value`) at the await's scanned line and column, recorded 25, observed 36.
  - `sub_fuel=16` on `main(1)` fails `fuel-exhausted` inside the map body's lines; the default budget finishes; a
    fresh service reproduces the small budget's failure from the record.
  - Refusals with nothing read or run: no executor bound (record unread, replay unrun), seven argument refusals
    checked before the handler, `jobs` on a plan record, another build (and `build=any` reproducing it), a missing
    input, a content mismatch (the host executor's refusal) and recording without the Record authority.
- `tests/tools/ceridc/test_ceridc_diag.cpp` gains a jobs-pool listener and one case: one `ceridc` process records
  `main(0)` with `executor=host jobs=3`, and its record is byte-equal to the native record of the same file; the file
  is edited; a second process reproduces it; a third names the await (recorded 25, observed 36, at its scanned line);
  an MCP stdio process replays it on `jobs=16`. Each answer is byte-equal to this process's native call.

## Teeth (win-debug)

Each break was applied, the target named below rebuilt and the biting cases run (`crd-ceir-host-tests "[diag]"`, 16
cases, or the ceridc host-record case); each source was restored by rewriting it, the whole win-debug tree rebuilt and
every touched suite passed again (host 40 cases, 1,695 assertions; ceridc 13, 527).

| Break | Result |
| --- | --- |
| `replay.run` sends a host record down the plan path | 3 cases fail |
| `executor=host` parsed but not used (the plan executor records) | 4 cases fail |
| the record request ignores `jobs` (always 8) | 1 case fails |
| the record request ignores `sub_fuel` (always 2^20) | 1 case fails |
| `jobs=` accepted on a plan record | 1 case fails |
| the replay request ignores `jobs=` (the recorded split runs) | 1 case fails |
| `jobs`/`sub_fuel` accepted without `executor=host` | 1 case fails |
| the no-executor refusal moved after the program is read | 1 case fails (bytes read) |
| the recorded fault not located in the record's program | 1 case fails |
| ceridc does not bind the host executor | the ceridc case fails |
| ceridc's `diag` verb starts no jobs pool (`ceridc` rebuilt) | the recording `ceridc` process never finished; it was killed after about 10 minutes and the case failed |

A first run of the last break rebuilt only `crd-ceridc-tests`, which does not relink the `ceridc` executable, so the
stale binary passed; the break was redone with `ceridc` rebuilt.

## Evidence

Final sources, every lane built from them:

- **win-debug**: the whole tree builds. `crd-ceir-host-tests` 40 cases (1,695 assertions), `crd-ceir-cook-tests` 66
  (3,429), `crd-ceridc-tests` 13 (527, run from a build folder with `CRD_CERIDC_EXE` set), `crd-chir-tests` 30
  (5,382) and `crd-sandbox-inspect-tests` 6 (198) pass. Through ctest, the 23 `diag 9a` and real-binary replay cases
  pass with `-j 8` (each case owns its files). The 8 ctest repository guards pass.
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane reconfigured and built the five suites, `ceridc` and
  `crd-sandbox`, and passed the suites with the same counts. The clang-cl links were clean the first time; win-asan
  ran inside vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane
  built the same targets and passed the five suites with the same counts; no ASan, UBSan or TSan report.
- **Checks**: strict tidy is clean on the 7 changed C++ sources (the headers through them). clang-format shows only
  the repository's hand alignment, include order and `case` indentation; no added line is over 120 columns. The
  Allman check, check-master-plan and check-repository pass.

## Hosted CI must show

- `crd-ceir-host-tests` (40 cases, with the 8 `diag 9a` host-record cases), `crd-ceir-cook-tests` (66),
  `crd-ceridc-tests` (13, with the real binary's cross-process host replay) green on all six lanes, and
  `crd-chir-tests` (30) and `crd-sandbox-inspect-tests` (6) still green.
- `ceridc` and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- Recording a host-provider run under an inspection session (the session installs its own step hooks).
- State cells across invokes and a run spanning a reload: they need a host that keeps one interpreter across invokes.
- Random streams, clock and time-step inputs, input events and external I/O completions: a host input seam and the op
  that reads it must exist first.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle.
- Network and physical effects stubbed only in an explicit test replay.
- Host records from crd-sandbox's panel: the request would have to run on a thread the pool enrolled.
- Not claimed: a cancel while a host run runs (the command checks the caller's cancel only before it starts the
  provider); a single request whose parallel ranges exhaust the 16 MiB frame arena (a property of the host provider in
  any host).
