// crd-perf-ui v0g -- helper utilities (color, formatting, aggregation).

#include <crd/perf/ui/panel_helpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{

using crd::perf::Sample;
using crd::perf::Category;
using crd::perf::NameId;
using crd::perf::ui::aggregate_top_level_by_name;
using crd::perf::ui::color_for_category;
using crd::perf::ui::color_for_name;
using crd::perf::ui::format_bytes;
using crd::perf::ui::format_count;
using crd::perf::ui::format_duration;
using crd::perf::ui::NameTotal;
using crd::perf::ui::total_thread_duration_ns;

} // namespace

TEST_CASE("color_for_name is deterministic + alpha-saturated", "[perf-ui][color]")
{
    const auto a = color_for_name(NameId{42U});
    const auto b = color_for_name(NameId{42U});
    CHECK(a.value == b.value);
    // alpha == 0xFF, so the high byte must be 0xFF.
    CHECK(((a.value >> 24) & 0xFFU) == 0xFFU);
}

TEST_CASE("color_for_name gives different colors to different ids", "[perf-ui][color]")
{
    const auto a = color_for_name(NameId{1U});
    const auto b = color_for_name(NameId{2U});
    const auto c = color_for_name(NameId{1000U});
    CHECK(a.value != b.value);
    CHECK(a.value != c.value);
    CHECK(b.value != c.value);
}

TEST_CASE("color_for_category returns the documented palette", "[perf-ui][color]")
{
    CHECK(color_for_category(Category::User).value   == 0xFFB0BEC5U);
    CHECK(color_for_category(Category::Job).value    == 0xFF7CB342U);
    CHECK(color_for_category(Category::System).value == 0xFFFFB300U);
    CHECK(color_for_category(Category::Pass).value   == 0xFF42A5F5U);
    CHECK(color_for_category(Category::Render).value == 0xFF26A69AU);
    CHECK(color_for_category(Category::Gpu).value    == 0xFFAB47BCU);
    CHECK(color_for_category(Category::Memory).value == 0xFFEC407AU);
    CHECK(color_for_category(Category::Io).value     == 0xFF8D6E63U);
    CHECK(color_for_category(Category::Wait).value   == 0xFF78909CU);
}

TEST_CASE("format_duration picks the right scale", "[perf-ui][format]")
{
    char buf[64];
    format_duration(750, buf, sizeof(buf));                   CHECK(std::strcmp(buf, "750 ns")        == 0);
    format_duration(1500, buf, sizeof(buf));                  CHECK(std::strcmp(buf, "1.500 us")      == 0);
    format_duration(2'500'000, buf, sizeof(buf));             CHECK(std::strcmp(buf, "2.500 ms")      == 0);
    format_duration(1'500'000'000, buf, sizeof(buf));         CHECK(std::strcmp(buf, "1.500 s")       == 0);
}

TEST_CASE("format_bytes picks the right scale (binary multipliers)", "[perf-ui][format]")
{
    char buf[32];
    format_bytes(512ULL, buf, sizeof(buf));                CHECK(std::strcmp(buf, "512 B")  == 0);
    format_bytes(2048ULL, buf, sizeof(buf));               CHECK(std::strcmp(buf, "2.00 KB") == 0);
    format_bytes(5ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "5.00 MB") == 0);
    format_bytes(3ULL * 1024ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "3.00 GB") == 0);
}

TEST_CASE("format_count picks the right shorthand", "[perf-ui][format]")
{
    char buf[16];
    format_count(42ULL, buf, sizeof(buf));         CHECK(std::strcmp(buf, "42")      == 0);
    format_count(1500ULL, buf, sizeof(buf));       CHECK(std::strcmp(buf, "1.50k")   == 0);
    format_count(2'000'000ULL, buf, sizeof(buf));  CHECK(std::strcmp(buf, "2.00M")   == 0);
    format_count(3'500'000'000ULL, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "3.50B") == 0);
}

TEST_CASE("total_thread_duration_ns sums only depth-0 samples", "[perf-ui][aggregate]")
{
    Sample s[4]{};
    s[0].begin_ns = 0;       s[0].end_ns = 100;   s[0].depth = 0;
    s[1].begin_ns = 200;     s[1].end_ns = 300;   s[1].depth = 1; // child -- ignored
    s[2].begin_ns = 400;     s[2].end_ns = 700;   s[2].depth = 0;
    s[3].begin_ns = 1000;    s[3].end_ns = 800;   s[3].depth = 0; // malformed -- ignored
    const auto total = total_thread_duration_ns(crd::containers::ConstSpan<Sample>{s, 4U});
    CHECK(total == 100U + 300U);
}

TEST_CASE("aggregate_top_level_by_name merges by name", "[perf-ui][aggregate]")
{
    Sample s[5]{};
    s[0].begin_ns = 0;    s[0].end_ns = 100;  s[0].name_id = 10; s[0].depth = 0;
    s[1].begin_ns = 100;  s[1].end_ns = 250;  s[1].name_id = 20; s[1].depth = 0;
    s[2].begin_ns = 250;  s[2].end_ns = 400;  s[2].name_id = 10; s[2].depth = 0;
    s[3].begin_ns = 400;  s[3].end_ns = 500;  s[3].name_id = 30; s[3].depth = 1; // nested -- ignored
    s[4].begin_ns = 500;  s[4].end_ns = 700;  s[4].name_id = 20; s[4].depth = 0;
    NameTotal out[8]{};
    const auto n = aggregate_top_level_by_name(crd::containers::ConstSpan<Sample>{s, 5U},
                                                out, 8U);
    REQUIRE(n == 2U);
    // Find by name; results unsorted.
    crd::u64 t10 = 0;
    crd::u64 t20 = 0;
    crd::u32 c10 = 0;
    crd::u32 c20 = 0;
    for (crd::u32 i = 0U; i < n; ++i)
    {
        if (out[i].name.value == 10U) { t10 = out[i].total_ns; c10 = out[i].occurrences; }
        if (out[i].name.value == 20U) { t20 = out[i].total_ns; c20 = out[i].occurrences; }
    }
    CHECK(t10 == 100U + 150U);
    CHECK(c10 == 2U);
    CHECK(t20 == 150U + 200U);
    CHECK(c20 == 2U);
}

TEST_CASE("aggregate respects out_capacity", "[perf-ui][aggregate][robustness]")
{
    Sample s[6]{};
    for (crd::u32 i = 0U; i < 6U; ++i)
    {
        s[i].begin_ns = static_cast<crd::i64>(i) * 100;
        s[i].end_ns   = static_cast<crd::i64>(i) * 100 + 50;
        s[i].name_id  = i;
        s[i].depth    = 0;
    }
    NameTotal out[3]{};
    const auto n = aggregate_top_level_by_name(crd::containers::ConstSpan<Sample>{s, 6U},
                                                out, 3U);
    CHECK(n == 3U); // capped at out_capacity
}
