#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::hesap::dense::asum;
using crd::hesap::dense::axpy;
using crd::hesap::dense::copy;
using crd::hesap::dense::dot;
using crd::hesap::dense::iamax;
using crd::hesap::dense::nrm2;
using crd::hesap::dense::scal;
using crd::hesap::dense::swap;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinRel;

namespace
{
template <typename T>
Vector<T> make_vec(crd::memory::IAllocator* alloc, std::initializer_list<T> il)
{
    return Vector<T>(alloc, il);
}
} // namespace

// ---- axpy --------------------------------------------------------------

TEST_CASE("axpy real: alpha=0 is identity on y", "[hesap][blas1][real][axpy]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, 2.0, 3.0});
    auto y = make_vec<crd::f64>(&alloc, {10.0, 20.0, 30.0});
    axpy<crd::f64>(0.0, x, y);
    REQUIRE(y(0) == 10.0);
    REQUIRE(y(1) == 20.0);
    REQUIRE(y(2) == 30.0);
}

TEST_CASE("axpy real: alpha=1 adds x to y", "[hesap][blas1][real][axpy]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, 2.0, 3.0});
    auto y = make_vec<crd::f64>(&alloc, {10.0, 20.0, 30.0});
    axpy<crd::f64>(1.0, x, y);
    REQUIRE(y(0) == 11.0);
    REQUIRE(y(1) == 22.0);
    REQUIRE(y(2) == 33.0);
}

TEST_CASE("axpy real: general alpha", "[hesap][blas1][real][axpy]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f32>(&alloc, {1.0F, 2.0F, 3.0F});
    auto y = make_vec<crd::f32>(&alloc, {0.0F, 0.0F, 0.0F});
    axpy<crd::f32>(2.5F, x, y);
    REQUIRE(y(0) == 2.5F);
    REQUIRE(y(1) == 5.0F);
    REQUIRE(y(2) == 7.5F);
}

// ---- dot ---------------------------------------------------------------

TEST_CASE("dot real: orthogonal vectors give zero", "[hesap][blas1][real][dot]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, 0.0});
    auto y = make_vec<crd::f64>(&alloc, {0.0, 1.0});
    REQUIRE(dot<crd::f64>(x, y) == 0.0);
}

TEST_CASE("dot real: standard textbook example", "[hesap][blas1][real][dot]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, 2.0, 3.0, 4.0});
    auto y = make_vec<crd::f64>(&alloc, {5.0, 6.0, 7.0, 8.0});
    // 5 + 12 + 21 + 32 = 70
    REQUIRE(dot<crd::f64>(x, y) == 70.0);
}

TEST_CASE("dot real: large N with KBN compensation", "[hesap][blas1][real][dot]")
{
    crd::memory::TlsfAllocator alloc(2 * 1024 * 1024);
    constexpr crd::usize kN = 100000;
    Vector<crd::f64> x(&alloc, kN);
    Vector<crd::f64> y(&alloc, kN);
    x.fill(1.0);
    y.fill(1.0);
    const auto d = dot<crd::f64>(x, y);
    REQUIRE(d == static_cast<crd::f64>(kN));
}

// ---- nrm2 --------------------------------------------------------------

TEST_CASE("nrm2 real: zero vector has zero norm", "[hesap][blas1][real][nrm2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> x(&alloc, 100);
    REQUIRE(nrm2<crd::f64>(x) == 0.0);
}

TEST_CASE("nrm2 real: all-ones gives sqrt(N)", "[hesap][blas1][real][nrm2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    constexpr crd::usize kN = 64;
    Vector<crd::f64> x(&alloc, kN);
    x.fill(1.0);
    REQUIRE_THAT(nrm2<crd::f64>(x), WithinRel(std::sqrt(static_cast<crd::f64>(kN)), 1e-15));
}

TEST_CASE("nrm2 real: 3-4-5 right triangle", "[hesap][blas1][real][nrm2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {3.0, 4.0});
    REQUIRE(nrm2<crd::f64>(x) == 5.0);
}

// ---- scal --------------------------------------------------------------

TEST_CASE("scal real: alpha=0 zeros the vector", "[hesap][blas1][real][scal]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, 2.0, 3.0});
    scal<crd::f64>(0.0, x);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        REQUIRE(x(i) == 0.0);
    }
}

TEST_CASE("scal real: alpha=2 doubles", "[hesap][blas1][real][scal]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, 2.0, 3.0});
    scal<crd::f64>(2.0, x);
    REQUIRE(x(0) == 2.0);
    REQUIRE(x(1) == 4.0);
    REQUIRE(x(2) == 6.0);
}

