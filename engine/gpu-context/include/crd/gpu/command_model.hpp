#pragma once

// crd-gpu-context — the canonical, backend-neutral DECLARATIVE GPU command model (RAF-2, mission §7).
//
// ONE data model that expresses every raster/compute/transfer/ray-trace command as VALUES, so no new feature ever
// needs another `draw_*` method and no pass kind grows a central enum. The ~53 combinatorial verbs on IRasterContext
// (draw_storage_depth · draw_storage_multi_indexed_depth · draw_bindless_depth · trace_rays_anyhit · …) all
// reduce to: a RENDERING SCOPE (typed attachments with load/store/clear/blend) + a RASTER DRAW PACKET (program +
// binding table + geometry source + a STRONG command variant + state). Both backends lower these DIRECTLY (RAF-2b),
// and both authored and hand-built graphs record them (RAF-7) — the clear-vs-load and 1-vs-MRT distinctions are DATA,
// never separate functions.
//
// This header REUSES the existing pass-state vocabulary (ClearColor · BlendMode · DepthCompare · PassRasterState ·
// SamplerDesc · ShadingRate/Combiner · ConservativeMode) and the opaque resource interfaces from raster_context.hpp —
// it consolidates onto them, it does not duplicate them. Host-side validation (command_model.cpp) checks structural
// invariants with NO device, and the packet uses FixedArray inline storage so building/validating one allocates
// NOTHING (the hot-path contract). See docs/design/raf-0-rendering-foundation-design.md §4.

#include <crd/containers/fixed_array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/raster_context.hpp> // ClearColor · BlendMode · DepthCompare · PassRasterState · SamplerDesc · ShadingRate* · ConservativeMode · opaque resources
#include <crd/renderasset/binding.hpp> // the SHARED BindingFrequency / BindingKind (RAF-4) — one definition, not per layer

