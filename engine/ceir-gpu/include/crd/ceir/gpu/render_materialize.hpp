#pragma once

// CEIR-14z-1: the render MATERIALIZERS — PURE functions that turn a CEIR `render.scope` op into a command_model
// `RenderingDesc`, and a `render.draw*` op into a `RasterDrawPacket`, for the 14z RASTER executor (`execute_render_lowered`,
// 14z-2). ⛔ Option A (user-ratified 2026-08-11): a TEST-SURFACE bridge onto `ICommandEncoder`; real frame-graph integration
// is CEIR-15/16, reversible. The concrete `IRasterTarget` / `IRasterProgram` / resource pointers come from caller RESOLVERS
// (the op → the device object); the materializers own the CEIR-attr → command_model-enum mapping ONLY (host-only inputs, no
// device). ⛔ assumes `find_render_misuse` passed (the verifier-first contract, as `execute_lowered` assumes find_dispatch).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gpu/execute.hpp> // ExecuteError
#include <crd/ceir/gpu/lower.hpp>   // LoweredCommand / LoweredKind
#include <crd/gpu/command_model.hpp>
#include <crd/gpu/raster_context.hpp> // BlendMode / DepthCompare / ClearColor / IRasterTarget / IRasterProgram / IFrameGraph

#include <crd/memory/allocator.hpp> // CEIR-14z-4a: the per-scope pass closures (execute_render_frame)

