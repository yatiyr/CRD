#include <crd/hesap/direct/sparse_lu.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>

#include <cmath>
#include <type_traits>

namespace crd::hesap::direct
{
namespace
{
// Magnitude for the pivot comparison: |x| for real, modulus (hypot) for complex.
template <typename T> [[nodiscard]] inline dense::RealType<T> lu_mag(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return crd::hesap::abs(x);
    }
    else
    {
        return x < T(0) ? -x : x;
    }
}

// The multiplicative identity (unit L diagonal).
template <typename T> [[nodiscard]] inline T lu_one() noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return T{dense::RealType<T>(1), dense::RealType<T>(0)};
    }
    else
    {
        return T(1);
    }
}

// Reachability of one start row through the graph of the partial L (Gilbert and Peierls,
// "Sparse partial pivoting in time proportional to arithmetic operations", 1988): the rows
// that column k's triangular solve can touch are exactly those reachable from A(:,k)'s
// rows, and a reverse finishing order of the walk is a valid elimination order.
// `pinv[r]` is the L column created when original row r was chosen as a pivot, or <0 while
// r is still unpivoted (such a row has no outgoing edges). The walk is iterative: xi[0..level]
// holds the current path, `cursor[d]` the next L entry to examine for the path node at depth d.
// Finished rows fill xi[--top] downwards, so the output block xi[top..n) never meets the
// path (path length + finished rows <= n). Each L column's first entry is its own unit
// diagonal and is never followed. `marked` is a separate byte array, so the growing `lp`
// is left alone. Returns the new `top`.
[[nodiscard]] crd::u32 lu_dfs(crd::u32 jstart, const crd::u32* lp, const crd::u32* li, const crd::i32* pinv,
                              crd::u32 top, crd::u32* xi, crd::u32* cursor, crd::u8* marked) noexcept
{
    const auto first_edge = [&](crd::u32 row) noexcept -> crd::u32
    {
        const crd::i32 col = pinv[row];
        return col < 0 ? 0U : lp[static_cast<crd::u32>(col)] + 1U;
    };
    crd::u32 level = 0;
    xi[0] = jstart;
    marked[jstart] = 1;
    cursor[0] = first_edge(jstart);
    for (;;)
    {
        const crd::u32 row = xi[level];
        const crd::i32 col = pinv[row];
        const crd::u32 edge_end = col < 0 ? 0U : lp[static_cast<crd::u32>(col) + 1];
        crd::u32 e = cursor[level];
        while (e < edge_end && marked[li[e]] != 0)
        {
            ++e;
        }
        if (e < edge_end)
        {
            const crd::u32 next = li[e];
            cursor[level] = e + 1;
            marked[next] = 1;
            ++level;
            xi[level] = next;
            cursor[level] = first_edge(next);
            continue;
        }
        xi[--top] = row; // every neighbour is done: this row is finished
        if (level == 0)
        {
            return top;
        }
        --level;
    }
}

// Sparse lower-triangular solve for one column of A, after Gilbert and Peierls (1988): finds
// the rows the column reaches in the graph of the partial L, then eliminates them in the
// finishing order the walk produced. `col_rows` / `col_vals` / `col_nnz` describe the column
// being factored. On return order[first..n) lists the touched rows in elimination order and
// work[] holds their values (everything else in work[] is stale). `marked` is back to all-0.
// `path_cursor` is the walk's scratch (see lu_dfs). Returns `first`.
template <typename T>
[[nodiscard]] crd::u32 lu_spsolve(crd::u32 n, const crd::u32* lp, const crd::u32* li, const T* lx,
                                  const crd::u32* col_rows, const T* col_vals, crd::u32 col_nnz, crd::u32* order,
                                  crd::u32* path_cursor, T* work, const crd::i32* pinv, crd::u8* marked)
{
    // Pass 1: walk from every nonzero row of the column that no earlier walk has covered.
    crd::u32 first = n;
    for (crd::u32 q = 0; q < col_nnz; ++q)
    {
        if (marked[col_rows[q]] == 0)
        {
            first = lu_dfs(col_rows[q], lp, li, pinv, first, order, path_cursor, marked);
        }
    }
    // Pass 2: one sweep over the reached rows resets their marks and zeroes their slots.
    for (crd::u32 s = first; s < n; ++s)
    {
        const crd::u32 row = order[s];
        marked[row] = 0;
        work[row] = T{};
    }
    // Pass 3: load the column's own values.
    for (crd::u32 q = 0; q < col_nnz; ++q)
    {
        work[col_rows[q]] = col_vals[q];
    }
    // Pass 4: eliminate in order. Only rows that were already chosen as pivots own an L column;
    // the others hold their final value as they are.
    for (crd::u32 s = first; s < n; ++s)
    {
        const crd::u32 row = order[s];
        const crd::i32 step = pinv[row];
        if (step >= 0)
        {
            const crd::u32 lcol = static_cast<crd::u32>(step);
            const T factor = work[row];
            const crd::u32 lend = lp[lcol + 1];
            for (crd::u32 q = lp[lcol] + 1; q < lend; ++q) // entry lp[lcol] is the unit diagonal
            {
                work[li[q]] = work[li[q]] - lx[q] * factor;
            }
        }
    }
    return first;
}
} // namespace

