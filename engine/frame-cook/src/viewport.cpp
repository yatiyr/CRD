// viewport.cpp — REN-37.8 + 37.9: the frame-level graph's composer and its budgeted viewport scheduler.
// Contract + rationale: viewport.hpp and docs/design/ren-37-material-technique-composition.md §14-§15.

#include <crd/framecook/viewport.hpp>

#include <cstring>

namespace crd::framecook
{
namespace
{
void copy_str(crd::containers::String& dst, const crd::containers::String& s)
{
    dst.clear();
    dst.append(s.c_str());
}
void set_str(crd::containers::String& dst, crd::containers::StringView s)
{
    dst.clear();
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char one[2] = {s[i], '\0'};
        dst.append(static_cast<const char*>(one));
    }
}
[[nodiscard]] bool str_eq(const crd::containers::String& a, crd::containers::StringView b)
{
    return a.size() == b.size() && std::memcmp(a.c_str(), b.data(), b.size()) == 0;
}
// A dependency is (kind, key) packed into one word so the registry needs no second container.
[[nodiscard]] crd::u64 dep_word(DependencyKind k, crd::u64 key) noexcept
{
    return (static_cast<crd::u64>(k) << 56U) | (key & 0x00FFFFFFFFFFFFFFULL);
}
} // namespace

crd::u32 ViewportRegistry::add(const ViewportDesc& d)
{
    ViewportState s(m_alloc);
    copy_str(s.desc.id, d.id);
    copy_str(s.desc.graph, d.graph);
    copy_str(s.desc.draw_scope, d.draw_scope);
    s.desc.target        = d.target;
    s.desc.policy        = d.policy;
    s.desc.priority      = d.priority;
    s.desc.period_frames = d.period_frames == 0U ? 1U : d.period_frames;
    s.desc.width         = d.width;
    s.desc.height        = d.height;
    s.desc.present       = d.present;
    s.desc.readback      = d.readback;
    // ⛔ A new viewport starts DIRTY. It has never rendered, so "clean" would mean "its cached result is valid" —
    // and there is no cached result. Starting clean is how a thumbnail grid comes up blank and stays that way.
    s.dirty = true;
    m_v.push_back(static_cast<ViewportState&&>(s));
    return static_cast<crd::u32>(m_v.size()) - 1U;
}

crd::i64 ViewportRegistry::find(crd::containers::StringView id) const
{
    for (crd::usize i = 0; i < m_v.size(); ++i)
    {
        if (str_eq(m_v[i].desc.id, id)) { return static_cast<crd::i64>(i); }
    }
    return -1;
}

void ViewportRegistry::depends_on(crd::u32 viewport, DependencyKind kind, crd::u64 key)
{
    if (viewport >= m_v.size()) { return; }
    const crd::u64 w = dep_word(kind, key);
    for (crd::usize i = 0; i < m_v[viewport].deps.size(); ++i)
    {
        if (m_v[viewport].deps[i] == w) { return; } // declared twice is once
    }
    m_v[viewport].deps.push_back(w);
}

crd::u32 ViewportRegistry::invalidate(DependencyKind kind, crd::u64 key)
{
    const crd::u64 w = dep_word(kind, key);
    crd::u32       n = 0;
    for (crd::usize i = 0; i < m_v.size(); ++i)
    {
        for (crd::usize d = 0; d < m_v[i].deps.size(); ++d)
        {
            if (m_v[i].deps[d] != w) { continue; }
            if (!m_v[i].dirty) { ++n; }
            m_v[i].dirty = true;
            break;
        }
    }
    return n;
}

void ViewportRegistry::invalidate_viewport(crd::u32 viewport)
{
    if (viewport < m_v.size()) { m_v[viewport].dirty = true; }
}

void ViewportRegistry::note_cost(crd::u32 viewport, double gpu_ms) noexcept
{
    if (viewport < m_v.size()) { m_v[viewport].last_gpu_ms = gpu_ms; }
}

