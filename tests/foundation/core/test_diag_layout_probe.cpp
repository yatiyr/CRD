// DIAG.10c -- layout probes and the strided value walk (crd/core/layout_probe.hpp), and the foreign-call mode witness
// (crd/core/fp_environment.hpp). A layout's element count and offsets are checked exactly against the u64/i64 ranges
// (an empty view's other extents too), against named storage only when it is named, and an overlap is reported only
// when proven and is a violation only for a written view; the walk names the first offender by its coordinates and
// lane and never depends on the caller's numerical mode; a foreign call that changes the mode is reported and undone.
// Every expected value is written by hand. ASCII test names.

#include <crd/core/fp_environment.hpp>
#include <crd/core/layout_probe.hpp>
#include <crd/core/value_domain.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::i64;
using crd::LayoutExpectation;
using crd::LayoutOverlap;
using crd::LayoutReport;
using crd::LayoutViolation;
using crd::u32;
using crd::u64;
using crd::ValueViolation;

constexpr u32 bit(LayoutViolation v)
{
    return static_cast<u32>(v);
}

constexpr u32 vbit(ValueViolation v)
{
    return static_cast<u32>(v);
}

constexpr u64 kTwoTo32 = 1ULL << 32U;
constexpr u64 kTwoTo40 = 1ULL << 40U;
constexpr i64 kTwoTo62 = i64{1} << 62;

LayoutReport probe(const u64* shape, const i64* strides, u32 rank, u64 element_size = 1U, bool writable = false)
{
    LayoutExpectation e{};
    e.element_size = element_size;
    e.writable     = writable;
    return crd::probe_layout(shape, strides, rank, e);
}

LayoutReport probe_in(const u64* shape, const i64* strides, u32 rank, i64 first, i64 end)
{
    LayoutExpectation e{};
    e.has_storage   = true;
    e.storage_first = first;
    e.storage_end   = end;
    return crd::probe_layout(shape, strides, rank, e);
}

