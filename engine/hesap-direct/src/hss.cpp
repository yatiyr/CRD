#include <crd/hesap/direct/hss.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/apply_q_block.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/randomized_range.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/svd.hpp>

namespace crd::hesap::direct
{
using crd::hesap::dense::counter_gaussian;
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::gemm_parallel_auto;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::MatrixView;
using crd::hesap::dense::QR;
using crd::hesap::dense::RealType;
using crd::hesap::dense::SVD;
using crd::hesap::dense::svd;
using crd::hesap::dense::Trans;
using crd::hesap::dense::detail::apply_q_block;

namespace
{
// Recursive index bisection; node ids assigned PRE-ORDER (parent before
// children) so parent id < child ids. Returns the id of the built subtree root.
template <typename T>
crd::usize build_subtree(HssMatrix<T>& h, crd::usize i0, crd::usize i1, crd::i64 parent, crd::usize leaf_size)
{
    const crd::usize id = h.nodes.size();
    h.nodes.emplace_back(h.alloc);
    {
        HssNode<T>& node = h.nodes[id];
        node.parent = parent;
        node.i0 = i0;
        node.i1 = i1;
    }
    if (i1 - i0 <= leaf_size)
    {
        h.nodes[id].is_leaf = true;
        return id;
    }
    const crd::usize mid = i0 + (i1 - i0) / 2;
    const crd::i64 l = static_cast<crd::i64>(build_subtree<T>(h, i0, mid, static_cast<crd::i64>(id), leaf_size));
    const crd::i64 r = static_cast<crd::i64>(build_subtree<T>(h, mid, i1, static_cast<crd::i64>(id), leaf_size));
    HssNode<T>& node = h.nodes[id];
    node.is_leaf = false;
    node.left = l;
    node.right = r;
    return id;
}

// out (a×c) = A (a×b) · B (b×c).
template <typename T>
Matrix<T> mm(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)
{
    CRD_ASSERT_MSG(a.cols() == b.rows(), "hss mm: inner dim mismatch");
    Matrix<T> out(alloc, a.rows(), b.cols());
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < b.cols(); ++j)
        {
            T s = T{0};
            for (crd::usize p = 0; p < a.cols(); ++p)
            {
                s += a.at(i, p) * b.at(p, j);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}

// out (a×c) = A (a×b) · Bᵀ (B is c×b).
template <typename T>
Matrix<T> mm_bt(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)
{
    CRD_ASSERT_MSG(a.cols() == b.cols(), "hss mm_bt: inner dim mismatch");
    Matrix<T> out(alloc, a.rows(), b.rows());
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < b.rows(); ++j)
        {
            T s = T{0};
            for (crd::usize p = 0; p < a.cols(); ++p)
            {
                s += a.at(i, p) * b.at(j, p);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}

template <typename T>
Matrix<T> identity(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> m(alloc, n, n);
    m.set_identity();
    return m;
}

// gUp(k → sk): the product of translations R from leaf/node k up to (but not
// through) `sk`, an ancestor of k. Result is rank_k × rank_sk.
template <typename T>
Matrix<T> g_up(crd::memory::IAllocator* alloc, const HssMatrix<T>& h, crd::i64 k, crd::i64 sk)
{
    Matrix<T> m = identity<T>(alloc, h.nodes[static_cast<crd::usize>(k)].rank);
    crd::i64 cur = k;
    while (cur != sk)
    {
        const Matrix<T>& rr = h.nodes[static_cast<crd::usize>(cur)].r;  // rank_cur × rank_parent
        m = mm<T>(alloc, m, rr);
        cur = h.nodes[static_cast<crd::usize>(cur)].parent;
    }
    return m;
}

// Lowest common ancestor of two nodes via parent walks.
template <typename T>
crd::i64 lca(const HssMatrix<T>& h, crd::i64 k, crd::i64 l, crd::containers::Array<crd::i64>& scratch)
{
    scratch.clear();
    for (crd::i64 cur = k; cur >= 0; cur = h.nodes[static_cast<crd::usize>(cur)].parent)
    {
        scratch.push_back(cur);
    }
    for (crd::i64 cur = l; cur >= 0; cur = h.nodes[static_cast<crd::usize>(cur)].parent)
    {
        for (crd::usize i = 0; i < scratch.size(); ++i)
        {
            if (scratch[i] == cur)
            {
                return cur;
            }
        }
    }
    return -1;  // unreachable for a connected tree
}

// Child of `a` that is an ancestor of (or equal to a child path of) k.
template <typename T>
crd::i64 child_on_path(const HssMatrix<T>& h, crd::i64 a, crd::i64 k)
{
    crd::i64 cur = k;
    while (h.nodes[static_cast<crd::usize>(cur)].parent != a)
    {
        cur = h.nodes[static_cast<crd::usize>(cur)].parent;
    }
    return cur;
}

// --- from-dense construction helpers -----------------------------------

// out (p×q) = Aᵀ B, where A is n×p and B is n×q.
template <typename T>
Matrix<T> mm_at(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)
{
    CRD_ASSERT_MSG(a.rows() == b.rows(), "hss mm_at: row dim mismatch");
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
Matrix<T> sub_rows(crd::memory::IAllocator* alloc, const Matrix<T>& m, crd::usize r0, crd::usize r1)
{
    Matrix<T> out(alloc, r1 - r0, m.cols());
    for (crd::usize i = r0; i < r1; ++i)
    {
        for (crd::usize j = 0; j < m.cols(); ++j)
        {
            out.at(i - r0, j) = m.at(i, j);
        }
    }
    return out;
}

template <typename T>
Matrix<T> gather_block(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize r0, crd::usize r1,
                       crd::usize c0, crd::usize c1)
{
    Matrix<T> out(alloc, r1 - r0, c1 - c0);
    for (crd::usize i = 0; i < r1 - r0; ++i)
    {
        for (crd::usize j = 0; j < c1 - c0; ++j)
        {
            out.at(i, j) = a.at(r0 + i, c0 + j);
        }
    }
    return out;
}

// The block row A(I_k, I_kᶜ): rows [i0,i1), all columns OUTSIDE [i0,i1).
template <typename T>
Matrix<T> gather_block_row(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize i0, crd::usize i1)
{
    const crd::usize n = a.cols();
    const crd::usize nk = i1 - i0;
    Matrix<T> out(alloc, nk, n - nk);
    for (crd::usize i = 0; i < nk; ++i)
    {
        crd::usize cc = 0;
        for (crd::usize j = 0; j < n; ++j)
        {
            if (j < i0 || j >= i1)
            {
                out.at(i, cc) = a.at(i0 + i, j);
                ++cc;
            }
        }
    }
    return out;
}

// Orthonormal basis for the column space (range) of `m` = leading left singular
// vectors with s_i > tol*s_max (capped at max_rank if > 0). Returns the rank;
// writes the basis (m.rows x rank) into `q_out`.
//
// NOTE: this from-dense path forms the block row + SVDs it (O(N^3) at the
// near-root nodes) -- it is the deterministic REFERENCE construction. The
// performance crush is the matrix-free GLOBAL-sample randomized construction
// (sample A*Omega ONCE, build all HSS levels from the shared samples; STRUMPACK
// HSSMatrix.compress) -- a v5e-2 deliverable. A measured per-NODE randomized
// swap does NOT crush (it re-samples each node independently); only the shared
// global sample reaches STRUMPACK's O(N^2 r).
template <typename T>
crd::usize compress_columns(crd::memory::IAllocator* alloc, const Matrix<T>& m, RealType<T> tol,
                            crd::usize max_rank, Matrix<T>& q_out)
{
    if (m.rows() == 0 || m.cols() == 0)
    {
        q_out = Matrix<T>(alloc, m.rows(), 0);
        return 0;
    }
    const SVD<T> s = svd<T>(alloc, m);  // s.u: rows x min, s.s: min descending
    const crd::usize ns = s.s.size();
    crd::usize r = 0;
    if (ns > 0 && s.s.data()[0] > RealType<T>{0})
    {
        const RealType<T> thresh = tol * s.s.data()[0];
        for (crd::usize i = 0; i < ns; ++i)
        {
            if (s.s.data()[i] > thresh)
            {
                ++r;
            }
            else
            {
                break;
            }
        }
    }
    if (max_rank > 0 && r > max_rank)
    {
        r = max_rank;
    }
    q_out = Matrix<T>(alloc, m.rows(), r);
    for (crd::usize i = 0; i < m.rows(); ++i)
    {
        for (crd::usize j = 0; j < r; ++j)
        {
            q_out.at(i, j) = s.u.at(i, j);
        }
    }
    return r;
}

// Blocked A*B via the dense BLAS-3 gemm (the kernel that beats Eigen).
template <typename T>
Matrix<T> dense_gemm(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)
{
    Matrix<T> c(alloc, a.rows(), b.cols());
    c.set_zero();
    const MatrixView<const T, Layout::RowMajor> av{a.data(), a.rows(), a.cols(), a.ld()};
    const MatrixView<const T, Layout::RowMajor> bv{b.data(), b.rows(), b.cols(), b.ld()};
    const MatrixView<T, Layout::RowMajor> cv{c.data(), c.rows(), c.cols(), c.ld()};
    gemm_parallel_auto<T, Layout::RowMajor>(T{1}, av, bv, T{0}, cv, Trans::None, Trans::None, alloc);
    return c;
}

// GLOBAL-sample compression (v5e-2): orthonormal basis of node k's block-row
// range from the off-diagonal samples S_k = Y(I_k,:) - A(I_k,I_k)*Omega(I_k,:)
// (n_k x ell), via S_k's leading left singular vectors. O(n_k * ell^2). If the
// sketch SATURATES (rank == ell while the block could hold more rank), fall back
// to the exact dense block-row SVD (O(N^3); ell must exceed the true rank for the
// crush -- a fixed ell is correct, adaptive ell-growth is the robust follow-on).
template <typename T>
crd::usize compress_samples(crd::memory::IAllocator* alloc, const Matrix<T>& sk, const Matrix<T>& a, crd::usize i0,
                            crd::usize i1, RealType<T> tol, crd::usize max_rank, crd::usize ell, Matrix<T>& q_out)
{
    // sk is n_k x ell. When TALL (n_k > ell), a full SVD of sk spends O(n_k ell^2)
    // in scalar bdsqr Givens-accumulation into U -> the compress bottleneck. Instead
    // QR sk = Q R (BLAS-3 Householder, fast), SVD the SMALL ell x ell R, then the
    // leading left singular vectors are Q * U_R[:, :r] (one implicit apply_q). The
    // singular values of sk equal those of R, so rank truncation is unchanged, and
    // every step is deterministic -> the counter-RNG basis moat is preserved.
    // When n_k <= ell (small/wide leaves) the direct SVD is already cheap and the
    // QR factorisation would be rank-deficient (only n_k reflectors), so use it raw.
    const crd::usize nk = sk.rows();
    const crd::usize kell = sk.cols();
    const bool tall = nk > kell;

    QR<T, Layout::RowMajor> qr(alloc, tall ? nk : 1, tall ? kell : 1);
    Matrix<T> svin(alloc);  // SVD input: ell x ell R (tall) or sk itself (small)
    if (tall)
    {
        factor_qr<T, Layout::RowMajor>(qr, sk);
        svin = Matrix<T>(alloc, kell, kell);  // ell x ell upper-triangular R
        svin.set_zero();
        for (crd::usize i = 0; i < kell; ++i)
        {
            for (crd::usize j = i; j < kell; ++j)
            {
                svin.at(i, j) = qr.packed().at(i, j);
            }
        }
    }
    else
    {
        svin = sk.clone();
    }
    const SVD<T> s = svd<T>(alloc, svin);  // ell x ell (tall) or n_k x ell (small)
    const crd::usize r = [&]() {
        const crd::usize ns = s.s.size();
        crd::usize rr = 0;
        if (ns > 0 && s.s.data()[0] > RealType<T>{0})
        {
            const RealType<T> thresh = tol * s.s.data()[0];
            for (crd::usize i = 0; i < ns; ++i)
            {
                if (s.s.data()[i] > thresh)
                {
                    ++rr;
                }
                else
                {
                    break;
                }
            }
        }
        return (max_rank > 0 && rr > max_rank) ? max_rank : rr;
    }();
    const crd::usize n = a.rows();
    const crd::usize mindim = nk < (n - nk) ? nk : (n - nk);
    if (r >= ell && ell < mindim)
    {
        // Sketch saturated -> the true rank may exceed ell -> exact dense fallback.
        Matrix<T> br = gather_block_row<T>(alloc, a, i0, i1);
        return compress_columns<T>(alloc, br, tol, max_rank, q_out);
    }
    q_out = Matrix<T>(alloc, nk, r);
    q_out.set_zero();
    const crd::usize usrc_rows = tall ? kell : nk;  // s.u rows: ell (R-SVD) or n_k
    for (crd::usize i = 0; i < usrc_rows; ++i)
    {
        for (crd::usize j = 0; j < r; ++j)
        {
            q_out.at(i, j) = s.u.at(i, j);
        }
    }
    if (tall && r > 0)
    {
        // q_out (top ell rows = U_R[:, :r]) := Q * q_out  (implicit Q, O(n_k ell r)).
        apply_q_block<T>(qr.packed().data(), qr.packed().ld(), nk, kell, qr.taus().data(), q_out.data(),
                         q_out.ld(), nk, r, /*right*/ false, /*transpose*/ false, alloc);
    }
    return r;
}
} // namespace

template <typename T>
void build_cluster_tree(HssMatrix<T>& h, crd::usize n, crd::usize leaf_size)
{
    h.n = n;
    if (n == 0)
    {
        return;
    }
    if (leaf_size == 0)
    {
        leaf_size = 1;
    }
    h.nodes.reserve(2 * n + 4);  // ≥ node count of any bisection tree ⇒ no realloc
    build_subtree<T>(h, 0, n, -1, leaf_size);
}

template <typename T>
void hss_matvec(const HssMatrix<T>& h, crd::containers::ConstSpan<T> x, crd::containers::Span<T> y)
{
    const crd::usize num = h.num_nodes();
    CRD_ASSERT_MSG(x.size() == h.n && y.size() == h.n, "hss_matvec: span size mismatch");
    if (num == 0)
    {
        return;
    }

    // Per-node coefficient-vector offsets (prefix sum of ranks).
    crd::containers::Array<crd::usize> off(h.alloc);
    off.resize(num + 1);
    off[0] = 0;
    for (crd::usize id = 0; id < num; ++id)
    {
        off[id + 1] = off[id] + h.nodes[id].rank;
    }
    const crd::usize total = off[num];

    crd::containers::Array<T> g(h.alloc);
    crd::containers::Array<T> f(h.alloc);
    g.resize(total);  // value-initialised to 0
    f.resize(total);

    // Upward sweep: children (higher id) before parents.
    for (crd::usize ri = num; ri-- > 0;)
    {
        const HssNode<T>& node = h.nodes[ri];
        if (node.is_leaf)
        {
            // g = Uᵀ x(I_k)
            for (crd::usize c = 0; c < node.rank; ++c)
            {
                T s = T{0};
                for (crd::usize i = 0; i < node.size(); ++i)
                {
                    s += node.u.at(i, c) * x[node.i0 + i];
                }
                g[off[ri] + c] = s;
            }
        }
        else
        {
            const HssNode<T>& lc = h.nodes[static_cast<crd::usize>(node.left)];
            const HssNode<T>& rc = h.nodes[static_cast<crd::usize>(node.right)];
            // g = R_leftᵀ g_left + R_rightᵀ g_right
            for (crd::usize c = 0; c < node.rank; ++c)
            {
                T s = T{0};
                for (crd::usize a = 0; a < lc.rank; ++a)
                {
                    s += lc.r.at(a, c) * g[off[static_cast<crd::usize>(node.left)] + a];
                }
                for (crd::usize a = 0; a < rc.rank; ++a)
                {
                    s += rc.r.at(a, c) * g[off[static_cast<crd::usize>(node.right)] + a];
                }
                g[off[ri] + c] = s;
            }
        }
    }

    // Downward sweep: parents (lower id) before children. Each internal node
    // assigns f for its two children (coupling B + its own translated f).
    for (crd::usize id = 0; id < num; ++id)
    {
        const HssNode<T>& node = h.nodes[id];
        if (node.is_leaf)
        {
            continue;
        }
        const crd::usize lci = static_cast<crd::usize>(node.left);
        const crd::usize rci = static_cast<crd::usize>(node.right);
        const HssNode<T>& lc = h.nodes[lci];
        const HssNode<T>& rc = h.nodes[rci];
        const bool non_root = node.parent >= 0;

        // f_left = B g_right (+ R_left f_node)
        for (crd::usize a = 0; a < lc.rank; ++a)
        {
            T s = T{0};
            for (crd::usize bcol = 0; bcol < rc.rank; ++bcol)
            {
                s += node.b.at(a, bcol) * g[off[rci] + bcol];
            }
            if (non_root)
            {
                for (crd::usize c = 0; c < node.rank; ++c)
                {
                    s += lc.r.at(a, c) * f[off[id] + c];
                }
            }
            f[off[lci] + a] = s;
        }
        // f_right = Bᵀ g_left (+ R_right f_node)
        for (crd::usize bcol = 0; bcol < rc.rank; ++bcol)
        {
            T s = T{0};
            for (crd::usize a = 0; a < lc.rank; ++a)
            {
                s += node.b.at(a, bcol) * g[off[lci] + a];
            }
            if (non_root)
            {
                for (crd::usize c = 0; c < node.rank; ++c)
                {
                    s += rc.r.at(bcol, c) * f[off[id] + c];
                }
            }
            f[off[rci] + bcol] = s;
        }
    }

    // Leaves: y(I_k) = D_k x(I_k) + U_k f_k.
    for (crd::usize id = 0; id < num; ++id)
    {
        const HssNode<T>& node = h.nodes[id];
        if (!node.is_leaf)
        {
            continue;
        }
        for (crd::usize i = 0; i < node.size(); ++i)
        {
            T s = T{0};
            for (crd::usize j = 0; j < node.size(); ++j)
            {
                s += node.d.at(i, j) * x[node.i0 + j];
            }
            for (crd::usize c = 0; c < node.rank; ++c)
            {
                s += node.u.at(i, c) * f[off[id] + c];
            }
            y[node.i0 + i] = s;
        }
    }
}

template <typename T>
Matrix<T> hss_to_dense(crd::memory::IAllocator* alloc, const HssMatrix<T>& h)
{
    Matrix<T> out(alloc, h.n, h.n);
    out.set_zero();
    const crd::usize num = h.num_nodes();
    if (num == 0)
    {
        return out;
    }

    // Collect leaf ids.
    crd::containers::Array<crd::i64> leaves(alloc);
    for (crd::usize id = 0; id < num; ++id)
    {
        if (h.nodes[id].is_leaf)
        {
            leaves.push_back(static_cast<crd::i64>(id));
        }
    }

    // Diagonal blocks.
    for (crd::usize li = 0; li < leaves.size(); ++li)
    {
        const HssNode<T>& k = h.nodes[static_cast<crd::usize>(leaves[li])];
        for (crd::usize i = 0; i < k.size(); ++i)
        {
            for (crd::usize j = 0; j < k.size(); ++j)
            {
                out.at(k.i0 + i, k.i0 + j) = k.d.at(i, j);
            }
        }
    }

    // Off-diagonal blocks: (I_k, I_l) = U_k · gUp(k)·B^{orient}·gUp(l)ᵀ · U_lᵀ.
    crd::containers::Array<crd::i64> anc(alloc);
    for (crd::usize ki = 0; ki < leaves.size(); ++ki)
    {
        for (crd::usize lj = 0; lj < leaves.size(); ++lj)
        {
            if (ki == lj)
            {
                continue;
            }
            const crd::i64 k = leaves[ki];
            const crd::i64 l = leaves[lj];
            const crd::i64 a = lca<T>(h, k, l, anc);
            const crd::i64 sk = child_on_path<T>(h, a, k);
            const crd::i64 sl = child_on_path<T>(h, a, l);
            const HssNode<T>& anode = h.nodes[static_cast<crd::usize>(a)];

            const Matrix<T> gupk = g_up<T>(alloc, h, k, sk);  // rank_k × rank_sk
            const Matrix<T> gupl = g_up<T>(alloc, h, l, sl);  // rank_l × rank_sl

            // Orientation: a.b is rank_{a.left} × rank_{a.right}. If sk is the
            // left child, B^{orient} = a.b (rank_sk × rank_sl); else its transpose.
            Matrix<T> core(alloc);  // gupk · B^{orient}  → rank_k × rank_sl
            if (sk == anode.left)
            {
                core = mm<T>(alloc, gupk, anode.b);  // (rank_k×rank_sk)(rank_sk×rank_sl)
            }
            else
            {
                // B^{orient} = a.bᵀ (rank_right × rank_left = rank_sk × rank_sl):
                // gupk · a.bᵀ.
                core = mm_bt<T>(alloc, gupk, anode.b);  // gupk · (a.b)ᵀ
            }
            const Matrix<T> c_kl = mm_bt<T>(alloc, core, gupl);  // core · guplᵀ → rank_k × rank_l

            const HssNode<T>& kn = h.nodes[static_cast<crd::usize>(k)];
            const HssNode<T>& ln = h.nodes[static_cast<crd::usize>(l)];
            const Matrix<T> uc = mm<T>(alloc, kn.u, c_kl);     // U_k · C   → n_k × rank_l
            const Matrix<T> blk = mm_bt<T>(alloc, uc, ln.u);   // (U_k C) · U_lᵀ → n_k × n_l
            for (crd::usize i = 0; i < kn.size(); ++i)
            {
                for (crd::usize j = 0; j < ln.size(); ++j)
                {
                    out.at(kn.i0 + i, ln.i0 + j) = blk.at(i, j);
                }
            }
        }
    }
    return out;
}

template <typename T>
HssMatrix<T> build_hss_from_dense(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize leaf_size,
                                  RealType<T> tol, crd::usize max_rank)
{
    HssMatrix<T> h(alloc);
    const crd::usize n = a.rows();
    CRD_ASSERT_MSG(a.cols() == n, "build_hss_from_dense: A must be square");
#ifndef NDEBUG
    // Symmetric-HSS precondition: U serves as both row and column basis, so a
    // non-symmetric A silently yields garbage. Reject it in debug builds.
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i + 1; j < n; ++j)
        {
            const RealType<T> aij = a.at(i, j);
            const RealType<T> aji = a.at(j, i);
            const RealType<T> diff = aij > aji ? aij - aji : aji - aij;
            const RealType<T> scale = (aij < RealType<T>{0} ? -aij : aij) + (aji < RealType<T>{0} ? -aji : aji);
            CRD_ASSERT_MSG(diff <= RealType<T>{1e-3} * (scale + RealType<T>{1}),
                           "build_hss_from_dense: A must be symmetric");
        }
    }
#endif
    build_cluster_tree<T>(h, n, leaf_size);
    const crd::usize num = h.num_nodes();
    if (num == 0)
    {
        return h;
    }

    // Explicit orthonormal basis Ufull per node (temporary; internal HSS nodes
    // do not store U — it is reconstructed from children via R).
    crd::containers::Array<Matrix<T>> ufull(alloc);
    ufull.reserve(num);
    for (crd::usize id = 0; id < num; ++id)
    {
        ufull.push_back(Matrix<T>(alloc));
    }

    // GLOBAL random sample (v5e-2): Omega (n x ell) via the counter-based RNG
    // (moat-ready: bit-identical across threads), Y = A*Omega computed ONCE
    // (blocked gemm, O(N^2 ell)). Each node's off-diagonal samples then come from
    // Y minus the local diagonal-block action -> O(N^2 ell) total, vs the O(N^3)
    // of forming + SVD-ing every block row.
    crd::usize ell = (max_rank > 0 ? max_rank : crd::usize{40}) + 8;
    if (ell > n)
    {
        ell = n;
    }
    Matrix<T> omega(alloc, n, ell);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < ell; ++j)
        {
            omega.at(i, j) = counter_gaussian<T>(0x5EED5A11ULL, static_cast<crd::u64>(i) * ell + j);
        }
    }
    const Matrix<T> ysamp = dense_gemm<T>(alloc, a, omega);  // n x ell

