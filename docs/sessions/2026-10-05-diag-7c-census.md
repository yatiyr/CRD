# DIAG.7c census: Vulkan synchronization, GPU-assisted validation and device faults

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7c](../ROADMAP.md#slice-diag.7c); contract:
> [DIAG.7c](../design/runtime-diagnostics.md#diag-7c); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md)
> DG12. Rules: [AGENTS](../../AGENTS.md). Preceding: [DIAG.7b census](2026-10-04-diag-7b-census.md).

## Position

DIAG.7c requires DIAG.7b, which has landed (b)-(g). Its only remaining unit, (h), is the hardware-gated real device
fault. The census changes no row status and starts no device-loss reproduction. It ran while hosted CI qualified the
repairs, per the user's direction to carry on serially.

## Acceptance clause (verbatim)

> Qualify validation layers, synchronization checks, optional GPU-assisted checks and supported device-fault extension
> collection. Pin SDK/layer versions; account for shader instrumentation resource limits. Preserve queue-family,
> access, layout, descriptor and program identity. Query capabilities before enabling optional modes and retain
> original VK_ERROR_DEVICE_LOST information when further calls fail.
>
> Acceptance: controlled missing-barrier/stale-resource cases, asynchronous reports after fence completion, layer
> absent, descriptor-limit fallback and device-loss capture on declared Windows/Linux tuples. External RenderDoc/vendor
> tools may supplement evidence, never replace the public reporting path or constitute unsupported platform
> qualification.

## Workstation tuple (measured 2026-10-05)

| Item | Value |
|---|---|
| Adapter | NVIDIA GeForce RTX 4070 Ti SUPER, driver 595.79, device API 1.4.329 |
| Loader / SDK | Vulkan instance 1.4.341; SDK `C:\VulkanSDK\1.4.341.1` (the pinned SDK) |
| Validation layer | `VK_LAYER_KHRONOS_validation` 1.4.341, implementation version 1 |
| Layer extensions | `VK_EXT_validation_features` rev 2, `VK_EXT_layer_settings` rev 2 |
| Device fault | `VK_EXT_device_fault` rev 2: `deviceFault` and `deviceFaultVendorBinary` true |
| Descriptor sets | `maxBoundDescriptorSets` 32 (GPU-assisted validation reserves one for its instrumentation) |

## What exists

- **Mode requests and activation (DIAG.7a f).** `init()` loads `VK_LAYER_KHRONOS_validation` when any mode is
  requested. It enables `VK_EXT_debug_utils`, and also `VK_EXT_validation_features`, which it enumerates from the layer
  itself. It chains the synchronization and GPU-assisted enables onto `VkInstanceCreateInfo` and reports a common
  `ValidationActivation`:
  - Core is active when the layer loaded.
  - Sync is active when the features extension is present.
  - GPU-assisted is active only when the device has the stores/atomics features its instrumentation needs, which are
    then enabled on the device.
- **Capture and correlation (DIAG.7a).** `ValidationCapture` rides a debug-utils messenger with bounded records.
  Cerid identities are resolved from named objects, then from message prose. Pass labels correlate through
  `PassLabelScope`.
- **Already-qualified controlled cases (DIAG.7a g-tests):**
  - a core hazard on a named buffer;
  - a synchronization write-after-write on a named buffer with no barrier; the barrier and sync-off controls stay clean;
  - a GPU-assisted shader out-of-bounds access read back at the fence wait, correlated to a Cerid identity;
  - pass-scope correlation.
- **Device-fault analogue on DX12 (DIAG.7b).** The DX12 helpers observe removal on every completion path, bound every
  wait, force removal on failed completion, record origin and DRED, and write a versioned bundle into a dump.

## What is missing

- **Layer absent.** Requesting validation without an installed layer fails `vkCreateInstance`
  (`VK_ERROR_LAYER_NOT_PRESENT`), so the whole context is invalid. It should be a working context that reports
  `LayerAbsent` per requested mode. Capabilities are not queried before the layer is requested.
- **No version pinning in evidence.** The layer's spec and implementation versions and the loader version are never
  recorded with a report.
- **Instrumentation limits not accounted for.** GPU-assisted validation needs a free descriptor-set slot. A pipeline
  layout using every slot is silently uninstrumented. Nothing reports or tests this fallback.
- **No device-loss handling.** Every completion path ignores the result of `vkQueueSubmit` and `vkWaitForFences`, and
  waits with `UINT64_MAX`. There is:
  - no bounded wait;
  - no `VK_ERROR_DEVICE_LOST` observation;
  - no retention of the original error when later calls fail;
  - no execution-failure count.
- **No device-fault collection.** `VK_EXT_device_fault` is never enabled or read (`vkGetDeviceFaultInfoEXT`: fault
  description, address infos, vendor infos, vendor binary), and there is no removal record or bundle like DX12's.
- **No stale-resource case.** No test covers use of a destroyed or retired resource, or a mismatched image layout.

## Design points for review

1. **One completion seam, as on DX12.** Add bounded `vk_submit` and `vk_wait` helpers in the provider, mirroring
   `dx12_submit` and `dx12_wait`. They report through the capture, latch device loss, and keep the first lost result.
   The existing paths are moved onto them one by one, compute first; a wholesale rewrite is out of scope.
2. **Simulated versus real loss.** Vulkan has no `RemoveDevice`. The simulated error path is an injected
   `VK_ERROR_DEVICE_LOST` at the helper seam, labelled simulated. A real loss stays the hardware-gated reproduction it
   is on DX12. `vkGetDeviceFaultInfoEXT` is queried only after a loss; on a live device the record says "not lost".
3. **Layer absence is a capability, not a failure.** Enumerate instance layers before requesting one. A missing layer
   reports `LayerAbsent` for every requested mode, and the context still comes up.

## Sub-unit plan (order re-confirmed per step)

| Unit | Work | Gate |
|---|---|---|
| (b) | Query the layer before enabling it. Report an absent layer as `LayerAbsent` and keep the context valid. Record the layer's spec and implementation versions with the activation. | Loader-disabled layer (`VK_LOADER_LAYERS_DISABLE`) on the workstation, plus a normal run |
| (c) | Bounded `vk_submit` and `vk_wait` with device-loss observation and first-error retention, counted as execution failures. Move the compute context onto them. | Injected loss at the seam (simulated) and a timed-out wait |
| (d) | `VK_EXT_device_fault`: enable when advertised. On a recorded loss, read the fault description, address infos and vendor infos into a fixed report. Keep last-known state when a query fails. | Capability report on hardware; record path on injected loss |
| (e) | Descriptor-limit fallback: a pipeline using every descriptor-set slot under GPU-assisted validation is reported uninstrumented; one using fewer is instrumented. | GPU-assisted on hardware |
| (f) | Stale-resource cases: a destroyed or retired resource and a wrong image layout, correlated to identities. Asynchronous reports arriving only after fence completion are asserted explicitly. | Core and sync validation on hardware |
| (g) | Loss bundle in the DIAG.5 evidence container, like DX12 (f) | Bundle round-trip |
| (h) | Real device-loss reproduction: contained, hardware-gated, never on the desktop GPU outside a qualified harness | Stays visible as a hardware gate |

The hosted lanes install the pinned SDK but expose no Vulkan device. Their Vulkan device tests skip; the
`linux-gcc-asan` log shows the DIAG.7a(f) activation case as Skipped. The workstation is therefore the only device
tuple. The Linux device tuple stays a visible gate, and no hardware claim is made for it.

## (b) landed (2026-10-05): the layer is queried before it is requested

- `query_validation_layer` enumerates the instance layers before `VK_LAYER_KHRONOS_validation` is requested. The
  context keeps the result as the public `VulkanValidationLayer` (queried, present, spec version, implementation
  version), exposed through `VulkanGpuContext::validation_layer()`. That accessor is appended at the end of the class
  with a default, so existing mocks still compile.
- An absent layer is no longer requested, and its features extension is not chained, so `vkCreateInstance`
  succeeds. `initial_activation` reports `LayerAbsent` for every requested mode, and the context is valid.
- Extracting the two helpers keeps `init()` under the strict gate's function-size threshold.

`[gpu-context][vulkan][validation]` (b), on the workstation:
- The present layer is recorded as spec 1.4.341, implementation 1, with Core active.
- The layer is then made genuinely absent through the loader's own filter (`VK_LOADER_LAYERS_DISABLE`), and the same
  three-mode request yields a valid context. The layer reads as not present, and Core, Sync and GPU-assisted are each
  requested, inactive and `LayerAbsent`.
- A context that requests nothing does not enumerate layers at all.
- Teeth: with the presence check removed (the layer requested regardless, as before), the factory returns null and the
  case fails at `REQUIRE(absent != nullptr)`. Restored and rebuilt, it passes.

Results: the case passes with 21 assertions; `[validation]` 7 cases; the whole Vulkan test executable 293 cases,
6,931 assertions. The strict gate and the brace guard are clean on the three files.

## (c) landed (2026-10-05): a bounded, observed Vulkan completion seam

`vulkan_execution.hpp/.cpp` (provider-private) add `vk_submit` and `vk_wait`, the counterparts of `dx12_submit` and
`dx12_wait`:
- **Bounded wait:** waits are bounded, defaulting to the DX12 30 s.
- **Classification:** each failure is classified as `Observed` (`VK_ERROR_DEVICE_LOST`), `TimedOut`, `Failed` or
  `Simulated`.
- **First failure kept:** the first failure per device is kept, with its result, its operation label and a
  process-wide sequence. Later failures only increment the count, which retains the original `VK_ERROR_DEVICE_LOST`
  information when further calls fail.
- **Lost device:** a device known to be lost is not called again; its submissions and waits return
  `VK_ERROR_DEVICE_LOST` at once.
- **Fixed table:** a fixed table holds the per-device records, with no allocation on the failure path.
- **Last-known failure:** `vk_last_device_failure` keeps the newest first failure after its device is destroyed. The
  context destructor calls `vk_forget_device` before `vkDestroyDevice`, so a later device that reuses the handle value
  starts clean.
- **Test seam:** `vk_inject_next_result` makes the next call return a chosen result without the driver. Vulkan has no
  `RemoveDevice`, so this is the simulated error path, recorded as `Simulated`.

`VulkanComputeContext::submit_and_wait` now uses the seam. A failed submission or wait latches the context invalid, and
nothing of that submission is read back. Moving the raster, ray-tracing and DGC completion paths is later work in this
row; they still use unbounded waits.

`[gpu-context][vulkan][device-loss]` (c), on the workstation:
- A healthy compute round trip records nothing.
- A 1 ms wait on a fence nobody signals returns `VK_TIMEOUT` and records `TimedOut`, not lost. This is contained: no GPU
  work is pending.
- On a second device, an injected loss at the compute submit latches the context invalid and records `Simulated` loss on
  `compute submit`.
- A later submit returns `VK_ERROR_DEVICE_LOST` without the driver. The first failure keeps its origin, operation and
  sequence, and the count becomes 2.
- The first device's timeout record is untouched.
- After the lost device is destroyed, the last-known failure is the simulated loss, and the destroyed handle reads clean.
- Three consecutive passes, 27 assertions.
- Teeth: letting a later failure overwrite the first fails the origin, operation and sequence checks. Restored and
  rebuilt, it passes.

The strict gate and the brace guard are clean on the five changed files.

Device consumers of the compute path (hosted lanes have no Vulkan device, so this is their only device run):
- `crd-gpu-context-vulkan-tests`: 294 cases, 6,958 assertions, pass.
- `crd-kir-vulkan-tests`: 31 cases, 33,023 assertions, pass.
- `crd-ceir-gpu-vulkan-tests`: 57 cases. It passed in 5 of 7 runs.
  - Both failures were `ceir 28b-2b`, the anti-drift check of the committed autotune row. The committed row has
    `fuse=1`; the fresh measurement chose `fuse=0`, with `share` agreeing.
  - That case runs in about 0.5 s and ranks the plans by GPU timestamps (`last_gpu_ms`), which the seam's host-side
    bookkeeping does not enter. So the 30 s bound and the new submit path cannot decide it.
  - **Diagnosed and fixed (2026-10-05, at the user's request, in this session).** Raw per-sample timings showed the
    cause was measurement design, not noise or the seam.
    - In isolation, fused beat unfused by 9-19% in 25 of 25 runs, with no overlap between the two distributions.
    - Inside the full suite, the same plans measured 17-25 us instead of 8-9 us, and one block caught a GPU clock
      ramp (config 0 at 39.3 us, then 1-3 at ~9 us), leaving the fuse/no-fuse ratio at 1.03.
    - The measurer timed configurations in blocks, so clock drift (DVFS) was compared instead of plans.
  - **The fix.** The measurer, in both the Vulkan and DX12 mirrors, now:
    - runs interleaved rounds, each configuration once per round with a rotating start;
    - judges each configuration by the median of its per-round ratio to the default (the clock cancels);
    - keeps the default unless a challenger is at least 5% faster, paired.
  - **Evidence.**
    - The paired ratios read [1, 1, 1.11, 1.11].
    - Vulkan `[tune]` 15 of 15; the full Vulkan suite 7 of 7 (57 cases); DX12 `[tune]` 10 of 10 and the full DX12
      suite (46 cases).
    - Teeth: the committed RTX row flipped to `fuse = false` still fails the anti-drift. Restored, it passes.
    - The lesson is `feedback_autotuner_interleave_and_pair_under_dvfs` in the memory corpus.

**Tooling note:** a `cmake --build <dir> --target rebuild_cache` run outside the MSVC environment emptied
`CMAKE_MAKE_PROGRAM` in `build/win-debug`. It was repaired with `scripts/configure-preset.bat win-debug
-UCMAKE_MAKE_PROGRAM`. Reconfigure only through the preset script.

## The first lavapipe run (2026-10-05): what a second Vulkan device found

Runs `4bd6a5de` and `f7689b5b` are the first with lavapipe on the hosted Linux lanes (Mesa 25.2.8, the same as the
WSL reference host). Every Windows lane passed; every Linux lane failed. Triage by cause:

1. **A regression in this row's (c) seam (fixed).** The radix sort (34.1 s), the B19-a3 depth sort (36.6 s) and B19-a4
   tile binning (34.7 s) segfaulted just past the 30 s wait bound. On the ASan lane the sort passed at 27.7 s, while
   B15-a ran 47.7 s and failed its oracle.
   - **Cause:** `submit_and_wait` abandoned the wait at the bound. The test then read results and freed buffers while
     lavapipe was still executing into them. DX12 forces `RemoveDevice` on a timeout, so its memory is safe; Vulkan has
     no way to stop queued work.
   - **Fix:** `vk_wait_complete` reports a wait that outlasts its threshold (`slow_waits`, not a failure) and keeps
     waiting until completion or loss. The compute context uses it; the bounded `vk_wait` stays for callers with
     nothing in flight.
   - **Test:** a submission held by a timeline-semaphore gate that a host thread opens after 50 ms, against a 1 ms
     threshold. It returns `VK_SUCCESS` with the fence signalled, one slow wait and no failure.
   - **Timeline semaphores:** the device now enables them (Vulkan 1.2 core, required by 1.3, chained as their own
     struct) and reports `timeline_semaphore()`.
   - **Teeth:** abandoning at the threshold fails the result, origin and failure-count checks.
2. **A latent shader-interface bug (fixed).** `REN-38-F6+` failed on every lane with
   `VUID-RuntimeSpirv-MeshEXT-10883`: a mesh shader that declares the task pairing (`payload = true`) was paired with a
   task shader that passed no payload to `OpEmitMeshTasksEXT`.
   - The GLSL task emitter declared `taskPayloadSharedEXT` only when the task wrote a payload field. The HLSL emitter
     always passes `s_payload` to `DispatchMesh`, which is why DX12 never failed.
   - A GLSL task shader now always declares the fixed four-field payload.
   - The NVIDIA path never reported the violation; lavapipe's validation did. On the RTX, `[ren38]` (43 cases) and the
     mesh and task cases stay green.
3. **GPU-assisted validation must never execute what it reports (fixed).** `DIAG.7a(g-3)` segfaulted on lavapipe in
   0.9 s. Reproduced on the WSL reference host (same Mesa 25.2.8), with these findings:
   - The fault is in llvmpipe's JIT-compiled shader code, on a driver worker thread. It persists with the layer's
     `gpuav_safe_mode` and even with shader instrumentation off: the specimen's own out-of-bounds store at index
     1,000,000 executes, and on a CPU device that write lands outside the buffer in process memory.
   - **Engine fix:** a context that requests GPU-assisted validation now runs it only in the layer's safe mode, set
     through `VK_EXT_layer_settings`. `VulkanValidationLayer::gpu_assisted_guarded` records this, and without layer
     settings the mode reports `ExtensionAbsent` instead of running unguarded.
   - **Specimen fix:** the specimen is now memory-safe by construction, in raw Vulkan. Its 16-byte buffer, which is the
     descriptor's range, sits at offset 0 of an 8 MiB allocation the test owns. The index is out of bounds for the
     descriptor (what GPU-AV checks) while the physical store stays inside owned memory.
   - It passes on the RTX (three runs, correlated to the Program identity) and on lavapipe.
4. **Lifetime specimens must not execute against destroyed objects (fixed in this batch, before CI saw them).**
   - **Vulkan:** the first version submitted a command buffer whose bound buffer had been destroyed. It segfaulted on
     lavapipe, which dereferences the driver's buffer object when it executes; keeping the memory alive was not enough.
     The destroyed buffer is now recorded in a secondary that a primary references with `vkCmdExecuteCommands`. The
     layer reports it at record time with the buffer's retired identity, and nothing is ever submitted.
   - **DX12:** after the in-flight release raises 921, the hazard leg removes the device (the engine's own failure
     response) instead of opening the gate. The held copy is discarded, never executed, so a software adapter cannot
     touch the released resource.
   - Both pass: the Vulkan one on the RTX and on lavapipe, the DX12 one three times plus the full DX12 suite.
5. **The kernel emitters hoisted guarded memory reads (fixed).** `B4-vis-2` (deferred attribute shading) segfaulted
   intermittently on hosted lavapipe: `linux-gcc-release` on `f7689b5b`, `linux-gcc-debug` on `1ab3b1ef`. The ASan lane
   on `1ab3b1ef` placed it: a page-aligned read in JIT-compiled shader code, on a driver worker thread.
   - **Cause:** the kernel guards the pixel index and the empty-key fetch, but every compute emitter's hoist pre-pass
     lifted the guarded temps above both `if`s. An empty key (`0xFFFFFFFF`) unpacks to triangle 4095, so the shader read
     element 12,285 of a three-element index buffer, then positions at whatever that returned. NVIDIA and WARP tolerate
     the stray read. On lavapipe it faults only when the address reaches an unmapped page, which depends on the heap
     layout; 143 local passes had never hit it.
   - **Fix:** `KernelEmissionOrder::must_defer` now keeps every computed-address read (`BufferLoad`, `SharedLoad`,
     `StorageLoad`, `TexelFetch`) and its consumers in order, so all five dialects emit them inside their guard. It
     previously deferred only loads of written buffers.
   - **Teeth:** the DAIS emit case asserts that no dialect reads the index buffer before the first `if (`. It fails in
     all five dialects with the old header.
   - The earlier hypothesis (an AVX-512 runner) is withdrawn, and the stderr device line added for it is removed.
6. **Cook tests wrote to a fixed host path (fixed).** Reference-host runs from the repository directory left an
   untracked `C<U+F03A>/Users/...` tree of `.crdr` and `.kgph` files at the root. The cause was not the Windows temp
   variables: seven cook-test paths in `test_vulkan_context.cpp`, and one hidden-tool path in `test_ckir_mlp.cpp`, were
   fixed to an old agent session's Windows scratch folder. Linux treats such a path as relative, and hosted Windows
   writes it under a user profile that does not exist on the runner. They now use `crd::platform::fs::temp_directory()`
   (the hidden tool uses `std::filesystem::temp_directory_path()`). On the reference host the cook, variant and D-007 D4
   cases pass from the repository directory and write only under `/tmp`.

Reference-host results on lavapipe after the fixes (Debug unless noted): the mesh gate, the radix sort, `[ren38]` (42 passed, 1 skipped),
`[validation]` (8 cases), `DIAG.7a(f)` and every `DIAG.7c` case pass.
