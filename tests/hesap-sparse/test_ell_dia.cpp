// crd-hesap-sparse v1f-2 -- ELL + DIA storage + spmv + CSR convert.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "sparse_jobs_fixture.hpp"

#include <cstdint>

using Catch::Approx;
using crd::hesap::Complex64;
namespace sp = crd::hesap::sparse;

namespace
{
// Uniform-ish CSR (regular -> ELL's native pattern).
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> regular_csr(crd::memory::IAllocator* alloc, crd::u32 n, crd::u32 per_row,
                                                       std::uint64_t seed)
{
    std::uint64_t s    = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    sp::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 t = 0; t < per_row; ++t)
        {
            tb.add(i, next() % n, static_cast<T>(1 + (next() % 9)) / static_cast<T>(5));
        }
    }
    return tb.compress();
}

// Banded CSR (DIA's native pattern): tridiagonal-ish over `offs` band offsets.
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> banded_csr(crd::memory::IAllocator* alloc, crd::u32 n,
                                                      std::uint64_t seed)
{
    std::uint64_t s    = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    const crd::i32 offs[] = {-3, -1, 0, 1, 2, 5};
    sp::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::i32 k : offs)
        {
            const crd::i32 j = static_cast<crd::i32>(i) + k;
            if (j >= 0 && j < static_cast<crd::i32>(n))
            {
                tb.add(i, static_cast<crd::u32>(j), static_cast<T>(1 + (next() % 9)) / static_cast<T>(6));
            }
        }
    }
    return tb.compress();
}

template <typename T>
void check_spmv_close(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& csr, const T* yref, const T* y, crd::u32 n);

template <>
void check_spmv_close<crd::f64>(const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>&, const crd::f64* yref,
                                const crd::f64* y, crd::u32 n)
{
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(y[i] == Approx(yref[i]).margin(1e-11));
    }
}
} // namespace

TEST_CASE("ell spmv matches CSR + round-trips", "[hesap][sparse][ell]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n   = 500;
    auto           csr = regular_csr<crd::f64>(&alloc, n, 6, 0x1234);
    auto           ell = sp::to_ell<crd::f64>(csr, &alloc);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 0.4 + 0.1 * static_cast<crd::f64>(i % 11);
    }
    crd::containers::Array<crd::f64> yc(&alloc);
    yc.resize(n);
    crd::containers::Array<crd::f64> ye(&alloc);
    ye.resize(n);
    for (crd::f64 beta : {0.0, 2.0})
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            yc[i] = 3.0 + static_cast<crd::f64>(i);
            ye[i] = 3.0 + static_cast<crd::f64>(i);
        }
        sp::spmv<crd::f64>(1.5, csr, sp::Trans::None, {x.data(), x.size()}, beta, {yc.data(), yc.size()});
        sp::spmv_ell<crd::f64>(1.5, ell, {x.data(), x.size()}, beta, {ye.data(), ye.size()});
        check_spmv_close<crd::f64>(csr, yc.data(), ye.data(), n);
    }

    auto rt = sp::from_ell<crd::f64>(ell, &alloc);
    REQUIRE(rt.pattern().topology_hash == csr.pattern().topology_hash);
}

TEST_CASE("parallel ell spmv is bit-exact with serial", "[hesap][sparse][ell][parallel]")
{
    crd_hesap_sparse_tests::sparse_jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 n   = 20000;
    auto           csr = regular_csr<crd::f64>(&alloc, n, 16, 0x55AA);
    auto           ell = sp::to_ell<crd::f64>(csr, &alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 0.3 + 0.05 * static_cast<crd::f64>(i % 23);
    }
    crd::containers::Array<crd::f64> ys(&alloc);
    ys.resize(n);
    sp::spmv_ell<crd::f64>(1.0, ell, {x.data(), x.size()}, 0.0, {ys.data(), ys.size()});
    for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U})
    {
        crd::containers::Array<crd::f64> yp(&alloc);
        yp.resize(n);
        sp::spmv_ell_parallel<crd::f64>(1.0, ell, {x.data(), x.size()}, 0.0, {yp.data(), yp.size()}, jobs);
        crd::jobs::frame_reset();
        for (crd::u32 i = 0; i < n; ++i)
        {
            INFO("jobs=" << jobs << " i=" << i);
            CHECK(yp[i] == ys[i]);
        }
    }
}

