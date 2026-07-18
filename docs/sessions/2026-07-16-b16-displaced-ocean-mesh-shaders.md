# 2026-07-16 — B16 displaced-geometry ocean + B4 mesh shaders + promote + visual polish (D-007)

Detour D-007 (GPU program system / CKIR). A long session: the FFT-ocean pivoted from a flat normal-map fragment render to
**real displaced GEOMETRY**, gained a full **mesh-shader fast path** (a new B4 subsystem), was **promoted** into the engine as a
reusable pass, and got a **gold-standard visual polish**. Everything authored in CKIR; the primary render path is GPU-proven on
Vulkan, the DX12/HLSL path is emit+DXC-validated.

## Shipped (all green)

### New CKIR op: `SampleIndexedLod` (bindless + explicit-LOD sample)
- The combo CKIR lacked: `SampleIndexed` is bindless but implicit-LOD/fragment-only; `SampleLod` is explicit-LOD but not
  bindless. A **vertex/mesh shader has no derivatives**, so displacing a grid from the bindless cascade textures needs both.
- Appended `SampleIndexedLod` at the END of `KOp` (cook-stable); builder `tex_sample_at_lod` (lod in the ext pool, mirroring
  `tex_sample_grad`); **GLSL** (`textureLod(sampler(tex[i],samp),uv,lod)`) + **HLSL** (`tex[i].SampleLevel(...)`) emitters + the
  nonuniform-qualifier gate. MSL/WGSL emit gracefully returns false (same status as bindless `SampleIndexed` — WebGPU has no
  descriptor-array bindless).

### Displaced-geometry ocean (the "white-noise" fix)
- A fragment normal-map on a flat plane renders the FFT's full spectrum as busy white noise. The smooth directional swells need
  **displaced GEOMETRY**. Built a **Johanson projected grid**: a screen-space lattice raycast onto the water plane, VERTEX-
  displaced by the baked FFT height, projected back to clip with the inverse-camera closed form (**no mat4**), camera-matched to
  the fragment sky so it composites pixel-aligned. The AAA split: **geometry carries the smooth swell; the FS carries the fine
  chop as a per-pixel mip-filtered normal map** (the FS has derivatives ⇒ minification resolves, no aliasing).
- **4 cascades** (non-harmonic world scales ⇒ LCM tiling ⇒ non-repeating); big cascades = tall geometry, fine = low-amplitude
  normal detail. **Joint-Jacobian foam**: each cascade bakes its fold, the FS sums them (folding of the combined surface).

### B4 mesh-shader fast path (a full new subsystem — GPU-proven on Vulkan)
- **IR**: a `Mesh` KEntry (`mesh_vertices`/`mesh_primitives`/`mesh_prim` uvec3-of-local-indices; per-vertex position/out from
  the workgroup builtins; `entry_valid` checks). **GLSL emitter** (`emit_mesh_glsl`: `GL_EXT_mesh_shader`, `SetMeshOutputsEXT`,
  guarded `gl_MeshVerticesEXT[tid]` + `gl_PrimitiveTriangleIndicesEXT[tid]`). **HLSL emitter** (`emit_mesh_hlsl`, SM 6.5:
  `[outputtopology]`+`[numthreads]`+`SetMeshOutputCounts`, `out vertices`/`out indices`, SV_Position last).
- **Device (Vulkan)**: `VK_EXT_mesh_shader` + `meshShader` + `maintenance4` enabled (gated so the non-mesh device is
  byte-identical); a mesh **shader object** (`VK_SHADER_STAGE_MESH_BIT_EXT` + the `NO_TASK_SHADER` flag); `create_mesh_program`,
  `draw_mesh`, `draw_mesh_bindless_depth` (+ `vkCmdDrawMeshTasksEXT`). shaderc `shaderc_mesh_shader`; DXC `ms_6_5`.
- **Proof**: the `[mesh]` triangle RENDERS (red-center/blue-corner, pixel-identical to vertex-pull); the **ocean meshlets**
  render == the vertex-pull ocean (8×8-vertex patches, 32×32 = 1024 meshlets, `WorkgroupIndex`→patch); the `[hlsl][mesh]` gate
  compiles the triangle + the bindless ocean meshlet HLSL → SPIR-V via DXC.

