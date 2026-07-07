#pragma once

// bicoloring.hpp — Phase 3.1.6 v16-c: BIDIRECTIONAL (row+column) Jacobian coloring — the reverse-mode complement to
// v15-e's unidirectional column coloring. A sparse Jacobian with BOTH a dense row and a dense column defeats any
// one-directional scheme: a dense row forces every column into its own color (ncol = n), a dense column forces every
// row apart (nrow = m). Bicoloring recovers the dense ROWS by REVERSE (tape VJP) sweeps and the remaining SPARSE-row
// entries by FORWARD (JVP) sweeps — total sweeps = ncol_fwd + nrow_rev, which collapses to O(1) on bordered /
// arrowhead systems. Hossain-Steihaug / Coleman-Verma bidirectional partition. ColPack has the coloring but no
// integrated tracer; there is no deterministic C++ trace -> bicolor -> recover incumbent (arXiv:2505.07308, 2025).
// This is the reverse-mode partner of v15-e's tracer — one module holds forward + reverse, exactly what this needs.
// ADR-0097.
//
// Correctness (DIRECT recovery, no substitution): among the SPARSE rows, distance-2 column coloring => each sparse
// row has <=1 column of any color, so a forward color-c sweep's B_fwd[i,c] IS J[i,j] for the unique color-c column j
// of sparse row i. Among the DENSE rows, distance-2 row coloring (two dense rows conflict iff they share a column)
// => each column has <=1 dense row of any color, so a reverse row-group-g sweep's B_rev[j,g] IS J[i,j] for the
// unique color-g dense row i touching column j. Every nonzero lives in exactly one side => no collision.

#include <crd/hesap/autodiff/reverse.hpp>         // Tape / Var / make_leaf (the reverse row sweeps)
#include <crd/hesap/autodiff/sparse_jacobian.hpp> // JacPattern / distance2_color / jvp / Dual (the forward col sweeps)

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <bit>

