#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::Complex64;
using crd::hesap::dense::Banded;
using crd::hesap::dense::Hermitian;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;

TEST_CASE("Matrix sized ctor zero-inits", "[hesap][dense][matrix]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 3, 4);
    REQUIRE(a.rows() == 3);
    REQUIRE(a.cols() == 4);
    REQUIRE(a.size() == 12);
    REQUIRE(a.ld() == 4);  // RowMajor: ld == cols
    for (crd::usize i = 0; i < 3; ++i)
    {
        for (crd::usize j = 0; j < 4; ++j)
        {
            REQUIRE(a(i, j) == 0.0);
        }
    }
}

TEST_CASE("Matrix RowMajor vs ColMajor indexing", "[hesap][dense][matrix]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64, Layout::RowMajor> r(&alloc, 2, 3,
                                          {1, 2, 3, 4, 5, 6});
    REQUIRE(r(0, 0) == 1.0);
    REQUIRE(r(0, 1) == 2.0);
    REQUIRE(r(0, 2) == 3.0);
    REQUIRE(r(1, 0) == 4.0);
    REQUIRE(r(1, 1) == 5.0);
    REQUIRE(r.ld() == 3);

    Matrix<crd::f64, Layout::ColMajor> c(&alloc, 2, 3,
                                          {1, 2, 3, 4, 5, 6});  // stored col-major
    // Col-major raw [1,2,3,4,5,6] means col 0 = [1,2], col 1 = [3,4], col 2 = [5,6]
    REQUIRE(c(0, 0) == 1.0);
    REQUIRE(c(1, 0) == 2.0);
    REQUIRE(c(0, 1) == 3.0);
    REQUIRE(c(1, 1) == 4.0);
    REQUIRE(c.ld() == 2);  // ColMajor: ld == rows
}

TEST_CASE("Matrix set_identity zeros off-diagonal", "[hesap][dense][matrix]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> i(&alloc, 4, 4);
    i.set_identity();
    for (crd::usize r = 0; r < 4; ++r)
    {
        for (crd::usize c = 0; c < 4; ++c)
        {
            REQUIRE(i(r, c) == (r == c ? 1.0 : 0.0));
        }
    }
}

TEST_CASE("Matrix clone is deep", "[hesap][dense][matrix]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 2, {1, 2, 3, 4});
    auto b = a.clone();
    REQUIRE(b(0, 0) == 1.0);
    b(0, 0) = 99.0;
    REQUIRE(a(0, 0) == 1.0);
}

TEST_CASE("MatrixView sub_view preserves stride", "[hesap][dense][matrix]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 4, 4);
    for (crd::usize i = 0; i < 4; ++i)
    {
        for (crd::usize j = 0; j < 4; ++j)
        {
            a(i, j) = static_cast<crd::f64>(i * 4 + j);
        }
    }
    auto v = a.view().sub_view(1, 1, 2, 2);
    REQUIRE(v.ld() == 4);  // sub_view preserves parent ld
    REQUIRE(v(0, 0) == 5.0);   // a(1, 1)
    REQUIRE(v(0, 1) == 6.0);   // a(1, 2)
    REQUIRE(v(1, 0) == 9.0);   // a(2, 1)
    REQUIRE(v(1, 1) == 10.0);  // a(2, 2)
}

TEST_CASE("Symmetric access is symmetric", "[hesap][dense][matrix_types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Symmetric<crd::f64> s(&alloc, 3);
    s.at(0, 0) = 1.0;
    s.at(1, 0) = 2.0;
    s.at(1, 1) = 3.0;
    s.at(2, 0) = 4.0;
    s.at(2, 1) = 5.0;
    s.at(2, 2) = 6.0;
    REQUIRE(s.at(0, 1) == 2.0);  // mirrored from (1, 0)
    REQUIRE(s.at(0, 2) == 4.0);
    REQUIRE(s.at(1, 2) == 5.0);
}

TEST_CASE("Hermitian upper half returns conj of lower", "[hesap][dense][matrix_types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Hermitian<Complex64> h(&alloc, 2);
    h.at_lower(0, 0) = Complex64{1.0, 0.0};  // real diagonal
    h.at_lower(1, 0) = Complex64{2.0, 3.0};
    h.at_lower(1, 1) = Complex64{4.0, 0.0};
    const auto upper = h.at_value(0, 1);
    REQUIRE(upper.re == 2.0);
    REQUIRE(upper.im == -3.0);  // conj of (2, 3)
}

TEST_CASE("Triangular Lower at_value returns 0 above diagonal", "[hesap][dense][matrix_types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> t(&alloc, 3);
    t.at(0, 0) = 1.0;
    t.at(1, 0) = 2.0;
    t.at(1, 1) = 3.0;
    t.at(2, 1) = 4.0;
    REQUIRE(t.at_value(0, 0) == 1.0);
    REQUIRE(t.at_value(2, 1) == 4.0);
    REQUIRE(t.at_value(0, 1) == 0.0);  // above diagonal
    REQUIRE(t.at_value(1, 2) == 0.0);
}

TEST_CASE("Triangular UnitDiag returns 1 for diagonal", "[hesap][dense][matrix_types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::UnitDiag> u(&alloc, 3);
    u.at(0, 1) = 7.0;
    u.at(1, 2) = 8.0;
    REQUIRE(u.at_value(0, 0) == 1.0);
    REQUIRE(u.at_value(1, 1) == 1.0);
    REQUIRE(u.at_value(2, 2) == 1.0);
    REQUIRE(u.at_value(0, 1) == 7.0);
    REQUIRE(u.at_value(0, 2) == 0.0);  // not stored, returns 0 (UnitDiag policy)
}

TEST_CASE("Banded in_band predicate", "[hesap][dense][matrix_types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Banded<crd::f64> b(&alloc, 5, 5, /*kl=*/1, /*ku=*/1);  // tridiagonal
    REQUIRE(b.in_band(0, 0));
    REQUIRE(b.in_band(0, 1));
    REQUIRE_FALSE(b.in_band(0, 2));
    REQUIRE(b.in_band(2, 1));
    REQUIRE(b.in_band(2, 2));
    REQUIRE(b.in_band(2, 3));
    REQUIRE_FALSE(b.in_band(2, 4));
    REQUIRE_FALSE(b.in_band(3, 0));
}

TEST_CASE("Banded at round-trip", "[hesap][dense][matrix_types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Banded<crd::f64> b(&alloc, 4, 4, 1, 1);
    // Set tridiagonal entries
    for (crd::usize i = 0; i < 4; ++i)
    {
        b.at(i, i) = 10.0 + static_cast<crd::f64>(i);  // main diag
        if (i > 0)
        {
            b.at(i, i - 1) = static_cast<crd::f64>(i);  // sub-diag
        }
        if (i + 1 < 4)
        {
            b.at(i, i + 1) = -static_cast<crd::f64>(i + 1);  // super-diag
        }
    }
    REQUIRE(b.at_value(0, 0) == 10.0);
    REQUIRE(b.at_value(1, 0) == 1.0);
    REQUIRE(b.at_value(0, 1) == -1.0);
    REQUIRE(b.at_value(3, 3) == 13.0);
    REQUIRE(b.at_value(0, 2) == 0.0);  // outside band
}
