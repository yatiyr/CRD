#pragma once

// ckir_raster_triangle.hpp — a SHARED, backend-neutral CKIR triangle for the B3-e "IR-authored draw" gate (D-007). The
// SAME graph feeds the Vulkan (C1) and DX12 (C4) raster paths — the literal proof that ONE IR lowers to EVERY backend
// (ADR-0101). Pure crd::kir; no Vulkan / D3D12 here. The vertex entry emits the C1-b/C4-b triangle
// {(0,-0.8),(0.8,0.8),(-0.8,0.8)} from `VertexIndex` via a select-on-index chain (no dynamic array indexing — SROA-safe);
// the fragment entry outputs a constant red. Readback: the centre texel is inside the triangle (red), a corner is outside
// (the clear colour). Both entries are built from this one header in both backend test suites, so they cannot drift.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cook.hpp>     // B8-k: the material cook seam (per-pass variants + lowering)
#include <crd/kir/ckir_eval.hpp>     // B6-b: the CPU oracle — used to compute the noise observable's expected value
#include <crd/kir/ckir_material.hpp> // B5: the OpenPBR surface struct + G-buffer packing
#include <crd/kir/ckir_lighting.hpp> // B8: the shared CKIR lighting library (Cook-Torrance BRDF)
#include <crd/kir/ckir_lower.hpp>    // B7: the material lowering pass (frequency + optimize + specialize)
#include <crd/kir/ckir_post.hpp>     // B13: the post-processing frontier (HDR exposure/tonemap/output)
#include <crd/kir/ckir_render.hpp>   // B8-l: the render-path math (clustering / deferred / decals)
#include <crd/kir/ckir_screen.hpp>   // B12: the screen-space lighting frontier (AO/SSR/SSGI/volumetrics/SSS)
#include <crd/kir/ckir_taa.hpp>      // B13-a: temporal AA resolve (YCoCg clamp / variance clip / luma-feedback / dither)
#include <crd/kir/ckir_bloom.hpp>    // B13-b: bloom (Karis 13-tap pyramid / soft-knee / tent / FFT complex-mul / lens flare)
#include <crd/kir/ckir_cinematic.hpp> // B13-d: cinematic (thin-lens CoC / Garcia complex bokeh / McGuire motion blur)
#include <crd/kir/ckir_finish.hpp>   // B13-e: finish (Tokuyoshi specular AA / CA / cos⁴ vignette / Lottes grain / AMD CAS)
#include <crd/kir/ckir_nodes.hpp>    // B6: the MaterialX-parity node library
#include <crd/kir/ckir_noise.hpp>    // B6-b: the MaterialX SOURCE noise nodes
#include <crd/kir/ckir_clouds.hpp>   // B15-b: the Nubis Perlin-Worley cloud density (real clouds for the ocean sky)
#include <crd/kir/ckir_water.hpp>    // B16-a-4: the water-shading library (water_shade)
#include <crd/kir/ckir_water_render.hpp> // B16: the PROMOTED displaced-geometry ocean render pass (config + VS + mesh + FS)

#include <crd/memory/allocators/tlsf_allocator.hpp> // B6-b: scratch for the noise expected-value oracle

#include <cmath> // B6-b: std::lround for the noise expected-value quantisation

namespace crd::gputest
{

// VERTEX entry: gl_Position from gl_VertexIndex. Position i in {0,1,2} selects one of three clip-space corners; a select
// chain on the (integer) index avoids dynamic array indexing (which SROA cannot lower).
inline void build_triangle_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int vid = g.builtin(kir::KBuiltin::VertexIndex); // int
    const int k0  = g.constant(0.0, sh, kir::DType::I32);
    const int k1  = g.constant(1.0, sh, kir::DType::I32);
    const int eq0 = g.binary(kir::KOp::CmpEq, vid, k0); // bool: vid == 0
    const int eq1 = g.binary(kir::KOp::CmpEq, vid, k1); // bool: vid == 1

    // x: vid==0 -> 0.0 · vid==1 -> 0.8 · else -0.8   |   y: vid==0 -> -0.8 · else 0.8
    const int x0 = g.constant(0.0, sh, kir::DType::F32);
    const int x1 = g.constant(0.8, sh, kir::DType::F32);
    const int x2 = g.constant(-0.8, sh, kir::DType::F32);
    const int y0 = g.constant(-0.8, sh, kir::DType::F32);
    const int y1 = g.constant(0.8, sh, kir::DType::F32);
    const int x  = g.select(eq0, x0, g.select(eq1, x1, x2));
    const int y  = g.select(eq0, y0, y1);

    const int z   = g.constant(0.0, sh, kir::DType::F32);
    const int w   = g.constant(1.0, sh, kir::DType::F32);
    const int pos = g.vec4(x, y, z, w);

    ve.stage    = kir::KStage::Vertex;
    ve.position = pos; // a vertex entry MUST write clip position
    ve.n_out    = 0;   // no interpolants: the fragment shader paints a constant
}

// FRAGMENT entry: a constant red colour attachment at location 0 (no interpolant input — matches the VS above).
inline void build_triangle_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int one  = g.constant(1.0, sh, kir::DType::F32);
    const int zero = g.constant(0.0, sh, kir::DType::F32);
    const int red  = g.vec4(one, zero, zero, one);

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {red, 0};
}

// B4 MESH entry: the SAME triangle as build_triangle_vs, emitted by a MESH shader (the modern amplification path). One
// workgroup, one meshlet: 3 vertices (thread tid = LocalInvocationIndex writes vertex tid via the corner select) + 1 triangle
// (thread 0 writes the local index triple (0,1,2)). Pairs with build_triangle_fs (constant red). The literal proof that ONE
// CKIR mesh entry lowers → GL_EXT_mesh_shader → a mesh shader object → vkCmdDrawMeshTasksEXT → pixels.
inline void build_triangle_mesh(crd::kir::KGraph& g, crd::kir::KEntry& me)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  tid  = g.cast(g.builtin(kir::KBuiltin::LocalInvocationIndex), kir::DType::I32); // 0..2 = the local vertex
    const int  k0   = g.constant(0.0, sh, kir::DType::I32);
    const int  k1   = g.constant(1.0, sh, kir::DType::I32);
    const int  eq0  = g.binary(kir::KOp::CmpEq, tid, k0);
    const int  eq1  = g.binary(kir::KOp::CmpEq, tid, k1);
    const int  x0   = g.constant(0.0, sh, kir::DType::F32);
    const int  x1   = g.constant(0.8, sh, kir::DType::F32);
    const int  x2   = g.constant(-0.8, sh, kir::DType::F32);
    const int  y0   = g.constant(-0.8, sh, kir::DType::F32);
    const int  y1   = g.constant(0.8, sh, kir::DType::F32);
    const int  x    = g.select(eq0, x0, g.select(eq1, x1, x2));
    const int  y    = g.select(eq0, y0, y1);
    const int  z    = g.constant(0.0, sh, kir::DType::F32);
    const int  w    = g.constant(1.0, sh, kir::DType::F32);
    const int  u0   = g.constant(0.0, sh, kir::DType::U32);
    const int  u1   = g.constant(1.0, sh, kir::DType::U32);
    const int  u2   = g.constant(2.0, sh, kir::DType::U32);

    me.stage           = kir::KStage::Mesh;
    me.position        = g.vec4(x, y, z, w); // per-vertex clip position (thread tid → vertex tid)
    me.mesh_vertices   = 3;
    me.mesh_primitives = 1;
    me.mesh_prim       = g.vec3(u0, u1, u2); // primitive 0's three local vertex indices
    me.n_out           = 0;                  // no interpolants — the fragment paints a constant
}

// B4 TASK / AMPLIFICATION entry: ONE task workgroup emits `n` MESH workgroups (EmitMeshTasksEXT) + a single-uint payload. The
// literal proof of GPU-driven amplification: draw with a TASK-group count of 1, and N meshlets appear. Pairs with
// build_mesh_amplified_tri (which reads the payload) + build_amplify_fs.
inline void build_task_amplify(crd::kir::KGraph& g, crd::kir::KEntry& te, crd::u32 n, crd::u32 payload_val)
{
    namespace kir      = crd::kir;
    const auto sh      = kir::make_shape({1});
    te.stage           = kir::KStage::Task;
    te.local_size[0]   = 1;
    te.task_emit       = g.constant(static_cast<double>(n), sh, kir::DType::U32);              // launch n mesh workgroups
    te.task_payload[0] = g.constant(static_cast<double>(payload_val), sh, kir::DType::U32);    // one payload field (mesh reads v0)
    te.n_task_payload  = 1;
}

// B4: a TASK that passes a 3-FIELD payload (v0,v1,v2 = distinct uints) — the richer meshlet channel (a real amplification pass
// hands the mesh bounds / LOD / material, not one uint). Pairs with build_mesh_amplified_rgb.
inline void build_task_amplify_rgb(crd::kir::KGraph& g, crd::kir::KEntry& te, crd::u32 n, crd::u32 r, crd::u32 gr, crd::u32 b)
{
    namespace kir      = crd::kir;
    const auto sh      = kir::make_shape({1});
    const auto cu      = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, kir::DType::U32); };
    te.stage           = kir::KStage::Task;
    te.local_size[0]   = 1;
    te.task_emit       = cu(n);
    te.task_payload[0] = cu(r);  // field 0 → red
    te.task_payload[1] = cu(gr); // field 1 → green
    te.task_payload[2] = cu(b);  // field 2 → blue
    te.n_task_payload  = 3;
}

// B4: a MESH that reads the 3-FIELD payload (TaskPayload / TaskPayload1 / TaskPayload2 → v0,v1,v2) and colours ONE triangle by
// (v0,v1,v2)/255 as an RGB interpolant — the literal proof the multi-field task→mesh channel flows all three fields. Pairs
// with build_task_amplify_rgb + build_amplify_rgb_fs.
inline void build_mesh_amplified_rgb(crd::kir::KGraph& g, crd::kir::KEntry& me)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  tid = g.cast(g.builtin(kir::KBuiltin::LocalInvocationIndex), kir::DType::I32); // 0..2 = local vertex
    const int  pr  = g.cast(g.builtin(kir::KBuiltin::TaskPayload), kir::DType::F32);           // payload v0 → red
    const int  pg  = g.cast(g.builtin(kir::KBuiltin::TaskPayload1), kir::DType::F32);          // payload v1 → green
    const int  pb  = g.cast(g.builtin(kir::KBuiltin::TaskPayload2), kir::DType::F32);          // payload v2 → blue
    const int  eq0 = g.binary(kir::KOp::CmpEq, tid, g.constant(0.0, sh, kir::DType::I32));
    const int  eq1 = g.binary(kir::KOp::CmpEq, tid, g.constant(1.0, sh, kir::DType::I32));
    const int  x   = g.select(eq0, f(0.0), g.select(eq1, f(0.7), f(-0.7)));  // a big centred triangle
    const int  y   = g.select(eq0, f(0.7), f(-0.7));
    const int  inv = f(1.0 / 255.0);
    const int  u0  = g.constant(0.0, sh, kir::DType::U32);
    const int  u1  = g.constant(1.0, sh, kir::DType::U32);
    const int  u2  = g.constant(2.0, sh, kir::DType::U32);
    me.stage           = kir::KStage::Mesh;
    me.position        = g.vec4(x, y, f(0.0), f(1.0));
    me.mesh_vertices   = 3;
    me.mesh_primitives = 1;
    me.mesh_prim       = g.vec3(u0, u1, u2);
    me.n_out           = 3; // three scalar interpolants, one per payload field — proves all three fields flow independently
    me.out[0]          = {g.binary(kir::KOp::Mul, pr, inv), 0, kir::Interp::Smooth}; // v0/255 → red
    me.out[1]          = {g.binary(kir::KOp::Mul, pg, inv), 1, kir::Interp::Smooth}; // v1/255 → green
    me.out[2]          = {g.binary(kir::KOp::Mul, pb, inv), 2, kir::Interp::Smooth}; // v2/255 → blue
}

// B4: a plain MESH (no task/payload) where workgroup w renders ONE red triangle at x = -0.8 + w*0.2 — so dispatching K mesh
// workgroups tiles K triangles left→right. Used by the GPU-driven indirect dispatch: the compute cull decides K, and only K
// triangles appear. Pairs with build_amplify_fs (a scalar interpolant → red). Up to ~8 workgroups fit across the viewport.
inline void build_mesh_grid_tri(crd::kir::KGraph& g, crd::kir::KEntry& me)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  tid = g.cast(g.builtin(kir::KBuiltin::LocalInvocationIndex), kir::DType::I32); // 0..2 = local vertex
    const int  wg  = g.cast(g.builtin(kir::KBuiltin::WorkgroupIndex), kir::DType::F32);        // 0..K-1 = which meshlet
    const int  xc  = g.binary(kir::KOp::Add, f(-0.8), g.binary(kir::KOp::Mul, wg, f(0.2)));    // per-workgroup x centre
    const int  eq0 = g.binary(kir::KOp::CmpEq, tid, g.constant(0.0, sh, kir::DType::I32));
    const int  eq1 = g.binary(kir::KOp::CmpEq, tid, g.constant(1.0, sh, kir::DType::I32));
    const int  ox  = g.select(eq0, f(0.0), g.select(eq1, f(0.08), f(-0.08)));
    const int  oy  = g.select(eq0, f(0.6), f(-0.6));
    const int  x   = g.binary(kir::KOp::Add, xc, ox);
    const int  u0  = g.constant(0.0, sh, kir::DType::U32);
    const int  u1  = g.constant(1.0, sh, kir::DType::U32);
    const int  u2  = g.constant(2.0, sh, kir::DType::U32);
    me.stage           = kir::KStage::Mesh;
    me.position        = g.vec4(x, oy, f(0.0), f(1.0));
    me.mesh_vertices   = 3;
    me.mesh_primitives = 1;
    me.mesh_prim       = g.vec3(u0, u1, u2);
    me.n_out           = 1;
    me.out[0]          = {f(1.0), 0, kir::Interp::Smooth}; // constant red intensity → the FS red channel
}

// B4: FRAGMENT for the multi-field payload proof — colour = (v0,v1,v2)/255 from the three mesh scalar interpolants.
inline void build_amplify_rgb_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const int  r  = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, kir::Interp::Smooth); // v0/255
    const int  gr = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 1, kir::Interp::Smooth); // v1/255
    const int  b  = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 2, kir::Interp::Smooth); // v2/255
    const int  o  = g.constant(1.0, sh, kir::DType::F32);
    fe.stage      = kir::KStage::Fragment;
    fe.n_out      = 1;
    fe.out[0]     = {g.vec4(r, gr, b, o), 0};
}

// B4 MESH entry driven by a TASK: each MESH workgroup emits ONE triangle at x = -0.7 + WorkgroupIndex·0.45 (so N task-amplified
// workgroups tile N triangles across the screen), coloured by the TASK PAYLOAD (payload/255 → red interpolant to the FS) — the
// literal proof that the task→mesh data channel (KBuiltin::TaskPayload) flows. Pairs with build_task_amplify + build_amplify_fs.
inline void build_mesh_amplified_tri(crd::kir::KGraph& g, crd::kir::KEntry& me)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  tid = g.cast(g.builtin(kir::KBuiltin::LocalInvocationIndex), kir::DType::I32); // 0..2 = local vertex
    const int  wg  = g.cast(g.builtin(kir::KBuiltin::WorkgroupIndex), kir::DType::F32);        // 0..N-1 = which meshlet
    const int  pay = g.cast(g.builtin(kir::KBuiltin::TaskPayload), kir::DType::F32);           // the task→mesh payload
    const int  xc  = g.binary(kir::KOp::Add, f(-0.7), g.binary(kir::KOp::Mul, wg, f(0.45)));   // per-workgroup x centre
    const int  eq0 = g.binary(kir::KOp::CmpEq, tid, g.constant(0.0, sh, kir::DType::I32));
    const int  eq1 = g.binary(kir::KOp::CmpEq, tid, g.constant(1.0, sh, kir::DType::I32));
    const int  ox  = g.select(eq0, f(0.0), g.select(eq1, f(0.16), f(-0.16)));                  // triangle x corners
    const int  oy  = g.select(eq0, f(0.6), f(-0.6));                                           // apex top, base bottom
    const int  x   = g.binary(kir::KOp::Add, xc, ox);
    const int  col = g.binary(kir::KOp::Mul, pay, f(1.0 / 255.0)); // payload/255 → red intensity (the payload proof)
    const int  u0  = g.constant(0.0, sh, kir::DType::U32);
    const int  u1  = g.constant(1.0, sh, kir::DType::U32);
    const int  u2  = g.constant(2.0, sh, kir::DType::U32);

    me.stage           = kir::KStage::Mesh;
    me.position        = g.vec4(x, oy, f(0.0), f(1.0));
    me.mesh_vertices   = 3;
    me.mesh_primitives = 1;
    me.mesh_prim       = g.vec3(u0, u1, u2);
    me.n_out           = 1;
    me.out[0]          = {col, 0, kir::Interp::Smooth}; // payload/255 → the FS red channel
}

// B4 FRAGMENT for the amplification proof: colour = (payload/255, 0, 0, 1) from the mesh interpolant — so the rendered red
// intensity IS the payload the task passed. Pairs with build_mesh_amplified_tri.
inline void build_amplify_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const int  r  = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, kir::Interp::Smooth); // payload/255
    const int  z  = g.constant(0.0, sh, kir::DType::F32);
    const int  o  = g.constant(1.0, sh, kir::DType::F32);
    fe.stage      = kir::KStage::Fragment;
    fe.n_out      = 1;
    fe.out[0]     = {g.vec4(r, z, z, o), 0};
}

// B4-tess VERTEX: the 4 corners of a quad patch (±0.6 clip), indexed by VertexIndex — the control points a tessellation hull
// consumes. Drawn as ONE 4-control-point patch (draw_tess, patch_count = 1). Pairs with build_tess_hull + build_tess_domain.
inline void build_tess_quad_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  vid = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::I32); // 0..3 = the patch corner
    const int  e0  = g.binary(kir::KOp::CmpEq, vid, g.constant(0.0, sh, kir::DType::I32));
    const int  e1  = g.binary(kir::KOp::CmpEq, vid, g.constant(1.0, sh, kir::DType::I32));
    const int  e2  = g.binary(kir::KOp::CmpEq, vid, g.constant(2.0, sh, kir::DType::I32));
    const int  x   = g.select(e0, f(-0.6), g.select(e1, f(0.6), g.select(e2, f(0.6), f(-0.6)))); // 0/3 = -x · 1/2 = +x
    const int  y   = g.select(e0, f(-0.6), g.select(e1, f(-0.6), g.select(e2, f(0.6), f(0.6))));  // 0/1 = -y · 2/3 = +y
    ve.stage       = kir::KStage::Vertex;
    ve.position    = g.vec4(x, y, f(0.0), f(1.0));
    ve.n_out       = 0;
}

// B4-tess TESS-CONTROL (hull): a 4-control-point quad patch subdivided 8×8 (inner + outer levels = 8). Passthrough control
// points (the emitter copies them). Pairs with build_tess_quad_vs + build_tess_domain.
inline void build_tess_hull(crd::kir::KGraph& g, crd::kir::KEntry& te)
{
    namespace kir      = crd::kir;
    const auto sh      = kir::make_shape({1});
    te.stage           = kir::KStage::TessControl;
    te.tess_patch_size = 4;
    te.tess_inner      = g.constant(8.0, sh, kir::DType::F32);
    te.tess_outer      = g.constant(8.0, sh, kir::DType::F32);
}

// B4-tess TESS-EVAL (domain): reads TessPatchPosition (the emitter's bilerp of the 4 corners by gl_TessCoord) and EXPANDS the
// quad ×1.3 — a domain transform that a plain quad can't produce, so a pixel between the base edge (0.6) and the expanded edge
// (0.78) proves the domain shader ran per generated vertex. Pairs with build_tess_quad_vs + build_tess_hull + build_triangle_fs.
inline void build_tess_domain(crd::kir::KGraph& g, crd::kir::KEntry& te)
{
    namespace kir      = crd::kir;
    const auto sh      = kir::make_shape({1});
    const auto f       = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  patch   = g.builtin(kir::KBuiltin::TessPatchPosition);   // vec4 bilerp of the patch corners
    const int  scale   = g.vec4(f(1.3), f(1.3), f(1.0), f(1.0));        // expand xy, keep z/w
    te.stage           = kir::KStage::TessEval;
    te.tess_patch_size = 4;
    te.position        = g.binary(kir::KOp::Mul, patch, scale);
    te.n_out           = 0;
}

