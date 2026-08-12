#pragma once

// crd-render-graph — the UNIFIED frame-graph runtime (RAF-7, mission §7).
//
// The separated forms — compile once, execute many:
//   • `FrameGraphTemplate`  — the cooked TOPOLOGY: resources (name · kind · lifetime · size class) + passes (name ·
//     executor id · typed payload whose resource slots declare reads/writes). Authoring-independent: a hand-built
//     template and an authored/parsed one that describe the same graph compile IDENTICALLY.
//   • `CompiledFrameGraph`  — a template compiled for a given size: a validated, deterministic SCHEDULE (topological
//     over pass reads/writes), transient ALIASING (non-overlapping lifetimes share a physical slot), persistent +
//     history resources pinned to their own slots. Recompiled only on a topology- or size-affecting change.
//   • EXECUTION            — walks the schedule, invokes each pass's registered EXECUTOR RECORD function, which emits
//     the RAF-2 CANONICAL command model into an `ICommandEncoder`. No `FramePassKind`→specialized-verb switch: the
//     executor records `begin_rendering`/`draw`/`end_rendering`/`dispatch`/`transfer`. A record function may only
//     touch resources the pass DECLARED (undeclared access is diagnosed).
//
// Both backends lower the recorded commands identically; a hand-built graph and an authored one that describe the
// same topology record the same commands. Built on render-asset-core (dependency graph + diagnostics), render-pass
// (executor schemas + validation), gpu-context (the command encoder + command model).

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/command_model.hpp>
#include <crd/gpu/frame_graph.hpp>    // RAF-12.2-b: IFrameGraph / FgImage / FgBuffer / FgPassKind / FgExecuteFn / present
#include <crd/gpu/raster_context.hpp> // RAF-12.2-b: SamplerDesc / RasterState for the per-pass device setup
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderpass/executor_registry.hpp>

// CEIR-16-3c: forward-decls for the per-pass CEIR PLAN a migrated executor replays (the ADR-0109 §4.2 anticipated
// render-graph→ceir-gpu edge). The plan is carried as pure pointers here (no ceir-gpu header in this widely-included
// public header); the replay executor that dereferences them links crd-ceir-gpu.
namespace crd::ceir
{
class Context;
namespace gpu
{
struct LoweredCommand;
} // namespace gpu
} // namespace crd::ceir

