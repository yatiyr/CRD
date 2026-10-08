# DIAG.9a device records through the replay commands, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a device records](2026-10-08-diag-9a-device-records.md).

## Goal

The first item the device-records batch left open: a device executor bound to the replay commands, so that
`replay.record` makes a device record from an authored program and `replay.run` replays one, in a host other than a
test. DIAG.9a stays Partial.

## What was there (checked before coding)

- `replay.run` refused every device record (`unavailable`, before loading it), and `replay.record` knew only the
  `plan` and `host` executors. `ReplayCommands` had a slot for the host executor only.
- A device run needs more than an entry and arguments: the CKIR text of every dispatched kernel, the initial
  contents of every declared buffer and a declared envelope. A command argument is at most 512 bytes and a request
  holds at most 8 arguments (`kDiagMaxArgs`), so the kernels and buffers can only come from files under the host's
  file root. The asset identity already maps the `ckir` folder to `.ckir` files.
- The only GPU executor was the tests' (`device_replay_gate.hpp`). crd-ceir-gpu is linked by the renderer
  (render-graph, frame-cook), so it must not link the cook bridge (the reason the previous batch put the record in
  crd-ceir-cook).
- **Threading at the GPU hosts.** `VulkanComputeContext` submits on `VulkanGpuContext::compute_queue()`, which is a
  dedicated compute family's queue when the device has one and the graphics queue otherwise; the crd-sandbox
  diagnostic panel sends its requests from its own thread. Running a device record from the panel would therefore
  submit to a queue the frame loop also submits to (Vulkan requires external synchronisation of a queue) on devices
  without a dedicated compute queue (lavapipe among them). `Dx12ComputeContext` creates its own queue. So crd-sandbox
  is not bound in this batch: binding it needs the run marshalled onto the frame thread, which is the next item.
- ceridc opens no GPU context, but the CPU reference executor (`reference_device_executor`, crd-kir's
  `eval_cpu_kernel`, adapter `cpu-reference`) is a device executor in its own right: an `exact` GPU record is refused
  on it before anything runs and an `ulp` record is checked against it within its declared envelope.

## What changed

- **`replay.record executor=device`** (crd-ceir-cook): new arguments `envelope` (`exact` or `ulp:N`, N at most 2^24,
  required: declared, never inferred), `kernel_dir` (kernel `@k`'s CKIR text is `<kernel_dir>/k.ckir` under the root,
  each at most 1 MiB) and `buffers` (1 to 16 comma-separated files under the root, one per declared buffer in
  declaration order, each little-endian 32-bit elements, at least one, at most 2^22 over all, each file bounded by
  what is left of that budget before a byte of it is read). `entry`, `args`, `max_events`, `seed`, `clock`,
  `sim_time`, `sim_step` and `events` are refused with `executor=device` (a device program has none of them), and the
  three device arguments are refused without it, all in the argument step. With no device executor bound the request
  is refused `unavailable` before anything is read. Otherwise the program is loaded and cooked as for the other
  executors, `describe_device_program` (new, `device_replay.hpp`) lists the kernels it dispatches (each once, in first
  dispatch order) and the buffers it declares, every kernel and buffer file is read and checked, and only then does
  `record_device_run` run the executor. The record is written exclusively. The answer has one item per input, per
  kernel (symbol, file, bytes, hash) and per buffer (element type, elements, written), and a summary with the
  adapter, the envelope, the counts, the executor's error code, the blamed dispatch's position and whether the record
  is replayable.
