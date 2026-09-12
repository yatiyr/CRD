#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <utility>

using crd::hesap::Complex32;
using crd::hesap::Complex64;
using crd::hesap::dense::Vector;

TEST_CASE("Vector default-construct is empty", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> v(&alloc);
    REQUIRE(v.size() == 0);
    REQUIRE(v.empty());
    REQUIRE(v.data() == nullptr);
}

TEST_CASE("Vector sized-construct zero-initialises", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> v(&alloc, 10);
    REQUIRE(v.size() == 10);
    for (crd::usize i = 0; i < 10; ++i)
    {
        REQUIRE(v(i) == 0.0);
    }
}

TEST_CASE("Vector initializer_list ctor", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> v(&alloc, {1.0, 2.0, 3.0, 4.0});
    REQUIRE(v.size() == 4);
    REQUIRE(v(0) == 1.0);
    REQUIRE(v(3) == 4.0);
}

TEST_CASE("Vector span ctor", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    const crd::f64 raw[] = {1.5, 2.5, 3.5};
    Vector<crd::f64> v(&alloc, crd::containers::ConstSpan<crd::f64>{raw, 3});
    REQUIRE(v.size() == 3);
    REQUIRE(v(1) == 2.5);
}

TEST_CASE("Vector move ctor transfers ownership", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> a(&alloc, 5);
    a.fill(7.0);
    const auto* original_data = a.data();

    Vector<crd::f64> b = std::move(a);
    REQUIRE(b.size() == 5);
    REQUIRE(b.data() == original_data);
    REQUIRE(b(2) == 7.0);
    REQUIRE(a.size() == 0);
    REQUIRE(a.data() == nullptr);
}

TEST_CASE("Vector move assign releases existing storage", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> a(&alloc, 5);
    a.fill(1.0);
    Vector<crd::f64> b(&alloc, 3);
    b.fill(2.0);
    b = std::move(a);
    REQUIRE(b.size() == 5);
    REQUIRE(b(0) == 1.0);
}

TEST_CASE("Vector clone is a deep copy", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> a(&alloc, {1.0, 2.0, 3.0});
    Vector<crd::f64> b = a.clone();
    REQUIRE(b.size() == 3);
    REQUIRE(b.data() != a.data());
    REQUIRE(b(1) == 2.0);
    b(1) = 99.0;
    REQUIRE(a(1) == 2.0);  // a not affected
}

TEST_CASE("Vector span() returns the right view", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> v(&alloc, {10.0, 20.0});
    const auto s = v.span();
    REQUIRE(s.size() == 2);
    REQUIRE(s[1] == 20.0);
}

TEST_CASE("Vector<Complex64> stores complex elements", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> v(&alloc, 3);
    v(0) = Complex64{1.0, 2.0};
    v(1) = Complex64{3.0, 4.0};
    v(2) = Complex64{5.0, 6.0};
    REQUIRE(v(1).re == 3.0);
    REQUIRE(v(1).im == 4.0);
}

TEST_CASE("Vector fill sets every element", "[hesap][dense][vector]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f32> v(&alloc, 100);
    v.fill(3.5F);
    for (crd::usize i = 0; i < 100; ++i)
    {
        REQUIRE(v(i) == 3.5F);
    }
}
