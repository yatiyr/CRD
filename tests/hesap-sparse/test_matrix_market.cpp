// crd-hesap-sparse v1g-1 -- Matrix Market (.mtx) I/O.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using Catch::Approx;
using crd::hesap::Complex64;
namespace sp = crd::hesap::sparse;

namespace
{
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> small_csr(crd::memory::IAllocator* alloc)
{
    sp::TripletBuilder<crd::f64> tb(alloc, 4, 5);
    tb.add(0, 1, 2.5);
    tb.add(0, 4, -1.0);
    tb.add(2, 0, 3.25);
    tb.add(3, 3, 7.0);
    tb.add(1, 2, 0.5);
    return tb.compress();
}

crd::containers::StringView sv(const crd::containers::String& s)
{
    return crd::containers::StringView{s.data(), s.size()};
}
} // namespace

TEST_CASE("mtx write -> read round-trips a real CSR", "[hesap][sparse][mtx]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    auto                       a   = small_csr(&alloc);
    auto                       txt = sp::write_matrix_market<crd::f64>(a, &alloc);
    sp::MatrixMarketError      err(&alloc);
    auto                       b = sp::read_matrix_market<crd::f64>(sv(txt), &alloc, err);
    REQUIRE(err.ok);
    REQUIRE(b.rows() == a.rows());
    REQUIRE(b.cols() == a.cols());
    REQUIRE(b.pattern().topology_hash == a.pattern().topology_hash);
    REQUIRE(b.nnz() == a.nnz());
    for (crd::usize k = 0; k < a.nnz(); ++k)
    {
        CHECK(b.values().values[k] == Approx(a.values().values[k]).margin(1e-12));
    }
}

TEST_CASE("mtx reads a symmetric matrix and expands it", "[hesap][sparse][mtx]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    const char*                text = "%%MatrixMarket matrix coordinate real symmetric\n"
                                      "% a 3x3 symmetric matrix, lower triangle stored\n"
                                      "3 3 4\n"
                                      "1 1 4.0\n"
                                      "2 1 1.5\n"   // -> also (1,2)
                                      "3 2 -2.0\n"  // -> also (2,3)
                                      "3 3 5.0\n";
    sp::MatrixMarketError err(&alloc);
    auto                  m = sp::read_matrix_market<crd::f64>(crd::containers::StringView{text}, &alloc, err);
    REQUIRE(err.ok);
    REQUIRE(m.rows() == 3);
    REQUIRE(m.nnz() == 6);  // 4 stored - (none on diag mirrored) + 2 mirrored off-diagonals
    CHECK(m.coeff(0, 0) == Approx(4.0));
    CHECK(m.coeff(1, 0) == Approx(1.5));
    CHECK(m.coeff(0, 1) == Approx(1.5));  // mirrored
    CHECK(m.coeff(2, 1) == Approx(-2.0));
    CHECK(m.coeff(1, 2) == Approx(-2.0));  // mirrored
    CHECK(m.coeff(2, 2) == Approx(5.0));
}

TEST_CASE("mtx skew-symmetric + hermitian mirror correctly", "[hesap][sparse][mtx]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    // Skew: (i,j)=v -> (j,i)=-v; no diagonal.
    const char* skew = "%%MatrixMarket matrix coordinate real skew-symmetric\n"
                       "3 3 1\n"
                       "3 1 2.0\n";
    sp::MatrixMarketError err(&alloc);
    auto                  s = sp::read_matrix_market<crd::f64>(crd::containers::StringView{skew}, &alloc, err);
    REQUIRE(err.ok);
    CHECK(s.coeff(2, 0) == Approx(2.0));
    CHECK(s.coeff(0, 2) == Approx(-2.0));

    // Hermitian: (i,j)=a+bi -> (j,i)=a-bi.
    const char* herm = "%%MatrixMarket matrix coordinate complex hermitian\n"
                       "2 2 1\n"
                       "2 1 3.0 4.0\n";
    sp::MatrixMarketError errc(&alloc);
    auto                  h = sp::read_matrix_market<Complex64>(crd::containers::StringView{herm}, &alloc, errc);
    REQUIRE(errc.ok);
    CHECK(h.coeff(1, 0).re == Approx(3.0));
    CHECK(h.coeff(1, 0).im == Approx(4.0));
    CHECK(h.coeff(0, 1).re == Approx(3.0));
    CHECK(h.coeff(0, 1).im == Approx(-4.0));  // conjugated
}

TEST_CASE("mtx complex round-trip + pattern + array rejection", "[hesap][sparse][mtx]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);

    sp::TripletBuilder<Complex64> tb(&alloc, 3, 3);
    tb.add(0, 0, Complex64{1.0, 2.0});
    tb.add(1, 2, Complex64{-3.0, 0.5});
    auto                  c   = tb.compress();
    auto                  txt = sp::write_matrix_market<Complex64>(c, &alloc);
    sp::MatrixMarketError err(&alloc);
    auto                  c2 = sp::read_matrix_market<Complex64>(sv(txt), &alloc, err);
    REQUIRE(err.ok);
    REQUIRE(c2.nnz() == c.nnz());
    CHECK(c2.coeff(0, 0).re == Approx(1.0));
    CHECK(c2.coeff(0, 0).im == Approx(2.0));
    CHECK(c2.coeff(1, 2).re == Approx(-3.0));
    CHECK(c2.coeff(1, 2).im == Approx(0.5));

    // pattern: values default to 1.
    const char* pat = "%%MatrixMarket matrix coordinate pattern general\n2 2 2\n1 1\n2 2\n";
    sp::MatrixMarketError errp(&alloc);
    auto                  p = sp::read_matrix_market<crd::f64>(crd::containers::StringView{pat}, &alloc, errp);
    REQUIRE(errp.ok);
    CHECK(p.coeff(0, 0) == Approx(1.0));
    CHECK(p.coeff(1, 1) == Approx(1.0));

    // array (dense) format must be rejected, not silently mis-parsed.
    const char* arr = "%%MatrixMarket matrix array real general\n2 2\n1.0\n2.0\n3.0\n4.0\n";
    sp::MatrixMarketError erra(&alloc);
    auto                  bad = sp::read_matrix_market<crd::f64>(crd::containers::StringView{arr}, &alloc, erra);
    CHECK_FALSE(erra.ok);
    (void)bad;
}
