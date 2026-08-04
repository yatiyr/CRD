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
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderpass/executor_registry.hpp>

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

// The context a pass's record function sees: it may resolve ONLY resources the pass declared (an undeclared slot is
// diagnosed — the "declared use matches recorded" contract), plus the host-bound program for this pass.
class RecordContext
{
public:
    RecordContext(const PassPayload& payload, const ResourceTable& table, const PassPrograms& programs,
                  DiagnosticList& diags, const DrawList* draws = nullptr) noexcept
        : m_payload(&payload), m_table(&table), m_programs(&programs), m_diags(&diags), m_draws(draws)
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
    [[nodiscard]] bool ok() const noexcept { return m_ok; }

private:
    [[nodiscard]] bool is_declared(u64 slot_name) const noexcept;
    const PassPayload* m_payload;
    const ResourceTable* m_table;
    const PassPrograms* m_programs;
    DiagnosticList* m_diags;
    const DrawList* m_draws = nullptr;
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
                           const DrawListTable* draw_lists = nullptr);

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
                                 const DrawListTable* draw_lists = nullptr);
} // namespace crd::rendergraph
