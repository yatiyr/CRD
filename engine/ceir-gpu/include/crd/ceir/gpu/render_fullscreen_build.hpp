#pragma once

// crd-ceir-gpu — CEIR-16-3b: the FULLSCREEN COMPOSITE BUILDER. `build_fullscreen_ceir` constructs the fullscreen executor's
// internal composite (a `render.scope` with one procedural 3-vertex draw over its declared reads) as a `ceir.render` program,
// verifies it (`find_render_misuse`), and lowers it to an inspectable `LoweredCommand` plan — the per-pass plan the generic
// replay `PassRecordFn` (CEIR-16-3c) executes through `execute_render_lowered`. ⛔ This REPLACES the imperative per-encoder
// `record_fullscreen_raster` (render-graph): the composite becomes DATA (IR) run by ONE generic executor, so the routing that
// used to be `if(n==1 && depth) bind_atlas(...)` encoder calls is now the BINDING DECLARATIONS this builder emits (their CEIR
// type selects the kind — CEIR-16-2/3a). ⛔ The composite is fully DERIVED from the frame.toml `raster.fullscreen` pass
// (reads/writes/shader/params) — there is deliberately NO second on-disk `ceir.render` authoring format (that would be a
// second source of truth that drifts); this builder is the same "high-level authoring → cook to CEIR" shape as `.frame.toml`
// itself (advisor 2026-08-12). The CALLER (render-graph graph-build) owns the extraction of a `PassPayload` into a
// `FullscreenBuildDesc` — keeping crd-ceir-gpu ⊥ crd-render-pass (no layering inversion; this header names only CEIR + gpu enums).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/lower.hpp>       // LoweredCommand
#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/raster_context.hpp>   // BlendMode / ClearColor / ShadingRate / ConservativeMode