    // Pass A: per-node basis from the GLOBAL samples + leaf D/U. Node bases are
    // mutually independent (each is a function of A + the shared sample) ⇒ any order.
    for (crd::usize id = 0; id < num; ++id)
    {
        auto& node = h.nodes[id];
        const bool is_root = node.parent < 0;
        if (!is_root)
        {
            // S_k = Y(I_k,:) - A(I_k,I_k) * Omega(I_k,:)  = A(I_k,I_k^c)*Omega(I_k^c,:)
            // (the off-diagonal block-row action, sketched).
            const crd::usize nk = node.size();
            const Matrix<T> adiag = gather_block<T>(alloc, a, node.i0, node.i1, node.i0, node.i1);  // n_k x n_k
            Matrix<T> wk(alloc, nk, ell);  // Omega(I_k,:)
            for (crd::usize rr = 0; rr < nk; ++rr)
            {
                for (crd::usize c = 0; c < ell; ++c)
                {
                    wk.at(rr, c) = omega.at(node.i0 + rr, c);
                }
            }
            const Matrix<T> aw = dense_gemm<T>(alloc, adiag, wk);  // n_k x ell
            Matrix<T> sk(alloc, nk, ell);
            for (crd::usize rr = 0; rr < nk; ++rr)
            {
                for (crd::usize c = 0; c < ell; ++c)
                {
                    sk.at(rr, c) = ysamp.at(node.i0 + rr, c) - aw.at(rr, c);
                }
            }
            Matrix<T> q(alloc);
            node.rank = compress_samples<T>(alloc, sk, a, node.i0, node.i1, tol, max_rank, ell, q);
            ufull[id] = std::move(q);
        }
        else
        {
            node.rank = 0;  // the root has no block row above it
        }
        if (node.is_leaf)
        {
            node.d = gather_block<T>(alloc, a, node.i0, node.i1, node.i0, node.i1);
            node.u = is_root ? Matrix<T>(alloc, node.size(), 0) : ufull[id].clone();
        }
    }