// B4-vis-4 VERTEX: a FULLSCREEN QUAD (6 vertices → 2 triangles) from VertexIndex — tri 0 (verts 0,1,2) tiles one screen half,
// tri 1 (verts 3,4,5) the other, so the HW rasterizer covers every pixel and each half carries a distinct gl_PrimitiveID.
inline void build_visbuffer_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  vid = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::I32); // 0..5
    const auto eqi = [&](int v) { return g.binary(kir::KOp::CmpEq, vid, g.constant(static_cast<double>(v), sh, kir::DType::I32)); };
    // x = +1 for verts {1,2,4}, else -1;  y = +1 for verts {2,4,5}, else -1 — a nested select chain (SROA-safe, no bool ops).
    const int x = g.select(eqi(1), f(1.0), g.select(eqi(2), f(1.0), g.select(eqi(4), f(1.0), f(-1.0))));
    const int y = g.select(eqi(2), f(1.0), g.select(eqi(4), f(1.0), g.select(eqi(5), f(1.0), f(-1.0))));
    ve.stage    = kir::KStage::Vertex;
    ve.position    = g.vec4(x, y, f(0.5), f(1.0));
    ve.n_out       = 0;
}

// B4-vis-4 FRAGMENT: write the VISIBILITY ID = gl_PrimitiveID (KBuiltin::PrimitiveId) as a uint to a R32_UINT target. The HW
// rasterizer sets the primitive id per covered pixel — the visibility buffer a deferred pass materializes. (A full pipeline
// packs (instanceId << 16) | primId; instance id would arrive as a flat VS→FS interpolant — here a single instance ⇒ primId.)
inline void build_visbuffer_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const int prim = g.cast(g.builtin(kir::KBuiltin::PrimitiveId), kir::DType::U32); // SV_PrimitiveID / gl_PrimitiveID
    fe.stage       = kir::KStage::Fragment;
    fe.n_out       = 1;
    fe.out[0]      = {prim, 0};
}

// B1-a FRAGMENT entry: output the screen-space derivatives of `FragCoord.x` as a colour. `FragCoord.x` increases by exactly
// 1 per pixel horizontally, so `dFdx(FragCoord.x) == 1.0` and `dFdy(FragCoord.x) == 0.0` on EVERY covered pixel and EVERY
// backend — a deterministic, hardware-independent derivative test. Colour = (dFdx, dFdy, 0, 1) ⇒ centre reads R≈1, G≈0.
inline void build_derivative_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int fc   = g.builtin(kir::KBuiltin::FragCoord); // vec4 (gl_FragCoord / SV_Position)
    const int fcx  = g.swizzle(fc, 0);                    // .x  (a 1-wide swizzle is scalar)
    const int dx   = g.dfdx(fcx);                         // == 1.0
    const int dy   = g.dfdy(fcx);                         // == 0.0
    const int zero = g.constant(0.0, sh, kir::DType::F32);
    const int one  = g.constant(1.0, sh, kir::DType::F32);
    const int col  = g.vec4(dx, dy, zero, one);

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B16-a-4 WATER SURFACE render: a fullscreen water quad whose normal tilts smoothly with FragCoord (a gentle bulge), so the
// Fresnel + sun glint sweep across the frame — screen centre reflects up (deep water), a bright sun-glint appears where the
// tilted normal reflects the sun into the eye. Runs `water_shade` on the GPU, Reinhard-tonemapped to [0,1] for the unorm8
// target. The readback proves it renders (a bluish body + a bright glint); drawn identically on Vulkan + DX12 it proves the
// fragment shading is portable.
inline void build_water_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace wt  = crd::kir::water;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc   = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const auto unit = [&](int vv) { return nd::detail::bin(g, kir::KOp::Div, vv, g.unary(kir::KOp::Sqrt, g.dot(vv, vv))); };

    const int fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int invd = k(1.0 / static_cast<double>(dim));
    const int tx   = g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.swizzle(fc, 0), invd), k(0.5)), k(2.6));
    const int tz   = g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.swizzle(fc, 1), invd), k(0.5)), k(2.6));
    const int n    = unit(g.vec3(tx, k(1.0), tz));
    const int v    = unit(kc(0.0, 0.75, 0.66));
    const int l    = unit(kc(0.35, 0.55, 0.75));
    const int col  = wt::water_shade(g, n, v, l, kc(5.0, 4.8, 4.2), kc(0.25, 0.45, 0.8), kc(0.08, 0.14, 0.12),
                                     kc(0.0, 0.06, 0.1), kc(0.45, 0.09, 0.06), k(3.0), k(0.3), k(0.0), kc(0.9, 0.92, 0.95), k(0.08));
    const int tm   = nd::detail::bin(g, kir::KOp::Div, col, nd::detail::bin(g, kir::KOp::Add, col, kc(1.0, 1.0, 1.0))); // Reinhard → [0,1]
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(tm, 0), g.swizzle(tm, 1), g.swizzle(tm, 2), k(1.0)), 0};
}

// B16-a-4 OCEAN FRAME: a full perspective sea to the horizon, shaded with the water library. A camera above the water casts a
// ray per pixel; below the horizon the ray hits the y=0 plane, a multi-directional sum-of-waves gives the surface normal +
// crest height, and the pixel is shaded — Fresnel-dielectric split of a sky reflection (sky_color of the reflected ray) + the
// slope-variance sun glitter vs deep Beer-absorbed water + a green subsurface glow, foam on the sharp crests, then distance
// fog fading the far water into the horizon haze. Above the horizon the ray shows the sky. Reinhard + sRGB for display. `time`
// animates the waves. This is a VISUAL frame (written to a BMP), not a bit-exact gate.
inline void build_ocean_frame_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim, double time)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace wt  = crd::kir::water;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc    = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const auto add   = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub   = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul   = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto b3    = [&](kir::KOp op, int a, int c) { return nd::detail::bin(g, op, a, c); };
    const auto sat   = [&](int x) { return g.binary(kir::KOp::Max, g.binary(kir::KOp::Min, x, k(1.0)), k(0.0)); };
    const auto sat3  = [&](int x) { return b3(kir::KOp::Max, b3(kir::KOp::Min, x, kc(1.0, 1.0, 1.0)), kc(0.0, 0.0, 0.0)); };
    const auto unit3 = [&](int vv) { return b3(kir::KOp::Div, vv, g.unary(kir::KOp::Sqrt, g.dot(vv, vv))); };

    // camera ray: top looks up (sky), bottom looks down (near water); horizon a touch above centre.
    const int fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int invd = k(2.0 / static_cast<double>(dim));
    const int ux   = sub(mul(g.swizzle(fc, 0), invd), k(1.0));
    const int uy   = sub(k(1.0), mul(g.swizzle(fc, 1), invd)); // flip: FragCoord.y is top-down
    const int rdir = unit3(g.vec3(mul(ux, k(0.9)), add(k(-0.14), mul(uy, k(0.60))), k(1.0)));
    const int ry   = g.swizzle(rdir, 1);

    // a LOW, warm, golden-hour sun near the horizon (lever 3).
    const int ldir  = unit3(kc(0.20, 0.10, 0.97));
    const int sunc  = kc(7.0, 4.0, 1.8);    // warm sun radiance (HDR)
    const int horiz = kc(0.55, 0.36, 0.22); // warm hazy horizon (dimmer ⇒ deep water reads dark)
    const int zen   = kc(0.03, 0.07, 0.18); // deep zenith blue

    // ── SKY with atmospheric in-scattering toward the sun ──
    const auto sky_of = [&](int dir) {
        const int ty   = sat(g.swizzle(dir, 1));
        const int grad = add(b3(kir::KOp::Mul, horiz, sub(k(1.0), ty)), b3(kir::KOp::Mul, zen, ty));
        const int sd   = g.binary(kir::KOp::Max, g.dot(dir, ldir), k(0.0));
        const int disk = g.unary(kir::KOp::Exp, mul(sub(sd, k(1.0)), k(700.0)));  // sun disk
        const int glow = g.unary(kir::KOp::Exp, mul(sub(sd, k(1.0)), k(12.0)));   // tight warm scatter
        const int haze = g.unary(kir::KOp::Exp, mul(sub(sd, k(1.0)), k(1.6)));    // broad horizon spill
        return add(grad, b3(kir::KOp::Mul, sunc, add(mul(disk, k(3.0)), add(mul(glow, k(0.6)), mul(haze, k(0.18))))));
    };
    const int sky = sky_of(rdir);

    // ── WATER: ray·plane y=0 at eye height 5 ──
    const int tdist = g.binary(kir::KOp::Div, k(5.0), g.binary(kir::KOp::Max, g.unary(kir::KOp::Neg, ry), k(1e-3)));
    const int wx    = mul(tdist, g.swizzle(rdir, 0));
    const int wzr   = mul(tdist, g.swizzle(rdir, 2));

    // a BIG dominant swell + octaves → slope (sx,sz) + crest height hh (lever 2, "big wave").
    int          sx = k(0.0);
    int          sz = k(0.0);
    int          hh = k(0.0);
    struct Wave { double dx, dz, kk, amp, spd; };
    const Wave   waves[7] = {{1.0, 0.22, 0.045, 1.5, 0.42}, {0.85, 0.5, 0.10, 0.7, 0.55}, {0.5, 0.9, 0.20, 0.36, 0.72},
                             {-0.6, 0.8, 0.40, 0.19, 0.95}, {0.9, -0.4, 0.78, 0.10, 1.2}, {0.3, 1.0, 1.5, 0.05, 1.55}, {-0.7, 0.6, 2.7, 0.025, 1.95}};
    for (const Wave& wv : waves)
    {
        const double dl = std::sqrt(wv.dx * wv.dx + wv.dz * wv.dz);
        const int    ph = add(mul(add(mul(wx, k(wv.kk * wv.dx / dl)), mul(wzr, k(wv.kk * wv.dz / dl))), k(1.0)), k(wv.spd * time));
        const int    cs = g.unary(kir::KOp::Cos, ph);
        hh              = add(hh, mul(k(wv.amp), g.unary(kir::KOp::Sin, ph)));
        sx              = add(sx, mul(k(wv.amp * wv.kk * wv.dx / dl), cs));
        sz              = add(sz, mul(k(wv.amp * wv.kk * wv.dz / dl), cs));
    }
    const int n = unit3(g.vec3(g.unary(kir::KOp::Neg, sx), k(1.0), g.unary(kir::KOp::Neg, sz)));
    const int v = g.unary(kir::KOp::Neg, rdir);

    // Fresnel-dielectric split: sky reflection + slope-variance sun glitter vs deep water + STRONG subsurface (lever 1 + scatter).
    const int nov  = g.binary(kir::KOp::Max, g.dot(n, v), k(1e-3));
    const int fr   = wt::fresnel_water(g, nov);
    const int rfl  = sub(rdir, b3(kir::KOp::Mul, n, mul(k(2.0), g.dot(rdir, n))));
    const int skyr = sky_of(unit3(rfl));
    const int glit = wt::ocean_sun_glitter(g, n, v, ldir, sunc, k(0.015));
    const int refl = add(skyr, glit);

    const int deep = kc(0.006, 0.10, 0.13); // deep teal (lever 1)
    const int vdl  = sat(g.dot(v, g.unary(kir::KOp::Neg, ldir)));
    const int sssf = mul(sat(add(mul(hh, k(0.5)), k(0.5))), mul(mul(vdl, vdl), mul(vdl, vdl))); // backlit high crests glow
    const int refr = add(deep, b3(kir::KOp::Mul, kc(0.10, 0.55, 0.42), mul(sssf, k(0.6))));     // teal-green scatter

    // FOAM on the sharp crests, lit by sun + sky (foams on the water).
    const int foam   = sat(mul(sub(hh, k(0.9)), k(1.3)));
    const int foamc  = b3(kir::KOp::Mul, kc(0.9, 0.94, 0.98), add(mul(sat(g.dot(n, ldir)), k(0.8)), k(0.5)));
    const int water0 = add(b3(kir::KOp::Mul, refl, fr), b3(kir::KOp::Mul, refr, sub(k(1.0), fr)));
    const int water1 = add(b3(kir::KOp::Mul, water0, sub(k(1.0), foam)), b3(kir::KOp::Mul, foamc, foam));

    // gentle warm aerial fog to the horizon (lever 1: less haze than before, warm-tinted).
    const int fog   = sat(sub(k(1.0), g.unary(kir::KOp::Exp, mul(tdist, k(-0.0016)))));
    const int water = add(b3(kir::KOp::Mul, water1, sub(k(1.0), fog)), b3(kir::KOp::Mul, horiz, fog));

    // sky vs water, EXPOSURE, filmic (Narkowicz ACES) tonemap + sRGB.
    const int col  = nd::detail::sel(g, g.binary(kir::KOp::CmpLt, ry, k(0.0)), water, sky);
    const int ce   = b3(kir::KOp::Mul, col, k(0.16)); // exposure — keep water deep, let only sun/glint/sky reach the top
    const int num  = b3(kir::KOp::Mul, ce, b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, k(2.51)), k(0.03)));
    const int den  = b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, k(2.43)), k(0.59))), k(0.14));
    const int aces = sat3(b3(kir::KOp::Div, num, den));
    const int srgb = b3(kir::KOp::Pow, aces, k(1.0 / 2.2));
    fe.stage       = kir::KStage::Fragment;
    fe.n_out       = 1;
    fe.out[0]      = {g.vec4(g.swizzle(srgb, 0), g.swizzle(srgb, 1), g.swizzle(srgb, 2), k(1.0)), 0};
}