namespace crd::hesap::autodiff::reverse
{
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::hesap::autodiff::forward::Dual;
using crd::hesap::autodiff::forward::JacPattern;

// number of nonzero columns in a row-pattern (= its degree).
template <int W>
[[nodiscard]] inline int pattern_degree(const JacPattern<W>& p) noexcept
{
    int d = 0;
    for (int w = 0; w < W; ++w) { d += std::popcount(p.bits[w]); }
    return d;
}
// do two row-patterns share a column? (dense-row conflict test)
template <int W>
[[nodiscard]] inline bool pattern_intersects(const JacPattern<W>& a, const JacPattern<W>& b) noexcept
{
    for (int w = 0; w < W; ++w)
    {
        if ((a.bits[w] & b.bits[w]) != 0U) { return true; }
    }
    return false;
}

// Partition + color for a FIXED density threshold tau (a row is DENSE, reverse-recovered, iff degree >= tau).
// Writes is_dense[m], row_color[m] (dense rows -> [0,nrow), else -1), col_color[n] (cols in a sparse row -> [0,ncol),
// else -1). Colors index into a JacPattern<W> forbidden set, so W*64 must cover max(n,m).
template <int W>
inline void bicolor_partition(const JacPattern<W>* rows, int m, int n, int tau, bool* is_dense, int* row_color,
                              int* col_color, int& ncol, int& nrow) noexcept
{
    for (int i = 0; i < m; ++i) { is_dense[i] = pattern_degree(rows[i]) >= tau; }

    // ---- forward side: distance-2 column coloring over the SPARSE rows only ----
    for (int j = 0; j < n; ++j) { col_color[j] = -1; }
    ncol = 0;
    for (int j = 0; j < n; ++j)
    {
        JacPattern<W> nb{}; // union of the sparse rows containing column j (its distance-2 neighbourhood)
        bool          present = false;
        for (int i = 0; i < m; ++i)
        {
            if (!is_dense[i] && rows[i].has(j))
            {
                nb      = nb | rows[i];
                present = true;
            }
        }
        if (!present) { continue; } // column j has no forward-recovered entry (only dense-row nonzeros, or none)
        JacPattern<W> forbidden{};
        for (int k = 0; k < n; ++k)
        {
            if (col_color[k] >= 0 && nb.has(k))
            {
                forbidden.bits[col_color[k] >> 6] |= crd::u64{1} << (col_color[k] & 63);
            }
        }
        int c = 0;
        while ((forbidden.bits[c >> 6] >> (c & 63)) & crd::u64{1}) { ++c; }
        col_color[j] = c;
        if (c + 1 > ncol) { ncol = c + 1; }
    }

    // ---- reverse side: distance-2 row coloring over the DENSE rows (conflict iff two dense rows share a column) ----
    for (int i = 0; i < m; ++i) { row_color[i] = -1; }
    nrow = 0;
    for (int i = 0; i < m; ++i)
    {
        if (!is_dense[i]) { continue; }
        JacPattern<W> forbidden{};
        for (int i2 = 0; i2 < m; ++i2)
        {
            if (i2 != i && is_dense[i2] && row_color[i2] >= 0 && pattern_intersects(rows[i], rows[i2]))
            {
                forbidden.bits[row_color[i2] >> 6] |= crd::u64{1} << (row_color[i2] & 63);
            }
        }
        int c = 0;
        while ((forbidden.bits[c >> 6] >> (c & 63)) & crd::u64{1}) { ++c; }
        row_color[i] = c;
        if (c + 1 > nrow) { nrow = c + 1; }
    }
}

// Auto: sweep the density threshold over [1, maxdeg+1] and keep the partition minimising total sweeps (ncol + nrow).
// tau = maxdeg+1 => no dense rows => pure forward (== the v15-e unidirectional column coloring), so auto NEVER loses
// to unidirectional. Allocation-free (recomputes the winner into the output arrays). Returns the total sweep count.
template <int W>
inline int bicolor_auto(const JacPattern<W>* rows, int m, int n, bool* is_dense, int* row_color, int* col_color,
                        int& ncol, int& nrow) noexcept
{
    int maxdeg = 0;
    for (int i = 0; i < m; ++i)
    {
        const int d = pattern_degree(rows[i]);
        if (d > maxdeg) { maxdeg = d; }
    }
    int best_total = n + m + 1;
    int best_tau   = maxdeg + 1;
    // Iterate tau DESCENDING (tau = maxdeg+1 is the pure-forward unidirectional baseline). With a strict `<`, ties
    // keep the largest tau => the MOST-forward partition — forward JVP sweeps are cheaper than building + replaying
    // the reverse tape, so a bidirectional split is chosen only when it STRICTLY cuts the total sweep count.
    for (int tau = maxdeg + 1; tau >= 1; --tau)
    {
        bicolor_partition(rows, m, n, tau, is_dense, row_color, col_color, ncol, nrow);
        const int total = ncol + nrow;
        if (total < best_total)
        {
            best_total = total;
            best_tau   = tau;
        }
    }
    bicolor_partition(rows, m, n, best_tau, is_dense, row_color, col_color, ncol, nrow);
    return best_total;
}

// Recover the FULL Jacobian by bicoloring. `f` is a generic vector functor `void f(const T* x, int n, T* y, int m)`
// valid for BOTH T = Dual<f64> (forward column sweeps) and T = Var (reverse row sweeps). Writes dense jac[m*n]
// (structural zeros stay 0). Scratch (caller-owned): forward v[n], bcol[m], bmat_f[m*ncol], ds[n], dy[m]; reverse
// `tape` + xs[n], ys[m], bmat_r[n*nrow].
template <int W, class F>
inline void bicolor_recover(const F& f, ConstSpan<crd::f64> x, int m, const JacPattern<W>* rows, const bool* is_dense,
                            const int* row_color, const int* col_color, int ncol, int nrow, Span<crd::f64> jac,
                            Span<crd::f64> v, Span<crd::f64> bcol, Span<crd::f64> bmat_f, Span<Dual<crd::f64>> ds,
                            Span<Dual<crd::f64>> dy, Tape& tape, Span<Var> xs, Span<Var> ys,
                            Span<crd::f64> bmat_r) noexcept
{
    const int n = static_cast<int>(x.size());
    // forward: ncol JVP sweeps -> B_fwd (m x ncol)
    for (int c = 0; c < ncol; ++c)
    {
        for (int j = 0; j < n; ++j) { v[j] = (col_color[j] == c) ? 1.0 : 0.0; }
        forward::jvp(f, x, ConstSpan<crd::f64>(v.data(), n), m, bcol, ds, dy);
        for (int i = 0; i < m; ++i) { bmat_f[i * ncol + c] = bcol[i]; }
    }
    // reverse: build the Var graph ONCE, then nrow backward sweeps -> B_rev (n x nrow)
    if (nrow > 0)
    {
        tape.reset();
        for (int i = 0; i < n; ++i) { xs[i] = make_leaf(tape, x[i]); }
        f(xs.data(), n, ys.data(), m);
        for (int g = 0; g < nrow; ++g)
        {
            tape.zero_adjoints();
            for (int i = 0; i < m; ++i)
            {
                if (is_dense[i] && row_color[i] == g) { tape.seed(ys[i].node, 1.0); }
            }
            tape.backward();
            for (int j = 0; j < n; ++j) { bmat_r[j * nrow + g] = tape.grad(xs[j].node); }
        }
    }
    // direct recovery: each nonzero reads its side's compressed entry; structural zeros stay 0.
    for (int i = 0; i < m; ++i)
    {
        for (int k = 0; k < n; ++k)
        {
            if (!rows[i].has(k)) { jac[i * n + k] = 0.0; }
            else if (is_dense[i]) { jac[i * n + k] = bmat_r[k * nrow + row_color[i]]; }
            else { jac[i * n + k] = bmat_f[i * ncol + col_color[k]]; }
        }
    }
}

} // namespace crd::hesap::autodiff::reverse
