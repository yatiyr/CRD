// test_bicoloring.cpp — Phase 3.1.6 v16-c: BIDIRECTIONAL (row+column) Jacobian coloring — the reverse-mode complement
// to v15-e's unidirectional column coloring. Gate on an ARROWHEAD system (dense row 0 + a dense column 0 + a
// diagonal): unidirectional column coloring needs n sweeps (the dense row forces every column apart); bicoloring
// recovers the dense row by ONE reverse sweep and the sparse remainder by TWO forward sweeps => 3 total, and the gap
// grows with n. Correctness: the recovered Jacobian equals the analytic one (independent oracle) exactly on the
// nonzeros with structural zeros preserved; determinism: bit-identical run to run.

#include <crd/hesap/autodiff/bicoloring.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace fwd = crd::hesap::autodiff::forward;
namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kN = 17; // n == m
f64           g_a[kN];
f64           g_b[kN];

// Arrowhead vector functor, scalar-generic (JacPattern for the trace, Dual for forward sweeps, Var for reverse).
//   row 0 (DENSE):   y0   = Σ_j a_j·sin(x_j)                (depends on every input)
//   rows i>0:        y_i  = x_i·x_i + b_i·x_0               (depends on x_i and x_0 => a dense column 0)
struct Arrowhead
{
    template <class T>
    void operator()(const T* x, int n, T* y, int /*m*/) const
    {
        T s = g_a[0] * sin(x[0]);
        for (int j = 1; j < n; ++j) { s = s + g_a[j] * sin(x[j]); }
        y[0] = s;
        for (int i = 1; i < n; ++i) { y[i] = x[i] * x[i] + g_b[i] * x[0]; }
    }
};

// Pure diagonal (no dense row) — bicoloring must degrade gracefully to the unidirectional single sweep.
struct Diag
{
    template <class T>
    void operator()(const T* x, int /*n*/, T* y, int m) const
    {
        for (int i = 0; i < m; ++i) { y[i] = sin(x[i]); }
    }
};
} // namespace

