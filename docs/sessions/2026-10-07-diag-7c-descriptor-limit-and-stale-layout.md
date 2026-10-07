# DIAG.7c(e) descriptor-limit fallback and (f) stale-resource cases, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7c](../ROADMAP.md#slice-diag.7c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-7c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.7c census and (b)-(d)](2026-10-05-diag-7c-census.md);
> [DIAG.7b(h1)](2026-10-07-diag-7b-h1-warp-fault-and-pass-resolution.md).

## Goal

The census plan left four units of DIAG.7c. This batch does two of them:
- **(e)** The descriptor-limit fallback. GPU-assisted validation needs a free descriptor-set slot for its
  instrumentation. A pipeline that uses every slot must be reported as uninstrumented, and one that uses fewer must be
  instrumented.
- **(f)** The stale-resource cases. These are a destroyed or retired resource and a wrong image layout, each correlated
  to its identity. The clause also asks that reports arriving only after fence completion be asserted explicitly.

(g), the loss bundle, and (h), a real device loss, remain.

## Measured first: the layer keeps the reported limit and refuses the last slot

A probe on the workstation (RTX 4070 Ti SUPER, VVL 1.4.341) compared two contexts, one without validation and one with
GPU-assisted validation.
- **The limit is not lowered.** Both contexts report `maxBoundDescriptorSets` 32. The layer reserves slot 31 for its own
  set, but a consumer cannot see that in the limit.
- **A 32-set layout is refused.** `vkCreatePipelineLayout` with 32 sets returns `VK_SUCCESS`, with no error and one
  warning. The warning's message-ID name is `WARNING-GPU-Assisted-Validation`, and its text says the layout "will
  conflicts with validation's descriptor set at slot 31". It also says pipelines created with the layout get no GPU
  shader instrumentation, so GPU-AV will report nothing for them at runtime.
- **A 31-set layout is accepted** with no message.
- **Shader objects:** the layer has the same refusal for `VkShaderEXT`.

So the fallback is silent, apart from one warning at creation time. The engine has to report two things: the slot,
because the limit hides it, and the refusal, as an instrumentation failure rather than an ordinary warning.

## What landed

### Engine

- **The slot as a capability.** `VulkanValidationLayer` (public, `vulkan_context.hpp`) gains `instrumentation_set_slot`.
  It is `maxBoundDescriptorSets - 1` while GPU-assisted validation is active, and `kNoInstrumentationSetSlot`
  otherwise. It also gains `instruments(set_layout_count)`, which says whether a layout with that many sets would be
  instrumented. The context records the slot in a new helper, `resolve_gpu_assisted`, which also takes over the
  existing check that decides whether GPU-assisted validation is active. Moving it out keeps the device-creation
  function within the strict gate's size limit.
