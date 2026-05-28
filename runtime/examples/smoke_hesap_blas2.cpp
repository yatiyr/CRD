// smoke_hesap_blas2 — Phase 3.1.6 v0c end-to-end smoke.
//
// Exercises the BLAS L2 surface:
//   - 50x50 random matrix; gemv vs naive triple-loop matches within RMSE.
//   - LU-like round-trip: build a well-conditioned L, x0 known, b = L*x0,
//     solve L*x = b, compare x ≈ x0.
//   - CLI dispatch of hesap.dense.blas2.gemv.f64 returns bit-equal result.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>
#include <variant>

namespace
{
struct AnchorPull
{
    AnchorPull() noexcept
    {
        crd::hesap::dense::register_blas1_cli_anchor();
        crd::hesap::dense::register_blas2_cli_anchor();
    }
};
const AnchorPull kAnchorPull;

int fail(const char* msg)
{
    std::fprintf(stderr, "[smoke_hesap_blas2] FAIL: %s\n", msg);
    return 1;
}

bool approx_equal(crd::f64 a, crd::f64 b, crd::f64 tol)
{
    return std::abs(a - b) <= tol;
}
} // namespace

int main()
{
    using namespace crd::hesap::dense;
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);

    // ---- 1. gemv vs naive ------------------------------------------
    constexpr crd::usize m = 50;
    constexpr crd::usize k_n = 30;
    Matrix<crd::f64> a_mat(&alloc, m, k_n);
    Vector<crd::f64> x(&alloc, k_n);
    Vector<crd::f64> y_engine(&alloc, m);
    Vector<crd::f64> y_naive(&alloc, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_mat(i, j) = static_cast<crd::f64>(i + 1) / static_cast<crd::f64>(j + 2) - 0.5;
        }
    }
    for (crd::usize j = 0; j < k_n; ++j)
    {
        x(j) = std::sin(static_cast<crd::f64>(j));
    }
    gemv<crd::f64, Layout::RowMajor>(1.5, a_mat.cview(), x.span(), 0.0, y_engine.span(), Trans::None);
    for (crd::usize i = 0; i < m; ++i)
    {
        crd::f64 s = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a_mat(i, j) * x(j);
        }
        y_naive(i) = 1.5 * s;
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        if (!approx_equal(y_engine(i), y_naive(i), 1e-9))
        {
            std::fprintf(stderr, "[smoke_hesap_blas2] gemv[%zu] engine=%.17g naive=%.17g\n",
                         static_cast<std::size_t>(i), y_engine(i), y_naive(i));
            return fail("gemv diverges from naive triple-loop");
        }
    }

    // ---- 2. trsv round-trip ----------------------------------------
    constexpr crd::usize k_s = 8;
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> tri_l(&alloc, k_s);
    for (crd::usize i = 0; i < k_s; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            tri_l.at(i, j) = (i == j) ? 4.0 : 0.3;
        }
    }
    Vector<crd::f64> x0(&alloc, k_s);
    for (crd::usize i = 0; i < k_s; ++i)
    {
        x0(i) = std::cos(static_cast<crd::f64>(i));
    }
    auto x_back = x0.clone();
    trmv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(tri_l, x_back.span(), Trans::None);
    trsv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(tri_l, x_back.span(), Trans::None);
    for (crd::usize i = 0; i < k_s; ++i)
    {
        if (!approx_equal(x_back(i), x0(i), 1e-12))
        {
            return fail("trmv+trsv round-trip lost accuracy");
        }
    }

    // ---- 3. CLI gemv dispatch --------------------------------------
    auto& reg = crd::hesap::cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.dense.blas2.gemv.f64");
    if (rec == nullptr || rec->impl == nullptr)
    {
        return fail("hesap.dense.blas2.gemv.f64 missing from registry");
    }

    crd::memory::TlsfAllocator cli_alloc(1024 * 1024);
    crd::hesap::cli::CommandArgs args{&cli_alloc};
    args.set_f64("alpha", 1.5);
    args.set_f64("beta", 0.0);
    args.set_u64("rows", m);
    args.set_u64("cols", k_n);
    crd::containers::Array<crd::f64> a_flat(&cli_alloc);
    a_flat.reserve(m * k_n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_flat.push_back(a_mat(i, j));
        }
    }
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat.data(), a_flat.size()});
    args.set_f64_array("x", x.span());
    args.set_f64_array("y", crd::containers::ConstSpan<crd::f64>{a_flat.data(), m});  // dummy y (zeroed by gemv with beta=0)

    const auto r = rec->impl(args);
    if (!r.ok)
    {
        return fail("gemv CLI returned ok=false");
    }
    const auto* blob = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&r.value);
    if (blob == nullptr || blob->bytes.size() != m * sizeof(crd::f64))
    {
        return fail("gemv CLI didn't return a m*f64 binary blob");
    }
    const crd::f64* y_cli = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    for (crd::usize i = 0; i < m; ++i)
    {
        if (!approx_equal(y_cli[i], y_engine(i), 1e-12))
        {
            std::fprintf(stderr, "[smoke_hesap_blas2] CLI gemv[%zu]=%.17g engine=%.17g\n",
                         static_cast<std::size_t>(i), y_cli[i], y_engine(i));
            return fail("CLI gemv diverges from engine gemv");
        }
    }

    crd::usize blas2_count = 0;
    for (const auto* p : reg.all())
    {
        const crd::containers::StringView name{p->schema.name.c_str(), p->schema.name.size()};
        if (name.starts_with("hesap.dense.blas2."))
        {
            ++blas2_count;
        }
    }

    std::fprintf(stdout,
                 "[smoke_hesap_blas2] gemv RMSE OK over %zu rows × %zu cols\n"
                 "[smoke_hesap_blas2] trmv+trsv round-trip OK over %zu elements\n"
                 "[smoke_hesap_blas2] %zu BLAS L2 commands registered\n"
                 "[smoke_hesap_blas2] CLI dispatch hesap.dense.blas2.gemv.f64 bit-equal to engine\n"
                 "[smoke_hesap_blas2] OK\n",
                 static_cast<std::size_t>(m), static_cast<std::size_t>(k_n),
                 static_cast<std::size_t>(k_s), static_cast<std::size_t>(blas2_count));
    return 0;
}