namespace crd::ceir::gpu
{
// Resolve a render.color_attachment / render.depth_attachment op to its device target (op → IRasterTarget).
using RasterTargetResolveFn = crd::gpu::IRasterTarget* (*)(const Operation* attachment_op, void* user);
// Resolve a render.draw* op's @program identity to its assembled raster program (op → IRasterProgram — DISTINCT from the
// 13z `IGpuProgram`: a raster program is VS+FS assembled via create_raster_program).
using RasterProgramResolveFn = crd::gpu::IRasterProgram* (*)(const Operation* draw_op, void* user);
// CEIR-14z-4c: resolve a render.draw* BINDING OPERAND (a resource Value in the draw's variadic binding tail) → its device
// storage buffer (Value → IStorageBuffer). ⛔ operand-level (a Value*), the same shape the 13z compute path resolves, typed
// to IStorageBuffer*. ⛔ FUTURE (14z-5/6): when a FOURTH resolver appears (index buffers / indirect args), bundle these four
// into a `RenderResolvers` struct rather than growing the parameter lists.
using RasterBindingResolveFn = crd::gpu::IStorageBuffer* (*)(const Value* binding_operand, void* user);
// CEIR-16: resolve an IMAGE-typed render.draw* binding operand → its device sampled texture (Value → ITexture). This is the
// FOURTH resolver, so — per the 14z-5/6 note above — the four now bundle into `RenderResolvers` rather than growing the lists.
using RasterTextureResolveFn = crd::gpu::ITexture* (*)(const Value* binding_operand, void* user);
// CEIR-16-3a: resolve a RESOURCE-TABLE-of-image binding operand → its device BINDLESS texture array (Value → ITexture*[]).
// The N-ness lives BEHIND the resolver (it returns the array pointer + writes the element `out_count`), so the fullscreen
// composite's n>1 / blend-load-of-one arms collapse to ONE typed IR binding (the authored-variant plan) — kind-from-type
// still holds: the RESOURCE-TABLE type selects BindlessTextureArray exactly as an IMAGE type selects SampledTexture.
using RasterTextureArrayResolveFn = crd::gpu::ITexture* const* (*)(const Value* binding_operand, void* user, crd::u32& out_count);
// CEIR-16c: one resolved SCENE-DRAW item — the ceir-gpu-neutral PROJECTION of render-graph's `RenderDrawItem` (the layering
// boundary: ceir-gpu never sees render-graph types, so the host maps its RenderDrawItem → this view ON DEMAND). A
// `render.mesh_dispatch_list` (amplify) reads the {program, vertex_count, storage} SUBSET (vertex_count = the task-mesh
// groups / patches count); a `render.scene_draw_list` (16d scene) reads the FULL set — the per-item verb ladder
// (record_scene_raster): `args`≠null ⇒ indirect; `index_count`>0 ⇒ indexed-pull (index_count/instance_count/first_index);
// else non-indexed `vertex_count`. `program`/`storage`/`texture`/`args` null ⇒ that binding is absent (⇒ the pass default).
// ⛔ `dispatch_groups` (RenderDrawItem's RAF-8 COMPUTE field) is DELIBERATELY absent — a raster scene item never dispatches.
struct RasterDrawItem
{
    crd::gpu::IRasterProgram* program        = nullptr; // per-item program (beats the pass default)
    crd::gpu::IStorageBuffer* storage        = nullptr; // per-item vertex-/storage-pull buffer (Object-frequency binding)
    crd::gpu::ITexture*       texture        = nullptr; // per-item albedo MAP (beats the pass sampled atlas)
    crd::gpu::IStorageBuffer* args           = nullptr; // != null ⇒ GPU-driven indirect (device-memory command)
    crd::u32                  vertex_count   = 0U;      // non-indexed draw count (amplify: the group / patch count)
    crd::u32                  index_count    = 0U;      // > 0 ⇒ indexed-pull draw
    crd::u32                  instance_count = 0U;
    crd::u32                  first_index    = 0U;
    crd::u32                  args_offset    = 0U;
    bool                      indexed        = false;   // the draw-index-rebased contract (the multi verb)
};
// Resolve the DrawList a `render.mesh_dispatch_list` / `render.scene_draw_list` expands over — WITHOUT the resolver owning a
// mapped array: `draws_count` returns the item count; `draws_item` fills item `index` (index < count). The render-graph maps
// its RenderDrawItem → this ceir-gpu-neutral view ON DEMAND, so the host DrawList (owned by the RecordContext for the whole
// record frame) is the only backing store — no lifetime/allocation snag.
using RasterDrawCountFn = crd::u32 (*)(void* user);
using RasterDrawItemFn  = void (*)(void* user, crd::u32 index, RasterDrawItem& out);
// CEIR-16d: resolve the PASS-level sampled atlas a `render.scene_draw_list` binds per-draw (the shadow / moment atlas a scene
// pass reads, e.g. DrawList::pass_texture) + its binding shape. Returns null ⇒ an UNTEXTURED pass (no per-pass atlas). Writes
// `out_is_depth` (a depth OR arrayed atlas binds at the ATLAS slot 4/5, never the base-colour map slot 1) and `out_comparison`
// (⛔⛔ REN-40-D: true ⇒ a COMPARISON/PCF sampler for a depth atlas, false ⇒ a PLAIN filtering sampler for a moment/variance
// COLOUR array — conflating them rendered every moment shadow black).
using RasterPassTextureFn = crd::gpu::ITexture* (*)(void* user, bool& out_is_depth, bool& out_comparison);
// CEIR-16d: the SCENE draw-run COALESCING cap — a consecutive run of plain-compatible items batches into ONE multi verb of at
// most this many draws (the batching perf contract, one descriptor reset per run). ⛔ SINGLE SOURCE OF TRUTH: the legacy
// record_scene_raster (frame_graph.cpp) references THIS constant so the CEIR scene template + the legacy recorder coalesce
// IDENTICALLY — pixel A/B parity depends on the run boundaries matching. A run's per-draw count/index arrays are stack-sized
// by this, so it also bounds that stack footprint.
inline constexpr crd::u32 kMaxSceneRun = 256U;

// ⛔ CEIR-17b: the SCENE-RESOLVE host-callback family — the host impl of the scene.resolve_* intrinsics (17a). The
// currency is an OPAQUE u64 HANDLE (`SceneResolveHandle`), NOT a device pointer: the values are ids (§22-18 id-keyed —
// a material is a 128-bit ResourceId table-INDEXED host-side; a resolved program's u64 is reinterpreted to
// IRasterProgram* AT THE SEAM by the host callback, never by CEIR — I3/I4). A chain callback takes the RESOLVED UPSTREAM
// handle(s) (material→technique→program), never the op / value-map — evaluate_scene_resolve owns the op-attr reading (the
// .valid() scar in ONE place) + the value→handle binding. 0 is the NULL/unresolvable handle. ⛔ crd-ceir core never sees
// these — they live here on the ceir-gpu (handles) side.
using SceneResolveHandle = crd::u64;
using SceneResolveMaterialFn  = SceneResolveHandle (*)(void* user, SceneResolveHandle draw);
using SceneResolveTechniqueFn = SceneResolveHandle (*)(void* user, SceneResolveHandle material, crd::containers::StringView phase);
using SceneResolveProgramFn   = SceneResolveHandle (*)(void* user, SceneResolveHandle technique, SceneResolveHandle draw);
using SceneResolveGeometryFn  = SceneResolveHandle (*)(void* user, SceneResolveHandle draw);

// CEIR-16 (14z-5/6): the render resolvers bundled — one struct threaded through materialize/execute instead of a growing
// tail of (fn,user) pairs. Each resolver keeps its OWN `user` (the device objects differ per resolver). `storage` resolves a
// BUFFER-typed binding operand → IStorageBuffer; `texture` resolves an IMAGE-typed one → ITexture — the CEIR-3c TYPE of the
// binding operand selects which (the 12a one-source-of-truth doctrine; the binding KIND is never a slot/attr).
struct RenderResolvers
{
    RasterTargetResolveFn  target       = nullptr;
    void*                  target_user  = nullptr;
    RasterProgramResolveFn program      = nullptr;
    void*                  program_user = nullptr;
    RasterBindingResolveFn storage      = nullptr;
    void*                  storage_user = nullptr;
    RasterTextureResolveFn texture      = nullptr;
    void*                  texture_user = nullptr;
    RasterTextureArrayResolveFn texture_array      = nullptr;
    void*                       texture_array_user = nullptr;
    // CEIR-16-mesh-2: the host DrawList a `render.mesh_dispatch_list` / `render.scene_draw_list` expands over — count +
    // per-index item accessor.
    RasterDrawCountFn           draws_count        = nullptr;
    RasterDrawItemFn            draws_item         = nullptr;
    void*                       draws_user         = nullptr;
    // CEIR-16d: the PASS-level sampled atlas a `render.scene_draw_list` binds per-draw (null ⇒ untextured pass).
    RasterPassTextureFn         pass_texture       = nullptr;
    void*                       pass_texture_user  = nullptr;
    // ⛔ CEIR-16d-live-2b: the PER-INSTANCE load override. A cooked scene plan bakes only the AUTHORED base load (kLoad);
    // the FRAME-VARYING per-for_each-instance `load_override` (shadow-cascade caching REN-40-E2) cannot be baked, so the
    // record fn passes it here. When true, execute_render_lowered forces every colour + the depth LoadOp to Load at
    // BeginRender (MONOTONE Clear→Load; never Store, never the clear values) — matching legacy record_scene_raster's
    // `load` / `load ‖ load_depth`. Idempotent when the base already baked Load. The depth's kLoadDepth arm has NO
    // force-side read (only `load` rides here), so the baked plan's depth load stays load-bearing — never strip it.
    bool                        force_load         = false;
    // ⛔ CEIR-17b: the SCENE-RESOLVE host-callback family — evaluate_scene_resolve invokes these to run a scene.resolve_*
    // chain through the host (17a's intrinsics). Each keeps its own `user`. A null callback that the chain needs ⇒
    // ExecuteError::UnresolvedSceneHandle (the seam is unwired) — never a garbage handle.
    SceneResolveMaterialFn      resolve_material   = nullptr;
    void*                       resolve_material_user = nullptr;
    SceneResolveTechniqueFn     resolve_technique  = nullptr;
    void*                       resolve_technique_user = nullptr;
    SceneResolveProgramFn       resolve_program    = nullptr;
    void*                       resolve_program_user = nullptr;
    SceneResolveGeometryFn      resolve_geometry   = nullptr;
    void*                       resolve_geometry_user = nullptr;
};

// ⛔ CEIR-17b: the resolved handles of a scene.resolve_* chain (0 = a kind the chain did not resolve). The output of
// evaluate_scene_resolve — the currency the migrated scene draw-build (17c) will read INSTEAD of pre-resolving in C++.
struct SceneResolvedHandles
{
    SceneResolveHandle material  = 0;
    SceneResolveHandle technique = 0;
    SceneResolveHandle program   = 0;
    SceneResolveHandle geometry  = 0;
};

// Materialize a render.scope op into a RenderingDesc: width/height/sample_count from the scope attrs; each attachment
// operand's DEFINING color/depth_attachment op → a ColorAttachmentDesc / DepthStencilAttachmentDesc (LoadOp/StoreOp/
// typed-clear[⭐ RAH-1a.1: clear_kind=uint → ClearKind::Uint + clear_uint]/blend/compare mapped from the CEIR string attrs;
// the target via `resolver`). ⛔ `out` is fully overwritten. Returns false on a malformed scope (an operand not defined by
// an attachment op, or > kMaxColorAttachments color attachments).
[[nodiscard]] bool materialize_rendering_desc(const Context& ctx, const Operation* scope_op, RasterTargetResolveFn resolver,
                                              void* user, crd::gpu::RenderingDesc& out);

// Materialize a render.draw* op into a RasterDrawPacket: the RasterCommandKind + GeometryKind from the op name; the counts
// const-folded from the operands; first_vertex/first_index/max_draws from attrs; the program via `resolvers.program`.
// ⭐ CEIR-14z-4c / CEIR-16: if a binding resolver is set, each operand in the draw's variadic binding tail (from
// `render_draw_binding_start`) is resolved by its CEIR-3c TYPE — a BUFFER-typed operand → `resolvers.storage` → a
// StorageBuffer `ResourceBinding`; an IMAGE-typed operand → `resolvers.texture` → a SampledTexture (CEIR-16-2); a
// RESOURCE-TABLE-of-image operand → `resolvers.texture_array` → a BindlessTextureArray (count behind the resolver,
// CEIR-16-3a). A binding that resolves to NULL → returns false (a draw that bound 1-of-N would render garbage from the wrong resource,
// pixel-plausibly — a typed failure, never a silent skip). Returns false on a malformed draw (unknown op name, a dynamic
// count that cannot const-fold, or a null binding).
[[nodiscard]] bool materialize_draw_packet(const Context& ctx, const Operation* draw_op, const RenderResolvers& resolvers,
                                           crd::gpu::RasterDrawPacket& out);

// CEIR-14z-2: execute a render-lowered command list on an ICommandEncoder (the RASTER executor, Option A test-surface):
// BeginRender → materialize_rendering_desc → encoder.begin_rendering; Draw → materialize_draw_packet → encoder.draw;
// EndRender → encoder.end_rendering. ⛔ a Barrier is INERT here (encoder-surface barriers are the frame graph's job —
// CEIR-15/16); a Dispatch/Transfer in a render list → UnsupportedCommand (this executor is render-only; a mixed
// compute+render CEIR program is 15/16). A null program/target from a resolver → UnresolvedProgram; a draw whose count
// cannot const-fold → UnsupportedCommand. ⛔ assumes find_render_misuse passed (the verifier-first contract).
[[nodiscard]] ExecuteError execute_render_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                                  crd::gpu::ICommandEncoder& encoder, const RenderResolvers& resolvers);