- **`replay.run` of a device record** goes to `replay_device_record` on the bound executor, which checks the build,
  the adapter (an exact record is claimed only on its own adapter and build, even with `build=any`) and the inputs
  before anything runs. `program=` replays it against another program and the new `kernel_dir=` against the kernel
  texts in that folder; `kernel_dir` on a plan or host record and `jobs` on a device record are refused. The answer
  names the first element outside the envelope (buffer, element, type, both bit patterns, distance, bound, the last
  dispatch writing it and the buffer's declaration at their authored positions) or the outcome divergence, the
  recorded and replayed outcomes, and a summary with both adapters, `adapter_differs`, the envelope, whether the
  program and kernels match the recorded ones, `compared` and `max_distance`. With no device executor bound it is
  refused `unavailable`, as before, now naming the missing executor.
- **`ReplayCommands::device`**: the host's `cook::DeviceExecutor` (null: device requests are refused). Unlike the host
  executor no indirection table is needed, since recording and replay live in crd-ceir-cook. The executor runs on the
  request's thread.
- **ceridc** binds the CPU reference as its device executor (its own scratch allocator; the CLI and the MCP loop run
  one request at a time) and registers the `resource` and `compute` dialects with its other host dialects.
- **crd-ceir-gpu**: new `crd/ceir/gpu/device_run.hpp`, `run_device_block`: the GPU half of a device executor, lifted
  from the tests' gate. It lowers the block, reads each dispatched kernel's CKIR text, compiles it with the host's hook
  (`KernelCompileFn`, one pipeline per dispatch with that dispatch's binding count, at most 64 dispatches), runs
  `execute_lowered_host` and reports the blamed dispatch. It names no record type, so crd-ceir-gpu still does not link
  crd-ceir-cook; a host wraps it as a `cook::DeviceExecutor` with its compute context's adapter (the gate does so in
  20 lines, and so will the sandbox).
- Two existing tests changed their expected text: the executor refusal now lists `device`
  (`test_host_replay_diag.cpp`), and the unbound refusal names the missing device executor (`test_replay_device.cpp`).

## Tests

- New `tests/execution/ceir-cook/test_replay_device_diag.cpp` (2 cases, `[device]`, device-free; the fixture gains
  `DeviceRoot`, which writes the program, its kernels and its buffers under a scratch root):
  - `replay.record executor=device envelope=ulp:4` on a counted CPU reference: the summary (backend, driver, envelope,
    2 kernels, 4 buffers, 1,024 elements, 3 written, error 0, replayable), the kernel and buffer items, and the file is
    byte for byte the library's `record_device_run` of the same artifact, inputs and envelope. A second record to the
    same file is refused before anything runs.
  - A fresh service reproduces it (768 elements compared, distance 0, same adapter and build). After the kernel and
    program files are edited it still reproduces from what it holds; against the edited kernel folder it names c[0]
    (buffer 2, element 0, f32, distance above 4, bound 4) at `@wave`'s dispatch and c's declaration in the record's
    program; with the edited program as well, at the given program's dispatch one line lower; the edited program alone
    reproduces with `program_matches` false. A program dispatching `@scale` twice reads one kernel.
  - Refusals with the executor never run: no executor bound (record, with nothing read and no file made; run, naming
    the missing executor); a missing `envelope`, `kernel_dir` or `buffers`; 7 malformed envelopes; device arguments
    without `executor=device`; 6 run arguments with it; an unsafe `kernel_dir`; 17 buffer files and an empty one;
    `executor=gpu`; 3 files for 4 buffers; a missing kernel folder; 6-byte and empty buffer files; a missing buffer
    file; a plan program (`unsupported`, naming `func.func`); `kernel_dir` on a plan record; `jobs` on a device
    record; a missing kernel folder on replay. An exact record is refused `other-adapter` on another adapter, even with
    `build=any`; an `ulp:0` record replays there and reports both backends.
- New ceridc case (`test_ceridc_diag.cpp`, the real binary): process 1 records the fixture under `ulp:4` on ceridc's
  CPU reference and its record is this process's native record byte for byte; `@wave`'s kernel file is edited;
  process 2 reproduces the record; process 3 replays it against the edited folder and names c[0] at `@wave`'s
  dispatch line; MCP stdio answers the same; every answer equals this process's native call. The root is a folder of
  the working directory, so no temp path with spaces reaches the command line.
- The GPU gate (`device_replay_gate.hpp`, the Vulkan and DX12 cases) now runs on `run_device_block` and gains a
  command leg: `replay.record executor=device envelope=ulp:32` on the device writes the library's ulp record of the
  same run byte for byte; a fresh service reproduces it on the device (distance 0); a service bound to the CPU
  reference replays it within the envelope with `adapter_differs` and the distance the library measured; after the
  kernel file is edited the device names c[0] at `@wave`'s dispatch line.

## Teeth (win-debug)

A script edited one source at a time, rebuilt the listed targets, ran the `[device]` cook cases, the ceridc device
case (with `ceridc` rebuilt) or the Vulkan `diag 9a` case, and rewrote the original. Every break made a test fail:

1. the declared envelope ignored (the record is exact);
2. buffer files read big-endian;
3. an empty buffer file accepted;
4. `kernel_dir` ignored on replay;
5. `program` ignored on a device replay;
6. run arguments allowed with `executor=device`;
7. device arguments allowed without it;
8. the envelope's bound unchecked;
9. `other-adapter` answered as `failed`;
10. the replay summary's backend taken from the record;
11. kernels not deduplicated;
12. ceridc binding no device executor;
13. ceridc without the `compute` dialect;
14. `run_device_block` compiling only the first dispatch (Vulkan);
15. its kernel lookup inverted (Vulkan).

Breaks 1 and 2 first ended in an abort: the byte comparison failed, then Catch2 asserted while printing the two
arrays. The comparisons became a `bool` check, and both breaks were rerun and failed cleanly. Break 8 was rerun after
the tidy rewrite of the envelope parser. Every touched target was then rebuilt from the restored sources on win-debug
and every suite passed again.

## Evidence (final sources)

Every lane was reconfigured (new sources) and built `crd-ceir-cook-tests`, `crd-ceir-gpu-tests`,
`crd-ceir-host-tests`, `ceridc`, `crd-ceridc-tests`, `crd-sandbox-inspect-tests`, `crd-sandbox`,
`crd-ceir-gpu-vulkan-tests` and `crd-gpu-context-vulkan-tests` (on Windows also `crd-ceir-gpu-dx12-tests`,
`crd-gpu-context-dx12-tests` and `crd-ceir-tests`); win-debug also built the whole tree.

| Suite | Cases | Lanes |
| --- | --- | --- |
| `crd-ceir-cook-tests` | 103 (6,547 assertions) | all seven |
| `crd-ceir-gpu-tests` | 140 | all seven |
| `crd-ceir-host-tests` | 47 | all seven |
| `crd-ceridc-tests` | 20 (881 assertions) | all seven |
| `crd-sandbox-inspect-tests` | 11 | all seven |
| `crd-ceir-gpu-vulkan-tests` | 60 (5,781 assertions on the RTX, 5,747 on lavapipe) | all seven |
| `crd-ceir-gpu-dx12-tests` | 47 (4,973 assertions) | the four Windows lanes |

- The lanes are win-debug, win-shipping, win-clang-cl-shipping, win-asan (run inside vcvars) and, on WSL,
  linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (one at a time, the pinned validation layers, the hosted TSan
  options). No ASan, LeakSanitizer, UBSan or TSan report on any lane; the clang-cl links worked first time. The
  Vulkan `diag 9a` case ran its command leg on lavapipe on all three WSL lanes (34 more assertions than the previous
  batch's 5,711).
- win-debug: the 8 ctest repository guards pass.
- The first full win-debug run failed one existing check in `crd-ceir-host-tests`: it expected the old executor
  refusal ("must be 'plan' or 'host'"). I updated its expected text; the suite then passed 47 of 47 on every lane.
- Strict tidy is clean on all 16 changed C++ files, after four fixes (a local constant renamed and moved to namespace
  scope, `starts_with` for a `substr` comparison, an increment taken out of a condition, a nested conditional in a test
  rewritten as `if`). clang-format shows only the repository's hand alignment, include grouping and one-line `case`
  style; I applied its line joins and the one-element-per-line argument lists by hand. The Allman check,
  check-master-plan, check-repository and test-repository-tools pass.
- The module check refused the first configure: the GPU test targets now link crd-perf and crd-platform, so
  `kir-vulkan` and `kir-dx12` declare them as test dependencies.

## Hosted CI must later show

On all six lanes: `crd-ceir-cook-tests` (103, with the 9 `[device]` cases), `crd-ceridc-tests` (20, with the real
binary's device record reproduced in another process) and `crd-ceir-host-tests` (47) green, with the other DIAG.9a
suites still green; `crd-ceir-gpu-vulkan-tests` (60) with its `diag 9a` case (now with the command leg) passing on the
lavapipe Linux lanes and skipping on the hosted Windows lanes; `crd-ceir-gpu-dx12-tests` (47) green on the Windows
lanes with its `diag 9a` case passing or skipping without a D3D12 device; `ceridc` and `crd-sandbox` building on every
lane.

## Remaining for DIAG.9a

- Can be done now:
  - a GPU device executor bound to crd-sandbox's replay commands, with the run marshalled onto the frame-loop thread
    (the panel's request thread may not submit to a queue the frame loop uses);
  - host records from crd-sandbox's panel (the host provider needs an enrolled thread).
- Waiting on something that does not exist yet: state kept across calls and a run spanning a reload (a host that
  keeps one interpreter across calls); external I/O completions, and network or physical effects stubbed only in
  explicit test replays (an op that does that kind of I/O).
- Not claimed: bit identity across hardware; a device record made in one process replayed on a GPU in another process
  (ceridc replays on the CPU reference; the GPU cases run in one test process).