namespace crd::gpu
{
using crd::containers::FixedArray;
using crd::containers::StringView;

class IGpuProgram; // compute / RT program (fwd)

// Fixed capacities — bounds the D3D12 root-signature / Vulkan descriptor layout and keeps the packet allocation-free.
inline constexpr crd::u32 kMaxColorAttachments = 8;  // MRT ceiling (both backends)
inline constexpr crd::u32 kMaxBindings = 16;         // resource bindings per packet
inline constexpr crd::u32 kMaxBindlessTextures = 8;  // matches the legacy draw_bindless array capacity

// ── Attachment load/store (the clear-vs-load axis, now DATA not two functions) ──
enum class LoadOp : crd::u8
{
    Load = 0,  // preserve prior contents (the "…_load" verb family)
    Clear,     // clear to the attachment's clear value (the default first-write)
    DontCare,  // contents undefined on load (a full-overwrite pass)
};
enum class StoreOp : crd::u8
{
    Store = 0, // keep the result (the default)
    DontCare,  // result not needed after the pass (a transient depth buffer)
};

// ⭐ RAH-1: the attachment clear is TYPED. `clear_kind == Float` ⇒ the float `clear` (the common colour case);
// `clear_kind == Uint` ⇒ the integer background id `clear_uint` (an R32_UINT id target — the visibility buffer).
// This is what makes VISIBILITY an ORDINARY typed attachment: the encoder derives the id-write draw from `clear_kind`,
// not a `RenderingDesc.visbuffer` boolean. (A float ClearColor cannot express an integer id, so the clear is typed.)
enum class ClearKind : crd::u8
{
    Float = 0, // the float `clear` (ClearColor)
    Uint,      // the integer `clear_uint` (R32_UINT id target)
};

// One colour attachment. `target` is the concrete image (a frame-graph transient or a standalone target); MRT is just
// N of these. `blend` replaces the per-attachment blend array threaded through draw_storage_mrt.
struct ColorAttachmentDesc
{
    IRasterTarget* target = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    ClearColor clear{};
    BlendMode blend = BlendMode::Opaque;
    // RAH-1: appended AT END so positional init `{target, load, store, clear, blend}` stays valid (D135 vtable-style
    // append discipline for aggregates). `clear_kind == Uint` selects `clear_uint` (the R32_UINT id) over `clear`.
    ClearKind clear_kind = ClearKind::Float;
    crd::u32 clear_uint = 0; // integer background id when clear_kind == Uint (was RenderingDesc.clear_id)
};

// The depth/stencil attachment. `enabled == false` ⇒ no depth attachment (a colour-only pass). Zero colour
// attachments + enabled depth ⇒ a DEPTH-ONLY pass (the shadow-map substrate — draw_storage_depth_only). `compare`
// + `depth_test` fold in the DepthCompare argument the verbs carried; stencil + depth_write live in RasterState.
struct DepthStencilAttachmentDesc
{
    IRasterTarget* target = nullptr;
    bool enabled = false;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    float clear_depth = 1.0F; // crd-lint-allow-untagged-physical: normalized device depth [0,1], a raw API scalar
    bool depth_test = true;
    DepthCompare compare = DepthCompare::LessEqual;
};

// The rendering SCOPE: the attachments + render area a set of draws target. Replaces the "which target verb" axis.
struct RenderingDesc
{
    crd::u32 width = 0;
    crd::u32 height = 0;
    crd::u32 sample_count = 1; // MSAA (>1 ⇒ multisample + resolve)
    FixedArray<ColorAttachmentDesc, kMaxColorAttachments> color;
    DepthStencilAttachmentDesc depth{};
    IRasterTarget* shading_rate_attachment = nullptr; // optional per-tile VRS source (3rd rate source)
    // ── ⭐ RAH-1: the VISIBILITY-BUFFER scope is now an ORDINARY TYPED ATTACHMENT — a `color[0]` whose `clear_kind == Uint`
    // carries the R32_UINT background id in `clear_uint`. The encoder derives the id-write (clear-once/load-rest) from that
    // typed clear; the old `bool visbuffer` + `u32 clear_id` scope fields are RETIRED (RAH-1a.1).
    // ── B5 deferred G-BUFFER scope. An IGBufferTarget bundles N (2..8) host-readable RGBA8 colour attachments +
    // their own per-attachment read_pixel — a DISTINCT target type, not an IRasterTarget, so it rides its own field
    // rather than the `color` list. When set (with GeometryKind::None), the encoder lowers a plain-vertex draw that
    // CLEARS all N attachments to `color[0].clear` (a single clear-carrier entry with a null target) and writes the
    // surface material's N colour outputs via MRT — draw_gbuffer.
    IGBufferTarget* gbuffer = nullptr;
};

// ── Resource bindings (the hard-coded "set 0 binding 1 = base colour" convention, now a typed table) ──
// The cooked program contract resolves NAMES to compact slots (RAF-4); here a binding carries its resolved slot +
// frequency so the recorder never hard-codes a register. One binding = one resource, by kind.
// The SHARED binding vocabulary (RAF-4) — defined once in render-asset-core, used by both the program contract and
// this command model. Aliased into crd::gpu so existing `BindingFrequency::Material` / `BindingKind::…` still resolve.
using crd::renderasset::BindingFrequency;
using crd::renderasset::BindingKind;
struct ResourceBinding
{
    BindingFrequency frequency = BindingFrequency::Draw;
    BindingKind kind = BindingKind::StorageBuffer;
    crd::u32 slot = 0; // compact resolved slot (from the cooked program contract)
    // Resource — the field matching `kind` is used; the rest stay null/zero.
    IStorageBuffer* buffer = nullptr;
    ITexture* texture = nullptr;
    ITexture* const* texture_array = nullptr; // BindlessTextureArray
    crd::u32 array_count = 0;                  // BindlessTextureArray element count
    SamplerDesc sampler{};                     // Sampler / ComparisonSampler
};
using ResourceBindingTable = FixedArray<ResourceBinding, kMaxBindings>;

// ── Geometry source (indexed/indirect/count/mesh/tess as DATA, not verb suffixes) ──
enum class GeometryKind : crd::u8
{
    None = 0,        // no pulled geometry (a fullscreen triangle: vertex_or_index_count = 3)
    StoragePull,     // vertex-pull from a storage buffer; vertex_or_index_count vertices
    Indexed,         // indexed pull; vertex_or_index_count indices + index_buffer
    Indirect,        // args from a native buffer; indirect_draw_count draws
    IndirectCount,   // args + a device count buffer (GPU decides the draw count)
    Meshlet,         // mesh-shader dispatch; group_count_{x,y,z}
    MeshletIndirect, // DispatchMeshIndirect from native args
    Patches,         // tessellation patch list; patch_count × control_points
    // ── ⭐ RAF-8: CPU MULTI-DRAW — the live scene BATCHING (draw_storage_multi_depth / _multi_indexed_depth). A run
    // of consecutive plain/indexed items sharing program+buffer records as ONE verb (one descriptor reset) — the perf
    // contract the per-draw executor loop must NOT lose (dropping it to N single draws is a measurable regression).
    // Appended at END (a renumbered kind reinterprets every cooked payload). ──
    MultiStoragePull, // N vertex counts (multi_counts), one storage buffer
    MultiIndexed,     // N IndexedDraw records (multi_indexed), one storage buffer
};
struct GeometrySource
{
    GeometryKind kind = GeometryKind::StoragePull;
    crd::u32 vertex_or_index_count = 0;
    crd::u32 instance_count = 1;
    // Indexed — indices live in a storage buffer (the pull buffer itself, at index_offset_bytes).
    IStorageBuffer* index_buffer = nullptr;
    crd::u32 index_offset = 0; // BYTE offset of the index section
    crd::u32 first_index = 0;  // START within the bound index section, in INDICES (the indexed-sampled scene draw)
    crd::u32 first_vertex = 0; // START vertex for a non-indexed StoragePull draw (the ranged debug overlay, draw_overlay_range)
    // Indirect / IndirectCount / MeshletIndirect. Two arg conventions: a tracked storage buffer (indexed-indirect
    // + mesh-indirect-buffer) OR a native handle (draw_mesh_indirect from a ComputeBuffer). count_buffer supplies a
    // device-computed draw count (indexed-indirect-count). Offsets are BYTES; max_draws bounds the indirect draws.
    IStorageBuffer* args_buffer = nullptr;
    crd::u64 args_offset = 0;
    IStorageBuffer* count_buffer = nullptr;
    crd::u64 count_offset = 0;
    crd::u32 max_draws = 1;
    void* native_args = nullptr; // native-handle convention (ComputeBuffer::native_handle())
    // Meshlet
    crd::u32 group_count_x = 1;
    crd::u32 group_count_y = 1;
    crd::u32 group_count_z = 1;
    // Patches
    crd::u32 patch_count = 0;
    crd::u32 control_points = 4; // quad patch by default
    // ── ⭐ RAF-8: CPU MULTI-DRAW (MultiStoragePull / MultiIndexed). The count arrays are HOST-OWNED (the packet stays
    // allocation-free — a pointer, like a bindless texture_array), valid for the duration of the draw. `draw_count`
    // is the run length; `first_draw_index` is the DrawIndex ROW BASE the run rebases every load by (REN-40-C2). ──
    const crd::u32* multi_counts = nullptr;                     // MultiStoragePull: draw_count vertex counts
    const IRasterContext::IndexedDraw* multi_indexed = nullptr; // MultiIndexed: draw_count {index_count, inst, first}
    crd::u32 draw_count = 0;
    crd::u32 first_draw_index = 0;
};

// The STRONG draw-command variant (no boolean bag). Must agree with the geometry kind (validated).
enum class RasterCommandKind : crd::u8
{
    Draw = 0,                 // non-indexed (None / StoragePull)
    DrawIndexed,              // Indexed
    DrawIndirect,             // Indirect
    DrawIndexedIndirect,      // Indirect + Indexed
    DrawIndexedIndirectCount, // IndirectCount
    DispatchMesh,             // Meshlet
    DispatchMeshIndirect,     // MeshletIndirect
    DrawPatches,              // Patches (tessellation)
    DrawMulti,                // RAF-8: MultiStoragePull (CPU multi-draw batch)
    DrawMultiIndexed,         // RAF-8: MultiIndexed
};

// Per-draw raster state — REUSES PassRasterState (depth_write · bias · cull · stencil) + the VRS/conservative axes.
// (The depth COMPARE op lives on the depth attachment; depth_write stays here because it is pipeline state.)
struct RasterState
{
    PassRasterState raster{};
    ShadingRate vrs_pipeline_rate = ShadingRate::Rate1x1;
    ShadingRateCombiner vrs_primitive_combiner = ShadingRateCombiner::Keep;
    ConservativeMode conservative = ConservativeMode::Off;
};

// THE canonical raster draw packet. ONE of these expresses every draw_* verb. Recorded inside a RenderingDesc scope;
// multiple packets can share one scope (batching). Allocation-free (FixedArray bindings).
struct RasterDrawPacket
{
    IRasterProgram* program = nullptr;
    RasterCommandKind command = RasterCommandKind::Draw;
    GeometrySource geometry{};
    ResourceBindingTable bindings{};
    RasterState state{};
};

// ── Compute dispatch (dispatch_kernel · _indirect · _rt · _sampled → one desc) ──
enum class DispatchKind : crd::u8
{
    Direct = 0, // groups_{x,y,z}
    Indirect,   // indirect_args (native {gx,gy,gz})
};
struct DispatchDesc
{
    IGpuProgram* kernel = nullptr;
    DispatchKind kind = DispatchKind::Direct;
    crd::u32 groups_x = 1;
    crd::u32 groups_y = 1;
    crd::u32 groups_z = 1;
    IStorageBuffer* args_buffer = nullptr; // Indirect: the {gx,gy,gz} args (dispatch_kernel_indirect)
    crd::u64 args_offset = 0;
    ResourceBindingTable bindings{};
    bool ray_tracing_pipeline = false; // dispatch_kernel_rt (an RT pipeline via the compute path)
    // ── ⭐ RAF-8: the INLINE RAY-QUERY dispatch (an authored `raytrace` pass). A Direct dispatch carrying an
    // acceleration structure binds the TLAS at set 0/binding 0 and the storage buffers at 1..N, then dispatches the
    // ray-query kernel into the frame's one submission (dispatch_kernel_rt) — NOT a ray-tracing pipeline (no SBT).
    IAccelerationStructure* acceleration_structure = nullptr;
};

// ── Transfer (clear/copy/blit/resolve) ──
enum class TransferKind : crd::u8
{
    Clear = 0,
    Copy,
    Blit,
    Resolve,
};
struct TransferDesc
{
    TransferKind kind = TransferKind::Clear;
    IRasterTarget* dst = nullptr;
    IRasterTarget* src = nullptr; // Copy / Blit / Resolve
    ClearColor clear{};           // Clear
    SamplerFilter filter = SamplerFilter::Linear; // Blit
};

// ── Ray tracing (trace_rays · _anyhit · _full → one desc) ──
// The SBT stage programs + the acceleration structure, as data. The encoder selects the backend verb by which
// optional stages are present: intersection|callable ⇒ full pipeline; else any_hit ⇒ anyhit; else the base trace.
struct TraceDesc
{
    IGpuProgram* raygen = nullptr;      // required
    IGpuProgram* miss = nullptr;        // required
    IGpuProgram* closest_hit = nullptr; // required
    IGpuProgram* any_hit = nullptr;     // optional (alpha-tested geometry)
    IGpuProgram* intersection = nullptr;// optional (procedural hit group)
    IGpuProgram* callable = nullptr;    // optional (SBT callable table)
    IAccelerationStructure* acceleration_structure = nullptr; // required
    crd::u32 width = 1;
    crd::u32 height = 1;
    crd::u32 depth = 1;
    ResourceBindingTable bindings{};
};

// The compact command ENCODER both backends implement (RAF-2b) and both authored + hand-built graphs record into
// (RAF-7). A rendering scope brackets N draw packets; compute/transfer/trace record outside a scope.
class ICommandEncoder
{
public:
    ICommandEncoder() = default;
    virtual ~ICommandEncoder() = default;
    ICommandEncoder(const ICommandEncoder&) = delete;
    ICommandEncoder& operator=(const ICommandEncoder&) = delete;
    ICommandEncoder(ICommandEncoder&&) = delete;
    ICommandEncoder& operator=(ICommandEncoder&&) = delete;