// ---- copy --------------------------------------------------------------

TEST_CASE("copy real: copies element-wise", "[hesap][blas1][real][copy]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto src = make_vec<crd::f64>(&alloc, {1.0, 2.0, 3.0});
    Vector<crd::f64> dst(&alloc, 3);
    copy<crd::f64>(src, dst);
    REQUIRE(dst(0) == 1.0);
    REQUIRE(dst(1) == 2.0);
    REQUIRE(dst(2) == 3.0);
}

// ---- swap --------------------------------------------------------------

TEST_CASE("swap real: exchanges element-wise", "[hesap][blas1][real][swap]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto a = make_vec<crd::f64>(&alloc, {1.0, 2.0});
    auto b = make_vec<crd::f64>(&alloc, {10.0, 20.0});
    swap<crd::f64>(a, b);
    REQUIRE(a(0) == 10.0);
    REQUIRE(a(1) == 20.0);
    REQUIRE(b(0) == 1.0);
    REQUIRE(b(1) == 2.0);
}

// ---- asum --------------------------------------------------------------

TEST_CASE("asum real: zero vector gives zero", "[hesap][blas1][real][asum]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f64> x(&alloc, 50);
    REQUIRE(asum<crd::f64>(x) == 0.0);
}

TEST_CASE("asum real: signed values use magnitudes", "[hesap][blas1][real][asum]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {-1.0, 2.0, -3.0, 4.0});
    REQUIRE(asum<crd::f64>(x) == 10.0);
}

// ---- iamax -------------------------------------------------------------

TEST_CASE("iamax real: returns argmax of magnitudes", "[hesap][blas1][real][iamax]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {1.0, -5.0, 3.0, -2.0});
    REQUIRE(iamax<crd::f64>(x) == 1);
}

TEST_CASE("iamax real: ties broken by FIRST index", "[hesap][blas1][real][iamax]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {3.0, -3.0, 3.0});
    REQUIRE(iamax<crd::f64>(x) == 0);
}

TEST_CASE("iamax real: single-element vector returns 0", "[hesap][blas1][real][iamax]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto x = make_vec<crd::f64>(&alloc, {-7.0});
    REQUIRE(iamax<crd::f64>(x) == 0);
}

// ---- f32 quick coverage (mirrors f64) ----------------------------------

TEST_CASE("f32: full op set works on a 16-element vector", "[hesap][blas1][real][f32]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<crd::f32> x(&alloc, 16);
    Vector<crd::f32> y(&alloc, 16);
    for (crd::usize i = 0; i < 16; ++i)
    {
        x(i) = static_cast<crd::f32>(i + 1);
        y(i) = 0.0F;
    }
    axpy<crd::f32>(0.5F, x, y);
    REQUIRE(y(5) == 3.0F);

    const crd::f32 d = dot<crd::f32>(x, x);
    REQUIRE(d == 1496.0F);  // 1^2 + 2^2 + ... + 16^2 = 1496

    scal<crd::f32>(2.0F, y);
    REQUIRE(y(0) == 1.0F);

    REQUIRE(iamax<crd::f32>(x) == 15);  // x[15] = 16, the max
}

// ---- pairwise tree size boundaries -------------------------------------

TEST_CASE("pairwise tree handles sizes around leaf boundary", "[hesap][blas1][real][pairwise]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    for (crd::usize n : {crd::usize{1}, crd::usize{7}, crd::usize{8}, crd::usize{9}, crd::usize{15},
                         crd::usize{16}, crd::usize{17}, crd::usize{63}, crd::usize{64}, crd::usize{65}})
    {
        Vector<crd::f64> x(&alloc, n);
        x.fill(1.0);
        const auto s = dot<crd::f64>(x, x);
        REQUIRE(s == static_cast<crd::f64>(n));
    }
}

// ---- determinism: same input -> same output ----------------------------

TEST_CASE("determinism: dot of fixed input is bit-stable across calls", "[hesap][blas1][real][det]")
{
    crd::memory::TlsfAllocator alloc(256 * 1024);
    constexpr crd::usize kN = 5000;
    Vector<crd::f64> x(&alloc, kN);
    Vector<crd::f64> y(&alloc, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x(i) = std::sin(static_cast<crd::f64>(i));
        y(i) = std::cos(static_cast<crd::f64>(i));
    }
    const auto d1 = dot<crd::f64>(x, y);
    const auto d2 = dot<crd::f64>(x, y);
    REQUIRE(d1 == d2);
    const auto n1 = nrm2<crd::f64>(x);
    const auto n2 = nrm2<crd::f64>(x);
    REQUIRE(n1 == n2);
}
