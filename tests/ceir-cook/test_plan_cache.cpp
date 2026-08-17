// CEIR-10b — the execution-plan cache (PlanCache): a content-addressed store keyed by (content hash × target × compiler
// version) that VALIDATES on hit against live truth (each recorded dep's current interface hash via a resolver), so the
// cache never trusts itself ("caches, never truth"). These cases use a MOCK resolver (pure cache mechanics); the
// ReloadSet PAIRING rehearsal (the 10z inheritor) lives in test_hot_reload.cpp where the cook helpers already are.
// Host-only. ASCII test names.

#include <crd/ceir/cook/plan_cache.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)  -- PlanCache / PlanKey / PlanDep / AssetId
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::u64;
using crd::u8;
using crd::usize;

namespace
{
// A mock interface resolver: asset id → interface hash (0 = unknown / gone).
struct MockIfaces
{
    crd::containers::Array<u64> ids;
    crd::containers::Array<u64> ifaces;
    explicit MockIfaces(crd::memory::IAllocator* a) : ids(a), ifaces(a) {}
    void set(AssetId a, u64 iface)
    {
        for (usize i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == a.value) { ifaces[i] = iface; return; }
        }
        ids.push_back(a.value);
        ifaces.push_back(iface);
    }
    void erase(AssetId a) { set(a, 0U); }
    [[nodiscard]] u64 get(AssetId a) const
    {
        for (usize i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == a.value) { return ifaces[i]; }
        }
        return 0U;
    }
};
u64 mock_resolve(AssetId a, void* user) { return static_cast<const MockIfaces*>(user)->get(a); }
} // namespace

TEST_CASE("ceir 10b: put then get is a HIT; unknown key / version bump / new target all MISS", "[ceir][plancache]")
{
    crd::memory::GrowableTlsfAllocator root;
    MockIfaces                   ifaces(&root);
    PlanCache                    cache(&root, &mock_resolve, &ifaces);
    const u8      plan[3] = {1U, 2U, 3U};
    const PlanKey k{/*content*/ 100U, plan_target(StringView("host")), /*ver*/ 1U};
    cache.put(AssetId{10U}, k, ConstSpan<u8>(plan, 3U), ConstSpan<PlanDep>());

    const PlanLookup h = cache.get(k);
    REQUIRE(h.hit());
    REQUIRE(h.size == 3U);
    CHECK(h.artifact[0] == 1U);
    CHECK(h.artifact[2] == 3U);
    CHECK(cache.hits() == 1U);

    CHECK(cache.get(PlanKey{200U, plan_target(StringView("host")), 1U}).status == PlanStatus::Miss); // unknown content
    CHECK(cache.get(PlanKey{100U, plan_target(StringView("host")), 2U}).status == PlanStatus::Miss); // version bump
    CHECK(cache.get(PlanKey{100U, plan_target(StringView("gpu")), 1U}).status == PlanStatus::Miss);  // different target
    CHECK(cache.misses() == 3U);

    // a second target coexists for the same content (two entries).
    const PlanKey kt{100U, plan_target(StringView("gpu")), 1U};
    cache.put(AssetId{10U}, kt, ConstSpan<u8>(plan, 3U), ConstSpan<PlanDep>());
    CHECK(cache.get(k).hit());
    CHECK(cache.get(kt).hit());
    CHECK(cache.size() == 2U);
}

TEST_CASE("ceir 10b: a callee body-edit keeps the caller plan a HIT; an interface change is StaleDeps", "[ceir][plancache]")
{
    crd::memory::GrowableTlsfAllocator root;
    MockIfaces                   ifaces(&root);
    const AssetId                a{1U};
    const AssetId                b{2U};
    ifaces.set(b, 0xBEEFU); // B's interface at A's compile time
    PlanCache cache(&root, &mock_resolve, &ifaces);

    const u8      plan_a[2] = {9U, 9U};
    const PlanDep deps[1]   = {{b, 0xBEEFU}}; // A's plan depends on B at interface 0xBEEF
    const PlanKey ka{/*A content*/ 111U, plan_target(StringView("host")), 1U};
    cache.put(a, ka, ConstSpan<u8>(plan_a, 2U), ConstSpan<PlanDep>(deps, 1U));

    // a BODY-only edit of B: B's interface is UNCHANGED (still 0xBEEF) → A's plan STILL VALIDATES (the §107 payoff).
    CHECK(cache.get(ka).hit());

    // B's INTERFACE changes → A's plan is stale (and the stale entry is dropped → a re-get is a plain Miss).
    ifaces.set(b, 0xF00DU);
    CHECK(cache.get(ka).status == PlanStatus::StaleDeps);
    CHECK(cache.get(ka).status == PlanStatus::Miss);

    // B REMOVED (resolver returns 0) → StaleDeps (EMPTY≠UNKNOWN — 0 is never valid).
    const PlanDep deps2[1] = {{b, 0xF00DU}}; // re-record at B's current interface
    cache.put(a, ka, ConstSpan<u8>(plan_a, 2U), ConstSpan<PlanDep>(deps2, 1U));
    CHECK(cache.get(ka).hit());
    ifaces.erase(b);
    CHECK(cache.get(ka).status == PlanStatus::StaleDeps);
}

TEST_CASE("ceir 10b: clear() and evict() drop entries - the cache never trusts itself", "[ceir][plancache]")
{
    crd::memory::GrowableTlsfAllocator root;
    MockIfaces                   ifaces(&root);
    PlanCache                    cache(&root, &mock_resolve, &ifaces);
    const u8      plan[1] = {7U};
    const PlanKey k{100U, plan_target(StringView("host")), 1U};
    cache.put(AssetId{5U}, k, ConstSpan<u8>(plan, 1U), ConstSpan<PlanDep>());
    REQUIRE(cache.get(k).hit());

    cache.clear(); // all-miss → recompute → re-hit is the "never truth" property
    CHECK(cache.size() == 0U);
    CHECK(cache.get(k).status == PlanStatus::Miss);
    cache.put(AssetId{5U}, k, ConstSpan<u8>(plan, 1U), ConstSpan<PlanDep>());
    CHECK(cache.get(k).hit());

    cache.evict(AssetId{5U}); // provenance-keyed drop
    CHECK(cache.get(k).status == PlanStatus::Miss);
}
