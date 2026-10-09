# DIAG.9a device records at crd-sandbox, 2026-10-09

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a device record commands](2026-10-08-diag-9a-device-record-commands.md).

## Goal

The first item the device record commands batch left open: a GPU device executor bound to crd-sandbox's replay
commands, with each run handed to the frame-loop thread. DIAG.9a stays Partial: host records from the sandbox's panel
remain.

## What was there (checked before coding)

- crd-sandbox registered `replay.record` and `replay.run` with no device executor, so device requests were refused
  `unavailable`. Its dialect registrar had no `resource` or `compute` dialect, so a device program would not load.
- The diagnostic panel (`DiagCommandPanel`) sends each request from its own worker thread, and `DeviceRunFn` takes no
  cancel flag. `VulkanComputeContext` submits on the device's compute queue, which is the graphics queue the frame loop
  submits to when the device has no dedicated compute family. `Dx12ComputeContext(alloc)` opens its own D3D12 device
  and compute queue on the default adapter; it cannot share the sandbox's `Dx12GpuContext`.
- The only GPU device executor was the tests' (`device_replay_gate.hpp`). Each GPU gate test had its own copy of the
  CKIR-kernel-to-pipeline hook (emit GLSL or HLSL, compile, `create_pipeline_from_spirv` or `_hlsl`). No engine API
  did that for a compute context; both backends already link crd-kir.
- The sandbox's teardown destroys the panel (which joins its worker) before the device. Early returns in `main` do the
  same through destructor order.

## What changed

- **`create_pipeline_from_ckir`** on `VulkanComputeContext` and `Dx12ComputeContext` (concrete contexts only; the
  backend-agnostic interface is unchanged): crd-kir's compute-kernel emitter, then SPIR-V or HLSL, then the existing
  pipeline factory, with the given binding count and push size; null when the kernel does not emit or compile. The two
  gate tests now call it instead of their local copies.
- **`sandbox/src/gpu_device_executor.{hpp,cpp}`**: `GpuDeviceExecutor::vulkan(VulkanGpuContext&, alloc)` and, on
  Windows, `::dx12(alloc)` build a compute context, a `DeviceRunRig` over `run_device_block` and the context's adapter
  as a `cook::DeviceExecutor`. Null when the context is invalid or reports no adapter. More buffers than a run binds
  (16) is a run that could not start, never an out-of-range write.
