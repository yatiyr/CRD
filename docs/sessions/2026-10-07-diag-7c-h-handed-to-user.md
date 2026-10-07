# DIAG.7c(h) handed to the user, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7c](../ROADMAP.md#slice-diag.7c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-7c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.7c(g)](2026-10-07-diag-7c-g-loss-bundle.md); [decisions](2026-10-05-user-decisions.md).

## Status

DIAG.7c (a) to (g) are done locally. Item (h) is the real-device-loss proof, split by the 2026-10-05 decision into a
software leg and the user's opt-in hardware run (h2).

Two serial loop steps on 2026-10-07 tried to plan the software leg (the Vulkan counterpart of the DX12 fault child in
`tests/gpu/gpu-context-dx12/test_dx12_device_fault.cpp`). Both stopped before any code was written. Nothing was built,
measured or committed for (h), and the working tree was left unchanged.

## Decision needed

The whole of (h) now waits for the user:

1. **Software leg (h1).** Decide whether to build it and how. The open question is whether lavapipe can produce a
   real `VK_ERROR_DEVICE_LOST` in a contained child process. No measurement exists yet.
2. **Hardware leg (h2).** The opt-in run on the user's own GPU, as already decided for DX12.

Until then DIAG.7c stays Partial. The validator lets later rows progress, so the loop continues with DIAG.8a.
