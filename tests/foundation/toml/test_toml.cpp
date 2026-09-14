// crd-toml -- reader/writer tests (native TOML, retires tomlplusplus).

#include <crd/toml/toml.hpp>

#include <catch2/catch_test_macros.hpp>

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
