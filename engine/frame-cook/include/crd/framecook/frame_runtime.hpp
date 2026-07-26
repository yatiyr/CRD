#pragma once

// frame_runtime.hpp — REN-36.2 (D-007 row 139): EXECUTE a cooked frame graph.
//
// Takes the `FrameGraphDesc` a `.crdr` deserializes into and drives the ordinary `IFrameGraph` — create the
// transients, import the output, declare each pass's reads/writes, and record the right `draw_*` per
// `FramePassKind`. Ordering, barriers, transient aliasing and the single submission are all still DERIVED by
// the graph exactly as they are for a hand-written frame: this layer adds no rendering machinery, it only
// replaces the `add_pass()` CALLS with data.
//
// ⛔ THE GATE THIS EXISTS TO PASS: one cooked asset, loaded unmodified on Vulkan AND DX12, produces readback
// BIT-IDENTICAL to the hand-written C++ frame. Not "works on both" — the same bytes.

#include <crd/framecook/frame_asset.hpp>

#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/raster_context.hpp>

namespace crd::framecook
{

// What a draw list resolves to when a pass records. REN-36.2 binds these STATICALLY through the host; REN-36.3
// replaces the binding with the real ECS query the asset already declares (`all`/`any`/`none` + cull + sort).
// ONE recorded draw within a resolved draw list. A real scene resolves to MANY of these (one per mesh group /
// per material batch), which is why a draw list is a LIST and not a single binding: a pass that could only ever
// issue one draw would force every multi-group technique back into hand-written C++, defeating the whole point
// of authoring the graph.
struct DrawItem
{
    crd::gpu::IStorageBuffer* storage      = nullptr; // the vertex-pull buffer this draw reads
    crd::gpu::IRasterProgram* program      = nullptr; // the program for this draw (per-material / per-group)
    crd::u32                  vertex_count = 0U;
    // REN-37.10: this draw's own MATERIAL TEXTURE (a base-colour map), appended at the END of the struct.
    // ⛔ It must beat the pass's sampled read. A geometry pass that reads the shadow atlas binds that atlas for
    // every draw; without a per-draw override, a group carrying an albedo map would silently lose it the moment
    // shadows turned on — a visible regression that looks like the texture failed to load.
    crd::gpu::ITexture*       texture      = nullptr;
};

inline constexpr crd::u32 kMaxDrawItems = 256; // stated cap; `resolved` reports what was actually filled

struct DrawListBinding
{
    // The host fills `items[0 .. resolved-1]`. `storage`/`program`/`vertex_count` remain as the FIRST item so
    // every existing single-draw host keeps working untouched.
    crd::gpu::IStorageBuffer* storage      = nullptr;
    crd::gpu::IRasterProgram* program      = nullptr;
    crd::u32                  vertex_count = 0U;

    DrawItem items[kMaxDrawItems]{};
    crd::u32 resolved = 0U; // 0 => use the single storage/program/vertex_count triple above

    // Uniform view for the executor: one draw when the host filled the legacy triple, N when it filled `items`.
    [[nodiscard]] crd::u32 count() const noexcept { return resolved > 0U ? resolved : (storage != nullptr ? 1U : 0U); }
    [[nodiscard]] DrawItem at(crd::u32 i) const noexcept
    {
        if (resolved > 0U) { return items[i]; }
        return DrawItem{storage, program, vertex_count, nullptr};
    }
};

// The seam between an authored graph and the running application. The asset names things (`"@output"`,
// `"crd://shaders/post/agx_tonemap"`, `"shadow_casters"`); the host resolves those names to live objects. Keeping
// resolution behind this interface is what lets the SAME asset run in a test, a game and an editor viewport.
class IFrameGraphHost
{
public:
    IFrameGraphHost()                                  = default;
    virtual ~IFrameGraphHost()                         = default;
    IFrameGraphHost(const IFrameGraphHost&)            = delete;
    IFrameGraphHost& operator=(const IFrameGraphHost&) = delete;
    IFrameGraphHost(IFrameGraphHost&&)                 = delete;
    IFrameGraphHost& operator=(IFrameGraphHost&&)      = delete;

    // The target `"@output"` refers to. Must not be null for a well-formed graph (the cooker already guarantees
    // some pass writes @output).
    [[nodiscard]] virtual crd::gpu::IRasterTarget* output() = 0;

    // A cooked shader/kernel id → a live program. Null ⇒ the pass is SKIPPED and `execute_frame_graph` reports
    // failure; a missing program must never silently render nothing that looks plausible.
    [[nodiscard]] virtual crd::gpu::IRasterProgram* program(crd::containers::StringView id) = 0;

