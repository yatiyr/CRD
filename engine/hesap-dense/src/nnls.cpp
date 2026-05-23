#include <crd/hesap/dense/nnls.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::dense
{
namespace
{
// Incremental thin-QR of the passive columns A_P = Q·R (Björck §5.8):
//   Q : m × pmax  (thin, RowMajor — column c strided by pmax),
//   R : pmax × pmax upper-triangular (RowMajor),
//   qtb[c] = q_cᵀ·b  (so the passive solve is R·z = qtb).
// add_column  = re-orthogonalised modified Gram-Schmidt step.
// remove_column = Givens re-triangularisation sweep (the downdate).
template <typename T>
struct PassiveQR
{
    crd::usize m;
    crd::usize pmax;
    crd::usize p = 0;
    crd::containers::Array<T> q;       // m × pmax
    crd::containers::Array<T> r;       // pmax × pmax
    crd::containers::Array<T> qtb;     // pmax
    crd::containers::Array<crd::usize> cols;  // passive column indices, length p
    crd::containers::Array<T> scratch; // length m (MGS residual)
    const T* b;

    PassiveQR(crd::memory::IAllocator* alloc, crd::usize m_, crd::usize pmax_, const T* b_)
        : m(m_), pmax(pmax_), q(alloc), r(alloc), qtb(alloc), cols(alloc), scratch(alloc), b(b_)
    {
        q.resize(m * pmax);
        r.resize(pmax * pmax);
        qtb.resize(pmax);
        cols.resize(pmax);
        scratch.resize(m);
    }

    // Add column `acol` (m-vector, = A[:,jcol]). Returns false if numerically
    // dependent on the current passive span (degenerate — caller bails).
    bool add_column(crd::usize jcol, const T* acol)
    {
        T* rv = scratch.data();
        for (crd::usize i = 0; i < m; ++i)
        {
            rv[i] = acol[i];
        }
        crd::containers::Array<T> vcol(q.allocator());
        vcol.resize(p == 0 ? 1 : p);
        for (crd::usize c = 0; c < p; ++c)
        {
            vcol[c] = T{0};
        }
        // Two MGS passes (orig + 1 re-orthogonalisation) for stability.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::usize c = 0; c < p; ++c)
            {
                T proj = T{0};
                for (crd::usize i = 0; i < m; ++i)
                {
                    proj += q[i * pmax + c] * rv[i];
                }
                vcol[c] += proj;
                for (crd::usize i = 0; i < m; ++i)
                {
                    rv[i] -= proj * q[i * pmax + c];
                }
            }
        }
        T rho_sq = T{0};
        for (crd::usize i = 0; i < m; ++i)
        {
            rho_sq += rv[i] * rv[i];
        }
        const T rho = std::sqrt(rho_sq);
        const T tiny = std::numeric_limits<T>::epsilon() * static_cast<T>(16);
        if (rho <= tiny)
        {
            return false;
        }
        for (crd::usize c = 0; c < p; ++c)
        {
            r[c * pmax + p] = vcol[c];
        }
        r[p * pmax + p] = rho;
        const T inv_rho = T{1} / rho;
        T dotqb = T{0};
        for (crd::usize i = 0; i < m; ++i)
        {
            const T qip = rv[i] * inv_rho;
            q[i * pmax + p] = qip;
            dotqb += qip * b[i];
        }
        qtb[p] = dotqb;
        cols[p] = jcol;
        ++p;
        return true;
    }

    // Solve R·z = qtb for the passive solution z (length p).
    void solve(T* z) const
    {
        for (crd::usize ii = p; ii-- > 0;)
        {
            T s = qtb[ii];
            for (crd::usize j = ii + 1; j < p; ++j)
            {
                s -= r[ii * pmax + j] * z[j];
            }
            z[ii] = s / r[ii * pmax + ii];
        }
    }

    // Remove the passive column at position `pos` (Givens downdate).
    void remove_column(crd::usize pos)
    {
        const crd::usize pold = p;
        // Shift columns pos+1..pold-1 left by one in R, Q, qtb, cols.
        for (crd::usize rr = 0; rr < pold; ++rr)
        {
            for (crd::usize cc = pos; cc + 1 < pold; ++cc)
            {
                r[rr * pmax + cc] = r[rr * pmax + (cc + 1)];
            }
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize cc = pos; cc + 1 < pold; ++cc)
            {
                q[i * pmax + cc] = q[i * pmax + (cc + 1)];
            }
        }
        for (crd::usize cc = pos; cc + 1 < pold; ++cc)
        {
            qtb[cc] = qtb[cc + 1];
            cols[cc] = cols[cc + 1];
        }
        const crd::usize pnew = pold - 1;
        // R is now pold rows × pnew cols, upper-Hessenberg from col `pos`
        // (bulge R[cc+1][cc] for cc = pos..pnew-1). Zero each bulge with a
        // Givens rotation on rows (cc, cc+1), applied to R rows, qtb, Q cols.
        for (crd::usize cc = pos; cc < pnew; ++cc)
        {
            const T alpha = r[cc * pmax + cc];
            const T beta = r[(cc + 1) * pmax + cc];
            const T rr = std::sqrt(alpha * alpha + beta * beta);
            if (rr == T{0})
            {
                continue;
            }
            const T cs = alpha / rr;
            const T sn = beta / rr;
            for (crd::usize jj = cc; jj < pnew; ++jj)
            {
                const T t1 = r[cc * pmax + jj];
                const T t2 = r[(cc + 1) * pmax + jj];
                r[cc * pmax + jj] = cs * t1 + sn * t2;
                r[(cc + 1) * pmax + jj] = -sn * t1 + cs * t2;
            }
            {
                const T t1 = qtb[cc];
                const T t2 = qtb[cc + 1];
                qtb[cc] = cs * t1 + sn * t2;
                qtb[cc + 1] = -sn * t1 + cs * t2;
            }
            for (crd::usize i = 0; i < m; ++i)
            {
                const T q1 = q[i * pmax + cc];
                const T q2 = q[i * pmax + (cc + 1)];
                q[i * pmax + cc] = cs * q1 + sn * q2;
                q[i * pmax + (cc + 1)] = -sn * q1 + cs * q2;
            }
        }
        p = pnew;
    }
};
} // namespace

