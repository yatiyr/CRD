# DIAG.9a host records at crd-sandbox, 2026-10-09

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a device records at crd-sandbox](2026-10-09-diag-9a-sandbox-device-records.md).

## Goal

The last item of DIAG.9a that local work could do: host records from crd-sandbox's diagnostic panel. After this
batch the row has no local work left. Its two remaining items wait on things that do not exist yet (see the end).
The row stays Partial only until a published run carries this work: the validator's Needs CI state needs that run's
link, and this batch is committed locally, not pushed.

## What was there (checked before coding)

- crd-sandbox registered `replay.record` and `replay.run` with no host executor, so `executor=host` and host records
  were refused `unavailable`. The sandbox did not link crd-ceir-host.
- The panel sends each request from its own worker thread (a `std::thread`). The host provider's parallel ranges
  (`task.map_reduce`, `task.parallel_for`) call `crd::jobs::parallel_for`, which takes its job array from the
  calling thread's frame arena (`frame_alloc`). Only the thread that called `jobs::init` and the pool's workers have
  one. On any other thread, `tl_frame_arena_ref` asserts in a debug build and reads a null arena otherwise.
- The sandbox's frame loop never calls `jobs::frame_reset`, so frame-arena allocations on its thread stay there.
- `cook::ReplayHostExecutor` was two function pointers with no user pointer, so a wrapping executor could not reach
  its own state.
- The sandbox's dialect registrar (in `main.cpp`) had arith, core, func, input, resource and compute, but no `task` or
  `async`. A host program such as `assets/ceir/host_replay_demo.ceir` (an `async.launch`, a `task.map_reduce`) would
  not load there.

## What changed

- **`cook::ReplayHostExecutor::user`** (`replay_diag.hpp`): passed to `record` and `replay` as their last argument.
  This is the executor's own state; the requests' `user` stays the registrar's. `replay_run_diag.cpp` passes
  `cmd.host->user`. The host provider's table is `{&record, &replay, nullptr}`, so ceridc, which binds
  `host_replay_executor()`, is unchanged. `test_host_inputs.cpp`'s three direct calls pass the table's `user`.
- **`FrameThreadHost`** (`sandbox/src/frame_thread_calls.{hpp,cpp}`) wraps a `ReplayHostExecutor`. Its `record` and
  `replay` hand the inner call to a `FrameThreadCalls` and wait. On the frame-loop thread the call takes
  `jobs::frame_get_mark()`, runs the inner function and restores the mark with `frame_set_mark`: the run's job arrays
  are dead once it returns, and the frame loop's arena does not grow with each run. A refused call (the calls are
  closed) is `Failed` with "the frame loop has ended: the host run did not start", and nothing is recorded.
- **crd-sandbox**: links crd-ceir-host (the module declaration gains `ceir-host`). After the device is up it builds
  `FrameThreadHost` over the same `frame_calls` as the device executor (so a host run and a device run never overlap)
  and binds it to `ReplayCommands::host` before the panel's worker exists. The pool is initialised on the frame-loop
  thread before the panel. Teardown is unchanged in order: the calls close, the panel joins its worker, the commands
  go, then the wrapper, then the jobs pool. `CloseOnExit` covers the host runs too.
- **`sandbox/src/sandbox_dialects.{hpp,cpp}`**: the registrar moved out of `main.cpp` and gained `task` and `async`.
  crd-sandbox-inspect-tests compiles it, so the panel cases load programs with the sandbox's own dialects.
- Test target: crd-sandbox-inspect-tests links crd-ceir-host and crd-jobs and compiles `sandbox_dialects.cpp`;
  scene-render's test dependencies gain `ceir-host` and `jobs`.

## Tests

`tests/applications/sandbox-inspect/test_frame_thread_host.cpp` (2 cases, `[sandbox][diag][host]`). The test thread
plays the frame loop: it initialises a 4-thread jobs pool (so it owns frame arena 0), pumps the calls and ticks a
real `DiagCommandPanel`. The replay commands use `crd::sandbox::register_sandbox_dialects`. A probe executor around
the host provider, reached through the new user pointer, records the thread each call ran on and the frame arena's
mark when the inner call began and when it returned.

- The panel's `replay.record executor=host args=0 jobs=3` of `host_replay_demo.ceir` in a temp root: `ok`,
  `bad-for-step` at the loop's scanned line. It ran once, on the pumping thread. The run moved that thread's arena
  mark (its parallel range's job array) and the mark was back at its starting value after the answer. The record
  file is the library's `record_host_run` of the same artifact, byte for byte. `replay.run` reproduces it, again on
  the pumping thread with the mark restored. `replay.run program=<edited copy>` (the launched constant 5 to 6) is
  `diverged` with a `value` divergence, recorded 25, observed 36, at the await's scanned line. Three calls in all.
- Closing: the worker posts the record and waits; nothing runs until a pump and the panel has no answer. After
  `close` it answers without a pump: `failed`, "the frame loop has ended: the host run did not start", nothing ran,
  no record file, one refusal. A later request is refused at once, and `pump` then has nothing to run.

## Teeth (win-debug)

A script made one break at a time, rebuilt `crd-sandbox-inspect-tests`, ran the `[host]` cases and restored the
source with a new modification time. Every break failed the tests:

1. The wrapper does not restore the arena mark: exit 42, the three mark checks fail (192 bytes left behind per run:
   384, 576, 768 against 192).
2. The wrapper ignores a refusal's reason: exit 42, both reason checks in the closing case fail.
3. The sandbox registrar without `task`: exit 42. The library oracle, which cooks with the same registrar, fails
   first; in the closing case the request answers before anything is posted.
4. `replay_run_diag.cpp` passes null instead of `cmd.host->user`: the wrapper reads a null `this`, and the first case
   dies with SIGSEGV.
5. The wrapper runs the inner call on the caller's thread (the panel's worker): the debug build's assertion fires on
   the worker ("tl_frame_arena_ref: frame arena not set"), the request never answers (the case's
   `frames_until_answer` fails after 20 s), and the process then waits at the interactive assertion until the script
   kills it. This is the failure the hand-over exists to prevent.