    // A draw-list name → what to draw. Returns false if the name is unknown.
    [[nodiscard]] virtual bool draw_list(crd::containers::StringView name, DrawListBinding& out) = 0;

    // Does the device provide this capability? Drives the asset's `requires` tier (REN-35's rule: degrade by
    // DECLARED capability, never by graphics API). Default: nothing is provided, so a graph that requires
    // anything falls back — the conservative direction.
    [[nodiscard]] virtual bool capability(crd::containers::StringView /*name*/) { return false; }

    // Resolve the graph named by `desc.fallback`. Returning null ends the chain at the ERROR GRAPH. In a shipped
    // engine this reads the built-in pack (mounted first); in a test it is a member.
    [[nodiscard]] virtual const FrameGraphDesc* fallback_graph(crd::containers::StringView /*name*/) { return nullptr; }

    // ── REN-36.3: MULTI-VIEW EXPANSION. Appended at the END of the vtable (D135). ──
    // How many instances does this `for_each` generator produce THIS frame? The count is the HOST's to answer
    // because it is scene state: how many cascades this light has, how many shadow-casting lights exist, whether
    // stereo is engaged. The default is 0, and the runtime treats 0 as the NAMED failure
    // `FrameExecError::UnresolvedForEach` — so a host that never implements this gets a report, not a shadow
    // graph that silently renders nothing.
    [[nodiscard]] virtual crd::u32 for_each_count(FrameForEach /*kind*/, crd::u32 /*arg*/) { return 0U; }

    // The program for instance `index` of an expanded pass. Returning null means "use the pass's own program",
    // which is the COMMON case: one shadow shader, and the per-cascade `light_vp` arrives as a pass-frequency
    // uniform (the ADR-0102 set-frequency model). Override it when instances genuinely need different programs.
    [[nodiscard]] virtual crd::gpu::IRasterProgram* instance_program(crd::containers::StringView /*pass*/,
                                                                     crd::u32 /*index*/)
    {
        return nullptr;
    }

    // ── REN-36.3-b: the ECS QUERY seam. Appended at the END of the vtable (D135). ──
    // The executor resolves draw lists through THIS, handing over the graph's whole `FrameDrawListDesc` — the
    // `all`/`any`/`none` component filters, cull mode, sort mode and limit the asset declared — not just a name.
    // ⛔ Before this the filters were parsed, validated, cooked and round-tripped, then DROPPED at execution:
    // the host got a bare name and invented its own list, so an authored query could not actually select
    // anything. A declared-but-ignored filter is worse than an unsupported one — it reads as working.
    //
    // Query EVALUATION deliberately lives in the host, not here. `crd-frame-cook` must never depend on
    // `crd-scene` (its one-way edge is what keeps the asset format free of engine types); the renderer owns the
    // World, so the renderer runs the query. The default forwards to the name-based `draw_list` so every
    // existing host keeps working unchanged — it simply ignores filters it never asked for.
    [[nodiscard]] virtual bool draw_list_query(const FrameDrawListDesc& query, DrawListBinding& out)
    {
        return draw_list(crd::containers::StringView(query.name.c_str(), query.name.size()), out);
    }
};

// ── WHY a graph failed. Reported, never swallowed. ───────────────────────────────────────────────────────────
enum class FrameExecError : crd::u8
{
    Ok = 0,
    NoOutput,             // the host resolved "@output" to null
    UnresolvedProgram,    // a pass names a shader the host could not resolve
    UnresolvedDrawList,   // a pass names a draw list the host does not know
    UnresolvedResource,   // a pass reads/writes a resource the graph does not declare
    TransientFailed,      // the device refused a declared transient (format/size unsupported)
    BuildRejected,        // IFrameGraph::build() rejected the graph
    UnsupportedCapability, // a `requires` entry the device does not provide
    // REN-36.3: a pass declares `for_each` but the host answered 0 instances. A zero count is REPORTED, never
    // treated as "skip this pass" — a shadow graph that silently renders no cascades looks exactly like a scene
    // with no shadows. Appended at the END of the enum.
    UnresolvedForEach
};

// WHICH graph actually ran. ⛔ A caller must be able to tell "my graph ran" from "something else ran instead" —
// a fallback that reports success is indistinguishable from a working frame, which is the exact class of lie
// this engine refuses elsewhere (a missing program FAILS rather than rendering something plausible).
enum class FrameExecStatus : crd::u8
{
    Ok = 0,               // the AUTHORED graph ran
    FellBackToDefault,    // the authored graph failed; the host's fallback graph ran
    FellBackToErrorGraph, // everything failed; the built-in ERROR GRAPH ran (loud magenta — see below)
    Failed                // nothing ran at all
};

struct FrameExecResult
{
    FrameExecStatus         status = FrameExecStatus::Ok;
    FrameExecError          error  = FrameExecError::Ok;  // why the AUTHORED graph failed (Ok if it did not)
    crd::containers::String graph;                        // the name of the graph that actually ran
    crd::containers::String where;                        // the offending pass / resource / shader id

