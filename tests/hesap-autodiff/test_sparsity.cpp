// test_sparsity.cpp — Phase 3.1.6 v15-e: Jacobian sparsity detection + coloring + compressed recovery. Gates: the
// traced pattern == the analytic structure (tridiagonal, arrowhead); distance-2 coloring is VALID (no two
// co-occurring columns share a color) and minimal (tridiag → 3); compressed recovery == the dense Jacobian; and the
// structural-vs-numerical zero distinction (global keeps x*0).

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;
namespace cc = crd::containers;

namespace
{
struct Tridiag // y_i depends on x_{i-1}, x_i, x_{i+1}
{
    template <class T>
    void operator()(const T* x, int n, T* y, int /*m*/) const
    {
        using std::sin;
        for (int i = 0; i < n; ++i)
        {
            T acc = x[i] * x[i] + sin(x[i]);
            if (i > 0) { acc = acc + 2.0 * x[i - 1]; }
            if (i < n - 1) { acc = acc + x[i + 1]; }
            y[i] = acc;
        }
    }
};
struct Arrow // y_0 dense (depends on all); y_i (i>0) depends on x_0 and x_i
{
    template <class T>
    void operator()(const T* x, int n, T* y, int /*m*/) const
    {
        T s = x[0] * x[0];
        for (int i = 1; i < n; ++i) { s = s + x[i] * x[i]; }
        y[0] = s;
        for (int i = 1; i < n; ++i) { y[i] = x[0] * x[i]; }
    }
};
struct MulZero // y_0 = x0*0 + x1 : the GLOBAL pattern keeps {0,1} (coefficient 0 unknown at trace time)
{
    template <class T>
    void operator()(const T* x, int /*n*/, T* y, int /*m*/) const
    {
        y[0] = x[0] * 0.0 + x[1];
    }
};
// Hessian interaction battery (scalar functors f: R^n → R).
struct HMulXY { template <class T> T operator()(const T* x, int /*n*/) const { return x[0] * x[1]; } };
struct HSinX  { template <class T> T operator()(const T* x, int /*n*/) const { using std::sin; return sin(x[0]); } };
struct HAddXY { template <class T> T operator()(const T* x, int /*n*/) const { return x[0] + x[1]; } };
struct HSumSq { template <class T> T operator()(const T* x, int /*n*/) const { return x[0] * x[0] + x[1] * x[1]; } };
struct HSqSum { template <class T> T operator()(const T* x, int /*n*/) const { T a = x[0] + x[1]; return a * a; } };
struct SparseHess // f = Σ x_i² + Σ sin(x_i·x_{i+1}) → tridiagonal Hessian
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        using std::sin;
        T acc = x[0] * x[0];
        for (int i = 1; i < n; ++i) { acc = acc + x[i] * x[i]; }
        for (int i = 0; i < n - 1; ++i) { acc = acc + sin(x[i] * x[i + 1]); }
        return acc;
    }
};
} // namespace

TEST_CASE("Jacobian pattern == analytic (tridiagonal)", "[autodiff][sparsity]")
{
    constexpr int     n = 16;
    ad::JacPattern<1> rows[n];
    ad::JacPattern<1> sc[n];
    ad::trace_jacobian<1>(Tridiag{}, n, n, rows, sc);
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < n; ++k)
        {
            const bool expect = (k == i) || (k == i - 1) || (k == i + 1);
            CHECK(rows[i].has(k) == expect);
        }
    }
}

TEST_CASE("distance-2 coloring is valid + minimal", "[autodiff][sparsity]")
{
    constexpr int     n = 16;
    ad::JacPattern<1> rows[n];
    ad::JacPattern<1> sc[n];
    ad::trace_jacobian<1>(Tridiag{}, n, n, rows, sc);
    int       color[n];
    const int ncol = ad::distance2_color<1>(rows, n, n, color);
    CHECK(ncol == 3); // tridiagonal distance-2 chromatic number
    // validity: no two columns co-occurring in a row share a color
    for (int i = 0; i < n; ++i)
    {
        for (int a = 0; a < n; ++a)
        {
            for (int b = a + 1; b < n; ++b)
            {
                if (rows[i].has(a) && rows[i].has(b)) { CHECK(color[a] != color[b]); }
            }
        }
    }
}

TEST_CASE("compressed recovery == dense Jacobian (tridiagonal + arrowhead)", "[autodiff][sparsity]")
{
    auto check = [](auto functor, auto tag) {
        constexpr int n = decltype(tag)::value;
        crd::f64      x[n];
        for (int i = 0; i < n; ++i) { x[i] = 0.2 + 0.1 * i; }

        // dense reference (v15-d driver)
        ad::JetPackD<8> jsc[n];
        ad::JetPackD<8> jys[n];
        crd::f64        dense[n * n];
        ad::jacobian<8>(functor, cc::ConstSpan<crd::f64>(x, n), n, cc::Span<crd::f64>(dense, n * n),
                        cc::Span<ad::JetPackD<8>>(jsc, n), cc::Span<ad::JetPackD<8>>(jys, n));

        // sparse pipeline
        ad::JacPattern<1> rows[n];
        ad::JacPattern<1> tsc[n];
        ad::trace_jacobian<1>(functor, n, n, rows, tsc);
        int       color[n];
        const int ncol = ad::distance2_color<1>(rows, n, n, color);
        crd::f64  sparse[n * n];
        crd::f64  v[n];
        crd::f64  bcol[n];
        crd::f64  bmat[n * n];
        ad::Dual<crd::f64> ds[n];
        ad::Dual<crd::f64> dy[n];
        ad::sparse_jacobian<1>(functor, cc::ConstSpan<crd::f64>(x, n), n, rows, color, ncol,
                               cc::Span<crd::f64>(sparse, n * n), cc::Span<crd::f64>(v, n), cc::Span<crd::f64>(bcol, n),
                               cc::Span<crd::f64>(bmat, n * ncol), cc::Span<ad::Dual<crd::f64>>(ds, n),
                               cc::Span<ad::Dual<crd::f64>>(dy, n));
        for (int e = 0; e < n * n; ++e) { CHECK_THAT(sparse[e], WithinRel(dense[e], 1e-12)); }
    };
    check(Tridiag{}, std::integral_constant<int, 16>{});
    check(Arrow{}, std::integral_constant<int, 12>{});
}