// B16-a-4 LEVER 4 — the REAL FFT-OCEAN frame. The surface is no longer a handful of procedural sines (which interfere into a
// visible parallelogram lattice) — it is OUR FFT-ocean field, baked into a texture (R,G = normal.xz · B = height · A = Jacobian
// foam) and SAMPLED here. A camera above the sea casts a ray per pixel; below the horizon it hits the y=0 plane, then:
//   (1) EXACT single-step PARALLAX — sample the height at the plane hit, shift the sample point to where the ray truly meets
//       that height (Δt = h/ray.y). Near crests displace toward the eye → real 3-D wave depth (not a flat normal map).
//   (2) MULTI-SCALE detail — the same periodic FFT tile is sampled at three world scales (rotated) and the slopes summed, so
//       the eye never sees the tile repeat AND the spectrum spans metres-to-centimetres of wavelength (kills the lattice).
//   (3) shade with the water library — Fresnel split of a sky reflection + slope-variance sun glitter vs deep Beer-absorbed
//       teal + a green subsurface glow on backlit crests, REAL Jacobian foam on the pinched crests, distance fog, ACES+sRGB.
// `hmax` decodes the height channel ((B−0.5)·2·hmax); `patch` is the FFT tile's world size. The texture is bound by draw_textured
// (set 0 / binding 1 image + binding 2 sampler). This is a VISUAL frame (→ BMP), driven by the actual FFT simulation.
inline void build_ocean_frame_fft_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim, double hmaxS, double hmaxC,
                                     double patchS, double patchC)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace nz  = crd::kir::nodes::noise;
    namespace wt  = crd::kir::water;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc    = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const auto add   = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto sub   = [&](int a, int b) { return g.binary(kir::KOp::Sub, a, b); };
    const auto mul   = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const auto b3    = [&](kir::KOp op, int a, int c) { return nd::detail::bin(g, op, a, c); };
    const auto sat   = [&](int x) { return g.binary(kir::KOp::Max, g.binary(kir::KOp::Min, x, k(1.0)), k(0.0)); };
    const auto sat3  = [&](int x) { return b3(kir::KOp::Max, b3(kir::KOp::Min, x, kc(1.0, 1.0, 1.0)), kc(0.0, 0.0, 0.0)); };
    const auto unit3 = [&](int vv) { return b3(kir::KOp::Div, vv, g.unary(kir::KOp::Sqrt, g.dot(vv, vv))); };

    // TWO cascade fields in a bindless array (index 0 = big SWELL, index 1 = fine CHOP), each RGBA8: R,G=normal.xz · B=height · A=foam.
    const int tex  = g.texture(0, 3, kir::DType::F32, kir::TexDim::Tex2D, false, false, false, /*array_count=*/8);
    const int samp = g.sampler(0, 2);
    const int csw  = g.constant(0.0, sh, kir::DType::U32); // swell cascade index
    const int cch  = g.constant(1.0, sh, kir::DType::U32); // chop cascade index

    // camera ray: top looks up (sky), bottom looks down (near water); horizon a touch above centre.
    const int fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int invd = k(2.0 / static_cast<double>(dim));
    const int ux   = sub(mul(g.swizzle(fc, 0), invd), k(1.0));
    const int uy   = sub(k(1.0), mul(g.swizzle(fc, 1), invd)); // flip: FragCoord.y is top-down
    const int rdir = unit3(g.vec3(mul(ux, k(0.9)), add(k(-0.14), mul(uy, k(0.60))), k(1.0)));
    const int ry   = g.swizzle(rdir, 1);
    const int rdx  = g.swizzle(rdir, 0);
    const int rdz  = g.swizzle(rdir, 2);

    // a HIGH, soft daytime sun — bright hazy sky, no harsh disk (AC4 tropical daylight).
    const int ldir  = unit3(kc(0.25, 0.28, 0.93)); // daytime sun, low enough to sit in the upper frame → rays leak from clouds
    const int sunc  = kc(2.4, 2.5, 2.4);    // soft near-white sun (broad sheen, not an orange blob)
    const int hazeh = kc(0.52, 0.67, 0.80); // hazy blue horizon
    const int zen   = kc(0.13, 0.40, 0.78); // richer blue zenith

    // ── SKY: a bright hazy-blue daytime dome (pale toward the horizon), a soft broad sun sheen, bright white cumulus. ──
    crd::kir::clouds::CloudConfig ccfg;
    ccfg.coverage    = 0.66; // fuller cover — thicker, more-defined masses with clear-sky gaps the sun rays leak through
    ccfg.base_freq   = 2.2;
    ccfg.detail_freq = 7.0;
    ccfg.erosion     = 0.42;
    const auto sky_of = [&](int dir, bool hi) {
        const int ty   = g.unary(kir::KOp::Sqrt, sat(g.swizzle(dir, 1))); // bias gradient toward the pale horizon
        const int grad = add(b3(kir::KOp::Mul, hazeh, sub(k(1.0), ty)), b3(kir::KOp::Mul, zen, ty));
        const int sd   = g.binary(kir::KOp::Max, g.dot(dir, ldir), k(0.0));
        const int disk = g.unary(kir::KOp::Exp, mul(sub(sd, k(1.0)), k(280.0))); // sun disc — bright enough to LEAK through cloud gaps
        const int glow = g.unary(kir::KOp::Exp, mul(sub(sd, k(1.0)), k(5.0)));   // broad soft sun haze
        const int suns = b3(kir::KOp::Mul, sunc, add(mul(disk, k(1.8)), mul(glow, k(0.4))));
        // bright white cumulus (bounded coords → no horizon aliasing).
        const int cy    = add(g.swizzle(dir, 1), k(0.5));
        const int cu    = mul(g.binary(kir::KOp::Div, g.swizzle(dir, 0), cy), k(0.7));
        const int cw    = mul(g.binary(kir::KOp::Div, g.swizzle(dir, 2), cy), k(0.7));
        const int hmask = sat(add(mul(g.swizzle(dir, 1), k(2.0)), k(0.12))); // clouds reach lower toward the horizon (background)
        const int pf    = nz::fractal2(g, cu, cw, 4, 2.0, 0.55);
        const int pf01  = sat(mul(add(pf, k(1.0)), k(0.5)));
        const int carv  = sat(mul(sub(pf01, k(1.0 - ccfg.coverage)), k(4.6)));  // THICK, defined cloud masses (partial cover)
        int       dens  = carv;
        if (hi) // light erosion only → thick clouds, not thin wisps
        {
            const int det = nz::fractal2(g, mul(cu, k(3.3)), mul(cw, k(3.3)), 3, 2.0, 0.5);
            dens          = sat(sub(carv, mul(sat(mul(add(det, k(1.0)), k(0.5))), k(0.08))));
        }
        const int cov    = mul(dens, hmask);
        // VOLUME shading: a DENSITY-driven self-shadow (thick cloud → darker underside) + a grey base ⇒ defined, three-dimensional
        // clouds that read DARKER and more VISIBLE against the bright hazy sky (the user's ask), while the thin edges stay sunlit.
        const int clit   = add(mul(sat(g.swizzle(dir, 1)), k(1.15)), k(0.34)); // sky-height brightness
        const int cshad  = sub(k(1.0), mul(dens, k(0.58)));                    // thick cores self-shadow → darker undersides
        const int cloudc = b3(kir::KOp::Mul, kc(0.96, 1.02, 1.14), mul(clit, cshad));
        const int skyc   = add(grad, suns);
        return add(b3(kir::KOp::Mul, skyc, sub(k(1.0), cov)), b3(kir::KOp::Mul, cloudc, cov)); // clouds occlude the sky
    };
    const int sky = sky_of(rdir, true);

    // ── WATER: the real Tessendorf MULTI-CASCADE — sample the SWELL (index 0, world/patchS: long "tidal" rollers) and the CHOP
    //    (index 1, world/patchC: fine detail) and SUM their slopes. No single-tile compromise between swell and chop. ──
    const auto tapc = [&](int idx, int wxv, int wzv, double sc) {
        const int uv = g.vec2(mul(wxv, k(1.0 / sc)), mul(wzv, k(1.0 / sc)));
        return g.tex_sample_at(tex, samp, uv, idx); // vec4: R=nx01 G=nz01 B=h01 A=foam
    };
    const auto nxof   = [&](int t) { return sub(mul(g.swizzle(t, 0), k(2.0)), k(1.0)); };
    const auto nzof   = [&](int t) { return sub(mul(g.swizzle(t, 1), k(2.0)), k(1.0)); };
    const auto dh_s    = [&](int t) { return mul(sub(g.swizzle(t, 2), k(0.5)), k(2.0 * hmaxS)); }; // swell height decode
    const auto dh_c    = [&](int t) { return mul(sub(g.swizzle(t, 2), k(0.5)), k(2.0 * hmaxC)); }; // chop height decode
    const auto distfd = [&](int t) { return sat(mul(sub(t, k(30.0)), k(0.005))); };
    // fog + distance-fade key off the SMOOTH analytic plane distance (monotonic across the screen ⇒ no grazing stripes).
    const int tplane = g.binary(kir::KOp::Div, k(5.0), g.binary(kir::KOp::Max, g.unary(kir::KOp::Neg, ry), k(0.05)));
    const int distf  = distfd(tplane);
    const int wx0    = mul(tplane, rdx);
    const int wz0    = mul(tplane, rdz);
    // single-step parallax off the COMBINED (swell + chop) plane-position height → real 3-D relief, stable derivative.
    const int ryc = g.binary(kir::KOp::Min, ry, k(-0.12));
    const int h0  = add(dh_s(tapc(csw, wx0, wz0, patchS)), dh_c(tapc(cch, wx0, wz0, patchC)));
    const int dt  = g.binary(kir::KOp::Div, h0, ryc);
    const int wx  = add(wx0, mul(dt, rdx));
    const int wz  = add(wz0, mul(dt, rdz));

    // sample both cascades at the parallaxed position → summed slopes (swell rollers + chop), combined height + foam.
    const int ts   = tapc(csw, wx, wz, patchS);  // swell
    const int tc   = tapc(cch, wx, wz, patchC);  // chop
    const int slx  = add(mul(nxof(ts), k(1.5)), mul(nxof(tc), k(0.22))); // SWELL rollers make the shape; chop is a faint texture,
    const int slz  = add(mul(nzof(ts), k(1.5)), mul(nzof(tc), k(0.22))); //  NOT amplified into per-pixel noise (the "white noise")
    const int gslf = mul(k(1.5), sub(k(1.0), mul(distf, k(0.3))));       // gentle slope gain → smooth directional swells
    const int n    = unit3(g.vec3(mul(slx, gslf), k(1.0), mul(slz, gslf)));
    const int hh   = add(dh_s(ts), dh_c(tc)); // combined height (swell rollers + chop) for depth/subsurface
    const int v    = g.unary(kir::KOp::Neg, rdir);

    // Fresnel split: bright-sky reflection + a soft BROAD daytime sheen vs a bright tropical-teal body (greener/shallower up
    // close, deepening to blue with distance) + a teal subsurface glow.
    const int nov  = g.binary(kir::KOp::Max, g.dot(n, v), k(1e-3));
    const int fr   = wt::fresnel_water(g, nov);
    const int rfl  = sub(rdir, b3(kir::KOp::Mul, n, mul(k(2.0), g.dot(rdir, n))));
    const int skyr = sky_of(unit3(rfl), false);
    const int glit = wt::ocean_sun_glitter(g, n, v, ldir, sunc, add(k(0.025), mul(distf, k(0.06)))); // soft broad daytime sheen
    const int refl = add(b3(kir::KOp::Mul, skyr, k(0.68)), b3(kir::KOp::Mul, glit, k(0.4))); // damp mirror + gentle sheen (soft day)

    // tropical body: bright teal-green shallow water up close, deepening to blue with distance (Beer depth), + a per-wave DEPTH
    // gradient (troughs darker/richer, crests brighter) for the AC4-style richness.
    // OCEAN BLUE body (deep blue in the troughs, a touch lighter far), + a TURQUOISE subsurface glow only where the low sun
    //  BACKLIGHTS a raised crest (the light scatters through the thin water) — blue sea, turquoise where the sun lights it.
    const int deep0  = add(kc(0.012, 0.055, 0.16), b3(kir::KOp::Mul, kc(0.02, 0.05, 0.07), sub(k(1.0), distf)));
    const int depthv = add(k(0.75), mul(sat(add(mul(hh, k(0.5)), k(0.5))), k(0.45))); // trough darker, crest brighter
    const int deep   = b3(kir::KOp::Mul, deep0, depthv);
    const int vdl    = sat(g.dot(v, g.unary(kir::KOp::Neg, ldir)));
    const int sssf   = mul(sat(sub(mul(hh, k(1.3)), k(0.15))), mul(vdl, vdl)); // ONLY raised + sun-backlit crests (selective)
    const int refr0  = add(deep, b3(kir::KOp::Mul, kc(0.0, 0.42, 0.40), mul(sssf, k(1.1)))); // TURQUOISE sun-scatter on lit crests
    // VORONOI CAUSTICS — the shallow-water light NETWORK. Two octaves of Worley (Voronoi) cells; bright near the cell centres
    // (light focusing), warped by the wave slope so it dances on the crests, and faded to the near/clear water only. This is the
    // "voronoi in shallow water" — a real, cheap CKIR Worley caustic added to the subsurface body.
    const int cx     = add(mul(wx, k(0.32)), mul(slx, k(0.4))); // slope-warp so the network moves with the waves
    const int cz     = add(mul(wz, k(0.32)), mul(slz, k(0.4)));
    const int wa     = g.unary(kir::KOp::Sqrt, nz::worley2(g, cx, cz, 1.0, 0, 0));
    const int wb     = g.unary(kir::KOp::Sqrt, nz::worley2(g, add(mul(cx, k(1.9)), k(3.1)), mul(cz, k(1.9)), 1.0, 0, 0));
    const int caust  = mul(g.binary(kir::KOp::Pow, sat(sub(k(1.0), wa)), k(3.5)), g.binary(kir::KOp::Pow, sat(sub(k(1.0), wb)), k(3.5)));
    const int caustf = mul(mul(caust, vdl), sub(k(1.0), distf)); // near/clear + sun-lit water only (subtle)
    const int refr   = add(refr0, b3(kir::KOp::Mul, kc(0.10, 0.42, 0.40), mul(caustf, k(0.7))));

    // REAL temporal foam (texture A = the accumulated breaking-crest foam: Jacobian J<0 injected, then lingering·decay). It is
    // ALREADY intermittent and at the wave tips, so we use it directly — no painted streaks — in PURE HDR WHITE (an LDR foam
    // colour darkens to a washed teal under exposure+ACES). Base cascade only, so it doesn't repeat at the finer sample scales.
    const int fa     = g.binary(kir::KOp::Max, g.swizzle(ts, 3), g.swizzle(tc, 3)); // foam where EITHER swell or chop crest breaks
    const int foam   = mul(sat(mul(fa, k(0.75))), sub(k(1.0), mul(distf, k(0.4))));
    const int foamc  = kc(4.4, 4.5, 4.55); // HDR pure white
    const int water0 = add(b3(kir::KOp::Mul, refl, fr), b3(kir::KOp::Mul, refr, sub(k(1.0), fr)));
    const int water1 = add(b3(kir::KOp::Mul, water0, sub(k(1.0), foam)), b3(kir::KOp::Mul, foamc, foam));

    // pale hazy aerial fog: a mild DISTANCE haze + a sharp ANGLE haze in the grazing horizon band (where isotropic mip filtering
    // can't resolve the extreme foreshortening → aliasing). The angle haze veils that band (a real hazy horizon) without fogging
    // the mid-field, so wave topology stays visible where it matters.
    const int fogc  = kc(0.70, 0.78, 0.85);
    const int fogd  = sat(sub(k(1.0), g.unary(kir::KOp::Exp, mul(tplane, k(-0.004)))));
    const int fogh  = sat(mul(sub(k(0.12), g.unary(kir::KOp::Neg, ry)), k(14.0)));
    const int fog   = g.binary(kir::KOp::Max, fogd, fogh);
    const int water = add(b3(kir::KOp::Mul, water1, sub(k(1.0), fog)), b3(kir::KOp::Mul, fogc, fog));

    // sky vs water, EXPOSURE, filmic (Narkowicz ACES) tonemap + sRGB.
    const int col  = nd::detail::sel(g, g.binary(kir::KOp::CmpLt, ry, k(0.0)), water, sky);
    const int ce   = b3(kir::KOp::Mul, col, k(0.42)); // daytime exposure (a touch down for saturated teal + contrast)
    const int num  = b3(kir::KOp::Mul, ce, b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, k(2.51)), k(0.03)));
    const int den  = b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, b3(kir::KOp::Add, b3(kir::KOp::Mul, ce, k(2.43)), k(0.59))), k(0.14));
    const int aces = sat3(b3(kir::KOp::Div, num, den));
    const int srgb = b3(kir::KOp::Pow, aces, k(1.0 / 2.2));
    fe.stage       = kir::KStage::Fragment;
    fe.n_out       = 1;
    fe.out[0]      = {g.vec4(g.swizzle(srgb, 0), g.swizzle(srgb, 1), g.swizzle(srgb, 2), k(1.0)), 0};
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// B16 DISPLACED-GEOMETRY ocean — the GOLD-STANDARD path. The FFT surface is carried by REAL displaced GEOMETRY (a Johanson
// "projected grid": a screen-space lattice raycast onto the water plane, then vertically displaced by the baked FFT height),
// not a per-pixel normal-map on a flat plane. That is what makes the difference the fragment path never can: crests genuinely
// occlude troughs (true silhouettes), the far field carries smooth directional SWELL geometry instead of minification "white
// noise", and the shading normal comes from the actual surface. The vertex shader has NO derivatives, so it fetches the
// bindless cascade textures at an EXPLICIT LOD (the new `SampleIndexedLod` op) — distance-ramped so the far chop mips away to
// smooth rollers. Camera EXACTLY matches build_ocean_frame_fft_fs (eye (0,5,0), rdir = normalize(ux·0.9, −0.14+uy·0.60, 1)),
// so a geometry pass composites pixel-aligned over the fragment pass's sky. Portable: pure vertex-pull (no mesh/tess ext), so
// it lowers to GLSL/HLSL today and to WGSL as soon as SampleIndexedLod gains a WGSL emit (WebGPU: a texture_2d_array + LOD).
//
// `grid` = cells per side (draws grid·grid·6 vertices — 2 triangles/cell from VertexIndex, no index buffer). The grid spans
// the WATER band of the screen (uy ∈ [−1, horizon]); above the horizon there is no geometry and the sky pass shows through.
//
// 4-CASCADE water (the production standard — Sea of Thieves / gasgiant): FOUR independent FFT spectra at descending world
// scales, band-limited so each owns a wavelength band. Combining 4 non-harmonic tile sizes pushes the visible tiling period to
// their LCM (effectively non-repeating). The BIG cascades carry HIGH-amplitude geometry (the rolling swell silhouette); the
// FINE cascades are LOW-amplitude and live mostly in the per-pixel normal (small detail, no geometry aliasing). Each cascade's
// baked RGBA8 = [nx, nz, height, fold] where fold = ½·(1−J) is the per-cascade Jacobian folding (J<1 ⇒ crest pinch); the FS
// SUMS the folds across cascades to get the JOINT Jacobian foam (folding of the combined 4-spectrum surface).
// PROMOTED to the engine — the reusable ocean render pass now lives in `crd::kir::water` (engine/kir/.../ckir_water_render.hpp).
// Aliased into `crd::gputest` so the test call sites are unchanged; the definitions are the engine's.
using crd::kir::water::OceanCascadeRender;
using crd::kir::water::ocean_projected_vertex;
using crd::kir::water::build_ocean_displaced_vs;
using crd::kir::water::build_ocean_displaced_mesh;
using crd::kir::water::build_ocean_water_geo_fs;


// B1-b FRAGMENT entry: a constant-red surface that DISCARDS (alpha-test / cutout) where `FragCoord.x < 16`. On a 32-px
// target this kills the left half of the covered triangle — those pixels keep the clear colour — while the right half
// paints red. Deterministic and backend-independent (the discard threshold is a screen coordinate).
inline void build_discard_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int fc     = g.builtin(kir::KBuiltin::FragCoord);
    const int fcx    = g.swizzle(fc, 0);                        // FragCoord.x
    const int thresh = g.constant(16.0, sh, kir::DType::F32);
    const int cond   = g.binary(kir::KOp::CmpLt, fcx, thresh);  // bool: FragCoord.x < 16 → discard

    const int one  = g.constant(1.0, sh, kir::DType::F32);
    const int zero = g.constant(0.0, sh, kir::DType::F32);
    const int red  = g.vec4(one, zero, zero, one);

    fe.stage        = kir::KStage::Fragment;
    fe.n_out        = 1;
    fe.out[0]       = {red, 0};
    fe.discard_cond = cond;
}

// B1-c FLAT-INTERPOLANT pair. An INTEGER varying cannot be smoothly interpolated — GLSL/HLSL reject it unless it is `flat`
// / `nointerpolation`. So this pair only compiles if the interpolation qualifier is actually emitted: the VS outputs a
// `flat int` payload (200) at location 0; the FS reads it, converts to a colour (200/255). The two sides MUST agree on the
// location, the int type, and the `flat` qualifier — exactly like a real VS→FS interface.
inline void build_flat_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    build_triangle_vs(g, ve); // clip position (sets n_out = 0)
    const auto sh      = kir::make_shape({1});
    const int  payload = g.constant(200.0, sh, kir::DType::I32); // an integer varying ⇒ must be flat
    ve.n_out  = 1;
    ve.out[0] = {payload, 0, kir::Interp::Flat};
}

inline void build_flat_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  in  = g.stage_in(kir::KType::make_scalar(kir::DType::I32), 0, kir::Interp::Flat); // flat int interpolant
    const int  f   = g.cast(in, kir::DType::F32);
    const int  inv = g.constant(1.0 / 255.0, sh, kir::DType::F32);
    const int  r   = g.binary(kir::KOp::Mul, f, inv); // 200/255 → unorm8 200
    const int  z   = g.constant(0.0, sh, kir::DType::F32);
    const int  one = g.constant(1.0, sh, kir::DType::F32);
    const int  col = g.vec4(r, z, z, one);

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B1-c NOPERSPECTIVE pair. A PERSPECTIVE triangle (the base NDC corners, but clip.w = {1, 4, 1} — vertex 1 is "far") makes
// perspective-correct interpolation diverge from screen-linear. The VS emits ONE per-vertex value V = {0, 0.9, 0} on TWO
// interpolants: location 0 `smooth` (perspective-correct, the default) and location 1 `noperspective` (screen-linear). The
// FS reads both and writes R = smooth, G = noperspective. At the centre they differ substantially (≈0.069 vs ≈0.225 →
// ≈40 in unorm8) — so the test BITES: if the `noperspective` qualifier were dropped, both interpolate perspective-correct
// and R == G. Screen-linear (noperspective) is IMMUNE to w, so its value is hardware-independent; smooth is well-defined too.
inline void build_noperspective_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int k0  = g.constant(0.0, sh, kir::DType::I32);
    const int k1  = g.constant(1.0, sh, kir::DType::I32);
    const int eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int eq1 = g.binary(kir::KOp::CmpEq, vid, k1);

    // NDC corners {(0,-0.8),(0.8,0.8),(-0.8,0.8)} — the base triangle, so it still covers the centre.
    const int nx0 = g.constant(0.0, sh, kir::DType::F32);
    const int nx1 = g.constant(0.8, sh, kir::DType::F32);
    const int nx2 = g.constant(-0.8, sh, kir::DType::F32);
    const int ny0 = g.constant(-0.8, sh, kir::DType::F32);
    const int ny1 = g.constant(0.8, sh, kir::DType::F32);
    const int ndc_x = g.select(eq0, nx0, g.select(eq1, nx1, nx2));
    const int ndc_y = g.select(eq0, ny0, ny1);

    // clip.w = 4 for vertex 1, else 1 → strong perspective. clip.xy = ndc * w so NDC = clip.xy / clip.w is unchanged.
    const int w4  = g.constant(4.0, sh, kir::DType::F32);
    const int w1  = g.constant(1.0, sh, kir::DType::F32);
    const int w   = g.select(eq1, w4, w1);
    const int cx  = g.binary(kir::KOp::Mul, ndc_x, w);
    const int cy  = g.binary(kir::KOp::Mul, ndc_y, w);
    const int z   = g.constant(0.0, sh, kir::DType::F32);
    const int pos = g.vec4(cx, cy, z, w);

    // per-vertex value V = 0.9 at the far vertex (1), else 0.
    const int v09 = g.constant(0.9, sh, kir::DType::F32);
    const int v0  = g.constant(0.0, sh, kir::DType::F32);
    const int val = g.select(eq1, v09, v0);

    ve.stage    = kir::KStage::Vertex;
    ve.position = pos;
    ve.n_out    = 2;
    ve.out[0]   = {val, 0, kir::Interp::Smooth};        // perspective-correct
    ve.out[1]   = {val, 1, kir::Interp::NoPerspective}; // screen-linear
}

inline void build_noperspective_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  smth = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, kir::Interp::Smooth);
    const int  nper = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 1, kir::Interp::NoPerspective);
    const int  z    = g.constant(0.0, sh, kir::DType::F32);
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  col  = g.vec4(smth, nper, z, one); // R = perspective-correct, G = screen-linear

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B1-c CENTROID pair (needs an MSAA target). The base triangle carries a STEEP per-vertex ramp V = {0, 0.9, 0} on two
// interpolants: location 0 `smooth` (sampled at the pixel CENTRE — which at an edge pixel may be OUTSIDE the triangle,
// extrapolating V beyond its vertex range) and location 1 `centroid` (sampled at the centroid of the COVERED samples,
// always inside). The FS writes R = smooth, G = centroid. Interior fully-covered pixels have R == G; at partially-covered
// EDGE pixels they diverge. The test scans for the max |R-G| — nonzero only if `centroid` is actually emitted (biting).
inline void build_centroid_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int k0  = g.constant(0.0, sh, kir::DType::I32);
    const int k1  = g.constant(1.0, sh, kir::DType::I32);
    const int eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int eq1 = g.binary(kir::KOp::CmpEq, vid, k1);

    // A SMALL triangle (NDC ±0.35) so the per-vertex ramp spans few pixels ⇒ a STEEP screen-space gradient, which
    // amplifies the (sub-pixel) centre-vs-centroid sample-location difference into a clearly-observable band of edge pixels.
    const int x0 = g.constant(0.0, sh, kir::DType::F32);
    const int x1 = g.constant(0.35, sh, kir::DType::F32);
    const int x2 = g.constant(-0.35, sh, kir::DType::F32);
    const int y0 = g.constant(-0.35, sh, kir::DType::F32);
    const int y1 = g.constant(0.35, sh, kir::DType::F32);
    const int x  = g.select(eq0, x0, g.select(eq1, x1, x2));
    const int y  = g.select(eq0, y0, y1);
    const int z  = g.constant(0.0, sh, kir::DType::F32);
    const int w  = g.constant(1.0, sh, kir::DType::F32); // no perspective — centroid is a sample-location axis, not w
    const int pos = g.vec4(x, y, z, w);

    const int v09 = g.constant(0.9, sh, kir::DType::F32);
    const int v0  = g.constant(0.0, sh, kir::DType::F32);
    const int val = g.select(eq1, v09, v0);

    ve.stage    = kir::KStage::Vertex;
    ve.position = pos;
    ve.n_out    = 2;
    ve.out[0]   = {val, 0, kir::Interp::Smooth};
    ve.out[1]   = {val, 1, kir::Interp::Centroid};
}

