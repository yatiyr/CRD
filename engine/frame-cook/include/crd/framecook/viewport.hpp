#pragma once

// viewport.hpp — REN-37.8 + 37.9 (D-007 row 140): THE FRAME-LEVEL GRAPH and its VIEWPORT SCHEDULER.
//
// ⛔⛔ THE DEFECT THIS EXISTS TO FIX. `SceneRenderer::render()` creates AND executes its own frame graph per call.
// That is fine for a game with one view and structurally wrong for anything else: an editor with a main viewport,
// an animation preview and 12 dirty thumbnails **SUBMITS 14 TIMES**, allocates every viewport's transients
// separately (peak VRAM = SUM instead of MAX), cannot order one viewport against another, and duplicates shared
// work — two views lighting the same scene each rebuild the shadow atlas, and nothing can notice they are the same.
//
// ⭐ THE MODEL: **ONE frame graph per FRAME. A viewport is a SUBGRAPH INSTANCE inside it.**
// Every mechanism this needs already exists: `[[include]]` + `bind` + `as` namespacing (REN-37.6) compose them,
// `for_each` (REN-36.3) expands multi-view passes, `persistent_image` (REN-37.5) caches a settled thumbnail, and
// the Kahn sort (REN-1) — which never knew viewports existed — orders one against another for free.
//
// So this header adds NO graph machinery. It adds the COMPOSER that assembles a per-frame `FrameGraphDesc` from a
// viewport registry, and the SCHEDULER that decides which viewports are in it.
//
// ⛔ WHAT MUST NOT HAPPEN — and this is the rule, not a preference:
//   · NO `render_thumbnail()` / `render_preview()` entry points. A viewport KIND is a different authored GRAPH
//     (`thumbnail.frame.toml`: one pass, no shadows, no post), never the main renderer with features switched off
//     in C++. The moment a kind gets its own function, the top rule is dead and the editor look starts drifting
//     from the shipped one.
//   · NO implicit viewport. If the registry is empty, the frame is empty.
//   · NO cross-viewport global state. Everything a viewport needs arrives through its `bind`, which is exactly
//     what makes two instances of one graph safe.

#include <crd/framecook/frame_asset.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::gpu
{
class IRasterTarget;
}

namespace crd::framecook
{

// ── REN-37.9: WHEN a viewport renders. ──────────────────────────────────────────────────────────────────────
enum class ViewportPolicy : crd::u8
{
    EveryFrame = 0, // the main viewport, a playing animation preview, a live VFX viewer
    OnDemand,       // renders when DIRTY, then holds its last result — thumbnails, static previews
    Periodic,       // renders at most every N frames — a background bake preview, a far reflection probe
};

// What a viewport's freshness DEPENDS on. ⛔ A dirty flag set by hand is a bug generator: the failure is a STALE
// THUMBNAIL, which is indistinguishable from a correct one by eye. Declaring the dependency lets the registry
// invalidate mechanically, and lets a debug build re-render and compare to catch a MISSING declaration.
enum class DependencyKind : crd::u8
{
    Asset = 0, // a mesh / texture / clip this viewport shows
    Material,  // a material it shades with
    Camera,    // its own view moved
    Resize,    // its target changed size
    Manual,    // the app invalidated it explicitly
};

struct ViewportDesc
{
    crd::containers::String  id;         // "main", "thumb.17", "anim.preview" — ALSO the include namespace
    crd::containers::String  graph;      // the authored graph this viewport runs
    crd::containers::String  draw_scope; // an ECS-query scope its draw lists intersect with (may be empty)
    crd::gpu::IRasterTarget* target = nullptr; // where it renders (swapchain view, or a persistent RT)
    ViewportPolicy           policy = ViewportPolicy::EveryFrame;
    crd::u32                 priority      = 0;  // scheduling priority; ties break on AGE
    crd::u32                 period_frames = 1;  // Periodic: render at most every N frames
    crd::u32                 width         = 0;  // for the pixel budget (0 ⇒ not charged against it)
    crd::u32                 height        = 0;
    bool                     present  = false;   // true for the ONE viewport that owns the swapchain
    // ⛔ REN-37.9: readback is PER VIEWPORT. A thumbnail CAPTURE wants the CPU copy and pays the stall once; a
    // live viewport must never pay it. The flag may only remove the WAIT, never a barrier — the REN-8 scar where
    // readback-off silently dropped a layout transition the present path depended on.
    bool                     readback = false;

