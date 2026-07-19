# Session — 2026-07-19 · RT frontier: path tracing → ReSTIR spatiotemporal → DX12 mirror → instancing → many-lights

**Detour:** D-007 (CKIR ray tracing). Continuation of the "complete gold-standard cutting-edge RT system integrated into CKIR"
campaign before resuming B18 hair. Entry state: RT-1 (inline rayQuery core) + RT-2 (shadows/RTAO/reflections) already landed.

## Shipped this session (every effect GPU-verified vs the shared CPU brute-force ray-triangle oracle)

1. **RT-3 path-tracing megakernel** (`build_pathtrace_kernel`) — runtime sample `For` loop + UNROLLED diffuse bounce chain
   (CKIR `For` carries no registers ⇒ origin/dir/throughput/radiance thread as SSA ids), cosine-weighted hemisphere, sky-as-light.
   GPU==oracle to ULP (1.19e-7); real multi-bounce GI spatial variation.
2. **RT-4 NEE + MIS area-light path tracer** (`build_pathtrace_nee_kernel`) — next-event estimation at every vertex + BSDF sampling
   combined via the Veach power heuristic. Emits 3 strategies (MIS/NEE/BSDF) → the Veach unbiasedness check (all three converge:
   MIS 0.6379 = NEE 0.6276 = BSDF 0.6396). GPU MIS==oracle (worst 0.043 = grazing shadow-ray flips) + real soft shadow.
   ⚠ SCAR: CKIR `select` can lower to arithmetic (mix) ⇒ masking does NOT guard NaN — clamp `hit.t` finite + guard every denom.
3. **RT-5 ReSTIR DI RIS core** (`build_restir_di_kernel`) — streaming WRS reservoir over M candidates, one visibility ray for the
   survivor, W=Σw/(M·p̂). Unbiased vs pure-NEE-direct (0.07%); GPU==oracle ULP-exact.
4. **RT-5b/c ReSTIR SPATIOTEMPORAL** (temporal → spatial → shade, persistent 6-float/pixel reservoir, GPU ping-pong via in-out
   `trace_dispatch` bindings). Temporal: unbiased + **5.8× variance reduction** (warm 0.030 vs single 0.175). Spatial: neighbour
   resample with the UNBIASED Z-normalisation W=Σw/(Z·p̂), unbiased to 0.08%. ⚠ SCAR: post-spatial temporal feedback darkens 12%
   (compounding bias only GRIS fixes) ⇒ feed back the pre-spatial reservoir; test bias via the SPATIAL MEAN (per-pixel rms is noise).
5. **C3b DX12 DXR mirror** (`Dx12RayTracingContext`) — ID3D12Device5 BLAS/TLAS + inline RayQuery; root SRV t0 = TLAS + UAV table
   u1..N. Bumped `compile_hlsl_to_dxil` → cs_6_5. Same CKIR kernels: NEE/MIS worst **0.0433693 byte-identical to Vulkan** ⇒
   **VK≈DX12 established**. Full DX12 suite 765/87.
6. **RT-6 multi-instance TLAS** (`build_scene_instanced`, both backends) — N instances, per-instance 3×4 transforms; GPU==oracle
   (world-space copies). The portable frontier-SCALE capability. (Literal SER/OMM/cluster = vendor-locked HW extensions, deferred.)
7. **RT-7 many-lights NEE** (`build_manylight_nee_kernel`) — N lights in a runtime buffer, uniform ⌊u·N⌋ selection + N·area weight,
   in-kernel |eu×ev|. GPU unbiased: N-light == Σ per-light (0.16%). The substrate for light-BVH / multi-light ReSTIR.

Every new kernel is both-backend emit+compile gated (GLSL + HLSL) in `test_ckir_glsl_compile.cpp`.

## Test tallies (no regression)
- gpu-context-vulkan: **1511 assertions / 127 cases** (full suite). `[rt]` 407/15, `[glsl]` 62/9.
- gpu-context-dx12: **765/87** (full suite; RT `[rt]` 31/4).
- kir `[rt]` 12/4.

## Files
- `engine/kir/include/crd/kir/ckir_rt.hpp` — +pathtrace, +NEE/MIS, +ReSTIR DI/temporal/spatial/shade, +many-lights kernels.
- `engine/gpu-context-vulkan/{src,include}/…vulkan_ray_tracing_context.*` — `build_scene_instanced`.
- `engine/gpu-context-dx12/{src,include}/…dx12_ray_tracing_context.*` (NEW) + `dx12_context.cpp` cs_6_5 bump.
- tests: `test_vulkan_rt.cpp`, `test_dx12_rt.cpp` (NEW), `test_ckir_rt.cpp`, `test_ckir_glsl_compile.cpp`.

## Next
Remaining integrator breadth (future increments): emissive/textured hit shading, Russian-roulette termination, ReSTIR GI
(indirect reservoirs), light-BVH/power sampling. Then resume **B18 hair** on top of the RT tier (LSS strands + path-traced
dual-scattering reference). Vendor SER/OMM/cluster micromaps: HW-gated follow-ons if a specific target needs them.