// ⛔ CEIR-17b: EVALUATE a scene.resolve_* chain (17a) through the host. Walk the resolve ops in `m` (pre-order, recursing
// regions), binding each op-result → an opaque host handle by calling the matching RenderResolvers callback with the
// RESOLVED upstream handle(s) (material→technique→program). The chain's scene.draw-typed INPUT value `draw_seed` maps to
// `draw_handle`. Fills `out` (0 for a kind the chain omits). ⛔ VERIFIER-FIRST: runs scene::find_scene_misuse first →
// SceneChainMisuse on a mis-typed chain (refuse — never a garbage handle). A null callback the chain NEEDS ⇒
// UnresolvedSceneHandle. The evaluator owns the phase-attr read (the .valid() scar, ONE place) + the value→handle map;
// the callbacks are pure host lookups. (17b is the SEAM + evaluator + sentinel tests; the REAL REN-37 ladder + the
// draw-handle table are 17c — this proves the CHAIN THREADS, not yet "== the C++ path's handles".)
[[nodiscard]] ExecuteError evaluate_scene_resolve(Context& ctx, const Module& m, const RenderResolvers& resolvers,
                                                  const Value* draw_seed, SceneResolveHandle draw_handle,
                                                  SceneResolvedHandles& out);

// CEIR-14z-4a (the GOLD-STANDARD drive): execute a render-lowered command list through the raster context's FRAME-RECORDING
// mode (ADR-0126 / the user-ratified gold-standard pull-forward 2026-08-11), NOT a standalone synchronous encoder. Each
// `render.scope` (BeginRender…EndRender) becomes ONE frame-graph pass: its color/depth targets are imported + declared
// `writes`, the frame graph DERIVES cross-pass barriers + owns end-of-frame readback, and the pass's record callback drives
// the EXISTING `execute_render_lowered` walk on that scope's command slice through a frame-recording `ICommandEncoder`. This
// is the ONLY mode where the frame-recording verbs (MRT / indexed-indirect / mesh / bindless — all `if(!frame_recording())`)
// execute device-side via their real bodies. Mirrors `crd::rendergraph::execute_frame`. ⛔ lowered `Barrier`s stay INERT
// (the fg derives barriers from the declared reads/writes; CEIR-barrier ↔ fg-barrier reconciliation is CEIR-15/16). Returns
// `NoFrameGraph` if the backend lacks a frame graph, `FrameBuildFailed` if `fg->build()` fails, else the first draw/scope
// error (or None). `alloc` backs the per-scope pass closures (reserved so their pointers stay stable across `execute()`).
// ⛔ STORAGE-BUFFER CONTRACT (CEIR-14z-4c): bound storage buffers (via `binding_resolver`) are bound directly in the draw
// packet and are NOT frame-graph-tracked — the caller MUST pre-upload them, and NO pass in the SAME frame may write them.
// (A frame graph tracks only the imported color/depth TARGETS, for barriers + readback.) Compute→raster hazard tracking on
// storage buffers lands with its FIRST consumer (a mixed compute+raster CEIR program — CEIR-15/16), not speculatively here.
[[nodiscard]] ExecuteError execute_render_frame(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                                crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc,
                                                const RenderResolvers& resolvers);
} // namespace crd::ceir::gpu
