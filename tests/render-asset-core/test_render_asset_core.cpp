// RAF-1 Gate 1 — canonical render-asset identity / namespaces / diagnostics / deps.
//
// Gates (mission §5/§15 · D-007 RAF-1): path normalization; namespace separation;
// stable-ID collision DETECTION; deterministic IDs; structured diagnostics;
// deterministic dependency ordering; NO implicit engine/app shadowing.
//
// ⛔ named allocator throughout (no hidden default malloc); ASCII-only test names.

#include <crd/renderasset/renderasset.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::usize;
using crd::containers::Array;
using crd::containers::StringView;
using namespace crd::renderasset;

namespace
{
usize index_of(const Array<AssetId>& order, AssetId id)
{
    for (usize i = 0; i < order.size(); ++i)
    {
        if (order[i] == id)
        {
            return i;
        }
    }
    return order.size(); // not found
}
} // namespace

// ── Path normalization: collapse "//", resolve "." and "..", strip trailing '/',
//    and fold '\' to '/'. Equivalent raw forms must produce ONE canonical + id. ──
TEST_CASE("raf1 path normalization is canonical")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-normalize");
    DiagnosticList diags(&alloc);

    const AssetRef base = AssetRef::parse("engine://frame/forward_csm", diags, &alloc);
    REQUIRE(base.valid());
    REQUIRE(base.canonical() == "engine://frame/forward_csm");
    REQUIRE(base.path() == "frame/forward_csm");
    REQUIRE(base.scheme() == AssetScheme::Engine);
    REQUIRE(base.type() == AssetType::FrameGraph); // inferred from the "frame" folder

    // Each of these normalizes to exactly `base`.
    const char* equivalents[] = {
        "engine://frame//forward_csm",     // doubled slash
        "engine://frame/forward_csm/",     // trailing slash
        "engine://frame/./forward_csm",    // "." segment
        "engine://tmp/../frame/forward_csm", // ".." pop
        "engine://frame\\forward_csm",     // backslash separator
        "engine://./frame/forward_csm/.",  // leading + trailing "."
    };
    for (const char* raw : equivalents)
    {
        const AssetRef r = AssetRef::parse(raw, diags, &alloc);
        REQUIRE(r.valid());
        REQUIRE(r.canonical() == base.canonical());
        REQUIRE(r.id() == base.id());
    }
    REQUIRE_FALSE(diags.has_errors());
}

// ── Namespace separation + NO shadowing: the same relative path under different
//    schemes yields DISTINCT ids; crd:// is an alias of engine:// (same id). ──
TEST_CASE("raf1 namespaces separate and crd aliases engine")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-namespace");
    DiagnosticList diags(&alloc);

    const AssetRef eng = AssetRef::parse("engine://frame/x", diags, &alloc);
    const AssetRef app = AssetRef::parse("app://frame/x", diags, &alloc);
    const AssetRef plg = AssetRef::parse("plugin://frame/x", diags, &alloc);
    const AssetRef tst = AssetRef::parse("test://frame/x", diags, &alloc);
    REQUIRE(eng.valid());
    REQUIRE(app.valid());
    REQUIRE(plg.valid());
    REQUIRE(tst.valid());

    // Four distinct namespaces -> four distinct ids (no implicit shadowing).
    REQUIRE(eng.id() != app.id());
    REQUIRE(eng.id() != plg.id());
    REQUIRE(eng.id() != tst.id());
    REQUIRE(app.id() != plg.id());
    REQUIRE(app.id() != tst.id());
    REQUIRE(plg.id() != tst.id());
    REQUIRE(app.scheme() == AssetScheme::App);

    // crd:// folds to engine:// -> identical canonical, id, and scheme.
    const AssetRef alias = AssetRef::parse("crd://frame/x", diags, &alloc);
    REQUIRE(alias.valid());
    REQUIRE(alias.scheme() == AssetScheme::Engine);
    REQUIRE(alias.canonical() == "engine://frame/x");
    REQUIRE(alias.id() == eng.id());

    REQUIRE_FALSE(diags.has_errors());
}

// ── Deterministic ids: same input -> same id (independent parses), and the free
//    asset_id_of() on a canonical string agrees with AssetRef::id(). ──
TEST_CASE("raf1 ids are deterministic")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-determinism");
    DiagnosticList diags(&alloc);

    const AssetRef a1 = AssetRef::parse("engine://material/scene", diags, &alloc);
    const AssetRef a2 = AssetRef::parse("engine://material/scene", diags, &alloc);
    REQUIRE(a1.valid());
    REQUIRE(a1.id() == a2.id());
    REQUIRE(a1.id().valid());
    REQUIRE(a1.type() == AssetType::Material);

    REQUIRE(asset_id_of("engine://material/scene") == a1.id());

    // Distinct canonical strings -> distinct ids.
    const AssetRef b = AssetRef::parse("engine://material/other", diags, &alloc);
    REQUIRE(b.id() != a1.id());
    REQUIRE_FALSE(diags.has_errors());
}

