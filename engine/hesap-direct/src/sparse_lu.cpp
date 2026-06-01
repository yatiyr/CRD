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

// Iterative DFS on the PARTIAL L's graph (CSparse cs_dfs, adapted to a separate
// `marked` byte array instead of the Lp-sign-flip trick — clearer + leaves the
// growing Lp untouched). `pinv[j]` = the pivot step (= L column) at which original
// row j was eliminated, or <0 if not yet (then j is a source: no outgoing L edges).
// Pushes every visited node onto xi[--top] in topological (post) order; the unit
// diagonal (first entry of each L column, the self-node) is skipped via Lp[col]+1.
// xi[0..head] is the recursion stack, xi[top..n) the output — disjoint (head < top
// always, since path + finished ≤ n). `pstack` (size n) holds the per-level cursor.
[[nodiscard]] crd::u32 lu_dfs(crd::u32 jstart, const crd::u32* lp, const crd::u32* li, const crd::i32* pinv,
                              crd::u32 top, crd::u32* xi, crd::u32* pstack, crd::u8* marked) noexcept
{
    crd::i32 head = 0;
    xi[0] = jstart;
    while (head >= 0)
    {
        const crd::u32 j = xi[static_cast<crd::u32>(head)];
        const crd::i32 jnew = pinv[j]; // L column for node j (<0 ⇒ unpivoted source)
        if (marked[j] == 0)
        {
            marked[j] = 1;
            pstack[head] = (jnew < 0) ? 0U : (lp[static_cast<crd::u32>(jnew)] + 1U); // +1 skips the unit diagonal
        }
        bool done = true;
        const crd::u32 pend = (jnew < 0) ? 0U : lp[static_cast<crd::u32>(jnew) + 1];
        for (crd::u32 p = pstack[head]; p < pend; ++p)
        {
            const crd::u32 i = li[p]; // neighbour (original row)
            if (marked[i] != 0)
            {
                continue;
            }
            pstack[head] = p;     // pause node j here
            xi[++head] = i;       // descend into i
            done = false;
            break;
        }
        if (done)
        {
            --head;
            xi[--top] = j; // node j finished → output (topological order)
        }
    }
    return top;
}

