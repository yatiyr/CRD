// frame_compose.cpp — REN-37.6: SUBGRAPHS + INJECTION POINTS, flattened away before `build()`.
//
// One function, `flatten_frame_graph`, turns a composed graph into an ordinary one:
//   · every `[[include]]` is inlined with its names prefixed by `<as>.` (or rewritten by `bind`), recursively;
//   · every `[[inject]]` splices its pass at a DECLARED anchor.
// After it runs, nothing downstream — the validator, the executor, lifetime analysis, barriers, aliasing, the
// dependency sort — knows composition exists. That is the same decision REN-36.3 made for `for_each`, for the
// same reason: a composition feature that reached the scheduler would need a special case in every one of those.
//
// ⛔ WHY NAMESPACING IS LOAD-BEARING, not cosmetic. Two instances of the same graph (two viewports, two blur
// chains) would otherwise both declare `hdr` and both write it. That is not an error in any validator — it just
// renders one instance's output into the other's buffer, and the picture looks *almost* right.

#include <crd/framecook/frame_asset.hpp>

#include <cstring>

namespace crd::framecook
{
namespace
{

void set_str(crd::containers::String& dst, crd::containers::StringView s)
{
    dst.clear();
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char one[2] = {s[i], '\0'};
        dst.append(static_cast<const char*>(one));
    }
}
void copy_str(crd::containers::String& dst, const crd::containers::String& s)
{
    dst.clear();
    dst.append(s.c_str());
}
[[nodiscard]] bool str_eq(const crd::containers::String& a, const crd::containers::String& b)
{
    return a.size() == b.size() && std::memcmp(a.c_str(), b.c_str(), a.size()) == 0;
}
void set_where(crd::containers::String* where, const crd::containers::String& s)
{
    if (where != nullptr) { copy_str(*where, s); }
}

// Rewrite one name from an included graph into the includer's scope. A `bind` entry wins; otherwise the name is
// namespaced. ⛔ `@output` and any other `@`-prefixed SENTINEL is left ALONE when unbound: those are resolved by
// the host, not by the graph, and namespacing one would produce `vp.main.@output`, which nothing resolves.
void rewrite(const crd::containers::String& src, const FrameIncludeDesc& inc, crd::containers::String& dst)
{
    for (crd::usize i = 0; i < inc.bind.size(); ++i)
    {
        if (str_eq(inc.bind[i].from, src))
        {
            copy_str(dst, inc.bind[i].to);
            return;
        }
    }
    if (src.size() > 0U && src.c_str()[0] == '@')
    {
        copy_str(dst, src);
        return;
    }
    dst.clear();
    dst.append(inc.as.c_str());
    dst.append(".");
    dst.append(src.c_str());
}

void copy_resource(const FrameResourceDesc& s, FrameResourceDesc& d)
{
    d.kind       = s.kind;
    d.format     = s.format;
    d.width      = s.width;
    d.height     = s.height;
    d.scale      = s.scale;
    d.layers     = s.layers;
    d.samples    = s.samples;
    d.sampled    = s.sampled;
    d.storage    = s.storage;
    d.size_bytes = s.size_bytes;
}

void copy_pass_body(const FramePassDesc& s, FramePassDesc& d)
{
    d.kind            = s.kind;
    d.material_pass   = s.material_pass;
    d.for_each        = s.for_each;
    d.for_each_arg    = s.for_each_arg;
    d.has_clear_color = s.has_clear_color;
    for (crd::u32 i = 0; i < 4U; ++i) { d.clear_color[i] = s.clear_color[i]; }
    d.has_clear_depth = s.has_clear_depth;
    d.clear_depth     = s.clear_depth;
    d.depth           = s.depth;
    copy_str(d.shader, s.shader);
    copy_str(d.kernel, s.kernel);
    copy_str(d.technique, s.technique);
    copy_str(d.view, s.view);
    for (crd::usize i = 0; i < s.params.size(); ++i)
    {
        FrameParam p(d.params.allocator());
        copy_str(p.name, s.params[i].name);
        p.type = s.params[i].type;
        for (crd::u32 c = 0; c < 4U; ++c) { p.v[c] = s.params[i].v[c]; }
        d.params.push_back(static_cast<FrameParam&&>(p));
    }
}

// Append `src`'s resources/draw-lists/passes into `out`, rewriting every name through `inc` (null ⇒ verbatim).
void append_graph(const FrameGraphDesc& src, const FrameIncludeDesc* inc, FrameGraphDesc& out)
{
    auto*      alloc = out.resources.allocator();
    const auto name_of = [&](const crd::containers::String& s, crd::containers::String& d) {
        if (inc == nullptr) { copy_str(d, s); }
        else { rewrite(s, *inc, d); }
    };

    for (crd::usize i = 0; i < src.resources.size(); ++i)
    {
        FrameResourceDesc r(alloc);
        name_of(src.resources[i].name, r.name);
        copy_resource(src.resources[i], r);
        out.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    }
    for (crd::usize i = 0; i < src.draw_lists.size(); ++i)
    {
        FrameDrawListDesc d(alloc);
        name_of(src.draw_lists[i].name, d.name);
        const auto cp = [&](const crd::containers::Array<crd::containers::String>& s,
                            crd::containers::Array<crd::containers::String>&       t) {
            for (crd::usize k = 0; k < s.size(); ++k)
            {
                crd::containers::String v(alloc);
                copy_str(v, s[k]); // component names are ECS-global — never namespaced
                t.push_back(static_cast<crd::containers::String&&>(v));
            }
        };
        cp(src.draw_lists[i].all, d.all);
        cp(src.draw_lists[i].any, d.any);
        cp(src.draw_lists[i].none, d.none);
        d.cull  = src.draw_lists[i].cull;
        d.sort  = src.draw_lists[i].sort;
        d.limit = src.draw_lists[i].limit;
        out.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
    }
    for (crd::usize i = 0; i < src.passes.size(); ++i)
    {
        const FramePassDesc& sp = src.passes[i];
        FramePassDesc        p(alloc);
        name_of(sp.name, p.name);
        copy_pass_body(sp, p);
        if (sp.draw_list.size() > 0U) { name_of(sp.draw_list, p.draw_list); }
        const auto cp_refs = [&](const crd::containers::Array<FrameResourceRef>& s,
                                 crd::containers::Array<FrameResourceRef>&       t) {
            for (crd::usize k = 0; k < s.size(); ++k)
            {
                FrameResourceRef r(alloc);
                name_of(s[k].name, r.name);
                r.indexed = s[k].indexed;
                t.push_back(static_cast<FrameResourceRef&&>(r));
            }
        };
        cp_refs(sp.reads, p.reads);
        cp_refs(sp.writes, p.writes);
        out.passes.push_back(static_cast<FramePassDesc&&>(p));
    }
    // Anchors come along namespaced too, so an include can expose its own extension points
    // (`vp.main.after_opaque`) — that is what lets a viewport instance be extended independently of its siblings.
    for (crd::usize i = 0; i < src.anchors.size(); ++i)
    {
        FrameAnchorDesc a(alloc);
        name_of(src.anchors[i].name, a.name);
        const auto cp = [&](const crd::containers::Array<crd::containers::String>& s,
                            crd::containers::Array<crd::containers::String>&       t) {
            for (crd::usize k = 0; k < s.size(); ++k)
            {
                crd::containers::String v(alloc);
                name_of(s[k], v);
                t.push_back(static_cast<crd::containers::String&&>(v));
            }
        };
        cp(src.anchors[i].after, a.after);
        cp(src.anchors[i].before, a.before);
        out.anchors.push_back(static_cast<FrameAnchorDesc&&>(a));
    }
}

// Recursive expansion with a depth bound. The bound doubles as the cycle detector: a graph including itself (or a
// ring of them) hits it, and the alternative — an explicit visited-set — costs more than it buys for a structure
// that is never legitimately more than a handful deep.
constexpr crd::u32 kMaxIncludeDepth = 8U;

FrameCookError expand(const FrameGraphDesc& src, const FrameIncludeDesc* inc, FrameGraphResolveFn resolve,
                      void* user, crd::u32 depth, FrameGraphDesc& out, crd::containers::String* where)
{
    if (depth > kMaxIncludeDepth) { return FrameCookError::IncludeCycle; }
    append_graph(src, inc, out);

    for (crd::usize i = 0; i < src.includes.size(); ++i)
    {
        const FrameIncludeDesc& ci = src.includes[i];
        if (ci.graph.size() == 0U || ci.as.size() == 0U) { return FrameCookError::IncludeMissingName; }
        const FrameGraphDesc* sub =
            resolve(crd::containers::StringView(ci.graph.c_str(), ci.graph.size()), user);
        if (sub == nullptr)
        {
            set_where(where, ci.graph);
            return FrameCookError::UnresolvedInclude;
        }
        // A nested include is namespaced under BOTH levels, so `vp.main` including `bloom` yields
        // `vp.main.bloom.*` — two viewports each running a bloom chain stay disjoint all the way down.
        FrameIncludeDesc eff(out.resources.allocator());
        copy_str(eff.graph, ci.graph);
        if (inc == nullptr) { copy_str(eff.as, ci.as); }
        else
        {
            eff.as.append(inc->as.c_str());
            eff.as.append(".");
            eff.as.append(ci.as.c_str());
        }
        eff.atomic = ci.atomic;
        for (crd::usize b = 0; b < ci.bind.size(); ++b)
        {
            FrameBinding fb(out.resources.allocator());
            copy_str(fb.from, ci.bind[b].from);
            // the TARGET of a bind is spelled in the INCLUDER's scope, so it needs the includer's rewrite
            if (inc == nullptr) { copy_str(fb.to, ci.bind[b].to); }
            else { rewrite(ci.bind[b].to, *inc, fb.to); }
            eff.bind.push_back(static_cast<FrameBinding&&>(fb));
        }
        const FrameCookError e = expand(*sub, &eff, resolve, user, depth + 1U, out, where);
        if (e != FrameCookError::Ok) { return e; }
    }
    return FrameCookError::Ok;
}

} // namespace

