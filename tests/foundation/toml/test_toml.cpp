// crd-toml -- reader/writer tests (native TOML, retires tomlplusplus).

#include <crd/toml/toml.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string_view>

namespace
{
namespace ct = crd::toml;
namespace cont = crd::containers;
} // namespace

TEST_CASE("toml: scalars parse with the right kinds", "[toml]")
{
    const auto r = ct::parse("i = 42\nf = 1.5\nb = true\ns = \"hi\"\nn = -7\n");
    REQUIRE(r);
    const ct::node& t = r.table();

    REQUIRE(t.get(cont::StringView{"i"}) != nullptr);
    CHECK(t.get(cont::StringView{"i"})->value<crd::i64>() == 42);
    CHECK(t.get(cont::StringView{"f"})->value<double>() == 1.5);
    CHECK(t.get(cont::StringView{"b"})->value<bool>() == true);
    CHECK(t.get(cont::StringView{"n"})->value<crd::i64>() == -7);

    const auto s = t.get(cont::StringView{"s"})->value<std::string_view>();
    REQUIRE(s.has_value());
    CHECK(*s == "hi");

    // A float is not an integer (strict), but an integer widens to double (toml++).
    CHECK_FALSE(t.get(cont::StringView{"f"})->value<crd::i64>().has_value());
    CHECK(t.get(cont::StringView{"i"})->value<double>() == 42.0);
}

TEST_CASE("toml: tables, dotted headers and get()", "[toml]")
{
    const auto r = ct::parse("schema = 1\n[a]\nx = 1\n[a.b]\ny = 2\n");
    REQUIRE(r);
    const ct::node& t = r.table();

    const auto* a = t.get(cont::StringView{"a"})->as_table();
    REQUIRE(a != nullptr);
    CHECK(a->get(cont::StringView{"x"})->value<crd::i64>() == 1);

    const auto* b = a->get(cont::StringView{"b"})->as_table();
    REQUIRE(b != nullptr);
    CHECK(b->get(cont::StringView{"y"})->value<crd::i64>() == 2);
}

TEST_CASE("toml: inline arrays and array-of-tables", "[toml]")
{
    const auto r = ct::parse(
        "list = [\"MeshRenderer\", \"Transform\"]\n"
        "[[resource]]\nname = \"a\"\nscale = 1.0\n"
        "[[resource]]\nname = \"b\"\nscale = 2.0\n");
    REQUIRE(r);
    const ct::node& t = r.table();

    const auto* arr = t.get(cont::StringView{"list"})->as_array();
    REQUIRE(arr != nullptr);
    REQUIRE(arr->size() == 2U);
    CHECK(*arr->get(0)->value<std::string_view>() == "MeshRenderer");
    CHECK(*arr->get(1)->value<std::string_view>() == "Transform");

    const auto* res = t.get(cont::StringView{"resource"})->as_array();
    REQUIRE(res != nullptr);
    REQUIRE(res->size() == 2U);
    CHECK(*res->get(0)->as_table()->get(cont::StringView{"name"})->value<std::string_view>() == "a");
    CHECK(res->get(1)->as_table()->get(cont::StringView{"scale"})->value<double>() == 2.0);
}

TEST_CASE("toml: inline tables and comments", "[toml]")
{
    const auto r = ct::parse("# a comment\npoint = { x = 1, y = 2 }  # trailing\n");
    REQUIRE(r);
    const auto* p = r.table().get(cont::StringView{"point"})->as_table();
    REQUIRE(p != nullptr);
    CHECK(p->get(cont::StringView{"x"})->value<crd::i64>() == 1);
    CHECK(p->get(cont::StringView{"y"})->value<crd::i64>() == 2);
}

TEST_CASE("toml: malformed input reports a failure, not a crash", "[toml]")
{
    const auto r = ct::parse("key = \n");
    CHECK_FALSE(r);
    CHECK_FALSE(cont::StringView{r.error().description()}.empty());
}

