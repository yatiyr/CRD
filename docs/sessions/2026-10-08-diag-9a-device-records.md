# DIAG.9a device records: numeric replay within a declared envelope, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a input events at the interactive hosts](2026-10-08-diag-9a-interactive-host-events.md).

## Goal

The row's acceptance criterion: "GPU nondeterminism uses a declared tolerance/oracle; no claim of bit identity across
all hardware." Record a run of compute dispatches with the adapter that ran it and a declared numeric envelope, and
replay it on the same adapter, on another adapter or against the CPU reference, refusing a bit-identity claim off its
own adapter and build and naming the first element outside the envelope at its authored dispatch. DIAG.9a stays
Partial.

## What was there (checked before coding)

- `replay.prepare` already listed a `device-tolerance` input (the `numeric` guarantee) needed by any `GPUCommand` op
  and always missing: "nothing declares a tolerance or oracle for backend-specific device numerics".
- No executor recorded a GPU run. `compute.dispatch` is a native op the compiled plan does not run; GPU work goes
  through crd-ceir-gpu's `lower_region` and `execute_lowered`, which record into a caller's `ComputeRecorder` and leave
  upload, submit and read back to the caller (the tests' `ceir_execute_1wg.hpp` did that by hand).
- crd-ceir-cook (the record format) links no GPU module, and crd-ceir-gpu is linked by the renderer (render-graph,
  frame-cook), so the record could not live in crd-ceir-gpu without pulling the cook bridge into the renderer.
- crd-kir is GPU-free and holds the CPU oracle every GPU backend is proven against: `ckir_read` and `eval_cpu_kernel`
  (scalar statement-tier kernels; it reads every builtin other than `LocalInvocationIndex` and `WorkgroupIndex` as 0).
- `IComputeContext` had no adapter identity. Vulkan's `VkLossAdapter` and DX12's `capture_adapter_name` are private
  to the loss records and the raster context.
- CKIR's GLSL and HLSL emitters lower `KOp::Exp` to the API's native `exp`, which the APIs specify only to a bound
  (Vulkan 3 + 2|x| ULP; D3D about 2^-21 relative). A first fixture of 8 elements in [0.375, 0.70] measured 0 ULP from
  the CPU reference on the RTX on both APIs, so the fixture was widened to 256 elements with x in [1, 4.75], where the
  same hardware differs by 3 ULP.

## What changed

- **Record format 5** (`crd/ceir/cook/replay_record.hpp`): `ReplayExecutorKind::Device` and a device section on every
  record (empty and zero for the other executors): the adapter (`DeviceAdapterRecord`), the declared envelope
  (`DeviceEnvelope`: `exact`, or `ulp` with a bound up to 2^24), the dispatched kernels (`DeviceKernelRecord`: symbol,
  CKIR text, FNV-1a 64 hash), the declared buffers (`DeviceBufferRecord`: element type, written, initial contents,
  contents after the run) and the executor's error code; `fault_op` is the blamed dispatch. Schema 4 is refused. The
  decoder bounds every count first (16 kernels, 16 buffers, 1 MiB per kernel, 2^22 elements) and refuses device
  fields in a plan or host record, an entry, trace, results or input reads in a device record, an exact envelope with
  a bound, and outputs that are not exactly the written buffers'.
- **Inputs**: on the device executor `entry-arguments` (the initial contents) and `device-tolerance` (the envelope)
  are recorded. `replay.run` refuses a device record (`unavailable`, before it loads the program): no host binds a
  device executor to the commands yet.
- **`crd/ceir/cook/device_replay.hpp`** (crd-ceir-cook now links crd-kir, declared in its module dependencies):
  - `record_device_run(request, executor, ...)`: the request is the cooked blob and its path, the kernels' CKIR texts
    and one initial content per declaration, and the envelope. The program must be a device program: one top-level
    block of `arith.const`, `resource.declare` (plain f32, i32 or u32) and `compute.dispatch` over a positive constant
    grid binding earlier declarations with one `r`, `w` or `rw` per binding; otherwise `unsupported` naming the op.
    Every dispatched symbol needs exactly one kernel and every kernel must be dispatched (`bad-request`).
  - `replay_device_record(record, executor, ..., options, out)`: refuses another executor, another build (an exact
    envelope even when another build is allowed), an exact envelope on another adapter (`other-adapter`, naming the
    differing fields), a missing input, a blob or kernel text that is not its recorded content, all before the
    executor runs. It then runs the recorded initial contents and compares the outcome first, then every written
    element in order; the first element outside the envelope is the divergence (buffer, element, both bit patterns,
    distance, bound, the buffer's declaration and its last writing dispatch at their authored positions).
    `max_distance` is the largest f32 distance over every element. `against` and `kernels_against` replay the same
    inputs against an edited program or edited kernel text, reported as `program_differs` and `kernels_differ`.
  - `f32_ulp_distance`, `device_distance` and `within_envelope` define the measure: representable values between two
    f32s (both zeros 0 apart; two NaNs 0; one NaN incomparable); under `exact` any bit difference is at least 1;
    integers are compared by value and must be equal under any envelope.
  - `reference_device_executor(scratch)`: the CPU reference executor (adapter `cpu-reference`, driver
    `kReferenceDeviceVersion`): `eval_cpu_kernel` over each dispatch in block order, f32 widened to f64 and narrowed
    after each dispatch, integers exact. It refuses a kernel it would not evaluate faithfully (not a scalar compute
    kernel with a one-dimensional workgroup and grid, a ray-tracing node or statement, a builtin other than the two
    it models) instead of skipping a statement or reading 0.
- **crd-ceir-gpu**: `execute_lowered_host(ctx, commands, device, resolver, user, buffers, sites)` runs a lowered list
  on host data end to end (device-local buffers, `validate_lowered` before anything is recorded, upload with the CKIR
  harness's barriers, `execute_lowered`, read back of the written buffers, submit and wait). A binding without data or
  a buffer the device refuses is the appended `ExecuteError::HostTransfer`.
- **gpu-context**: `IComputeContext::adapter()` (appended, default unknown) returns `ComputeAdapter` (backend, name,
  vendor, device, driver, API version). `VulkanComputeContext` answers from its physical device's properties,
  `Dx12ComputeContext` from its device's DXGI adapter (the driver is the user-mode driver version). The
  `compute_usage` constants became `inline constexpr` (the strict tidy run on the touched header flagged them unused).
- The tests' GPU executor (`tests/execution/ceir-gpu-vulkan/device_replay_gate.hpp`, shared by the Vulkan test and its
  DX12 twin) lowers the block, compiles each kernel's CKIR text for its backend, runs `execute_lowered_host` with a
  `DispatchSites` table (its fault is the blamed dispatch) and reports the context's adapter.

## Tests

The fixture (`tests/execution/ceir-cook/device_replay_fixture.hpp`) is a two-dispatch program authored as text under
`programs/diag/device_replay.ceir` and cooked: `@scale` writes b = 3a + 0.25 (exact in f32 for the inputs, so every
device agrees) and `@wave` writes c = exp(b) a and d = i, over 4 workgroups of 64 threads; a = 0.25 + i / 256. The
kernels are built with the CKIR builder and written with `ckir_write`. Positions come from scanning the text.

- `tests/execution/ceir-cook/test_replay_device.cpp` (7 cases, `[device]`, device-free on every lane):
  - Recorded on the CPU reference: executor, path, adapter, envelope, kernels and their hashes, the input states, the
    written flags, and every output against an independent evaluation (b exact, c the f32 rounding of
    `crd::math::exp` then of the product, d the index). The encoding is deterministic; the file decodes and
    re-encodes to the same bytes and replays in fresh Contexts with every element bit-identical.
  - An exact record is refused on another adapter (each of backend, name, vendor, device, driver and API alone is
    enough) and on another build even when allowed, with the executor never run; an ulp record is refused on another
    build unless allowed, and then replays; a plan record is `wrong-executor`.
  - An adapter 3 ULP above the reference holds under ulp:3 (`max_distance` 3) and fails under ulp:2 at b[0] (distance
    3, bound 2, both bit patterns), named at `@scale`'s dispatch and at b's declaration; an index one off fails under
    ulp:1000 at d[0] (integers exact), named at `@wave`; an executor that fails where the recording one did not is an
    `outcome` divergence blamed on its dispatch, and a recorded failure reproduces.
  - An edited `@wave` (product scaled by 1.25) is named at `@wave`'s dispatch with `kernels_differ`; the recorded
    texts passed as edited reproduce; an edited program (one more constant) with the edited kernel moves the named
    line down by one with `program_differs`; with the recorded kernels it reproduces.
  - Refusals: schema 4; an entry or events in a device record; outputs longer than the buffer or on an unwritten
    buffer; an exact envelope with a bound; a bound past 2^24; no kernels; no backend; device fields in a plan record;
    a fourth executor; a tampered kernel text and content hash (`content-mismatch`); a truncated blob (`not-loaded`);
    a missing `device-tolerance` (`missing-inputs`); 3 buffers for 4 declarations, an empty buffer, a missing kernel,
    a spare kernel and an exact envelope with a bound (`bad-request`); an f64 buffer, a zero grid dimension and no
    dispatch (`unsupported`); a kernel indexed by `GlobalInvocationId` refused by the reference (`device-failed`). No
    refused request made a record.
  - The distance measure at its edges (zeros, subnormals across zero, infinity, NaN payloads, integers).
  - `replay.run` answers a device record `unavailable` naming the device executor; `replay.prepare` reads the same
    record and answers its `device-tolerance` as in the run record, with nothing missing (added after the first lane run,
    rebuilt and run on all seven lanes: 101 cases, 6,173 assertions).
- `tests/execution/ceir-gpu-vulkan/test_device_replay_vulkan.cpp` and `tests/execution/ceir-gpu-dx12/test_device_replay_dx12.cpp`
  (1 case each, soft-skip without a device), through `device_replay_gate`: the context's adapter is known and names
  its backend; recorded on the device under `exact`, b and d are exact and c is within 32 ULP of the independent
  evaluation; through a file and fresh Contexts the record replays on the same adapter with every element
  bit-identical; it is refused on the CPU reference before the reference runs; recorded under ulp:32 it replays on
  the CPU reference within the bound; when the measured distance m is above 0, recorded under ulp:(m - 1) it diverges
  in c at distance m, named at `@wave`'s dispatch (when m is 0 the test says so instead); and against the edited
  `@wave` on the device it is named at `@wave`'s dispatch.

## Teeth (win-debug)

I broke 18 checks one at a time (a script edited one source, rebuilt the listed targets, ran the `[device]` cases or
the `diag 9a` device case, and rewrote the original); every one made a test fail, and every target was rebuilt from the
restored sources afterwards, with all suites passing again:

1. an exact record accepted on another adapter (cook and Vulkan);
2. an exact record accepted on another build when another build is allowed;
3. the ulp bound loosened by one (cook and Vulkan: the one-ULP-less leg);
4. integers measured in ULPs (the first try left the parameter unused and did not compile under `/WX`; redone);
5. negative floats not mirrored onto the ordered line;
6. written buffers not marked (assertions failed, then an empty output was indexed and the run aborted);
7. the replay ignoring edited kernels (cook and Vulkan);
8. the kernel text hash not checked;
9. the decoder accepting device fields in another executor's record;
10. `execute_lowered_host` skipping the read back (Vulkan);
11. `execute_lowered_host` skipping the upload (Vulkan);
12. `replay.run` not refusing device records;
13. `device-tolerance` not recorded;
14. the outcome not compared;
15. the Vulkan adapter naming no backend;
16. the DX12 adapter naming no adapter;
17. the record keeping no outputs (assertions failed, then the run aborted on an empty output);
18. the CPU reference accepting vector nodes.

## Evidence (final sources)

All seven lanes built `crd-ceir-cook-tests`, `crd-ceir-gpu-tests`, `crd-ceir-gpu-vulkan-tests`,
`crd-ceir-host-tests`, `ceridc`, `crd-ceridc-tests`, `crd-sandbox-inspect-tests`, `crd-sandbox` and
`crd-gpu-context-vulkan-tests` (and on Windows `crd-ceir-gpu-dx12-tests` and `crd-gpu-context-dx12-tests`); win-debug
also built the whole tree, since `crd/gpu/compute.hpp` is widely included. Each lane was reconfigured first.

| Suite | Cases | Lanes |
| --- | --- | --- |
| `crd-ceir-cook-tests` | 101 (6,173 assertions) | all seven |
| `crd-ceir-gpu-tests` | 140 | all seven |
| `crd-ceir-host-tests` | 47 | all seven |
| `crd-ceridc-tests` | 19 | all seven |
| `crd-sandbox-inspect-tests` | 11 | all seven |
| `crd-ceir-gpu-vulkan-tests` | 60 (5,745 assertions on the RTX, 5,711 on lavapipe) | all seven |
| `crd-ceir-gpu-dx12-tests` | 47 | the four Windows lanes |

- The `diag 9a` device cases measured the largest distance from the CPU reference: 3 ULP on the RTX 4070 Ti SUPER
  through Vulkan and through DX12 on every Windows lane, 4 ULP on lavapipe (llvmpipe, LLVM 20.1.2) on all three WSL
  lanes. Each was within the declared 32 and diverged one ULP below its own figure. WARP (the hosted Windows DX12
  device) was not measured here.
- No ASan, LeakSanitizer, UBSan or TSan report on any lane; on WSL the Vulkan suites ran on lavapipe with the pinned
  validation layers. The clang-cl links worked first time.
- The first win-asan run of `crd-ceir-gpu-vulkan-tests` failed one case, `ceir 28b-2b` (the committed tune cache's
  fusion choice for this device, a timing measurement): fresh fuse 0 against the committed 1 under ASan. It passed
  three reruns alone and the whole suite passed when rerun; the case does not touch this batch's code.
- The first gcc-debug build on WSL failed: the fixture's constructor parameter `a` shadowed a member (`-Wshadow`,
  which MSVC does not report). I renamed it, rebuilt `crd-ceir-cook-tests` and both GPU test targets on all four
  Windows lanes and reran them (same counts), then ran the three WSL lanes from the start.
- Strict tidy is clean on all 19 changed C++ files (after renaming one local constant, splitting three concatenated
  test programs into named constants, making the `compute_usage` constants `inline constexpr`, and moving the shared
  GPU gate beside the Vulkan test so its includes resolve). clang-format shows only the repository's hand alignment
  and `case` style; I applied its line wraps by hand and kept every line within 120 columns. The Allman check, the 8
  ctest repository guards (win-debug), check-master-plan and check-repository pass.

## Hosted CI must later show

On all six lanes: `crd-ceir-cook-tests` (101, with the 7 `[device]` cases), `crd-ceir-gpu-tests` (140),
`crd-ceir-host-tests` (47), `crd-ceridc-tests` (19) and `crd-sandbox-inspect-tests` (11) green, with the other DIAG.9a
suites the row lists still green; `crd-ceir-gpu-vulkan-tests` (60) with the `diag 9a` device case passing on the
lavapipe Linux lanes (it skips on the hosted Windows lanes, which have no Vulkan device); `crd-ceir-gpu-dx12-tests`
(47) green on the Windows lanes, its `diag 9a` case passing on whatever D3D12 device the runner has or skipping without
one; `ceridc`, `crd-sandbox`, `crd-gpu-context-vulkan-tests` and `crd-gpu-context-dx12-tests` building wherever they
exist.

## Remaining for DIAG.9a

- Can be done now:
  - a device executor bound to the replay commands (`replay.record` and `replay.run` for device records) in a host
    with a GPU context, crd-sandbox first (ceridc has no GPU context);
  - host records from crd-sandbox's panel (the host provider needs an enrolled thread).
- Waiting on something that does not exist yet:
  - state kept across calls and a run that spans a reload need a host that keeps one interpreter across calls;
  - external I/O completions, and network or physical effects stubbed only in explicit test replays, need an op that
    does that kind of I/O.
- Not claimed: bit identity across hardware (by design); numeric replay of render, ray-tracing or `ceir.work`
  dispatches (only direct compute dispatches over constant grids are device programs); a device run recorded while a
  debugger session holds it (`DeviceInspect` is not threaded through `execute_lowered_host`).