void select_viewports(ViewportRegistry& reg, const ViewportBudget& budget, crd::u32 frame, ViewportSelection& out)
{
    out.active.clear();
    out.deferred.clear();
    out.charged_ms = 0.0;

    const auto cost_of = [&](const ViewportState& s) {
        return s.last_gpu_ms > 0.0 ? s.last_gpu_ms : budget.unmeasured_cost_ms;
    };
    const auto pixels_of = [](const ViewportState& s) {
        return static_cast<crd::u64>(s.desc.width) * static_cast<crd::u64>(s.desc.height);
    };

    crd::u64 pixels = 0;

    // ── 1. EVERY-FRAME viewports are admitted FIRST and charged. ⛔ The main viewport is never starved by
    // thumbnails; that is why this is a separate pass and not a priority value that a busy thumbnail could beat.
    for (crd::u32 i = 0; i < reg.count(); ++i)
    {
        const ViewportState& s = reg.at(i);
        if (s.desc.policy != ViewportPolicy::EveryFrame) { continue; }
        out.active.push_back(i);
        out.charged_ms += cost_of(s);
        pixels += pixels_of(s);
    }

    // ── 2. Candidates: dirty OnDemand, or Periodic whose period has elapsed. A CLEAN OnDemand viewport is not a
    // candidate at all — that is what makes a settled browser cost ZERO passes.
    crd::containers::Array<crd::u32> cand(out.active.allocator());
    for (crd::u32 i = 0; i < reg.count(); ++i)
    {
        const ViewportState& s = reg.at(i);
        if (s.desc.policy == ViewportPolicy::EveryFrame) { continue; }
        if (s.desc.policy == ViewportPolicy::OnDemand && !s.dirty) { continue; }
        if (s.desc.policy == ViewportPolicy::Periodic
            && (frame - s.last_rendered) < s.desc.period_frames && s.last_rendered != 0U)
        {
            continue;
        }
        cand.push_back(i);
    }

    // ── 3. Order by EFFECTIVE priority = declared priority + frames skipped. ⭐ The ageing term is what prevents
    // starvation: without it a steady stream of high-priority invalidations keeps one thumbnail permanently
    // unrendered, and nothing in the system would ever report that. Insertion sort — the candidate list is a
    // handful of entries, and a stable order keeps the selection reproducible frame to frame.
    for (crd::usize a = 1; a < cand.size(); ++a)
    {
        const crd::u32 v  = cand[a];
        const crd::u64 vk = static_cast<crd::u64>(reg.at(v).desc.priority) + reg.at(v).skipped;
        crd::usize     b  = a;
        while (b > 0)
        {
            const crd::u32 p  = cand[b - 1];
            const crd::u64 pk = static_cast<crd::u64>(reg.at(p).desc.priority) + reg.at(p).skipped;
            if (pk >= vk) { break; }
            cand[b] = cand[b - 1];
            --b;
        }
        cand[b] = v;
    }

    // ── 4. Admit while the budget holds; everything else is DEFERRED and reported.
    for (crd::usize a = 0; a < cand.size(); ++a)
    {
        const crd::u32       i = cand[a];
        const ViewportState& s = reg.at(i);
        const double         c = cost_of(s);
        const crd::u64       p = pixels_of(s);
        const bool           fits =
            out.active.size() < budget.max_viewports && (out.charged_ms + c) <= budget.max_gpu_ms
            && (pixels + p) <= budget.max_pixels;
        if (fits)
        {
            out.active.push_back(i);
            out.charged_ms += c;
            pixels += p;
        }
        else { out.deferred.push_back(i); }
    }
}

void commit_selection(ViewportRegistry& reg, const ViewportSelection& sel, crd::u32 frame)
{
    for (crd::usize i = 0; i < sel.active.size(); ++i)
    {
        ViewportState& s = reg.at(sel.active[i]);
        s.dirty          = false;
        s.skipped        = 0U;
        s.last_rendered  = frame;
    }
    // ⛔ Deferred viewports stay DIRTY and age. Marking them clean here would be the silent-cap failure in its
    // purest form: the thumbnail never renders and nothing ever asks again.
    for (crd::usize i = 0; i < sel.deferred.size(); ++i) { ++reg.at(sel.deferred[i]).skipped; }
}

FrameCookError compose_frame(const ViewportRegistry& reg, crd::containers::ConstSpan<crd::u32> active,
                             const crd::containers::String* shared, FrameGraphDesc& out,
                             crd::containers::String* where)
{
    auto* alloc = out.resources.allocator();
    out.schema  = kFrameSchemaVersion;
    set_str(out.name, crd::containers::StringView("@frame"));

    // The SHARED graph is included ONCE, outside any viewport. ⭐ That is what makes the shadow atlas (or the sky,
    // or the IBL) a single producer that every reader orders behind automatically — the dependency sort has
    // always known how to do this; it simply never saw two viewports in one graph before.
    if (shared != nullptr && shared->size() > 0U)
    {
        FrameIncludeDesc inc(alloc);
        copy_str(inc.graph, *shared);
        set_str(inc.as, crd::containers::StringView("shared"));
        out.includes.push_back(static_cast<FrameIncludeDesc&&>(inc));
    }

    crd::u32 presenters = 0;
    for (crd::usize i = 0; i < active.size(); ++i)
    {
        const crd::u32 vi = active[i];
        if (vi >= reg.count()) { return FrameCookError::UnresolvedInclude; }
        const ViewportDesc& d = reg.at(vi).desc;
        if (d.graph.size() == 0U || d.id.size() == 0U)
        {
            if (where != nullptr) { copy_str(*where, d.id); }
            return FrameCookError::IncludeMissingName;
        }
        // ⛔ Two viewports sharing an `as` namespace is the collision REN-37.6 exists to prevent — and here the
        // namespace IS the viewport id, so a duplicate id would silently render one viewport into another.
        for (crd::usize k = 0; k < out.includes.size(); ++k)
        {
            if (str_eq(out.includes[k].as, crd::containers::StringView(d.id.c_str(), d.id.size())))
            {
                if (where != nullptr) { copy_str(*where, d.id); }
                return FrameCookError::DuplicateInclude;
            }
        }
        if (d.present) { ++presenters; }

        FrameIncludeDesc inc(alloc);
        copy_str(inc.graph, d.graph);
        copy_str(inc.as, d.id);
        // Bind the viewport graph's `@output` to THIS viewport's own output name. The presenting viewport binds
        // to the frame's `@output`; every other viewport binds to a name of its own, so its result lands in its
        // own target and never in the swapchain.
        FrameBinding b(alloc);
        set_str(b.from, crd::containers::StringView("@output"));
        if (d.present) { set_str(b.to, crd::containers::StringView("@output")); }
        else
        {
            b.to.append("@vp.");
            b.to.append(d.id.c_str());
        }
        inc.bind.push_back(static_cast<FrameBinding&&>(b));
        out.includes.push_back(static_cast<FrameIncludeDesc&&>(inc));
    }

    // ⛔ Exactly one owner of the swapchain, or none. TWO would each blit over the other and the winner would
    // depend on declaration order — a bug that looks like flicker and points nowhere.
    if (presenters > 1U)
    {
        if (where != nullptr) { set_str(*where, crd::containers::StringView("present")); }
        return FrameCookError::DuplicateInclude;
    }
    return FrameCookError::Ok;
}

} // namespace crd::framecook