namespace crd::rendergraph
{
using crd::containers::Array;
using crd::gpu::IAccelerationStructure;
using crd::gpu::ICommandEncoder;
using crd::gpu::IGpuProgram;
using crd::gpu::IRasterContext;
using crd::gpu::IRasterProgram;
using crd::gpu::IRasterTarget;
using crd::gpu::IStorageBuffer;
using crd::gpu::ITexture;

// The per-pass host program bindings a record function needs (resolved by a host; a test supplies fakes): the raster
// program for a raster pass, the compute kernel for a dispatch, the raygen/miss/hit + acceleration structure for RT.
struct PassPrograms
{
    IRasterProgram* raster = nullptr;
    IGpuProgram* kernel = nullptr;
    IGpuProgram* raygen = nullptr;
    IGpuProgram* miss = nullptr;
    IGpuProgram* closest_hit = nullptr;
    // ⭐ RAF-8: the OPTIONAL SBT stages of a ray-tracing PIPELINE pass — any-hit (alpha-tested geometry), intersection
    // (a procedural hit group) and callable (the SBT fourth table). Null on a base three-stage pipeline; the record
    // function selects trace_rays / _anyhit / _full by which of these are present.
    IGpuProgram* any_hit = nullptr;
    IGpuProgram* intersection = nullptr;
    IGpuProgram* callable = nullptr;
    IAccelerationStructure* accel = nullptr;
};

// ── RAF-8: ONE resolved scene draw — PURE gpu-context handles (no ECS / no FrameDrawListDesc; the HOST pre-resolves
// the ECS query into these, keeping crd-render-graph ⊥ crd-scene). Mirrors frame-cook's DrawItem contract: an item's
// own `program`/`texture` win over the pass default; `index_count > 0` ⇒ indexed-pull; `args != nullptr` ⇒ GPU-driven
// (the command lives in device memory; instance_count is stale by construction and MUST NOT be read). ──
struct RenderDrawItem
{
    IStorageBuffer* storage = nullptr;       // the vertex-pull buffer this draw reads
    IRasterProgram* program = nullptr;       // per-item program override (null ⇒ the pass default raster program)
    ITexture* texture = nullptr;             // per-item albedo MAP (beats the pass's sampled atlas)
    u32 vertex_count = 0;                     // non-indexed draw
    bool indexed = false;                     // the draw-index-rebased contract (records through the multi verb)
    u32 index_count = 0;                      // > 0 ⇒ indexed-pull; index_count/instance_count/first_index apply
    u32 instance_count = 0;
    u32 first_index = 0;
    IStorageBuffer* args = nullptr;           // != null ⇒ GPU-driven indirect (device-memory command)
    u32 args_offset = 0;
    // RAF-8: a COMPUTE pass that walks this draw list dispatches ONCE PER ITEM, with THIS item's workgroup count
    // (ceil(instances / wg) — only the host knows a group's instance count). 0 ⇒ the item is skipped (a raster item).
    u32 dispatch_groups = 0;
};

// A per-pass resolved draw list (host-owned, valid for the execution). `items[0..count-1]` are recorded in order;
// consecutive compatible items may coalesce into ONE canonical multi-draw packet (the batching perf contract).
struct DrawList
{
    const RenderDrawItem* items = nullptr;
    u32 count = 0;
    ITexture* pass_texture = nullptr; // the pass's sampled read (e.g. the shadow atlas); null for an untextured pass
    bool pass_texture_is_depth = false; // depth OR arrayed ⇒ bind at the ATLAS slot (4/5), not the base-colour map slot
    // ⛔⛔ REN-40-D: the SAMPLER type, split from the routing above. A true DEPTH atlas takes a comparison sampler (PCF);
    // a COLOUR-ARRAY atlas (moment/variance tiers) takes a PLAIN sampler. Conflating the two made every moment shadow
    // render black — the executor bound a comparison sampler where the shader declared a plain `sampler2DArray`.
    bool pass_texture_comparison = false; // true ⇒ comparison (depth/shadow) sampler; false ⇒ plain filtering sampler
};
using crd::renderasset::DiagCode;
using crd::renderasset::DiagnosticList;
using crd::renderpass::ExecutorRegistry;
using crd::renderpass::ExecutorTypeId;
using crd::renderpass::PassPayload;
using crd::renderpass::SlotAccess;
using crd::renderpass::SlotResourceKind;

// A resource's lifetime class. Transients are aliased; persistent/history resources keep a dedicated slot and survive
// across executions (history = the previous frame's value, read this frame).
enum class ResourceLifetime : u8
{
    Transient = 0,
    Persistent,
    History,
};

// A declared graph resource. `size_class` groups transients that may alias (same class + non-overlapping lifetime).
struct GraphResource
{
    u64 name_hash = 0;
    SlotResourceKind kind = SlotResourceKind::ColorTarget;
    ResourceLifetime lifetime = ResourceLifetime::Transient;
    u64 size_class = 0; // e.g. a (width,height,format) hash for images; a byte size for buffers
};

// A declared pass. Its payload's resource slots are its reads/writes (Read/ReadWrite ⇒ read; Write/ReadWrite ⇒ write).
struct GraphPass
{
    u64 name_hash = 0;
    PassPayload payload;
};

// The cooked topology. Authoring-independent.
class FrameGraphTemplate
{
public:
    explicit FrameGraphTemplate(memory::IAllocator* alloc) noexcept : m_resources(alloc), m_passes(alloc) {}

    void add_resource(const GraphResource& r) { m_resources.push_back(r); }
    void add_pass(const GraphPass& p) { m_passes.push_back(p); }

