// CEIR-8h (ADR-0118) — crd::containers::IncrementalDag, the ONE dependency/dirty engine (render-asset-core's
// DependencyGraph is now a thin wrapper over it). Determinism (deps-first, ascending-id tie-break); affected_by
// transitive dependents in topo order; cycle => false; and ⛔ THE headline: the §107 rule — recompute_after_change
// propagates to dependents on an INTERFACE change but NOT on a content-only change (the hot-swap). ASCII names.

#include <crd/containers/incremental_dag.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::containers::Array;
using crd::containers::IncrementalDag;
using crd::u64;
using crd::usize;

namespace
{
// Fixed fixture: 1 <- 2, 1 <- 3, 2 <- 4, 3 <- 4  (edges are "from depends on to"). So deps: 2:[1] 3:[1] 4:[2,3].
IncrementalDag diamond(crd::memory::IAllocator* a)
{
    IncrementalDag dag(a);
    dag.add_edge(2U, 1U);
    dag.add_edge(3U, 1U);
    dag.add_edge(4U, 2U);
    dag.add_edge(4U, 3U);
    return dag;
}
[[nodiscard]] bool eq(const Array<u64>& v, const u64* expect, usize n) noexcept
{
    if (v.size() != n) { return false; }
    for (usize i = 0; i < n; ++i)
    {
        if (v[i] != expect[i]) { return false; }
    }
    return true;
}
} // namespace

TEST_CASE("incremental dag: topo order is deterministic (deps-first, ascending-id tie-break)", "[containers][incremental]")
{
    crd::memory::MallocAllocator root;
    IncrementalDag               dag = diamond(&root);
    Array<u64>                   order(&root);
    REQUIRE(dag.topo_order(order));
    const u64 expect[4] = {1U, 2U, 3U, 4U}; // 1 first; 2 before 3 (min-id tie-break); 4 last
    CHECK(eq(order, expect, 4U));
}

TEST_CASE("incremental dag: affected_by yields the transitive dependents in topo order", "[containers][incremental]")
{
    crd::memory::MallocAllocator root;
    IncrementalDag               dag = diamond(&root);
    Array<u64>                   out(&root);

    REQUIRE(dag.affected_by(1U, out));
    const u64 dep1[3] = {2U, 3U, 4U}; // everything depends (directly or via 2/3) on 1
    CHECK(eq(out, dep1, 3U));
    REQUIRE(dag.affected_by(2U, out));
    const u64 dep2[1] = {4U}; // only 4 depends on 2
    CHECK(eq(out, dep2, 1U));
    REQUIRE(dag.affected_by(4U, out));
    CHECK(out.size() == 0U); // nothing depends on 4
    REQUIRE(dag.affected_by(0U, out)); // invalid id -> true + empty
    CHECK(out.size() == 0U);
}

TEST_CASE("incremental dag: a cycle makes topo_order and affected_by return false", "[containers][incremental]")
{
    crd::memory::MallocAllocator root;
    IncrementalDag               dag(&root);
    dag.add_edge(1U, 2U);
    dag.add_edge(2U, 1U); // 1 <-> 2 cycle
    Array<u64>                   out(&root);
    CHECK_FALSE(dag.topo_order(out));
    CHECK_FALSE(dag.affected_by(1U, out));
}

TEST_CASE("incremental dag: the SEC-107 rule - interface change propagates, content-only does NOT", "[containers][incremental]")
{
    crd::memory::MallocAllocator root;
    IncrementalDag               dag = diamond(&root);
    // seed every node with a (content, interface) revision.
    for (u64 id = 1U; id <= 4U; ++id) { dag.set_revision(id, /*content*/ 100U + id, /*interface*/ 200U + id); }
    CHECK(dag.content_of(1U) == 101U);
    CHECK(dag.interface_of(1U) == 201U);

    Array<u64> out(&root);
    // CONTENT-ONLY change of node 1 (interface unchanged) -> only node 1 recomputes; dependents HOT-SWAP (stay valid).
    REQUIRE(dag.recompute_after_change(1U, /*content*/ 999U, /*interface*/ 201U, out));
    const u64 self_only[1] = {1U};
    CHECK(eq(out, self_only, 1U));

    // INTERFACE change of node 1 -> node 1 AND its transitive dependents (2,3,4) recompute, topo-ordered.
    REQUIRE(dag.recompute_after_change(1U, /*content*/ 1000U, /*interface*/ 999U, out));
    const u64 all4[4] = {1U, 2U, 3U, 4U};
    CHECK(eq(out, all4, 4U));

    // NO change (same content + interface as the last call) -> nothing recomputes.
    REQUIRE(dag.recompute_after_change(1U, /*content*/ 1000U, /*interface*/ 999U, out));
    CHECK(out.size() == 0U);

    // ⛔ INTERFACE-only change (content unchanged) still recomputes SELF (conservative — an interface change without a
    // content change is incoherent under content-addressing) AND its dependents.
    REQUIRE(dag.recompute_after_change(1U, /*content*/ 1000U /*same*/, /*interface*/ 555U /*new*/, out));
    CHECK(eq(out, all4, 4U));
}

TEST_CASE("incremental dag: topo order is independent of insertion order", "[containers][incremental]")
{
    crd::memory::MallocAllocator root;
    IncrementalDag               dag(&root);
    // the SAME diamond, edges added in REVERSE order — the emitted order must be IDENTICAL (the determinism contract
    // that makes RAF-11 rebuilds reproducible "regardless of insertion order").
    dag.add_edge(4U, 3U);
    dag.add_edge(4U, 2U);
    dag.add_edge(3U, 1U);
    dag.add_edge(2U, 1U);
    Array<u64> order(&root);
    REQUIRE(dag.topo_order(order));
    const u64 expect[4] = {1U, 2U, 3U, 4U};
    CHECK(eq(order, expect, 4U));
}
