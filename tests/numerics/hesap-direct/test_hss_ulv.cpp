#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/hss.hpp>
#include <crd/hesap/direct/hss_ulv.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::hesap::dense::Matrix;
using crd::hesap::direct::build_hss_from_dense;
using crd::hesap::direct::factor_hss_ulv;
using crd::hesap::direct::hss_to_dense;
using crd::hesap::direct::HssMatrix;

namespace
{
template <typename T>
T frob(const Matrix<T>& m) noexcept
{
    T acc = T{0};
    for (crd::usize i = 0; i < m.size(); ++i)
    {
        acc += m.data()[i] * m.data()[i];
    }
    return std::sqrt(acc);
}

// Dense y = A·x.
template <typename T>
void dense_matvec(const Matrix<T>& a, const T* x, T* y) noexcept
{
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            s += a.at(i, j) * x[j];
        }
        y[i] = s;
    }
}

// SPD A = B·Bᵀ + n·I (well-conditioned; generic dense ⇒ full off-diagonal rank).
template <typename T>
Matrix<T> spd_random(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> b(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            b.at(i, j) = static_cast<T>(std::sin(static_cast<double>(i * 11 + j * 7 + 1) * 0.3) +
                                        std::cos(static_cast<double>(i * 5 + j * 13 + 2) * 0.17));
        }
    }
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::usize k = 0; k < n; ++k)
            {
                s += b.at(i, k) * b.at(j, k);  // (B Bᵀ)_{ij}
            }
            a.at(i, j) = s + (i == j ? static_cast<T>(n) : T{0});
        }
    }
    return a;
}

// SPD, low off-diagonal rank: diag(2 + i/10) + Σ_{t=0}^{3} 0.05^t w_t w_tᵀ (PSD).
template <typename T>
Matrix<T> kernel_spd(crd::memory::IAllocator* alloc, crd::usize n)
{
    constexpr crd::usize terms = 4;
    const double decay = 0.05;
    const double freq[terms] = {0.21, 0.37, 0.53, 0.71};
    Matrix<T> a(alloc, n, n);
    a.set_zero();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i; j < n; ++j)
        {
            double g = 0.0;
            double wt = 1.0;
            for (crd::usize t = 0; t < terms; ++t)
            {
                const double wi = std::sin(static_cast<double>(i) * freq[t] + 0.3 * static_cast<double>(t) + 0.1);
                const double wj = std::sin(static_cast<double>(j) * freq[t] + 0.3 * static_cast<double>(t) + 0.1);
                g += wt * wi * wj;
                wt *= decay;
            }
            const T v = static_cast<T>(g);
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
        a.at(i, i) += static_cast<T>(2.0 + static_cast<double>(i) * 0.1);
    }
    return a;
}

template <typename T>
T solve_recover_error(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize leaf, T tol)
{
    const crd::usize n = a.rows();
    crd::containers::Array<T> xt(alloc);
    crd::containers::Array<T> b(alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = static_cast<T>(std::sin(static_cast<double>(i) * 0.6 + 0.2));
    }
    dense_matvec<T>(a, xt.data(), b.data());  // b = A·x_true
    HssMatrix<T> h = build_hss_from_dense<T>(alloc, a, leaf, tol);
    auto f = factor_hss_ulv<T>(alloc, h);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve(crd::containers::Span<T>{b.data(), n}, 1));  // b -> x
    T e = T{0};
    T xn = T{0};
    for (crd::usize i = 0; i < n; ++i)
    {
        e += (b[i] - xt[i]) * (b[i] - xt[i]);
        xn += xt[i] * xt[i];
    }
    return std::sqrt(e) / std::sqrt(xn);
}
} // namespace

TEST_CASE("hss ulv: lossless solve recovers x_true (2 leaves)", "[hesap][hss][ulv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<double> a = spd_random<double>(&alloc, 16);  // leaf 8 -> 2 leaves, 1 merge
    CHECK(solve_recover_error<double>(&alloc, a, 8, 1e-12) < 1e-9);
}

