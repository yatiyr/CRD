#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_catalog.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::dense::Banded;
using crd::hesap::dense::BlockDiagonal;
using crd::hesap::dense::BlockTridiagonal;
using crd::hesap::dense::Circulant;
using crd::hesap::dense::Diagonal;
using crd::hesap::dense::Hankel;
using crd::hesap::dense::Hermitian;
using crd::hesap::dense::Identity;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::MatrixView;
using crd::hesap::dense::Permutation;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::Toeplitz;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::Vandermonde;

TEST_CASE("Matrix and MatrixView default to zero size and propagate layout", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<double, Layout::RowMajor> m{&alloc};
    REQUIRE(m.rows() == 0);
    REQUIRE(m.cols() == 0);
    REQUIRE(m.size() == 0);
    REQUIRE(m.is_square());
    REQUIRE(m.allocator() == &alloc);
    STATIC_REQUIRE(decltype(m)::layout == Layout::RowMajor);

    MatrixView<float, Layout::ColMajor> v;
    REQUIRE(v.rows() == 0);
    REQUIRE(v.cols() == 0);
    REQUIRE(v.ld() == 0);
    STATIC_REQUIRE(decltype(v)::layout == Layout::ColMajor);
}

TEST_CASE("Diagonal and Identity are square by construction", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Diagonal<double> d{&alloc};
    REQUIRE(d.is_square());
    REQUIRE(d.n() == 0);

    constexpr Identity<float> kIdentitySeven(7);
    STATIC_REQUIRE(kIdentitySeven.is_square());
    STATIC_REQUIRE(kIdentitySeven.n() == 7);
    STATIC_REQUIRE(kIdentitySeven.rows() == 7);
}

TEST_CASE("Permutation and Triangular carry the allocator", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Permutation p{&alloc};
    REQUIRE(p.is_square());

    // v0c Triangular body keeps the alloc-only ctor.
    Triangular<double, TriangularSide::Upper, TriangularDiag::UnitDiag> t{&alloc};
    REQUIRE(t.is_square());
    STATIC_REQUIRE(decltype(t)::side == TriangularSide::Upper);
    STATIC_REQUIRE(decltype(t)::diag == TriangularDiag::UnitDiag);
}

TEST_CASE("Symmetric and Hermitian instantiate for real and complex", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Symmetric<double> s{&alloc};
    Hermitian<double> h{&alloc};
    REQUIRE(s.is_square());
    REQUIRE(h.is_square());
}

TEST_CASE("Banded carries kl and ku bandwidths", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    // v0c Banded body takes (alloc, rows, cols, kl, ku).
    Banded<double> b{&alloc, 5, 5, 2, 1};
    REQUIRE(b.kl() == 2);
    REQUIRE(b.ku() == 1);
    REQUIRE(b.rows() == 5);
    REQUIRE(b.cols() == 5);
}

TEST_CASE("BlockDiagonal and BlockTridiagonal instantiate", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    BlockDiagonal<double> bd{&alloc};
    BlockTridiagonal<double> bt{&alloc};
    REQUIRE(bd.num_blocks() == 0);
    REQUIRE(bt.num_blocks() == 0);
}

TEST_CASE("Structured matrices (Toeplitz/Hankel/Circulant/Vandermonde) instantiate", "[hesap][dense][catalog]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Toeplitz<double> t{&alloc};
    Hankel<double> h{&alloc};
    Circulant<double> c{&alloc};
    Vandermonde<double> v{&alloc};
    REQUIRE(t.rows() == 0);
    REQUIRE(h.rows() == 0);
    REQUIRE(c.is_square());
    REQUIRE(v.is_square());
}

TEST_CASE("Catalog types are sizes >= sizeof allocator pointer", "[hesap][dense][catalog]")
{
    // Sanity: every shell carries at least an IAllocator* (or constexpr count for Identity).
    STATIC_REQUIRE(sizeof(Identity<float>) >= sizeof(crd::usize));
}