template <typename T>
NNLS<T> nnls(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Vector<T>& b, T tol,
             crd::usize max_iter)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(b.size() == m, "nnls: A rows != b size");

    NNLS<T> out(alloc);
    out.x = Vector<T>(alloc, n);
    out.x.fill(T{0});
    if (n == 0 || m == 0)
    {
        out.converged = true;
        return out;
    }

    const crd::usize pmax = m < n ? m : n;
    const crd::usize iter_cap = (max_iter == 0) ? 3 * (n + 1) : max_iter;

    // Problem-scaled tolerances. scale ≈ max column 2-norm of A.
    T scale = T{0};
    for (crd::usize j = 0; j < n; ++j)
    {
        T s = T{0};
        for (crd::usize i = 0; i < m; ++i)
        {
            const T v = a.at(i, j);
            s += v * v;
        }
        s = std::sqrt(s);
        if (s > scale)
        {
            scale = s;
        }
    }
    if (scale == T{0})
    {
        out.converged = true;
        return out;  // A == 0 → x = 0 is optimal.
    }
    const T eps = std::numeric_limits<T>::epsilon();
    const T tol_w = (tol >= T{0}) ? tol : scale * scale * eps * static_cast<T>(10 * (m > n ? m : n));
    const T tol_x = scale * eps * static_cast<T>(10 * (m > n ? m : n));

    PassiveQR<T> qr(alloc, m, pmax, b.data());
    crd::containers::Array<bool> in_passive(alloc);
    in_passive.resize(n);
    for (crd::usize j = 0; j < n; ++j)
    {
        in_passive[j] = false;
    }
    crd::containers::Array<T> w(alloc);    // gradient Aᵀ(b − Ax)
    crd::containers::Array<T> res(alloc);  // residual b − Ax
    crd::containers::Array<T> z(alloc);    // passive solution
    w.resize(n);
    res.resize(m);
    z.resize(pmax);

    // Column-pointer helper: A[:,j] strided (RowMajor). MGS needs it contiguous,
    // so gather per add.
    crd::containers::Array<T> acol(alloc);
    acol.resize(m);

    auto recompute_w = [&]() {
        for (crd::usize i = 0; i < m; ++i)
        {
            T ax = T{0};
            for (crd::usize j = 0; j < n; ++j)
            {
                ax += a.at(i, j) * out.x(j);
            }
            res[i] = b(i) - ax;
        }
        for (crd::usize j = 0; j < n; ++j)
        {
            T g = T{0};
            for (crd::usize i = 0; i < m; ++i)
            {
                g += a.at(i, j) * res[i];
            }
            w[j] = g;
        }
    };

    recompute_w();  // x = 0 → res = b, w = Aᵀb

    crd::usize iters = 0;
    while (true)
    {
        // Entering variable: largest gradient over the active set, strict `>`
        // with ascending-index tie-break (first max wins).
        crd::isize jstar = -1;
        T wmax = tol_w;
        for (crd::usize j = 0; j < n; ++j)
        {
            if (!in_passive[j] && w[j] > wmax)
            {
                wmax = w[j];
                jstar = static_cast<crd::isize>(j);
            }
        }
        if (jstar < 0)
        {
            out.converged = true;
            break;
        }
        const crd::usize je = static_cast<crd::usize>(jstar);
        for (crd::usize i = 0; i < m; ++i)
        {
            acol[i] = a.at(i, je);
        }
        if (!qr.add_column(je, acol.data()))
        {
            // Numerically dependent column — cannot enter; stop (optimal w.r.t.
            // the achievable passive span).
            out.converged = true;
            break;
        }
        in_passive[je] = true;

        // Inner feasibility loop.
        while (true)
        {
            qr.solve(z.data());
            bool all_pos = true;
            for (crd::usize c = 0; c < qr.p; ++c)
            {
                if (z[c] <= T{0})
                {
                    all_pos = false;
                    break;
                }
            }
            if (all_pos)
            {
                for (crd::usize c = 0; c < qr.p; ++c)
                {
                    out.x(qr.cols[c]) = z[c];
                }
                break;
            }
            // Step length α = min over binding (z ≤ 0) of x/(x − z).
            T alpha = std::numeric_limits<T>::infinity();
            for (crd::usize c = 0; c < qr.p; ++c)
            {
                if (z[c] <= T{0})
                {
                    const T xc = out.x(qr.cols[c]);
                    const T denom = xc - z[c];  // > 0
                    if (denom > T{0})
                    {
                        const T ratio = xc / denom;
                        if (ratio < alpha)
                        {
                            alpha = ratio;
                        }
                    }
                }
            }
            if (!(alpha < std::numeric_limits<T>::infinity()))
            {
                alpha = T{0};
            }
            for (crd::usize c = 0; c < qr.p; ++c)
            {
                const crd::usize jc = qr.cols[c];
                out.x(jc) = out.x(jc) + alpha * (z[c] - out.x(jc));
            }
            // Remove all passive vars that hit zero (re-scan after each removal
            // since positions shift).
            crd::usize cpos = 0;
            while (cpos < qr.p)
            {
                if (out.x(qr.cols[cpos]) <= tol_x)
                {
                    const crd::usize jrem = qr.cols[cpos];
                    out.x(jrem) = T{0};
                    in_passive[jrem] = false;
                    qr.remove_column(cpos);
                }
                else
                {
                    ++cpos;
                }
            }
        }

        recompute_w();
        ++iters;
        if (iters >= iter_cap)
        {
            out.converged = false;
            break;
        }
    }

    out.iterations = iters;
    return out;
}

template NNLS<float> nnls<float>(crd::memory::IAllocator*, const Matrix<float>&, const Vector<float>&,
                                 float, crd::usize);
template NNLS<double> nnls<double>(crd::memory::IAllocator*, const Matrix<double>&,
                                   const Vector<double>&, double, crd::usize);

} // namespace crd::hesap::dense
