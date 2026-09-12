#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/nnls.hpp>
#include <crd/hesap/dense/tls.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::Complex;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::nnls;
using crd::hesap::dense::NNLS;
using crd::hesap::dense::tls;
using crd::hesap::dense::TLS;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinAbs;

namespace
{
struct NnlsTlsAnchorPull
{
    NnlsTlsAnchorPull() noexcept { crd::hesap::dense::register_lstsq_cli_anchor(); }
};
const NnlsTlsAnchorPull kNnlsTlsAnchorPull;
} // namespace

// ===================== NNLS =====================

TEST_CASE("nnls: textbook 2x2 clamps the negative component", "[hesap][nnls][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // A=[[2,1],[1,2]], b=[3,0]. Unconstrained LS = [2,-1]; NNLS clamps x2=0 →
    // x1 = (Aᵀb)/(‖a0‖²) on the single passive col = 6/5 = 1.2.
    Matrix<double, Layout::RowMajor> a(&alloc, 2, 2, {2.0, 1.0, 1.0, 2.0});
    Vector<double> b(&alloc, 2);
    b(0) = 3.0; b(1) = 0.0;
    NNLS<double> r = nnls<double>(&alloc, a, b);
    REQUIRE(r.converged);
    CHECK_THAT(r.x(0), WithinAbs(1.2, 1e-10));
    CHECK_THAT(r.x(1), WithinAbs(0.0, 1e-12));
}

TEST_CASE("nnls: recovers a non-negative ground truth exactly", "[hesap][nnls][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 10;
    constexpr crd::usize k_n = 4;
    // Full-rank Vandermonde in [0,1] (well-conditioned, rank k_n) so the
    // consistent system has a UNIQUE non-negative optimum = the unconstrained
    // LS solution = x_true.
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(k_m - 1);  // 0..1
        double pw = 1.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = pw;
            pw *= t;
        }
    }
    crd::containers::Array<double> xt(&alloc);
    xt.resize(k_n);
    xt[0] = 1.5; xt[1] = 0.5; xt[2] = 2.25; xt[3] = 0.75;  // all > 0
    Vector<double> b(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a.at(i, j) * xt[j];
        }
        b(i) = s;
    }
    NNLS<double> r = nnls<double>(&alloc, a, b);
    REQUIRE(r.converged);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        CHECK_THAT(r.x(j), WithinAbs(xt[j], 1e-8));
        CHECK(r.x(j) >= -1e-12);
    }
}

TEST_CASE("nnls: KKT optimality on a general problem", "[hesap][nnls][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 12;
    constexpr crd::usize k_n = 6;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i * 5 + j * 2) * 0.19) +
                         (static_cast<double>((i + j) % 3) - 1.0);
        }
    }
    Vector<double> b(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        b(i) = std::sin(static_cast<double>(i) * 0.8) * 2.0 - 0.3;
    }
    NNLS<double> r = nnls<double>(&alloc, a, b);
    REQUIRE(r.converged);

    // w = Aᵀ(b − A x). KKT: x_j >= 0; x_j > 0 ⇒ w_j ≈ 0; x_j == 0 ⇒ w_j <= 0.
    crd::containers::Array<double> res(&alloc);
    res.resize(k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double ax = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            ax += a.at(i, j) * r.x(j);
        }
        res[i] = b(i) - ax;
    }
    double wscale = 0.0;
    for (crd::usize j = 0; j < k_n; ++j)
    {
        double g = 0.0;
        for (crd::usize i = 0; i < k_m; ++i)
        {
            g += a.at(i, j) * res[i];
        }
        wscale = std::max(wscale, std::abs(g));
        CHECK(r.x(j) >= -1e-10);
        if (r.x(j) > 1e-8)
        {
            CHECK_THAT(g, WithinAbs(0.0, 1e-7));  // passive: gradient ~ 0
        }
        else
        {
            CHECK(g <= 1e-7);  // active: gradient non-positive
        }
    }
    (void)wscale;
}

TEST_CASE("nnls: f32 clamps negative component", "[hesap][nnls][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    Matrix<float, Layout::RowMajor> a(&alloc, 2, 2, {2.0F, 1.0F, 1.0F, 2.0F});
    Vector<float> b(&alloc, 2);
    b(0) = 3.0F; b(1) = 0.0F;
    NNLS<float> r = nnls<float>(&alloc, a, b);
    REQUIRE(r.converged);
    CHECK_THAT(static_cast<double>(r.x(0)), WithinAbs(1.2, 1e-4));
    CHECK_THAT(static_cast<double>(r.x(1)), WithinAbs(0.0, 1e-5));
}

