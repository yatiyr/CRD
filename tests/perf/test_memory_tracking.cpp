// crd-perf v0e -- allocator registry + per-frame AllocatorRecord snapshot.

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("register_allocator returns a stable index", "[perf][memory][register]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "alpha"};
    crd::memory::TlsfAllocator b{1U << 16, nullptr, "beta"};

    const auto ai = crd::perf::register_allocator("alpha", &a);
    const auto bi = crd::perf::register_allocator("beta", &b);
    CHECK(ai != crd::perf::kInvalidAllocatorIdx);
    CHECK(bi != crd::perf::kInvalidAllocatorIdx);
    CHECK(ai != bi);
    CHECK(crd::perf::registered_allocator_count() == 2U);
}

TEST_CASE("register_allocator dedups same pointer", "[perf][memory][register]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "same"};
    const auto i1 = crd::perf::register_allocator("same", &a);
    const auto i2 = crd::perf::register_allocator("same-relabel", &a);
    CHECK(i1 == i2);
    CHECK(crd::perf::registered_allocator_count() == 1U);

    // Relabel takes effect.
    const auto info = crd::perf::allocator_info(i1);
    CHECK(std::string_view{info.name} == "same-relabel");
}

TEST_CASE("allocator_info reports name + pointer", "[perf][memory][info]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "named"};
    const auto i = crd::perf::register_allocator("named-track", &a);
    const auto info = crd::perf::allocator_info(i);
    CHECK(std::string_view{info.name} == "named-track");
    CHECK(info.allocator == &a);
}

TEST_CASE("allocator_snapshot reads live stats", "[perf][memory][snapshot]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "snap"};
    const auto i = crd::perf::register_allocator("snap-track", &a);

    void* p = a.allocate(1024U);
    REQUIRE(p != nullptr);

    const auto snap = crd::perf::allocator_snapshot(i);
    CHECK(std::string_view{snap.name} == "snap-track");
    CHECK(snap.alloc_count   >= 1U);
    CHECK(snap.bytes_in_use  >= 1024U);
    CHECK(snap.peak_bytes    >= 1024U);
    CHECK(snap.total_bytes   >= 1024U);

    a.deallocate(p);
    const auto snap2 = crd::perf::allocator_snapshot(i);
    CHECK(snap2.dealloc_count == snap.dealloc_count + 1U);
    CHECK(snap2.bytes_in_use < snap.bytes_in_use);
    CHECK(snap2.peak_bytes  == snap.peak_bytes);
}

TEST_CASE("frame_mark stamps allocator stats into the FrameRecord history",
          "[perf][memory][frame]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "framed"};
    const auto i = crd::perf::register_allocator("framed-alloc", &a);

    void* p = a.allocate(2048U);
    REQUIRE(p != nullptr);

    CRD_PERF_FRAME_MARK();

    const auto* rec = crd::perf::frame_record(0U);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->allocator_count >= 1U);
    const auto& ar = rec->allocators[i];
    CHECK(ar.alloc_count  >= 1U);
    CHECK(ar.bytes_in_use >= 2048U);
    CHECK(ar.peak_bytes   >= 2048U);

    // Historical via API.
    const auto histsnap = crd::perf::allocator_snapshot_history(i, 0U);
    CHECK(std::string_view{histsnap.name} == "framed-alloc");
    CHECK(histsnap.alloc_count == ar.alloc_count);
    CHECK(histsnap.bytes_in_use == ar.bytes_in_use);

    a.deallocate(p);
}

TEST_CASE("unregister_allocator zeros the slot but keeps the index stable",
          "[perf][memory][unregister]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "transient"};
    const auto i = crd::perf::register_allocator("transient", &a);
    CHECK(crd::perf::registered_allocator_count() == 1U);

    crd::perf::unregister_allocator(i);
    // High-water is preserved (UI sees stable indexing); the slot is empty.
    CHECK(crd::perf::registered_allocator_count() == 1U);
    const auto info = crd::perf::allocator_info(i);
    CHECK(info.allocator == nullptr);
    CHECK(std::string_view{info.name} == "");
}

TEST_CASE("register after unregister reuses the cleared slot",
          "[perf][memory][unregister]")
{
    PerfFixture fx;
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "first"};
    crd::memory::TlsfAllocator b{1U << 16, nullptr, "second"};

    const auto i1 = crd::perf::register_allocator("first", &a);
    crd::perf::unregister_allocator(i1);
    const auto i2 = crd::perf::register_allocator("second", &b);
    CHECK(i2 == i1); // reuse the cleared slot
}

TEST_CASE("allocator_snapshot on invalid index returns zeros, no crash",
          "[perf][memory][robustness]")
{
    PerfFixture fx;
    const auto snap = crd::perf::allocator_snapshot(99U);
    CHECK(snap.alloc_count == 0U);
    CHECK(std::string_view{snap.name} == "");
}

TEST_CASE("register_allocator on inactive profiler returns invalid",
          "[perf][memory][register]")
{
    crd::memory::TlsfAllocator a{1U << 16, nullptr, "off"};
    CHECK_FALSE(crd::perf::is_active());
    const auto i = crd::perf::register_allocator("off", &a);
    CHECK(i == crd::perf::kInvalidAllocatorIdx);
}

#endif // CRD_PERF_ENABLED