    // Pass B: sibling coupling B + child translations R.
    for (crd::usize id = 0; id < num; ++id)
    {
        auto& node = h.nodes[id];
        if (node.is_leaf)
        {
            continue;
        }
        const crd::usize c1 = static_cast<crd::usize>(node.left);
        const crd::usize c2 = static_cast<crd::usize>(node.right);
        const crd::usize nc1 = h.nodes[c1].size();
        const crd::usize rl = h.nodes[c1].rank;
        const crd::usize rr = h.nodes[c2].rank;

        // B_{c1,c2} = Ufull_c1ᵀ · A(I_c1, I_c2) · Ufull_c2.
        Matrix<T> asub = gather_block<T>(alloc, a, h.nodes[c1].i0, h.nodes[c1].i1, h.nodes[c2].i0, h.nodes[c2].i1);
        Matrix<T> tmp = mm<T>(alloc, asub, ufull[c2]);  // n_c1 × rr
        node.b = mm_at<T>(alloc, ufull[c1], tmp);       // rl × rr

        if (node.parent < 0)
        {
            // Root's children translations are vacuous (rank_root == 0).
            h.nodes[c1].r = Matrix<T>(alloc, rl, 0);
            h.nodes[c2].r = Matrix<T>(alloc, rr, 0);
        }
        else
        {
            // R_ci = Ufull_ciᵀ · (Ufull_parent restricted to c_i's rows).
            Matrix<T> sub1 = sub_rows<T>(alloc, ufull[id], 0, nc1);
            Matrix<T> sub2 = sub_rows<T>(alloc, ufull[id], nc1, node.size());
            h.nodes[c1].r = mm_at<T>(alloc, ufull[c1], sub1);  // rl × rank_p
            h.nodes[c2].r = mm_at<T>(alloc, ufull[c2], sub2);  // rr × rank_p
        }
    }

    return h;
}

// ---- explicit instantiations (v5e-1c: real f32/f64) -------------------
template void build_cluster_tree<float>(HssMatrix<float>&, crd::usize, crd::usize);
template void build_cluster_tree<double>(HssMatrix<double>&, crd::usize, crd::usize);
template void hss_matvec<float>(const HssMatrix<float>&, crd::containers::ConstSpan<float>,
                                crd::containers::Span<float>);
template void hss_matvec<double>(const HssMatrix<double>&, crd::containers::ConstSpan<double>,
                                 crd::containers::Span<double>);
template Matrix<float> hss_to_dense<float>(crd::memory::IAllocator*, const HssMatrix<float>&);
template Matrix<double> hss_to_dense<double>(crd::memory::IAllocator*, const HssMatrix<double>&);
template HssMatrix<float> build_hss_from_dense<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize,
                                                      float, crd::usize);
template HssMatrix<double> build_hss_from_dense<double>(crd::memory::IAllocator*, const Matrix<double>&, crd::usize,
                                                       double, crd::usize);

} // namespace crd::hesap::direct