inline void build_centroid_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  smth = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, kir::Interp::Smooth);
    const int  cent = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 1, kir::Interp::Centroid);
    const int  z    = g.constant(0.0, sh, kir::DType::F32);
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  col  = g.vec4(smth, cent, z, one); // R = centre-sampled, G = centroid-sampled

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B1-c SAMPLE pair (needs an MSAA target). A FULL-SCREEN triangle (so every pixel is 100% covered — no coverage-based edge
// blending to pollute the result) carries a linear ramp V = 0.5·(x_ndc + 1) that crosses 0.5 exactly at the screen centre
// column. The FS thresholds it: R = (V > 0.5) ? 1 : 0. With `interp == Sample` the fragment stage runs PER SAMPLE and V is
// interpolated at each sample, so the samples of the centre column straddle the threshold → the AVERAGE-resolve produces an
// INTERMEDIATE grey (antialiased). With `interp == Smooth` the stage runs once per pixel at the centre → every pixel is a
// hard 0 or 255 (no intermediates). Drawing both and comparing the count of intermediate pixels is the biting `sample` gate.
inline void build_ramp_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve, crd::kir::Interp interp)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});

    const int vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int k0  = g.constant(0.0, sh, kir::DType::I32);
    const int k1  = g.constant(1.0, sh, kir::DType::I32);
    const int eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int eq1 = g.binary(kir::KOp::CmpEq, vid, k1);

    // Oversized fullscreen triangle {(-1,-1),(3,-1),(-1,3)} → covers the whole [-1,1] viewport with a single primitive.
    const int xm1 = g.constant(-1.0, sh, kir::DType::F32);
    const int x3  = g.constant(3.0, sh, kir::DType::F32);
    const int y3  = g.constant(3.0, sh, kir::DType::F32);
    const int x   = g.select(eq0, xm1, g.select(eq1, x3, xm1));
    const int y   = g.select(eq0, xm1, g.select(eq1, xm1, y3));
    const int z   = g.constant(0.0, sh, kir::DType::F32);
    const int w   = g.constant(1.0, sh, kir::DType::F32);
    const int pos = g.vec4(x, y, z, w);

    // V per vertex {A:0, B:1, C:1} ⇒ V = 0.25·(x_ndc + y_ndc) + 0.5: the V = 0.5 contour is the DIAGONAL x_ndc + y_ndc = 0.
    // A diagonal threshold necessarily cuts through many pixels' sample spans (unlike an axis-aligned one, which can fall
    // cleanly between pixel columns), so per-sample shading antialiases a whole diagonal band of pixels — robustly countable.
    const int v0  = g.constant(0.0, sh, kir::DType::F32);
    const int v1  = g.constant(1.0, sh, kir::DType::F32);
    const int val = g.select(eq0, v0, v1); // vertex 0 → 0, vertices 1 & 2 → 1

    ve.stage    = kir::KStage::Vertex;
    ve.position = pos;
    ve.n_out    = 1;
    ve.out[0]   = {val, 0, interp};
}

inline void build_step_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::kir::Interp interp)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  v    = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, interp);
    const int  half = g.constant(0.5, sh, kir::DType::F32);
    const int  gt   = g.binary(kir::KOp::CmpGt, v, half); // bool: V > 0.5
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  zero = g.constant(0.0, sh, kir::DType::F32);
    const int  r    = g.select(gt, one, zero);
    const int  col  = g.vec4(r, zero, zero, one);

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B1-d FULLSCREEN triangle at PRIMITIVE DEPTH 0 (clip.z = 0). Every pixel of the viewport is covered, and WITHOUT a
// fragment `frag_depth` write each fragment's depth is 0.0 — so a depth buffer cleared to 0.5 with a LessEqual test would
// pass everywhere. Anything that then fails the test can ONLY be a shader-written depth. Used by the frag-depth + early-Z
// tests. (Same oversized {(-1,-1),(3,-1),(-1,3)} triangle as build_ramp_vs, but no interpolants.)
inline void build_fullscreen_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int  k0  = g.constant(0.0, sh, kir::DType::I32);
    const int  k1  = g.constant(1.0, sh, kir::DType::I32);
    const int  eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int  eq1 = g.binary(kir::KOp::CmpEq, vid, k1);
    const int  xm1 = g.constant(-1.0, sh, kir::DType::F32);
    const int  x3  = g.constant(3.0, sh, kir::DType::F32);
    const int  y3  = g.constant(3.0, sh, kir::DType::F32);
    const int  x   = g.select(eq0, xm1, g.select(eq1, x3, xm1));
    const int  y   = g.select(eq0, xm1, g.select(eq1, xm1, y3));
    const int  z   = g.constant(0.0, sh, kir::DType::F32); // primitive depth 0
    const int  w   = g.constant(1.0, sh, kir::DType::F32);

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, z, w);
    ve.n_out    = 0;
}

// B1-d FRAG-DEPTH fragment: paints red AND writes gl_FragDepth = FragCoord.x / 32 (a left→right ramp crossing 0.5 at
// x = 16 on a 32-px target). Drawn over `build_fullscreen_vs` into a depth buffer cleared to 0.5 with a LessEqual test:
// the left half (written depth ≤ 0.5) PASSES → red; the right half FAILS → the clear colour shows. Since the primitive
// depth is 0 (would pass everywhere), the right-half cut appears ONLY because the shader WROTE depth — the biting gate.
// The caller sets `depth_mode`: the ramp only ever raises depth above the primitive's 0, so the `Greater` promise holds.
inline void build_fragdepth_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  fcx   = g.swizzle(fc, 0);                          // FragCoord.x
    const int  inv   = g.constant(1.0 / 32.0, sh, kir::DType::F32);
    const int  depth = g.binary(kir::KOp::Mul, fcx, inv);         // FragCoord.x / 32
    const int  one   = g.constant(1.0, sh, kir::DType::F32);
    const int  zero  = g.constant(0.0, sh, kir::DType::F32);
    const int  red   = g.vec4(one, zero, zero, one);

    fe.stage      = kir::KStage::Fragment;
    fe.n_out      = 1;
    fe.out[0]     = {red, 0};
    fe.frag_depth = depth;
}

// B1-d EARLY-FRAGMENT-TESTS fragment: constant red, forcing the depth test BEFORE the shader
// (`layout(early_fragment_tests) in;` / `[earlydepthstencil]`). Drawn over the fullscreen tri (primitive depth 0) into a
// 0.5-cleared depth buffer with LessEqual, every fragment passes → all red. The gate is that the emitted execution mode
// COMPILES, the draw runs, and validation is silent (a behavioural FS-side-effect test waits for B1-f storage buffers).
inline void build_early_fragment_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  zero = g.constant(0.0, sh, kir::DType::F32);
    const int  red  = g.vec4(one, zero, zero, one);

    fe.stage                = kir::KStage::Fragment;
    fe.n_out                = 1;
    fe.out[0]               = {red, 0};
    fe.early_fragment_tests = true;
}

// B1-e: a fragment ramp R = FragCoord.x / 32 — R changes by ~8/pixel horizontally, so at 1×1 rate adjacent pixels DIFFER,
// but under a coarse VRS rate a whole W×H block shares ONE fragment-shader invocation ⇒ one R ⇒ the block is uniform. That
// block uniformity (horizontal even-x neighbour pairs becoming equal) is the observable VRS effect.
inline void build_vrs_ramp_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int  fcx  = g.swizzle(fc, 0);                       // FragCoord.x
    const int  inv  = g.constant(1.0 / 32.0, sh, kir::DType::F32);
    const int  r    = g.binary(kir::KOp::Mul, fcx, inv);      // FragCoord.x / 32
    const int  zero = g.constant(0.0, sh, kir::DType::F32);
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  col  = g.vec4(r, zero, zero, one);

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B1-e: the fullscreen VS + a per-PRIMITIVE VRS output of 2×2 (packed 5) via gl_PrimitiveShadingRateEXT / SV_ShadingRate.
// The packing is (Yshift<<2)|Xshift, shift 0=1×,1=2×,2=4× ⇒ 2×2 = (1<<2)|1 = 5, identical on Vulkan + D3D12.
inline void build_vrs_primitive_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    build_fullscreen_vs(g, ve);
    const auto sh   = kir::make_shape({1});
    // GLSL's gl_PrimitiveShadingRateEXT is `int`, so author the packed rate as I32 (the HLSL emitter casts it to uint).
    ve.shading_rate = g.constant(5.0, sh, kir::DType::I32); // 2×2 = (1<<2)|1
}

// B4: a MESH shader emitting a fullscreen triangle + a PER-PRIMITIVE 2×2 VRS rate (gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT
// / SV_ShadingRate) — the mesh-pipeline analogue of build_vrs_primitive_vs. Drawn with draw_mesh_vrs over build_vrs_ramp_fs; the
// 2×2 rate coarsens the ramp so each 2×2 block shades once. One workgroup: 3 verts, 1 primitive.
inline void build_vrs_primitive_mesh(crd::kir::KGraph& g, crd::kir::KEntry& me)
{
    namespace kir  = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto f   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  tid = g.cast(g.builtin(kir::KBuiltin::LocalInvocationIndex), kir::DType::I32); // 0..2 = local vertex
    const int  eq0 = g.binary(kir::KOp::CmpEq, tid, g.constant(0.0, sh, kir::DType::I32));
    const int  eq1 = g.binary(kir::KOp::CmpEq, tid, g.constant(1.0, sh, kir::DType::I32));
    const int  x   = g.select(eq0, f(-1.0), g.select(eq1, f(3.0), f(-1.0)));  // fullscreen triangle (covers the viewport)
    const int  y   = g.select(eq0, f(-1.0), g.select(eq1, f(-1.0), f(3.0)));
    const int  u0  = g.constant(0.0, sh, kir::DType::U32);
    const int  u1  = g.constant(1.0, sh, kir::DType::U32);
    const int  u2  = g.constant(2.0, sh, kir::DType::U32);
    me.stage           = kir::KStage::Mesh;
    me.position        = g.vec4(x, y, f(0.0), f(1.0));
    me.mesh_vertices   = 3;
    me.mesh_primitives = 1;
    me.mesh_prim       = g.vec3(u0, u1, u2);
    me.n_out           = 0;
    me.shading_rate    = g.constant(5.0, sh, kir::DType::I32); // 2×2 = (1<<2)|1 — the per-primitive rate
}

// B1-f SMALL TILTED triangle (NDC ±0.35, no interpolants) — the covers-more / inner-coverage substrate. Its non-axis-aligned
// edges pass through many pixels whose CENTRES lie outside it: normal raster skips those (top-left rule), CONSERVATIVE
// OVERESTIMATE raster covers them. So the same primitive draws MORE pixels under overestimate — the biting observable.
inline void build_small_triangle_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int  k0  = g.constant(0.0, sh, kir::DType::I32);
    const int  k1  = g.constant(1.0, sh, kir::DType::I32);
    const int  eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int  eq1 = g.binary(kir::KOp::CmpEq, vid, k1);
    const int  x0  = g.constant(0.0, sh, kir::DType::F32);
    const int  x1  = g.constant(0.35, sh, kir::DType::F32);
    const int  x2  = g.constant(-0.35, sh, kir::DType::F32);
    const int  y0  = g.constant(-0.35, sh, kir::DType::F32);
    const int  y1  = g.constant(0.35, sh, kir::DType::F32);
    const int  x   = g.select(eq0, x0, g.select(eq1, x1, x2));
    const int  y   = g.select(eq0, y0, y1);
    const int  z   = g.constant(0.0, sh, kir::DType::F32);
    const int  w   = g.constant(1.0, sh, kir::DType::F32);

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, z, w);
    ve.n_out    = 0;
}

// B1-f INNER-COVERAGE fragment: colour = InnerCoverage broadcast to RGB (white where the pixel is FULLY inside the
// primitive, black where only partially covered). Drawn over `build_small_triangle_vs` under conservative OVERESTIMATE
// raster (which generates the partially-covered EDGE fragments): the interior reads 1 → white, the edge rim reads 0 →
// black. So `KBuiltin::InnerCoverage` demonstrably VARIES across the primitive — the biting inner-coverage gate. (On a
// blue clear, background stays blue, so all three regions are distinguishable.)
inline void build_inner_coverage_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  ic  = g.builtin(kir::KBuiltin::InnerCoverage); // uint: 1 = fully covered, 0 = partial
    const int  icf = g.cast(ic, kir::DType::F32);             // 1.0 (white) / 0.0 (black)
    const int  one = g.constant(1.0, sh, kir::DType::F32);
    const int  col = g.vec4(icf, icf, icf, one);

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B1-f INTERLOCK counter VS: the base triangle {(0,-0.8),(0.8,0.8),(-0.8,0.8)} authored so that a 6-vertex draw emits it
// TWICE (vid % 3 selects the corner). The two primitives cover the SAME pixels, so every covered pixel is hit by two
// fragments from DIFFERENT primitives — the contention the fragment interlock must serialise. (vid % 3 via a select: for
// vid in 0..5, vid<3 ? vid : vid-3.)
inline void build_interlock_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int  k3  = g.constant(3.0, sh, kir::DType::I32);
    const int  lt3 = g.binary(kir::KOp::CmpLt, vid, k3);      // vid < 3
    const int  vm3 = g.binary(kir::KOp::Sub, vid, k3);        // vid - 3
    const int  ci  = g.select(lt3, vid, vm3);                 // vid % 3 for vid in 0..5
    const int  k0  = g.constant(0.0, sh, kir::DType::I32);
    const int  k1  = g.constant(1.0, sh, kir::DType::I32);
    const int  eq0 = g.binary(kir::KOp::CmpEq, ci, k0);
    const int  eq1 = g.binary(kir::KOp::CmpEq, ci, k1);
    const int  x0  = g.constant(0.0, sh, kir::DType::F32);
    const int  x1  = g.constant(0.8, sh, kir::DType::F32);
    const int  x2  = g.constant(-0.8, sh, kir::DType::F32);
    const int  y0  = g.constant(-0.8, sh, kir::DType::F32);
    const int  y1  = g.constant(0.8, sh, kir::DType::F32);
    const int  x   = g.select(eq0, x0, g.select(eq1, x1, x2));
    const int  y   = g.select(eq0, y0, y1);
    const int  z   = g.constant(0.0, sh, kir::DType::F32);
    const int  w   = g.constant(1.0, sh, kir::DType::F32);

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, z, w);
    ve.n_out    = 0;
}

// B1-f INTERLOCK counter FS: a rasterizer-ordered read-modify-write `storage[y*width + x] += 1`, keyed by the pixel
// coordinate so ONLY overlapping (same-pixel) fragments contend — the case the interlock serialises. Drawn twice (see
// build_interlock_vs) under `draw_storage`, every covered pixel is incremented by two DIFFERENT primitives' fragments;
// under interlock the RMW is ordered, so every covered pixel reads EXACTLY 2 and the background reads 0 — deterministic.
// Also paints red so the colour target still shows coverage.
inline void build_interlock_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 width)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord); // vec4; .xy = pixel centre (x+0.5, y+0.5)
    const int  fx  = g.swizzle(fc, 0);
    const int  fy  = g.swizzle(fc, 1);
    const int  ux  = g.cast(fx, kir::DType::U32);         // truncates the +0.5 centre back to the integer pixel
    const int  uy  = g.cast(fy, kir::DType::U32);
    const int  wc  = g.constant(static_cast<double>(width), sh, kir::DType::U32);
    const int  row = g.binary(kir::KOp::Mul, uy, wc);     // uy * width
    const int  idx = g.binary(kir::KOp::Add, row, ux);    // uy*width + ux
    const int  cur = g.storage_load(idx);                 // uint: the current counter
    const int  u1  = g.constant(1.0, sh, kir::DType::U32);
    const int  nxt = g.binary(kir::KOp::Add, cur, u1);    // cur + 1

    const int one  = g.constant(1.0, sh, kir::DType::F32);
    const int zero = g.constant(0.0, sh, kir::DType::F32);
    const int red  = g.vec4(one, zero, zero, one);

    fe.stage               = kir::KStage::Fragment;
    fe.n_out               = 1;
    fe.out[0]              = {red, 0};
    fe.storage_write_index = idx;
    fe.storage_write_value = nxt;
    fe.interlock           = true;
}

// B2: a fullscreen VS that also emits a UV interpolant. The oversized tri {(-1,-1),(3,-1),(-1,3)} carries UV {(0,0),(2,0),
// (0,2)} so across the visible [-1,1] NDC the UV runs [0,1] (UV = (NDC+1)/2) — the standard fullscreen-blit UV. Screen-left
// (NDC.x<0) ⇒ UV.x<0.5, screen-right ⇒ UV.x>0.5 (the x mapping is backend-identical; only UV.y flips Vulkan vs DX12).
inline void build_textured_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const int  vid = g.builtin(kir::KBuiltin::VertexIndex);
    const int  k0  = g.constant(0.0, sh, kir::DType::I32);
    const int  k1  = g.constant(1.0, sh, kir::DType::I32);
    const int  eq0 = g.binary(kir::KOp::CmpEq, vid, k0);
    const int  eq1 = g.binary(kir::KOp::CmpEq, vid, k1);
    const int  xm1 = g.constant(-1.0, sh, kir::DType::F32);
    const int  x3  = g.constant(3.0, sh, kir::DType::F32);
    const int  y3  = g.constant(3.0, sh, kir::DType::F32);
    const int  x   = g.select(eq0, xm1, g.select(eq1, x3, xm1));
    const int  y   = g.select(eq0, xm1, g.select(eq1, xm1, y3));
    const int  z   = g.constant(0.0, sh, kir::DType::F32);
    const int  w   = g.constant(1.0, sh, kir::DType::F32);
    const int  u0  = g.constant(0.0, sh, kir::DType::F32);
    const int  u2  = g.constant(2.0, sh, kir::DType::F32);
    const int  uu  = g.select(eq0, u0, g.select(eq1, u2, u0)); // {0,2,0}
    const int  vv  = g.select(eq0, u0, g.select(eq1, u0, u2)); // {0,0,2}
    const int  uv  = g.vec2(uu, vv);

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, z, w);
    ve.n_out    = 1;
    ve.out[0]   = {uv, 0, kir::Interp::Smooth};
}

// B2: a fragment shader that samples a 2D texture at the interpolated UV and writes the sampled colour. Texture at set 0 /
// binding 1, sampler at set 0 / binding 2 (matching draw_textured's descriptors). The biting observable: with a left-red /
// right-green texture, screen-left reads red and screen-right reads green — proving the sample runs AND UV.x drives it.
inline void build_sample_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const int uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int tex  = g.texture(0, 1); // 2D float texture
    const int samp = g.sampler(0, 2);
    const int col  = g.tex_sample(tex, samp, uv); // vec4

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {col, 0};
}

// B2-b helpers — each reads the UV interpolant (build_textured_vs) and exercises ONE sample-op on the left-red/right-green
// texture (tex_0_1 + samp_0_2), writing a colour a readback can check. `dim_texels` = the texture edge (16) for coord math.

// SampleLod at LOD 0 ≡ base level: left red / right green (proves the explicit-LOD path runs, the VS/CS-legal sampler).
inline void build_samplelod_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex  = g.texture(0, 1);
    const int  samp = g.sampler(0, 2);
    const int  lod0 = g.constant(0.0, sh, kir::DType::F32);
    const int  col  = g.tex_sample_lod(tex, samp, uv, lod0);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// SampleGrad with zero gradients ≡ base level: left red / right green.
inline void build_samplegrad_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  uv    = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex   = g.texture(0, 1);
    const int  samp  = g.sampler(0, 2);
    const int  zero  = g.constant(0.0, sh, kir::DType::F32);
    const int  zero2 = g.vec2(zero, zero); // ddx = ddy = (0,0)
    const int  col   = g.tex_sample_grad(tex, samp, uv, zero2, zero2);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// TexelFetch at the integer texel `ivec2(uv * dim_texels)` (LOD 0, no filtering): left texel red / right texel green.