    [[nodiscard]] const Array<GraphResource>& resources() const noexcept { return m_resources; }
    [[nodiscard]] const Array<GraphPass>& passes() const noexcept { return m_passes; }
    [[nodiscard]] const GraphResource* find_resource(u64 name_hash) const noexcept;

private:
    Array<GraphResource> m_resources;
    Array<GraphPass> m_passes;
};

// A compiled resource: its physical slot (transients aliasing share one), lifetime, and size class.
struct CompiledResource
{
    u64 name_hash = 0;
    u32 physical_slot = 0;
    ResourceLifetime lifetime = ResourceLifetime::Transient;
    SlotResourceKind kind = SlotResourceKind::ColorTarget;
    u64 size_class = 0;
};

// A template compiled for a size. Recompiled only on a topology- or size-affecting change.
class CompiledFrameGraph
{
public:
    explicit CompiledFrameGraph(memory::IAllocator* alloc) noexcept : m_schedule(alloc), m_resources(alloc) {}

    [[nodiscard]] const Array<u32>& schedule() const noexcept { return m_schedule; }        // pass indices, in order
    [[nodiscard]] const Array<CompiledResource>& resources() const noexcept { return m_resources; }
    [[nodiscard]] u32 physical_slot_count() const noexcept { return m_physical_slot_count; }
    [[nodiscard]] u32 width() const noexcept { return m_width; }
    [[nodiscard]] u32 height() const noexcept { return m_height; }
    [[nodiscard]] const CompiledResource* find(u64 name_hash) const noexcept;