TEST_CASE("dia spmv matches CSR + round-trips banded", "[hesap][sparse][dia]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n   = 600;
    auto           csr = banded_csr<crd::f64>(&alloc, n, 0x99);
    auto           dia = sp::to_dia<crd::f64>(csr, &alloc);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 0.7 + 0.03 * static_cast<crd::f64>(i % 19);
    }
    crd::containers::Array<crd::f64> yc(&alloc);
    yc.resize(n);
    crd::containers::Array<crd::f64> yd(&alloc);
    yd.resize(n);
    for (crd::f64 beta : {0.0, 1.5})
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            yc[i] = 2.0 + static_cast<crd::f64>(i % 7);
            yd[i] = 2.0 + static_cast<crd::f64>(i % 7);
        }
        sp::spmv<crd::f64>(0.9, csr, sp::Trans::None, {x.data(), x.size()}, beta, {yc.data(), yc.size()});
        sp::spmv_dia<crd::f64>(0.9, dia, {x.data(), x.size()}, beta, {yd.data(), yd.size()});
        check_spmv_close<crd::f64>(csr, yc.data(), yd.data(), n);
    }

    auto rt = sp::from_dia<crd::f64>(dia, &alloc);
    REQUIRE(rt.pattern().topology_hash == csr.pattern().topology_hash);
    REQUIRE(rt.nnz() == csr.nnz());
}

TEST_CASE("parallel dia spmv is bit-exact with serial", "[hesap][sparse][dia][parallel]")
{
    crd_hesap_sparse_tests::sparse_jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 n   = 50000;
    auto           csr = banded_csr<crd::f64>(&alloc, n, 0x424242);
    auto           dia = sp::to_dia<crd::f64>(csr, &alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 0.2 + 0.04 * static_cast<crd::f64>(i % 31);
    }
    crd::containers::Array<crd::f64> ys(&alloc);
    ys.resize(n);
    sp::spmv_dia<crd::f64>(1.0, dia, {x.data(), x.size()}, 0.0, {ys.data(), ys.size()});
    for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U})
    {
        crd::containers::Array<crd::f64> yp(&alloc);
        yp.resize(n);
        sp::spmv_dia_parallel<crd::f64>(1.0, dia, {x.data(), x.size()}, 0.0, {yp.data(), yp.size()}, jobs);
        crd::jobs::frame_reset();
        for (crd::u32 i = 0; i < n; ++i)
        {
            INFO("jobs=" << jobs << " i=" << i);
            CHECK(yp[i] == ys[i]);
        }
    }
}

TEST_CASE("ell + dia complex spmv match CSR", "[hesap][sparse][ell][dia][complex]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n   = 80;
    auto           csr = banded_csr<Complex64>(&alloc, n, 0xC0DE);
    auto           ell = sp::to_ell<Complex64>(csr, &alloc);
    auto           dia = sp::to_dia<Complex64>(csr, &alloc);

    crd::containers::Array<Complex64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = Complex64{0.5 + static_cast<crd::f64>(i % 4), static_cast<crd::f64>(i % 3)};
    }
    crd::containers::Array<Complex64> yc(&alloc);
    yc.resize(n);
    crd::containers::Array<Complex64> ye(&alloc);
    ye.resize(n);
    crd::containers::Array<Complex64> yd(&alloc);
    yd.resize(n);
    sp::spmv<Complex64>(Complex64{1.0, 0.0}, csr, sp::Trans::None, {x.data(), x.size()}, Complex64{0.0, 0.0},
                        {yc.data(), yc.size()});
    sp::spmv_ell<Complex64>(Complex64{1.0, 0.0}, ell, {x.data(), x.size()}, Complex64{0.0, 0.0},
                            {ye.data(), ye.size()});
    sp::spmv_dia<Complex64>(Complex64{1.0, 0.0}, dia, {x.data(), x.size()}, Complex64{0.0, 0.0},
                            {yd.data(), yd.size()});
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(ye[i].re == Approx(yc[i].re).margin(1e-11));
        CHECK(ye[i].im == Approx(yc[i].im).margin(1e-11));
        CHECK(yd[i].re == Approx(yc[i].re).margin(1e-11));
        CHECK(yd[i].im == Approx(yc[i].im).margin(1e-11));
    }
}
