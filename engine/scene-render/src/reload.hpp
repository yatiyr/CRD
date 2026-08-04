#pragma once

// RAF-11 — dependency-aware hot reload of render assets (scene-render orchestration; mission §13).
//
// The reloader owns the ORCHESTRATION: find the changed asset, re-cook it, decide no-op-vs-swap by content hash,
// bump the generation, and keep the last valid generation on any cook failure (never install a partial). Inc3 adds
// the dependency-ordered REBUILD SET (`DependencyGraph::affected_by`) with all-or-none atomic commit + the
// interface-change rule; Inc4 adds deferred GPU destruction of the retired objects.
//
// Each asset KIND (frame graph · program · technique · material) plugs in a tiny vtable of pure functions over a
// `void* user` — the engine's fn-ptr + void* idiom (NO std::function, NO virtual across the seam). The
// generation/staleness substrate is RAF-3's `RuntimeSlot` (the kind owns its slot; the reloader drives it), so
// "generation" here is always the kind's own slot generation — one source of truth, never a second counter.

#include <crd/containers/array.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/cooked.hpp>     // ContentHash · InterfaceHash · Generation
#include <crd/renderasset/dependency.hpp> // DependencyGraph · affected_by (RAF-11 rebuild set)
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp>

namespace crd::scenerender
{
// RAF-11 §13 — DEFERRED GPU DESTRUCTION. When a hot reload replaces a GPU object (an old program / pipeline), the
// old object may still be referenced by frames the GPU has not finished. It must NOT be freed until every such
// in-flight frame completes. This is a frame-indexed release queue: retire the old object here, and it is released
// once `frames_in_flight` `begin_frame()` cycles have elapsed — by which point the frame that retired it has
// completed, exactly the guarantee the frame graph's per-slot fences give its own transients (kFramesInFlight-deep
// fencing). Type-erased release via fn-ptr + void* (NO std::function); the release fn is where the actual device
// destruction happens, so this class itself stays device-free (and unit-testable without a GPU).
class DeferredReleaseQueue
{
public:
    using ReleaseFn = void (*)(void* object, void* ctx);

    // `frames_in_flight` must match the presenter's in-flight depth (Vulkan/DX12 both keep kFramesInFlight = 2). A
    // value of 0 is treated as 1 (release on the very next frame — never same-frame, which would race the GPU).
    DeferredReleaseQueue(crd::memory::IAllocator* alloc, crd::u32 frames_in_flight) noexcept;

    // Retire `object` for deferred release; `fn(object, ctx)` frees it once the in-flight window has elapsed.
    void retire(void* object, ReleaseFn fn, void* ctx);
    // Advance one rendered frame: release everything whose in-flight window has now passed. Call ONCE per frame,
    // AFTER submitting that frame (so the retire-frame accounting matches the fence the presenter waits on).
    void begin_frame();
    // Release EVERYTHING immediately — only valid once the device is idle (shutdown / a hard resize). Returns the
    // number of objects released.
    crd::usize drain_all();

