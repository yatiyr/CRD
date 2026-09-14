// DIAG.3a -- overflow-checked allocation arithmetic, including near-address-width boundaries.
// Contract: docs/design/runtime-diagnostics.md#diag-3a.

#include <crd/memory/checked_math.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
namespace mem = crd::memory;
constexpr crd::usize kMax = SIZE_MAX;
} // namespace

// Compile-time proof the helpers are usable in constant expressions (allocator sizing constants).
static_assert([] { crd::usize o = 0; return mem::checked_mul(16U, 16U, &o) && o == 256U; }());
static_assert([] { crd::usize o = 0; return !mem::checked_mul(kMax, 2U, &o); }());
static_assert([] { crd::usize o = 0; return mem::checked_align_up(1U, 16U, &o) && o == 16U; }());

TEST_CASE("checked_add: sum and overflow at the width boundary", "[memory][checked][diag]")
{
    crd::usize o = 0;
    CHECK((mem::checked_add(10U, 20U, &o) && o == 30U));
    CHECK((mem::checked_add(kMax - 1U, 1U, &o) && o == kMax)); // exact boundary is fine
    CHECK_FALSE(mem::checked_add(kMax, 1U, &o));               // one past overflows
    CHECK_FALSE(mem::checked_add(kMax, kMax, &o));
    CHECK((mem::checked_add(0U, 0U, &o) && o == 0U));
}

TEST_CASE("checked_mul: product and overflow", "[memory][checked][diag]")
{
    crd::usize o = 0;
    CHECK((mem::checked_mul(0U, kMax, &o) && o == 0U));   // zero short-circuits
    CHECK((mem::checked_mul(kMax, 1U, &o) && o == kMax)); // times one is fine
    CHECK((mem::checked_mul(1U << 20, 1U << 20, &o) && o == (crd::usize{1} << 40)));
    CHECK_FALSE(mem::checked_mul(kMax / 2U + 1U, 2U, &o));
    CHECK_FALSE(mem::checked_mul(kMax, kMax, &o));
}

TEST_CASE("checked_array_size: count*stride overflow", "[memory][checked][diag]")
{
    crd::usize o = 0;
    CHECK((mem::checked_array_size(1000U, 24U, &o) && o == 24000U));
    CHECK_FALSE(mem::checked_array_size(kMax, 2U, &o));
    CHECK((mem::checked_array_size(0U, 999U, &o) && o == 0U));
}

TEST_CASE("checked_align_up: rounding, already-aligned, non-pow2 and boundary overflow",
          "[memory][checked][diag]")
{
    crd::usize o = 0;
    CHECK((mem::checked_align_up(1U, 16U, &o) && o == 16U));
    CHECK((mem::checked_align_up(16U, 16U, &o) && o == 16U)); // already aligned
    CHECK((mem::checked_align_up(17U, 16U, &o) && o == 32U));
    CHECK((mem::checked_align_up(0U, 4096U, &o) && o == 0U));
    CHECK((mem::checked_align_up(123U, 1U, &o) && o == 123U)); // alignment 1 is a no-op

    CHECK_FALSE(mem::checked_align_up(100U, 24U, &o)); // 24 is not a power of two
    CHECK_FALSE(mem::checked_align_up(100U, 0U, &o));  // zero alignment

    // Near the width boundary the naive (value + align - 1) wraps; checked_align_up refuses.
    CHECK_FALSE(mem::checked_align_up(kMax - 1U, 16U, &o));
    CHECK((mem::checked_align_up(kMax - 15U, 16U, &o) && o == kMax - 15U)); // exactly aligned, no round
}

TEST_CASE("checked_page_count: rounding up and overflow", "[memory][checked][diag]")
{
    crd::usize o = 0;
    CHECK((mem::checked_page_count(0U, 4096U, &o) && o == 0U));
    CHECK((mem::checked_page_count(1U, 4096U, &o) && o == 1U));
    CHECK((mem::checked_page_count(4096U, 4096U, &o) && o == 1U)); // exact multiple
    CHECK((mem::checked_page_count(4097U, 4096U, &o) && o == 2U)); // rounds up
    CHECK_FALSE(mem::checked_page_count(100U, 0U, &o));            // zero page size
    CHECK_FALSE(mem::checked_page_count(kMax, 4096U, &o));         // bytes + page-1 overflows
}

TEST_CASE("checked_padded_array_size: stride padded then multiplied, end-to-end checked",
          "[memory][checked][diag]")
{
    crd::usize o = 0;
    // stride 12 padded to 16, times 10 = 160.
    CHECK((mem::checked_padded_array_size(10U, 12U, 16U, &o) && o == 160U));
    // A non-power-of-two alignment fails at the padding step.
    CHECK_FALSE(mem::checked_padded_array_size(10U, 12U, 24U, &o));
    // Overflow in the multiply after padding is caught.
    CHECK_FALSE(mem::checked_padded_array_size(kMax, 12U, 16U, &o));
}