TEST_CASE("hss ulv: lossless solve recovers x_true (4 leaves)", "[hesap][hss][ulv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    const Matrix<double> a = spd_random<double>(&alloc, 32);  // leaf 8 -> 4 leaves, recursive merge
    CHECK(solve_recover_error<double>(&alloc, a, 8, 1e-12) < 1e-9);
}

TEST_CASE("hss ulv: compressed solve - H-residual is machine-eps, A-residual ~ tol",
          "[hesap][hss][ulv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize n = 64;
    const double tol = 1e-6;
    const Matrix<double> a = kernel_spd<double>(&alloc, n);
    const double anorm = frob<double>(a);

    crd::containers::Array<double> b0(&alloc);
    crd::containers::Array<double> b(&alloc);
    b0.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        b0[i] = std::cos(static_cast<double>(i) * 0.5 + 0.1);
        b[i] = b0[i];
    }

    HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, 8, tol);
    auto f = factor_hss_ulv<double>(&alloc, h);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve(crd::containers::Span<double>{b.data(), n}, 1));  // b -> x

    // ULV solves H EXACTLY ⇒ ‖H·x − b‖ is machine-eps (NOT tol).
    const Matrix<double> hdense = hss_to_dense<double>(&alloc, h);
    crd::containers::Array<double> hx(&alloc);
    hx.resize(n);
    dense_matvec<double>(hdense, b.data(), hx.data());
    double rh = 0.0;
    double bn = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        rh += (hx[i] - b0[i]) * (hx[i] - b0[i]);
        bn += b0[i] * b0[i];
    }
    CHECK(std::sqrt(rh) < 1e-10 * std::sqrt(bn));  // exact solve of H

    // End-to-end ‖A·x − b‖ ~ tol·‖A‖ (the compression error, separate).
    crd::containers::Array<double> ax(&alloc);
    ax.resize(n);
    dense_matvec<double>(a, b.data(), ax.data());
    double ra = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        ra += (ax[i] - b0[i]) * (ax[i] - b0[i]);
    }
    CHECK(std::sqrt(ra) < 1e-3 * anorm);  // ~ tol·sqrt(#blocks)·‖A‖, NOT machine-eps
}

TEST_CASE("hss ulv: multi-RHS", "[hesap][hss][ulv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 32;
    constexpr crd::usize nrhs = 3;
    const Matrix<double> a = spd_random<double>(&alloc, n);
    crd::containers::Array<double> xt(&alloc);
    crd::containers::Array<double> b(&alloc);
    xt.resize(n * nrhs);
    b.resize(n * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            xt[c * n + i] = std::sin(static_cast<double>(i * 3 + c * 7 + 1) * 0.4);
        }
        dense_matvec<double>(a, xt.data() + c * n, b.data() + c * n);
    }
    HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, 8, 1e-12);
    auto f = factor_hss_ulv<double>(&alloc, h);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve(crd::containers::Span<double>{b.data(), n * nrhs}, nrhs));
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        double e = 0.0;
        double xn = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            e += (b[c * n + i] - xt[c * n + i]) * (b[c * n + i] - xt[c * n + i]);
            xn += xt[c * n + i] * xt[c * n + i];
        }
        CHECK(std::sqrt(e) < 1e-9 * std::sqrt(xn));
    }
}

TEST_CASE("hss ulv: indefinite matrix is detected (info != 0, solve false)", "[hesap][hss][ulv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize n = 16;
    Matrix<double> a = spd_random<double>(&alloc, n);
    // Force a strongly non-positive pivot in the first leaf's diagonal block.
    a.at(0, 0) -= static_cast<double>(10 * n);
    HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, 8, 1e-12);
    auto f = factor_hss_ulv<double>(&alloc, h);
    CHECK(f.info() != 0);
    crd::containers::Array<double> b(&alloc);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        b[i] = 1.0;
    }
    CHECK_FALSE(f.solve(crd::containers::Span<double>{b.data(), n}, 1));
}