// x = L \ A(:,k) over the reachable pattern. Returns `top`; xi[top..n) is the
// pattern (topological order) and x[xi[top..n)] the values. `marked` is left all-0.
template <typename T>
[[nodiscard]] crd::u32 lu_spsolve(crd::u32 n, const crd::u32* lp, const crd::u32* li, const T* lx, const crd::u32* ap,
                                  const crd::u32* ai, const T* ax, crd::u32 k, crd::u32* xi, crd::u32* pstack, T* x,
                                  const crd::i32* pinv, crd::u8* marked)
{
    crd::u32 top = n;
    for (crd::u32 p = ap[k]; p < ap[k + 1]; ++p)
    {
        const crd::u32 i = ai[p];
        if (marked[i] == 0)
        {
            top = lu_dfs(i, lp, li, pinv, top, xi, pstack, marked);
        }
    }
    for (crd::u32 p = top; p < n; ++p) // clear marks + zero x over the pattern
    {
        marked[xi[p]] = 0;
        x[xi[p]] = T{};
    }
    for (crd::u32 p = ap[k]; p < ap[k + 1]; ++p) // scatter A(:,k) into x
    {
        x[ai[p]] = ax[p];
    }
    for (crd::u32 px = top; px < n; ++px) // forward substitute in topological order
    {
        const crd::u32 j = xi[px];
        const crd::i32 jnew = pinv[j];
        if (jnew < 0)
        {
            continue; // x[j] is final (row j not yet a pivot ⇒ no L column)
        }
        const crd::u32 c = static_cast<crd::u32>(jnew);
        const T xj = x[j];
        for (crd::u32 p = lp[c] + 1; p < lp[c + 1]; ++p) // skip the unit diagonal
        {
            x[li[p]] = x[li[p]] - lx[p] * xj;
        }
    }
    return top;
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
    crd::containers::Array<crd::u32> xi(m_alloc); // [0..n) dfs-stack/output + [n..2n) pstack
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
    crd::u32 lnz = 0;
    crd::u32 unz = 0;
    bool failed = false;
    crd::u32 fail_k = 0;
    for (crd::u32 k = 0; k < n; ++k)
    {
        m_lp[k] = lnz;
        m_up[k] = unz;
        if (lnz + n > cap || unz + n > cap) // ensure a full column (≤ n entries each) fits in L and U
        {
            const crd::usize need = (lnz > unz ? lnz : unz) + n;
            cap = need * 2;
            m_li.resize_uninitialized(cap);
            m_lx.resize_uninitialized(cap);
            m_ui.resize_uninitialized(cap);
            m_ux.resize_uninitialized(cap);
        }
        const crd::u32 top = lu_spsolve<T>(n, m_lp.data(), m_li.data(), m_lx.data(), ap, ai, ax, k, xi.data(),
                                           xi.data() + n, x.data(), pinv.data(), marked.data());
        // Pivot search: max |x[i]| over unpivoted rows; pivoted rows are U entries.
        crd::i32 ipiv = -1;
        dense::RealType<T> a_max = dense::RealType<T>(-1);
        for (crd::u32 px = top; px < n; ++px)
        {
            const crd::u32 i = xi[px];
            if (pinv[i] < 0)
            {
                const dense::RealType<T> t = lu_mag<T>(x[i]);
                if (t > a_max)
                {
                    a_max = t;
                    ipiv = static_cast<crd::i32>(i);
                }
            }
            else // x[i] is an upper-triangular entry U(pinv[i], k)
            {
                m_ui[unz] = static_cast<crd::u32>(pinv[i]);
                m_ux[unz] = x[i];
                ++unz;
            }
        }
        if (ipiv < 0 || !(a_max > dense::RealType<T>(0)))
        {
            failed = true;
            fail_k = k;
            break;
        }
        // Threshold partial pivot: prefer the natural diagonal (row k) when its
        // magnitude ≥ tol·max (tol = 1 ⇒ pure partial pivot). Reduces fill (SuperLU knob).
        if (pinv[k] < 0)
        {
            const dense::RealType<T> dmag = lu_mag<T>(x[k]);
            if (dmag > dense::RealType<T>(0) && dmag >= piv_tol * a_max)
            {
                ipiv = static_cast<crd::i32>(k);
            }
        }
        const T pivot = x[static_cast<crd::u32>(ipiv)];
        m_ui[unz] = k; // U(k,k) = pivot (last entry of U's column k)
        m_ux[unz] = pivot;
        ++unz;
        pinv[static_cast<crd::u32>(ipiv)] = static_cast<crd::i32>(k); // row ipiv → pivot step k
        m_li[lnz] = static_cast<crd::u32>(ipiv); // L(k,k) = 1 (unit diagonal, first entry of L's column k)
        m_lx[lnz] = lu_one<T>();
        ++lnz;
        for (crd::u32 px = top; px < n; ++px)
        {
            const crd::u32 i = xi[px];
            if (pinv[i] < 0) // still unpivoted ⇒ L(i,k) = x[i] / pivot
            {
                m_li[lnz] = i;
                m_lx[lnz] = x[i] / pivot;
                ++lnz;
            }
        }
    }
    m_lp[n] = lnz;
    m_up[n] = unz;
    if (failed)
    {
        m_info = static_cast<crd::usize>(fail_k) + 1;
        return;
    }
    // Remap L's row indices (original rows) → permuted rows (P·A = L·U).
    for (crd::u32 p = 0; p < lnz; ++p)
    {
        m_li[p] = static_cast<crd::u32>(pinv[m_li[p]]);
    }
    m_pinv.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        m_pinv[i] = static_cast<crd::u32>(pinv[i]);
    }
    m_lnz = lnz;
    m_unz = unz;
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
