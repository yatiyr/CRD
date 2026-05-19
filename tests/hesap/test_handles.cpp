#include <catch2/catch_test_macros.hpp>

#include <crd/containers/hash_map.hpp>
#include <crd/hesap/handles.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <type_traits>

using crd::hesap::MatrixId;
using crd::hesap::VectorId;

TEST_CASE("MatrixId and VectorId are POD-shaped", "[hesap][handles]")
{
    STATIC_REQUIRE(sizeof(MatrixId) == 8);
    STATIC_REQUIRE(sizeof(VectorId) == 8);
    STATIC_REQUIRE(std::is_trivially_copyable_v<MatrixId>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<VectorId>);
    STATIC_REQUIRE(std::is_standard_layout_v<MatrixId>);
}

TEST_CASE("Handle null sentinel and make/decompose round-trip", "[hesap][handles]")
{
    const MatrixId null = MatrixId::null();
    REQUIRE(null.is_null());
    REQUIRE(null.raw == 0);

    const MatrixId h = MatrixId::make(0x1234, 0x5);
    REQUIRE_FALSE(h.is_null());
    REQUIRE(h.index() == 0x1234);
    REQUIRE(h.generation() == 0x5);

    REQUIRE_FALSE(h == null);

    const VectorId v = VectorId::make(7, 1);
    REQUIRE(v.index() == 7);
    REQUIRE(v.generation() == 1);
}

TEST_CASE("Handles work as HashMap keys", "[hesap][handles]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    crd::containers::HashMap<MatrixId, int> m(&alloc);

    const MatrixId a = MatrixId::make(1, 1);
    const MatrixId b = MatrixId::make(2, 1);
    m.insert(a, 100);
    m.insert(b, 200);

    REQUIRE(m.contains(a));
    REQUIRE(m.contains(b));
    REQUIRE_FALSE(m.contains(MatrixId::make(3, 1)));

    const int* va = m.find(a);
    REQUIRE(va != nullptr);
    REQUIRE(*va == 100);
}