After the last restore the target was rebuilt and the `[host]` cases passed again.

## Observed on this machine (win-debug)

The real crd-sandbox, from `build/win-debug/sandbox`, with a scratch root under `build/win-debug` holding the
committed `host_replay_demo.ceir`:

- `crd-sandbox --smoke-test 4 --diag replay.record --diag-grant execute,record --diag-root <root> --diag-path
  programs/host_replay_demo.ceir --diag-param out=host.crpl --diag-param args=0 --diag-param executor=host
  --diag-param jobs=3`: `replay.record ok`, a 3,882-byte record, executor `host`, jobs 3, 14 events, `bad-for-step`
  at line 43 column 9 (the loop), `replayable`; the smoke passed (60 frames in 4 s). The device executor was bound
  as before (Vulkan on the RTX 4070 Ti SUPER).
- A second sandbox process, `--diag replay.run` of `host.crpl`: `reproduced`, 14 of 14 events verified, the same
  content hash, build `same`.
- A third, with `--diag-param program=programs/edited.ceir` (the launched constant edited): `diverged`, a `value`
  divergence at line 16 column 9 (the await), recorded 25, observed 36.
- ceridc (`diag --command replay.run`) on the sandbox's record: `reproduced`; with `jobs=16` also `reproduced`.
  ceridc's own `replay.record` of the same file with the same arguments is byte-identical to the sandbox's record.

## Evidence (final sources)

Every lane built `crd-sandbox-inspect-tests`, `crd-ceir-host-tests`, `crd-ceir-cook-tests`, `ceridc`,
`crd-ceridc-tests` and `crd-sandbox`; win-debug also built the whole tree (`replay_diag.hpp` is widely included).

| Suite | Cases | Lanes |
| --- | --- | --- |
| `crd-sandbox-inspect-tests` | 24 on Windows (793 assertions on win-debug), 23 on Linux | all seven |
| `crd-ceir-host-tests` | 47 (2,078 assertions) | all seven |
| `crd-ceir-cook-tests` | 103 (6,547 assertions) | all seven |
| `crd-ceridc-tests` | 20 (882 assertions), from a scratch folder with `CRD_CERIDC_EXE` set; win-debug through ctest (its 15 registered cases) | all seven |

- The lanes are win-debug, win-shipping, win-clang-cl-shipping, win-asan (run inside vcvars) and, on WSL,
  linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (one at a time; TSan with the preset's options). The `[host]`
  cases ran three times on each WSL lane, and ten more times under TSan (10 of 10). No ASan, LeakSanitizer, UBSan or
  TSan report on any lane; the clang-cl links worked first time.
- Strict tidy (LLVM 20.1.8) is clean on all 10 changed C++ files. clang-format, on the new files and the changed lines
  of the others, shows only the repository's hand alignment; I applied its wrap of the sandbox's binding and of three
  lines in `frame_thread_calls.cpp` by hand. The Allman check, check-master-plan and check-repository pass, and the 8
  ctest repository guards pass on win-debug.
- A defect found in passing, outside this batch: on win-debug, `ctest -R ceridc -j 4` fails 1 or 2 of the 15 ceridc
  cases (they vary between runs) with a Windows sharing violation in the child ceridc process, because the cases run
  in one working directory. Run serially, all 15 pass. This batch touches ceridc only through the executor table's new
  field. The collision is filed as a separate task, not fixed here.
- check-master-plan refuses `Needs CI` without a published run's link. This batch is committed locally and not pushed,
  so the row says that all its local clauses are shown and that it waits only for its first hosted run, as DIAG.8b
  and 8c do.

## Hosted CI must later show

On all six lanes: `crd-sandbox-inspect-tests` (24 on Windows, 23 on Linux) green, with the 2 `[host]` cases passing
everywhere (they need no device); `crd-ceir-host-tests` (47), `crd-ceir-cook-tests` (103) and `crd-ceridc-tests` (20)
still green after the executor table's change; `crd-sandbox` and `ceridc` building on every lane where they exist.
The rest of the row's hosted list is unchanged.

## Remaining for DIAG.9a

No local work. Two items wait on things that do not exist yet, and are recorded as boundaries of the row:

- State kept across calls and a run spanning a reload wait for a host that keeps one interpreter across calls (only
  tests call `migrate_state`).
- External I/O completions, and network or physical effects stubbed only in explicit test replays, wait for an op
  that does that kind of I/O (none declares `FileIO`, `NetworkIO`, `DeviceIO`, `ExternalCall`, `AgentAction` or a
  physics write). Until then a program that needs external results keeps them missing and its record is refused
  before anything runs, so nothing is silently rerun.
