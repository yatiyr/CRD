# DIAG.7 Vulkan capability bail-outs report skipped, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.7a](../ROADMAP.md#slice-diag.7a) and [DIAG.7c](../ROADMAP.md#slice-diag.7c).
> Contract: [runtime diagnostics](../design/runtime-diagnostics.md#diag-7a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.7c(e)+(f)](2026-10-07-diag-7c-descriptor-limit-and-stale-layout.md).

## Goal

The Vulkan DIAG.7 device cases left through `WARN("...; skipping")` followed by a bare `return` when the machine
lacked a capability (no Vulkan device, GPU-assisted validation not active, no validation layer, no ray query). Catch2
counts such a case as passed, so a lane without a device reported these cases as green although they tested nothing.
This batch makes them report skipped. It is committed locally and not pushed.

## How a skip is counted (checked before the change)

- **Registration.** `crd_discover_tests` forwards to Catch2's `catch_discover_tests` without `SKIP_IS_FAILURE`, so
  Catch.cmake adds `SKIP_RETURN_CODE 4` to every discovered case (all 302 cases of `crd-gpu-context-vulkan-tests` in
  the generated win-debug test file carry it). CTest runs one case per process; Catch2 exits 4
  (`AllTestsSkippedExitCode`) when every selected case skipped, and CTest records the case as `Skipped` (JUnit
  `status="notrun"` with `<skipped>`), returning 0. No CMake change is needed.
- **`scripts/check-registered-failures.py`** maps `<skipped>`/`notrun` to `skipped` and compares only the failed set
  with the register. An unregistered skip is therefore neither a failure nor a pass. A registered test that skips
  still fails the gate (existing test). The hosted Windows lanes already skip other Vulkan device cases this way
  (the 7a(f) and 7c(b)-(d) cases in `test_vulkan_context.cpp` already use `SKIP`).
- **`scripts/ci-evidence.py`** counts skipped cases apart from passed ones in the lane summary.

## What changed

- `tests/gpu/gpu-context-vulkan/test_diag_validation_identity.cpp`: all 12 capability bail-outs (the identity case,
  DIAG.7a(g), (g-2), (g-3), (g-4), the 7a lifetime case, 7c(e) and 7c(f)) are `SKIP("<same reason>")`. The
  `GpuavRig` destructor's early return is cleanup and stays.
- Audit of the other Vulkan DIAG.7 cases: the seven DIAG.7a(d2b-vk) identity cases in `test_vulkan_context.cpp`
  (14 sites) and `D2b-vk: build_scene` in `test_vulkan_rt.cpp` (3 sites) had the same pattern and now `SKIP`. The
  frame-graph d2b/d2c cases and the 7a(f), 7c(b)-(d) cases already used `SKIP`. Their leading comments that said
  "WARN-skips" now say "SKIPs".
- `scripts/check-registered-failures.py` prints the skipped count in its first report line; a new unit test in
  `scripts/test-repository-tools.py` pins that an unregistered skip neither fails nor passes a lane, with and without
  register entries.

## Verification

- **Before, no device** (win-debug, `VK_LOADER_DRIVERS_DISABLE=*` makes `create_vulkan_gpu_context` return null):
  `crd-gpu-context-vulkan-tests "[identity]"` reported `21 | 16 passed | 5 skipped`, exit 0. Fifteen of the 16 "passes" were
  converted cases that tested nothing; only the device-free adapter case ran its assertions.
- **After, no device** (same environment): `21 | 1 passed | 20 skipped`. A single case (`DIAG.7a lifetime*`) prints
  `SKIPPED: no Vulkan device available` and exits 4. Through CTest (`--test-dir .../gpu-context-vulkan` with the 23
  converted and related DIAG.7 Vulkan cases): `***Skipped` for 22, `Passed` for the adapter case, CTest exit 0.
  `check-registered-failures.py --lane win-debug` on that JUnit: `observed failures: 0; skipped: 22; results: 23`,
  exit 0; `ci-evidence.py`'s summary counts 1 passed, 22 skipped, 0 failed.
- **After, device present, validation layer disabled** (`VK_LOADER_LAYERS_DISABLE=*`): 7a(g-3) and 7c(e) skip at
  "GPU-assisted validation not active on this device", the lifetime case and 7c(f) at "the validation layer is not
  present here"; each exits 4. Catch2 counts a case as skipped even after a passed `REQUIRE(vk->valid())`.
- **Device present** (this machine's GPU), whole `crd-gpu-context-vulkan-tests`: win-debug 302/302 passed (7307
  assertions); win-shipping and win-clang-cl-shipping 302/302 passed (7284 assertions each). All three lanes built
  clean.
- **Lavapipe** (WSL, linux-gcc-debug, Mesa lavapipe with the pinned validation layer as on the hosted Linux lanes):
  the 23 DIAG.7 Vulkan cases through CTest all `Passed`, none skipped (GPU-assisted validation is active there);
  whole suite `302 | 301 passed | 1 skipped` (REN-38-A13, "adapter has no variable-rate shading", unchanged).
- Strict tidy (LLVM 20.1.8) clean on the four touched test files; `check-allman-braces.py` PASS; clang-format
  `--dry-run` reports the same count on each touched file as at HEAD (the files are hand-formatted); `ctest -R
  "^crd-no-|^crd-check"` 8/8; `test-repository-tools.py` OK; `check-master-plan.py` and `check-repository.py` PASS.

## Left for later

The same `WARN` + `return` pattern remains in about 50 non-DIAG.7 test files (and the DX12 DIAG.7 cases); it is
listed for a separate batch rather than widened here.