// ===================== TLS =====================

TEST_CASE("tls: recovers x exactly for a consistent system", "[hesap][tls][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 8;
    constexpr crd::usize k_n = 3;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i + j * 2) * 0.3) + (i == j ? 2.0 : 0.0);
        }
    }
    crd::containers::Array<double> xt(&alloc);
    xt.resize(k_n);
    xt[0] = 1.5; xt[1] = -2.0; xt[2] = 0.5;
    Vector<double> b(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a.at(i, j) * xt[j];
        }
        b(i) = s;  // exact (no error) → TLS recovers x_true (smallest σ = 0)
    }
    TLS<double> r = tls<double>(&alloc, a, b);
    REQUIRE(r.exists);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        CHECK_THAT(r.x.at(j, 0), WithinAbs(xt[j], 1e-9));
    }
}

TEST_CASE("tls: multivariate (d=2) recovers X exactly", "[hesap][tls][real][multirhs]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 9;
    constexpr crd::usize k_n = 3;
    constexpr crd::usize k_d = 2;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i * 2 + j) * 0.25) + (i == j ? 3.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> xt(&alloc, k_n, k_d);
    xt.at(0, 0) = 1.0; xt.at(0, 1) = -0.5;
    xt.at(1, 0) = 2.0; xt.at(1, 1) = 1.5;
    xt.at(2, 0) = -1.0; xt.at(2, 1) = 0.25;
    Matrix<double, Layout::RowMajor> b(&alloc, k_m, k_d);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize l = 0; l < k_d; ++l)
        {
            double s = 0.0;
            for (crd::usize j = 0; j < k_n; ++j)
            {
                s += a.at(i, j) * xt.at(j, l);
            }
            b.at(i, l) = s;
        }
    }
    TLS<double> r = tls<double>(&alloc, a, b);
    REQUIRE(r.exists);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        for (crd::usize l = 0; l < k_d; ++l)
        {
            CHECK_THAT(r.x.at(j, l), WithinAbs(xt.at(j, l), 1e-8));
        }
    }
}

TEST_CASE("tls: complex consistent system recovers x", "[hesap][tls][complex]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    using Cx = Complex<double>;
    constexpr crd::usize k_m = 7;
    constexpr crd::usize k_n = 2;
    Matrix<Cx, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = Cx{std::sin(static_cast<double>(i + j) * 0.4) + (i == j ? 2.0 : 0.0),
                            std::cos(static_cast<double>(i * 2 + j) * 0.3)};
        }
    }
    crd::containers::Array<Cx> xt(&alloc);
    xt.resize(k_n);
    xt[0] = Cx{1.0, -0.5}; xt[1] = Cx{-1.5, 0.75};
    Vector<Cx> b(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        Cx s{0.0, 0.0};
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s = s + a.at(i, j) * xt[j];
        }
        b(i) = s;
    }
    TLS<Cx> r = tls<Cx>(&alloc, a, b);
    REQUIRE(r.exists);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        CHECK_THAT(r.x.at(j, 0).re, WithinAbs(xt[j].re, 1e-8));
        CHECK_THAT(r.x.at(j, 0).im, WithinAbs(xt[j].im, 1e-8));
    }
}

TEST_CASE("CLI: nnls + tls commands are registered and callable", "[hesap][nnls][tls][cli]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    auto& reg = crd::hesap::cli::CommandRegistry::global();
    for (const char* nm : {"hesap.dense.nnls.f32", "hesap.dense.nnls.f64", "hesap.dense.tls.f32",
                           "hesap.dense.tls.f64", "hesap.dense.tls.c32", "hesap.dense.tls.c64"})
    {
        INFO("missing command: " << nm);
        REQUIRE(reg.find(nm) != nullptr);
    }

    // nnls: A=[[2,1],[1,2]], b=[3,0] → x=[1.2, 0].
    const crd::f64 a_flat[] = {2.0, 1.0, 1.0, 2.0};
    const crd::f64 b_data[] = {3.0, 0.0};
    crd::hesap::cli::CommandArgs args{&alloc};
    args.set_u64("m", 2);
    args.set_u64("n", 2);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 4});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b_data, 2});
    const auto* cmd = reg.find("hesap.dense.nnls.f64");
    const auto res = cmd->impl(args);
    REQUIRE(res.ok);
    const auto* blob = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&res.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 2 * sizeof(crd::f64));
    const auto* x = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK_THAT(x[0], WithinAbs(1.2, 1e-10));
    CHECK_THAT(x[1], WithinAbs(0.0, 1e-12));
}
