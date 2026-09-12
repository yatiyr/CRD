#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/linear_op_dense.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/randomized_range.hpp>
#include <crd/hesap/dense/svd.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::dense::counter_gaussian;
using crd::hesap::dense::eig_sym;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::MatrixLinearOp;
using crd::hesap::dense::randomized_range;
using crd::hesap::dense::rsvd_op;
using crd::hesap::dense::rsyev_op;
using crd::hesap::dense::svd;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::SymmetricLinearOp;
using Catch::Matchers::WithinAbs;

namespace
{
template <typename T>
using Mat = Matrix<T, Layout::RowMajor>;

template <typename T>
T prand(crd::usize i, crd::usize j, T scale) noexcept
{
    return static_cast<T>(std::sin(static_cast<double>(i * 13 + j * 7 + 1) * 0.37) +
                          std::cos(static_cast<double>(i * 5 + j * 11 + 3) * 0.21)) *
           scale;
}

// A = B·C with B (m×k), C (k×n) ⇒ exact rank min(k,m,n).
template <typename T>
Mat<T> make_low_rank(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n, crd::usize k)
{
    Mat<T> b(alloc, m, k);
    Mat<T> c(alloc, k, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize t = 0; t < k; ++t)
        {
            b.at(i, t) = prand<T>(i, t, static_cast<T>(1));
        }
    }
    for (crd::usize t = 0; t < k; ++t)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            c.at(t, j) = prand<T>(t + 100, j, static_cast<T>(1));
        }
    }
    Mat<T> a(alloc, m, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::usize t = 0; t < k; ++t)
            {
                s += b.at(i, t) * c.at(t, j);
            }
            a.at(i, j) = s;
        }
    }
    return a;
}

// ‖(I − QQᵀ)A‖_F : how much of A's range Q fails to capture.
template <typename T>
T residual_norm(crd::memory::IAllocator* alloc, const Mat<T>& a, const Mat<T>& q) noexcept
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const crd::usize ell = q.cols();
    crd::containers::Array<T> c(alloc);
    c.resize(ell);
    T acc = T{0};
    for (crd::usize j = 0; j < n; ++j)
    {
        for (crd::usize p = 0; p < ell; ++p)
        {
            T s = T{0};
            for (crd::usize i = 0; i < m; ++i)
            {
                s += q.at(i, p) * a.at(i, j);
            }
            c[p] = s;
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            T proj = T{0};
            for (crd::usize p = 0; p < ell; ++p)
            {
                proj += q.at(i, p) * c[p];
            }
            const T d = a.at(i, j) - proj;
            acc += d * d;
        }
    }
    return std::sqrt(acc);
}

// A = U diag(rho^i) Vᵀ with U,V from the SVD of a random matrix ⇒ A has an
// EXACTLY controlled (geometric) spectrum. Lets the power-iteration test
// guarantee a strict residual improvement.
template <typename T>
Mat<T> make_controlled_spectrum(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n, T rho)
{
    Mat<T> rnd(alloc, m, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            rnd.at(i, j) = prand<T>(i + 7, j + 3, static_cast<T>(1));
        }
    }
    const auto s = svd<T>(alloc, rnd);  // u: m×min, v: n×min
    const crd::usize mn = m < n ? m : n;
    Mat<T> a(alloc, m, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T acc = T{0};
            for (crd::usize p = 0; p < mn; ++p)
            {
                acc += s.u.at(i, p) * std::pow(rho, static_cast<T>(p)) * s.v.at(j, p);
            }
            a.at(i, j) = acc;
        }
    }
    return a;
}

// Diagonal operator that DOES NOT advertise a transpose — exercises the
// has_transpose() guard (rsvd_op refuses; randomized_range clamps power iters).
class DiagNoTransposeOp : public crd::hesap::LinearOp<double>
{
public:
    DiagNoTransposeOp(const double* d, crd::usize n) noexcept
        : crd::hesap::LinearOp<double>(/*has_transpose*/ false, /*has_adjoint*/ false), m_d(d), m_n(n)
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<double> x, crd::containers::Span<double> y) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = m_d[i] * x[i];
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    const double* m_d;
    crd::usize m_n;
};
} // namespace

TEST_CASE("randomized_range: captures the range of a low-rank operator", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize m = 40;
    constexpr crd::usize n = 30;
    constexpr crd::usize k = 5;
    Mat<double> a = make_low_rank<double>(&alloc, m, n, k);
    MatrixLinearOp<double> op(a);

    auto rb = randomized_range<double>(&alloc, op, k);  // oversampling makes ell > k
    REQUIRE(rb.rank >= k);
    REQUIRE(rb.q.rows() == m);
    REQUIRE(rb.q.cols() == rb.rank);
    // Q has orthonormal columns (QᵀQ = I).
    for (crd::usize p = 0; p < rb.rank; ++p)
    {
        for (crd::usize qq = 0; qq < rb.rank; ++qq)
        {
            double dot = 0.0;
            for (crd::usize i = 0; i < m; ++i)
            {
                dot += rb.q.at(i, p) * rb.q.at(i, qq);
            }
            CHECK_THAT(dot, WithinAbs(p == qq ? 1.0 : 0.0, 1e-9));
        }
    }
    // Exact rank-k ⇒ the captured range is exact.
    CHECK(residual_norm<double>(&alloc, a, rb.q) < 1e-9);
}