- **The refusal through the one collector.** The capture classifies messages named `WARNING-GPU-Assisted-Validation`
  (the layer's GPU-AV setup warning) as instrumentation refusals:
  - `ValidationMessage` gains `message_id_name` (the layer's `pMessageIdName`) and `instrumentation_refused`.
  - The capture gains `uninstrumented_count()`.
  - `report().instrumentation_failures` now carries that count. It was always 0 on Vulkan. A run that saw a refusal is
    therefore not `clean()` under the common rule, whatever else it reported.
  - The common `ValidationReport` vocabulary is unchanged. The classifier is pinned to the SDK's layer, and the (e)
    test fails if a layer update renames the message.
- **No consumer changes.** The engine's own layouts use few sets. Every layer message reaches the capture whoever
  created the layout, so the production reporting path covers application layouts too.

### Tests (`tests/gpu/gpu-context-vulkan/test_diag_validation_identity.cpp`)

- **The GPU-AV specimen takes a set count.** The DIAG.7a(g-3) out-of-bounds specimen moved into
  `run_gpuav_oob(vk, set_count)`. Set 0 holds the two buffers and the rest are empty. It stays memory-safe by
  construction: the 16-byte descriptor range sits inside an 8 MiB allocation. One capture covers layout and pipeline
  creation, where a refusal is reported. A second capture covers the dispatch only, as before, so g-3's oracle is
  unchanged: an error or warning, a record correlated to the live pipeline or buffer, and nothing dropped.
- **g-3 adds the asynchronous assertion (f).** The specimen counts errors and correlated records after `vk_submit`
  returns and before `vk_wait_complete`. Both must be 0, and the correlated record must arrive by the wait. GPU-AV reads
  its error buffer back only when completion is observed, so this is deterministic and not a race.
- **New case `DIAG.7c(e)`** (`[gpuav][descriptor-limit]`):
  - The slot is `limit - 1`, derived from the device limit and never a literal 32. `instruments(limit - 1)` is true and
    `instruments(limit)` is false.
  - **One slot fewer than the limit:** no refusal, and the out-of-bounds store is reported after the fence and
    correlated to a live identity.
  - **Every slot:** a refusal at layout creation. The same out-of-bounds dispatch then reports nothing: no correlated
    record, and no error or warning at all. This silence is what proves the refusal is real and not only a warning
    string.
  - **The common report:** a scope that saw the refusal has `instrumentation_failures >= 1`, equal to
    `uninstrumented_count()`. It is not `clean()`, and the record carries `instrumentation_refused`.
  - **Shader objects** (when the device has them): a compute `VkShaderEXT` with every slot is refused, and with one
    fewer it is not.
- **New case `DIAG.7c(f)`** (`[hazard][stale-layout]`):
  - **Hazard leg:** a Cerid-named 4x4 image is never transitioned out of `UNDEFINED`, then cleared as
    `TRANSFER_DST_OPTIMAL` on the compute queue and submitted through the seam.
  - The command is valid when recorded: 0 errors at record. Once completion is observed, an error names the image's
    live identity.
  - **Control leg:** the same clear after an `UNDEFINED` to `TRANSFER_DST_OPTIMAL` barrier is clean, with no error,
    warning or correlated record.
  - The clear writes only the fresh image, so it is safe on every device, lavapipe included.
- **The destroyed or retired resource** was already covered by the DIAG.7a lifetime case. A destroyed Cerid buffer,
  referenced through a secondary command buffer, produces an error correlated to its retired identity. (f) cites it
  rather than duplicating it.

### Measured: the layout error arrives at the wait, not at the submit

The layer's message names `vkQueueSubmit()` (`VUID-vkCmdDraw-None-09600`, the generic layout-mismatch VUID). Even so,
on VVL 1.4.341 it is not in the capture when `vkQueueSubmit` returns:
- 0 errors in five runs;
- still 0 after a 300 ms sleep with the fence not waited;
- present once `vk_wait_complete` returns.

The case asserts only what is robust: nothing at record, and the correlated error by the wait. It prints the count at
submit return for the record. The sleep probe was measurement only and is not kept.

## Evidence

Workstation: RTX 4070 Ti SUPER, driver 595.79, VVL 1.4.341. The (e), (f) and g-3 cases have 90, 31 and 35
assertions. (f) has 33 after the teardown change described under the WSL results, and `[validation]` has 315.
- **win-debug:**
  - The three cases pass, and so does `[validation]` (10 cases, 313 assertions).
  - The full `crd-gpu-context-vulkan-tests` passes: 300 cases, 7,180 assertions. This run came before the (f)
    teardown change below; after that change, the three cases and `[validation]` were rerun on every lane.
  - The whole tree builds (`all`): the two public headers are included by 39 files.
- **win-shipping, win-clang-cl-shipping and win-asan:** the three cases, `[validation]` and `[device-loss]` (4 cases,
  76 assertions) pass. ASan reports nothing, and the clang-cl link was clean on the first attempt.
- **WSL reference host** (lavapipe, Mesa 25.2.8, the pinned VVL 1.4.341, as on the hosted Linux lanes):
  - On linux-gcc-debug, linux-gcc-asan and linux-clang-tsan, the three cases, `[validation]` and `[device-loss]`
    (69 assertions) pass. So the slot is `limit - 1`, and the refusal and the silent dispatch hold on lavapipe too.
  - **TSan found a teardown race in the first (f) version.** The leg destroyed its fence right after the fence wait,
    while the layer's own queue thread (`vvl::Queue::ThreadFunc`) was still releasing that fence's state lock. The
    race is inside the layer and not in Cerid code, but the test caused it.
  - The leg now idles the device through the seam (`vk_device_wait_idle`) before the messenger and the objects are
    destroyed, as the GPU-AV rig already did. After that, (f) ran five times with 0 TSan reports, and `[validation]`
    was clean. With that change, gcc debug and gcc asan were rerun, and every Windows lane rebuilt and passed (f)
    (33 assertions) and `[validation]` (315).
- **Checks:**
  - Strict tidy is clean on the five changed C++ files, and the brace guard passes.
  - clang-format `--dry-run`: the only finding on the added lines that is more than local alignment (a line break)
    was taken. The rest is the files' established hand alignment, which the configuration does not model.
  - The 8 ctest guards, check-master-plan and check-repository pass.

## Teeth

- **Classifier off** (`is_instrumentation_refusal` never matches): the (e) case fails four assertions. They are the
  every-slot setup refusal, the report's `instrumentation_failures`, the record flag and the shader-object refusal.
  `clean()` stays false because the warning still counts, which is the point of carrying the failure separately.
- **Slot off by one** (`maxBoundDescriptorSets` instead of `- 1`): the (e) case fails at the slot check.
- **Hazard leg transitioned** (the stale leg run with the barrier): the (f) case fails at `stale.correlated_error`.

Each edit was restored and win-debug rebuilt. The three cases passed again (90, 31 and 35 assertions), and so did
`[validation]` (10 cases, 313 assertions).

## Remaining in DIAG.7c

- **(g) Loss bundle.** The recorded `VkDeviceFailure` and `VkDeviceFaultReport` in the DIAG.5 evidence container, like
  the DX12 removal bundle.
- **(h) A real device loss,** following the 2026-10-05 split:
  - Measure whether a software Vulkan device (lavapipe) can produce a real `VK_ERROR_DEVICE_LOST` in a contained child.
    That is local work, like 7b(h1) on WARP.
  - The opt-in hardware run is the user's (h2).
- **Hosted evidence.** The new cases run on hosted lavapipe. The hosted run must show the (e), (f) and g-3 cases
  passing on the Linux lanes. The hosted Windows lanes have no Vulkan device, so there the cases skip with a warning.