TEST_CASE("global tracer keeps structural (not numerical) zeros: x*0", "[autodiff][sparsity]")
{
    ad::JacPattern<1> rows[1];
    ad::JacPattern<1> sc[2];
    ad::trace_jacobian<1>(MulZero{}, 2, 1, rows, sc);
    CHECK(rows[0].has(0)); // kept structurally
    CHECK(rows[0].has(1));
}

TEST_CASE("Hessian sparsity: interaction battery (5-flag)", "[autodiff][sparsity][hessian]")
{
    ad::HessPattern<8> sc[2];
    ad::HessPattern<8> out;
    ad::trace_hessian<8>(HMulXY{}, 2, sc, out); // x0*x1 → CROSS (0,1) only
    CHECK(out.has_pair(0, 1));
    CHECK_FALSE(out.has_pair(0, 0));
    CHECK_FALSE(out.has_pair(1, 1));
    ad::trace_hessian<8>(HSinX{}, 2, sc, out); // sin(x0) → SELF (0,0)
    CHECK(out.has_pair(0, 0));
    CHECK_FALSE(out.has_pair(0, 1));
    ad::trace_hessian<8>(HAddXY{}, 2, sc, out); // x0+x1 → NONE
    CHECK_FALSE(out.has_pair(0, 0));
    CHECK_FALSE(out.has_pair(0, 1));
    CHECK_FALSE(out.has_pair(1, 1));
    ad::trace_hessian<8>(HSumSq{}, 2, sc, out); // x0²+x1² → (0,0),(1,1) NOT (0,1)
    CHECK(out.has_pair(0, 0));
    CHECK(out.has_pair(1, 1));
    CHECK_FALSE(out.has_pair(0, 1));
    ad::trace_hessian<8>(HSqSum{}, 2, sc, out); // (x0+x1)² → DENSE (0,0),(0,1),(1,1)
    CHECK(out.has_pair(0, 0));
    CHECK(out.has_pair(0, 1));
    CHECK(out.has_pair(1, 1));
}

TEST_CASE("sparse Hessian recovery == dense hyper-dual Hessian", "[autodiff][sparsity][hessian]")
{
    constexpr int n = 8;
    crd::f64      x[n];
    for (int i = 0; i < n; ++i) { x[i] = 0.3 + 0.1 * i; }

    // dense reference (v15-c hyper-dual)
    crd::f64 dense[n * n];
    ad::hessian<n>(SparseHess{}, x, dense);

    // sparse: trace pattern → upper-triangle CSR → ε2-tiled recovery
    ad::HessPattern<8> hsc[n];
    ad::HessPattern<8> h;
    ad::trace_hessian<8>(SparseHess{}, n, hsc, h);
    int       row_ptr[n + 1];
    int       col_idx[n * n];
    const int nnz = ad::build_hess_csr<8>(h, n, row_ptr, col_idx);
    CHECK(nnz == 2 * n - 1); // tridiagonal upper triangle: n diagonal + (n-1) super
    crd::f64       values[n * n];
    ad::HessRow<4> hrsc[n];
    ad::sparse_hessian<4>(SparseHess{}, x, n, row_ptr, col_idx, values, hrsc);

    for (int i = 0; i < n; ++i) // each recovered nonzero == dense
    {
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e)
        {
            CHECK_THAT(values[e], WithinRel(dense[i * n + col_idx[e]], 1e-12));
        }
    }
    for (int i = 0; i < n; ++i) // pattern ⊇ numerical nonzeros (upper triangle)
    {
        for (int j = i; j < n; ++j)
        {
            if (std::abs(dense[i * n + j]) > 1e-9) { CHECK(h.has_pair(i, j)); }
        }
    }
}

TEST_CASE("local tracer drops numerical zeros + resolves min (vs global keeps)", "[autodiff][sparsity][local]")
{
    using L = ad::JacLocal<1>;
    // f = x0*0 + x1 : the LOCAL pattern drops {0} (x0*0 is numerically dead here); global kept {0,1} (above test).
    L a[2] = {L::seed(0.5, 0), L::seed(0.7, 1)};
    L f    = a[0] * 0.0 + a[1];
    CHECK_FALSE(f.has(0)); // dropped locally
    CHECK(f.has(1));
    // min resolves on the value: 0.3 < 0.8 → carries branch 0's deps only.
    L b[2] = {L::seed(0.3, 0), L::seed(0.8, 1)};
    L m    = ad::min(b[0], b[1]);
    CHECK(m.v == 0.3);
    CHECK(m.has(0));
    CHECK_FALSE(m.has(1));
    // abs resolves the sign branch: |−0.4| carries the operand's deps with a flipped value.
    L c = ad::abs(L::seed(-0.4, 0));
    CHECK(c.v == 0.4);
    CHECK(c.has(0));
}
