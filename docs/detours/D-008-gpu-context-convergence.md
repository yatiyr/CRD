# D-008 — the gpu-context convergence  ·  ⟶ MERGED into D-007

> **This detour is now tracked in the single master doc: [`D-007-gpu-program-system.md`](D-007-gpu-program-system.md).**
> D-007 (the universal shader IR) and D-008 (the device convergence) were merged 2026-07-11 into ONE ordered subslice
> table — the two-doc split was confusing (two different "C3"s; interleaved shader/device slices). This file is kept only
> as a redirect so existing links resolve.

**D-008 status (for the record, as of 2026-07-11 — the `rhi/shader` modules named below were later deleted
outright at RET-8, 2026-07-23, ADR-0105): the device convergence is CLOSED.** `C0 · C1 · C2-a…f` all ✅ — ONE `VkDevice` (the
`VulkanGpuContext` owns it; `rhi-vulkan` adopts, never creates), **I1 + I2 both closed** (no shading language in
`crd-shader`; no bytecode in any public rhi header — consumers hold opaque `IGpuProgram`). Full 4-config sweep green
2026-07-11. Decision record: [ADR-0103](../decisions/0103-gpu-context-owns-every-gpu-program.md).

**The remaining frontier device slices** (`C3` ray-tracing context · `C4` DX12 raster · `C5` GPU-driven/work-graphs ·
`C6` cooperative-vector device) live in the master table in [`D-007-gpu-program-system.md`](D-007-gpu-program-system.md),
interleaved with the shader slices they pair with (C3↔B9, C5↔B11, C6↔B10).
