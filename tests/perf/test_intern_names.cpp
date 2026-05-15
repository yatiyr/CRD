// crd-perf v0a -- name interning + resolve.

#include <crd/perf/profiler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

#if CRD_PERF_ENABLED

TEST_CASE("intern_name interns once, resolves to the same string", "[perf][intern]")
{
    PerfFixture fx;
    const auto a = crd::perf::intern_name("scope.alpha");
    const auto b = crd::perf::intern_name("scope.beta");
    CHECK(a.is_valid());
    CHECK(b.is_valid());
    CHECK(a.value != b.value);
    CHECK(std::strcmp(crd::perf::resolve_name(a), "scope.alpha") == 0);
    CHECK(std::strcmp(crd::perf::resolve_name(b), "scope.beta") == 0);
}

TEST_CASE("intern_name is idempotent for identical literal content", "[perf][intern]")
{
    PerfFixture fx;
    const auto x1 = crd::perf::intern_name("idempotent");
    const auto x2 = crd::perf::intern_name("idempotent");
    CHECK(x1.value == x2.value);
}

TEST_CASE("intern_name is idempotent across duplicate-content literals (FNV+strcmp)", "[perf][intern]")
{
    PerfFixture fx;
    // Two distinct string-literal objects with identical content. The
    // intern table dedupes by content, not pointer.
    const char* a = "duplicated_text_for_dedup";
    char buf[64] = {};
    const char* src = "duplicated_text_for_dedup";
    const auto src_len = std::strlen(src);
    std::copy_n(src, src_len + 1U, buf); // include the NUL

    const auto id_a   = crd::perf::intern_name(a);
    const auto id_buf = crd::perf::intern_name(buf);
    CHECK(id_a.value == id_buf.value);
}

TEST_CASE("intern_name on nullptr / inactive profiler returns invalid", "[perf][intern]")
{
    // No init() — profiler inactive.
    CHECK_FALSE(crd::perf::is_active());
    const auto id = crd::perf::intern_name("foo");
    CHECK_FALSE(id.is_valid());
}

TEST_CASE("resolve_name on invalid id returns empty string (no crash)", "[perf][intern]")
{
    PerfFixture fx;
    const char* s = crd::perf::resolve_name(crd::perf::kInvalidNameId);
    REQUIRE(s != nullptr);
    CHECK(std::strlen(s) == 0U);
}

#endif // CRD_PERF_ENABLED