template <typename T> SparseLU<T>::SparseLU(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_lp(alloc), m_li(alloc), m_lx(alloc), m_up(alloc), m_ui(alloc), m_ux(alloc), m_pinv(alloc)
{
}

template <typename T>
void SparseLU<T>::factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a, double tol)
{
    const sparse::SparsePattern& pat = a.pattern();
    m_n = pat.cols;
    const crd::u32 n = m_n;
    CRD_ASSERT_MSG(pat.rows == pat.cols, "SparseLU requires a square matrix");
    CRD_ASSERT_MSG(pat.is_compressed(), "SparseLU requires a compressed CSC matrix");

    const crd::u32* ap = pat.outer_ptr.data(); // column pointers
    const crd::u32* ai = pat.inner_idx.data(); // row indices
    const T* ax = a.values().values.data();
    const crd::u32 anz = static_cast<crd::u32>(pat.inner_idx.size());

    m_lp.resize(static_cast<crd::usize>(n) + 1);
    m_up.resize(static_cast<crd::usize>(n) + 1);
    // Initial fill estimate = anz + n (grow ×2 on demand — LU fill is unbounded a priori).
    crd::usize cap = static_cast<crd::usize>(anz) + n + 1;
    m_li.resize_uninitialized(cap);
    m_lx.resize_uninitialized(cap);
    m_ui.resize_uninitialized(cap);
    m_ux.resize_uninitialized(cap);

    // Workspaces (serial — no per-worker scratch; this is the serial oracle).
    crd::containers::Array<crd::i32> pinv(m_alloc); // pinv[i] = pivot step or -1
    pinv.resize(n);
    crd::containers::Array<crd::u32> xi(m_alloc); // [0..n) dfs-stack/output + [n..2n) resume offsets (Gilbert-Peierls)
    xi.resize(static_cast<crd::usize>(n) * 2);
    crd::containers::Array<crd::u8> marked(m_alloc);
    marked.resize(n); // value-init 0
    crd::containers::Array<T> x(m_alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        pinv[i] = -1;
    }

    const dense::RealType<T> piv_tol = static_cast<dense::RealType<T>>(tol);
    crd::u32 fill_l = 0; // entries written to L so far
    crd::u32 fill_u = 0; // entries written to U so far
    for (crd::u32 col = 0; col < n; ++col)
    {
        m_lp[col] = fill_l;
        m_up[col] = fill_u;
        // One column adds at most n entries to each factor: make room before writing any of them.
        if (fill_l + n > cap || fill_u + n > cap)
        {
            const crd::usize need = (fill_l > fill_u ? fill_l : fill_u) + n;
            cap = need * 2;
            m_li.resize_uninitialized(cap);
            m_lx.resize_uninitialized(cap);
            m_ui.resize_uninitialized(cap);
            m_ux.resize_uninitialized(cap);
        }
        const crd::u32 first = lu_spsolve<T>(n, m_lp.data(), m_li.data(), m_lx.data(), ai + ap[col], ax + ap[col],
                                             ap[col + 1] - ap[col], xi.data(), xi.data() + n, x.data(), pinv.data(),
                                             marked.data());

        // Walk the reached rows in elimination order. A row that already is a pivot contributes
        // its value to U; among the rest, the one of largest magnitude is the pivot candidate.
        crd::i32 pivot_row = -1;
        dense::RealType<T> best_mag = dense::RealType<T>(-1);
        for (crd::u32 s = first; s < n; ++s)
        {
            const crd::u32 row = xi[s];
            const crd::i32 earlier = pinv[row];
            if (earlier >= 0)
            {
                m_ui[fill_u] = static_cast<crd::u32>(earlier);
                m_ux[fill_u] = x[row];
                ++fill_u;
                continue;
            }
            const dense::RealType<T> mag = lu_mag<T>(x[row]);
            if (mag > best_mag)
            {
                best_mag = mag;
                pivot_row = static_cast<crd::i32>(row);
            }
        }
        if (pivot_row < 0 || !(best_mag > dense::RealType<T>(0)))
        {
            // No usable pivot: the matrix is singular at this column. Report it and stop.
            m_lp[n] = fill_l;
            m_up[n] = fill_u;
            m_info = static_cast<crd::usize>(col) + 1;
            return;
        }
        // Threshold pivoting: keep the diagonal row when it is unpivoted, nonzero and at least
        // tol times as large as the best candidate (tol = 1 degenerates to plain partial pivoting).
        // Choosing the diagonal where possible limits fill, as in SuperLU's threshold option.
        if (pinv[col] < 0)
        {
            const dense::RealType<T> diag_mag = lu_mag<T>(x[col]);
            if (diag_mag > dense::RealType<T>(0) && diag_mag >= piv_tol * best_mag)
            {
                pivot_row = static_cast<crd::i32>(col);
            }
        }
        const T pivot_value = x[static_cast<crd::u32>(pivot_row)];
        pinv[static_cast<crd::u32>(pivot_row)] = static_cast<crd::i32>(col);
        // U's column ends with its diagonal; L's column starts with its unit diagonal.
        m_ui[fill_u] = col;
        m_ux[fill_u] = pivot_value;
        ++fill_u;
        m_li[fill_l] = static_cast<crd::u32>(pivot_row);
        m_lx[fill_l] = lu_one<T>();
        ++fill_l;
        // The remaining unpivoted rows become the sub-diagonal part of L, scaled by the pivot.
        for (crd::u32 s = first; s < n; ++s)
        {
            const crd::u32 row = xi[s];
            if (pinv[row] < 0)
            {
                m_li[fill_l] = row;
                m_lx[fill_l] = x[row] / pivot_value;
                ++fill_l;
            }
        }
    }
    m_lp[n] = fill_l;
    m_up[n] = fill_u;
    // L was filled with original row numbers; translate them to pivot order so P*A = L*U holds.
    for (crd::u32 q = 0; q < fill_l; ++q)
    {
        m_li[q] = static_cast<crd::u32>(pinv[m_li[q]]);
    }
    m_pinv.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        m_pinv[i] = static_cast<crd::u32>(pinv[i]);
    }
    m_lnz = fill_l;
    m_unz = fill_u;
    m_info = 0;
}