// ── Structured diagnostics: each malformed reference yields the right code +
//    context, and the ref is invalid. ──
TEST_CASE("raf1 malformed references produce structured diagnostics")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-diagnostics");

    {
        DiagnosticList d(&alloc);
        const AssetRef r = AssetRef::parse("no-scheme-separator", d, &alloc);
        REQUIRE_FALSE(r.valid());
        REQUIRE(d.contains(DiagCode::MalformedPath));
    }
    {
        DiagnosticList d(&alloc);
        const AssetRef r = AssetRef::parse("://frame/x", d, &alloc);
        REQUIRE_FALSE(r.valid());
        REQUIRE(d.contains(DiagCode::MalformedPath));
    }
    {
        DiagnosticList d(&alloc);
        const AssetRef r = AssetRef::parse("bogus://frame/x", d, &alloc);
        REQUIRE_FALSE(r.valid());
        REQUIRE(d.contains(DiagCode::UnknownScheme));
        REQUIRE(d.size() == 1);
        REQUIRE(d[0].code == DiagCode::UnknownScheme);
        REQUIRE(d[0].field == "bogus");            // the offending scheme token
        REQUIRE(d[0].severity == Severity::Error);
    }
    {
        DiagnosticList d(&alloc);
        const AssetRef r = AssetRef::parse("engine://", d, &alloc);
        REQUIRE_FALSE(r.valid());
        REQUIRE(d.contains(DiagCode::EmptyPath));
    }
    {
        DiagnosticList d(&alloc);
        const AssetRef r = AssetRef::parse("engine://a/../..", d, &alloc);
        REQUIRE_FALSE(r.valid());
        REQUIRE(d.contains(DiagCode::PathEscapesRoot));
    }
}

// ── Collision detection: idempotent re-register is fine; same id / different
//    canonical path fires IdCollision. ──
TEST_CASE("raf1 registry detects id collisions")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-registry");
    DiagnosticList diags(&alloc);
    AssetRegistry reg(&alloc);

    REQUIRE(reg.register_path("engine://frame/a", diags));
    REQUIRE(reg.register_path("engine://frame/b", diags));
    REQUIRE(reg.size() == 2);

    // Idempotent: same path again does not grow or error.
    REQUIRE(reg.register_path("engine://frame/a", diags));
    REQUIRE(reg.size() == 2);
    REQUIRE_FALSE(diags.has_errors());

    // Lookup round-trips.
    StringView found{};
    REQUIRE(reg.lookup(asset_id_of("engine://frame/a"), found));
    REQUIRE(found == "engine://frame/a");

    // Force the collision branch (real FNV clashes are unreachable by hand):
    // the same id registered under a different canonical path must be rejected.
    const AssetId forged{0xABCDEF0123456789ULL};
    REQUIRE(reg.register_id(forged, "engine://forged/one", diags));
    REQUIRE_FALSE(reg.register_id(forged, "engine://forged/two", diags));
    REQUIRE(diags.contains(DiagCode::IdCollision));
}

// ── Deterministic dependency ordering: deps precede dependents, ties by id, and
//    two graphs built in different insertion orders yield IDENTICAL orderings. ──
TEST_CASE("raf1 dependency ordering is deterministic")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-deps");
    DiagnosticList diags(&alloc);

    const AssetId a = asset_id_of("engine://frame/a");
    const AssetId b = asset_id_of("engine://frame/b");
    const AssetId c = asset_id_of("engine://frame/c");
    const AssetId d = asset_id_of("engine://frame/d");

    // DAG: a depends on b and c; b and c each depend on d. Valid order: d first,
    // then b and c (tie broken by ascending id), then a.
    DependencyGraph g1(&alloc);
    g1.add_edge(a, b);
    g1.add_edge(a, c);
    g1.add_edge(b, d);
    g1.add_edge(c, d);

    Array<AssetId> order1(&alloc);
    REQUIRE(g1.topo_order(order1, diags));
    REQUIRE(order1.size() == 4);

    // Topological correctness.
    REQUIRE(index_of(order1, d) < index_of(order1, b));
    REQUIRE(index_of(order1, d) < index_of(order1, c));
    REQUIRE(index_of(order1, b) < index_of(order1, a));
    REQUIRE(index_of(order1, c) < index_of(order1, a));

    // Tie-break: among the two ready siblings, the smaller id is emitted first.
    const AssetId lo = (b < c) ? b : c;
    const AssetId hi = (b < c) ? c : b;
    REQUIRE(index_of(order1, lo) < index_of(order1, hi));

    // Same DAG, edges added in a different order -> byte-identical ordering.
    DependencyGraph g2(&alloc);
    g2.add_edge(c, d);
    g2.add_edge(b, d);
    g2.add_edge(a, c);
    g2.add_edge(a, b);

    Array<AssetId> order2(&alloc);
    REQUIRE(g2.topo_order(order2, diags));
    REQUIRE(order2.size() == order1.size());
    for (usize i = 0; i < order1.size(); ++i)
    {
        REQUIRE(order1[i] == order2[i]);
    }
    REQUIRE_FALSE(diags.has_errors());
}