- **`sandbox/src/frame_thread_calls.{hpp,cpp}`**: `FrameThreadCalls` holds one posted call. `call` (a request thread)
  waits for the slot, posts, and waits until the call is done; `pump` (the frame-loop thread) runs a posted call
  outside the lock; `close` refuses the posted call that has not started and every later one, and wakes the waiters.
  `CloseOnExit` closes on scope exit. `FrameThreadDevice` wraps a `DeviceExecutor`: same adapter, and its run hands the
  inner run to `call`; a refused call is a run that did not start ("the frame loop has ended: the device run did not
  start"), which the commands answer `failed` with nothing recorded.
- **crd-sandbox**: registers the `resource` and `compute` dialects; after the device is up it builds the executor on
  its backend (Vulkan over its own device, DX12 on its own device), wraps it in `FrameThreadDevice` and binds it to
  `ReplayCommands::device` before the panel's worker exists, logging the adapter (or that device records are refused).
  The frame loop calls `frame_calls.pump()` once per frame before the panel's tick. Teardown closes the calls before
  the panel is destroyed, and a `CloseOnExit` declared after the panel covers every other return path; the executor
  dies after the commands that point at it and before the device. Links crd-ceir-gpu; the module declarations gained
  `ceir-gpu` (sandbox) and `ceir-gpu`, `perf`, `perf-ui` (scene-render's test dependencies, for the inspect tests).

## Tests

`tests/applications/sandbox-inspect/test_frame_thread_calls.cpp` (in `crd-sandbox-inspect-tests`, which now compiles
the two new sandbox sources; tags `[sandbox][diag][device]`). The test thread plays the frame loop: it pumps the calls
and ticks a real `DiagCommandPanel` over a service with the replay commands bound through `FrameThreadDevice`.

- The panel's `replay.record executor=device envelope=ulp:32` of the device fixture on a CPU reference executor that
  records the thread it ran on: the run is on the pumping thread, once, and the record file is the library's
  `record_device_run` of the same inputs byte for byte. `replay.run` reproduces it (768 compared, distance 0), again on
  the pumping thread. Against an edited `@wave` it names element 0 of buffer 2 at `@wave`'s dispatch, at the line
  scanned from the text. The wrapped executor reports the inner adapter.
- Closing: the worker posts the run; while the frame loop does not pump, nothing runs and the panel has no answer.
  After `close`, the panel answers without a pump: `failed`, "the frame loop has ended", nothing ran, no record file,
  one refusal. A later request is refused at once without the frame loop; `pump` then has nothing to run.
- Four threads make 50 calls each while the test thread pumps: 200 calls answered, each on the pumping thread, never
  two at once. After `close`, a call returns false at once and runs nothing.
- On a device (soft-skips without one): `GpuDeviceExecutor::vulkan` over a headless Vulkan context and, on Windows,
  `GpuDeviceExecutor::dx12`. The panel's record is byte-equal to the library's record on the same executor, replays
  on the CPU reference within the declared 32 ULP, and `replay.run` reproduces it on the device (distance 0, same
  adapter); three runs on the device in all.

## Teeth (win-debug)

A script made one break at a time, rebuilt `crd-sandbox-inspect-tests`, ran the `[device]` cases and restored the
source with a new modification time. Every break failed the tests (all five exits 42):

1. `pump` never runs a posted call: the record request never answers, the concurrent case answers none of its 200
   calls, and the GPU cases fail;
2. `FrameThreadDevice` runs the inner executor on the caller's thread: the thread and call-count checks fail in the
   first case and the GPU cases, and in the closing case the panel answers before the frame loop pumps, with no call
   ever posted;
3. `close` leaves the posted call waiting: the closing case gets no answer (its fallback pump then lets the panel be
   destroyed), and the refusal, call and record-file checks fail;
4. the wrapper drops the adapter: the adapter checks fail, and the record command refuses an executor with no adapter
   before a run is posted (the first, closing and GPU cases fail);
5. the GPU executor reads nothing back: the panel's record no longer replays on the CPU reference within its envelope
   (Vulkan and DX12).

After the last restore the target was rebuilt and the `[device]` cases passed again (186 assertions, 5 cases).

## Observed on this machine (crd-sandbox, win-debug, RTX 4070 Ti SUPER)

The fixture's files (program, two kernel texts, four buffers) were exported once by a temporary hidden test case,
which was then removed before the target was rebuilt.

- `crd-sandbox --smoke-test 4 --diag replay.record --diag-grant execute,record --diag-root <fixture> --diag-path
  programs/diag/device_replay.ceir` with `out`, `executor=device`, `envelope=ulp:32`, `kernel_dir=kernels` and the
  four buffer files: "device records: vulkan on NVIDIA GeForce RTX 4070 Ti SUPER", `replay.record ok`, a 12,091-byte
  record with backend `vulkan`, 2 kernels, 4 buffers, 3 written, error 0; the smoke passed (136 frames in 4 s).
- A second sandbox process, `--diag replay.run` of that record: reproduced, same adapter, 768 compared, distance 0.
- `--backend dx12`: "device records: dx12 on NVIDIA GeForce RTX 4070 Ti SUPER", a 12,089-byte record (driver
  9007199255733611, api 0), reproduced by another DX12 sandbox process at distance 0.
- The Vulkan record replayed by a DX12 sandbox: `adapter_differs` true, reproduced, distance 0.
- The Vulkan record repeated on the final binary with the loader's layer log on: `VK_LAYER_KHRONOS_validation` (SDK
  1.4.341.1) was loaded, and with its log action forced to stdout for errors and warnings it reported nothing (no
  VUID, no threading report) while the device run shared the frame loop's device and queue.
- ceridc (`diag --command replay.run`, CPU reference) on both records: reproduced within 32 ULP, `max_distance` 3, the
  same distance the gates measured.

## Evidence (final sources)

Every lane built `crd-perf-ui-tests`, `crd-sandbox-inspect-tests`, `crd-sandbox` and `crd-ceir-gpu-vulkan-tests`
(Windows also `crd-ceir-gpu-dx12-tests`, `crd-gpu-context-vulkan-tests` and `crd-gpu-context-dx12-tests`); win-debug
built the whole tree, twice (the second after the perf-ui fix below).

| Suite | Cases | Lanes |
| --- | --- | --- |
| `crd-sandbox-inspect-tests` | 22 on Windows (697 assertions on win-debug), 21 on Linux | all seven |
| `crd-perf-ui-tests` | 21 with profiling, 14 without (the shipping lanes) | all seven |
| `crd-ceir-gpu-vulkan-tests` | 60 (5,781 assertions on the RTX) on Windows; its `diag 9a` case (851 assertions) on lavapipe | all seven |
| `crd-ceir-gpu-dx12-tests` | 47 (4,973 assertions) | the four Windows lanes |
| `crd-gpu-context-vulkan-tests`, `crd-gpu-context-dx12-tests` | 302 and 203 | win-debug |

- The lanes are win-debug, win-shipping, win-clang-cl-shipping, win-asan (run inside vcvars) and, on WSL,
  linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (one at a time; TSan with the preset's options). The `[device]`
  cases ran three times on each WSL lane, and the Vulkan one ran on lavapipe (llvmpipe, LLVM 20.1.2) there; the
  concurrent-callers case passed 20 of 20 more runs under TSan. No ASan, LeakSanitizer, UBSan or TSan report remains on
  any lane; the clang-cl links worked first time.
- **A defect the new test found (fixed).** On linux-gcc-asan, UBSan reported `diag_panel.cpp`: `DiagCommandPanel::
  set_args` (and `set_path`) passed an empty view's null data to `memcpy`, which the test reached by sending
  `replay.run` with no arguments. Both now copy only when the view is non-empty. `test_diag_panel.cpp` gains the
  check (an empty default view clears both fields). Tooth on linux-gcc-asan: the guard removed, the perf-ui suite
  fails with the UBSan report; restored and rebuilt, 21 of 21 pass, and the sandbox-inspect suite passes there again.
- win-debug: the 8 ctest repository guards pass. The final sandbox binary records again on Vulkan (smoke passed).
- Strict tidy is clean on all 14 changed C++ files after one fix (two local constants moved to namespace scope).
  clang-format, on the new files and on the changed lines of the others, shows only the repository's hand alignment and
  include grouping; I applied its lambda layout, two continuation indents, two comment wraps and one line join by hand.
  The Allman check and check-master-plan pass.
- The module check refused the first configure: crd-sandbox now links crd-ceir-gpu, and the inspect tests link
  crd-perf and crd-perf-ui, so `sandbox` declares `ceir-gpu` and scene-render's test dependencies gain `ceir-gpu`,
  `perf` and `perf-ui`.

## Hosted CI must later show

On all six lanes: `crd-sandbox-inspect-tests` (22 on Windows, 21 on Linux) green, with the `[device]` cases' Vulkan
case passing on the lavapipe Linux lanes and skipping on the hosted Windows lanes (no Vulkan device) and the DX12 case
passing or skipping without a D3D12 device; `crd-perf-ui-tests` (21 with profiling, 14 without) green, UBSan clean on
linux-gcc-asan; `crd-ceir-gpu-vulkan-tests` (60) and `crd-ceir-gpu-dx12-tests` (47) still green with their `diag 9a`
cases as before; `crd-sandbox`, `crd-gpu-context-vulkan-tests` and `crd-gpu-context-dx12-tests` building on every lane
where they exist.

## Remaining for DIAG.9a

- Can be done now: host records from crd-sandbox's panel. The host provider takes its parallel ranges' job arrays from
  the calling thread's frame arena, so a host run must be handed to an enrolled thread too (the frame loop's, with the
  arena mark restored after each run), and `cook::ReplayHostExecutor` has no user pointer, so a wrapping executor needs
  one added to the table.
- Waiting on something that does not exist yet: state kept across calls and a run spanning a reload (a host that
  keeps one interpreter across calls); external I/O completions, and network or physical effects stubbed only in
  explicit test replays (an op that does that kind of I/O).
- Not claimed: bit identity across hardware. The Vulkan and DX12 records of one GPU agreeing bit for bit here is a
  measurement on one adapter, not a guarantee.