    virtual void begin_rendering(const RenderingDesc& rendering) = 0;
    virtual void draw(const RasterDrawPacket& packet) = 0; // must be inside an active rendering scope
    virtual void end_rendering() = 0;

    virtual void dispatch(const DispatchDesc& dispatch) = 0;
    virtual void transfer(const TransferDesc& transfer) = 0;
    virtual void trace_rays(const TraceDesc& trace) = 0;
};

// ── Host-side validation (RAF-2a, pure CPU — no device) ──
// Structural invariants a backend must be able to trust before lowering. Deterministic; allocation-free.
enum class CommandError : crd::u8
{
    None = 0,
    NoAttachments,                     // a rendering scope with neither colour nor depth
    TooManyColorAttachments,           // > kMaxColorAttachments
    MismatchedAttachmentSize,          // a colour/depth target differs from the render area
    ZeroRenderArea,                    // width or height is 0
    NullProgram,                       // packet/dispatch/trace has no program
    DuplicateBinding,                  // two bindings share (frequency, slot)
    ComparisonSamplerWithoutTexture,   // a comparison sampler with no sampled texture to pair with
    BindlessCountExceeded,             // a bindless array with 0 or > kMaxBindlessTextures elements
    GeometryCommandMismatch,           // command kind disagrees with geometry kind
    MissingIndexBuffer,                // an indexed command with no index buffer
    MissingIndirectArgs,               // an indirect command with no args buffer
    MissingCountBuffer,                // an indirect-count command with no count buffer
    MissingAccelerationStructure,     // a trace with no acceleration structure
    ZeroDraw,                          // zero vertices / indices / groups
};

[[nodiscard]] StringView command_error_name(CommandError err) noexcept;

// Validate a rendering scope (attachment counts, sizes, render area).
[[nodiscard]] CommandError validate_rendering(const RenderingDesc& rendering) noexcept;
// Validate a raster draw packet (program, bindings, geometry/command agreement). Does NOT re-check the scope.
[[nodiscard]] CommandError validate_packet(const RasterDrawPacket& packet) noexcept;
// Validate a compute dispatch.
[[nodiscard]] CommandError validate_dispatch(const DispatchDesc& dispatch) noexcept;
// Validate a ray-trace (required stages + acceleration structure + non-zero dispatch).
[[nodiscard]] CommandError validate_trace(const TraceDesc& trace) noexcept;
} // namespace crd::gpu