    // (compile() fills these)
    Array<u32> m_schedule;
    Array<CompiledResource> m_resources;
    u32 m_physical_slot_count = 0;
    u32 m_width = 0;
    u32 m_height = 0;
};

// One physically-resolved resource handed to a pass at execute time (a real backend supplies real handles; a test
// supplies fakes). Which member is set follows `kind`.
struct ResolvedResource
{
    u64 name_hash = 0;
    SlotResourceKind kind = SlotResourceKind::ColorTarget;
    IRasterTarget* target = nullptr;
    IStorageBuffer* buffer = nullptr;
    IAccelerationStructure* accel = nullptr;
    ITexture* texture = nullptr; // SlotResourceKind::Texture — sampled maps / shadow atlases / bindless elements
};

// The per-execution resource set (indexed by name).
class ResourceTable
{
public:
    explicit ResourceTable(memory::IAllocator* alloc) noexcept : m_resources(alloc) {}
    void bind(const ResolvedResource& r) { m_resources.push_back(r); }
    [[nodiscard]] const ResolvedResource* find(u64 name_hash) const noexcept;

private:
    Array<ResolvedResource> m_resources;
};

// RAF-8: the per-pass DRAW LISTS (indexed by pass name hash). The host pre-resolves each scene pass's ECS query into
// a `DrawList` of pure gpu-context handles and binds it here; the scene.raster executor resolves its list by pass.
class DrawListTable
{
public:
    explicit DrawListTable(memory::IAllocator* alloc) noexcept : m_lists(alloc) {}
    void bind(u64 pass_name_hash, const DrawList& list) { m_lists.push_back(Entry{pass_name_hash, list}); }
    [[nodiscard]] const DrawList* find(u64 pass_name_hash) const noexcept;

private:
    struct Entry
    {
        u64 pass_name_hash;
        DrawList list;
    };
    Array<Entry> m_lists;
};

// ⭐⭐ RAF-12.2-b: the per-pass PROGRAM bindings (indexed by pass name hash) — the authored-frame runtime's counterpart
// to DrawListTable. A live frame's passes each name their OWN shader/kernel/SBT (the shadow cascade's depth VS, the
// forward pass's lit program, the tonemap FS, a compute cull kernel, an RT pipeline's SBT), so a SINGLE frame-wide
// PassPrograms cannot drive them. The host resolves each pass's programs and binds them here by pass; the runtime looks
// them up per pass and falls back to the frame-wide `programs` argument for a pass with no per-pass entry.
class PassProgramsTable
{
public:
    explicit PassProgramsTable(memory::IAllocator* alloc) noexcept : m_entries(alloc) {}
    void bind(u64 pass_name_hash, const PassPrograms& programs) { m_entries.push_back(Entry{pass_name_hash, programs}); }
    [[nodiscard]] const PassPrograms* find(u64 pass_name_hash) const noexcept;

private:
    struct Entry
    {
        u64 pass_name_hash;
        PassPrograms programs;
    };
    Array<Entry> m_entries;
};

// ⭐ CEIR-16-3c: a migrated executor's per-pass CEIR PLAN — the lowered command slice of the executor's composite (built
// once from the pass payload at graph-build by `build_fullscreen_ceir` / future builders) plus the CEIR `Context` those
// commands' `Operation*` back-pointers reference (the Context OUTLIVES the plan — the owner keeps it alive). The generic
// replay `PassRecordFn` drives `execute_render_lowered(*ctx, {commands, count}, encoder, resolvers)`; scene.raster (16d)
// rides the SAME seam, so this is executor-agnostic. Pointers only (no ceir-gpu header here — the replay executor links it).
struct CeirPassPlan
{
    const crd::ceir::Context*             ctx      = nullptr; // the plan's CEIR context (outlives the plan)
    const crd::ceir::gpu::LoweredCommand* commands = nullptr; // the lowered BeginRender…Draw…EndRender slice
    u32                                   count    = 0U;      // number of commands
};

// CEIR-16-3c: the per-pass CEIR plans (indexed by pass name hash) — the DrawListTable/PassProgramsTable counterpart for
// migrated (ceir.render-backed) executors. The owner builds each migrated pass's plan at graph-build and binds it here;
// the runtime looks it up per pass and threads it into the pass's RecordContext (null for a non-migrated pass).
class CeirPlanTable
{
public:
    explicit CeirPlanTable(memory::IAllocator* alloc) noexcept : m_plans(alloc) {}
    void bind(u64 pass_name_hash, const CeirPassPlan& plan) { m_plans.push_back(Entry{pass_name_hash, plan}); }
    [[nodiscard]] const CeirPassPlan* find(u64 pass_name_hash) const noexcept;

private:
    struct Entry
    {
        u64 pass_name_hash;
        CeirPassPlan plan;
    };
    Array<Entry> m_plans;
};

// The context a pass's record function sees: it may resolve ONLY resources the pass declared (an undeclared slot is
// diagnosed — the "declared use matches recorded" contract), plus the host-bound program for this pass.
class RecordContext
{
public:
    RecordContext(const PassPayload& payload, const ResourceTable& table, const PassPrograms& programs,
                  DiagnosticList& diags, const DrawList* draws = nullptr, const CeirPassPlan* plan = nullptr) noexcept
        : m_payload(&payload), m_table(&table), m_programs(&programs), m_diags(&diags), m_draws(draws), m_plan(plan)
    {
    }

    // True if this pass DECLARED the named slot (in its payload). A record function checks this before touching an
    // OPTIONAL slot; touching a slot without declaring it is the "declared use matches recorded" violation.
    [[nodiscard]] bool has(u64 slot_name) const noexcept { return is_declared(slot_name); }

