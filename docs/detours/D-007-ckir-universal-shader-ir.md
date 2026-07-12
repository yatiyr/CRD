# D-007 — CKIR becomes the universal shader IR  ·  ⟶ MERGED into the master doc

> **This detour is now tracked in the single master doc: [`D-007-gpu-program-system.md`](D-007-gpu-program-system.md).**
> D-007 (the universal shader IR) and D-008 (the device convergence) were merged 2026-07-11 into ONE ordered subslice
> table — the two-doc split had become hard to follow (two different "C3"s; the shader (B) and device (C) slices
> interleave). This file is kept only as a redirect so existing links resolve.

**D-007 status (for the record):** Phase A (CKIR core, A1–A4) ✅ · backend fan-out (Vulkan/DX12/WGSL/CUDA/Metal) ✅ ·
B0 type system ✅ · B3-a/a′ stage model (14 execution models) ✅. **NEXT = B3-c** (raster GLSL VS+FS emitters), then the
rest of Phase B (materials/lighting → mesh → ray tracing → neural → work-graphs) and Phase D (cook), per the locked
"full visual frontier before hesap-GPU" order.

**The full ordered subslice table, the frontier capability→slice maps, the invariants (I1/I2), the stage model, and the
deferred front-ends (node editor + text DSL)** all live in [`D-007-gpu-program-system.md`](D-007-gpu-program-system.md).
North-star ADRs: [0101](../decisions/0101-ir-is-source-of-truth-for-all-shaders.md) · [0102](../decisions/0102-render-data-lighting-pass-architecture.md) · [0103](../decisions/0103-gpu-context-owns-every-gpu-program.md).