inline void build_texelfetch_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim_texels)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  uv    = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex   = g.texture(0, 1);
    const int  samp  = g.sampler(0, 2);
    const int  scale = g.constant(static_cast<double>(dim_texels), sh, kir::DType::F32);
    const int  uvs   = g.binary(kir::KOp::Mul, uv, g.vec2(scale, scale)); // uv * dim
    const int  coord = g.cast(uvs, kir::DType::I32);                      // ivec2 texel coord
    const int  lod0  = g.constant(0.0, sh, kir::DType::I32);
    const int  col   = g.tex_fetch(tex, samp, coord, lod0);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// TexSize(LOD 0): writes (size.x/255, size.y/255, 0, 1) so a readback reads R = G = dim_texels (16). Backend-independent.
inline void build_texsize_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  tex  = g.texture(0, 1);
    const int  samp = g.sampler(0, 2);
    const int  lod0 = g.constant(0.0, sh, kir::DType::I32);
    const int  sz   = g.tex_size(tex, samp, lod0);      // ivec2
    const int  szf  = g.cast(sz, kir::DType::F32);       // vec2
    const int  inv  = g.constant(1.0 / 255.0, sh, kir::DType::F32);
    const int  norm = g.binary(kir::KOp::Mul, szf, g.vec2(inv, inv)); // size / 255
    const int  zero = g.constant(0.0, sh, kir::DType::F32);
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  col  = g.vec4(g.swizzle(norm, 0), g.swizzle(norm, 1), zero, one);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// TexGather of the RED channel: the 4 texels' red around uv. Left (all red R=255) ⇒ (1,1,1,1) ⇒ white; right ⇒ black.
inline void build_gather_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex  = g.texture(0, 1);
    const int  samp = g.sampler(0, 2);
    const int  comp = g.constant(0.0, sh, kir::DType::I32); // red channel
    const int  col  = g.tex_gather(tex, samp, uv, comp);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// B2-b SHADOW (depth-compare) FS: samples a depth texture (shadow sampler) with `ref = uv.x`. With the depth = 0.5
// everywhere and compareOp LESS_OR_EQUAL, the result is 1 where uv.x <= 0.5 (screen-left → white) and 0 where uv.x > 0.5
// (screen-right → black). Proves the shadow path: comparison sampler + depth texture + KOp::SampleCmp, both backends.
inline void build_shadow_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex  = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex2D, false, false, /*shadow=*/true);
    const int  samp = g.sampler(0, 2, /*shadow=*/true);
    const int  ref  = g.swizzle(uv, 0); // ref = uv.x
    const int  r    = g.tex_sample_cmp(tex, samp, uv, ref);
    const int  one  = g.constant(1.0, sh, kir::DType::F32);
    const int  col  = g.vec4(r, r, r, one);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// B8-f SHADOW FOUNDATION: a receiver plane (world pos synthesized from FragCoord) projected into a shadow map via a light_vp,
// with the full bias stack (normal-offset + slope-scaled), hardware-PCF compared against an uploaded depth map (0.5). With the
// identity light_vp: uv=(fx,fy)/32, receiver depth = fx/32 → the biased depth crosses 0.5 near fx≈15 → screen-left lit / right
// shadowed. Exercises the B8-f emitter path (mat4 · mat_mul_vec · perspective divide · the bias math · SampleCmp), both backends.
inline void build_shadow_foundation_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc   = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int  fx   = g.swizzle(fc, 0);
    const int  fy   = g.swizzle(fc, 1);
    const int  wpos = g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, fx, k(2.0 / 32.0)), k(1.0)),
                             g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, fy, k(2.0 / 32.0)), k(1.0)),
                             g.binary(kir::KOp::Mul, fx, k(1.0 / 32.0)));
    const int  lvp  = g.mat4(g.vec4(k(1.0), k(0.0), k(0.0), k(0.0)), g.vec4(k(0.0), k(1.0), k(0.0), k(0.0)), g.vec4(k(0.0), k(0.0), k(1.0), k(0.0)), g.vec4(k(0.0), k(0.0), k(0.0), k(1.0)));
    const int  tex  = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex2D, false, false, /*shadow=*/true);
    const int  samp = g.sampler(0, 2, /*shadow=*/true);
    const int  vis  = lt::shadow_factor(g, wpos, kc(0.0, 0.0, 1.0), k(0.6), lvp, tex, samp, k(0.03), k(0.002), k(0.01));
    const int  col  = nd::detail::bin(g, kir::KOp::Mul, kc(0.9, 0.7, 0.5), g.splat(vis, 3)); // lit → warm, shadowed → black
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}

// B8-g PCF: the B8-f receiver + bias, but the shadow test is an 8-tap per-pixel-rotated Poisson PCF (`pcf_shadow`) → the
// same left-lit/right-shadowed boundary, now via the multi-tap rotated kernel. Exercises the PCF emitter path both backends.
inline void build_pcf_shadow_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc   = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int  fx   = g.swizzle(fc, 0);
    const int  fy   = g.swizzle(fc, 1);
    const int  wpos = g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, fx, k(2.0 / 32.0)), k(1.0)),
                             g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, fy, k(2.0 / 32.0)), k(1.0)),
                             g.binary(kir::KOp::Mul, fx, k(1.0 / 32.0)));
    const int  lvp  = g.mat4(g.vec4(k(1.0), k(0.0), k(0.0), k(0.0)), g.vec4(k(0.0), k(1.0), k(0.0), k(0.0)), g.vec4(k(0.0), k(0.0), k(1.0), k(0.0)), g.vec4(k(0.0), k(0.0), k(0.0), k(1.0)));
    const int  proj = lt::shadow_project(g, lt::normal_offset_bias(g, wpos, kc(0.0, 0.0, 1.0), k(0.6), k(0.03)), lvp);
    const int  uv   = g.vec2(g.swizzle(proj, 0), g.swizzle(proj, 1));
    const int  dep  = g.binary(kir::KOp::Sub, g.swizzle(proj, 2), lt::slope_scaled_bias(g, k(0.6), k(0.002), k(0.01)));
    const int  tex  = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex2D, false, false, /*shadow=*/true);
    const int  samp = g.sampler(0, 2, /*shadow=*/true);
    const int  vis  = lt::pcf_shadow(g, tex, samp, uv, dep, k(0.015), g.vec2(fx, fy));
    const int  col  = nd::detail::bin(g, kir::KOp::Mul, kc(0.9, 0.7, 0.5), g.splat(vis, 3));
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}

// B2-b: fill `dst` (w*h floats) with a uniform depth `value`.
inline void fill_uniform_depth(float* dst, crd::u32 w, crd::u32 h, float value)
{
    for (crd::u32 i = 0; i < w * h; ++i) { dst[i] = value; }
}

// B2-d BINDLESS FS: a texture ARRAY at set 0 / binding 3 (`texture(...,8)`), sampled at a DYNAMIC per-fragment index
// (screen-left → 0, right → 1). With texture[0]=red, texture[1]=green ⇒ screen-left red, right green.
inline void build_bindless_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  uv    = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  tex   = g.texture(0, 3, kir::DType::F32, kir::TexDim::Tex2D, false, false, false, /*array_count=*/8);
    const int  samp  = g.sampler(0, 2);
    const int  hf    = g.constant(0.5, sh, kir::DType::F32);
    const int  lt    = g.binary(kir::KOp::CmpLt, g.swizzle(uv, 0), hf);
    const int  i0    = g.constant(0.0, sh, kir::DType::U32);
    const int  i1    = g.constant(1.0, sh, kir::DType::U32);
    const int  idx   = g.select(lt, i0, i1); // uint index 0 / 1
    const int  col   = g.tex_sample_at(tex, samp, uv, idx);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// B2-c: fill `texels` RGBA8 texels with a solid colour (used for cube faces / array layers).
inline void fill_solid(crd::u8* dst, crd::u32 texels, crd::u8 r, crd::u8 g, crd::u8 b)
{
    for (crd::u32 i = 0; i < texels; ++i) { dst[i * 4U] = r; dst[i * 4U + 1U] = g; dst[i * 4U + 2U] = b; dst[i * 4U + 3U] = 255U; }
}

// B2-c FS builders — each reads the UV interpolant, builds the SAMPLE COORDINATE matching the texture dimension, and
// samples tex_0_1 (of that dim) through samp_0_2. All resolve to "screen-left = red, screen-right = green".

// 1D: coord = uv.x (float). Texture (16x1) left-red/right-green.
inline void build_sample_1d_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const int uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int u    = g.swizzle(uv, 0);
    const int tex  = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex1D);
    const int samp = g.sampler(0, 2);
    const int col  = g.tex_sample(tex, samp, u);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// 3D: coord = vec3(uv, 0.5). Texture (w x h x 2), each slice left-red/right-green.
inline void build_sample_3d_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  uv    = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  half  = g.constant(0.5, sh, kir::DType::F32);
    const int  coord = g.vec3(g.swizzle(uv, 0), g.swizzle(uv, 1), half);
    const int  tex   = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex3D);
    const int  samp  = g.sampler(0, 2);
    const int  col   = g.tex_sample(tex, samp, coord);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// Cube: dir = (uv.x - 0.5, 0, 0.05) so X dominates ⇒ screen-left hits -X, screen-right hits +X. Faces: +X red · -X green.
inline void build_sample_cube_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const int  uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  hf   = g.constant(0.5, sh, kir::DType::F32);
    const int  dx   = g.binary(kir::KOp::Sub, g.swizzle(uv, 0), hf);
    const int  zero = g.constant(0.0, sh, kir::DType::F32);
    const int  zc   = g.constant(0.05, sh, kir::DType::F32);
    const int  dir  = g.vec3(dx, zero, zc);
    const int  tex  = g.texture(0, 1, kir::DType::F32, kir::TexDim::TexCube);
    const int  samp = g.sampler(0, 2);
    const int  col  = g.tex_sample(tex, samp, dir);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// 2DArray: coord = vec3(uv, layer) with layer = (uv.x < 0.5 ? 0 : 1). Layer 0 red · layer 1 green.
inline void build_sample_array_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  uv    = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  u     = g.swizzle(uv, 0);
    const int  hf    = g.constant(0.5, sh, kir::DType::F32);
    const int  lt    = g.binary(kir::KOp::CmpLt, u, hf);
    const int  zero  = g.constant(0.0, sh, kir::DType::F32);
    const int  one   = g.constant(1.0, sh, kir::DType::F32);
    const int  layer = g.select(lt, zero, one);
    const int  coord = g.vec3(u, g.swizzle(uv, 1), layer);
    const int  tex   = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex2D, /*arrayed=*/true);
    const int  samp  = g.sampler(0, 2);
    const int  col   = g.tex_sample(tex, samp, coord);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// CubeArray: coord = vec4(0,0,1, cube) with cube = (uv.x < 0.5 ? 0 : 1); dir (0,0,1) hits +Z. Cube 0 red · cube 1 green.
inline void build_sample_cubearray_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const int  uv    = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int  hf    = g.constant(0.5, sh, kir::DType::F32);
    const int  lt    = g.binary(kir::KOp::CmpLt, g.swizzle(uv, 0), hf);
    const int  zero  = g.constant(0.0, sh, kir::DType::F32);
    const int  one   = g.constant(1.0, sh, kir::DType::F32);
    const int  cube  = g.select(lt, zero, one);
    const int  coord = g.vec4(zero, zero, one, cube);
    const int  tex   = g.texture(0, 1, kir::DType::F32, kir::TexDim::TexCube, /*arrayed=*/true);
    const int  samp  = g.sampler(0, 2);
    const int  col   = g.tex_sample(tex, samp, coord);
    fe.stage = kir::KStage::Fragment; fe.n_out = 1; fe.out[0] = {col, 0};
}

// B2: fill `dst` (w*h*4 RGBA8) — the LEFT half red (255,0,0), the RIGHT half green (0,255,0), alpha 255. A large uniform
// split so a bilinear sample away from the seam returns the pure texel colour (no edge blend).
inline void fill_left_red_right_green(crd::u8* dst, crd::u32 w, crd::u32 h)
{
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x)
        {
            crd::u8*   px  = dst + (static_cast<crd::usize>(y) * w + x) * 4U;
            const bool left = x < w / 2U;
            px[0] = left ? 255U : 0U;   // R
            px[1] = left ? 0U : 255U;   // G
            px[2] = 0U;                 // B
            px[3] = 255U;               // A
        }
    }
}

// B5-a SURFACE MATERIAL FS: builds an OpenPBR surface with known constant params and packs it to the deferred G-buffer
// (4 MRT attachments) via `material::pack_gbuffer`. Drawn over `build_fullscreen_vs`; each G-buffer channel reads back a
// known value — proving a lighting-agnostic material outputs its surface through MRT on both backends.
//   base=(0.8,0.2,0.1) metallic=0.5 · normal=(0,0,1) roughness=0.3 · emissive=(0.1,0.9,0.2) occlusion=0.7 · opacity=1.0
inline void build_surface_material_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  sid = kir::material::define_surface(g);
    const int  bc  = g.vec3(k(0.8), k(0.2), k(0.1));
    const int  me  = k(0.5);
    const int  ro  = k(0.3);
    const int  nm  = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  em  = g.vec3(k(0.1), k(0.9), k(0.2));
    const int  oc  = k(0.7);
    const int  op  = k(1.0);
    const int  surf = kir::material::build_surface(g, sid, bc, me, ro, nm, em, oc, op);
    kir::material::pack_gbuffer(g, fe, surf);
}

// B5-b FULL-SLAB material FS: builds the complete OpenPBR 1.1 surface (specular/coat/fuzz/transmission/subsurface/thin-film/
// thin-walled layers set to known weights + colours) and packs it to the EXTENDED 8-attachment G-buffer. Every OpenPBR
// layer reads back a known value — proving the full slab is output, both backends. (Core layer as B5-a; layers below.)
inline void build_surface_full_material_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace m   = crd::kir::material;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto v3  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  sid = m::define_surface(g);
    int        f[m::SfCount];
    m::surface_defaults(g, f);
    f[m::SfBaseColor]         = v3(0.8, 0.2, 0.1);
    f[m::SfMetallic]          = k(0.5);
    f[m::SfRoughness]         = k(0.3);
    f[m::SfNormal]            = v3(0.0, 0.0, 1.0);
    f[m::SfEmissive]          = v3(0.1, 0.9, 0.2);
    f[m::SfOcclusion]         = k(0.7);
    f[m::SfOpacity]           = k(1.0);
    f[m::SfSpecularWeight]    = k(0.6);
    f[m::SfCoatWeight]        = k(0.4);
    f[m::SfCoatColor]         = v3(0.3, 0.5, 0.9);
    f[m::SfCoatRoughness]     = k(0.2);
    f[m::SfFuzzWeight]        = k(0.8);
    f[m::SfFuzzColor]         = v3(0.9, 0.7, 0.5);
    f[m::SfFuzzRoughness]     = k(0.6);
    f[m::SfTransmissionWeight] = k(0.25);
    f[m::SfTransmissionColor] = v3(0.2, 0.8, 0.4);
    f[m::SfThinFilmWeight]    = k(0.9);
    f[m::SfThinFilmThickness] = k(0.55);
    f[m::SfSubsurfaceWeight]  = k(0.35);
    f[m::SfThinWalled]        = k(1.0);
    const int surf = m::build_surface_full(g, sid, f);
    m::pack_gbuffer_ext(g, fe, surf);
}

// B5-c SHADING-MODEL material FS: a surface tagged with ShadingModel::Gooch. The tag flows through the compact G-buffer's
// gbuf3.G channel (shading_model/255) — read back ×255 gives the enum value (4). Proves the shading-model taxonomy is output.
inline void build_gooch_material_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace m   = crd::kir::material;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  sid = m::define_surface(g);
    int        f[m::SfCount];
    m::surface_defaults(g, f);
    f[m::SfBaseColor]    = g.vec3(k(0.5), k(0.5), k(0.5));
    f[m::SfShadingModel] = k(static_cast<double>(m::ShadingModel::Gooch));      // 4
    f[m::SfAlphaMode]    = k(static_cast<double>(m::AlphaMode::Opaque));        // 0
    m::pack_gbuffer(g, fe, m::build_surface_full(g, sid, f));
}

// B5-c MASKED material FS: opacity = FragCoord.x / `dim` (a left→right ramp), AlphaMode::Masked, cutoff 0.5 ⇒ the left half
// (opacity < 0.5) is DISCARDED (keeps the clear colour), the right half writes base_color red. Proves the masked alpha domain.
inline void build_masked_material_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim)
{
    namespace kir = crd::kir;
    namespace m   = crd::kir::material;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord);
    const int  op  = g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(1.0 / static_cast<double>(dim))); // FragCoord.x / dim
    const int  sid = m::define_surface(g);
    int        f[m::SfCount];
    m::surface_defaults(g, f);
    f[m::SfBaseColor] = g.vec3(k(0.8), k(0.1), k(0.1)); // red where kept
    f[m::SfOpacity]   = op;
    f[m::SfAlphaMode] = k(static_cast<double>(m::AlphaMode::Masked)); // 1
    const int surf     = m::build_surface_full(g, sid, f);
    m::pack_gbuffer(g, fe, surf);
    m::set_masked(g, fe, surf, 0.5); // discard where opacity < 0.5
}

// B6-a NODE-LIBRARY FS: the MaterialX-parity operator nodes exercised on the GPU. Drives `bg` from FragCoord.x (a left→right
// ramp crossing 0.5 at screen centre) and composites it under a constant `fg` colour with `nodes::overlay` (mix=1). Overlay's
// per-channel branch FLIPS at the centre — the LEFT half takes the multiply branch (2*fg*bg), the RIGHT half the screen
// branch (1-2*(1-bg)*(1-fg)) — so left/right pixels read distinct, hand-computable colours IDENTICALLY on both backends. This
// exercises the per-channel decomposition + Select + arithmetic emit path for the whole operator library. Draw over
// build_fullscreen_vs. fg = (0.8, 0.5, 0.2).
inline void build_nodes_overlay_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord);
    const int  bgs = g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(1.0 / static_cast<double>(dim))); // FragCoord.x / dim
    const int  bg3 = g.splat(bgs, 3);
    const int  fg3 = g.vec3(k(0.8), k(0.5), k(0.2));
    const int  one = k(1.0);
    const int  res = nd::overlay(g, fg3, bg3, one); // per-channel overlay, mix = 1 ⇒ result = base
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(res, 0), g.swizzle(res, 1), g.swizzle(res, 2), one), 0};
}