    // Resolve a declared colour/depth target or storage buffer by slot name. Returns nullptr (+ diagnostic) if the
    // slot was not declared by this pass, or if it is unbound.
    [[nodiscard]] IRasterTarget* color_target(u64 slot_name) const;
    [[nodiscard]] IRasterTarget* depth_target(u64 slot_name) const;
    [[nodiscard]] IStorageBuffer* storage(u64 slot_name) const;
    [[nodiscard]] IAccelerationStructure* accel(u64 slot_name) const;
    [[nodiscard]] ITexture* texture(u64 slot_name) const; // a declared Texture slot (map / shadow atlas / bindless)
    [[nodiscard]] const PassPrograms& programs() const noexcept { return *m_programs; }
    // RAF-8: this pass's resolved scene draw list (host pre-resolved). Empty (count 0) for a pass with none.
    [[nodiscard]] DrawList draws() const noexcept { return m_draws != nullptr ? *m_draws : DrawList{}; }
    // CEIR-16-3c: this pass's CEIR replay PLAN (null for a non-migrated pass — its executor uses its C++ record path).
    [[nodiscard]] const CeirPassPlan* plan() const noexcept { return m_plan; }
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    // ⭐ CEIR-16-3d-3: a record function marks its pass FAILED — the frame then fails to record LOUD (execute()/the recorder
    // return false and the caller reports it), never a silent no-op that draws nothing. Used when a plan-driven executor
    // (record_ceir_render) has no replay plan — a load-path bug the load-time build already logged.
    void fail() noexcept { m_ok = false; }

private:
    [[nodiscard]] bool is_declared(u64 slot_name) const noexcept;
    const PassPayload* m_payload;
    const ResourceTable* m_table;
    const PassPrograms* m_programs;
    DiagnosticList* m_diags;
    const DrawList* m_draws = nullptr;
    const CeirPassPlan* m_plan = nullptr;
    mutable bool m_ok = true;
};

// An executor's RECORD function: emit the canonical command model for this pass. Registered per ExecutorTypeId.
using PassRecordFn = void (*)(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder);

// Maps ExecutorTypeId → record function. Built-ins mirror the render-pass schema built-ins; apps register their own.
class GraphExecutorTable
{
public:
    explicit GraphExecutorTable(memory::IAllocator* alloc) noexcept : m_entries(alloc) {}
    bool register_record(ExecutorTypeId id, PassRecordFn fn, DiagnosticList& diags);
    [[nodiscard]] PassRecordFn find(ExecutorTypeId id) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(m_entries.size()); }

private:
    struct Entry
    {
        ExecutorTypeId id;
        PassRecordFn fn;
    };
    [[nodiscard]] usize lower_bound(ExecutorTypeId id) const noexcept;
    Array<Entry> m_entries; // sorted by id
};
// Register record functions for the built-in executors (scene.raster · fullscreen.raster · compute.dispatch ·
// transfer.{clear,copy,blit,resolve} · raytrace.dispatch [inline ray query] · raytrace.pipeline · mesh.raster ·
// tess.raster · mesh.indirect · visbuffer.raster · present).
u32 register_builtin_records(GraphExecutorTable& table, DiagnosticList& diags);

// ⭐ CEIR-16-3c: the GENERIC CEIR replay record function — drives `execute_render_lowered` on the pass's `CeirPassPlan`
// (`ctx.plan()`) through the (frame-recording) encoder, with resolvers backed by `RecordContext` (target→the "color" slot,
// program→the pass raster program, textures/bindless/samplers/storage→the pass's declared slots via each binding's `source`
// attr). A null plan (a NON-MIGRATED pass) is a no-op. Registered for `fullscreen.raster` behind the CEIR-replay selection
// (16b-3c-5); `scene.raster` (16d) reuses it. Exposed (unlike the imperative built-in records) so the migration can be
// A/B-gated and device-free-tested against a hand-built plan.
void record_ceir_render(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder);

// Compile a template for (width, height): validate every pass's executor + payload against the schema registry and
// that every declared slot resolves to a declared resource; build the deterministic schedule; assign transient
// aliasing + pin persistent/history resources. Returns false + diagnostics on any error.
[[nodiscard]] bool compile(const FrameGraphTemplate& tmpl, const ExecutorRegistry& schemas, u32 width, u32 height,
                           CompiledFrameGraph& out, DiagnosticList& diags);

// Execute a compiled graph: walk the schedule, invoke each pass's record function against a RecordContext, emitting
// the canonical command model into `encoder`. `table` binds each resource name to a resolved handle; `program`
// supplies the per-pass program (a host resolves it). Returns false if a record function touched an undeclared
// resource.
[[nodiscard]] bool execute(const CompiledFrameGraph& compiled, const FrameGraphTemplate& tmpl,
                           const GraphExecutorTable& records, const ResourceTable& table, const PassPrograms& programs,
                           ICommandEncoder& encoder, DiagnosticList& diags,
                           const DrawListTable* draw_lists = nullptr,
                           const PassProgramsTable* pass_programs = nullptr,
                           const CeirPlanTable* plans = nullptr);