### Promote (#2) — the ocean is now a reusable engine pass
- New `engine/kir/include/crd/kir/ckir_water_render.hpp` (`crd::kir::water`): `OceanCascadeRender` config + `ocean_grid` camera +
  the shared `ocean_projected_vertex` + `build_ocean_displaced_vs` + `build_ocean_displaced_mesh` + `build_ocean_water_geo_fs`.
  The renderer + node editor can drive it. The tests alias it into `crd::gputest` (call sites unchanged; render byte-identical).

### B16 visual polish (user spec → matches `build/ref_ocean_2.png`)
- **Temporal foam** restored (`build_ocean_foam_accumulate` in the bake: 22-step warmup, `foam=max(prev·decay, inject)`, decay
  0.965 ⇒ accumulates then fades exponentially, never instant). **Textured** in the FS (Worley bubbles + fractal break-up, not a
  flat gray blob). **Gentle waves** + amplitude split. **Seamless horizon** (grazing veil → output ALPHA = transparency to the
  real sky). **AA** SSAA 3×. **God rays** tuned. **Darker defined clouds** (coverage + density self-shadow).

## Verification
- Vulkan raster suite **620 assertions / 56 cases green** (incl. the `[mesh]` triangle + `[hlsl][mesh]` gate); `[.ocean-frame]`
  **105 asserts green** (fragment + vertex-pull + mesh, all three render). Zero regression from the device/emitter changes.
- The `Vec2`/`Vec3` GLSL emitter fix (below) is a no-op for float ⇒ byte-exact canary held.

## Surprising lessons (durable — see MEMORY.md)
- ⛔ **mesh shader created without a task shader must set `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`** — else the driver expects a
  task stage and DEVICE-LOSTs on `vkCmdDrawMeshTasksEXT`, with NO validation error. This was the whole device battle.
  → [[feedback_mesh_shader_device_scars]]
- ⛔ **`nonuniformEXT` on a UNIFORM (compile-time-constant) bindless index returns ZERO in a mesh shader** on this NVIDIA driver
  (works fine in VS/FS). The emitter now omits the qualifier for a constant index (correct anyway) — that's what made the mesh
  ocean's texture displacement finally read. → [[feedback_mesh_shader_device_scars]]
- ⛔ Once `meshShader` is enabled, EVERY plain `vkCmdDraw` must bind the MESH stage to null (VUID-08690) or it device-losts.
- ⛔ glslang caps a mesh workgroup at **128** invocations (`local_size` too large) — an 8×8 patch (98 threads), not 12×12 (242).
- ⛔ The raster `Vec2`/`Vec3` GLSL emitter hardcoded the `vec2(`/`vec3(` float constructor → a `uvec3` of indices emitted
  `vec3(...)` (compile fail); fixed to `vtype(nd.type)`.
- ⚠ **Temporal-foam tuning is very sensitive** (too-high injection foams the whole surface; too-low gives none). Key: choppiness
  sharpens only the baked Jacobian, NOT the gentle vertical geometry — so crank it for foam without steepening the waves.
  → [[project_ocean_visual_gaps_before_b16_close]]

## Next
- **B16-close DoD**: clang-tidy on the touched headers (`ckir.hpp`, `ckir_glsl.hpp`, `ckir_hlsl.hpp`, `ckir_water_render.hpp`,
  the gpu-context sources) + the 4-config per-slice sweep (win-debug + clang-cl + asan + shipping, both backends).
- Optional further ocean polish: the faint residual horizon line, a softer sun, more directional god-ray shafts.
- Then carry on D-007: **B4 remaining** (DX12 device mesh render — PSO + `DispatchMesh`; task/amplification stage; B4-vis
  visibility buffer; B4-tess), the WGSL-portable `texture_2d_array` form of the cascade sampling, then **B17 OIT → B18 hair →
  B19 3DGS → the RT tier (C3/B9)**.