// B8-a LIGHTING FS: evaluate the Cook-Torrance BRDF (crd::kir::lighting::brdf_direct) for a constant PBR surface + a
// directional light, with the ROUGHNESS driven by FragCoord.x (a 0.05→0.95 ramp so it can't constant-fold — the specular
// highlight softens across the row). Output = clamp01(lit colour). Each column equals the library's own F32 eval → a
// gold-standard BRDF renders identically on both backends. base=(0.8,0.3,0.2) metal=0.1 · N=V=(0,0,1) · L=norm(0.4,0.3,1).
namespace lighting_obs
{
[[nodiscard]] inline int build(crd::kir::KGraph& g, int rough_node)
{
    namespace kir = crd::kir;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  base = g.vec3(k(0.8), k(0.3), k(0.2));
    const int  n    = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  view = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  ln   = g.normalize(g.vec3(k(0.4), k(0.3), k(1.0)));
    const int  lc   = g.vec3(k(1.5), k(1.5), k(1.5));
    const int  col  = lt::brdf_direct(g, base, k(0.1), rough_node, n, view, ln, lc);
    return crd::kir::nodes::clamp01(g, col);
}
// B8-b LAYERED: the same lit surface with a clearcoat lobe (weight 0.5, IOR 1.5) attenuating the base + an added sheen lobe.
[[nodiscard]] inline int build_layered(crd::kir::KGraph& g, int rough_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  base = g.vec3(k(0.8), k(0.3), k(0.2));
    const int  n    = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  view = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  ln   = g.normalize(g.vec3(k(0.4), k(0.3), k(1.0)));
    const int  lc   = g.vec3(k(1.5), k(1.5), k(1.5));
    const int  core = lt::brdf_direct(g, base, k(0.1), rough_node, n, view, ln, lc); // B8-a core (vec3)
    const int  h    = g.normalize(g.binary(kir::KOp::Add, view, ln));
    const int  nov  = g.binary(kir::KOp::Add, g.unary(kir::KOp::Abs, g.dot(n, view)), k(1e-5));
    const int  nol  = nd::clamp01(g, g.dot(n, ln));
    const int  noh  = nd::clamp01(g, g.dot(n, h));
    const int  loh  = nd::clamp01(g, g.dot(ln, h));
    const int  fcc  = lt::clearcoat_fresnel(g, k(0.5), loh);
    const int  coat = g.binary(kir::KOp::Mul, g.binary(kir::KOp::Mul, lt::clearcoat_lobe(g, k(0.5), k(0.1), noh, loh), nol), k(1.5)); // scalar coat·NoL·light
    const int  sheen = nd::detail::bin(g, kir::KOp::Mul, lt::sheen_lobe(g, g.vec3(k(0.3), k(0.3), k(0.3)), k(0.3), nov, nol, noh), g.binary(kir::KOp::Mul, g.splat(nol, 3), lc));
    const int  atten = nd::detail::bin(g, kir::KOp::Mul, core, g.binary(kir::KOp::Sub, k(1.0), fcc)); // base ·(1−Fcc)
    const int  lit   = nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Add, atten, g.splat(coat, 3)), sheen);
    return nd::clamp01(g, lit);
}
// B8-b THIN-FILM + TRANSMISSION: an iridescent, refracting glass surface. Exercises the new emitter paths end-to-end
// (Cos/Exp in eval_sensitivity, Smoothstep in the thin-film mix, Log/Exp in Beer's law, Refract in the refraction ray) so
// both backends must agree pixel-for-pixel. The refraction ray drives the absorption distance (nothing is dead code).
[[nodiscard]] inline int build_glass(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  n    = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  view = g.vec3(k(0.0), k(0.0), k(1.0));
    const int  ln   = g.normalize(g.vec3(k(0.4), k(0.3), k(1.0)));
    // thin-film iridescence — thickness ramps with the sweep; base F0 varies per channel to exercise BOTH phi23 branches
    // (baseIor ≈ 1.25 / 1.5 / 2.0 vs iridIor 1.3 → phi23 = π / 0 / 0).
    const int  thick  = g.binary(kir::KOp::Add, k(120.0), g.binary(kir::KOp::Mul, sweep_node, k(700.0)));
    const int  base_f0 = g.vec3(k(0.012), k(0.04), k(0.09));
    const int  irid_f0 = lt::eval_iridescence(g, k(1.0), k(1.3), k(0.72), thick, base_f0);
    const int  h      = g.normalize(g.binary(kir::KOp::Add, view, ln));
    const int  nol    = nd::clamp01(g, g.dot(n, ln));
    const int  loh    = nd::clamp01(g, g.dot(ln, h));
    const int  spec_f  = lt::f_schlick(g, irid_f0, k(1.0), loh);                 // Schlick with the iridescent F0
    const int  spec   = nd::detail::bin(g, kir::KOp::Mul, spec_f, g.splat(nol, 3));
    // transmission — a refracted, Beer-absorbed tint; the refraction ray sets the absorption distance.
    const int  alpha    = g.binary(kir::KOp::Mul, sweep_node, sweep_node);
    const int  trans    = lt::transmission_btdf(g, g.vec3(k(0.9), k(0.95), k(1.0)), g.vec3(k(0.04), k(0.04), k(0.04)), k(1.0), alpha, k(1.5), n, view, ln);
    const int  rray     = lt::refraction_ray(g, n, view, k(1.5), k(0.35));     // Refract → a ray direction
    const int  dist     = g.binary(kir::KOp::Mul, g.unary(kir::KOp::Abs, g.swizzle(rray, 2)), k(1.5));
    const int  absorbed = lt::volume_attenuation(g, trans, dist, g.vec3(k(0.82), k(0.9), k(1.0)), k(1.0));
    const int  col      = nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Mul, spec, g.splat(k(1.5), 3)), nd::detail::bin(g, kir::KOp::Mul, absorbed, g.splat(k(6.0), 3)));
    return nd::clamp01(g, col);
}
// B8-c PUNCTUAL LIGHTS: a surface lit by a directional + point + spot light; the sweep drives the surface world-x so the
// point/spot distance + cone attenuation vary across the ramp. Both backends must agree pixel-for-pixel.
[[nodiscard]] inline int build_lights(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = kc(0.0, 0.0, 1.0);
    const int  wpos = g.vec3(g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, sweep_node, k(0.5)), k(6.0)), k(0.0), k(0.0)); // world-x ∈ [−2.7, 2.7]
    const int  base = kc(0.8, 0.35, 0.2);
    const int  met  = k(0.1);
    const int  rgh  = k(0.55); // broader (dimmer) specular so the highlight does not blow out to white
    // lights placed IN FRONT of the +z-facing surface so NoL > 0; the sweeping world-x varies point distance + spot cone.
    // A dim directional base + a bright point (left of centre) + a bright spot (right of centre) → a clear spatial gradient.
    const int  dir  = lt::directional_light(g, base, met, rgh, n, view, kc(0.2, -0.3, -1.0), kc(0.15, 0.15, 0.15));
    const int  pt   = lt::point_light(g, base, met, rgh, n, view, wpos, kc(1.5, 1.0, 2.5), kc(1.6, 1.2, 0.8), k(0.04));
    const int  sdir = g.normalize(kc(1.5, -1.0, -2.5)); // spot axis: from the spot toward the origin region
    const int  sp   = lt::spot_light(g, base, met, rgh, n, view, wpos, kc(-1.5, 1.0, 2.5), kc(0.8, 1.6, 1.2), k(0.04), sdir, k(4.0), k(-2.0));
    const int  col  = nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Add, dir, pt), sp);
    return nd::clamp01(g, col);
}
// B8-d AREA LIGHT (LTC): a diffuse RECTANGLE area light via Heitz Linearly-Transformed-Cosines (Minv = identity, scale =
// 1/2π → the un-clipped Lambertian form factor). The sweep moves the shading point under the rect so the form factor (and
// thus the soft shading gradient) varies. Exercises the LTC emitter path — mat3/transpose/matmul/matvec/length/rsqrt/cross.
[[nodiscard]] inline int build_area_rect(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.1, 1.0));                                   // V ≠ N
    const int  wpos = g.vec3(g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, sweep_node, k(0.5)), k(6.0)), k(0.0), k(0.0)); // world-x ∈ [−2.7, 2.7]
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0)); // Minv = I → diffuse
    const int  q0 = kc(-1.5, -1.0, 2.5); const int q1 = kc(1.5, -1.0, 2.5); const int q2 = kc(1.5, 1.0, 2.5); const int q3 = kc(-1.5, 1.0, 2.5);
    const int  ff  = lt::ltc_evaluate_rect(g, n, view, wpos, ident, q0, q1, q2, q3, k(1.0 / (2.0 * lt::kPi)), true);
    const int  col = nd::detail::bin(g, kir::KOp::Mul, kc(0.8, 0.5, 0.3), g.binary(kir::KOp::Mul, g.splat(ff, 3), kc(9.0, 9.0, 9.0)));
    return nd::clamp01(g, col);
}
// B8-d TUBE area light (LTC line integral, diffuse: Minv = I). The sweep moves the shading point under the tube.
[[nodiscard]] inline int build_area_tube(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.15, 1.0));
    const int  wpos = g.vec3(g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, sweep_node, k(0.5)), k(6.0)), k(0.0), k(0.0));
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0));
    const int  ff   = lt::ltc_evaluate_line(g, n, view, wpos, ident, kc(-1.5, 0.3, 2.0), kc(1.3, -0.2, 2.3), k(0.4));
    const int  col  = nd::detail::bin(g, kir::KOp::Mul, kc(0.45, 0.6, 0.95), g.binary(kir::KOp::Mul, g.splat(ff, 3), kc(9.0, 9.0, 9.0)));
    return nd::clamp01(g, col);
}
// B8-d DISK area light (LTC, diffuse: Minv = I). The sweep moves the shading point under the disk.
[[nodiscard]] inline int build_area_disk(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.15, 1.0));
    const int  wpos = g.vec3(g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, sweep_node, k(0.5)), k(6.0)), k(0.0), k(0.0));
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0));
    const int  ff   = lt::ltc_evaluate_disk(g, n, view, wpos, ident, kc(1.0, -0.8, 2.5), kc(1.0, 0.8, 2.5), kc(-1.0, 0.8, 2.5), k(1.0), true);
    const int  col  = nd::detail::bin(g, kir::KOp::Mul, kc(0.9, 0.55, 0.35), g.binary(kir::KOp::Mul, g.splat(ff, 3), kc(2.6, 2.6, 2.6)));
    return nd::clamp01(g, col);
}
// B8-e IBL: a surface lit by image-based lighting — SH L2 irradiance (diffuse) + Karis split-sum (specular). The sweep
// rotates the normal (so the SH irradiance varies) AND ramps the roughness (so the split-sum specular varies).
[[nodiscard]] inline int build_ibl(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  view = kc(0.0, 0.0, 1.0);
    const int  n    = g.normalize(g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, sweep_node, k(2.4)), k(1.2)), k(0.4), k(0.9))); // n.x sweeps
    const int  nov  = g.binary(kir::KOp::Add, g.unary(kir::KOp::Abs, g.dot(n, view)), k(1e-5));
    const int  perc = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, sweep_node, k(0.9)));
    // a representative sky SH set (9 RGB coefficients).
    const double shr[9][3] = {{0.7, 0.75, 0.9}, {0.15, 0.16, 0.2}, {0.28, 0.3, 0.38}, {-0.08, -0.07, -0.05}, {0.02, 0.02, 0.03}, {-0.03, -0.03, -0.02}, {0.1, 0.11, 0.14}, {0.04, 0.04, 0.03}, {-0.05, -0.05, -0.06}};
    int          shn[9];
    for (int i = 0; i < 9; ++i) { shn[i] = kc(shr[i][0], shr[i][1], shr[i][2]); }
    const int  irr  = lt::sh_irradiance(g, n, shn);
    const int  diff = lt::ibl_diffuse(g, kc(0.7, 0.5, 0.35), irr);
    const int  spec = lt::ibl_specular(g, kc(0.5, 0.6, 0.8), kc(0.04, 0.04, 0.04), perc, nov); // prefiltered env × DFG
    const int  col  = nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Mul, diff, kc(0.8, 0.8, 0.8)), nd::detail::bin(g, kir::KOp::Mul, spec, kc(1.1, 1.1, 1.1)));
    return nd::clamp01(g, col);
}