// Execute on a DEVICE in ONE SUBMISSION (mission Gate 7 "one submission where expected"). Unlike `execute` — which
// records into a caller-supplied encoder and is what the device-free architecture gate drives with a mock — this
// runs the compiled graph as a real frame: it builds a gpu-context `IFrameGraph` from `raster`, imports every bound
// resource, declares each pass's reads/writes from its payload, and records each pass by invoking its record function
// against a frame-recording `ICommandEncoder`. The gpu-context frame graph owns the ONE command buffer, the
// cross-pass barriers, and the end-of-frame readback — so the frame-graph-shaped verbs (MRT · indexed-indirect ·
// comparison-sampler/shadow · bindless) record in their NATIVE frame-recording mode with no synchronous scaffolding.
// Returns false if the backend lacks a frame graph (`create_frame_graph` ⇒ nullptr), if a resource is unresolved, or
// if the graph fails to build. `alloc` backs the per-execution bookkeeping (handle map + pass closures).
// `out_submit_count`, when non-null, receives the frame graph's actual submission count after execution — the
// checkable proof of the "one submission where expected" property (mission Gate 7).
[[nodiscard]] bool execute_frame(const CompiledFrameGraph& compiled, const FrameGraphTemplate& tmpl,
                                 const GraphExecutorTable& records, const ResourceTable& table,
                                 const PassPrograms& programs, IRasterContext& raster, memory::IAllocator& alloc,
                                 DiagnosticList& diags, u32* out_submit_count = nullptr,
                                 const DrawListTable* draw_lists = nullptr,
                                 const PassProgramsTable* pass_programs = nullptr,
                                 const CeirPlanTable* plans = nullptr);

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// RAF-12.2-b: THE AUTHORED-FRAME DISPATCH — the ONE record path every pass runs through, replacing frame-cook's
// FramePassKind switch + the 11 per-kind wrappers + record_pass.
//
// A recorder (frame-cook's FrameRecorder, per the one-way module edge that keeps resource creation on the frame-cook
// side) resolves each already-for_each-expanded pass into an `AuthoredPass` — the payload + slot bindings (FgImage /
// FgBuffer handles) + draw list + programs + device setup — and adds it to its frame graph with `authored_pass_fn()`
// as the pass callback. That ONE callback (`run_authored_cb`) resolves the handles to device pointers at execute time
// and invokes the pass's registered EXECUTOR. render-graph stays ⊥ frame-cook (identity is graph-name hashes +
// gpu-context handles); the executor/RecordContext model is render-graph's, so every pass shares one dispatch.
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════

// HOW the runtime resolves a slot's FgImage to a device target at execute time — a plain image, a LAYER of a layered
// atlas (a shadow cascade slice), or an image WITH a bundled depth companion (the depth-tested colour target).
enum class SlotResolve : crd::u8
{
    Image = 0,       // fctx.image(image)
    ImageLayer,      // fctx.image_layer(image, layer)   — a for_each cascade slice
    ImageWithDepth,  // fctx.image_with_depth(image, depth) — colour target carrying its depth-stencil
    Texture,         // fctx.texture(image)              — a sampled map / atlas / bindless element
    Buffer,          // fctx.buffer(buffer)              — a storage buffer
    Accel,           // the external acceleration structure pointer directly
};

// A payload slot's physical handle, so the runtime resolves it to a device pointer at execute time (via IFrameContext).
// `name_hash` is the payload ResourceRef's `resource_id` (so the RecordContext matches slot → ref → this binding).
struct SlotBinding
{
    u64 name_hash = 0;
    SlotResourceKind kind = SlotResourceKind::ColorTarget;
    SlotResolve resolve = SlotResolve::Image;
    crd::gpu::FgImage image{};
    crd::gpu::FgImage depth{};     // ImageWithDepth: the bundled depth companion
    crd::gpu::FgBuffer buffer{};
    IAccelerationStructure* accel = nullptr;
    crd::u32 layer = 0;            // ImageLayer: which slice
};

