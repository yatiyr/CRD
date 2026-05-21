// crd-hesap-sparse v1f-1 -- BSR storage + block-spmv + CSR<->BSR convert.

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
// Build a block-DENSE CSR: every chosen b x b block is fully populated, so
// to_bsr/from_bsr round-trips byte-for-byte and BSR spmv == scalar CSR spmv.
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> block_dense_csr(crd::memory::IAllocator* alloc, crd::u32 block_rows,
                                                           crd::u32 block_cols, crd::u32 b, crd::u32 blocks_per_row,
                                                           std::uint64_t seed)
{
    std::uint64_t s    = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    sp::TripletBuilder<T> tb(alloc, block_rows * b, block_cols * b);
    for (crd::u32 ib = 0; ib < block_rows; ++ib)
    {
        for (crd::u32 t = 0; t < blocks_per_row; ++t)
        {
            const crd::u32 jb = next() % block_cols;
            for (crd::u32 rr = 0; rr < b; ++rr)
            {
                for (crd::u32 cc = 0; cc < b; ++cc)
                {
                    // Nonzero everywhere in the block (1..9 scaled) so it stays dense.
                    tb.add(ib * b + rr, jb * b + cc, static_cast<T>(1 + (next() % 9)) / static_cast<T>(4));
                }
            }
        }
    }
    return tb.compress();  // duplicate block-cols sum -> still block-dense
}
} // namespace

TEST_CASE("bsr spmv matches scalar CSR spmv", "[hesap][sparse][bsr]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    for (crd::u32 b : {1U, 2U, 3U, 4U, 6U, 5U})  // 5 exercises the runtime fallback
    {
        const crd::u32 br = 12;
        const crd::u32 bc = 10;
        auto           csr = block_dense_csr<crd::f64>(&alloc, br, bc, b, 3, 0x100 + b);
        auto           bsr = sp::to_bsr<crd::f64>(csr, b, &alloc);
        REQUIRE(bsr.block_size == b);
        REQUIRE(bsr.rows == br * b);
        REQUIRE(bsr.cols == bc * b);

        crd::containers::Array<crd::f64> x(&alloc);
        x.resize(bc * b);
        for (crd::u32 i = 0; i < x.size(); ++i)
        {
            x[i] = 0.5 + 0.1 * static_cast<crd::f64>(i % 13);
        }
        crd::containers::Array<crd::f64> y_csr(&alloc);
        y_csr.resize(br * b);
        crd::containers::Array<crd::f64> y_bsr(&alloc);
        y_bsr.resize(br * b);

        for (crd::f64 beta : {0.0, 2.0})
        {
            for (crd::u32 i = 0; i < y_csr.size(); ++i)
            {
                y_csr[i] = 7.0 + static_cast<crd::f64>(i);
                y_bsr[i] = 7.0 + static_cast<crd::f64>(i);
            }
            sp::spmv<crd::f64>(1.5, csr, sp::Trans::None,
                               crd::containers::ConstSpan<crd::f64>{x.data(), x.size()}, beta,
                               crd::containers::Span<crd::f64>{y_csr.data(), y_csr.size()});
            sp::spmv_bsr<crd::f64>(1.5, bsr, crd::containers::ConstSpan<crd::f64>{x.data(), x.size()}, beta,
                                   crd::containers::Span<crd::f64>{y_bsr.data(), y_bsr.size()});
            for (crd::u32 i = 0; i < y_csr.size(); ++i)
            {
                INFO("b=" << b << " beta=" << beta << " i=" << i);
                CHECK(y_bsr[i] == Approx(y_csr[i]).margin(1e-12));
            }
        }
    }
}

TEST_CASE("from_bsr o to_bsr round-trips a block-dense matrix", "[hesap][sparse][bsr]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 b   = 3;
    auto           csr = block_dense_csr<crd::f64>(&alloc, 8, 8, b, 3, 0xABC);
    auto           bsr = sp::to_bsr<crd::f64>(csr, b, &alloc);
    auto           rt  = sp::from_bsr<crd::f64>(bsr, &alloc);
    REQUIRE(rt.pattern().topology_hash == csr.pattern().topology_hash);
    REQUIRE(rt.nnz() == csr.nnz());
    for (crd::usize k = 0; k < csr.nnz(); ++k)
    {
        CHECK(rt.values().values[k] == csr.values().values[k]);
    }
}

TEST_CASE("parallel bsr spmv is bit-exact with serial", "[hesap][sparse][bsr][parallel]")
{
    crd_hesap_sparse_tests::sparse_jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 b   = 3;
    auto           csr = block_dense_csr<crd::f64>(&alloc, 4000, 3500, b, 8, 0xBEEF);
    auto           bsr = sp::to_bsr<crd::f64>(csr, b, &alloc);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(bsr.cols);
    for (crd::u32 i = 0; i < x.size(); ++i)
    {
        x[i] = 0.3 + 0.07 * static_cast<crd::f64>(i % 29);
    }
    crd::containers::Array<crd::f64> ys(&alloc);
    ys.resize(bsr.rows);
    sp::spmv_bsr<crd::f64>(1.0, bsr, crd::containers::ConstSpan<crd::f64>{x.data(), x.size()}, 0.0,
                           crd::containers::Span<crd::f64>{ys.data(), ys.size()});
    for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U})
    {
        crd::containers::Array<crd::f64> yp(&alloc);
        yp.resize(bsr.rows);
        sp::spmv_bsr_parallel<crd::f64>(1.0, bsr, crd::containers::ConstSpan<crd::f64>{x.data(), x.size()}, 0.0,
                                        crd::containers::Span<crd::f64>{yp.data(), yp.size()}, jobs);
        crd::jobs::frame_reset();
        for (crd::u32 i = 0; i < ys.size(); ++i)
        {
            INFO("jobs=" << jobs << " i=" << i);
            CHECK(yp[i] == ys[i]);  // bit-exact
        }
    }
}

TEST_CASE("bsr spmv complex matches scalar CSR", "[hesap][sparse][bsr][complex]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 b   = 2;
    auto           csr = block_dense_csr<Complex64>(&alloc, 6, 6, b, 2, 0xC0FFEE);
    auto           bsr = sp::to_bsr<Complex64>(csr, b, &alloc);

    crd::containers::Array<Complex64> x(&alloc);
    x.resize(bsr.cols);
    for (crd::u32 i = 0; i < x.size(); ++i)
    {
        x[i] = Complex64{0.5 + static_cast<crd::f64>(i % 5), static_cast<crd::f64>(i % 3)};
    }
    crd::containers::Array<Complex64> y_csr(&alloc);
    y_csr.resize(bsr.rows);
    crd::containers::Array<Complex64> y_bsr(&alloc);
    y_bsr.resize(bsr.rows);
    sp::spmv<Complex64>(Complex64{1.0, 0.0}, csr, sp::Trans::None,
                        crd::containers::ConstSpan<Complex64>{x.data(), x.size()}, Complex64{0.0, 0.0},
                        crd::containers::Span<Complex64>{y_csr.data(), y_csr.size()});
    sp::spmv_bsr<Complex64>(Complex64{1.0, 0.0}, bsr, crd::containers::ConstSpan<Complex64>{x.data(), x.size()},
                            Complex64{0.0, 0.0}, crd::containers::Span<Complex64>{y_bsr.data(), y_bsr.size()});
    for (crd::u32 i = 0; i < y_csr.size(); ++i)
    {
        INFO("i=" << i);
        CHECK(y_bsr[i].re == Approx(y_csr[i].re).margin(1e-12));
        CHECK(y_bsr[i].im == Approx(y_csr[i].im).margin(1e-12));
    }
}
