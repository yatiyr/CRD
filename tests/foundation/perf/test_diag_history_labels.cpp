// DIAG.6a(d2): allocator history labels survive slot reuse. `AllocatorMeta` / `allocator_info(slot)` resolve a name
// from the LIVE slot, so a frame recorded before an unregister+reuse would show the NEW occupant's name (the
// acceptance's "mislabelled history"). (d2) stamps the allocator's interned NameId into `AllocatorRecord::_pad`
// (name_id + 1; 0 = unset) at each frame_mark, and `CaptureView::frame_allocator_name` prefers it -- so an older frame
// keeps the name the slot held THEN. Self-describing in the CPROF name blob; no format change (the 48 B pin holds, and
// pre-(d2) captures with _pad == 0 fall back to the live slot name). Proof lanes: win-debug + win-asan.
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};
} // namespace

TEST_CASE("history labels: an allocator record keeps its name after the slot is unregistered and reused",
          "[perf][diag][history]")
{
    crd::memory::GrowableTlsfAllocator cap_alloc{64ULL << 20, nullptr, "d2-capture"};
    PerfFixture                        fx;

    // Two distinct allocator instances (allocators do NOT auto-register), so the slot can be freed then reused.
    crd::memory::GrowableTlsfAllocator a{1ULL << 20, nullptr, "a-inst"};
    crd::memory::GrowableTlsfAllocator b{1ULL << 20, nullptr, "b-inst"};

    const crd::u32 slot = crd::perf::register_allocator("alloc.first", &a);
    REQUIRE(slot < crd::perf::kMaxAllocators);
    crd::perf::frame_mark(); // frame N: slot holds "alloc.first"

    crd::perf::unregister_allocator(slot);
    const crd::u32 slot2 = crd::perf::register_allocator("alloc.second", &b);
    REQUIRE(slot2 == slot); // the freed slot was reused by a different allocator
    crd::perf::frame_mark(); // frame N+1: same slot now holds "alloc.second"

    const auto buf = crd::perf::save_capture_to_buffer(&cap_alloc);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    REQUIRE(view.frame_record_count() >= 2U);

    // Frames are written oldest-first, so [0] is the "alloc.first" frame and [last] the "alloc.second" frame.
    const crd::u32 oldest = 0U;
    const crd::u32 newest = view.frame_record_count() - 1U;
    // The fix: the older frame keeps "alloc.first". Without the per-record stamp both frames would resolve to the
    // live-at-save name "alloc.second" (the mislabel).
    CHECK(std::strcmp(view.frame_allocator_name(oldest, slot), "alloc.first") == 0);
    CHECK(std::strcmp(view.frame_allocator_name(newest, slot), "alloc.second") == 0);
}

TEST_CASE("history labels: a record with no stamped identity falls back to the live slot name",
          "[perf][diag][history]")
{
    // Saturate the NAME TABLE (not the arena) so the allocator's name cannot intern -> name_id invalid -> _pad stays 0,
    // exercising the legacy/fallback path while the AllocatorMeta name (owned in the still-roomy arena) stays correct.
    crd::perf::InitConfig cfg{};
    cfg.max_region_names = 2U;
    crd::perf::init(cfg);

    crd::memory::GrowableTlsfAllocator cap_alloc{64ULL << 20, nullptr, "d2-fallback-cap"};
    char                              nm[32];
    for (int i = 0; i < 8; ++i) // fill the tiny name table so later interns fail
    {
        std::snprintf(nm, sizeof(nm), "fill.%d", i);
        (void)crd::perf::intern_name(nm);
    }
    REQUIRE(crd::perf::name_bytes_dropped_count() > 0U); // the table is saturated

    crd::memory::GrowableTlsfAllocator a{1ULL << 20, nullptr, "fb-inst"};
    const crd::u32                     slot = crd::perf::register_allocator("alloc.fallback", &a);
    REQUIRE(slot < crd::perf::kMaxAllocators);
    crd::perf::frame_mark();

    const auto buf = crd::perf::save_capture_to_buffer(&cap_alloc);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    REQUIRE(view.frame_record_count() >= 1U);

    // The record's identity is unset (table was saturated at register time), so the reader falls back to the live-slot
    // name -- which is still correct for a slot that was never reused.
    const auto frames = view.frame_records();
    CHECK(frames[0].allocators[slot]._pad == 0U); // no stamp -> fallback path is the one under test
    // The fallback source is the live-at-save AllocatorMeta name (not the stamped path); pin that explicitly.
    CHECK(std::strcmp(view.allocator_info(slot).name, "alloc.fallback") == 0);
    CHECK(std::strcmp(view.frame_allocator_name(0U, slot), "alloc.fallback") == 0);

    crd::perf::shutdown();
}

TEST_CASE("history labels: out-of-range frame/slot returns empty", "[perf][diag][history]")
{
    crd::memory::GrowableTlsfAllocator cap_alloc{64ULL << 20, nullptr, "d2-range-cap"};
    PerfFixture                        fx;
    crd::perf::frame_mark();
    const auto buf = crd::perf::save_capture_to_buffer(&cap_alloc);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    CHECK(std::strcmp(view.frame_allocator_name(9999U, 0U), "") == 0);
    CHECK(std::strcmp(view.frame_allocator_name(0U, crd::perf::kMaxAllocators), "") == 0);
}

#endif // CRD_PERF_ENABLED