// B8-d SPECULAR area light: a rect lit through the LTC LUT path — a reconstructed fitted Minv (`ltc_matrix`) drives the
// specular lobe, plus the diffuse (Minv = I) term. Proves sample→reconstruct→evaluate agrees on both backends.
[[nodiscard]] inline int build_area_specular(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.15, 1.0));
    const int  wpos = g.vec3(g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, sweep_node, k(0.5)), k(6.0)), k(0.0), k(0.0));
    const int  q0 = kc(-1.5, -1.0, 2.5); const int q1 = kc(1.5, -1.0, 2.5); const int q2 = kc(1.5, 1.0, 2.5); const int q3 = kc(-1.5, 1.0, 2.5);
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0));
    const int  diff  = lt::ltc_evaluate_rect(g, n, view, wpos, ident, q0, q1, q2, q3, k(1.0 / (2.0 * lt::kPi)), true);
    const int  minv  = lt::ltc_matrix(g, g.vec4(k(1.0), k(0.04), k(0.03), k(0.95))); // reconstructed fitted (isotropic, moderate-roughness → broad) Minv
    const int  spec  = lt::ltc_evaluate_rect(g, n, view, wpos, minv, q0, q1, q2, q3, k(1.0 / (2.0 * lt::kPi)), true); // scale = ltc_2 magnitude
    const int  col   = nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Mul, kc(0.7, 0.5, 0.35), g.splat(g.binary(kir::KOp::Mul, diff, k(4.0)), 3)), g.splat(g.binary(kir::KOp::Mul, spec, k(4.5)), 3));
    return nd::clamp01(g, col);
}
// B8-d ANISOTROPIC-GGX area light: the SAME rect eval driven by an ANISOTROPIC Minv (off-diagonal m01/m10) → a stretched
// specular reflection. The eval is identical to the isotropic case; only the fitted matrix (`ltc_matrix_aniso`) differs.
[[nodiscard]] inline int build_area_aniso(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto kc  = [&](double x, double y, double z) { return g.vec3(k(x), k(y), k(z)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.15, 1.0));
    const int  wpos = g.vec3(g.binary(kir::KOp::Mul, g.binary(kir::KOp::Sub, sweep_node, k(0.5)), k(6.0)), k(0.0), k(0.0));
    const int  q0 = kc(-1.5, -1.0, 2.5); const int q1 = kc(1.5, -1.0, 2.5); const int q2 = kc(1.5, 1.0, 2.5); const int q3 = kc(-1.5, 1.0, 2.5);
    const int  minv = lt::ltc_matrix_aniso(g, g.vec4(k(1.0), k(0.05), k(0.04), k(0.9)), g.vec2(k(0.12), k(0.95))); // anisotropic fit (m01 = 0.12, broad)
    const int  spec = lt::ltc_evaluate_rect(g, n, view, wpos, minv, q0, q1, q2, q3, k(1.0 / (2.0 * lt::kPi)), true);
    const int  col  = nd::detail::bin(g, kir::KOp::Mul, kc(0.85, 0.75, 0.4), g.splat(g.binary(kir::KOp::Mul, spec, k(9.0)), 3));
    return nd::clamp01(g, col);
}
// B8-h CASCADED SHADOW MAPS: pure cascade-selection math visualized as a color. The sweep maps to a view depth crossing the
// three practical splits (0.5/1.0/1.5) so R = cascade index/3 (a 4-step staircase), G = the split-boundary blend factor, and
// B = a texel-snapped sweep (an 8-step staircase). No texture sampling — the full 2D-array atlas sample rides B8-k/l — so
// both backends must agree pixel-for-pixel on Step/Round/clamp arithmetic alone.
[[nodiscard]] inline int build_csm(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  vd   = g.binary(kir::KOp::Mul, sweep_node, k(2.0));                   // view depth ∈ ~[0.1, 1.9]
    const int  idx  = lt::csm_select_cascade(g, vd, k(0.5), k(1.0), k(1.5));         // {0,1,2,3}
    const int  cid  = g.binary(kir::KOp::Mul, idx, k(1.0 / 3.0));                    // R = index/3 → [0,1]
    const int  bld  = lt::csm_blend_factor(g, vd, k(1.0), k(0.1));                   // G = split blend
    const int  snp  = lt::csm_texel_snap(g, g.vec2(sweep_node, sweep_node), k(8.0)); // vec2, snapped
    const int  col  = g.vec3(cid, bld, g.swizzle(snp, 0));                           // B = snapped sweep
    return nd::clamp01(g, col);
}
// B8-i CONTACT SHADOWS: a 4-tap screen-space contact-shadow march visualized. The sweep ramps the occluder depth so the
// near end is fully contact-shadowed (all taps inside the thickness window → dark) and the far end lit (occluder behind the
// receiver → no taps occlude) — a soft contact-hardening band. Pure Step/Max/clamp arithmetic; both backends agree.
[[nodiscard]] inline int build_contact_shadow(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  occ = g.binary(kir::KOp::Add, k(0.35), g.binary(kir::KOp::Mul, sweep_node, k(0.6))); // occluder depth ramp 0.35→0.95
    const int  rz  = g.vec4(k(0.40), k(0.48), k(0.56), k(0.64));                                    // ray depth at the 4 taps
    const int  sz  = g.splat(occ, 4);                                                               // scene (occluder) depth
    const int  vis = lt::contact_shadow(g, rz, sz, k(0.01), k(0.5), k(0.9));
    const int  col = nd::detail::bin(g, kir::KOp::Mul, g.vec3(k(0.95), k(0.75), k(0.55)), g.splat(vis, 3)); // warm × visibility
    return nd::clamp01(g, col);
}
// B8-i TRANSLUCENT / DEEP SHADOWS (Fourier Opacity Maps): a participating occluder's fractional transmittance vs depth. The
// sweep is the normalized depth through the medium → the Fourier series reconstructs the optical depth → `exp(−τ)` fades the
// light smoothly (hair/foliage/smoke). Exercises Sin/Cos/Exp on the raster path; both backends within ±4.
[[nodiscard]] inline int build_fom(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  tr  = lt::fourier_opacity_transmittance(g, k(1.5), k(0.4), k(0.3), k(0.2), k(0.1), sweep_node);
    const int  col = nd::detail::bin(g, kir::KOp::Mul, g.vec3(k(0.9), k(0.85), k(0.7)), g.splat(tr, 3)); // tint × transmittance
    return nd::clamp01(g, col);
}
// B8-i VIRTUAL SHADOW MAP addressing: the clipmap-level + page-coordinate math. The sweep ramps a view-space depth → the
// clipmap level (R, a coarse staircase from log2/floor) and drives a virtual uv → the page coordinate (G/B staircases). The
// sampled pixels sit clear of the 2^n level boundaries so the floor is stable across backends.
[[nodiscard]] inline int build_vsm(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  vz  = g.binary(kir::KOp::Add, k(0.5), g.binary(kir::KOp::Mul, sweep_node, k(30.0))); // view depth 0.5→30.5
    const int  lvl = lt::vsm_clipmap_level(g, vz, k(1.0), k(6.0));
    const int  uv  = g.vec2(sweep_node, g.binary(kir::KOp::Mul, sweep_node, k(0.7)));
    const int  pg  = lt::vsm_page_coord(g, uv, k(16.0));
    const int  col = g.vec3(g.binary(kir::KOp::Mul, lvl, k(1.0 / 6.0)),
                            g.binary(kir::KOp::Mul, g.swizzle(pg, 0), k(1.0 / 16.0)),
                            g.binary(kir::KOp::Mul, g.swizzle(pg, 1), k(1.0 / 16.0)));
    return nd::clamp01(g, col);
}
// B8-j LINEAR-BLEND SKINNING: a rest point blended between bone-0 (identity) and bone-1 (translate +0.5,+0.3) by the sweep
// weight. As the sweep ramps, the skinned position slides → an R=x/G=y gradient. Exercises matvec + weighted vector sum.
[[nodiscard]] inline int build_lbs_skin(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  m0  = g.mat4(g.vec4(k(1), k(0), k(0), k(0)), g.vec4(k(0), k(1), k(0), k(0)), g.vec4(k(0), k(0), k(1), k(0)), g.vec4(k(0), k(0), k(0), k(1))); // identity
    const int  m1  = g.mat4(g.vec4(k(1), k(0), k(0), k(0)), g.vec4(k(0), k(1), k(0), k(0)), g.vec4(k(0), k(0), k(1), k(0)), g.vec4(k(0.5), k(0.3), k(0), k(1))); // translate
    const int  lw  = g.vec4(g.binary(kir::KOp::Sub, k(1.0), sweep_node), sweep_node, k(0.0), k(0.0)); // w0=1−s, w1=s
    const int  p   = g.vec3(k(0.2), k(0.2), k(0.2));
    return nd::clamp01(g, lt::lbs_skin_position(g, m0, m1, m0, m0, lw, p)); // RGB = skinned x/y/z
}
// B8-j DUAL-QUATERNION SKINNING: the same rest point blended between an identity rotation and a 90°-about-z rotation by the
// sweep weight — the volume-preserving arc LBS would collapse. Colour = skinned·0.5+0.4 (keeps the rotated point in range).
[[nodiscard]] inline int build_dquat_skin(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  r0  = g.vec4(k(0), k(0), k(0), k(1));                            // identity rotation
    const int  d0  = g.vec4(k(0), k(0), k(0), k(0));
    const int  r1  = g.vec4(k(0), k(0), k(0.70710678), k(0.70710678));          // 90° about z
    const int  d1  = g.vec4(k(0), k(0), k(0), k(0));
    const int  w0  = g.binary(kir::KOp::Sub, k(1.0), sweep_node);
    const int  sp  = lt::dquat_skin_position(g, r0, d0, r1, d1, w0, sweep_node, g.vec3(k(0.5), k(0.2), k(0.3)));
    const int  col = nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Mul, sp, k(0.5)), k(0.4));
    return nd::clamp01(g, col);
}
// B8-l DEFERRED render path: a material writes its surface to the B5 G-buffer MRT, then the deferred lighting pass DECODES the
// G-buffer + shades it (B8 Cook-Torrance). The base_color.r sweeps → a lit gradient identical to the forward path (the pack →
// decode is lossless for the direct channels) — proving the deferred path renders the SAME lit surface.
[[nodiscard]] inline int build_deferred(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace mat = crd::kir::material;
    namespace rn  = crd::kir::render;
    const auto  sh   = kir::make_shape({1});
    const auto  k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int   base = g.vec3(sweep_node, k(0.3), k(0.2));
    const int   sid  = mat::define_surface(g);
    const int   surf = mat::build_surface(g, sid, base, k(0.1), k(0.7), g.vec3(k(0.0), k(0.0), k(1.0)), g.vec3(k(0.0), k(0.0), k(0.0)), k(1.0), k(1.0));
    kir::KEntry tmp; mat::pack_gbuffer(g, tmp, surf); // surface → the 4 MRT attachments (the deferred G-buffer)
    return rn::deferred_shade(g, tmp.out[0].node, tmp.out[1].node, tmp.out[2].node, g.vec3(k(0.0), k(0.0), k(1.0)), g.vec3(k(0.3), k(0.2), k(-1.0)), g.vec3(k(0.22), k(0.22), k(0.22)));
}
// B8-l FORWARD+ clustered light culling: the exponential froxel z-slice (R staircase) + the light-sphere-vs-cluster cull (B) as
// a box slides across a light at the origin. The cull flips 1 in the middle, 0 at the edges — the per-cluster test visualized.
[[nodiscard]] inline int build_cluster(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace rn  = crd::kir::render;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  vz    = g.binary(kir::KOp::Add, k(0.2), g.binary(kir::KOp::Mul, sweep_node, k(30.0)));
    const int  slice = rn::cluster_z_slice(g, vz, k(0.1), k(100.0), k(16.0));
    const int  cxc   = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, sweep_node, k(4.0)), k(2.0)); // box centre x sweeps −2→2
    const int  amin  = g.vec3(g.binary(kir::KOp::Sub, cxc, k(0.5)), k(-0.5), k(-0.5));
    const int  amax  = g.vec3(g.binary(kir::KOp::Add, cxc, k(0.5)), k(0.5), k(0.5));
    const int  cull  = rn::light_cluster_cull(g, g.vec3(k(0.0), k(0.0), k(0.0)), k(1.0), amin, amax);
    return nd::clamp01(g, g.vec3(g.binary(kir::KOp::Mul, slice, k(1.0 / 16.0)), k(0.3), cull));
}
// B8-l CLUSTERED DECAL projection: a world point slides across a decal box (inv = scale 1.5) → decal uv (R,G) + inside test (B).
// `inside` is 1 where the point lies in the box, 0 outside; the uv ramps across.
[[nodiscard]] inline int build_decal(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace rn  = crd::kir::render;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  wp    = g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, sweep_node, k(2.0)), k(1.0)), k(0.1), k(0.0)); // world x sweeps −1→1
    const int  dminv = g.mat4(g.vec4(k(1.5), k(0), k(0), k(0)), g.vec4(k(0), k(1.5), k(0), k(0)), g.vec4(k(0), k(0), k(1.5), k(0)), g.vec4(k(0), k(0), k(0), k(1)));
    const int  proj  = rn::decal_project(g, wp, dminv);
    return g.vec3(nd::clamp01(g, g.swizzle(proj, 0)), nd::clamp01(g, g.swizzle(proj, 1)), g.swizzle(proj, 2));
}
// B8-m THE CULMINATION — build_master_direct: the DIRECT lighting of a SKINNED, textured surface. Skinning (B8-j) tilts the
// normal, the surface (B5) base_color sweeps, and the B8-a/c Cook-Torrance directional light shades it. Returned UNCLAMPED so
// the master FS can modulate it by the shadow factor before adding the (unshadowed) ambient.
[[nodiscard]] inline int build_master_direct(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace lt  = crd::kir::lighting;
    const auto sh = kir::make_shape({1});
    const auto k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    // SKINNING (B8-j): blend bone-0 (identity) with bone-1 (≈40° about x) at weight ½ → a skin-deformed normal.
    const int m0 = g.mat4(g.vec4(k(1), k(0), k(0), k(0)), g.vec4(k(0), k(1), k(0), k(0)), g.vec4(k(0), k(0), k(1), k(0)), g.vec4(k(0), k(0), k(0), k(1)));
    const int m1 = g.mat4(g.vec4(k(1), k(0), k(0), k(0)), g.vec4(k(0), k(0.765), k(0.644), k(0)), g.vec4(k(0), k(-0.644), k(0.765), k(0)), g.vec4(k(0), k(0), k(0), k(1)));
    const int w  = g.vec4(k(0.5), k(0.5), k(0.0), k(0.0));
    const int n  = lt::lbs_skin_normal(g, m0, m1, m0, m0, w, g.vec3(k(0.0), k(0.0), k(1.0)));
    const int base = g.vec3(sweep_node, k(0.3), k(0.2)); // SURFACE (B5): base_color.r sweeps
    return lt::directional_light(g, base, k(0.1), k(0.5), n, g.vec3(k(0.0), k(0.0), k(1.0)), g.vec3(k(0.3), k(0.2), k(-1.0)), g.vec3(k(0.4), k(0.4), k(0.4)));
}
// B8-m build_master_ambient: the IBL indirect (B8-e SH diffuse + split-sum specular), dimmed — the ambient floor that SURVIVES
// in shadow (a shadowed pixel keeps its indirect light; only the DIRECT term is occluded — the physically-correct composition).
[[nodiscard]] inline int build_master_ambient(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    const auto sh = kir::make_shape({1});
    const auto k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    return nd::detail::bin(g, kir::KOp::Mul, build_ibl(g, sweep_node), k(0.35));
}
// B12-a AMBIENT OCCLUSION observable: composes all four AO cores. GTAO slice + the SSILVB u32-bitmask AO (min'd) → the combined
// AO → GTAO multi-bounce re-lighting of an albedo, × the Frostbite specular-occlusion factor. The sweep varies the horizon
// angles + the sector range → an AO gradient. The SSILVB floor/ceil land clear of integers at the sampled pixels (stable u32).
[[nodiscard]] inline int build_ssao(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace scr = crd::kir::screen;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  h1   = g.binary(kir::KOp::Add, k(-0.8), g.binary(kir::KOp::Mul, sweep_node, k(0.6)));
    const int  h2   = g.binary(kir::KOp::Add, k(0.2), g.binary(kir::KOp::Mul, sweep_node, k(0.7)));
    const int  gtao = nd::clamp01(g, scr::gtao_slice(g, h1, h2, k(0.1), k(1.0)));
    const int  mn   = g.binary(kir::KOp::Mul, sweep_node, k(0.3));                                 // sector min ∈ [0,~0.29]
    const int  mx   = g.binary(kir::KOp::Add, mn, g.binary(kir::KOp::Mul, sweep_node, k(0.3)));    // width sweeps → count varies
    const int  aob  = scr::ssilvb_ao(g, scr::ssilvb_sector_mask(g, mn, mx, 32.0), 32.0);
    const int  ao   = g.binary(kir::KOp::Min, gtao, aob);                                          // combined AO
    const int  mb   = scr::gtao_multibounce(g, ao, g.vec3(k(0.8), k(0.4), k(0.2)));                // AO × albedo (multi-bounce)
    const int  so   = scr::spec_occlusion(g, k(0.7), ao, k(0.4));                                  // Frostbite specular occlusion
    return nd::clamp01(g, nd::detail::bin(g, kir::KOp::Mul, mb, g.splat(so, 3)));
}
// B12-b SCREEN-SPACE REFLECTIONS observable: reflect a view ray about a sweep-tilted normal (the reflection direction), a Hi-Z
// intersection test against a procedural surface depth, a GGX-importance-sampled rough perturbation, the screen-edge fade + the
// confidence weight — composited into a reflection that appears only where the SS trace HIT + is confident.
[[nodiscard]] inline int build_ssr(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    namespace scr = crd::kir::screen;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  nrm   = g.normalize(g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, sweep_node, k(0.4)), k(0.2)), k(0.2), k(1.0))); // tilts with sweep
    const int  rr    = scr::ssr_reflect(g, g.vec3(k(0.0), k(0.0), k(-1.0)), nrm);                                                          // reflection ray
    const int  ray_z = g.binary(kir::KOp::Add, k(0.3), g.binary(kir::KOp::Mul, sweep_node, k(0.5)));
    const int  hit   = scr::ssr_hiz_hit(g, ray_z, k(0.5), k(0.4));                                                                         // hit for sweep > 0.4
    const int  uv    = g.vec2(sweep_node, k(0.5));
    const int  ef    = scr::ssr_edge_fade(g, uv, k(0.15));
    const int  conf  = scr::ssr_confidence(g, k(0.3), ef, g.binary(kir::KOp::Mul, sweep_node, k(0.5)));
    const int  hh    = lt::importance_sample_ggx(g, uv, k(0.3));                                                                           // stochastic rough dir
    const int  refl  = g.vec3(nd::clamp01(g, g.swizzle(rr, 2)), nd::clamp01(g, g.swizzle(hh, 2)), k(0.7));                                 // reflection colour proxy
    const int  w     = g.binary(kir::KOp::Mul, hit, conf);
    return nd::clamp01(g, nd::detail::bin(g, kir::KOp::Mul, refl, g.splat(w, 3)));
}
// B12-d VOLUMETRIC LIGHTING observable: a god-ray/fog beam. The sweep is the view-light angle → the Cornette-Shanks phase peaks
// forward, Beer-Lambert attenuates through the fog, and the froxel scatter integrand tints it — a beam that brightens toward
// the light. Exercises the phase-function family + Beer-Lambert + the scatter integrand.
[[nodiscard]] inline int build_volumetric(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace scr = crd::kir::screen;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  ct  = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, sweep_node, k(2.0)), k(1.0)); // cosθ ∈ [−1,1]
    const int  ph  = scr::cornette_shanks(g, ct, k(0.6));                                          // forward-scattering phase
    const int  tr  = scr::beer_lambert(g, g.vec3(k(0.3), k(0.4), k(0.5)), sweep_node);             // fog transmittance
    return nd::clamp01(g, scr::froxel_scatter(g, g.vec3(k(1.0), k(0.9), k(0.7)), ph, tr, k(0.6))); // warm in-scatter
}
// B12-e SUBSURFACE SCATTERING observable: the Burley diffusion profile (per-channel skin falloff — red penetrates deepest) ×
// the separable Gaussian kernel weight, over a sweep-driven radius → the subsurface glow gradient. Exercises Burley + Gaussian.
[[nodiscard]] inline int build_sss(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace scr = crd::kir::screen;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  r    = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, sweep_node, k(0.5)), k(0.1)); // radius ∈ [0.1, 0.6]
    const int  prof = scr::burley_diffusion(g, r, g.vec3(k(0.5), k(0.2), k(0.1)));                  // per-channel diffusion
    const int  gw   = scr::sss_gaussian(g, r, k(0.1));                                              // separable kernel weight
    const int  col  = nd::detail::bin(g, kir::KOp::Mul, nd::detail::bin(g, kir::KOp::Mul, prof, g.splat(gw, 3)), k(0.3));
    return nd::clamp01(g, col);
}
// B12-c SSGI observable: the visibility-bitmask indirect diffuse. A bounce radiance illuminates the sectors a moving sample
// newly occludes (sample_mask & ~bitfield) — the indirect light varies as the sweep slides the sample. Exercises the SSILVB
// bit machinery (BitAnd/BitNot/BitCount) on the raster path.
[[nodiscard]] inline int build_ssgi(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace scr = crd::kir::screen;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  rad  = g.vec3(k(0.7), k(0.5), sweep_node);                                                          // bounce radiance
    const int  smpl = scr::ssilvb_sector_mask(g, g.binary(kir::KOp::Mul, sweep_node, k(0.3)), g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, sweep_node, k(0.3)), k(0.4)), 32.0);
    const int  bf   = scr::ssilvb_sector_mask(g, k(0.1), k(0.5), 32.0);                                            // current bitfield
    const int  ind  = scr::ssgi_bounce(g, rad, smpl, bf, k(0.8), 32.0);                                            // indirect contribution
    return nd::clamp01(g, nd::detail::bin(g, kir::KOp::Mul, ind, k(1.5)));
}
// B13-c HDR observable — the AgX filmic pipeline: an HDR scene ramp into the highlights → auto-exposure (EV100) → AgX tonemap.
// AgX rolls the highlights off (no hard clip to white) → the beautiful filmic gradient. Exercises ev100 + exposure + AgX.
[[nodiscard]] inline int build_hdr_agx(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace pst = crd::kir::post;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  hdr  = g.vec3(g.binary(kir::KOp::Mul, sweep_node, k(6.0)), g.binary(kir::KOp::Mul, sweep_node, k(4.0)), g.binary(kir::KOp::Mul, sweep_node, k(2.5))); // HDR, into the highlights
    const int  expo = pst::exposure_from_ev100(g, pst::ev100_from_luminance(g, k(0.5)));                       // auto-exposure multiplier
    return nd::clamp01(g, pst::agx(g, nd::detail::bin(g, kir::KOp::Mul, hdr, expo)));                          // AgX display-encoded
}
// B13-c HDR observable — the Khronos PBR-Neutral pipeline: HDR → gamut-compress → PBR-Neutral tonemap → sRGB output. Preserves
// saturated hues, compresses only the highlights. Exercises gamut_compress + pbr_neutral + srgb_encode.
[[nodiscard]] inline int build_hdr_neutral(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace pst = crd::kir::post;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  hdr = g.vec3(g.binary(kir::KOp::Mul, sweep_node, k(3.0)), g.binary(kir::KOp::Mul, sweep_node, k(1.6)), g.binary(kir::KOp::Mul, sweep_node, k(0.9)));
    return pst::srgb_encode(g, pst::pbr_neutral(g, pst::gamut_compress(g, hdr, k(0.5))));
}
// B13-c HDR observable — the PQ (ST.2084) HDR10 output transform, per channel: a normalized display luminance → PQ code value.
[[nodiscard]] inline int build_hdr_pq(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace pst = crd::kir::post;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  ln   = g.vec3(sweep_node, g.binary(kir::KOp::Mul, sweep_node, k(0.7)), g.binary(kir::KOp::Mul, sweep_node, k(0.5))); // normalized display luminance
    return nd::clamp01(g, g.vec3(pst::pq_encode(g, g.swizzle(ln, 0)), pst::pq_encode(g, g.swizzle(ln, 1)), pst::pq_encode(g, g.swizzle(ln, 2))));
}
// B13-a TAA observable — a compact temporal-resolve pixel: a current + history colour, rectified by variance_clip (→ clip_aabb)
// against the neighborhood moments, blended by taa_resolve at a luma_feedback-driven alpha, then deband-dithered by the
// temporal IGN. Exercises variance_clip / clip_aabb / luma_feedback / taa_resolve / ign_temporal / dither_apply on the raster path.
[[nodiscard]] inline int build_taa(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace taa = crd::kir::taa;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto ax   = [&](double a, double b) { return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, sweep_node, k(a)), k(b)); };
    const int  cur  = g.vec3(ax(0.6, 0.15), ax(0.5, 0.20), ax(0.4, 0.10)); // current sample
    const int  his  = g.vec3(ax(0.5, 0.25), ax(0.6, 0.10), ax(0.3, 0.30)); // reprojected history
    const int  m1   = g.vec3(ax(0.55, 0.18), ax(0.55, 0.15), ax(0.35, 0.18)); // neighborhood mean
    const int  m2   = g.vec3(ax(0.30, 0.22), ax(0.32, 0.18), ax(0.20, 0.20)); // mean of squares
    const int  clip = taa::variance_clip(g, his, m1, m2, 1.0);
    const int  luma = g.vec3(k(0.2126), k(0.7152), k(0.0722));
    const int  a    = taa::luma_feedback(g, g.dot(his, luma), g.dot(cur, luma), 0.1, 0.9);
    const int  res  = taa::taa_resolve(g, clip, cur, a);
    const int  noi  = taa::ign_temporal(g, g.vec2(g.binary(kir::KOp::Mul, sweep_node, k(32.0)), g.binary(kir::KOp::Mul, sweep_node, k(18.0))), k(3.0));
    return nd::clamp01(g, taa::dither_apply(g, res, noi, 255.0));
}
// B13-b BLOOM observable — a mini dual-filter pixel: 13 synthesized HDR taps → Karis firefly downsample → soft-knee prefilter,
// then composited over a scene colour, modulated by an aperture starburst × its spectral tint. Exercises the Karis 13-tap
// downsample / soft-knee / combine / starburst / spectral_tint on the raster path.
[[nodiscard]] inline int build_bloom(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace blm = crd::kir::bloom;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto ax  = [&](double a, double b) { return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, sweep_node, k(a)), k(b)); };
    int taps[13];
    for (int j = 0; j < 13; ++j) { const double f = 1.0 + 0.05 * j; taps[j] = g.vec3(ax(0.8 * f, 0.02 * j), ax(0.5 * f, 0.03 * j), ax(0.3 * f, 0.01 * j)); } // HDR taps (into highlights)
    const int down  = blm::downsample_karis(g, taps);          // firefly-free downsample
    const int glow  = blm::soft_knee(g, down, 0.6, 0.4);       // thresholded glow
    const int scene = g.vec3(ax(0.25, 0.05), ax(0.2, 0.07), ax(0.15, 0.1));
    const int comp  = blm::combine(g, scene, glow, k(0.6));    // composite
    const int star  = blm::starburst(g, g.binary(kir::KOp::Mul, sweep_node, k(3.0)), 6.0, 2.0);
    const int tint  = blm::spectral_tint(g, sweep_node);
    return nd::clamp01(g, nd::detail::bin(g, kir::KOp::Add, comp, nd::detail::bin(g, kir::KOp::Mul, tint, g.binary(kir::KOp::Mul, star, k(0.15)))));
}
// B13-d CINEMATIC observable — a sharp/blurred DoF composite driven by the thin-lens CoC (sweep = depth), modulated by a
// McGuire motion-blur cone weight over a scaled velocity, plus a Garcia complex-Gaussian bokeh accent. Exercises
// circle_of_confusion / dof_composite / velocity_scale / mb_cone / complex_gaussian / bokeh_realize (Exp/Cos/Sin/Sqrt/Abs).
[[nodiscard]] inline int build_cine(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace ci  = crd::kir::cinematic;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto ax  = [&](double a, double b) { return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, sweep_node, k(a)), k(b)); };
    const int  depth = ax(8.0, 1.0);                                                           // depth ∈ [1,9]
    const int  coc   = ci::circle_of_confusion(g, depth, k(4.0), 0.05, 2.8);
    const int  sharp = g.vec3(ax(0.6, 0.15), ax(0.5, 0.2), ax(0.4, 0.1));
    const int  blur  = g.vec3(ax(0.4, 0.25), ax(0.45, 0.15), ax(0.35, 0.2));
    const int  dof   = ci::dof_composite(g, sharp, blur, coc, 10.0);
    const int  vel   = ci::velocity_scale(g, g.vec2(ax(6.0, 0.0), ax(3.0, 0.0)), 0.5, 16.0);   // scaled velocity
    const int  vlen  = g.unary(kir::KOp::Sqrt, g.dot(vel, vel));
    const int  mb    = ci::mb_cone(g, g.binary(kir::KOp::Mul, sweep_node, k(4.0)), vlen);
    const int  bk    = ci::complex_gaussian(g, g.binary(kir::KOp::Mul, sweep_node, sweep_node), -4.0, 1.0); // vec2 phasor
    const int  acc   = ci::bokeh_realize(g, g.swizzle(bk, 0), g.swizzle(bk, 1), 0.7, -0.2);     // real bokeh accent
    const int  lit   = nd::detail::bin(g, kir::KOp::Mul, dof, g.binary(kir::KOp::Add, k(0.6), g.binary(kir::KOp::Mul, k(0.4), mb)));
    return nd::clamp01(g, nd::detail::bin(g, kir::KOp::Add, lit, g.binary(kir::KOp::Mul, acc, k(0.1))));
}
// B13-e FINISH observable — the finishing chain on a synthesized pixel: CAS-sharpen a 5-tap cross, blend with the film-grained
// colour, darken by the cos⁴ vignette, and lift by the geometric-specular-AA roughness. Exercises cas_sharpen / film_grain /
// vignette / specular_aa (Sqrt / Min / Max / Div / dot).
[[nodiscard]] inline int build_finish(crd::kir::KGraph& g, int sweep_node)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace fin = crd::kir::finish;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto ax  = [&](double a, double b) { return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, sweep_node, k(a)), k(b)); };
    const int  color = g.vec3(ax(0.6, 0.15), ax(0.5, 0.2), ax(0.4, 0.1));
    const int  vig   = fin::vignette(g, g.vec2(sweep_node, g.binary(kir::KOp::Mul, sweep_node, k(0.7))), g.vec2(k(0.5), k(0.5)), 1.5);
    const int  grain = fin::film_grain(g, color, sweep_node, 0.1);
    int taps[5];
    for (int t = 0; t < 5; ++t) { const double f = 1.0 + 0.1 * t; taps[t] = g.vec3(ax(0.5 * f, 0.02 * t), ax(0.45 * f, 0.03 * t), ax(0.35 * f, 0.01 * t)); }
    const int  sharp = fin::cas_sharpen(g, taps[0], taps[1], taps[2], taps[3], taps[4], 0.6);
    const int  aa    = fin::specular_aa(g, sweep_node, g.vec3(k(0.1), k(0.05), k(0.02)), g.vec3(k(0.03), k(0.08), k(0.01)), 0.5, 0.18);
    const int  base  = g.ternary(kir::KOp::Mix, grain, sharp, g.splat(k(0.5), 3));       // blend grained + sharpened
    const int  dark  = nd::detail::bin(g, kir::KOp::Mul, base, vig);                       // vignette darken
    return nd::clamp01(g, nd::detail::bin(g, kir::KOp::Add, dark, g.splat(g.binary(kir::KOp::Mul, aa, k(0.05)), 3)));
}
} // namespace lighting_obs