TEST_CASE("toml: a key defined twice in one table is refused, never last-one-wins", "[toml]")
{
    const auto root = ct::parse("capacity = 64\nname = \"a\"\ncapacity = 1\n");
    REQUIRE_FALSE(root);
    CHECK(cont::StringView{root.error().description()}.find(cont::StringView{"duplicate key"}) !=
          cont::StringView::npos);
    CHECK(root.error().line() == 3U);

    const auto in_table = ct::parse("[pass]\nmode = \"a\"\n[other]\nmode = \"b\"\n[pass.params]\nx = 1\nx = 2\n");
    CHECK_FALSE(in_table);
    const auto inline_table = ct::parse("point = { x = 1, x = 2 }\n");
    CHECK_FALSE(inline_table);

    // The same key in different tables, and in successive array-of-tables elements, is not a duplicate.
    const auto distinct = ct::parse("[a]\nk = 1\n[b]\nk = 2\n[[item]]\nk = 1\n[[item]]\nk = 2\n");
    CHECK(distinct);
}

namespace
{
// crd-toml's nesting limit (toml.cpp): the root table is depth 0, and each table, array or inline table one deeper.
constexpr crd::u32 kMaxNestingDepth = 64U;

// `levels` nested arrays (`[[[...]]]`) as the value of `v`, or inline tables (`{ a = { a = ... } }`) when `tables`.
cont::String nested_value(crd::u32 levels, bool tables)
{
    cont::String text{"v = "};
    for (crd::u32 i = 0U; i < levels; ++i)
    {
        text.append(tables ? cont::StringView{"{ a = "} : cont::StringView{"["});
    }
    text.append(cont::StringView{"1"});
    for (crd::u32 i = 0U; i < levels; ++i)
    {
        text.append(tables ? cont::StringView{" }"} : cont::StringView{"]"});
    }
    text.push_back('\n');
    return text;
}

// A `[t.t.t...]` header of `segments` segments, then one key.
cont::String nested_header(crd::u32 segments)
{
    cont::String text{"[t"};
    for (crd::u32 i = 1U; i < segments; ++i)
    {
        text.append(cont::StringView{".t"});
    }
    text.append(cont::StringView{"]\nk = 1\n"});
    return text;
}

bool refused_with(const cont::String& text, const char* reason)
{
    const auto r = ct::parse(std::string_view{text.data(), text.size()});
    return !r && cont::StringView{r.error().description()}.find(cont::StringView{reason}) != cont::StringView::npos;
}

bool accepts(const cont::String& text)
{
    return static_cast<bool>(ct::parse(std::string_view{text.data(), text.size()}));
}
} // namespace

TEST_CASE("toml: nesting past the depth limit is refused, never a stack overflow", "[toml][diag10a]")
{
    // A key's value sits one level below its table, so 64 nested containers are the deepest a root key may hold.
    CHECK(accepts(nested_value(kMaxNestingDepth, false)));
    CHECK(accepts(nested_value(kMaxNestingDepth, true)));
    CHECK(refused_with(nested_value(kMaxNestingDepth + 1U, false), "64-level limit"));
    CHECK(refused_with(nested_value(kMaxNestingDepth + 1U, true), "64-level limit"));
    // Far past the limit (the recursion is refused at the limit, not after it ran out of stack).
    CHECK(refused_with(nested_value(20000U, false), "64-level limit"));
    CHECK(refused_with(nested_value(20000U, true), "64-level limit"));

    // Header paths count too, and a value under a deep table counts from the table's depth.
    CHECK(accepts(nested_header(kMaxNestingDepth)));
    CHECK(refused_with(nested_header(kMaxNestingDepth + 1U), "64-level limit"));
    CHECK(refused_with(nested_header(30000U), "64-level limit"));
    cont::String deep_table = nested_header(kMaxNestingDepth - 1U);
    deep_table.append(cont::StringView{"x = [1]\n"});
    CHECK(accepts(deep_table));
    deep_table.append(cont::StringView{"y = [[1]]\n"});
    CHECK(refused_with(deep_table, "64-level limit"));

    // A segment through an array of tables is two levels: the array, then its most recent element.
    cont::String through_arrays;
    for (crd::u32 i = 1U; i <= 33U; ++i)
    {
        through_arrays.append(cont::StringView{"[["});
        for (crd::u32 s = 0U; s < i; ++s)
        {
            through_arrays.append(s == 0U ? cont::StringView{"a"} : cont::StringView{".a"});
        }
        through_arrays.append(cont::StringView{"]]\n"});
        if (i == 32U)
        {
            CHECK(accepts(through_arrays)); // 32 arrays of tables, each with its element: 64 levels
        }
    }
    CHECK(refused_with(through_arrays, "64-level limit"));
}