template <typename T> bool SparseLU<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return false;
    }
    const crd::u32 n = m_n;
    if (n == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(rhs.size() == static_cast<crd::usize>(n) * nrhs, "SparseLU::solve rhs size mismatch");
    crd::containers::Array<T> x(m_alloc);
    x.resize(n);
    const crd::u32* lp = m_lp.data();
    const crd::u32* li = m_li.data();
    const T* lx = m_lx.data();
    const crd::u32* up = m_up.data();
    const crd::u32* ui = m_ui.data();
    const T* ux = m_ux.data();
    const crd::u32* pinv = m_pinv.data();
    for (crd::usize col = 0; col < nrhs; ++col)
    {
        T* b = rhs.data() + col * n;
        for (crd::u32 i = 0; i < n; ++i) // x = P·b
        {
            x[pinv[i]] = b[i];
        }
        for (crd::u32 j = 0; j < n; ++j) // x = L\x (unit lower CSC, forward)
        {
            const T xj = x[j];
            for (crd::u32 p = lp[j] + 1; p < lp[j + 1]; ++p) // skip the unit diagonal (first entry)
            {
                x[li[p]] = x[li[p]] - lx[p] * xj;
            }
        }
        for (crd::u32 jj = 0; jj < n; ++jj) // x = U\x (upper CSC, backward)
        {
            const crd::u32 j = n - 1 - jj;
            const crd::u32 pdiag = up[j + 1] - 1; // U(j,j) = last entry of column j
            x[j] = x[j] / ux[pdiag];
            const T xj = x[j];
            for (crd::u32 p = up[j]; p < pdiag; ++p) // off-diagonals (rows < j)
            {
                x[ui[p]] = x[ui[p]] - ux[p] * xj;
            }
        }
        for (crd::u32 i = 0; i < n; ++i) // column order is identity ⇒ x is the solution
        {
            b[i] = x[i];
        }
    }
    return true;
}

template <typename T>
SparseLU<T> factor_gp_lu(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a, crd::memory::IAllocator* alloc,
                         double tol)
{
    SparseLU<T> lu(alloc);
    lu.factorize(a, tol);
    return lu;
}

// Explicit instantiations: f32 / f64 / Complex32 / Complex64.
template class SparseLU<crd::f32>;
template class SparseLU<crd::f64>;
template class SparseLU<crd::hesap::Complex32>;
template class SparseLU<crd::hesap::Complex64>;
template SparseLU<crd::f32> factor_gp_lu<crd::f32>(const sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csc>&,
                                                   crd::memory::IAllocator*, double);
template SparseLU<crd::f64> factor_gp_lu<crd::f64>(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csc>&,
                                                   crd::memory::IAllocator*, double);
template SparseLU<crd::hesap::Complex32>
factor_gp_lu<crd::hesap::Complex32>(const sparse::SparseMatrix<crd::hesap::Complex32, sparse::SparseFormat::Csc>&,
                                    crd::memory::IAllocator*, double);
template SparseLU<crd::hesap::Complex64>
factor_gp_lu<crd::hesap::Complex64>(const sparse::SparseMatrix<crd::hesap::Complex64, sparse::SparseFormat::Csc>&,
                                    crd::memory::IAllocator*, double);

} // namespace crd::hesap::direct
