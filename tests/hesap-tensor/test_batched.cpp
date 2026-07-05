// v14-h batched-LA gates - increment A: batched_gemm.
// The BIT gate: batched == loop-of-single hesap-dense gemm, exactly; the moat
// gate: {1,2,4,8,16} workers bit-identical. Peers board separate (bench doc).
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/batched.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

using crd::hesap::tensor::batched_gemm;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

template <typename T>
void fill_rand(Tensor<T>& t, crd::u64 seed, crd::u64 stream)
{
    crd::hesap::stats::PhiloxRng rng(seed, stream);
    T* p = t.data();
    const crd::u64 n = t.shape(0) * t.shape(1) * t.shape(2);
    for (crd::u64 i = 0; i < n; ++i)
    {
        p[i] = static_cast<T>(2.0 * rng.next_f64() - 1.0);
    }
}

} // namespace

TEST_CASE("batched: tiny-tier gemm bit-matches the scalar fma-chain contract", "[v14h][batched]")
{
    // the tiny tier's bit contract: every element is the k-ordered
    // single-rounded fma chain (vector lanes are lane-wise IEEE fma)
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 shapes[][3] = {{64U, 6U, 6U}, {33U, 4U, 4U}, {17U, 16U, 16U}};
    for (const auto& s : shapes)
    {
        const crd::u64 bsz = s[0];
        const crd::u64 n = s[1];
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> a(&alloc, {shp, 3});
        Tensor<crd::f64> b(&alloc, {shp, 3});
        Tensor<crd::f64> c(&alloc, {shp, 3});
        Tensor<crd::f64> cref(&alloc, {shp, 3});
        fill_rand(a, 7U, 0U);
        fill_rand(b, 7U, 1U);
        fill_rand(c, 7U, 2U);
        for (crd::u64 i = 0; i < bsz * n * n; ++i)
        {
            cref.data()[i] = c.data()[i];
        }
        const crd::f64 alpha = 1.25;
        const crd::f64 beta = -0.5;
        REQUIRE(batched_gemm<crd::f64>(alpha, TensorView<const crd::f64>(a.view()),
                                       TensorView<const crd::f64>(b.view()), beta, c.view(), &alloc,
                                       1U) == TensorStatus::Ok);
        for (crd::u64 i = 0; i < bsz; ++i)
        {
            const crd::f64* am = a.data() + i * n * n;
            const crd::f64* bm = b.data() + i * n * n;
            crd::f64* cm = cref.data() + i * n * n;
            for (crd::u64 r = 0; r < n; ++r)
            {
                for (crd::u64 col = 0; col < n; ++col)
                {
                    crd::f64 acc = 0.0;
                    for (crd::u64 p = 0; p < n; ++p)
                    {
                        acc = std::fma(am[r * n + p], bm[p * n + col], acc);
                    }
                    cm[r * n + col] = std::fma(beta, cm[r * n + col], alpha * acc);
                }
            }
        }
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < bsz * n * n; ++i)
        {
            if (std::bit_cast<crd::u64>(c.data()[i]) != std::bit_cast<crd::u64>(cref.data()[i]))
            {
                ++mism;
            }
        }
        INFO("batch " << bsz << " n " << n);
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("batched: large-tier gemm bit-matches the loop of single dense gemms", "[v14h][batched]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 shapes[][3] = {{8U, 40U, 40U}};
    for (const auto& s : shapes)
    {
        const crd::u64 bsz = s[0];
        const crd::u64 n = s[1];
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> a(&alloc, {shp, 3});
        Tensor<crd::f64> b(&alloc, {shp, 3});
        Tensor<crd::f64> c(&alloc, {shp, 3});
        Tensor<crd::f64> cref(&alloc, {shp, 3});
        fill_rand(a, 7U, 0U);
        fill_rand(b, 7U, 1U);
        fill_rand(c, 7U, 2U);
        for (crd::u64 i = 0; i < bsz * n * n; ++i)
        {
            cref.data()[i] = c.data()[i];
        }
        const crd::f64 alpha = 1.25;
        const crd::f64 beta = -0.5;
        REQUIRE(batched_gemm<crd::f64>(alpha, TensorView<const crd::f64>(a.view()),
                                       TensorView<const crd::f64>(b.view()), beta, c.view(), &alloc,
                                       1U) == TensorStatus::Ok);
        // reference: single dense gemm per matrix
        for (crd::u64 i = 0; i < bsz; ++i)
        {
            using MV = crd::hesap::dense::MatrixView<crd::f64, crd::hesap::dense::Layout::RowMajor>;
            using MVC = crd::hesap::dense::MatrixView<const crd::f64, crd::hesap::dense::Layout::RowMajor>;
            const MVC av{a.data() + i * n * n, static_cast<crd::usize>(n), static_cast<crd::usize>(n),
                         static_cast<crd::usize>(n)};
            const MVC bv{b.data() + i * n * n, static_cast<crd::usize>(n), static_cast<crd::usize>(n),
                         static_cast<crd::usize>(n)};
            MV cv{cref.data() + i * n * n, static_cast<crd::usize>(n), static_cast<crd::usize>(n),
                  static_cast<crd::usize>(n)};
            crd::hesap::dense::gemm<crd::f64, crd::hesap::dense::Layout::RowMajor>(
                alpha, av, bv, beta, cv, crd::hesap::dense::Trans::None, crd::hesap::dense::Trans::None, &alloc);
        }
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < bsz * n * n; ++i)
        {
            if (std::bit_cast<crd::u64>(c.data()[i]) != std::bit_cast<crd::u64>(cref.data()[i]))
            {
                ++mism;
            }
        }
        INFO("batch " << bsz << " n " << n);
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("batched: gemm rejects loose inner strides and shape mismatches", "[v14h][batched]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u64 shp[3] = {4U, 6U, 6U};
    Tensor<crd::f64> a(&alloc, {shp, 3});
    Tensor<crd::f64> b(&alloc, {shp, 3});
    Tensor<crd::f64> c(&alloc, {shp, 3});
    fill_rand(a, 3U, 0U);
    fill_rand(b, 3U, 1U);
    fill_rand(c, 3U, 2U);
    // rank != 3
    const crd::u64 shp2[2] = {6U, 6U};
    Tensor<crd::f64> r2(&alloc, {shp2, 2});
    REQUIRE(batched_gemm<crd::f64>(1.0, TensorView<const crd::f64>(r2.view()), TensorView<const crd::f64>(b.view()),
                                   0.0, c.view(), &alloc, 1U) == TensorStatus::BadInput);
    // shape mismatch
    const crd::u64 shp3[3] = {4U, 6U, 5U};
    Tensor<crd::f64> bad(&alloc, {shp3, 3});
    REQUIRE(batched_gemm<crd::f64>(1.0, TensorView<const crd::f64>(a.view()), TensorView<const crd::f64>(b.view()),
                                   0.0, bad.view(), &alloc, 1U) == TensorStatus::ShapeMismatch);
}

TEST_CASE("batched: the {1,2,4,8,16} moat - gemm bit-identical at every worker count", "[v14h][batched][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 shp[3] = {1000U, 6U, 6U};
    Tensor<crd::f64> a(&alloc, {shp, 3});
    Tensor<crd::f64> b(&alloc, {shp, 3});
    Tensor<crd::f64> c0(&alloc, {shp, 3});
    Tensor<crd::f64> c(&alloc, {shp, 3});
    fill_rand(a, 11U, 0U);
    fill_rand(b, 11U, 1U);
    fill_rand(c0, 11U, 2U);
    Tensor<crd::f64> serial(&alloc, {shp, 3});
    for (crd::u64 i = 0; i < 1000U * 36U; ++i)
    {
        serial.data()[i] = c0.data()[i];
    }
    REQUIRE(batched_gemm<crd::f64>(1.0, TensorView<const crd::f64>(a.view()), TensorView<const crd::f64>(b.view()),
                                   0.25, serial.view(), &alloc, 1U) == TensorStatus::Ok);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        for (crd::u64 i = 0; i < 1000U * 36U; ++i)
        {
            c.data()[i] = c0.data()[i];
        }
        const TensorStatus st =
            batched_gemm<crd::f64>(1.0, TensorView<const crd::f64>(a.view()), TensorView<const crd::f64>(b.view()),
                                   0.25, c.view(), &alloc, 0U);
        crd::jobs::shutdown();
        REQUIRE(st == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < 1000U * 36U; ++i)
        {
            if (std::bit_cast<crd::u64>(c.data()[i]) != std::bit_cast<crd::u64>(serial.data()[i]))
            {
                ++mism;
            }
        }
        INFO("workers " << nw);
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("batched: cholesky factor+solve - accuracy, tier bit-identity, poison isolation", "[v14h][batched][chol]")
{
    using crd::hesap::tensor::batched_cholesky_factor;
    using crd::hesap::tensor::batched_cholesky_solve;
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (const crd::u64 n : {4ULL, 6ULL, 8ULL, 16ULL})
    {
        const crd::u64 bsz = 37U; // odd: exercises the lane remainder group
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> a(&alloc, {shp, 3});
        fill_rand(a, 21U, n);
        for (crd::u64 b = 0; b < bsz; ++b) // SPD-ify: A = A A^T + n I
        {
            crd::f64* m = a.data() + b * n * n;
            crd::f64 tmp[16 * 16];
            for (crd::u64 i = 0; i < n; ++i)
            {
                for (crd::u64 j = 0; j < n; ++j)
                {
                    crd::f64 s = i == j ? static_cast<crd::f64>(n) : 0.0;
                    for (crd::u64 p = 0; p < n; ++p)
                    {
                        s += m[i * n + p] * m[j * n + p];
                    }
                    tmp[i * n + j] = s;
                }
            }
            for (crd::u64 e = 0; e < n * n; ++e)
            {
                m[e] = tmp[e];
            }
        }
        Tensor<crd::f64> aref(&alloc, {shp, 3});
        Tensor<crd::f64> ascalar(&alloc, {shp, 3});
        for (crd::u64 e = 0; e < bsz * n * n; ++e)
        {
            aref.data()[e] = a.data()[e];
            ascalar.data()[e] = a.data()[e];
        }
        crd::containers::Array<crd::i32> info(&alloc);
        info.resize(bsz, -7);
        REQUIRE(batched_cholesky_factor<crd::f64>(a.view(), {info.data(), bsz}, 1U) == TensorStatus::Ok);
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            REQUIRE(info[b] == 0);
        }
        for (crd::u64 b = 0; b < bsz; ++b) // tier bit-identity
        {
            crd::i32 si = -1;
            crd::hesap::tensor::batcheddetail::chol_scalar_one(ascalar.data() + b * n * n, n, &si);
            REQUIRE(si == 0);
        }
        crd::u64 mism = 0;
        for (crd::u64 e = 0; e < bsz * n * n; ++e)
        {
            if (std::bit_cast<crd::u64>(a.data()[e]) != std::bit_cast<crd::u64>(ascalar.data()[e]))
            {
                ++mism;
            }
        }
        INFO("n " << n);
        REQUIRE(mism == 0U);
        crd::f64 worst = 0.0; // accuracy: squared rel residual of A - L L^T
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            const crd::f64* l = a.data() + b * n * n;
            const crd::f64* m0 = aref.data() + b * n * n;
            crd::f64 num = 0.0;
            crd::f64 den = 0.0;
            for (crd::u64 i = 0; i < n; ++i)
            {
                for (crd::u64 j = 0; j < n; ++j)
                {
                    crd::f64 s = 0.0;
                    const crd::u64 lim = i < j ? i : j;
                    for (crd::u64 p = 0; p <= lim; ++p)
                    {
                        s += l[i * n + p] * l[j * n + p];
                    }
                    num += (s - m0[i * n + j]) * (s - m0[i * n + j]);
                    den += m0[i * n + j] * m0[i * n + j];
                }
            }
            const crd::f64 rel = num / (den > 0.0 ? den : 1.0);
            if (rel > worst)
            {
                worst = rel;
            }
        }
        REQUIRE(worst < 1e-24);
        const crd::u64 rshp[3] = {bsz, n, 2U};
        Tensor<crd::f64> rhs(&alloc, {rshp, 3});
        fill_rand(rhs, 22U, n);
        Tensor<crd::f64> rhs0(&alloc, {rshp, 3});
        for (crd::u64 e = 0; e < bsz * n * 2U; ++e)
        {
            rhs0.data()[e] = rhs.data()[e];
        }
        REQUIRE(batched_cholesky_solve<crd::f64>(TensorView<const crd::f64>(a.view()), rhs.view(), 1U) ==
                TensorStatus::Ok);
        crd::f64 worst_s = 0.0;
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            const crd::f64* m0 = aref.data() + b * n * n;
            const crd::f64* x = rhs.data() + b * n * 2U;
            const crd::f64* b0 = rhs0.data() + b * n * 2U;
            for (crd::u64 i = 0; i < n; ++i)
            {
                for (crd::u64 c = 0; c < 2U; ++c)
                {
                    crd::f64 s = 0.0;
                    for (crd::u64 p = 0; p < n; ++p)
                    {
                        s += m0[i * n + p] * x[p * 2U + c];
                    }
                    const crd::f64 d = s - b0[i * 2U + c];
                    if (d * d > worst_s)
                    {
                        worst_s = d * d;
                    }
                }
            }
        }
        REQUIRE(worst_s < 1e-16);
    }
}