TEST_CASE("randomized_range: power iteration sharpens a slow-decay spectrum", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    constexpr crd::usize m = 40;
    constexpr crd::usize n = 30;
    Mat<double> a = make_controlled_spectrum<double>(&alloc, m, n, 0.7);
    MatrixLinearOp<double> op(a);

    auto rb0 = randomized_range<double>(&alloc, op, 5, /*oversampling*/ 2, /*power_iters*/ 0);
    auto rb2 = randomized_range<double>(&alloc, op, 5, /*oversampling*/ 2, /*power_iters*/ 3);
    const double r0 = residual_norm<double>(&alloc, a, rb0.q);
    const double r2 = residual_norm<double>(&alloc, a, rb2.q);
    CHECK(r2 < r0);  // power iteration drives Q toward the dominant subspace
}

TEST_CASE("rsvd_op: matches the dense SVD spectrum (oracle)", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize m = 32;
    constexpr crd::usize n = 24;
    constexpr crd::usize k = 4;
    Mat<double> a = make_low_rank<double>(&alloc, m, n, k);
    MatrixLinearOp<double> op(a);

    const auto truth = svd<double>(&alloc, a);  // ground-truth singular values (descending)
    const auto r = rsvd_op<double>(&alloc, op, k);
    REQUIRE(r.s.size() == k);
    REQUIRE(r.u.rows() == m);
    REQUIRE(r.u.cols() == k);
    REQUIRE(r.v.rows() == n);
    REQUIRE(r.v.cols() == k);
    for (crd::usize i = 0; i < k; ++i)
    {
        CHECK_THAT(r.s.data()[i], WithinAbs(truth.s.data()[i], 1e-8));
    }
    // Vectors too (the U = Q·U_b lift + V extraction): A is exact rank-k ⇒ the
    // top-k reconstructs A exactly. ‖A − U·diag(S)·Vᵀ‖_F.
    double recon2 = 0.0;
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < k; ++p)
            {
                s += r.u.at(i, p) * r.s.data()[p] * r.v.at(j, p);
            }
            const double dd = a.at(i, j) - s;
            recon2 += dd * dd;
        }
    }
    CHECK(std::sqrt(recon2) < 1e-9);
}

TEST_CASE("rsyev_op: matches the dense symmetric eig (oracle)", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize n = 28;
    constexpr crd::usize k = 4;
    // A = B Bᵀ ⇒ PSD, rank min(k_inner, n); use k_inner = 6 so the top-4 are well separated.
    constexpr crd::usize k_inner = 6;
    Mat<double> b = make_low_rank<double>(&alloc, n, n, k_inner);  // reuse: gives a rank-6 n×n
    Symmetric<double> a(&alloc, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                s += b.at(i, p) * b.at(j, p);  // (B Bᵀ)_{ij}
            }
            a.at(i, j) = s;
        }
    }
    SymmetricLinearOp<double> op(a);

    const auto truth = eig_sym<double>(&alloc, a);  // ascending values
    const auto r = rsyev_op<double>(&alloc, op, k);
    REQUIRE(r.values.size() == k);
    REQUIRE(r.vectors.rows() == n);
    REQUIRE(r.vectors.cols() == k);
    // rsyev_op returns the k largest (top of spectrum), descending.
    for (crd::usize idx = 0; idx < k; ++idx)
    {
        const double expect = truth.values.data()[n - 1 - idx];
        CHECK_THAT(r.values.data()[idx], WithinAbs(expect, 1e-7));
    }
    // Vectors too (the V = Q·V_b lift): each returned pair satisfies the
    // eigen-relation ‖A·v − λ·v‖ ≈ 0 (A·v via op.apply ⇒ also exercises the op).
    crd::containers::Array<double> v(&alloc);
    crd::containers::Array<double> av(&alloc);
    v.resize(n);
    av.resize(n);
    for (crd::usize idx = 0; idx < k; ++idx)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            v[i] = r.vectors.at(i, idx);
        }
        REQUIRE(op.apply(crd::containers::ConstSpan<double>{v.data(), n}, crd::containers::Span<double>{av.data(), n}));
        const double lam = r.values.data()[idx];
        double res2 = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            const double dd = av[i] - lam * v[i];
            res2 += dd * dd;
        }
        CHECK(std::sqrt(res2) < 1e-7);
    }
}