f32 f32_of(u32 b)
{
    f32 f = 0.0F;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

f64 f64_of(u64 b)
{
    f64 f = 0.0;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

constexpr u32 kF32NaN    = 0x7FC00000U;
constexpr u32 kF32Inf    = 0x7F800000U;
constexpr u32 kF32NegSub = 0x80000100U; // -(2^-141)
constexpr u32 kF32MinSub = 0x00000001U; // 2^-149
constexpr u64 kF64NegInf = 0xFFF0000000000000ULL;
} // namespace

TEST_CASE("diag10c: a layout probe reads offsets and proves overlap only where it can", "[core][layout][diag10c]")
{
    const u64 s23[] = {2U, 3U};
    const i64 c23[] = {3, 1};
    const LayoutReport contiguous = probe(s23, c23, 2U, 4U, true);
    CHECK(contiguous.ok());
    CHECK(contiguous.element_count == 6U);
    CHECK(contiguous.min_offset == 0);
    CHECK(contiguous.max_offset == 5);
    CHECK(contiguous.overlap == LayoutOverlap::Disjoint);
    CHECK_FALSE(contiguous.storage_checked);

    const u64 s4[] = {4U};
    const i64 flip[] = {-1};
    const LayoutReport flipped = probe(s4, flip, 1U);
    CHECK(flipped.ok());
    CHECK(flipped.min_offset == -3);
    CHECK(flipped.max_offset == 0);
    CHECK(flipped.overlap == LayoutOverlap::Disjoint);

    const u64 s32[] = {3U, 2U};
    const i64 t32[] = {1, 3}; // a transposed [2, 3]
    CHECK(probe(s32, t32, 2U).overlap == LayoutOverlap::Disjoint);
    CHECK(probe(s32, t32, 2U).max_offset == 5);

    // A broadcast is a valid read and a proven overlap; it is a violation only for a written view.
    const u64 s34[] = {3U, 4U};
    const i64 b34[] = {0, 1};
    const LayoutReport read = probe(s34, b34, 2U, 4U, false);
    const LayoutReport write = probe(s34, b34, 2U, 4U, true);
    CHECK(read.ok());
    CHECK(read.overlap == LayoutOverlap::Overlapping);
    CHECK(write.overlap == LayoutOverlap::Overlapping);
    CHECK(write.violations == bit(LayoutViolation::Overlap));
    // A stride-0 dim of extent 1 addresses one element: no overlap.
    const u64 s14[] = {1U, 4U};
    CHECK(probe(s14, b34, 2U, 4U, true).overlap == LayoutOverlap::Disjoint);

    // Equal strides, and a stride that a smaller one reaches: index (1, 0) meets index (0, 2) at offset 2.
    const u64 s22[] = {2U, 2U};
    const i64 e22[] = {1, 1};
    CHECK(probe(s22, e22, 2U).overlap == LayoutOverlap::Overlapping);
    const i64 m23[] = {2, 1};
    CHECK(probe(s23, m23, 2U, 1U, true).violations == bit(LayoutViolation::Overlap));
    // Opposite signs: index (1, 2) meets the origin (2 - 2 = 0).
    const i64 o23[] = {2, -1};
    CHECK(probe(s23, o23, 2U).overlap == LayoutOverlap::Overlapping);
    // Strides 2 and 1 over extents 3 and 2 nest (offsets 0..5, each once).
    const i64 n32[] = {2, 1};
    CHECK(probe(s32, n32, 2U).overlap == LayoutOverlap::Disjoint);
    // Offsets 3i + 5j over extents 3 and 2 are distinct, but no rule proves it: not proven, never an overlap.
    const i64 odd[] = {3, 5};
    const LayoutReport unproven = probe(s32, odd, 2U, 1U, true);
    CHECK(unproven.overlap == LayoutOverlap::NotProven);
    CHECK(unproven.ok());

    // Rank 0 is one element at offset 0.
    const LayoutReport scalar = probe(nullptr, nullptr, 0U, 8U, true);
    CHECK(scalar.ok());
    CHECK(scalar.element_count == 1U);
    CHECK(scalar.overlap == LayoutOverlap::Disjoint);
}

TEST_CASE("diag10c: a layout probe refuses counts and offsets past the integer ranges", "[core][layout][diag10c]")
{
    // [2^32, 2^32, 2]: the count is 2^65, which a plain u64 product wraps to 0 (an empty tensor).
    const u64 big[]  = {kTwoTo32, kTwoTo32, 2U};
    const i64 bigs[] = {0, 0, 0};
    const LayoutReport wraps = probe(big, bigs, 3U);
    CHECK(wraps.violations == bit(LayoutViolation::ElementCountOverflow));
    CHECK(wraps.element_count == 0U);

    // An empty view whose other extents do not multiply out is refused; one whose extents do is empty and fine.
    const u64 empty_big[] = {0U, kTwoTo40, kTwoTo40};
    CHECK(probe(empty_big, bigs, 3U).violations == bit(LayoutViolation::ElementCountOverflow));
    const u64 empty[] = {0U, 4U, 5U};
    const i64 es[]    = {20, 5, 1};
    const LayoutReport fine = probe(empty, es, 3U, 8U, true);
    CHECK(fine.ok());
    CHECK(fine.element_count == 0U);
    CHECK(fine.overlap == LayoutOverlap::Disjoint);

    // The byte size counts too: 2^62 elements of 8 bytes.
    const u64 s62[] = {u64{1} << 62U};
    const i64 one[] = {1};
    CHECK(probe(s62, one, 1U, 4U).violations == bit(LayoutViolation::ElementCountOverflow));
    CHECK(probe(s62, one, 1U, 2U).ok());

    // Offsets: element 2 of stride 2^62 is at 2^63, past i64; stride -2^62 reaches exactly -2^63, which fits in
    // elements but not in 4-byte units.
    const u64 s3[]   = {3U};
    const i64 up[]   = {kTwoTo62};
    const i64 down[] = {-kTwoTo62};
    CHECK(probe(s3, up, 1U).violations == bit(LayoutViolation::OffsetOverflow));
    const LayoutReport edge = probe(s3, down, 1U);
    CHECK(edge.ok());
    CHECK(edge.min_offset == std::numeric_limits<i64>::min());
    CHECK(probe(s3, down, 1U, 4U).violations == bit(LayoutViolation::OffsetOverflow));
    // INT64_MIN as a stride over two elements reaches INT64_MIN.
    const u64 s2[]  = {2U};
    const i64 min[] = {std::numeric_limits<i64>::min()};
    CHECK(probe(s2, min, 1U).ok());
    // Two dims whose reaches fit alone but not summed.
    const u64 s22[]  = {2U, 2U};
    const i64 sum[]  = {std::numeric_limits<i64>::max(), 1};
    CHECK(probe(s22, sum, 2U).violations == bit(LayoutViolation::OffsetOverflow));

    // Rank: past the bound nothing else is read.
    LayoutExpectation two{};
    two.max_rank = 2U;
    const LayoutReport over = crd::probe_layout(empty, es, 3U, two);
    CHECK(over.violations == bit(LayoutViolation::RankOverflow));
    CHECK(over.overlap == LayoutOverlap::NotProven);
    CHECK(probe(big, bigs, crd::kLayoutMaxRank + 1U).violations == bit(LayoutViolation::RankOverflow));
}

TEST_CASE("diag10c: storage is checked only when it is named", "[core][layout][diag10c]")
{
    const u64 s23[] = {2U, 3U};
    const i64 c23[] = {3, 1};
    const LayoutReport inside = probe_in(s23, c23, 2U, 0, 6);
    CHECK(inside.ok());
    CHECK(inside.storage_checked);
    CHECK(probe_in(s23, c23, 2U, 0, 5).violations == bit(LayoutViolation::OutsideStorage));
    CHECK(probe_in(s23, c23, 2U, 1, 6).violations == bit(LayoutViolation::OutsideStorage));

    const u64 s4[]   = {4U};
    const i64 flip[] = {-1};
    CHECK(probe_in(s4, flip, 1U, -3, 1).ok());
    CHECK(probe_in(s4, flip, 1U, -2, 1).violations == bit(LayoutViolation::OutsideStorage));

    // Unnamed storage is unchecked, never "inside"; an empty view lies inside any storage; overflowing offsets cannot
    // be compared, so they are not reported as inside either.
    CHECK_FALSE(probe(s23, c23, 2U).storage_checked);
    const u64 empty[] = {0U, 3U};
    const LayoutReport none = probe_in(empty, c23, 2U, 0, 0);
    CHECK(none.ok());
    CHECK(none.storage_checked);
    const u64 s3[] = {3U};
    const i64 up[] = {kTwoTo62};
    const LayoutReport unknown = probe_in(s3, up, 1U, 0, 6);
    CHECK(unknown.violations == bit(LayoutViolation::OffsetOverflow));
    CHECK_FALSE(unknown.storage_checked);
}

TEST_CASE("diag10c: a strided walk names the first offender by its coordinates and lane", "[core][layout][diag10c]")
{
    // A [2, 3] view stored transposed (column-major): logical (i, j) is at offset i + 2j.
    f32 storage[6] = {};
    for (f32& v : storage)
    {
        v = 0.5F;
    }
    storage[1 + 2 * 2] = f32_of(kF32NaN); // logical (1, 2): row-major index 5
    storage[0 + 2 * 1] = f32_of(kF32Inf); // logical (0, 1): row-major index 1
    const u64 shape[]   = {2U, 3U};
    const i64 strides[] = {1, 2};
    const crd::StridedValueReport r =
        crd::probe_strided_values(crd::finite_float_domain(), storage, shape, strides, 2U, 1U);
    REQUIRE(r.walked);
    CHECK(r.values.checked == 6U);
    CHECK(r.values.violating == 2U);
    CHECK(r.values.violations == (vbit(ValueViolation::NaN) | vbit(ValueViolation::Infinity)));
    CHECK(r.values.first_index == 1U);
    CHECK(r.first_coords[0] == 0U);
    CHECK(r.first_coords[1] == 1U);
    CHECK(r.values.first_bits == kF32Inf);
    CHECK(r.first_lane == 0U);

    // A flipped view starts at the physical end: logical 0 is storage[3].
    f64 row[4] = {1.0, 2.0, f64_of(kF64NegInf), 4.0};
    const u64 s4[]   = {4U};
    const i64 flip[] = {-1};
    const crd::StridedValueReport back =
        crd::probe_strided_values(crd::finite_float_domain(), row + 3, s4, flip, 1U, 1U);
    REQUIRE(back.walked);
    CHECK(back.values.first_index == 1U); // storage[2] is logical 1
    CHECK(back.values.first_violations == vbit(ValueViolation::NegativeInfinity));
    CHECK(back.values.first_bits == kF64NegInf);

    // Interleaved complex values: element 1's imaginary lane is the offender.
    f32 complex[6] = {0.0F, 0.0F, 1.0F, f32_of(kF32Inf), 2.0F, 3.0F};
    const u64 s3[] = {3U};
    const i64 c1[] = {1};
    const crd::StridedValueReport z = crd::probe_strided_values(crd::finite_float_domain(), complex, s3, c1, 1U, 2U);
    REQUIRE(z.walked);
    CHECK(z.values.checked == 6U);
    CHECK(z.values.first_index == 1U);
    CHECK(z.first_lane == 1U);

    // A broadcast walks the same element once per index.
    f32 one[1] = {f32_of(kF32NaN)};
    const u64 s22[] = {2U, 2U};
    const i64 b22[] = {0, 0};
    const crd::StridedValueReport bc = crd::probe_strided_values(crd::finite_float_domain(), one, s22, b22, 2U, 1U);
    CHECK(bc.values.violating == 4U);
    CHECK(bc.values.first_index == 0U);

    // A domain that admits the special value reports nothing.
    CHECK(crd::probe_strided_values(crd::any_float_domain(), storage, shape, strides, 2U, 1U).values.ok());
}

TEST_CASE("diag10c: a strided walk refuses what it cannot walk and ignores the numerical mode",
          "[core][layout][diag10c][fp]")
{
    f32 storage[4] = {f32_of(kF32NegSub), f32_of(kF32MinSub), 0.25F, 0.75F};
    const u64 shape[]   = {4U};
    const i64 strides[] = {1};

    crd::FloatDomain invalid{};
    invalid.minimum = 2.0;
    invalid.maximum = 1.0;
    const crd::StridedValueReport bad = crd::probe_strided_values(invalid, storage, shape, strides, 1U, 1U);
    CHECK_FALSE(bad.walked);
    CHECK_FALSE(bad.values.domain_valid);
    CHECK(bad.values.checked == 0U);

    const u64 huge[]   = {3U};
    const i64 hstride[] = {kTwoTo62};
    const crd::StridedValueReport overflow =
        crd::probe_strided_values(crd::finite_float_domain(), storage, huge, hstride, 1U, 1U);
    CHECK_FALSE(overflow.walked);
    CHECK(overflow.layout.violations == bit(LayoutViolation::OffsetOverflow));

    const u64 none[] = {0U};
    const crd::StridedValueReport empty =
        crd::probe_strided_values(crd::finite_float_domain(), storage, none, strides, 1U, 1U);
    CHECK_FALSE(empty.walked);
    CHECK(empty.values.checked == 0U);
    CHECK_FALSE(crd::probe_strided_values(crd::finite_float_domain(), storage, shape, strides, 1U, 0U).walked);

    // unit-interval: the negative subnormal is below 0, the positive one is inside. Under flush-to-zero,
    // denormals-are-zero and round-down a float compare would call both 0; the walk reads bits.
    const crd::StridedValueReport declared =
        crd::probe_strided_values(crd::unit_interval_domain(), storage, shape, strides, 1U, 1U);
    CHECK(declared.values.violating == 1U);
    CHECK(declared.values.first_index == 0U);
    CHECK(declared.values.first_violations == vbit(ValueViolation::BelowMinimum));

    crd::FpEnvironment hostile = crd::declared_fp_environment();
    hostile.mxcsr |= crd::kMxcsrFlushToZero | crd::kMxcsrDenormalsAreZero;
    hostile.mxcsr = (hostile.mxcsr & ~crd::kMxcsrRoundingMask) |
                    (static_cast<u32>(crd::FpRounding::Down) << crd::kMxcsrRoundingShift);
    const crd::ScopedFpEnvironment scoped(hostile);
    const crd::StridedValueReport under =
        crd::probe_strided_values(crd::unit_interval_domain(), storage, shape, strides, 1U, 1U);
    CHECK(under.values.violating == declared.values.violating);
    CHECK(under.values.first_index == declared.values.first_index);
    CHECK(under.values.first_violations == declared.values.first_violations);
    CHECK(under.values.first_bits == declared.values.first_bits);
}

TEST_CASE("diag10c: a foreign call's mode change is reported and undone", "[core][layout][diag10c][fp]")
{
    const crd::FpEnvironment before = crd::read_fp_environment();
    const u32 quiet = crd::call_keeping_fp_environment([]() {});
    CHECK(quiet == 0U);

    const u32 changed = crd::call_keeping_fp_environment(
        []()
        {
            crd::FpEnvironment ftz = crd::read_fp_environment();
            ftz.mxcsr |= crd::kMxcsrFlushToZero;
            crd::write_fp_environment(ftz);
        });
    const crd::FpEnvironment after = crd::read_fp_environment();
    CHECK(crd::fp_differences(after, before) == 0U);
#if CRD_ARCH_X64
    CHECK(changed == static_cast<u32>(crd::FpDifference::FlushToZero));
#else
    CHECK(changed == 0U); // nothing is read on this architecture, so nothing can be reported or put back
#endif
}
