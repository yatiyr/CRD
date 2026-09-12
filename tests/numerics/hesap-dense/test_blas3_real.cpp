#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::dense::gemm;
using crd::hesap::dense::gemm_mixed;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::syrk;
using crd::hesap::dense::syr2k;
using crd::hesap::dense::Trans;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::trmm;
using crd::hesap::dense::trsm;
using Catch::Matchers::WithinAbs;

TEST_CASE("gemm: 2x3 * 3x2 identity-alpha textbook", "[hesap][blas3][real][gemm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 3, {1, 2, 3, 4, 5, 6});
    Matrix<crd::f64> b(&alloc, 3, 2, {7, 8, 9, 10, 11, 12});
    Matrix<crd::f64> c(&alloc, 2, 2);
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c);
    // c[0,0] = 1*7+2*9+3*11 = 58, c[0,1] = 1*8+2*10+3*12 = 64
    // c[1,0] = 4*7+5*9+6*11 = 139, c[1,1] = 4*8+5*10+6*12 = 154
    REQUIRE(c(0, 0) == 58.0);
    REQUIRE(c(0, 1) == 64.0);
    REQUIRE(c(1, 0) == 139.0);
    REQUIRE(c(1, 1) == 154.0);
}

TEST_CASE("gemm: beta scales existing C", "[hesap][blas3][real][gemm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 2, {1, 0, 0, 1});  // identity
    Matrix<crd::f64> b(&alloc, 2, 2, {2, 3, 4, 5});
    Matrix<crd::f64> c(&alloc, 2, 2, {10, 20, 30, 40});
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 2.0, c);
    // C = I*B + 2*C = B + 2*C = {2+20, 3+40, 4+60, 5+80}
    REQUIRE(c(0, 0) == 22.0);
    REQUIRE(c(0, 1) == 43.0);
    REQUIRE(c(1, 0) == 64.0);
    REQUIRE(c(1, 1) == 85.0);
}

TEST_CASE("gemm vs naive triple-loop at N=64 (AVX2 boundary)", "[hesap][blas3][real][gemm]")
{
    crd::memory::TlsfAllocator alloc(4 * 1024 * 1024);
    constexpr crd::usize n = 64;
    Matrix<crd::f32> a(&alloc, n, n);
    Matrix<crd::f32> b(&alloc, n, n);
    Matrix<crd::f32> c(&alloc, n, n);
    Matrix<crd::f32> c_naive(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a(i, j) = static_cast<crd::f32>(i + 1) * 0.01F + static_cast<crd::f32>(j) * 0.002F;
            b(i, j) = static_cast<crd::f32>(j + 1) * 0.03F - static_cast<crd::f32>(i) * 0.005F;
        }
    }
    gemm<crd::f32, Layout::RowMajor>(1.0F, a, b, 0.0F, c);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            crd::f32 s = 0.0F;
            for (crd::usize p = 0; p < n; ++p)
            {
                s += a(i, p) * b(p, j);
            }
            c_naive(i, j) = s;
        }
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            REQUIRE_THAT(c(i, j), WithinAbs(c_naive(i, j), 1e-3));
        }
    }
}

TEST_CASE("gemm Transpose flag conjugates index pattern", "[hesap][blas3][real][gemm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 3, {1, 2, 3, 4, 5, 6});
    Matrix<crd::f64> b(&alloc, 2, 3, {1, 0, 0, 0, 1, 0});  // selects col 0, then col 1
    Matrix<crd::f64> c(&alloc, 3, 3);
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c, Trans::Transpose, Trans::None);
    // (A^T * B)_ij = sum_k A[k,i] * B[k,j]
    // A^T = [[1,4],[2,5],[3,6]]; B = [[1,0,0],[0,1,0]]
    // c[0,0]=1*1+4*0=1, c[0,1]=1*0+4*1=4, c[0,2]=0
    // c[1,0]=2, c[1,1]=5, c[1,2]=0
    // c[2,0]=3, c[2,1]=6, c[2,2]=0
    REQUIRE(c(0, 0) == 1.0);
    REQUIRE(c(0, 1) == 4.0);
    REQUIRE(c(1, 0) == 2.0);
    REQUIRE(c(1, 1) == 5.0);
    REQUIRE(c(2, 0) == 3.0);
    REQUIRE(c(2, 1) == 6.0);
}

TEST_CASE("syrk: A * A^T (real)", "[hesap][blas3][real][syrk]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 3, 2, {1, 2, 3, 4, 5, 6});
    Symmetric<crd::f64> c(&alloc, 3);
    syrk<crd::f64>(1.0, a.cview(), 0.0, c, Trans::None);
    // A * A^T: row i dot row j
    // row0=[1,2] row1=[3,4] row2=[5,6]
    // c[0,0]=1+4=5, c[1,1]=9+16=25, c[2,2]=25+36=61
    // c[1,0]=3+8=11, c[2,0]=5+12=17, c[2,1]=15+24=39
    REQUIRE(c.at(0, 0) == 5.0);
    REQUIRE(c.at(1, 0) == 11.0);
    REQUIRE(c.at(1, 1) == 25.0);
    REQUIRE(c.at(2, 0) == 17.0);
    REQUIRE(c.at(2, 1) == 39.0);
    REQUIRE(c.at(2, 2) == 61.0);
}