FrameCookError flatten_frame_graph(const FrameGraphDesc& desc, FrameGraphResolveFn resolve, void* user,
                                   FrameGraphDesc& out, crd::containers::String* where)
{
    if (resolve == nullptr) { return FrameCookError::UnresolvedInclude; }
    out.schema = desc.schema;
    copy_str(out.name, desc.name);
    copy_str(out.fallback, desc.fallback);
    for (crd::usize i = 0; i < desc.requires_caps.size(); ++i)
    {
        crd::containers::String c(out.resources.allocator());
        copy_str(c, desc.requires_caps[i]);
        out.requires_caps.push_back(static_cast<crd::containers::String&&>(c));
    }

    const FrameCookError e = expand(desc, nullptr, resolve, user, 0U, out, where);
    if (e != FrameCookError::Ok) { return e; }

    // ── INJECTION. The pass is already in `out` (it is declared in this asset); what an anchor decides is WHERE
    // in the declaration order it sits. Declaration order is only a tie-break — the dependency sort still runs
    // afterwards and still has the final word — but it is what makes "insert a blur BETWEEN these two nodes"
    // predictable when the passes are otherwise independent.
    //
    // ⛔ An anchor nobody declared is `UnknownAnchor`; a pass this asset does not declare is `InjectUnknownPass`.
    // Both are the difference between an extension point and a monkey-patch.
    for (crd::usize i = 0; i < desc.injects.size(); ++i)
    {
        const FrameInjectDesc& inj = desc.injects[i];
        const FrameAnchorDesc* anchor = nullptr;
        for (crd::usize a = 0; a < out.anchors.size(); ++a)
        {
            if (str_eq(out.anchors[a].name, inj.anchor)) { anchor = &out.anchors[a]; break; }
        }
        if (anchor == nullptr)
        {
            set_where(where, inj.anchor);
            return FrameCookError::UnknownAnchor;
        }
        crd::i64 pass_idx = -1;
        for (crd::usize p = 0; p < out.passes.size(); ++p)
        {
            if (str_eq(out.passes[p].name, inj.pass)) { pass_idx = static_cast<crd::i64>(p); break; }
        }
        if (pass_idx < 0)
        {
            set_where(where, inj.pass);
            return FrameCookError::InjectUnknownPass;
        }
        // Target slot = just after the LAST pass the anchor sits `after`. `before` is advisory here because the
        // dependency sort owns real ordering; what this guarantees is a deterministic, author-visible position.
        crd::i64 slot = 0;
        for (crd::usize k = 0; k < anchor->after.size(); ++k)
        {
            for (crd::usize p = 0; p < out.passes.size(); ++p)
            {
                if (str_eq(out.passes[p].name, anchor->after[k]) && static_cast<crd::i64>(p) + 1 > slot)
                {
                    slot = static_cast<crd::i64>(p) + 1;
                }
            }
        }
        if (slot > static_cast<crd::i64>(out.passes.size())) { slot = static_cast<crd::i64>(out.passes.size()); }
        // Move the pass into the slot by rotation. ⛔ Rotate rather than erase+insert: `FramePassDesc` owns
        // allocator-backed strings and arrays, and a copy through a temporary would be both a needless allocation
        // and a chance to drop a field the next time one is added.
        if (pass_idx > slot)
        {
            for (crd::i64 p = pass_idx; p > slot; --p)
            {
                auto& a1 = out.passes[static_cast<crd::usize>(p)];
                auto& b1 = out.passes[static_cast<crd::usize>(p - 1)];
                FramePassDesc tmp(static_cast<FramePassDesc&&>(a1));
                a1 = static_cast<FramePassDesc&&>(b1);
                b1 = static_cast<FramePassDesc&&>(tmp);
            }
        }
        else if (pass_idx < slot - 1)
        {
            for (crd::i64 p = pass_idx; p < slot - 1; ++p)
            {
                auto& a1 = out.passes[static_cast<crd::usize>(p)];
                auto& b1 = out.passes[static_cast<crd::usize>(p + 1)];
                FramePassDesc tmp(static_cast<FramePassDesc&&>(a1));
                a1 = static_cast<FramePassDesc&&>(b1);
                b1 = static_cast<FramePassDesc&&>(tmp);
            }
        }
    }

    // The flattened graph faces the SAME 19+ rejections a hand-authored one does. A composed graph must not get a
    // weaker contract than a typed one, or composition becomes the unsafe path.
    return validate_frame_graph(out, where);
}

} // namespace crd::framecook