TEST_CASE("batched: cholesky {1,2,4,8,16} moat", "[v14h][batched][chol][moat]")
{
    using crd::hesap::tensor::batched_cholesky_factor;
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u64 n = 6U;
    const crd::u64 bsz = 999U;
    const crd::u64 shp[3] = {bsz, n, n};
    Tensor<crd::f64> base(&alloc, {shp, 3});
    fill_rand(base, 41U, 0U);
    for (crd::u64 b = 0; b < bsz; ++b)
    {
        crd::f64* m = base.data() + b * n * n;
        crd::f64 tmp[36];
        for (crd::u64 i = 0; i < n; ++i)
        {
            for (crd::u64 j = 0; j < n; ++j)
            {
                crd::f64 s = i == j ? 6.0 : 0.0;
                for (crd::u64 p = 0; p < n; ++p)
                {
                    s += m[i * n + p] * m[j * n + p];
                }
                tmp[i * n + j] = s;
            }
        }
        for (crd::u64 e = 0; e < 36U; ++e)
        {
            m[e] = tmp[e];
        }
    }
    Tensor<crd::f64> serial(&alloc, {shp, 3});
    for (crd::u64 e = 0; e < bsz * 36U; ++e)
    {
        serial.data()[e] = base.data()[e];
    }
    crd::containers::Array<crd::i32> si(&alloc);
    si.resize(bsz, -7);
    REQUIRE(batched_cholesky_factor<crd::f64>(serial.view(), {si.data(), bsz}, 1U) == TensorStatus::Ok);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        Tensor<crd::f64> par(&alloc, {shp, 3});
        for (crd::u64 e = 0; e < bsz * 36U; ++e)
        {
            par.data()[e] = base.data()[e];
        }
        crd::containers::Array<crd::i32> pi(&alloc);
        pi.resize(bsz, -7);
        const TensorStatus st = batched_cholesky_factor<crd::f64>(par.view(), {pi.data(), bsz}, 0U);
        crd::jobs::shutdown();
        REQUIRE(st == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 e = 0; e < bsz * 36U; ++e)
        {
            if (std::bit_cast<crd::u64>(par.data()[e]) != std::bit_cast<crd::u64>(serial.data()[e]))
            {
                ++mism;
            }
        }
        INFO("workers " << nw);
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("batched: LU factor+solve - tier bit-identity incl pivots, residual, moat", "[v14h][batched][lu]")
{
    using crd::hesap::tensor::batched_lu_factor;
    using crd::hesap::tensor::batched_lu_solve;
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (const crd::u64 n : {4ULL, 6ULL, 8ULL, 16ULL})
    {
        const crd::u64 bsz = 37U;
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> a(&alloc, {shp, 3});
        fill_rand(a, 51U, n);
        Tensor<crd::f64> aref(&alloc, {shp, 3});
        Tensor<crd::f64> ascalar(&alloc, {shp, 3});
        for (crd::u64 e = 0; e < bsz * n * n; ++e)
        {
            aref.data()[e] = a.data()[e];
            ascalar.data()[e] = a.data()[e];
        }
        crd::containers::Array<crd::i32> piv(&alloc);
        crd::containers::Array<crd::i32> info(&alloc);
        piv.resize(bsz * n, -9);
        info.resize(bsz, -9);
        REQUIRE(batched_lu_factor<crd::f64>(a.view(), {piv.data(), bsz * n}, {info.data(), bsz}, 1U) ==
                TensorStatus::Ok);
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            REQUIRE(info[b] == 0);
        }
        // tier bit-identity: scalar tier must reproduce factors AND pivots
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            crd::i32 spiv[16];
            crd::i32 si = -1;
            crd::hesap::tensor::batcheddetail::lu_scalar_one(ascalar.data() + b * n * n, n, spiv, &si);
            REQUIRE(si == 0);
            for (crd::u64 j = 0; j < n; ++j)
            {
                REQUIRE(spiv[j] == piv[b * n + j]);
            }
        }
        crd::u64 mism = 0;
        for (crd::u64 e = 0; e < bsz * n * n; ++e)
        {
            if (std::bit_cast<crd::u64>(a.data()[e]) != std::bit_cast<crd::u64>(ascalar.data()[e]))
            {
                ++mism;
            }
        }
        INFO("n " << n);
        REQUIRE(mism == 0U);
        // solve residual: A x = b
        const crd::u64 rshp[3] = {bsz, n, 2U};
        Tensor<crd::f64> rhs(&alloc, {rshp, 3});
        fill_rand(rhs, 52U, n);
        Tensor<crd::f64> rhs0(&alloc, {rshp, 3});
        for (crd::u64 e = 0; e < bsz * n * 2U; ++e)
        {
            rhs0.data()[e] = rhs.data()[e];
        }
        REQUIRE(batched_lu_solve<crd::f64>(TensorView<const crd::f64>(a.view()), {piv.data(), bsz * n},
                                           rhs.view(), 1U) == TensorStatus::Ok);
        crd::f64 worst = 0.0;
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            const crd::f64* m0 = aref.data() + b * n * n;
            const crd::f64* x = rhs.data() + b * n * 2U;
            const crd::f64* b0 = rhs0.data() + b * n * 2U;
            for (crd::u64 i = 0; i < n; ++i)
            {
                for (crd::u64 c = 0; c < 2U; ++c)
                {
                    crd::f64 s = 0.0;
                    for (crd::u64 p = 0; p < n; ++p)
                    {
                        s += m0[i * n + p] * x[p * 2U + c];
                    }
                    const crd::f64 d = s - b0[i * 2U + c];
                    if (d * d > worst)
                    {
                        worst = d * d;
                    }
                }
            }
        }
        REQUIRE(worst < 1e-14);
    }
    // moat on the lane path
    {
        const crd::u64 n = 6U;
        const crd::u64 bsz = 777U;
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> base(&alloc, {shp, 3});
        fill_rand(base, 61U, 0U);
        Tensor<crd::f64> serial(&alloc, {shp, 3});
        for (crd::u64 e = 0; e < bsz * 36U; ++e)
        {
            serial.data()[e] = base.data()[e];
        }
        crd::containers::Array<crd::i32> sp(&alloc);
        crd::containers::Array<crd::i32> si(&alloc);
        sp.resize(bsz * n);
        si.resize(bsz);
        REQUIRE(batched_lu_factor<crd::f64>(serial.view(), {sp.data(), bsz * n}, {si.data(), bsz}, 1U) ==
                TensorStatus::Ok);
        for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
        {
            crd::jobs::Config cfg;
            cfg.num_threads = nw;
            crd::jobs::init(cfg);
            Tensor<crd::f64> par(&alloc, {shp, 3});
            for (crd::u64 e = 0; e < bsz * 36U; ++e)
            {
                par.data()[e] = base.data()[e];
            }
            crd::containers::Array<crd::i32> pp(&alloc);
            crd::containers::Array<crd::i32> pi2(&alloc);
            pp.resize(bsz * n);
            pi2.resize(bsz);
            const TensorStatus st =
                batched_lu_factor<crd::f64>(par.view(), {pp.data(), bsz * n}, {pi2.data(), bsz}, 0U);
            crd::jobs::shutdown();
            REQUIRE(st == TensorStatus::Ok);
            crd::u64 mism = 0;
            for (crd::u64 e = 0; e < bsz * 36U; ++e)
            {
                if (std::bit_cast<crd::u64>(par.data()[e]) != std::bit_cast<crd::u64>(serial.data()[e]))
                {
                    ++mism;
                }
            }
            for (crd::u64 e = 0; e < bsz * n; ++e)
            {
                if (pp[e] != sp[e])
                {
                    ++mism;
                }
            }
            INFO("workers " << nw);
            REQUIRE(mism == 0U);
        }
    }
}