inline void build_lighting_csm_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_csm(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_csm_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_csm(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_contact_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_contact_shadow(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_contact_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_contact_shadow(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_fom_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_fom(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_fom_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_fom(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_lbsskin_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_lbs_skin(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_lbsskin_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_lbs_skin(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_lighting_dqskin_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_dquat_skin(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_dqskin_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_dquat_skin(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B8-k COOK SEAM: a material authored ONCE (base_color sweeps with uv, the interpolated normal, fixed metallic/roughness),
// then COOKED into a render-pass variant. The Forward variant shades the surface (B8 Cook-Torrance) into one lit colour;
// the GBuffer variant packs it into the deferred MRT. Both come from the one `build_surface` callback via `build_fs_for_pass`.
inline int cook_obs_surface(crd::kir::KGraph& g, int struct_id, const crd::kir::cook::SurfaceInputs& in, void* /*user*/)
{
    namespace kir = crd::kir;
    namespace mat = crd::kir::material;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  base = g.vec3(g.swizzle(in.uv, 0), k(0.3), k(0.2)); // base_color.r sweeps across the screen
    const int  emis = g.vec3(k(0.0), k(0.0), k(0.0));
    // roughness 0.7 → a broad, dim specular so the diffuse base_color gradient stays visible (not blown to white).
    return mat::build_surface(g, struct_id, base, k(0.1), k(0.7), in.world_normal, emis, k(1.0), k(1.0));
}
inline void build_cook_forward_into(crd::kir::KGraph& g, crd::kir::KEntry& e, int sweep)
{
    namespace kir = crd::kir;
    namespace ck  = crd::kir::cook;
    namespace mat = crd::kir::material;
    const auto        sh = kir::make_shape({1});
    const auto        k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    ck::SurfaceInputs in;
    in.uv           = g.vec2(sweep, k(0.3));
    in.world_normal = g.vec3(k(0.0), k(0.0), k(1.0));
    in.view_dir     = g.vec3(k(0.0), k(0.0), k(1.0));
    const int                  ldir = g.vec3(k(0.3), k(0.2), k(-1.0)); // key light in front → NoL > 0
    const int                  lcol = g.vec3(k(0.22), k(0.22), k(0.22)); // dim → the lit base_color gradient stays below saturation
    const ck::MaterialTemplate tmpl{cook_obs_surface, nullptr};
    ck::build_fs_for_pass(tmpl, ck::PassType::Forward, {mat::AlphaMode::Opaque, 0.5}, in, g, e, ldir, lcol);
}
inline void build_cook_forward_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    build_cook_forward_into(g, fe, sweep);
}
inline int build_cook_forward_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    kir::KEntry                e;
    build_cook_forward_into(g, e, sweep);
    crd::f64 out[4] = {0.0, 0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, e.out[0].node, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_cook_gbuffer_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace ck  = crd::kir::cook;
    namespace mat = crd::kir::material;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    ck::SurfaceInputs in;
    in.uv           = g.vec2(sweep, k(0.3));
    in.world_normal = g.vec3(k(0.0), k(0.0), k(1.0));
    in.view_dir     = g.vec3(k(0.0), k(0.0), k(1.0));
    const int                  ldir = g.vec3(k(0.3), k(0.2), k(-1.0));
    const int                  lcol = g.vec3(k(2.0), k(2.0), k(2.0));
    const ck::MaterialTemplate tmpl{cook_obs_surface, nullptr};
    ck::build_fs_for_pass(tmpl, ck::PassType::GBuffer, {mat::AlphaMode::Opaque, 0.5}, in, g, fe, ldir, lcol);
}

// B8-l render-path observables (deferred / clustered light-cull / decal) — the standard sweep→colour FS + expected pattern.
inline void build_lighting_deferred_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_deferred(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_deferred_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_deferred(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_lighting_cluster_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_cluster(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_cluster_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_cluster(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_lighting_decal_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_decal(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_decal_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_decal(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B8-m THE CULMINATION FS: a SKINNED, textured surface, LIT (Cook-Torrance direct + IBL ambient), and SHADOWED by a real PCF
// shadow-map sample — the whole B8 stack in one program. The shadow modulates the DIRECT term only; the IBL ambient survives
// (physically correct: a shadowed pixel still receives indirect light). Rendered via `draw_shadow` (comparison sampler). In the
// LIT region (shadow ≈ 1) the pixel equals `build_master_lit_expected`; in the SHADOWED region only the ambient floor remains.
inline void build_master_material_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    namespace lt  = crd::kir::lighting;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  fx    = g.swizzle(fc, 0);
    const int  fy    = g.swizzle(fc, 1);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, fx, k(0.9 / 32.0)));
    // SHADOW (B8-f/g): receiver world pos (depth ramps with x, crossing the 0.5 shadow-map depth mid-screen) → project + bias
    // → PCF-sample the uploaded shadow map. Left = lit (receiver in front), right = shadowed.
    const int  wpos = g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, fx, k(2.0 / 32.0)), k(1.0)),
                             g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, fy, k(2.0 / 32.0)), k(1.0)),
                             g.binary(kir::KOp::Mul, fx, k(1.0 / 32.0)));
    const int  lvp  = g.mat4(g.vec4(k(1.0), k(0.0), k(0.0), k(0.0)), g.vec4(k(0.0), k(1.0), k(0.0), k(0.0)), g.vec4(k(0.0), k(0.0), k(1.0), k(0.0)), g.vec4(k(0.0), k(0.0), k(0.0), k(1.0)));
    const int  proj = lt::shadow_project(g, lt::normal_offset_bias(g, wpos, g.vec3(k(0.0), k(0.0), k(1.0)), k(0.6), k(0.03)), lvp);
    const int  uv   = g.vec2(g.swizzle(proj, 0), g.swizzle(proj, 1));
    const int  dep  = g.binary(kir::KOp::Sub, g.swizzle(proj, 2), lt::slope_scaled_bias(g, k(0.6), k(0.002), k(0.01)));
    const int  tex  = g.texture(0, 1, kir::DType::F32, kir::TexDim::Tex2D, false, false, /*shadow=*/true);
    const int  samp = g.sampler(0, 2, /*shadow=*/true);
    const int  shadow  = lt::pcf_shadow(g, tex, samp, uv, dep, k(0.015), g.vec2(fx, fy));
    const int  direct  = lighting_obs::build_master_direct(g, sweep);
    const int  ambient = lighting_obs::build_master_ambient(g, sweep);
    const int  col     = nd::clamp01(g, nd::detail::bin(g, kir::KOp::Add, nd::detail::bin(g, kir::KOp::Mul, direct, g.splat(shadow, 3)), ambient));
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
// The LIT-region value (shadow = 1): clamp01(direct + ambient) — the CPU oracle the unshadowed pixels must match ±4.
inline int build_master_lit_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = nd::clamp01(g, nd::detail::bin(g, kir::KOp::Add, lighting_obs::build_master_direct(g, sweep), lighting_obs::build_master_ambient(g, sweep)));
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_ssao_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_ssao(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_ssao_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_ssao(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_ssr_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_ssr(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_ssr_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_ssr(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B13-c HDR wrappers (3 observables: AgX filmic · Khronos PBR-Neutral · PQ HDR10 output).
#define CRD_HDR_WRAP(NAME, BUILDER)                                                                                             \
    inline void build_lighting_##NAME##_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)                                           \
    {                                                                                                                          \
        namespace kir    = crd::kir;                                                                                           \
        const auto sh    = kir::make_shape({1});                                                                               \
        const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };                                       \
        const int  fc    = g.builtin(kir::KBuiltin::FragCoord);                                                                \
        const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));         \
        const int  col   = lighting_obs::BUILDER(g, sweep);                                                                    \
        fe.stage  = kir::KStage::Fragment;                                                                                     \
        fe.n_out  = 1;                                                                                                         \
        fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};                             \
    }                                                                                                                          \
    inline int build_lighting_##NAME##_expected(crd::u32 x, int channel)                                                       \
    {                                                                                                                          \
        namespace kir = crd::kir;                                                                                              \
        crd::memory::TlsfAllocator alloc(8U << 20U);                                                                           \
        kir::KGraph                g(&alloc);                                                                                  \
        const auto                 sh    = kir::make_shape({1});                                                               \
        const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32); \
        const int                  col   = lighting_obs::BUILDER(g, sweep);                                                    \
        crd::f64 out[3] = {0.0, 0.0, 0.0};                                                                                     \
        crd::kir::eval_cpu(g, nullptr, &alloc, col, out);                                                                      \
        int q = static_cast<int>(std::lround(out[channel] * 255.0));                                                          \
        if (q < 0) { q = 0; }                                                                                                 \
        if (q > 255) { q = 255; }                                                                                             \
        return q;                                                                                                             \
    }
CRD_HDR_WRAP(hdragx, build_hdr_agx)
CRD_HDR_WRAP(hdrneutral, build_hdr_neutral)
CRD_HDR_WRAP(hdrpq, build_hdr_pq)
CRD_HDR_WRAP(taa, build_taa)     // B13-a temporal-resolve observable (same single-vec3 wrapper shape)
CRD_HDR_WRAP(bloom, build_bloom) // B13-b bloom observable
CRD_HDR_WRAP(cine, build_cine)     // B13-d cinematic observable
CRD_HDR_WRAP(finish, build_finish) // B13-e finishing-chain observable
#undef CRD_HDR_WRAP

inline void build_lighting_ssgi_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_ssgi(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_ssgi_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_ssgi(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_lighting_volumetric_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_volumetric(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_volumetric_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_volumetric(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_lighting_sss_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir    = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_sss(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_sss_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_sss(g, sweep);
    crd::f64 out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_vsm_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_vsm(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_vsm_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_vsm(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_ibl_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_ibl(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_ibl_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_ibl(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_specular_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_area_specular(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_specular_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_area_specular(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}
inline void build_lighting_aniso_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_area_aniso(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_aniso_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_area_aniso(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_disk_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_area_disk(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_disk_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_area_disk(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_tube_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_area_tube(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_tube_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_area_tube(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_area_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_area_rect(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_area_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_area_rect(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_lights_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_lights(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_lights_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_lights(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B8-b thin-film + transmission observable FS + expected.
inline void build_lighting_glass_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  sweep = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_glass(g, sweep);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_glass_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh    = kir::make_shape({1});
    const int                  sweep = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_glass(g, sweep);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

inline void build_lighting_brdf_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh   = kir::make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int  rough = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0))); // 0.05→0.95
    const int  col  = lighting_obs::build(g, rough);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_brdf_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});
    const int                  rough = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build(g, rough);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B8-b LAYERED (clearcoat + sheen) FS + expected.
inline void build_lighting_layered_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh    = kir::make_shape({1});
    const auto k     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
    const int  rough = g.binary(kir::KOp::Add, k(0.05), g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(0.9 / 32.0)));
    const int  col   = lighting_obs::build_layered(g, rough);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(col, 0), g.swizzle(col, 1), g.swizzle(col, 2), k(1.0)), 0};
}
inline int build_lighting_layered_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});
    const int                  rough = g.constant(0.05 + (static_cast<double>(x) + 0.5) * (0.9 / 32.0), sh, kir::DType::F32);
    const int                  col   = lighting_obs::build_layered(g, rough);
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, col, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B7-c LOWERED material FS: the SAME overlay material as build_nodes_overlay_fs, but run through the B7 lowering pass
// (crd::kir::lower::lower_entry — const-fold + DCE + CSE) BEFORE create_program. It MUST render pixel-identically to the
// un-lowered material (57,36,14 / 244,227,210) — the end-to-end proof that lowering is round-trip bit-stable on real GPUs.
inline void build_lowered_overlay_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe, crd::u32 dim)
{
    build_nodes_overlay_fs(g, fe, dim);
    crd::kir::lower::lower_entry(g, fe); // optimize the material graph in place; the entry's outputs are renumbered
}

// B6-b NOISE FS: MaterialX perlin noise on the GPU — the acid test for the U32 Bob-Jenkins hash (logical `>>`, 32-bit
// wraparound) emitting correctly through GLSL(uint)/HLSL(uint). Coordinate = (FragCoord.x * kNoiseScale, 0.5) — using only
// FragCoord.x sidesteps the Vulkan/DX12 FragCoord.y flip, so every column has a deterministic, backend-identical perlin
// value. Output grayscale = perlin*0.5+0.5. A pixel's expected value is computed by evaluating the SAME node library at the
// SAME coordinate through the F32 CPU oracle (build_noise_perlin_expected below) — so GPU == the library's own F32 eval.
inline constexpr double kNoiseScale = 0.15;

inline void build_noise_perlin_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nz  = crd::kir::nodes::noise;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord);
    const int  cx  = g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(kNoiseScale));
    const int  cy  = k(0.5);
    const int  n   = nz::perlin2(g, cx, cy);
    const int  gray = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, n, k(0.5)), k(0.5)); // perlin*0.5+0.5 → [0,1]
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(gray, gray, gray, k(1.0)), 0};
}

// B6-b WORLEY FS: worley2 (euclidean nearest-cell distance) at (FragCoord.x*scale, 0.5) → grayscale. Exercises the 3×3
// cell search + Select min-tracking on the GPU (the U32 hash already proven by perlin). Distance ∈ [0,~0.7] → clamp01.
inline void build_noise_worley_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nz  = crd::kir::nodes::noise;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord);
    const int  cx  = g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(kNoiseScale));
    const int  w   = nz::worley2(g, cx, k(0.5), 1.0, 0, 0);                         // jitter 1, style 0, euclidean
    const int  gray = kir::nodes::clamp01(g, w);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(gray, gray, gray, k(1.0)), 0};
}

// B6-d NPR FS: gooch_shade with the normal driven by FragCoord.x (so N·L sweeps → the warm/cool gradient varies across the
// row). Exercises normalize/dot/reflect/mix/pow on the raster path. Outputs the gooch colour; the test checks the R channel.
inline void build_npr_gooch_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto v3  = [&](double a, double b, double c) { return g.vec3(k(a), k(b), k(c)); };
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord);
    const int  nrm = g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(1.0 / 16.0)), k(1.0)), k(0.3), k(1.0)); // (x/16 - 1, 0.3, 1)
    const int  res = nd::gooch_shade(g, nrm, v3(0.0, 0.0, 1.0), v3(0.8, 0.8, 0.7), v3(0.3, 0.3, 0.8), k(0.9), k(32.0), v3(1.0, -0.5, -0.5));
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(g.swizzle(res, 0), g.swizzle(res, 1), g.swizzle(res, 2), k(1.0)), 0};
}
inline int build_npr_gooch_expected(crd::u32 x, int channel)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});
    const auto                 k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto                 v3 = [&](double a, double b, double c) { return g.vec3(k(a), k(b), k(c)); };
    const int                  nrm = g.vec3(g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, k(static_cast<double>(x) + 0.5), k(1.0 / 16.0)), k(1.0)), k(0.3), k(1.0));
    const int                  res = nd::gooch_shade(g, nrm, v3(0.0, 0.0, 1.0), v3(0.8, 0.8, 0.7), v3(0.3, 0.3, 0.8), k(0.9), k(32.0), v3(1.0, -0.5, -0.5));
    crd::f64                   out[3] = {0.0, 0.0, 0.0};
    crd::kir::eval_cpu(g, nullptr, &alloc, res, out);
    int q = static_cast<int>(std::lround(out[channel] * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// B6-c UV FS: place2d (rotate 30°, scale 2, pivot .5) on a FragCoord-derived UV → outputs the transformed U as grayscale.
// Exercises Radians/Sin/Cos on the raster path (the rotate2d core). texcoord = (FragCoord.x/dim, 0.5).
inline void build_uv_place2d_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    const auto sh  = kir::make_shape({1});
    const auto k   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  fc  = g.builtin(kir::KBuiltin::FragCoord);
    const int  tc  = g.vec2(g.binary(kir::KOp::Mul, g.swizzle(fc, 0), k(1.0 / 32.0)), k(0.5));
    const int  piv = g.vec2(k(0.5), k(0.5));
    const int  scl = g.vec2(k(2.0), k(2.0));
    const int  ofs = g.vec2(k(0.0), k(0.0));
    const int  res = nd::place2d(g, tc, piv, scl, k(30.0), ofs, 0);
    const int  gray = nd::clamp01(g, g.swizzle(res, 0));
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(gray, gray, gray, k(1.0)), 0};
}
inline int build_uv_place2d_expected(crd::u32 x)
{
    namespace kir = crd::kir;
    namespace nd  = crd::kir::nodes;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});
    const auto                 k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int                  tc = g.vec2(g.binary(kir::KOp::Mul, k(static_cast<double>(x) + 0.5), k(1.0 / 32.0)), k(0.5));
    const int                  res = nd::place2d(g, tc, g.vec2(k(0.5), k(0.5)), g.vec2(k(2.0), k(2.0)), k(30.0), g.vec2(k(0.0), k(0.0)), 0);
    const int                  gray = nd::clamp01(g, g.swizzle(res, 0));
    crd::f64                   out  = 0.0;
    crd::kir::eval_cpu(g, nullptr, &alloc, gray, &out);
    int q = static_cast<int>(std::lround(out * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// The matching F32 CPU-oracle expected grayscale (unorm8) for pixel column `x`: FragCoord.x = x + 0.5, so evaluate
// perlin2((x+0.5)*kNoiseScale, 0.5) in F32 exactly as the shader does, then map *0.5+0.5 and quantise to 8-bit.
inline int build_noise_perlin_expected(crd::u32 x)
{
    namespace kir = crd::kir;
    namespace nz  = crd::kir::nodes::noise;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});
    const int                  cx = g.binary(kir::KOp::Mul, g.constant(static_cast<double>(x) + 0.5, sh, kir::DType::F32), g.constant(kNoiseScale, sh, kir::DType::F32));
    const int                  cy = g.constant(0.5, sh, kir::DType::F32);
    const int                  n  = nz::perlin2(g, cx, cy);
    const int gray = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, n, g.constant(0.5, sh, kir::DType::F32)), g.constant(0.5, sh, kir::DType::F32));
    crd::f64  out  = 0.0;
    crd::kir::eval_cpu(g, nullptr, &alloc, gray, &out);
    int q = static_cast<int>(std::lround(out * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

// The matching F32 expected grayscale (unorm8) for the worley observable at pixel column `x` — worley2 clamped to [0,1].
inline int build_noise_worley_expected(crd::u32 x)
{
    namespace kir = crd::kir;
    namespace nz  = crd::kir::nodes::noise;
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});
    const int                  cx = g.binary(kir::KOp::Mul, g.constant(static_cast<double>(x) + 0.5, sh, kir::DType::F32), g.constant(kNoiseScale, sh, kir::DType::F32));
    const int                  w  = nz::worley2(g, cx, g.constant(0.5, sh, kir::DType::F32), 1.0, 0, 0);
    const int                  gray = kir::nodes::clamp01(g, w);
    crd::f64                   out  = 0.0;
    crd::kir::eval_cpu(g, nullptr, &alloc, gray, &out);
    int q = static_cast<int>(std::lround(out * 255.0));
    if (q < 0) { q = 0; }
    if (q > 255) { q = 255; }
    return q;
}

} // namespace crd::gputest