TEST_CASE("syr2k: alpha*(A*B^T + B*A^T)", "[hesap][blas3][real][syr2k]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 2, {1, 2, 3, 4});
    Matrix<crd::f64> b(&alloc, 2, 2, {5, 6, 7, 8});
    Symmetric<crd::f64> c(&alloc, 2);
    syr2k<crd::f64>(1.0, a.cview(), b.cview(), 0.0, c, Trans::None);
    // A*B^T: row0(A) . row0(B) = 1*5+2*6=17; row0(A).row1(B)=1*7+2*8=23
    //        row1(A).row0(B)=3*5+4*6=39; row1(A).row1(B)=3*7+4*8=53
    // B*A^T: row0(B).row0(A)=5+12=17; row0(B).row1(A)=5*3+6*4=39
    //        row1(B).row0(A)=7+16=23; row1(B).row1(A)=7*3+8*4=53
    // c[0,0] = 17+17 = 34; c[1,0] = 39 + 23 = 62; c[1,1] = 53 + 53 = 106
    REQUIRE(c.at(0, 0) == 34.0);
    REQUIRE(c.at(1, 0) == 62.0);
    REQUIRE(c.at(1, 1) == 106.0);
}

TEST_CASE("trmm Lower: B = L*B in-place", "[hesap][blas3][real][trmm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, 3);
    l.at(0, 0) = 1.0;
    l.at(1, 0) = 2.0;
    l.at(1, 1) = 3.0;
    l.at(2, 0) = 4.0;
    l.at(2, 1) = 5.0;
    l.at(2, 2) = 6.0;
    Matrix<crd::f64> b(&alloc, 3, 2, {1, 0, 0, 1, 0, 0});  // first col = e1, second = e2
    trmm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(1.0, l, b.view(), Trans::None);
    // L * [e1 e2] = [L_col0 L_col1] = first col of L, second col of L
    // L = [[1,0,0],[2,3,0],[4,5,6]]; col0 = [1,2,4]; col1 = [0,3,5]
    REQUIRE(b(0, 0) == 1.0);
    REQUIRE(b(1, 0) == 2.0);
    REQUIRE(b(2, 0) == 4.0);
    REQUIRE(b(0, 1) == 0.0);
    REQUIRE(b(1, 1) == 3.0);
    REQUIRE(b(2, 1) == 5.0);
}

TEST_CASE("trsm Lower: B = L^-1 * B in-place", "[hesap][blas3][real][trsm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, 3);
    l.at(0, 0) = 2.0;
    l.at(1, 0) = 1.0;
    l.at(1, 1) = 4.0;
    l.at(2, 0) = 3.0;
    l.at(2, 1) = 2.0;
    l.at(2, 2) = 5.0;
    // L * X = B where X is a known matrix. Set X = I, so B = L.
    // Verify trsm recovers X = I.
    Matrix<crd::f64> b(&alloc, 3, 3);
    for (crd::usize i = 0; i < 3; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            b(i, j) = l.at(i, j);
        }
    }
    trsm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(1.0, l, b.view(), Trans::None);
    for (crd::usize i = 0; i < 3; ++i)
    {
        for (crd::usize j = 0; j < 3; ++j)
        {
            REQUIRE_THAT(b(i, j), WithinAbs(i == j ? 1.0 : 0.0, 1e-12));
        }
    }
}

TEST_CASE("trsm + trmm round-trip preserves B", "[hesap][blas3][real][trmm][trsm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    constexpr crd::usize n = 6;
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            l.at(i, j) = (i == j) ? 4.0 : 0.3;
        }
    }
    Matrix<crd::f64> b_initial(&alloc, n, 3);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < 3; ++j)
        {
            b_initial(i, j) = static_cast<crd::f64>(i + 1) * static_cast<crd::f64>(j + 1) - 5.0;
        }
    }
    auto b = b_initial.clone();
    trmm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(1.0, l, b.view(), Trans::None);
    trsm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(1.0, l, b.view(), Trans::None);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < 3; ++j)
        {
            REQUIRE_THAT(b(i, j), WithinAbs(b_initial(i, j), 1e-11));
        }
    }
}

TEST_CASE("gemm_mixed: f32 input + f64 accumulator", "[hesap][blas3][real][mixed]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f32> a(&alloc, 2, 2, {1.0F, 2.0F, 3.0F, 4.0F});
    Matrix<crd::f32> b(&alloc, 2, 2, {5.0F, 6.0F, 7.0F, 8.0F});
    Matrix<crd::f64> c(&alloc, 2, 2);
    gemm_mixed<crd::f32, crd::f64, Layout::RowMajor>(1.0, a.cview(), b.cview(), 0.0, c.view());
    REQUIRE(c(0, 0) == 19.0);
    REQUIRE(c(0, 1) == 22.0);
    REQUIRE(c(1, 0) == 43.0);
    REQUIRE(c(1, 1) == 50.0);
}
