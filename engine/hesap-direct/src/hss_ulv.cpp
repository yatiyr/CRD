#include <crd/hesap/direct/hss_ulv.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/apply_q_block.hpp>
#include <crd/hesap/dense/qr.hpp>

#include <cmath>
#include <utility>

namespace crd::hesap::direct
{
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::QR;
using crd::hesap::dense::detail::apply_q_block;

namespace
{
// --- small dense kernels (real, RowMajor; sizes are O(rank)/O(leaf)) ----

template <typename T>
Matrix<T> submat(crd::memory::IAllocator* alloc, const Matrix<T>& m, crd::usize r0, crd::usize r1, crd::usize c0,
                 crd::usize c1)
{
    Matrix<T> out(alloc, r1 - r0, c1 - c0);
    for (crd::usize i = 0; i < r1 - r0; ++i)
    {
        for (crd::usize j = 0; j < c1 - c0; ++j)
        {
            out.at(i, j) = m.at(r0 + i, c0 + j);
        }
    }
    return out;
}

template <typename T>
Matrix<T> mm(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)  // A*B
{
    CRD_ASSERT_MSG(a.cols() == b.rows(), "ulv mm dim");
    Matrix<T> out(alloc, a.rows(), b.cols());
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < b.cols(); ++j)
        {
            T s = T{0};
            for (crd::usize k = 0; k < a.cols(); ++k)
            {
                s += a.at(i, k) * b.at(k, j);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}

template <typename T>
Matrix<T> mm_at(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)  // A^T * B
{
    CRD_ASSERT_MSG(a.rows() == b.rows(), "ulv mm_at dim");
    Matrix<T> out(alloc, a.cols(), b.cols());
    for (crd::usize i = 0; i < a.cols(); ++i)
    {
        for (crd::usize j = 0; j < b.cols(); ++j)
        {
            T s = T{0};
            for (crd::usize k = 0; k < a.rows(); ++k)
            {
                s += a.at(k, i) * b.at(k, j);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}

template <typename T>
Matrix<T> mm_bt(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)  // A * B^T
{
    CRD_ASSERT_MSG(a.cols() == b.cols(), "ulv mm_bt dim");
    Matrix<T> out(alloc, a.rows(), b.rows());
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < b.rows(); ++j)
        {
            T s = T{0};
            for (crd::usize k = 0; k < a.cols(); ++k)
            {
                s += a.at(i, k) * b.at(j, k);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}

// Cholesky A = L*L^T (A k*k SPD, L lower). Returns false on a non-positive pivot.
template <typename T>
bool chol_lower(const Matrix<T>& a, Matrix<T>& l)
{
    const crd::usize k = a.rows();
    l.set_zero();
    for (crd::usize j = 0; j < k; ++j)
    {
        T sum = a.at(j, j);
        for (crd::usize q = 0; q < j; ++q)
        {
            sum -= l.at(j, q) * l.at(j, q);
        }
        if (!(sum > T{0}))
        {
            return false;
        }
        const T ljj = std::sqrt(sum);
        l.at(j, j) = ljj;
        for (crd::usize i = j + 1; i < k; ++i)
        {
            T s = a.at(i, j);
            for (crd::usize q = 0; q < j; ++q)
            {
                s -= l.at(i, q) * l.at(j, q);
            }
            l.at(i, j) = s / ljj;
        }
    }
    return true;
}

template <typename T>
void trsv_lower(const Matrix<T>& l, const T* b, T* x) noexcept  // L*x = b
{
    const crd::usize k = l.rows();
    for (crd::usize i = 0; i < k; ++i)
    {
        T s = b[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            s -= l.at(i, j) * x[j];
        }
        x[i] = s / l.at(i, i);
    }
}

// --- batched (m x nrhs, row-major) block kernels for the multi-RHS solve ----

// Cblk(a.rows x nrhs) = A * Bblk(a.cols x nrhs). AXPY form: the inner c-loop is a
// CONTIGUOUS vectorized AXPY (nrhs lanes), A read row-wise (contiguous).
template <typename T>
void gemm_blk(const Matrix<T>& a, const T* bblk, crd::usize nrhs, T* cblk) noexcept
{
    const crd::usize rows = a.rows();
    const crd::usize cols = a.cols();
    const crd::usize ld = a.ld();
    for (crd::usize i = 0; i < rows; ++i)
    {
        T* __restrict ci = cblk + i * nrhs;
        for (crd::usize c = 0; c < nrhs; ++c)
        {
            ci[c] = T{0};
        }
        const T* ai = a.data() + i * ld;  // row i
        for (crd::usize k = 0; k < cols; ++k)
        {
            const T aik = ai[k];
            const T* __restrict bk = bblk + k * nrhs;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                ci[c] += aik * bk[c];
            }
        }
    }
}

// Cblk(a.cols x nrhs) = A^T * Bblk(a.rows x nrhs). Outer-product (rank-1 update)
// form: A read row-wise (a(k,i) contiguous in i), inner c-loop a contiguous AXPY.
template <typename T>
void gemm_at_blk(const Matrix<T>& a, const T* bblk, crd::usize nrhs, T* cblk) noexcept
{
    const crd::usize rows = a.rows();
    const crd::usize cols = a.cols();
    const crd::usize ld = a.ld();
    for (crd::usize i = 0; i < cols * nrhs; ++i)
    {
        cblk[i] = T{0};
    }
    for (crd::usize k = 0; k < rows; ++k)
    {
        const T* ak = a.data() + k * ld;  // row k
        const T* __restrict bk = bblk + k * nrhs;
        for (crd::usize i = 0; i < cols; ++i)
        {
            const T aki = ak[i];  // a(k,i)
            T* __restrict ci = cblk + i * nrhs;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                ci[c] += aki * bk[c];
            }
        }
    }
}

// Allocation-free LEFT application of k Householder reflectors (LAPACK packed
// convention: v_j[j]=1 implicit, qpacked[i*ld+j] = v_j[i] for i>j) to
// C (m × ccols, row-major, leading dim ldc). For the tiny k (= node rank, ~4)
// of the HSS solve this beats the blocked-WY apply_q_block, which allocates FIVE
// scratch Arrays PER CALL -> thousands of calls => allocation-bound, not compute.
// Q = H_0…H_{k-1};  Qᵀ·C applies H_0..H_{k-1} in order, Q·C in reverse.
// wq is caller scratch (>= ccols). Inner cc-loops are contiguous vectorized AXPYs.
template <typename T>
void apply_reflectors_left(const T* qpacked, crd::usize ld, crd::usize m, crd::usize k, const T* taus, T* c,
                           crd::usize ldc, crd::usize ccols, bool transpose, T* wq) noexcept
{
    const auto apply_one = [&](crd::usize j)
    {
        const T tau = taus[j];
        if (tau == T{0})
        {
            return;
        }
        // wq = v_jᵀ · C  (ccols);  v_j[j] = 1, v_j[i>j] = qpacked[i*ld+j].
        T* cj = c + j * ldc;
        for (crd::usize cc = 0; cc < ccols; ++cc)
        {
            wq[cc] = cj[cc];
        }
        for (crd::usize i = j + 1; i < m; ++i)
        {
            const T vij = qpacked[i * ld + j];
            const T* ci = c + i * ldc;
            for (crd::usize cc = 0; cc < ccols; ++cc)
            {
                wq[cc] += vij * ci[cc];
            }
        }
        // C -= tau · v_j · wqᵀ.
        for (crd::usize cc = 0; cc < ccols; ++cc)
        {
            cj[cc] -= tau * wq[cc];
        }
        for (crd::usize i = j + 1; i < m; ++i)
        {
            const T vij = tau * qpacked[i * ld + j];
            T* ci = c + i * ldc;
            for (crd::usize cc = 0; cc < ccols; ++cc)
            {
                ci[cc] -= vij * wq[cc];
            }
        }
    };
    if (transpose)
    {
        for (crd::usize j = 0; j < k; ++j)
        {
            apply_one(j);
        }
    }
    else
    {
        for (crd::usize j = k; j-- > 0;)
        {
            apply_one(j);
        }
    }
}

// Column-block width kept in registers across the dot-product (k) loop of the
// triangular solves. W doubles = W/4 AVX2 ymm accumulators; the running result
// row stays in registers -> 1:1 mem:FMA (vs 3:1 if reloaded/restored each k).
inline constexpr crd::usize kTriSolveBlock = 16;

// In-place multi-RHS triangular solve of a (p x nrhs, row-major) RHS X.
// lower: solve L*X = X; else L^T*X = X. Both are LEFT-LOOKING: the result row's
// nrhs-block is held in a register accumulator across the k-loop (1:1 mem:FMA);
// the inner cc-loop is a fixed-width vectorized AXPY. At HSS leaf sizes the L
// panel is L2-resident, so this matches a blocked dtrsm without any gemm dispatch.
template <typename T>
void tri_solve_vec(const Matrix<T>& l, T* x, crd::usize nrhs) noexcept  // L*X = X
{
    constexpr crd::usize w = kTriSolveBlock;
    const crd::usize p = l.rows();
    const crd::usize ld = l.ld();
    for (crd::usize i = 0; i < p; ++i)
    {
        const T* li = l.data() + i * ld;  // row i (contiguous in k)
        T* xi = x + i * nrhs;
        const T inv = T{1} / li[i];
        crd::usize c0 = 0;
        for (; c0 + w <= nrhs; c0 += w)
        {
            T acc[w];  // register-resident accumulator for this column block
            for (crd::usize cc = 0; cc < w; ++cc)
            {
                acc[cc] = xi[c0 + cc];
            }
            for (crd::usize k = 0; k < i; ++k)
            {
                const T lik = li[k];
                const T* xk = x + k * nrhs + c0;
                for (crd::usize cc = 0; cc < w; ++cc)
                {
                    acc[cc] -= lik * xk[cc];
                }
            }
            for (crd::usize cc = 0; cc < w; ++cc)
            {
                xi[c0 + cc] = acc[cc] * inv;
            }
        }
        for (; c0 < nrhs; ++c0)  // column tail (nrhs % w)
        {
            T s = xi[c0];
            for (crd::usize k = 0; k < i; ++k)
            {
                s -= li[k] * x[k * nrhs + c0];
            }
            xi[c0] = s * inv;
        }
    }
}

template <typename T>
void tri_solve_vec_t(const Matrix<T>& l, T* x, crd::usize nrhs) noexcept  // L^T*X = X
{
    constexpr crd::usize w = kTriSolveBlock;
    const crd::usize p = l.rows();
    const crd::usize ld = l.ld();
    // Left-looking back-substitution: row ii's block accumulates -L[j,ii]*x[j] for
    // j>ii (L column ii, strided but L2-resident), held in registers across j.
    for (crd::usize ii = p; ii-- > 0;)
    {
        const T inv = T{1} / l.data()[ii * ld + ii];  // L[ii,ii]
        T* xi = x + ii * nrhs;
        crd::usize c0 = 0;
        for (; c0 + w <= nrhs; c0 += w)
        {
            T acc[w];
            for (crd::usize cc = 0; cc < w; ++cc)
            {
                acc[cc] = xi[c0 + cc];
            }
            for (crd::usize j = ii + 1; j < p; ++j)
            {
                const T uij = l.data()[j * ld + ii];  // L[j,ii] = (L^T)[ii,j]
                const T* xj = x + j * nrhs + c0;
                for (crd::usize cc = 0; cc < w; ++cc)
                {
                    acc[cc] -= uij * xj[cc];
                }
            }
            for (crd::usize cc = 0; cc < w; ++cc)
            {
                xi[c0 + cc] = acc[cc] * inv;
            }
        }
        for (; c0 < nrhs; ++c0)  // column tail
        {
            T s = xi[c0];
            for (crd::usize j = ii + 1; j < p; ++j)
            {
                s -= l.data()[j * ld + ii] * x[j * nrhs + c0];
            }
            xi[c0] = s * inv;
        }
    }
}

} // namespace

template <typename T>
HssUlv<T> factor_hss_ulv(crd::memory::IAllocator* alloc, const HssMatrix<T>& h)
{
    HssUlv<T> f(alloc);
    const crd::usize num = h.num_nodes();
    f.m_n = h.n;

    for (crd::usize id = 0; id < num; ++id)
    {
        const HssNode<T>& nd = h.nodes[id];
        f.m_is_leaf.push_back(nd.is_leaf ? crd::u8{1} : crd::u8{0});
        f.m_parent.push_back(nd.parent);
        f.m_left.push_back(nd.left);
        f.m_right.push_back(nd.right);
        f.m_i0.push_back(nd.i0);
        f.m_i1.push_back(nd.i1);
        f.m_rank.push_back(nd.rank);
        f.m_fac.push_back(UlvNodeFactor<T>(alloc));
    }

    // Per-node Schur complement (r*r) and QR residual T (r*r) handed to the parent.
    crd::containers::Array<Matrix<T>> schur(alloc);
    crd::containers::Array<Matrix<T>> tk(alloc);
    schur.reserve(num);
    tk.reserve(num);
    for (crd::usize id = 0; id < num; ++id)
    {
        schur.push_back(Matrix<T>(alloc));
        tk.push_back(Matrix<T>(alloc));
    }

    bool ok = true;
    for (crd::usize ri = num; ri-- > 0;)
    {
        const HssNode<T>& nd = h.nodes[ri];
        const crd::usize rk = nd.rank;
        Matrix<T> dnode(alloc);
        Matrix<T> ubasis(alloc);
        crd::usize m = 0;

        if (nd.is_leaf)
        {
            dnode = nd.d.clone();
            ubasis = nd.u.clone();
            m = nd.size();
        }
        else
        {
            const crd::usize c1 = static_cast<crd::usize>(nd.left);
            const crd::usize c2 = static_cast<crd::usize>(nd.right);
            const crd::usize rc1 = h.nodes[c1].rank;
            const crd::usize rc2 = h.nodes[c2].rank;
            m = rc1 + rc2;
            // M = T_c1 * B * T_c2^T
            Matrix<T> mb = mm<T>(alloc, tk[c1], nd.b);  // rc1 * rc2
            Matrix<T> mcouple = mm_bt<T>(alloc, mb, tk[c2]);
            // D_node = [[S_c1, M],[M^T, S_c2]]
            dnode = Matrix<T>(alloc, m, m);
            dnode.set_zero();
            for (crd::usize i = 0; i < rc1; ++i)
            {
                for (crd::usize j = 0; j < rc1; ++j)
                {
                    dnode.at(i, j) = schur[c1].at(i, j);
                }
                for (crd::usize j = 0; j < rc2; ++j)
                {
                    dnode.at(i, rc1 + j) = mcouple.at(i, j);
                    dnode.at(rc1 + j, i) = mcouple.at(i, j);
                }
            }
            for (crd::usize i = 0; i < rc2; ++i)
            {
                for (crd::usize j = 0; j < rc2; ++j)
                {
                    dnode.at(rc1 + i, rc1 + j) = schur[c2].at(i, j);
                }
            }
            // U_basis = [T_c1*R_c1 ; T_c2*R_c2]   (m * rk)
            Matrix<T> ub1 = mm<T>(alloc, tk[c1], h.nodes[c1].r);  // rc1 * rk
            Matrix<T> ub2 = mm<T>(alloc, tk[c2], h.nodes[c2].r);  // rc2 * rk
            ubasis = Matrix<T>(alloc, m, rk);
            for (crd::usize i = 0; i < rc1; ++i)
            {
                for (crd::usize j = 0; j < rk; ++j)
                {
                    ubasis.at(i, j) = ub1.at(i, j);
                }
            }
            for (crd::usize i = 0; i < rc2; ++i)
            {
                for (crd::usize j = 0; j < rk; ++j)
                {
                    ubasis.at(rc1 + i, j) = ub2.at(i, j);
                }
            }
        }

        UlvNodeFactor<T>& fc = f.m_fac[ri];
        const crd::usize p = m - rk;
        fc.m = m;
        fc.r = rk;
        fc.p = p;

        // QR of the m*rk basis, stored as IMPLICIT Householder reflectors
        // (Q^T basis = [T_k; 0]). rk==0 (root / decoupled) => 0 reflectors => Q = I.
        fc.qr = QR<T, Layout::RowMajor>(alloc, m, rk);
        factor_qr<T, Layout::RowMajor>(fc.qr, ubasis);
        {
            Matrix<T> rr(alloc, rk, rk);
            rr.set_zero();
            for (crd::usize i = 0; i < rk; ++i)
            {
                for (crd::usize j = i; j < rk; ++j)
                {
                    rr.at(i, j) = fc.qr.packed().at(i, j);
                }
            }
            tk[ri] = std::move(rr);  // T_k = R, handed to the parent merge
        }
        // Dt = Q^T D Q applied IMPLICITLY via blocked reflectors (dlarfb, O(m^2 * rk));
        // never materialise the dense m*m Q. (rk==0 => Q = I, dt = dnode.)
        Matrix<T> dt = dnode.clone();
        if (rk > 0)
        {
            apply_q_block<T>(fc.qr.packed().data(), fc.qr.packed().ld(), m, rk, fc.qr.taus().data(), dt.data(),
                             dt.ld(), m, m, /*right*/ false, /*transpose*/ true, alloc);  // Q^T D
            apply_q_block<T>(fc.qr.packed().data(), fc.qr.packed().ld(), m, rk, fc.qr.taus().data(), dt.data(),
                             dt.ld(), m, m, /*right*/ true, /*transpose*/ false, alloc);  // (Q^T D) Q
        }
        // skeleton = top rk; fully-summed = bottom p.
        const Matrix<T> d11 = submat<T>(alloc, dt, 0, rk, 0, rk);
        if (p > 0)
        {
            const Matrix<T> d22 = submat<T>(alloc, dt, rk, m, rk, m);  // p * p
            const Matrix<T> d21 = submat<T>(alloc, dt, rk, m, 0, rk);  // p * rk
            fc.l = Matrix<T>(alloc, p, p);
            if (!chol_lower<T>(d22, fc.l))
            {
                ok = false;
            }
            // W = L^-1 * D21 (p * rk); Schur = D11 - W^T * W.
            Matrix<T> w(alloc, p, rk);
            crd::containers::Array<T> bcol(alloc);
            crd::containers::Array<T> xcol(alloc);
            bcol.resize(p);
            xcol.resize(p);
            for (crd::usize j = 0; j < rk; ++j)
            {
                for (crd::usize i = 0; i < p; ++i)
                {
                    bcol[i] = d21.at(i, j);
                }
                trsv_lower<T>(fc.l, bcol.data(), xcol.data());
                for (crd::usize i = 0; i < p; ++i)
                {
                    w.at(i, j) = xcol[i];
                }
            }
            const Matrix<T> wtw = mm_at<T>(alloc, w, w);  // rk * rk
            Matrix<T> s(alloc, rk, rk);
            for (crd::usize i = 0; i < rk; ++i)
            {
                for (crd::usize j = 0; j < rk; ++j)
                {
                    s.at(i, j) = d11.at(i, j) - wtw.at(i, j);
                }
            }
            schur[ri] = std::move(s);
            fc.w = std::move(w);  // p × r = L⁻¹·D21, reused in the 2-solve apply
        }
        else
        {
            fc.l = Matrix<T>(alloc, 0, 0);
            schur[ri] = d11.clone();  // no fully-summed part => Schur = D11
        }
    }

    f.m_info = ok ? 0 : 1;
    crd::u64 nnz = 0;
    for (crd::usize id = 0; id < num; ++id)
    {
        nnz += static_cast<crd::u64>(f.m_fac[id].qr.packed().size() + f.m_fac[id].l.size() +
                                     f.m_fac[id].w.size());
    }
    f.m_nnz = nnz;
    return f;
}

template <typename T>
bool HssUlv<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return false;
    }
    const crd::usize num = m_fac.size();
    if (num == 0 || nrhs == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(rhs.size() == m_n * nrhs, "HssUlv::solve: rhs size mismatch");

    // Per-node offsets into the flat z (size p) and bhat/xskel (size r) buffers.
    crd::containers::Array<crd::usize> poff(m_alloc);
    crd::containers::Array<crd::usize> roff(m_alloc);
    poff.resize(num + 1);
    roff.resize(num + 1);
    crd::usize maxm = 0;
    for (crd::usize id = 0; id < num; ++id)
    {
        poff[id + 1] = poff[id] + m_fac[id].p;
        roff[id + 1] = roff[id] + m_fac[id].r;
        if (m_fac[id].m > maxm)
        {
            maxm = m_fac[id].m;
        }
    }
    const crd::usize totp = poff[num];
    const crd::usize totr = roff[num];

    // Per-node blocks are m x nrhs, ROW-MAJOR (stride nrhs) so the whole RHS
    // block goes through ONE BLAS-3 reflector apply + one gemm per node — a
    // single batched tree traversal, not nrhs traversals.
    crd::containers::Array<T> zb(m_alloc);      // totp x nrhs (per node: p x nrhs)
    crd::containers::Array<T> bhatb(m_alloc);   // totr x nrhs
    crd::containers::Array<T> xskelb(m_alloc);  // totr x nrhs
    zb.resize(totp * nrhs);
    bhatb.resize(totr * nrhs);
    xskelb.resize(totr * nrhs);
    crd::containers::Array<T> btb(m_alloc);    // maxm x nrhs working block
    crd::containers::Array<T> tmpb(m_alloc);   // maxm x nrhs scratch
    crd::containers::Array<T> snode(m_alloc);  // maxm x nrhs scratch
    crd::containers::Array<T> wq(m_alloc);     // nrhs reflector-apply scratch (alloc-free apply)
    btb.resize(maxm * nrhs);
    tmpb.resize(maxm * nrhs);
    snode.resize(maxm * nrhs);
    wq.resize(nrhs);

    const T* rd = rhs.data();
    T* rw = rhs.data();

    // ---- forward: children (higher id) before parents ----
    for (crd::usize ri = num; ri-- > 0;)
    {
        const UlvNodeFactor<T>& fc = m_fac[ri];
        const crd::usize m = fc.m;
        const crd::usize r = fc.r;
        // assemble b_node block (m x nrhs). RHS is COLUMN-major n x nrhs.
        if (m_is_leaf[ri] != 0)
        {
            const crd::usize i0 = m_i0[ri];
            for (crd::usize i = 0; i < m; ++i)
            {
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    btb[i * nrhs + c] = rd[c * m_n + i0 + i];
                }
            }
        }
        else
        {
            const crd::usize c1 = static_cast<crd::usize>(m_left[ri]);
            const crd::usize c2 = static_cast<crd::usize>(m_right[ri]);
            const crd::usize rc1 = m_fac[c1].r;
            const crd::usize rc2 = m_fac[c2].r;
            for (crd::usize i = 0; i < rc1 * nrhs; ++i)
            {
                btb[i] = bhatb[roff[c1] * nrhs + i];
            }
            for (crd::usize i = 0; i < rc2 * nrhs; ++i)
            {
                btb[rc1 * nrhs + i] = bhatb[roff[c2] * nrhs + i];
            }
        }
        // btb := Q^T * btb (Left, transpose) — alloc-free per-reflector apply (rk==0 => I).
        if (r > 0)
        {
            apply_reflectors_left<T>(fc.qr.packed().data(), fc.qr.packed().ld(), m, r, fc.qr.taus().data(),
                                     btb.data(), nrhs, nrhs, /*transpose*/ true, wq.data());
        }
        // u = L^-1 * btb[r:m, :]  (bottom p rows), stored in zb. ONE lower solve
        // (the inner L^-T is folded into the backward sweep via W = L^-1 D21).
        const crd::usize p = fc.p;
        T* uptr = zb.data() + poff[ri] * nrhs;
        for (crd::usize i = 0; i < p * nrhs; ++i)
        {
            uptr[i] = btb[r * nrhs + i];
        }
        tri_solve_vec<T>(fc.l, uptr, nrhs);  // L^-1 · b_bot
        // bhat = btb[0:r, :] - W^T * u   (== btb[0:r] - D12 · D22^-1 · b_bot).
        if (r > 0)
        {
            if (p > 0)
            {
                gemm_at_blk<T>(fc.w, uptr, nrhs, tmpb.data());  // W^T · u (r x nrhs)
                for (crd::usize i = 0; i < r * nrhs; ++i)
                {
                    bhatb[roff[ri] * nrhs + i] = btb[i] - tmpb[i];
                }
            }
            else
            {
                for (crd::usize i = 0; i < r * nrhs; ++i)
                {
                    bhatb[roff[ri] * nrhs + i] = btb[i];
                }
            }
        }
    }

    // ---- backward: parents (lower id) before children ----
    for (crd::usize ri = 0; ri < num; ++ri)
    {
        const UlvNodeFactor<T>& fc = m_fac[ri];
        const crd::usize m = fc.m;
        const crd::usize r = fc.r;
        const crd::usize p = fc.p;
        // btb top r rows = x_top = xskel block.
        for (crd::usize i = 0; i < r * nrhs; ++i)
        {
            btb[i] = xskelb[roff[ri] * nrhs + i];
        }
        if (p > 0)
        {
            // x_bot = L^-T · (u - W · x_top)   (u = L^-1 b_bot stored by the forward).
            const T* uptr = zb.data() + poff[ri] * nrhs;
            if (r > 0)
            {
                gemm_blk<T>(fc.w, btb.data(), nrhs, tmpb.data());  // W · x_top (p x nrhs)
                for (crd::usize i = 0; i < p * nrhs; ++i)
                {
                    snode[i] = uptr[i] - tmpb[i];
                }
            }
            else
            {
                for (crd::usize i = 0; i < p * nrhs; ++i)
                {
                    snode[i] = uptr[i];
                }
            }
            tri_solve_vec_t<T>(fc.l, snode.data(), nrhs);  // L^-T · (u - W x_top)
            for (crd::usize i = 0; i < p * nrhs; ++i)
            {
                btb[r * nrhs + i] = snode[i];
            }
        }
        // btb := Q * btb (Left, no-transpose) — alloc-free per-reflector apply (rk==0 => I).
        if (r > 0)
        {
            apply_reflectors_left<T>(fc.qr.packed().data(), fc.qr.packed().ld(), m, r, fc.qr.taus().data(),
                                     btb.data(), nrhs, nrhs, /*transpose*/ false, wq.data());
        }
        // distribute / write (back to the COLUMN-major RHS for leaves).
        if (m_is_leaf[ri] != 0)
        {
            const crd::usize i0 = m_i0[ri];
            for (crd::usize i = 0; i < m; ++i)
            {
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    rw[c * m_n + i0 + i] = btb[i * nrhs + c];
                }
            }
        }
        else
        {
            const crd::usize c1 = static_cast<crd::usize>(m_left[ri]);
            const crd::usize c2 = static_cast<crd::usize>(m_right[ri]);
            const crd::usize rc1 = m_fac[c1].r;
            const crd::usize rc2 = m_fac[c2].r;
            for (crd::usize i = 0; i < rc1 * nrhs; ++i)
            {
                xskelb[roff[c1] * nrhs + i] = btb[i];
            }
            for (crd::usize i = 0; i < rc2 * nrhs; ++i)
            {
                xskelb[roff[c2] * nrhs + i] = btb[rc1 * nrhs + i];
            }
        }
    }
    return true;
}

// ---- explicit instantiations (v5e-1d: real f32/f64) -------------------
template HssUlv<float> factor_hss_ulv<float>(crd::memory::IAllocator*, const HssMatrix<float>&);
template HssUlv<double> factor_hss_ulv<double>(crd::memory::IAllocator*, const HssMatrix<double>&);
template class HssUlv<float>;
template class HssUlv<double>;

} // namespace crd::hesap::direct
