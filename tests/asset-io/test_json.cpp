// tests/asset-io/test_json.cpp — GEO-3: the owned JSON parser gates. RFC 8259 conformance on the surface glTF (and the
// future agent-CLI reports) exercise: nesting, escapes incl. surrogate pairs, number forms, and the failure classes
// (trailing junk, unterminated strings, depth bombs, lone surrogates) — no partial DOM ever escapes a failed parse.

#include <catch2/catch_test_macros.hpp>

#include <crd/assetio/json.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

namespace js = crd::assetio::json;

namespace
{
[[nodiscard]] crd::containers::ConstSpan<crd::u8> span_of(const char* s)
{
    return crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(s), std::strlen(s));
}
} // namespace

TEST_CASE("assetio: JSON basics -- nesting, finds, typed reads", "[assetio][json]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    js::JsonDoc                doc(&alloc);
    REQUIRE(js::parse(span_of(R"({"a": 1, "b": [10, 20.5, -3e2], "c": {"d": true, "e": null}, "f": "hi"})"), doc));

    const crd::u32 root = doc.root;
    CHECK(js::as_i64(doc, js::find(doc, root, "a"), -1) == 1);
    const crd::u32 b = js::find(doc, root, "b");
    CHECK(js::count_of(doc, b) == 3U);
    CHECK(js::as_f64(doc, js::at(doc, b, 1), 0.0) == 20.5);
    CHECK(js::as_f64(doc, js::at(doc, b, 2), 0.0) == -300.0);
    CHECK(js::at(doc, b, 3) == js::kInvalid); // out of range
    const crd::u32 c = js::find(doc, root, "c");
    CHECK(js::as_bool(doc, js::find(doc, c, "d"), false));
    CHECK(js::find(doc, c, "nope") == js::kInvalid);
    CHECK(js::str_value_eq(doc, js::find(doc, root, "f"), "hi"));
    char buf[8];
    CHECK(js::str_value(doc, js::find(doc, root, "f"), buf, sizeof(buf)) == 2U);
    CHECK(std::strcmp(buf, "hi") == 0);
    // kInvalid-safe chaining
    CHECK(js::as_f64(doc, js::find(doc, js::kInvalid, "x"), 7.5) == 7.5);
}

TEST_CASE("assetio: JSON string escapes incl. unicode surrogate pairs", "[assetio][json]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    js::JsonDoc                doc(&alloc);
    REQUIRE(js::parse(span_of(R"({"s": "a\"b\\c\n\tAé😀"})"), doc));
    char buf[64];
    (void)js::str_value(doc, js::find(doc, doc.root, "s"), buf, sizeof(buf));
    // a"b\c <nl> <tab> A é 😀  — é = C3 A9, the emoji = F0 9F 98 80 (surrogate pair D83D DE00 → U+1F600)
    const char expect[] = {'a', '"', 'b', '\\', 'c', '\n', '\t', 'A', '\xC3', '\xA9', '\xF0', '\x9F', '\x98', '\x80', '\0'};
    CHECK(std::strcmp(buf, expect) == 0);
}

TEST_CASE("assetio: JSON failure classes -- no partial DOM survives", "[assetio][json]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    js::JsonDoc                doc(&alloc);

    CHECK_FALSE(js::parse(span_of(R"({"a": 1} trailing)"), doc));
    CHECK(doc.nodes.size() == 0U); // cleared on failure
    CHECK_FALSE(js::parse(span_of(R"({"a": "unterminated)"), doc));
    CHECK_FALSE(js::parse(span_of(R"({"a": tru})"), doc));
    CHECK_FALSE(js::parse(span_of(R"({"a": 1,})"), doc));   // trailing comma
    CHECK_FALSE(js::parse(span_of(R"({"a" 1})"), doc));     // missing colon
    CHECK_FALSE(js::parse(span_of("{\"a\": \"\x01\"}"), doc)); // raw control char in a string
    CHECK_FALSE(js::parse(span_of(R"({"s": "\udc00"})"), doc)); // lone low surrogate
    CHECK_FALSE(js::parse(span_of(""), doc));

    // depth bomb: 80 nested arrays exceed the 64 cap — clean failure, no stack blow
    char bomb[161];
    for (int i = 0; i < 80; ++i) { bomb[i] = '['; }
    for (int i = 0; i < 80; ++i) { bomb[80 + i] = ']'; }
    bomb[160] = '\0';
    CHECK_FALSE(js::parse(span_of(bomb), doc));
}

TEST_CASE("assetio: JSON empty containers + whitespace tolerance", "[assetio][json]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    js::JsonDoc                doc(&alloc);
    REQUIRE(js::parse(span_of(" \t\r\n { \"a\" : { } , \"b\" : [ ] } \n"), doc));
    CHECK(js::count_of(doc, js::find(doc, doc.root, "a")) == 0U);
    CHECK(js::count_of(doc, js::find(doc, doc.root, "b")) == 0U);
}
