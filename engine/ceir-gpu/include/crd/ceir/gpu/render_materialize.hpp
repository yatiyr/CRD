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

// Materialize a render.scope op into a RenderingDesc: width/height/sample_count from the scope attrs; each attachment
// operand's DEFINING color/depth_attachment op → a ColorAttachmentDesc / DepthStencilAttachmentDesc (LoadOp/StoreOp/
// typed-clear[⭐ RAH-1a.1: clear_kind=uint → ClearKind::Uint + clear_uint]/blend/compare mapped from the CEIR string attrs;
// the target via `resolver`). ⛔ `out` is fully overwritten. Returns false on a malformed scope (an operand not defined by
// an attachment op, or > kMaxColorAttachments color attachments).
[[nodiscard]] bool materialize_rendering_desc(const Context& ctx, const Operation* scope_op, RasterTargetResolveFn resolver,
                                              void* user, crd::gpu::RenderingDesc& out);

// Materialize a render.draw* op into a RasterDrawPacket: the RasterCommandKind + GeometryKind from the op name; the counts
// const-folded from the operands; first_vertex/first_index/max_draws from attrs; the program via `resolver`. ⭐ CEIR-14z-4c:
// if `binding_resolver` is non-null, each operand in the draw's variadic binding tail (from `render_draw_binding_start`) is
// resolved → a StorageBuffer `ResourceBinding` (slot = its ordinal). A binding that resolves to NULL → returns false (a
// draw that bound 1-of-N buffers would render garbage from the wrong buffer, pixel-plausibly — a typed failure, never a
// silent skip). Returns false on a malformed draw (unknown op name, a dynamic count that cannot const-fold, or a null binding).
[[nodiscard]] bool materialize_draw_packet(const Context& ctx, const Operation* draw_op, RasterProgramResolveFn resolver,
                                           void* user, crd::gpu::RasterDrawPacket& out,
                                           RasterBindingResolveFn binding_resolver = nullptr, void* binding_user = nullptr);

// CEIR-14z-2: execute a render-lowered command list on an ICommandEncoder (the RASTER executor, Option A test-surface):
// BeginRender → materialize_rendering_desc → encoder.begin_rendering; Draw → materialize_draw_packet → encoder.draw;
// EndRender → encoder.end_rendering. ⛔ a Barrier is INERT here (encoder-surface barriers are the frame graph's job —
// CEIR-15/16); a Dispatch/Transfer in a render list → UnsupportedCommand (this executor is render-only; a mixed
// compute+render CEIR program is 15/16). A null program/target from a resolver → UnresolvedProgram; a draw whose count
// cannot const-fold → UnsupportedCommand. ⛔ assumes find_render_misuse passed (the verifier-first contract).
[[nodiscard]] ExecuteError execute_render_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                                  crd::gpu::ICommandEncoder& encoder, RasterTargetResolveFn target_resolver,
                                                  void* target_user, RasterProgramResolveFn program_resolver,
                                                  void* program_user, RasterBindingResolveFn binding_resolver = nullptr,
                                                  void* binding_user = nullptr);

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
                                                RasterTargetResolveFn target_resolver, void* target_user,
                                                RasterProgramResolveFn program_resolver, void* program_user,
                                                RasterBindingResolveFn binding_resolver = nullptr,
                                                void* binding_user = nullptr);
} // namespace crd::ceir::gpu