    explicit FrameExecResult(crd::memory::IAllocator* a) : graph(a), where(a) {}
};

[[nodiscard]] const char* frame_exec_error_text(FrameExecError e) noexcept;
[[nodiscard]] const char* frame_exec_status_text(FrameExecStatus s) noexcept;

// ── REN-37.10: RECORD an authored graph into a CALLER-OWNED frame graph. ────────────────────────────────────
// `execute_frame_graph` below creates a graph, records into it, builds and executes — fine for one view, and
// structurally wrong for a frame that has several. A recorder lets a host reset ONE graph, record N authored
// graphs into it (one per viewport), then build and execute ONCE. That is the frame-level model (REN-37.8) at
// the ASSET layer, and it is the same split `SceneRenderer::contribute()` makes at the renderer layer.
//
// ⛔ WHY THIS IS A CLASS AND NOT A FREE FUNCTION. `add_pass(...).execute(fn, user)` stores the USER POINTER and
// dereferences it at `execute()` time. While recording and executing happened in one call, that pointer could be
// a LOCAL. Split them and every such local DANGLES — the host executes long after `record()` returned. The
// recorder owns that storage, at addresses reserved up front so no growth can move an entry the graph already
// points at (the same discipline the `for_each` expansion table uses).
class FrameRecorder
{
public:
    explicit FrameRecorder(crd::memory::IAllocator* alloc);
    ~FrameRecorder();
    FrameRecorder(const FrameRecorder&)            = delete;
    FrameRecorder& operator=(const FrameRecorder&) = delete;
    FrameRecorder(FrameRecorder&&)                 = delete;
    FrameRecorder& operator=(FrameRecorder&&)      = delete;

    // Recycle the arena. Call ONCE per frame, right after the host's `IFrameGraph::reset()` and before the first
    // `record()`. The cap is CHECKED, so forgetting it surfaces as a reported failure, not a use-after-free.
    void begin_frame() noexcept;

    // How many authored graphs one frame may record. Stated rather than hidden: exceeding it FAILS by name.
    static constexpr crd::u32 kMaxRecordingsPerFrame = 32U;

    [[nodiscard]] bool record(const FrameGraphDesc& desc, crd::gpu::IFrameGraph& fg, crd::gpu::IRasterContext& raster,
                              IFrameGraphHost& host, FrameExecError* err = nullptr,
                              crd::containers::String* where = nullptr);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// Build + execute `desc` once. `err`/`where` receive the failure reason when it returns false — never a
// partially-recorded frame. Exactly `FrameRecorder::record` plus create/build/execute, so the one-view and
// multi-view paths cannot drift.
//
// `for_each` expansion (REN-36.3) is NOT applied here yet: a pass declaring `for_each` records ONCE with index 0.
// That is stated rather than silent because a caller could otherwise mistake one cascade for four.
[[nodiscard]] bool execute_frame_graph(const FrameGraphDesc& desc, crd::gpu::IRasterContext& raster,
                                       IFrameGraphHost& host, FrameExecError* err = nullptr,
                                       crd::containers::String* where = nullptr);

// ── The FALLBACK CHAIN: authored → the graph its `fallback` names → the built-in ERROR GRAPH. ────────────────
// Every step down is REPORTED (returned in the result AND logged at Error level). The terminal step paints the
// output a loud magenta so a fallback is IMPOSSIBLE to mistake for a working frame on screen.
//
// ⛔ THE ERROR GRAPH IS CODE, NOT AN ASSET — deliberately. If it were an asset it would share the failure modes
// it exists to report: a broken cook, a bad mount, a missing pack would take the error path down with it. It
// uses `IRasterContext::clear`, which needs no program, no shader, no resource and no host resolution, so it
// still works when literally every other thing is broken.
[[nodiscard]] FrameExecResult execute_frame_graph_with_fallback(const FrameGraphDesc& desc,
                                                                crd::gpu::IRasterContext& raster,
                                                                IFrameGraphHost&          host,
                                                                crd::memory::IAllocator*  alloc);

// The colour the error graph paints. Loud magenta — the industry's universal "this is missing" signal, chosen so
// no plausible authored scene produces it by accident.
inline constexpr float kErrorGraphColor[4] = {1.0F, 0.0F, 1.0F, 1.0F};

} // namespace crd::framecook