    explicit ViewportDesc(crd::memory::IAllocator* a) : id(a), graph(a), draw_scope(a) {}
};

// ── REN-37.9: the BUDGET, in measured GPU milliseconds. ─────────────────────────────────────────────────────
// ⭐ `max_gpu_ms` closes the loop with REN-8: each viewport is charged its LAST MEASURED cost, so the budget is in
// real time rather than a guessed pixel count. A viewport with no measurement yet is charged
// `unmeasured_cost_ms` — deliberately pessimistic, so a first-time expensive viewport cannot blow the frame.
struct ViewportBudget
{
    crd::u32 max_viewports      = 8;
    crd::u64 max_pixels         = 8ULL << 20U;
    crd::u32 max_draw_items     = 4096;
    double   max_gpu_ms         = 4.0;
    double   unmeasured_cost_ms = 1.0;
};

// One registered viewport's live state.
struct ViewportState
{
    ViewportDesc desc;
    bool         dirty          = true; // a viewport has never rendered, so it starts dirty
    crd::u32     skipped        = 0;    // consecutive frames deferred — the AGEING term
    crd::u32     last_rendered  = 0;    // frame index
    double       last_gpu_ms    = 0.0;  // REN-8 feedback; 0 ⇒ never measured
    crd::containers::Array<crd::u64> deps; // (kind << 56) | key — what invalidates it

    explicit ViewportState(crd::memory::IAllocator* a) : desc(a), deps(a) {}
};

// The registry. Owns viewport state; knows nothing about GPUs, graphs or scheduling policy beyond the fields.
class ViewportRegistry
{
public:
    explicit ViewportRegistry(crd::memory::IAllocator* a) : m_v(a), m_alloc(a) {}

    [[nodiscard]] crd::u32 add(const ViewportDesc& d);
    [[nodiscard]] crd::u32 count() const noexcept { return static_cast<crd::u32>(m_v.size()); }
    [[nodiscard]] ViewportState&       at(crd::u32 i) noexcept { return m_v[i]; }
    [[nodiscard]] const ViewportState& at(crd::u32 i) const noexcept { return m_v[i]; }
    [[nodiscard]] crd::i64 find(crd::containers::StringView id) const;

    // Declare what a viewport's freshness depends on. `key` is a caller-chosen id (an asset guid hash, a
    // material id); `Camera`/`Resize` conventionally use the viewport's own index.
    void depends_on(crd::u32 viewport, DependencyKind kind, crd::u64 key);

    // Invalidate everything that declared this dependency. Returns how many viewports were dirtied — REPORTED so
    // "nothing re-rendered" is distinguishable from "nothing needed to".
    crd::u32 invalidate(DependencyKind kind, crd::u64 key);
    void     invalidate_viewport(crd::u32 viewport);

    // REN-8 feedback: what this viewport actually cost on the GPU last time it ran.
    void note_cost(crd::u32 viewport, double gpu_ms) noexcept;

private:
    crd::containers::Array<ViewportState> m_v;
    crd::memory::IAllocator*              m_alloc = nullptr;
};

// What the scheduler decided this frame.
struct ViewportSelection
{
    crd::containers::Array<crd::u32> active;   // composed into this frame's graph
    crd::containers::Array<crd::u32> deferred; // dirty but over budget — REPORTED, never silently dropped
    double                           charged_ms = 0.0;

    explicit ViewportSelection(crd::memory::IAllocator* a) : active(a), deferred(a) {}
};

// ── REN-37.9: SELECT which viewports run this frame. ────────────────────────────────────────────────────────
// 1. Every `EveryFrame` viewport is admitted FIRST and charged. ⛔ The main viewport is never starved by
//    thumbnails — that is non-negotiable, and is why it is not merely "priority 0".
// 2. Remaining budget goes to dirty `OnDemand` / due `Periodic` viewports, ordered by **priority, then AGE**.
// 3. ⭐ AGEING IS WHAT PREVENTS STARVATION. Ordering by priority alone lets a stream of high-priority
//    invalidations keep one thumbnail permanently unrendered; each skipped frame raises its effective priority,
//    so every dirty viewport renders in bounded time.
// 4. Whatever does not fit stays dirty and lands in `deferred`. A silent cap reads as "everything rendered".
void select_viewports(ViewportRegistry& reg, const ViewportBudget& budget, crd::u32 frame,
                      ViewportSelection& out);

// Mark the selected viewports as rendered (clears dirty, resets ageing) and age the deferred ones. Called after
// the frame executes, so a viewport that failed to render is not falsely marked clean.
void commit_selection(ViewportRegistry& reg, const ViewportSelection& sel, crd::u32 frame);

// ── REN-37.8: COMPOSE the frame. ────────────────────────────────────────────────────────────────────────────
// Assembles ONE `FrameGraphDesc` whose `[[include]]`s are the active viewports, each namespaced by its id and
// bound to its own output. `shared` (may be null) is a graph included ONCE, outside any viewport — the shadow
// atlas, the sky, the IBL — so two viewports that read it order behind the single producer automatically.
//
// The result still goes through `flatten_frame_graph` + `validate_frame_graph`, so a viewport that names a
// missing resource fails BY NAME rather than rendering a black panel.
//
// ⛔ Exactly ONE active viewport may set `present`. Two would each try to own the swapchain; zero is legal (an
// offscreen frame — a thumbnail bake, a test) and is NOT an error.
[[nodiscard]] FrameCookError compose_frame(const ViewportRegistry& reg, crd::containers::ConstSpan<crd::u32> active,
                                           const crd::containers::String* shared, FrameGraphDesc& out,
                                           crd::containers::String* where = nullptr);

} // namespace crd::framecook