TEST_CASE("counter_gaussian: order-independent (reproducibility primitive)", "[hesap][rrange][real]")
{
    // The counter-based RNG is a PURE function of (seed, idx): filling forward
    // vs reverse gives bit-identical values. This is the thread-independence
    // primitive the v5e-2 cross-thread moat is built on (NOT a moat claim here;
    // v5e-1b is serial).
    constexpr crd::u64 seed = 0xABCDEF12ULL;
    constexpr crd::usize count = 257;
    crd::containers::Array<double> fwd(crd::memory::default_allocator());
    crd::containers::Array<double> rev(crd::memory::default_allocator());
    fwd.resize(count);
    rev.resize(count);
    for (crd::usize i = 0; i < count; ++i)
    {
        fwd[i] = counter_gaussian<double>(seed, static_cast<crd::u64>(i));
    }
    for (crd::usize ii = count; ii-- > 0;)
    {
        rev[ii] = counter_gaussian<double>(seed, static_cast<crd::u64>(ii));
    }
    for (crd::usize i = 0; i < count; ++i)
    {
        CHECK(fwd[i] == rev[i]);  // bit-identical
    }
    // Different seed ⇒ a different stream (sanity; not all-equal).
    bool any_diff = false;
    for (crd::usize i = 0; i < count && !any_diff; ++i)
    {
        if (counter_gaussian<double>(seed + 1, static_cast<crd::u64>(i)) != fwd[i])
        {
            any_diff = true;
        }
    }
    CHECK(any_diff);
}

TEST_CASE("randomized_range: deterministic given seed (reproducible)", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    Mat<double> a = make_low_rank<double>(&alloc, 30, 24, 5);
    MatrixLinearOp<double> op(a);
    auto rb1 = randomized_range<double>(&alloc, op, 5);
    auto rb2 = randomized_range<double>(&alloc, op, 5);
    REQUIRE(rb1.rank == rb2.rank);
    for (crd::usize i = 0; i < rb1.q.rows(); ++i)
    {
        for (crd::usize j = 0; j < rb1.rank; ++j)
        {
            CHECK(rb1.q.at(i, j) == rb2.q.at(i, j));  // bit-identical
        }
    }
}

TEST_CASE("rsvd_op: f32 low-rank spectrum", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    Mat<float> a = make_low_rank<float>(&alloc, 28, 20, 3);
    MatrixLinearOp<float> op(a);
    const auto truth = svd<float>(&alloc, a);
    const auto r = rsvd_op<float>(&alloc, op, 3);
    REQUIRE(r.s.size() == 3);
    for (crd::usize i = 0; i < 3; ++i)
    {
        CHECK_THAT(r.s.data()[i], WithinAbs(truth.s.data()[i], 1e-3F));
    }
    // Vector lift, f32: ‖A − U·diag(S)·Vᵀ‖_F on the exact rank-3 input.
    float recon2 = 0.0F;
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            float s = 0.0F;
            for (crd::usize p = 0; p < 3; ++p)
            {
                s += r.u.at(i, p) * r.s.data()[p] * r.v.at(j, p);
            }
            const float dd = a.at(i, j) - s;
            recon2 += dd * dd;
        }
    }
    CHECK(std::sqrt(recon2) < 1e-3F);
}

TEST_CASE("randomized_range / rsvd_op: no-transpose op (guard + clamp)", "[hesap][rrange][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize n = 16;
    crd::containers::Array<double> d(&alloc);
    d.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        d[i] = static_cast<double>(n - i);  // 16, 15, ..., 1 — distinct, decreasing
    }
    DiagNoTransposeOp op(d.data(), n);
    REQUIRE_FALSE(op.has_transpose());

    // randomized_range MUST NOT crash here: power_iters=3 is silently clamped to
    // 0 because the op has no transpose (so apply_transpose is never called —
    // calling it would trip the CRD_ASSERT on the default false return).
    auto rb = randomized_range<double>(&alloc, op, 4, /*oversampling*/ 4, /*power_iters*/ 3);
    REQUIRE(rb.rank >= 4);
    // Build the dense diagonal A to measure the residual against ‖A‖_F.
    Mat<double> a(&alloc, n, n);
    double anorm2 = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        a.at(i, i) = d[i];
        anorm2 += d[i] * d[i];
    }
    const double anorm = std::sqrt(anorm2);
    const double resid = residual_norm<double>(&alloc, a, rb.q);
    // The ell-column basis captures the dominant subspace ⇒ residual well below ‖A‖_F.
    CHECK(std::isfinite(resid));
    CHECK(resid < 0.6 * anorm);

    // rsvd_op refuses (needs a transpose) ⇒ empty result, not garbage.
    const auto r = rsvd_op<double>(&alloc, op, 4);
    CHECK(r.s.size() == 0);
    CHECK(r.u.cols() == 0);
}