TEST_CASE("batched: small-SVD Jacobi - reconstruction, orthogonality, tier bit-identity, moat", "[v14h][batched][svd]")
{
    using crd::hesap::tensor::batched_svd_small;
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (const crd::u64 n : {4ULL, 6ULL, 8ULL, 16ULL})
    {
        const crd::u64 bsz = 37U;
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> a(&alloc, {shp, 3});
        fill_rand(a, 71U, n);
        Tensor<crd::f64> u(&alloc, {shp, 3});
        Tensor<crd::f64> v(&alloc, {shp, 3});
        crd::containers::Array<crd::f64> sig(&alloc);
        crd::containers::Array<crd::i32> info(&alloc);
        sig.resize(bsz * n);
        info.resize(bsz, -9);
        REQUIRE(batched_svd_small<crd::f64>(TensorView<const crd::f64>(a.view()), u.view(),
                                            {sig.data(), bsz * n}, v.view(), {info.data(), bsz}, 30U,
                                            1U) == TensorStatus::Ok);
        crd::f64 worst_rec = 0.0;
        crd::f64 worst_orth = 0.0;
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            REQUIRE(info[b] == 0);
            const crd::f64* am = a.data() + b * n * n;
            const crd::f64* um = u.data() + b * n * n;
            const crd::f64* vm = v.data() + b * n * n;
            const crd::f64* sg = sig.data() + b * n;
            for (crd::u64 j = 1; j < n; ++j)
            {
                REQUIRE(sg[j] <= sg[j - 1U]); // descending
            }
            for (crd::u64 i = 0; i < n; ++i)
            {
                for (crd::u64 j = 0; j < n; ++j)
                {
                    crd::f64 rec = 0.0;
                    crd::f64 uo = 0.0;
                    for (crd::u64 p = 0; p < n; ++p)
                    {
                        rec += um[i * n + p] * sg[p] * vm[j * n + p];
                        uo += um[p * n + i] * um[p * n + j];
                    }
                    const crd::f64 dr = rec - am[i * n + j];
                    const crd::f64 di = uo - (i == j ? 1.0 : 0.0);
                    if (dr * dr > worst_rec)
                    {
                        worst_rec = dr * dr;
                    }
                    if (di * di > worst_orth)
                    {
                        worst_orth = di * di;
                    }
                }
            }
        }
        INFO("n " << n);
        REQUIRE(worst_rec < 1e-24);
        REQUIRE(worst_orth < 1e-24);
        // tier bit-identity: scalar sweeps + shared finalize must match
        Tensor<crd::f64> us(&alloc, {shp, 3});
        Tensor<crd::f64> vs(&alloc, {shp, 3});
        crd::containers::Array<crd::f64> sigs(&alloc);
        sigs.resize(bsz * n);
        for (crd::u64 b = 0; b < bsz; ++b)
        {
            crd::f64* uw = us.data() + b * n * n;
            crd::f64* vw = vs.data() + b * n * n;
            const crd::f64* aw = a.data() + b * n * n;
            for (crd::u64 e = 0; e < n * n; ++e)
            {
                uw[e] = aw[e];
            }
            crd::i32 si = -1;
            crd::hesap::tensor::batcheddetail::svd_scalar_sweeps(uw, vw, n, 30U, &si);
            REQUIRE(si == 0);
            crd::hesap::tensor::batcheddetail::svd_finalize_one(uw, vw, sigs.data() + b * n, n);
        }
        crd::u64 mism = 0;
        for (crd::u64 e = 0; e < bsz * n * n; ++e)
        {
            if (std::bit_cast<crd::u64>(u.data()[e]) != std::bit_cast<crd::u64>(us.data()[e]))
            {
                ++mism;
            }
            if (std::bit_cast<crd::u64>(v.data()[e]) != std::bit_cast<crd::u64>(vs.data()[e]))
            {
                ++mism;
            }
        }
        for (crd::u64 e = 0; e < bsz * n; ++e)
        {
            if (std::bit_cast<crd::u64>(sig[e]) != std::bit_cast<crd::u64>(sigs[e]))
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // moat
    {
        const crd::u64 n = 6U;
        const crd::u64 bsz = 555U;
        const crd::u64 shp[3] = {bsz, n, n};
        Tensor<crd::f64> a(&alloc, {shp, 3});
        fill_rand(a, 81U, 0U);
        Tensor<crd::f64> u1(&alloc, {shp, 3});
        Tensor<crd::f64> v1(&alloc, {shp, 3});
        crd::containers::Array<crd::f64> s1(&alloc);
        crd::containers::Array<crd::i32> i1(&alloc);
        s1.resize(bsz * n);
        i1.resize(bsz);
        REQUIRE(batched_svd_small<crd::f64>(TensorView<const crd::f64>(a.view()), u1.view(),
                                            {s1.data(), bsz * n}, v1.view(), {i1.data(), bsz}, 30U,
                                            1U) == TensorStatus::Ok);
        for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
        {
            crd::jobs::Config cfg;
            cfg.num_threads = nw;
            crd::jobs::init(cfg);
            Tensor<crd::f64> u2(&alloc, {shp, 3});
            Tensor<crd::f64> v2(&alloc, {shp, 3});
            crd::containers::Array<crd::f64> s2(&alloc);
            crd::containers::Array<crd::i32> i2(&alloc);
            s2.resize(bsz * n);
            i2.resize(bsz);
            const TensorStatus st =
                batched_svd_small<crd::f64>(TensorView<const crd::f64>(a.view()), u2.view(),
                                            {s2.data(), bsz * n}, v2.view(), {i2.data(), bsz}, 30U, 0U);
            crd::jobs::shutdown();
            REQUIRE(st == TensorStatus::Ok);
            crd::u64 mism = 0;
            for (crd::u64 e = 0; e < bsz * n * n; ++e)
            {
                if (std::bit_cast<crd::u64>(u2.data()[e]) != std::bit_cast<crd::u64>(u1.data()[e]))
                {
                    ++mism;
                }
            }
            for (crd::u64 e = 0; e < bsz * n; ++e)
            {
                if (std::bit_cast<crd::u64>(s2[e]) != std::bit_cast<crd::u64>(s1[e]))
                {
                    ++mism;
                }
            }
            INFO("workers " << nw);
            REQUIRE(mism == 0U);
        }
    }
}