// ── RAF-11 hot-reload rebuild set: the transitive DEPENDENTS of a changed asset, deps-first, changed excluded. ──
TEST_CASE("raf11 affected_by yields the transitive dependents in rebuild order")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf11-affected");
    DiagnosticList diags(&alloc);

    // Same diamond: a (frame) depends on b and c (techniques); b and c each depend on d (a shared module).
    const AssetId a = asset_id_of("engine://frame/a");
    const AssetId b = asset_id_of("engine://frame/b");
    const AssetId c = asset_id_of("engine://frame/c");
    const AssetId d = asset_id_of("engine://frame/d");

    DependencyGraph g(&alloc);
    g.add_edge(a, b);
    g.add_edge(a, c);
    g.add_edge(b, d);
    g.add_edge(c, d);

    // Changing the shared module d must rebuild b, c, then a — b and c BEFORE a (a depends on them), d NOT included.
    Array<AssetId> hit(&alloc);
    REQUIRE(g.affected_by(d, hit, diags));
    REQUIRE(hit.size() == 3);
    REQUIRE(index_of(hit, d) == hit.size()); // the changed asset never appears in its own rebuild set
    REQUIRE(index_of(hit, b) < index_of(hit, a));
    REQUIRE(index_of(hit, c) < index_of(hit, a));
    const AssetId lo = (b < c) ? b : c; // ties broken by ascending id, same as topo_order
    const AssetId hi = (b < c) ? c : b;
    REQUIRE(index_of(hit, lo) < index_of(hit, hi));

    // Changing the top frame a affects nobody (nothing depends on it).
    Array<AssetId> none(&alloc);
    REQUIRE(g.affected_by(a, none, diags));
    REQUIRE(none.size() == 0);

    // Changing a mid technique b rebuilds only its one dependent, a.
    Array<AssetId> one(&alloc);
    REQUIRE(g.affected_by(b, one, diags));
    REQUIRE(one.size() == 1);
    REQUIRE(one[0] == a);

    // An asset the graph never heard of affects nobody (and is not an error).
    Array<AssetId> unknown(&alloc);
    REQUIRE(g.affected_by(asset_id_of("engine://frame/z"), unknown, diags));
    REQUIRE(unknown.size() == 0);

    // Determinism: the rebuild set is independent of edge-insertion order.
    DependencyGraph g2(&alloc);
    g2.add_edge(c, d);
    g2.add_edge(a, b);
    g2.add_edge(b, d);
    g2.add_edge(a, c);
    Array<AssetId> hit2(&alloc);
    REQUIRE(g2.affected_by(d, hit2, diags));
    REQUIRE(hit2.size() == hit.size());
    for (usize i = 0; i < hit.size(); ++i)
    {
        REQUIRE(hit[i] == hit2[i]);
    }
    REQUIRE_FALSE(diags.has_errors());
}

// ── A cyclic graph has no rebuild order — affected_by rejects it exactly like topo_order. ──
TEST_CASE("raf11 affected_by rejects a cyclic graph")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf11-affected-cycle");
    DiagnosticList diags(&alloc);

    const AssetId a = asset_id_of("engine://frame/a");
    const AssetId b = asset_id_of("engine://frame/b");

    DependencyGraph g(&alloc);
    g.add_edge(a, b);
    g.add_edge(b, a); // cycle

    Array<AssetId> out(&alloc);
    REQUIRE_FALSE(g.affected_by(a, out, diags));
    REQUIRE(out.size() == 0);
    REQUIRE(diags.contains(DiagCode::CyclicDependency));
}

// ── Cycle rejection. ──
TEST_CASE("raf1 dependency cycle is rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-cycle");
    DiagnosticList diags(&alloc);

    const AssetId a = asset_id_of("engine://frame/a");
    const AssetId b = asset_id_of("engine://frame/b");

    DependencyGraph g(&alloc);
    g.add_edge(a, b);
    g.add_edge(b, a); // cycle

    Array<AssetId> order(&alloc);
    REQUIRE_FALSE(g.topo_order(order, diags));
    REQUIRE(order.size() == 0);
    REQUIRE(diags.contains(DiagCode::CyclicDependency));
}

// ── Missing-dependency validation against the registry. ──
TEST_CASE("raf1 missing dependency is diagnosed")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf1-missing");
    DiagnosticList diags(&alloc);
    AssetRegistry reg(&alloc);

    const AssetId a = asset_id_of("engine://frame/a");
    const AssetId b = asset_id_of("engine://frame/b");

    DependencyGraph g(&alloc);
    g.add_edge(a, b); // a depends on b

    // Only `a` is registered -> b is missing.
    REQUIRE(reg.register_path("engine://frame/a", diags));
    REQUIRE_FALSE(g.validate_against(reg, diags));
    REQUIRE(diags.contains(DiagCode::MissingDependency));

    // Register b -> now valid.
    DiagnosticList diags2(&alloc);
    REQUIRE(reg.register_path("engine://frame/b", diags2));
    REQUIRE(g.validate_against(reg, diags2));
    REQUIRE_FALSE(diags2.contains(DiagCode::MissingDependency));
}