// One draw the runtime resolves at execute time: `storage` is an FgBuffer (the graph-imported vertex-pull buffer,
// resolved via IFrameContext — a transient produced upstream resolves only now); `program`/`texture`/`args` are real
// host pointers the host already owns (the DrawItem's own handles, used directly, exactly as the legacy wrappers did).
struct AuthoredDraw
{
    crd::gpu::FgBuffer storage{};
    IStorageBuffer* args = nullptr;   // the GPU-driven indirect command buffer (host pointer, used directly)
    IRasterProgram* program = nullptr;
    ITexture* texture = nullptr;
    u32 vertex_count = 0;
    bool indexed = false;
    u32 index_count = 0;
    u32 instance_count = 0;
    u32 first_index = 0;
    u32 args_offset = 0;
    u32 dispatch_groups = 0;
    bool has_storage = false;
};

inline constexpr crd::u32 kMaxAuthoredCounters = 8U;

// One fully-resolved pass the recorder hands the dispatch. All device handles are FgImage/FgBuffer (resolved at execute
// time) or real host pointers (programs/textures the recorder already owns). The recorder builds these — including every
// scar rule of the legacy driver (the graph reads/writes, present, overlay stay on the recorder side) — so the dispatch
// callback stays a mechanical resolve-and-invoke.
struct AuthoredPass
{
    explicit AuthoredPass(memory::IAllocator* a) noexcept : bindings(a), draws(a) {}

    ExecutorTypeId executor{};                                     // resolved executor id (records store this, not a string)
    crd::gpu::FgPassKind device_kind = crd::gpu::FgPassKind::Raster;
    PassPayload payload;                                           // the executor payload (params + slot refs)
    Array<SlotBinding> bindings;                                   // payload slot → physical handle
    Array<AuthoredDraw> draws;                                     // the resolved draw/dispatch list (empty ⇒ none)
    // scene draw-list sampled-read routing (REN-40-D): the pass texture (shadow atlas / moment array), depth-ness and
    // sampler kind select the atlas/sampler arm inside the scene executor.
    crd::gpu::FgImage pass_texture{};
    bool has_pass_texture = false;
    bool pass_texture_is_depth = false;
    bool pass_texture_comparison = false;
    PassPrograms programs;                                         // raster / kernel / RT-SBT (real host pointers)
    // per-pass DEVICE setup (applied before the executor records), mirroring the legacy record_pass preamble:
    bool has_sampler = false;
    crd::gpu::SamplerDesc sampler{};
    crd::gpu::PassRasterState state{};                             // depth-write / bias / cull / stencil for this pass
    crd::gpu::FgBuffer counters[kMaxAuthoredCounters];             // counters zeroed first (REN-38-B3)
    u32 n_counters = 0;
    // ── runtime-filled (the recorder sets these; the fg callback reads them at execute time) ──
    const GraphExecutorTable* records = nullptr;                   // executor lookup for this pass's dispatch
    memory::IAllocator* alloc = nullptr;                           // per-callback scratch (ResourceTable + draw items)
    DiagnosticList* diags = nullptr;
    const CeirPassPlan* plan = nullptr;                            // CEIR-16-3c: this pass's CEIR replay plan (null ⇒ C++ record path)
    bool ok = true;                                                // set false if the executor touched an undeclared slot
};

// The ONE generic per-pass record callback (a gpu-context FgExecuteFn) — resolves an `AuthoredPass`'s handles to device
// pointers at execute time and dispatches its executor. A recorder adds each pass to its frame graph with this as the
// `execute(fn, &authored_pass)` callback. ONE dispatch — no FramePassKind, no per-kind wrappers.
[[nodiscard]] crd::gpu::FgExecuteFn authored_pass_fn() noexcept;
} // namespace crd::rendergraph
