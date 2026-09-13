# Qualifying inner coverage without adapter exceptions

<!-- doc-role: reference -->
> Technical recipe. Live owner: [REPO.3c.7](../ROADMAP.md#slice-repo.3c.7). Rules: [AGENTS](../../AGENTS.md).

## Parameters

| Input | Meaning / units | Reproduction value and constraint |
|---|---|---|
| Target / viewport | Physical pixels | 64 × 64, single sample, full scissor |
| Primitive | Clip-space coordinates, w=1 | (0,-0.35,0), (0.35,0.35,0), (-0.35,0.35,0) |
| Raster state | Native conservative mode | Overestimate ON, solid fill, no culling, no depth/stencil, full sample mask |
| Capability | Queried conservative tier | Tier 3 required; absence is a real skip, never a pass |
| Interior probe | Pixel coordinate | (32,30), fully inside |
| Edge probe | Pixel coordinate | (32,20), crosses the horizontal edge at y=20.8 |
| Background probe | Pixel coordinate | (0,0), outside the primitive |
| Rendered colors | Exact RGBA8 values | White interior, red partial pixel, opaque black clear |
| Native reduction | Raw system value | R32_UINT output, integer clear 2, inspect bit 0 |
| Bounds | Wall time / capture capacity | CTest 180 s; SDK fence 30 s; native capture 4,096 messages |

## Contract and oracle

Conservative overestimate emits fragments for partially intersected pixels. Inner coverage identifies pixels entirely
inside the primitive. Bit 0 is the native Boolean; it must not claim complete coverage for a partial pixel. Uncertainty
near raster snapping boundaries is bounded, so choose probes well away from that band. These requirements apply to
queried Tier-3 providers, including software. See Microsoft's [InnerCoverage specification](https://microsoft.github.io/DirectX-Specs/d3d/ConservativeRasterization.html#innercoverage).

For the horizontal clip-space edge y=0.35, viewport conversion gives y=(1-0.35)×64/2=20.8. Pixel row 20 spans [20,21],
crossing that edge by a substantial fraction of a pixel. Its coverage cannot become fully inside through subpixel
rounding. The interior and background probes separately prevent a missing draw or all-zero shader from passing.

The public test counts every pixel as one of three exact colors and requires both interior and edge populations.
It additionally checks the three specified coordinates. Changing the previous blue background / black edge to black
background / red edge preserves the geometric oracle and matches the standalone target's optimized black clear hint.
Native capture consequently need not filter an avoidable clear-hint warning.

## Assembly and diagnosis

1. Start the [shared validation capture](../../tests/gpu/gpu-shared/dx12_validation.hpp) before device/context creation.
   Query Tier 3. Context/compiler absence uses Catch2 SKIP; invalid programs after successful creation fail explicitly.
2. Build the triangle through CKIR. In separate sections compile a CKIR fragment and a direct native HLSL conformance
   fragment. Both map `inner & 1` to white/red. Direct HLSL is an independent backend test, not a shipped algorithm.
3. Use the public command encoder. Its `RasterDrawPacket.state.conservative` selects `draw_conservative`; DX12 builds
   and caches the graphics PSO with native ConservativeRaster ON. Read back only after checked completion.
4. Preserve the pixel assertions and inspect native messages after all contexts unwind. A silent InfoQueue does not
   prove correct pixels. Compare actual hardware and explicitly identified WARP, restoring original settings afterward.
5. If both shader routes fail, reduce to SDK-only D3D12: explicit adapter, empty root, the same triangle/viewport/state,
   R32_UINT target, direct shader output `return coverage`, ordered render-to-copy barrier, readback and finite fence.
   Compile both DXIL (DXC SM6) and DXBC (D3DCompile SM5). Record the loaded WARP module path as well as driver identity.

The retained SDK shaders are deliberately minimal:

```hlsl
float4 main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 0 ? 0 : (id == 1 ? .35 : -.35), id == 0 ? -.35 : .35, 0, 1);
}
// Separate pixel shader compilation:
uint main(nointerpolation uint coverage : SV_InnerCoverage) : SV_Target { return coverage; }
```

## Findings and traps

The [dated session](../sessions/2026-09-13-atomic-and-coverage-verification.md#coverage-reproduction-and-native-reduction)
records the actual provider mismatch and all failed arms. The native reduction excludes Cerid's libraries, CKIR,
command lowering and frame graph; it does not identify an internal vendor-code defect or establish a universal
statement about every WARP/runtime release. A different runtime/provider combination still needs its own result.

- Adapter classification is identity evidence, not permission to exempt a promised feature from its oracle.
- Masking the native bit field was tested; it did not repair the reproduced false interior result.
- Switching DXIL to DXBC was tested; it did not repair this reproduction.
- A newer signed WARP package was tested in isolation and removed afterward. Its presence is not proof of a fix.
- Do not silently report Tier 2, add a software skip or suppress diagnostics to close this native-feature contract.
  The route contract below is the reviewed capability/fallback contract; it is qualified end to end on both providers
  and never replaces the native bit where that bit is qualified.

No performance benchmark or speed claim is made. This is correctness and provider-isolation evidence.

## Route contract (implemented 2026-09-13)

`IRasterContext::inner_coverage_route()` reports `Native`, `Barycentric` or `Unsupported`; `supports_inner_coverage()`
is true for either route. DX12 (`dx12_inner_coverage_route`) keeps the native `SV_InnerCoverage` on conservative Tier 3
unless the selected adapter is the documented software provider; that provider, and Tier 1/2 providers, take the
barycentric route when OPTIONS3 barycentrics and shader model 6.1 exist (both providers here report `barycentrics=1`,
`highest=0x66`); otherwise program creation is refused. The emitter (`HlslInnerCoverage::Barycentric`) declares
`noperspective float3 bary_ic : SV_Barycentrics`, compiles as `ps_6_1`, and lowers the builtin to
`all(b - 0.5*(abs(ddx(b)) + abs(ddy(b))) >= 0)`: screen-space-linear barycentrics are affine, so the derivatives are
exact and the expression is the minimum over the four pixel corners. The same conservative overestimate draw feeds it.

The public test is three cases against one CPU oracle that classifies every pixel by separating axes (fully covered,
touched, untouched; corners within 1/64 px of an edge or vertex excluded): the public route, the forced barycentric route
(`dx12_override_inner_coverage_route`, a test-only override), and native conformance through direct HLSL, which skips
with its reason where the native route is unqualified. Hardware and WARP 10.0.26100.8972 both render interior 220, edge
92, background 3,784 with zero ambiguous or mismatching pixels ([evidence](../sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md)).
Unknown adapters keep the D3D12 contract; the oracle, not the identity, decides whether such a provider is correct.

## Code and reproduction

- [DX12 test](../../tests/gpu/gpu-context-dx12/test_dx12_raster.cpp): named B1-f inner-coverage case, two shader sections.
- [Shared triangle](../../tests/gpu/gpu-shared/ckir_raster_triangle.hpp),
  [command lowering](../../engine/gpu/gpu-context/include/crd/gpu/detail/command_lowering.hpp),
  [DX12 PSO/recording](../../engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp).
- Rebuild `crd-gpu-context-dx12-tests` in the primary configuration. Scoped CTest regex:
  `^D-007 B1-f: (inner coverage distinguishes.*|conservative OVERESTIMATE.*) \(DX12\)$`.
- SDK-only source, CMake project, bounded runner and logs remain under ignored
  `build/research-dev-workflow-20260912/coverage-sdk-probe/`; the exact native assembly and shaders are preserved above.
  Package acquisition evidence is beside it under `warp-package-1.0.20/`. Microsoft distributes that package for
  [testing and development](https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.20); it was not installed globally
  or added as a shipping dependency.
