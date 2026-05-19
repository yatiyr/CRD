#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/json_writer.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

using crd::hesap::cli::JsonWriter;

TEST_CASE("JsonWriter emits a flat object compactly", "[hesap][cli][json]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    JsonWriter w{&alloc};
    w.begin_object();
    w.key_value("name", crd::containers::StringView{"hesap.test.echo"});
    w.key_value("ok", true);
    w.end_object();
    const crd::containers::String& s = w.str();
    REQUIRE(std::strcmp(s.c_str(), R"({"name":"hesap.test.echo","ok":true})") == 0);
}

TEST_CASE("JsonWriter escapes special chars in strings", "[hesap][cli][json]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    JsonWriter w{&alloc};
    w.value_string(crd::containers::StringView{"a\\b\"c\nd"});
    REQUIRE(std::strcmp(w.str().c_str(), R"("a\\b\"c\nd")") == 0);
}

TEST_CASE("JsonWriter nests arrays inside objects correctly", "[hesap][cli][json]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    JsonWriter w{&alloc};
    w.begin_object();
    w.key("items");
    w.begin_array();
    w.value_u32(1);
    w.value_u32(2);
    w.value_u32(3);
    w.end_array();
    w.end_object();
    REQUIRE(std::strcmp(w.str().c_str(), R"({"items":[1,2,3]})") == 0);
}
