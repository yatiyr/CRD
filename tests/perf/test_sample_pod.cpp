// crd-perf v0a -- Sample POD layout + ABI guarantees.
//
// The on-disk capture format (v0f) memcpy's Sample arrays verbatim. The
// shape must stay locked: 32 bytes, 8-byte aligned, every field at a
// known offset. Any change here bumps the CPROF version.

#include <crd/perf/sample.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <type_traits>

namespace
{

using crd::perf::Category;
using crd::perf::NameId;
using crd::perf::Sample;
using crd::perf::kInvalidNameId;

} // namespace

TEST_CASE("Sample is exactly 32 bytes, 8-aligned, trivially-copyable", "[perf][sample]")
{
    STATIC_REQUIRE(sizeof(Sample) == 32);
    STATIC_REQUIRE(alignof(Sample) == 8);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Sample>);
    STATIC_REQUIRE(std::is_standard_layout_v<Sample>);
}

TEST_CASE("Sample field offsets are pinned (on-disk format depends on these)", "[perf][sample][layout]")
{
    // 8B + 8B = 16B header.
    CHECK(offsetof(Sample, begin_ns)     == 0);
    CHECK(offsetof(Sample, end_ns)       == 8);
    // 4B + 4B = 8B identity.
    CHECK(offsetof(Sample, name_id)      == 16);
    CHECK(offsetof(Sample, color_rgba)   == 20);
    // 1+1+1+1 = 4B thread-and-depth packed.
    CHECK(offsetof(Sample, begin_thread) == 24);
    CHECK(offsetof(Sample, end_thread)   == 25);
    CHECK(offsetof(Sample, depth)        == 26);
    CHECK(offsetof(Sample, category)     == 27);
    // 4B fiber id closing the 32-byte payload.
    CHECK(offsetof(Sample, fiber_id)     == 28);
}

TEST_CASE("NameId default-constructs to the invalid sentinel", "[perf][sample][nameid]")
{
    NameId id{};
    CHECK_FALSE(id.is_valid());
    CHECK(id.value == kInvalidNameId.value);
}

TEST_CASE("Category enum fits in u8", "[perf][sample]")
{
    STATIC_REQUIRE(sizeof(Category) == 1);
    CHECK(static_cast<int>(Category::User) == 0);
    CHECK(static_cast<int>(Category::Job)  == 1);
    CHECK(static_cast<int>(Category::Gpu)  == 5);
}
