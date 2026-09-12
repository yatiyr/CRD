#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::cli::ArgValue;

TEST_CASE("ArgValue default is Empty", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    REQUIRE(v.kind() == ArgValue::Kind::Empty);
    REQUIRE_FALSE(v.as_f64().has_value());
    REQUIRE(v.as_string().empty());
    REQUIRE(v.as_f64_array().empty());
}

TEST_CASE("ArgValue set/get f64 round-trip", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    v.set_f64(3.14);
    REQUIRE(v.kind() == ArgValue::Kind::F64);
    const auto got = v.as_f64();
    REQUIRE(got.has_value());
    REQUIRE(*got == 3.14);
    REQUIRE_FALSE(v.as_i64().has_value());
}

TEST_CASE("ArgValue set/get i64 / u64 / bool", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    v.set_i64(-42);
    REQUIRE(v.as_i64().value() == -42);
    v.set_u64(99999999999ULL);
    REQUIRE(v.as_u64().value() == 99999999999ULL);
    v.set_bool(true);
    REQUIRE(v.as_bool().value() == true);
}

TEST_CASE("ArgValue set/get complex64", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    v.set_complex64(crd::hesap::Complex64{1.5, -2.25});
    const auto z = v.as_complex64();
    REQUIRE(z.has_value());
    REQUIRE(z->re == 1.5);
    REQUIRE(z->im == -2.25);
}

TEST_CASE("ArgValue set/get string with allocator-owned storage", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    v.set_string(crd::containers::StringView{"hello hesap"});
    REQUIRE(v.kind() == ArgValue::Kind::String);
    REQUIRE(v.as_string() == crd::containers::StringView{"hello hesap"});
}

TEST_CASE("ArgValue set/get f64 array", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    const crd::f64 raw[] = {1.0, 2.0, 3.0, 4.0};
    v.set_f64_array(crd::containers::ConstSpan<crd::f64>{raw, 4});
    const auto a = v.as_f64_array();
    REQUIRE(a.size() == 4);
    REQUIRE(a[0] == 1.0);
    REQUIRE(a[3] == 4.0);
}

TEST_CASE("ArgValue set/get i64 array", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    const crd::i64 raw[] = {-1, 2, -3};
    v.set_i64_array(crd::containers::ConstSpan<crd::i64>{raw, 3});
    const auto a = v.as_i64_array();
    REQUIRE(a.size() == 3);
    REQUIRE(a[2] == -3);
}

TEST_CASE("ArgValue set/get MatrixId / VectorId", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    v.set_matrix_id(crd::hesap::MatrixId::make(7, 1));
    REQUIRE(v.as_matrix_id().value().index() == 7);
    v.set_vector_id(crd::hesap::VectorId::make(42, 3));
    REQUIRE(v.as_vector_id().value().generation() == 3);
}

TEST_CASE("ArgValue setters mutate kind, getters mismatch returns empty", "[hesap][cli][arg_value]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    ArgValue v{&alloc};
    v.set_f64(2.5);
    REQUIRE(v.kind() == ArgValue::Kind::F64);
    REQUIRE_FALSE(v.as_i64().has_value());
    REQUIRE_FALSE(v.as_bool().has_value());
    REQUIRE(v.as_string().empty());
    REQUIRE(v.as_f64_array().empty());
    // Mutate to a different kind.
    v.set_string(crd::containers::StringView{"now a string"});
    REQUIRE(v.kind() == ArgValue::Kind::String);
    REQUIRE_FALSE(v.as_f64().has_value());
}
