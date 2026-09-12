# CEIR execution foundation

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

CEIR owns the canonical executable IR: operations, values, blocks, regions, types, effects, stable identity,
serialization, verification, compiler analyses/transforms and provider execution. CHIR owns high-level language
semantics and lowers into CEIR; CKIR owns device kernels/shaders. See
[ADR-0109](../decisions/0109-ceir-chir-ckir-ownership-and-module-placement.md).

## Modules and source

- [ceir](../../engine/ceir/include/crd/ceir/): GPU-independent IR, verifier, dialects and compiler infrastructure.
- [ceir-host](../../engine/ceir-host/include/): host execution/provider mechanisms.
- [ceir-gpu](../../engine/ceir-gpu/include/): GPU lowering/materialization/provider bridge, depending on the generic
  gpu-context facade and CKIR, not a concrete Vulkan/DX12 backend.
- [ceir-cook](../../engine/ceir-cook/include/): authored execution assets and cook integration.
- [Dialect definitions](../../engine/ceir/ops/): generated operations; regenerate through the canonical generator.

## Recorded completion

CEIR-1…35 is closed on its recorded contracts: IR/control/effects/lifecycle, compiler/execution planning, GPU/resource/
render/frame convergence, native tensor/ML/autodiff/optimization, transform/rewrite strategy assets, autotuning,
native launch graphs, distribution, media/UI/audio bridges, CHIR-0 and legacy deletion/qualification.
The [CEIR history](../detours/D-007-ceir-tracker.md) and
[35 close](../sessions/2026-09-11-ceir-35z-band-close.md) contain the evidence.

CEIR-33 C2 closed domain schemas/contracts, with the widget implementation retained in I2D-9. CEIR-35's production
work qualified the execution substrate and routed feature quality to its owning bands; it did not close the full
renderer/UI library. Its internal A/B boards are not renderer peer-crush claims.

## Runtime and lifecycle boundaries

Authored frame graphs converge through CEIR under [ADR-0127](../decisions/0127-ceir-frame-dialect-and-converter.md).
Providers bridge to host, Vulkan/DX12 graphics/compute and CUDA compute where implemented. Provider selection is
capability-dependent. Emitting a dialect or shader for a backend is not sufficient evidence of its full execution.

Cook/cache keys, compiled plans, state-schema compatibility and migration are reusable mechanisms. The standalone
plan-cache API's existence does not imply every host uses it. Consumer integration is checked in the owning slice.
Source reload in scene-render still needs the RAH-7 atomic/granular registry work. Audio's CEIR-31 acyclic proof does
not complete real-time feedback execution. CHIR-0 is not a full application language.

## Maturity and further work

The [manifest](../capabilities/gpu-platform-capabilities.toml) and
[generated matrix](../generated/gpu-platform-capability-matrix.md) own exact feature counts/levels/provider evidence.
Do not copy a dated count here. RAF and CEIR levels are distinct axes governed by the recorded C2/qualification
choices; none is a blanket “the whole engine is finished” claim.

Future rendering/UI/science/media work is tracked only in [ROADMAP](../ROADMAP.md#master-table), with source gaps
in the [audit](../research/2026-09-12-system-audit.md). The
[pre-audit overview](../archive/2026-09-12-superseded-plans.md#system-ceir) is preserved as a historical record.