TEST_CASE("hss ulv: large-leaf low-rank solve (exercises BLAS-3 trsm path)", "[hesap][hss][ulv][real]")
{
    // leaf 128 + low off-diagonal rank ⇒ p = 128 - rank ~ 124 > 32 ⇒ the solve
    // routes the fully-summed elimination through BLAS-3 trsm (not scalar trsv).
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize n = 256;
    const Matrix<double> a = kernel_spd<double>(&alloc, n);
    crd::containers::Array<double> xt(&alloc);
    crd::containers::Array<double> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = std::cos(static_cast<double>(i) * 0.4 + 0.1);
    }
    dense_matvec<double>(a, xt.data(), b.data());
    HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, 128, 1e-9);
    auto f = factor_hss_ulv<double>(&alloc, h);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve(crd::containers::Span<double>{b.data(), n}, 1));
    double e = 0.0;
    double xn = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        e += (b[i] - xt[i]) * (b[i] - xt[i]);
        xn += xt[i] * xt[i];
    }
    CHECK(std::sqrt(e) < 1e-5 * std::sqrt(xn));  // approximate HSS solve (tol-level)
}

TEST_CASE("hss ulv: f32 lossless recover", "[hesap][hss][ulv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<float> a = spd_random<float>(&alloc, 16);
    CHECK(solve_recover_error<float>(&alloc, a, 8, 1e-6F) < 1e-3F);
}

namespace
{
// Flatten an HSS representation (every node's D/U/R/B generator) into one array
// so two builds can be compared bit-for-bit.
template <typename T>
crd::containers::Array<T> serialize_hss(crd::memory::IAllocator* alloc, const HssMatrix<T>& h)
{
    crd::containers::Array<T> out(alloc);
    const auto append = [&](const Matrix<T>& m)
    {
        for (crd::usize i = 0; i < m.size(); ++i)
        {
            out.push_back(m.data()[i]);
        }
    };
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        const auto& nd = h.nodes[id];
        append(nd.d);
        append(nd.u);
        append(nd.r);
        append(nd.b);
    }
    return out;
}

template <typename T>
bool exact_eq(const crd::containers::Array<T>& x, const crd::containers::Array<T>& y) noexcept
{
    if (x.size() != y.size())
    {
        return false;
    }
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        if (x[i] != y[i])  // bit-exact (finite values): the determinism moat
        {
            return false;
        }
    }
    return true;
}
} // namespace

// The determinism MOAT — the differentiator STRUMPACK structurally lacks. The
// counter-RNG sample Ω is thread-count-independent and the parallel gemm reduces
// each output in a fixed k-order, so compress (incl. the parallel A·Ω), factor,
// and solve are a pure function of A — BIT-IDENTICAL regardless of worker count.
// n=512 makes A·Ω (512·512·48 ≈ 12.6M ≥ the 256K serial threshold) go parallel.
TEST_CASE("v5e HSS+ULV: compress+factor+solve bit-identical across {1,2,4,8} workers",
          "[hesap][hss][ulv][moat][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U * 1024U * 1024U));
    constexpr crd::usize n = 512;
    const Matrix<double> a = kernel_spd<double>(&alloc, n);
    crd::containers::Array<double> xt(&alloc);
    crd::containers::Array<double> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = std::sin(static_cast<double>(i) * 0.6 + 0.2);
    }
    dense_matvec<double>(a, xt.data(), b.data());

    crd::containers::Array<double> ref_hss(&alloc);
    crd::containers::Array<double> ref_x(&alloc);
    bool have_ref = false;

    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        REQUIRE(crd::jobs::num_workers() == nw);

        HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, 64, 1e-9);
        auto f = factor_hss_ulv<double>(&alloc, h);
        REQUIRE(f.info() == 0);
        crd::containers::Array<double> x(&alloc);
        x.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        REQUIRE(f.solve(crd::containers::Span<double>{x.data(), n}, 1));
        crd::containers::Array<double> ser = serialize_hss<double>(&alloc, h);

        crd::jobs::shutdown();

        if (!have_ref)
        {
            ref_hss = std::move(ser);
            ref_x = std::move(x);
            have_ref = true;
        }
        else
        {
            CHECK(exact_eq<double>(ser, ref_hss));  // HSS bases bit-identical (counter-RNG moat)
            CHECK(exact_eq<double>(x, ref_x));      // and the full compress->factor->solve output
        }
    }
}