TEST_CASE("toml: an out-of-range or over-long number is refused, never clamped or cut", "[toml][diag10a]")
{
    const auto integer = [](const char* text) {
        const auto r = ct::parse(text);
        return r ? r.table().get(cont::StringView{"n"})->value<crd::i64>() : std::optional<crd::i64>{};
    };
    CHECK(integer("n = 9223372036854775807\n") == crd::i64{9223372036854775807});
    CHECK(integer("n = -9223372036854775808\n") == (-crd::i64{9223372036854775807} - 1));
    CHECK(refused_with(cont::String{"n = 9223372036854775808\n"}, "64-bit signed range"));
    CHECK(refused_with(cont::String{"n = -9223372036854775809\n"}, "64-bit signed range"));
    CHECK(refused_with(cont::String{"n = 99999999999999999999999\n"}, "64-bit signed range"));

    CHECK(refused_with(cont::String{"f = 1e999\n"}, "float out of range"));
    CHECK(refused_with(cont::String{"f = -1e999\n"}, "float out of range"));
    CHECK(accepts(cont::String{"f = 1e-999\n"})); // an underflow is the nearest double, not an error

    // 63 characters fit; the 64th would have been dropped, reading "1000...0" as a number ten times smaller.
    cont::String fits{"f = 0."};
    cont::String over{"n = 1"};
    for (crd::u32 i = 0U; i < 60U; ++i)
    {
        fits.push_back('0');
    }
    fits.append(cont::StringView{"1\n"});
    for (crd::u32 i = 0U; i < 63U; ++i)
    {
        over.push_back('0');
    }
    over.push_back('\n');
    CHECK(accepts(fits));
    CHECK(refused_with(over, "longer than 63 characters"));
    // Underscores are not characters of the number.
    CHECK(integer("n = 1_000_000\n") == crd::i64{1000000});
}

TEST_CASE("toml: build and serialize round-trips", "[toml]")
{
    ct::table root;
    root.insert_or_assign(cont::StringView{"name"}, "engine://frame/x");
    root.insert_or_assign(cont::StringView{"schema"}, crd::i64{1});
    ct::array arr;
    arr.push_back(crd::f64{1.0});
    arr.push_back(crd::f64{2.0});
    root.insert_or_assign(cont::StringView{"scale"}, static_cast<ct::node&&>(arr));

    const cont::String text = root.to_toml();
    const auto reparsed = ct::parse(std::string_view{text});
    REQUIRE(reparsed);
    const ct::node& t = reparsed.table();
    CHECK(*t.get(cont::StringView{"name"})->value<std::string_view>() == "engine://frame/x");
    CHECK(t.get(cont::StringView{"schema"})->value<crd::i64>() == 1);
    const auto* s = t.get(cont::StringView{"scale"})->as_array();
    REQUIRE(s != nullptr);
    REQUIRE(s->size() == 2U);
    CHECK(s->get(1)->value<double>() == 2.0);
}
