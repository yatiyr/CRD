#include "reload.hpp"

namespace crd::scenerender
{
using crd::renderasset::AssetId;
using crd::renderasset::ContentHash;
using crd::renderasset::DiagCode;
using crd::renderasset::DiagnosticList;
using crd::renderasset::Generation;
using crd::renderasset::InterfaceHash;

DeferredReleaseQueue::DeferredReleaseQueue(crd::memory::IAllocator* alloc, crd::u32 frames_in_flight) noexcept
    : m_items(alloc), m_frames_in_flight(frames_in_flight < 1U ? 1U : frames_in_flight)
{
}

void DeferredReleaseQueue::retire(void* object, ReleaseFn fn, void* ctx)
{
    if (object == nullptr || fn == nullptr)
    {
        return;
    }
    m_items.push_back(Item{object, fn, ctx, m_frame});
}

void DeferredReleaseQueue::begin_frame()
{
    m_frame += 1;
    // Release every item whose in-flight window has elapsed: retired at frame F, freed once m_frame >= F + N (frame F
    // has completed by then). Compact in place, preserving order for the survivors — deterministic, no reallocation.
    crd::usize keep = 0;
    for (crd::usize i = 0; i < m_items.size(); ++i)
    {
        const Item& it = m_items[i];
        if (it.retire_frame + m_frames_in_flight <= m_frame)
        {
            it.fn(it.object, it.ctx); // the in-flight window has passed — release it now
        }
        else
        {
            m_items[keep] = it;
            ++keep;
        }
    }
    while (m_items.size() > keep)
    {
        m_items.pop_back();
    }
}

crd::usize DeferredReleaseQueue::drain_all()
{
    const crd::usize n = m_items.size();
    for (crd::usize i = 0; i < n; ++i)
    {
        m_items[i].fn(m_items[i].object, m_items[i].ctx);
    }
    m_items.clear();
    return n;
}

RenderAssetReloader::RenderAssetReloader(crd::memory::IAllocator* alloc) noexcept
    : m_entries(alloc), m_deps(alloc), m_alloc(alloc)
{
}

void RenderAssetReloader::register_asset(AssetId id, const ReloadableVtbl* vtbl, void* user,
                                         ContentHash initial_content, const AssetId* deps, crd::usize n_deps)
{
    if (!id.valid() || vtbl == nullptr)
    {
        return;
    }
    if (Entry* e = find(id))
    {
        e->vtbl    = vtbl; // re-init: swap the plug-in, keep the dependency edges + last content
        e->user    = user;
        e->content = initial_content;
    }
    else
    {
        m_entries.push_back(Entry{id, vtbl, user, initial_content});
    }
    m_deps.add_node(id);
    for (crd::usize i = 0; i < n_deps; ++i)
    {
        m_deps.add_edge(id, deps[i]); // "id depends on deps[i]"
    }
}

ReloadOutcome RenderAssetReloader::reload(AssetId changed, DiagnosticList& diags)
{
    Entry* ex = find(changed);
    if (ex == nullptr)
    {
        diags.error(DiagCode::AssetNotFound, "reload: asset is not registered as reloadable");
        return ReloadOutcome{};
    }

    // 1) Re-cook the CHANGED asset into its staging slot.
    ContentHash   new_content{};
    InterfaceHash new_iface{};
    if (!ex->vtbl->stage(ex->user, diags, new_content, new_iface))
    {
        // Cook/validate failed → keep the last valid generation, never install a partial (§13 last-good). The
        // diagnostic naming the failing asset + reason was emitted by `stage`.
        return ReloadOutcome{false, false, ex->vtbl->generation(ex->user)};
    }
    if (new_content == ex->content)
    {
        ex->vtbl->discard(ex->user); // byte-identical re-cook → no swap, nothing downstream to rebuild
        return ReloadOutcome{true, false, ex->vtbl->generation(ex->user)};
    }

    // 2) The content changed → build the DEPENDENT rebuild set (deps-first) and re-cook each one. Any dependent that
    //    can no longer re-cook against the new asset — the interface-change rejection — fails the WHOLE set.
    crd::containers::Array<AssetId> affected(m_alloc);
    if (!m_deps.affected_by(changed, affected, diags))
    {
        ex->vtbl->discard(ex->user); // a cyclic graph has no safe rebuild order (diagnostic already emitted)
        return ReloadOutcome{false, false, ex->vtbl->generation(ex->user)};
    }

    struct Staged
    {
        Entry*      entry;
        ContentHash content;
    };
    crd::containers::Array<Staged> staged(m_alloc);
    bool                           set_ok = true;
    for (crd::usize i = 0; i < affected.size(); ++i)
    {
        Entry* ed = find(affected[i]);
        if (ed == nullptr)
        {
            continue; // in the dep graph but not registered as reloadable — nothing for this reloader to rebuild
        }
        ContentHash   dc{};
        InterfaceHash di{};
        if (!ed->vtbl->stage(ed->user, diags, dc, di))
        {
            set_ok = false; // §13: a mandatory dependent could not rebuild (e.g. an interface change) → reject the set
            break;
        }
        if (dc == ed->content)
        {
            ed->vtbl->discard(ed->user); // this dependent's re-cook is byte-identical → it is unaffected, keep its
                                         // generation (dependency-chain invalidation touches only what actually changed)
        }
        else
        {
            staged.push_back(Staged{ed, dc});
        }
    }

    if (!set_ok)
    {
        // Roll back: discard every staged object (changed + dependents). NOT ONE generation was bumped, so no caller
        // can ever observe a mixed-generation set — the previous valid generation of every asset stands (last-good).
        for (crd::usize i = 0; i < staged.size(); ++i)
        {
            staged[i].entry->vtbl->discard(staged[i].entry->user);
        }
        ex->vtbl->discard(ex->user);
        return ReloadOutcome{false, false, ex->vtbl->generation(ex->user)};
    }

    // 3) The whole set cooked → COMMIT atomically: the changed asset first, then dependents in dependency order. The
    //    caller invokes reload() at a frame boundary, so no frame observes a half-committed set; retired objects go to
    //    deferred destruction (Inc4). Every generation bumps together.
    ex->vtbl->commit(ex->user);
    ex->content = new_content;
    for (crd::usize i = 0; i < staged.size(); ++i)
    {
        staged[i].entry->vtbl->commit(staged[i].entry->user);
        staged[i].entry->content = staged[i].content;
    }
    return ReloadOutcome{true, true, ex->vtbl->generation(ex->user)};
}

Generation RenderAssetReloader::generation_of(AssetId id) const
{
    const Entry* e = find(id);
    return (e != nullptr) ? e->vtbl->generation(e->user) : Generation{};
}

bool RenderAssetReloader::is_registered(AssetId id) const
{
    return find(id) != nullptr;
}

RenderAssetReloader::Entry* RenderAssetReloader::find(AssetId id)
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].id == id)
        {
            return &m_entries[i];
        }
    }
    return nullptr;
}

const RenderAssetReloader::Entry* RenderAssetReloader::find(AssetId id) const
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].id == id)
        {
            return &m_entries[i];
        }
    }
    return nullptr;
}
} // namespace crd::scenerender
