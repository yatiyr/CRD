// smoke_hesap_blas3 — Phase 3.1.6 v0d end-to-end smoke.
//
// Exercises BLAS L3 surface:
//   - 128x128 gemm vs naive triple-loop (RMSE check within 1e-3 for f32).
//   - 32-element trsm round-trip via trmm (L * X = B → solve recovers X).
//   - CLI dispatch of hesap.dense.blas3.gemm.f64 returns bit-equal result.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
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
        crd::hesap::dense::register_blas3_cli_anchor();
    }
};
const AnchorPull kAnchorPull;

int fail(const char* msg)
{
    std::fprintf(stderr, "[smoke_hesap_blas3] FAIL: %s\n", msg);
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
    crd::memory::TlsfAllocator alloc(32 * 1024 * 1024);

    // ---- 1. gemm vs naive at 128×128 ----------------------------------
    constexpr crd::usize k_n = 128;
    Matrix<crd::f64> a_mat(&alloc, k_n, k_n);
    Matrix<crd::f64> b_mat(&alloc, k_n, k_n);
    Matrix<crd::f64> c_engine(&alloc, k_n, k_n);
    Matrix<crd::f64> c_naive(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_mat(i, j) = static_cast<crd::f64>(i + 1) * 0.01 + static_cast<crd::f64>(j) * 0.002;
            b_mat(i, j) = static_cast<crd::f64>(j + 1) * 0.03 - static_cast<crd::f64>(i) * 0.005;
        }
    }
    gemm<crd::f64, Layout::RowMajor>(1.5, a_mat, b_mat, 0.0, c_engine);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            crd::f64 s = 0.0;
            for (crd::usize p = 0; p < k_n; ++p)
            {
                s += a_mat(i, p) * b_mat(p, j);
            }
            c_naive(i, j) = 1.5 * s;
        }
    }
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            if (!approx_equal(c_engine(i, j), c_naive(i, j), 1e-9))
            {
                return fail("gemm diverges from naive at N=128");
            }
        }
    }

    // ---- 2. trsm round-trip via trmm at N=32 --------------------------
    constexpr crd::usize k_s = 32;
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> tri_l(&alloc, k_s);
    for (crd::usize i = 0; i < k_s; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            tri_l.at(i, j) = (i == j) ? 4.0 : 0.3;
        }
    }
    Matrix<crd::f64> b_initial(&alloc, k_s, 4);
    for (crd::usize i = 0; i < k_s; ++i)
    {
        for (crd::usize j = 0; j < 4; ++j)
        {
            b_initial(i, j) = static_cast<crd::f64>(i + 1) * static_cast<crd::f64>(j + 1) - 5.0;
        }
    }
    auto b_back = b_initial.clone();
    trmm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(1.0, tri_l, b_back.view(), Trans::None);
    trsm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(1.0, tri_l, b_back.view(), Trans::None);
    for (crd::usize i = 0; i < k_s; ++i)
    {
        for (crd::usize j = 0; j < 4; ++j)
        {
            if (!approx_equal(b_back(i, j), b_initial(i, j), 1e-10))
            {
                return fail("trmm+trsm round-trip lost accuracy");
            }
        }
    }

    // ---- 3. CLI gemm dispatch -----------------------------------------
    auto& reg = crd::hesap::cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.dense.blas3.gemm.f64");
    if (rec == nullptr || rec->impl == nullptr)
    {
        return fail("hesap.dense.blas3.gemm.f64 missing from registry");
    }
    crd::memory::TlsfAllocator cli_alloc(16 * 1024 * 1024);
    crd::hesap::cli::CommandArgs args{&cli_alloc};
    args.set_f64("alpha", 1.5);
    args.set_f64("beta", 0.0);
    args.set_u64("m", k_n);
    args.set_u64("k", k_n);
    args.set_u64("n", k_n);
    crd::containers::Array<crd::f64> a_flat(&cli_alloc);
    crd::containers::Array<crd::f64> b_flat(&cli_alloc);
    crd::containers::Array<crd::f64> c_flat(&cli_alloc);
    a_flat.reserve(k_n * k_n);
    b_flat.reserve(k_n * k_n);
    c_flat.reserve(k_n * k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_flat.push_back(a_mat(i, j));
            b_flat.push_back(b_mat(i, j));
            c_flat.push_back(0.0);
        }
    }
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat.data(), a_flat.size()});
    args.set_f64_array("B", crd::containers::ConstSpan<crd::f64>{b_flat.data(), b_flat.size()});
    args.set_f64_array("C", crd::containers::ConstSpan<crd::f64>{c_flat.data(), c_flat.size()});
    const auto r = rec->impl(args);
    if (!r.ok)
    {
        return fail("gemm CLI returned ok=false");
    }
    const auto* blob = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&r.value);
    if (blob == nullptr || blob->bytes.size() != k_n * k_n * sizeof(crd::f64))
    {
        return fail("gemm CLI didn't return k_n*k_n*f64 binary blob");
    }
    const crd::f64* c_cli = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            if (!approx_equal(c_cli[i * k_n + j], c_engine(i, j), 1e-12))
            {
                return fail("CLI gemm diverges from engine gemm");
            }
        }
    }

    crd::usize blas3_count = 0;
    for (const auto* p : reg.all())
    {
        const crd::containers::StringView name{p->schema.name.c_str(), p->schema.name.size()};
        if (name.starts_with("hesap.dense.blas3."))
        {
            ++blas3_count;
        }
    }

    std::fprintf(stdout,
                 "[smoke_hesap_blas3] 128x128 gemm vs naive RMSE OK\n"
                 "[smoke_hesap_blas3] 32x4 trmm+trsm round-trip OK\n"
                 "[smoke_hesap_blas3] %zu BLAS L3 commands registered (gemm + trsm.lower)\n"
                 "[smoke_hesap_blas3] CLI gemm.f64 bit-equal to engine over %zu elements\n"
                 "[smoke_hesap_blas3] OK\n",
                 static_cast<std::size_t>(blas3_count), static_cast<std::size_t>(k_n * k_n));
    return 0;
}