    [[nodiscard]] crd::usize pending() const noexcept { return m_items.size(); }
    [[nodiscard]] crd::u64   frame() const noexcept { return m_frame; }

private:
    struct Item
    {
        void*       object;
        ReleaseFn   fn;
        void*       ctx;
        crd::u64    retire_frame;
    };
    crd::containers::Array<Item> m_items;
    crd::u64                     m_frame = 0;
    crd::u32                     m_frames_in_flight;
};

// The outcome of a reload request. A byte-identical re-cook is a clean no-op (`ok && !changed`); a real swap is
// `ok && changed`; a cook/validate failure is `!ok` with `generation` still at the last valid value.
struct ReloadOutcome
{
    bool                         ok = false;      // request completed without leaving a broken/partial state
    bool                         changed = false; // the cooked content actually differed → a real swap happened
    crd::renderasset::Generation generation{};    // the primary asset's generation AFTER the request
};

// One reloadable asset kind's plug-in, type-erased over `user`. Every function is pure over `user`; none may throw.
struct ReloadableVtbl
{
    // Re-read source → reparse → validate → COOK the new object into a STAGING slot inside `user`. Fill `out_content`
    // (the cook-cache key — identical content ⇒ no swap) and `out_iface` (the public interface signature — a change
    // forces dependents to revalidate, Inc3). Return false + emit a diagnostic on ANY failure; on failure NOTHING is
    // staged and the live object is untouched (last-good preserved).
    bool (*stage)(void* user, crd::renderasset::DiagnosticList& diags, crd::renderasset::ContentHash& out_content,
                  crd::renderasset::InterfaceHash& out_iface);
    // Publish the staged object: install it into the kind's `RuntimeSlot` (bumping its generation) and hand the
    // previous object to deferred destruction (Inc4). Only ever called after a `stage` the reloader chose to commit.
    void (*commit)(void* user);
    // Drop the staged object WITHOUT publishing (content was identical, or the replacement set was rejected).
    void (*discard)(void* user);
    // The kind's current `RuntimeSlot` generation — the single source of truth for staleness.
    crd::renderasset::Generation (*generation)(void* user);
    // The last cooked INTERFACE hash of the live object (for the Inc3 interface-change comparison). May be a stored
    // value the kind updates on commit.
    crd::renderasset::InterfaceHash (*iface)(void* user);
};

class RenderAssetReloader
{
public:
    explicit RenderAssetReloader(crd::memory::IAllocator* alloc) noexcept;

    // Register a reloadable asset with the content hash it was loaded with (so an immediate re-cook of unchanged
    // source is correctly a no-op) and the ids it depends on (edges into the dependency graph for the Inc3 rebuild
    // set). Re-registering the same id replaces its plug-in (a re-init path), keeping its dependency edges.
    void register_asset(crd::renderasset::AssetId id, const ReloadableVtbl* vtbl, void* user,
                        crd::renderasset::ContentHash initial_content,
                        const crd::renderasset::AssetId* deps = nullptr, crd::usize n_deps = 0);

    // Reload a changed asset AND every asset that transitively depends on it, as ONE atomic set (§13):
    //   1. Re-cook the changed asset. If its content is byte-identical → a clean no-op (nothing else touched).
    //   2. Otherwise re-cook every dependent in `affected_by(changed)` order. A dependent whose re-cook FAILS —
    //      including because the changed asset's INTERFACE changed in a way it can no longer bind — rejects the WHOLE
    //      set: nothing commits, every generation stays at its last valid value, and the failing asset + reason are
    //      reported. No partial install, no mixed generation is ever observable.
    //   3. If the whole set cooks, COMMIT it — the changed asset first, then its dependents in dependency order —
    //      bumping every generation. Call this at a safe frame boundary (no frame recording); the retired objects are
    //      handed to deferred destruction (Inc4). The outcome reports the PRIMARY (changed) asset's result.
    [[nodiscard]] ReloadOutcome reload(crd::renderasset::AssetId changed, crd::renderasset::DiagnosticList& diags);

    [[nodiscard]] crd::renderasset::Generation generation_of(crd::renderasset::AssetId id) const;
    [[nodiscard]] bool                         is_registered(crd::renderasset::AssetId id) const;
    [[nodiscard]] crd::usize                   asset_count() const noexcept { return m_entries.size(); }
    // Exposed for the Inc3 rebuild-set computation and its gate.
    [[nodiscard]] const crd::renderasset::DependencyGraph& dependencies() const noexcept { return m_deps; }

private:
    struct Entry
    {
        crd::renderasset::AssetId     id;
        const ReloadableVtbl*         vtbl;
        void*                         user;
        crd::renderasset::ContentHash content; // last committed content hash (no-op detection)
    };
    [[nodiscard]] Entry*       find(crd::renderasset::AssetId id);
    [[nodiscard]] const Entry* find(crd::renderasset::AssetId id) const;

    crd::containers::Array<Entry>     m_entries; // insertion order; small N, linear lookup (like ProgramRegistry)
    crd::renderasset::DependencyGraph m_deps;
    crd::memory::IAllocator*          m_alloc; // for the per-reload rebuild-set + staged-set scratch arrays
};
} // namespace crd::scenerender
