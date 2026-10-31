#include <catch2/catch_test_macros.hpp>
#include <crd/core/fnv1a.hpp>

// The engine's one FNV-1a (capability hash.fnv1a): the published FNV-1a test values, the running-state identity every
// field-by-field caller relies on, the endian-independent integer folds and the word step.

namespace
{
constexpr const char kFoobar[] = "foobar";
// FNV-1a over a native-order buffer, as a byte loop: the reference the folds must equal on this little-endian host.
crd::u64 reference_64(const crd::u8* bytes, crd::usize n)
{
    crd::u64 h = 0xCBF29CE484222325ULL;
    for (crd::usize i = 0; i < n; ++i)
    {
        h ^= bytes[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}
} // namespace

TEST_CASE("FNV-1a gives the published test values", "[core][fnv1a]")
{
    STATIC_REQUIRE(crd::fnv1a_64("", 0) == 0xCBF29CE484222325ULL);
    STATIC_REQUIRE(crd::fnv1a_64("a", 1) == 0xAF63DC4C8601EC8CULL);
    STATIC_REQUIRE(crd::fnv1a_64(kFoobar, 6) == 0x85944171F73967E8ULL);
    STATIC_REQUIRE(crd::fnv1a_32("", 0) == 0x811C9DC5U);
    STATIC_REQUIRE(crd::fnv1a_32("a", 1) == 0xE40C292CU);
    STATIC_REQUIRE(crd::fnv1a_32(kFoobar, 6) == 0xBF9CF968U);
    const crd::u8 bytes[6] = {'f', 'o', 'o', 'b', 'a', 'r'};
    CHECK(crd::fnv1a_64(static_cast<const void*>(bytes), 6) == 0x85944171F73967E8ULL);
    CHECK(crd::fnv1a_32(static_cast<const void*>(bytes), 6) == 0xBF9CF968U);
    // Bytes above 0x7F hash as unsigned in the text form too (a signed char must not sign-extend).
    const char high[2] = {static_cast<char>(0xC3), static_cast<char>(0xA7)};
    const crd::u8 high_bytes[2] = {0xC3U, 0xA7U};
    CHECK(crd::fnv1a_64(high, 2) == reference_64(high_bytes, 2));
    CHECK(crd::fnv1a_64(high, 2) == crd::fnv1a_64(static_cast<const void*>(high_bytes), 2));
}

TEST_CASE("FNV-1a continues from a running state", "[core][fnv1a]")
{
    for (crd::usize split = 0; split <= 6; ++split)
    {
        const crd::u64 head = crd::fnv1a_64(kFoobar, split);
        CHECK(crd::fnv1a_64(kFoobar + split, 6 - split, head) == crd::fnv1a_64(kFoobar, 6));
        const crd::u32 head32 = crd::fnv1a_32(kFoobar, split);
        CHECK(crd::fnv1a_32(kFoobar + split, 6 - split, head32) == crd::fnv1a_32(kFoobar, 6));
    }
}

TEST_CASE("FNV-1a integer folds feed the bytes least significant first", "[core][fnv1a]")
{
    const crd::u64 v64 = 0x0123456789ABCDEFULL;
    const crd::u8  le64[8] = {0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    CHECK(crd::fnv1a_64_le(v64, crd::kFnv1a64Offset) == reference_64(le64, 8));
    const crd::u8 le32[4] = {0xEF, 0xCD, 0xAB, 0x89};
    CHECK(crd::fnv1a_64_le(static_cast<crd::u32>(v64), crd::kFnv1a64Offset) == reference_64(le32, 4));
    CHECK(crd::fnv1a_64_le(static_cast<crd::u16>(0xABCDU), crd::kFnv1a64Offset) == reference_64(le32 + 1, 2));
    CHECK(crd::fnv1a_64_le(static_cast<crd::u8>(0xEFU), crd::kFnv1a64Offset) == reference_64(le64, 1));
    // A negative value folds its own two's-complement bytes, not a sign extension beyond them.
    const crd::u8 minus_two[4] = {0xFE, 0xFF, 0xFF, 0xFF};
    CHECK(crd::fnv1a_64_le(static_cast<crd::i32>(-2), crd::kFnv1a64Offset) == reference_64(minus_two, 4));
    CHECK(crd::fnv1a_32_le(static_cast<crd::u32>(v64), crd::kFnv1a32Offset) ==
          crd::fnv1a_32(static_cast<const void*>(le32), 4));
    STATIC_REQUIRE(crd::fnv1a_64_le(crd::u64{0}, crd::kFnv1a64Offset) != crd::kFnv1a64Offset);
}

TEST_CASE("FNV-1a word step is one xor-multiply over the whole word", "[core][fnv1a]")
{
    STATIC_REQUIRE(crd::fnv1a_64_word(0, crd::kFnv1a64Offset) == crd::kFnv1a64Offset * crd::kFnv1a64Prime);
    const crd::u64 v = 0x0123456789ABCDEFULL;
    CHECK(crd::fnv1a_64_word(v, 7U) == ((7U ^ v) * 0x100000001B3ULL));
    // It is not FNV-1a over the word's bytes: a persisted identity uses fnv1a_64_le.
    CHECK(crd::fnv1a_64_word(v, crd::kFnv1a64Offset) != crd::fnv1a_64_le(v, crd::kFnv1a64Offset));
    CHECK(crd::fnv1a_64_word(1U, crd::fnv1a_64_word(2U, crd::kFnv1a64Offset)) !=
          crd::fnv1a_64_word(2U, crd::fnv1a_64_word(1U, crd::kFnv1a64Offset)));
}