TEST_CASE("v16-c: bicoloring crushes unidirectional on an arrowhead Jacobian, exact recovery", "[autodiff][reverse][bicolor]")
{
    for (int i = 0; i < kN; ++i)
    {
        g_a[i] = 0.3 + 0.1 * std::sin(1.0 + i);
        g_b[i] = 0.2 * std::cos(0.5 + i);
    }
    constexpr int w = 1; // w*64 >= max(n,m) = 17
    f64           x[kN];
    for (int i = 0; i < kN; ++i) { x[i] = 0.4 * std::sin(0.7 + 1.1 * i); }

    // trace the structural pattern
    fwd::JacPattern<w> rows[kN];
    fwd::JacPattern<w> scratch[kN];
    fwd::trace_jacobian<w>(Arrowhead{}, kN, kN, rows, scratch);

    // unidirectional column coloring (v15-e) — the baseline the reverse complement must beat
    int uni_color[kN];
    const int ncol_uni = fwd::distance2_color<w>(rows, kN, kN, uni_color);

    // bicoloring (auto threshold)
    bool is_dense[kN];
    int  row_color[kN];
    int  col_color[kN];
    int  ncol = 0;
    int  nrow = 0;
    const int total = rev::bicolor_auto<w>(rows, kN, kN, is_dense, row_color, col_color, ncol, nrow);

    // ★ CRUSH: a dense row forces unidirectional to n colors; bicoloring recovers it by one reverse sweep => 3 total.
    CHECK(ncol_uni == kN);         // 17
    CHECK(total == ncol + nrow);
    CHECK(total == 3);             // ncol=2 (col 0 + the diagonal columns) + nrow=1 (the dense row)
    CHECK(total < ncol_uni);

    // recover the full Jacobian by bicoloring
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::Tape                  tape(&alloc);
    f64                        jac[kN * kN];
    f64                        jac2[kN * kN];
    f64                        v[kN];
    f64                        bcol[kN];
    f64                        bmat_f[kN * kN];
    f64                        bmat_r[kN * kN];
    fwd::Dual<f64>             ds[kN];
    fwd::Dual<f64>             dy[kN];
    rev::Var                   xs[kN];
    rev::Var                   ys[kN];
    rev::bicolor_recover<w>(Arrowhead{}, {x, kN}, kN, rows, is_dense, row_color, col_color, ncol, nrow, {jac, kN * kN},
                            {v, kN}, {bcol, kN}, {bmat_f, kN * kN}, {ds, kN}, {dy, kN}, tape, {xs, kN}, {ys, kN},
                            {bmat_r, kN * kN});

    // analytic Jacobian (independent oracle): row 0 dense = a_j cos(x_j); rows i>0 = {col 0: b_i, col i: 2 x_i}.
    f64 an[kN * kN] = {};
    for (int j = 0; j < kN; ++j) { an[0 * kN + j] = g_a[j] * std::cos(x[j]); }
    for (int i = 1; i < kN; ++i)
    {
        an[i * kN + 0] = g_b[i];
        an[i * kN + i] = 2.0 * x[i];
    }
    for (int i = 0; i < kN; ++i)
    {
        for (int k = 0; k < kN; ++k) { CHECK_THAT(jac[i * kN + k], WithinAbs(an[i * kN + k], 1e-10)); }
    }

    // determinism: recover again, bit-identical
    rev::bicolor_recover<w>(Arrowhead{}, {x, kN}, kN, rows, is_dense, row_color, col_color, ncol, nrow,
                            {jac2, kN * kN}, {v, kN}, {bcol, kN}, {bmat_f, kN * kN}, {ds, kN}, {dy, kN}, tape,
                            {xs, kN}, {ys, kN}, {bmat_r, kN * kN});
    for (int i = 0; i < kN * kN; ++i) { CHECK(jac[i] == jac2[i]); }
}

TEST_CASE("v16-c: bicoloring degrades gracefully to unidirectional on a diagonal Jacobian", "[autodiff][reverse][bicolor]")
{
    constexpr int w = 1;
    constexpr int n = 12;
    f64           x[n];
    for (int i = 0; i < n; ++i) { x[i] = 0.5 * std::cos(0.3 + i); }

    fwd::JacPattern<w> rows[n];
    fwd::JacPattern<w> scratch[n];
    fwd::trace_jacobian<w>(Diag{}, n, n, rows, scratch);

    bool is_dense[n];
    int  row_color[n];
    int  col_color[n];
    int  ncol = 0;
    int  nrow = 0;
    const int total = rev::bicolor_auto<w>(rows, n, n, is_dense, row_color, col_color, ncol, nrow);
    CHECK(nrow == 0);   // no dense row
    CHECK(total == 1);  // one forward sweep recovers the whole diagonal

    crd::memory::TlsfAllocator alloc(1 << 20);
    rev::Tape                  tape(&alloc);
    f64                        jac[n * n];
    f64                        v[n];
    f64                        bcol[n];
    f64                        bmat_f[n * n];
    f64                        bmat_r[n * n];
    fwd::Dual<f64>             ds[n];
    fwd::Dual<f64>             dy[n];
    rev::Var                   xs[n];
    rev::Var                   ys[n];
    rev::bicolor_recover<w>(Diag{}, {x, n}, n, rows, is_dense, row_color, col_color, ncol, nrow, {jac, n * n},
                            {v, n}, {bcol, n}, {bmat_f, n * n}, {ds, n}, {dy, n}, tape, {xs, n}, {ys, n},
                            {bmat_r, n * n});
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < n; ++k)
        {
            const f64 expect = (i == k) ? std::cos(x[i]) : 0.0;
            CHECK_THAT(jac[i * n + k], WithinAbs(expect, 1e-10));
        }
    }
}