namespace crd::ceir::gpu
{
// One declared read of the fullscreen composite. `source_param` is the pass-parameter identity the RECORD-time resolver maps
// this binding back to (`pass_param_id("input0")` …) — ⭐ CEIR-16-3b advisor constraint #2: the descriptor SLOT (emitted as
// the binding's `slot` attr) is NOT the source-param identity; the resolver needs BOTH (slot → the shader register, source →
// which RecordContext texture). `is_depth` drives the n==1 shadow-atlas routing (a depth read → the atlas tex@4 + a comparison
// sampler@5), UNLESS `depth_as_float` (the HZB/TAA raw-depth read) samples it through a plain sampler.
struct FullscreenInput
{
    crd::u64 source_param = 0;     // pass_param_id("inputN") — the resolver's operand → RecordContext.texture(source_param)
    bool     is_depth     = false; // a depth texture (→ shadow-atlas routing at n==1 unless depth_as_float)
};

// The payload-derived recipe for one fullscreen pass's composite. The CALLER extracts this from the pass `PassPayload`
// (params + reads/writes) — this struct carries NO device objects and NO renderpass types (the layering boundary). The color
// TARGET, the raster PROGRAM, and each read's TEXTURE are all resolved at RECORD (CEIR-16-3c) — never baked here.
struct FullscreenBuildDesc
{
    // ── the color attachment (writes[0]) ──
    bool                       load          = false;                        // load the target (WBOIT composite) vs clear it
    crd::gpu::BlendMode        blend         = crd::gpu::BlendMode::Opaque;  // blend the draw over the target (composite)
    crd::gpu::ClearColor       clear         = {};                           // the clear colour when !load
    // ── the draw state ──
    crd::gpu::ShadingRate      shading_rate  = crd::gpu::ShadingRate::Rate1x1;
    crd::gpu::ConservativeMode conservative  = crd::gpu::ConservativeMode::Off;
    // ── the declared reads (input0..N, in binding order) ──
    FullscreenInput            inputs[8]     = {};
    crd::u32                   num_inputs    = 0U;
    bool                       depth_as_float = false;                       // a depth input sampled as float (HZB/TAA), no comparison
    crd::u64                   constants_param = 0U;                         // pass_param_id("constants"); 0 ⇒ no constants buffer
};

// Build the fullscreen composite described by `desc` into `ctx` (⛔ a FRESH caller-owned Context — this registers the arith /
// resource / render dialects on it; the caller MUST keep `ctx` alive as long as `out_plan` is used, since the lowered
// commands hold `Operation*`s into `ctx`). Verifies the emitted program with `find_render_misuse` (the verifier-first
// contract `execute_render_lowered` assumes) and, on success, lowers it into `out_plan` (BeginRender → Draw → EndRender).
// `out_plan` is CLEARED then filled. Returns false on a malformed `desc` (e.g. > 8 inputs) or a `find_render_misuse`
// rejection (the builder's own correctness check) — leaving `out_plan` empty.
[[nodiscard]] bool build_fullscreen_ceir(Context& ctx, const FullscreenBuildDesc& desc,
                                         containers::Array<LoweredCommand>& out_plan);

// ── CEIR-16-mesh-1: the MESH-INDIRECT COMPOSITE BUILDER (record_mesh_indirect). A GPU-driven meshlet dispatch whose group
// counts are read from an `%args` buffer — one `render.scope` (clearing the sole colour target) with one
// `render.mesh_dispatch_indirect(%args)`. NO descriptor bindings + NO draw-list loop (a single static dispatch), so it reuses
// the fullscreen playbook verbatim; the per-draw amplify loop is a separate builder (16b-mesh-2). ──
struct MeshIndirectBuildDesc
{
    crd::u64             args_param  = 0U; // pass_param_id("args") — the resolver maps the %args operand → RecordContext.storage(args_param)
    crd::u64             args_offset = 0U; // byte offset into %args (u32_param "args_offset")
    crd::gpu::ClearColor clear       = {}; // the colour attachment clear (record_mesh_indirect always clears: clear_from(payload))
};

// Build the mesh-indirect composite described by `desc` into `ctx` (a FRESH caller-owned Context; same lifetime contract as
// build_fullscreen_ceir — `ctx` must outlive `out_plan`). Verifies with `find_render_misuse` and lowers to `out_plan`
// (BeginRender → Draw(DispatchMeshIndirect) → EndRender). `out_plan` is CLEARED then filled. Returns false on a
// `find_render_misuse` rejection.
[[nodiscard]] bool build_mesh_indirect_ceir(Context& ctx, const MeshIndirectBuildDesc& desc,
                                            containers::Array<LoweredCommand>& out_plan);

// ── CEIR-16-mesh-2: the AMPLIFY COMPOSITE BUILDER (record_amplify_raster — mesh.raster + tess.raster). ONE `render.scope`
// (clearing the sole colour target) with ONE `render.mesh_dispatch_list` that the record-time walk EXPANDS over the host
// DrawList into N per-item draws (a mesh dispatch when `patches=false`, a patch draw when `patches=true`). No operands, no
// binding tail — the per-item program/count/storage are DrawList data resolved at record. ──
struct AmplifyBuildDesc
{
    bool                 patches        = false; // the primitive: false = meshlet (mesh.raster) | true = patches (tess.raster)
    crd::u32             fallback_count = 0U;    // the amplify_count PROCEDURAL arm (draws.count==0 -> one default-program draw)
    crd::gpu::ClearColor clear          = {};    // the colour attachment clear (record_amplify_raster clears; a dispatch may not cover all pixels)
};

// Build the amplify composite described by `desc` into `ctx` (a FRESH caller-owned Context; same lifetime contract as
// build_fullscreen_ceir). Verifies with `find_render_misuse` and lowers to `out_plan` (BeginRender → Draw(mesh_dispatch_list)
// → EndRender). `out_plan` is CLEARED then filled. Returns false on a `find_render_misuse` rejection.
[[nodiscard]] bool build_amplify_ceir(Context& ctx, const AmplifyBuildDesc& desc,
                                      containers::Array<LoweredCommand>& out_plan);

// ── CEIR-16d: the SCENE COMPOSITE BUILDER (record_scene_raster — scene.raster's single-colour arm). ONE render.scope over an
// OPTIONAL colour attachment (absent ⇒ a DEPTH-ONLY shadow cascade / depth-prepass — a 0-COLOUR scope) + an OPTIONAL depth
// attachment, containing ONE `render.scene_draw_list` the record-time walk EXPANDS over the host DrawList into the per-item
// verb ladder (indirect / indexed-sampled / combined-tex / plain-coalesced). No operands, no binding tail — every per-item
// field (program/storage/texture/args/counts) + the pass atlas are DrawList/resolver data at record. ⛔ the mrt>=2 G-buffer
// arm (n_writes>1) is a SEPARATE representation (a later sub-slice), NOT built here.
struct SceneBuildDesc
{
    bool                   has_color     = true;                              // a colour attachment (false ⇒ depth-only: 0-colour scope)
    bool                   has_depth     = false;                             // a depth attachment present
    bool                   load          = false;                             // colour LoadOp: load (a `load=true` pass STACKS) vs clear
    crd::gpu::ClearColor   clear         = {};                                // the colour clear when !load
    bool                   load_depth    = false;                             // depth LoadOp: load (a depth-prepass consumer) vs clear
    crd::f32               clear_depth   = 1.0F;                              // the depth clear (a reverse-Z scene clears to 0)
    crd::gpu::DepthCompare depth_compare = crd::gpu::DepthCompare::LessEqual; // ⛔ the live scene is REVERSE-Z (GreaterEqual): a PARAM, not a constant
};

// Build the scene composite described by `desc` into `ctx` (a FRESH caller-owned Context; same lifetime contract as
// build_fullscreen_ceir). Verifies with `find_render_misuse` and lowers to `out_plan` (BeginRender → Draw(scene_draw_list) →
// EndRender). `out_plan` is CLEARED then filled. Returns false on `!has_color && !has_depth` (a scope needs an attachment) or
// a `find_render_misuse` rejection.
[[nodiscard]] bool build_scene_ceir(Context& ctx, const SceneBuildDesc& desc,
                                    containers::Array<LoweredCommand>& out_plan);
} // namespace crd::ceir::gpu
