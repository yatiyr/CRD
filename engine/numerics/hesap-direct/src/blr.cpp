#include <crd/hesap/direct/blr.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/detail/apply_q_block.hpp>
#include <crd/hesap/dense/detail/factor_arms.hpp>
#include <crd/hesap/dense/interp_decomp.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/svd.hpp>
#include <crd/jobs/jobs.hpp>

#include <cmath>
#include <utility>

namespace crd::hesap::direct
{
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::gemm_parallel;
using crd::hesap::dense::interp_decomp;
using crd::hesap::dense::InterpDecomp;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::MatrixView;
using crd::hesap::dense::QR;
using crd::hesap::dense::RealType;
using crd::hesap::dense::svd;
using crd::hesap::dense::SVD;
using crd::hesap::dense::Trans;
using crd::hesap::dense::detail::apply_q_block;

namespace
{
// Gather A[r0:r1, c0:c1] into a fresh (r1-r0)×(c1-c0) matrix.
template <typename T>
Matrix<T> gather(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize r0, crd::usize r1, crd::usize c0,
                 crd::usize c1)
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

// In-place lower Cholesky A = L·Lᵀ (reads/writes the LOWER triangle; the upper is
// left untouched). Returns false on a non-positive pivot.
template <typename T>
bool chol_lower_dense(Matrix<T>& a) noexcept
{
    const crd::usize n = a.rows();
    for (crd::usize j = 0; j < n; ++j)
    {
        T sum = a.at(j, j);
        for (crd::usize q = 0; q < j; ++q)
        {
            sum -= a.at(j, q) * a.at(j, q);
        }
        if (!(sum > T{0}))
        {
            return false;
        }
        const T ljj = std::sqrt(sum);
        a.at(j, j) = ljj;
        for (crd::usize i = j + 1; i < n; ++i)
        {
            T s = a.at(i, j);
            for (crd::usize q = 0; q < j; ++q)
            {
                s -= a.at(i, q) * a.at(j, q);
            }
            a.at(i, j) = s / ljj;
        }
    }
    return true;
}

// Fast in-place lower Cholesky of a diagonal block: the BLOCKED `factor_cholesky`
// (gemm-backed, ~50 GF/s — the crush lever; naive `chol_lower_dense` is O(k³) at
// ~2 GF/s, catastrophic on big fronts). Tiny blocks stay naive (factor_cholesky's
// object + copy overhead is not worth amortizing there). Lower triangle in/out.
template <typename T>
bool chol_lower_fast(crd::memory::IAllocator* alloc, Matrix<T>& a)
{
    const crd::usize k = a.rows();
    if (k < 48)
    {
        return chol_lower_dense<T>(a);
    }
    crd::hesap::dense::Cholesky<T, Layout::RowMajor> chol(alloc, k);
    Matrix<T>& packed = chol.packed();
    for (crd::usize i = 0; i < k; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            packed.at(i, j) = a.at(i, j);
        }
    }
    crd::hesap::dense::factor_cholesky<T, Layout::RowMajor>(chol, alloc);
    if (chol.info() != 0)
    {
        return false;
    }
    for (crd::usize i = 0; i < k; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            a.at(i, j) = packed.at(i, j);
        }
    }
    return true;
}

// Solve L·x = b in place (x holds b), reading only L's lower triangle.
template <typename T>
void trsv_lower(const Matrix<T>& l, T* x) noexcept
{
    const crd::usize k = l.rows();
    for (crd::usize i = 0; i < k; ++i)
    {
        T s = x[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            s -= l.at(i, j) * x[j];
        }
        x[i] = s / l.at(i, i);
    }
}

// Solve Lᵀ·x = b in place (x holds b), reading only L's lower triangle.
template <typename T>
void trsv_lower_t(const Matrix<T>& l, T* x) noexcept
{
    const crd::usize k = l.rows();
    for (crd::usize ii = k; ii-- > 0;)
    {
        T s = x[ii];
        for (crd::usize j = ii + 1; j < k; ++j)
        {
            s -= l.at(j, ii) * x[j];  // (Lᵀ)[ii,j] = L[j,ii]
        }
        x[ii] = s / l.at(ii, ii);
    }
}

// dst[0:rows] -= blk · src[0:cols]. Low-rank: u·(vᵀ·src); dense: A·src.
template <typename T>
void block_gemv_sub(const BlrBlock<T>& blk, const T* src, T* dst) noexcept
{
    if (blk.is_lowrank)
    {
        for (crd::usize q = 0; q < blk.rank; ++q)  // t_q = (vᵀ·src)_q
        {
            T t = T{0};
            for (crd::usize j = 0; j < blk.cols; ++j)
            {
                t += blk.v.at(j, q) * src[j];
            }
            for (crd::usize i = 0; i < blk.rows; ++i)  // dst -= u[:,q]·t
            {
                dst[i] -= blk.u.at(i, q) * t;
            }
        }
    }
    else
    {
        for (crd::usize i = 0; i < blk.rows; ++i)
        {
            T s = T{0};
            for (crd::usize j = 0; j < blk.cols; ++j)
            {
                s += blk.dense.at(i, j) * src[j];
            }
            dst[i] -= s;
        }
    }
}

// dst[0:cols] -= blkᵀ · src[0:rows]. Low-rank: v·(uᵀ·src); dense: Aᵀ·src.
template <typename T>
void block_gemv_t_sub(const BlrBlock<T>& blk, const T* src, T* dst) noexcept
{
    if (blk.is_lowrank)
    {
        for (crd::usize q = 0; q < blk.rank; ++q)  // t_q = (uᵀ·src)_q
        {
            T t = T{0};
            for (crd::usize i = 0; i < blk.rows; ++i)
            {
                t += blk.u.at(i, q) * src[i];
            }
            for (crd::usize j = 0; j < blk.cols; ++j)  // dst -= v[:,q]·t
            {
                dst[j] -= blk.v.at(j, q) * t;
            }
        }
    }
    else
    {
        for (crd::usize j = 0; j < blk.cols; ++j)
        {
            T s = T{0};
            for (crd::usize i = 0; i < blk.rows; ++i)
            {
                s += blk.dense.at(i, j) * src[i];
            }
            dst[j] -= s;
        }
    }
}

// C = alpha op(A) op(B) + beta C on `jobs` from its m n k on, else on the calling thread: either gives gemm's bits
// (fixed reduction order ⇒ moat-safe). FrontJobsPick{} is gemm_parallel_auto's choice. `scratch` packs the operands.
template <typename T>
void front_gemm(const detail::FrontJobsPick& jobs, T alpha, MatrixView<const T, Layout::RowMajor> a,
                MatrixView<const T, Layout::RowMajor> b, T beta, MatrixView<T, Layout::RowMajor> c, Trans ta, Trans tb,
                crd::memory::IAllocator* scratch)
{
    const crd::u64 depth = ta == Trans::None ? a.cols() : a.rows();
    const crd::u64 mnk = static_cast<crd::u64>(c.rows()) * c.cols() * depth;
    crd::u32 nw = 1;
    if (mnk >= jobs.gemm_min_mnk)
    {
        nw = jobs.workers != 0 ? jobs.workers : crd::jobs::num_workers();
    }
    gemm_parallel<T, Layout::RowMajor>(nw, alpha, a, b, beta, c, ta, tb, scratch);
}

// The three products, beta=0 over an explicitly-zeroed C (gemm scales beta·C first, so C must not hold NaN garbage).
template <typename T>
Matrix<T> mm(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
             const detail::FrontJobsPick& jobs)  // A·B
{
    Matrix<T> out(alloc, a.rows(), b.cols());
    out.set_zero();
    front_gemm<T>(jobs, T{1}, a.cview(), b.cview(), T{0}, out.view(), Trans::None, Trans::None, a.allocator());
    return out;
}

template <typename T>
Matrix<T> mm_at(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
                const detail::FrontJobsPick& jobs)  // Aᵀ·B
{
    Matrix<T> out(alloc, a.cols(), b.cols());
    out.set_zero();
    front_gemm<T>(jobs, T{1}, a.cview(), b.cview(), T{0}, out.view(), Trans::Transpose, Trans::None, a.allocator());
    return out;
}

template <typename T>
Matrix<T> mm_bt(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
                const detail::FrontJobsPick& jobs)  // A·Bᵀ
{
    Matrix<T> out(alloc, a.rows(), b.rows());
    out.set_zero();
    front_gemm<T>(jobs, T{1}, a.cview(), b.cview(), T{0}, out.view(), Trans::None, Trans::Transpose, a.allocator());
    return out;
}

// Expand a BLR block to its dense rows×cols matrix (LR: u·vᵀ via the fast gemm).
template <typename T>
Matrix<T> block_dense(crd::memory::IAllocator* alloc, const BlrBlock<T>& blk, const detail::FrontJobsPick& jobs)
{
    if (!blk.is_lowrank)
    {
        return blk.dense.clone();
    }
    return mm_bt<T>(alloc, blk.u, blk.v, jobs);  // u·vᵀ
}

// dst (rows×cols dense) -= ub·vbᵀ  (ub: rows×ru, vb: cols×ru) via the fast gemm.
template <typename T>
void dense_sub_lr(Matrix<T>& dst, const Matrix<T>& ub, const Matrix<T>& vb, const detail::FrontJobsPick& jobs)
{
    front_gemm<T>(jobs, T{-1}, ub.cview(), vb.cview(), T{1}, dst.view(), Trans::None, Trans::Transpose,
                  ub.allocator());
}

// Blocked lower-triangular solve L·X = B in place (X holds B, L npiv×npiv lower
// Explicit-diag, X npiv×nrhs). The trailing panel update goes through the fast gemm
// (cast-as-gemm trsm — hesap-dense's `trsm` is unblocked ~5 GF/s; this is ~gemm rate).
// Small nb diagonal blocks use scalar substitution.
template <typename T>
void blocked_trsm_lower(const Matrix<T>& l, Matrix<T>& x, crd::memory::IAllocator* alloc,
                        const detail::FrontJobsPick& jobs)
{
    const crd::usize n = l.rows();
    const crd::usize nrhs = x.cols();
    constexpr crd::usize nb = 64;
    const crd::usize lld = l.ld();
    const crd::usize xld = x.ld();
    for (crd::usize kb = 0; kb < n; kb += nb)
    {
        const crd::usize kk = (n - kb < nb) ? (n - kb) : nb;
        for (crd::usize i = 0; i < kk; ++i)  // diagonal nb-block: scalar forward subst
        {
            const crd::usize gi = kb + i;
            T* xi = x.data() + gi * xld;
            for (crd::usize j = 0; j < i; ++j)
            {
                const T lij = l.at(gi, kb + j);
                const T* xj = x.data() + (kb + j) * xld;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    xi[c] -= lij * xj[c];
                }
            }
            const T inv = T{1} / l.at(gi, gi);
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                xi[c] *= inv;
            }
        }
        const crd::usize tail = n - (kb + kk);
        if (tail > 0)  // X[kb+kk:,:] -= L[kb+kk:, kb:kb+kk] · X[kb:kb+kk,:]  (GEMM, M=tail large)
        {
            const MatrixView<const T, Layout::RowMajor> lv{l.data() + (kb + kk) * lld + kb, tail, kk, lld};
            const MatrixView<const T, Layout::RowMajor> xv{x.data() + kb * xld, kk, nrhs, xld};
            const MatrixView<T, Layout::RowMajor> cv{x.data() + (kb + kk) * xld, tail, nrhs, xld};
            front_gemm<T>(jobs, T{-1}, lv, xv, T{1}, cv, Trans::None, Trans::None, alloc);
        }
    }
}

// Blocked lower-triangular syrk Schur update: front[off:, off:] (lower) −= l21·l21ᵀ, one gemm per block-COLUMN
// over the lower TRAPEZOID (rows [bj,ms) × cols [bj,bj+rj), k=kk): ~ms/nb big gemms (v5e-3 Leg B B2a — the prior
// nb×nb tiling fired ~ms²/nb² tiny parallel dispatches ⇒ 0.2× scaling) for ~½ the flops of a full ms×ms gemm, each
// on `jobs` from its m n k. The diagonal block-row writes its unused upper half (harmless: the front's upper trailing
// block is never read — cb[f]/L-extract take the lower only). l21 is (ms × kk); off = npiv.
template <typename T>
void syrk_lower_sub(crd::memory::IAllocator* alloc, const Matrix<T>& l21, Matrix<T>& front, crd::usize off,
                    const detail::FrontJobsPick& jobs)
{
    const crd::usize ms = l21.rows();
    const crd::usize kk = l21.cols();
    const crd::usize lld = l21.ld();
    const crd::usize fld = front.ld();
    constexpr crd::usize nb = 256;
    for (crd::usize bj = 0; bj < ms; bj += nb)
    {
        const crd::usize rj = (ms - bj < nb) ? (ms - bj) : nb;     // this block-column's width
        const crd::usize rows = ms - bj;                            // trapezoid height: rows [bj, ms)
        const MatrixView<const T, Layout::RowMajor> ai{l21.data() + bj * lld, rows, kk, lld};  // rows [bj,ms)
        const MatrixView<const T, Layout::RowMajor> aj{l21.data() + bj * lld, rj, kk, lld};    // rows [bj,bj+rj)
        const MatrixView<T, Layout::RowMajor> cv{front.data() + (off + bj) * fld + (off + bj), rows, rj, fld};
        front_gemm<T>(jobs, T{-1}, ai, aj, T{1}, cv, Trans::None, Trans::Transpose, alloc);
    }
}
} // namespace

template <typename T>
BlrMatrix<T> compress_blr_sym(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize block_size,
                              RealType<T> tol)
{
    CRD_ASSERT_MSG(a.rows() == a.cols(), "compress_blr_sym: A must be square");
    const crd::usize n = a.rows();
    BlrMatrix<T> blr(alloc);
    blr.n = n;
    if (n == 0)
    {
        blr.nb = 0;
        blr.bstart.push_back(0);
        return blr;
    }
    if (block_size == 0)
    {
        block_size = 1;
    }

    // Uniform block partition (the last block absorbs the remainder).
    const crd::usize nb = (n + block_size - 1) / block_size;
    blr.nb = nb;
    blr.bstart.resize(nb + 1);
    for (crd::usize i = 0; i < nb; ++i)
    {
        blr.bstart[i] = i * block_size;
    }
    blr.bstart[nb] = n;

    blr.blocks.reserve(nb * nb);
    for (crd::usize i = 0; i < nb * nb; ++i)
    {
        blr.blocks.push_back(BlrBlock<T>(alloc));
    }

    for (crd::usize i = 0; i < nb; ++i)
    {
        const crd::usize r0 = blr.bstart[i];
        const crd::usize r1 = blr.bstart[i + 1];
        for (crd::usize j = 0; j <= i; ++j)  // lower block-triangle only
        {
            const crd::usize c0 = blr.bstart[j];
            const crd::usize c1 = blr.bstart[j + 1];
            BlrBlock<T>& blk = blr.at(i, j);
            blk.rows = r1 - r0;
            blk.cols = c1 - c0;

            Matrix<T> sub = gather<T>(alloc, a, r0, r1, c0, c1);
            if (i == j)
            {
                // Diagonal block: always dense (it is factored directly).
                blk.is_lowrank = false;
                blk.dense = std::move(sub);
                continue;
            }

            // Off-diagonal: try a column ID. Keep low-rank IFF it saves storage.
            const InterpDecomp<T, Layout::RowMajor> id = interp_decomp<T, Layout::RowMajor>(alloc, sub, tol);
            const crd::usize r = id.rank;
            const bool saves = r > 0 && r * (blk.rows + blk.cols) < blk.rows * blk.cols;
            if (saves)
            {
                // block ≈ cols · proj  ⇒  u = cols (rows×r), v = projᵀ (cols×r).
                blk.is_lowrank = true;
                blk.rank = r;
                blk.u = id.cols.clone();
                blk.v = Matrix<T>(alloc, blk.cols, r);
                for (crd::usize p = 0; p < blk.cols; ++p)
                {
                    for (crd::usize q = 0; q < r; ++q)
                    {
                        blk.v.at(p, q) = id.proj.at(q, p);
                    }
                }
            }
            else
            {
                blk.is_lowrank = false;
                blk.dense = std::move(sub);
            }
        }
    }
    return blr;
}

template <typename T>
Matrix<T> blr_to_dense_sym(crd::memory::IAllocator* alloc, const BlrMatrix<T>& b)
{
    Matrix<T> out(alloc, b.n, b.n);
    out.set_zero();
    for (crd::usize i = 0; i < b.nb; ++i)
    {
        const crd::usize r0 = b.bstart[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            const crd::usize c0 = b.bstart[j];
            const BlrBlock<T>& blk = b.at(i, j);
            // Expand the block to dense values, place lower, mirror to upper.
            for (crd::usize p = 0; p < blk.rows; ++p)
            {
                for (crd::usize q = 0; q < blk.cols; ++q)
                {
                    T val;
                    if (blk.is_lowrank)
                    {
                        T s = T{0};
                        for (crd::usize k = 0; k < blk.rank; ++k)
                        {
                            s += blk.u.at(p, k) * blk.v.at(q, k);  // (u·vᵀ)_{pq}
                        }
                        val = s;
                    }
                    else
                    {
                        val = blk.dense.at(p, q);
                    }
                    out.at(r0 + p, c0 + q) = val;
                    if (i != j)
                    {
                        out.at(c0 + q, r0 + p) = val;  // symmetric upper
                    }
                }
            }
        }
    }
    return out;
}

template <typename T>
bool blr_cholesky_factor(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize block_size, RealType<T> tol,
                         BlrMatrix<T>& l)
{
    CRD_ASSERT_MSG(a.rows() == a.cols(), "blr_cholesky_factor: A must be square");
    const crd::usize n = a.rows();
    l.n = n;
    if (block_size == 0)
    {
        block_size = 1;
    }

    // FSCU simple pass: factor densely (L in w's lower triangle), then compress L's
    // off-diagonal blocks into the BLR factor. The LR×LR factor update is v5e-3c.
    Matrix<T> w = a.clone();
    if (n > 0 && !chol_lower_fast<T>(alloc, w))
    {
        return false;  // not numerically SPD
    }

    const crd::usize nb = (n == 0) ? 0 : (n + block_size - 1) / block_size;
    l.nb = nb;
    l.bstart.resize(nb + 1);
    for (crd::usize i = 0; i < nb; ++i)
    {
        l.bstart[i] = i * block_size;
    }
    l.bstart[nb] = n;
    l.blocks.reserve(nb * nb);
    for (crd::usize i = 0; i < nb * nb; ++i)
    {
        l.blocks.push_back(BlrBlock<T>(alloc));
    }

    for (crd::usize i = 0; i < nb; ++i)
    {
        const crd::usize r0 = l.bstart[i];
        const crd::usize r1 = l.bstart[i + 1];
        for (crd::usize j = 0; j <= i; ++j)
        {
            const crd::usize c0 = l.bstart[j];
            const crd::usize c1 = l.bstart[j + 1];
            BlrBlock<T>& blk = l.at(i, j);
            blk.rows = r1 - r0;
            blk.cols = c1 - c0;
            Matrix<T> sub = gather<T>(alloc, w, r0, r1, c0, c1);
            if (i == j)
            {
                // Diagonal L_ii: keep dense, zero the strict upper (w's upper is the
                // untouched original A, not part of the factor).
                for (crd::usize p = 0; p < blk.rows; ++p)
                {
                    for (crd::usize q = p + 1; q < blk.cols; ++q)
                    {
                        sub.at(p, q) = T{0};
                    }
                }
                blk.is_lowrank = false;
                blk.dense = std::move(sub);
                continue;
            }
            const InterpDecomp<T, Layout::RowMajor> id = interp_decomp<T, Layout::RowMajor>(alloc, sub, tol);
            const crd::usize r = id.rank;
            const bool saves = r > 0 && r * (blk.rows + blk.cols) < blk.rows * blk.cols;
            if (saves)
            {
                blk.is_lowrank = true;
                blk.rank = r;
                blk.u = id.cols.clone();
                blk.v = Matrix<T>(alloc, blk.cols, r);
                for (crd::usize p = 0; p < blk.cols; ++p)
                {
                    for (crd::usize q = 0; q < r; ++q)
                    {
                        blk.v.at(p, q) = id.proj.at(q, p);
                    }
                }
            }
            else
            {
                blk.is_lowrank = false;
                blk.dense = std::move(sub);
            }
        }
    }
    return true;
}

template <typename T>
void blr_cholesky_solve(const BlrMatrix<T>& l, T* x) noexcept
{
    const crd::usize nb = l.nb;
    // Forward: L·z = b  (z overwrites x).
    for (crd::usize i = 0; i < nb; ++i)
    {
        T* xi = x + l.bstart[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            block_gemv_sub<T>(l.at(i, j), x + l.bstart[j], xi);  // xi -= L_ij·x_j
        }
        trsv_lower<T>(l.at(i, i).dense, xi);
    }
    // Backward: Lᵀ·x = z.
    for (crd::usize i = nb; i-- > 0;)
    {
        T* xi = x + l.bstart[i];
        for (crd::usize j = i + 1; j < nb; ++j)
        {
            block_gemv_t_sub<T>(l.at(j, i), x + l.bstart[j], xi);  // xi -= L_jiᵀ·x_j
        }
        trsv_lower_t<T>(l.at(i, i).dense, xi);
    }
}

namespace detail
{
template <typename T>
void low_rank_recompress(crd::memory::IAllocator* alloc, const Matrix<T>& uc, const Matrix<T>& vc, RealType<T> tol,
                         crd::usize max_rank, Matrix<T>& u_out, Matrix<T>& v_out, crd::usize& rank_out,
                         const FrontJobsPick& jobs)
{
    const crd::usize m = uc.rows();
    const crd::usize n = vc.rows();
    const crd::usize rc = uc.cols();  // == vc.cols()
    if (rc == 0)
    {
        u_out = Matrix<T>(alloc, m, 0);
        v_out = Matrix<T>(alloc, n, 0);
        rank_out = 0;
        return;
    }
    // Fallback: when rc >= min(m,n) the QR-trick gives no gain — form the dense
    // product and column-ID it.
    if (rc >= m || rc >= n)
    {
        Matrix<T> d = mm_bt<T>(alloc, uc, vc, jobs);  // m×n
        const InterpDecomp<T, Layout::RowMajor> id = interp_decomp<T, Layout::RowMajor>(alloc, d, tol, max_rank);
        rank_out = id.rank;
        u_out = id.cols.clone();
        v_out = Matrix<T>(alloc, n, id.rank);
        for (crd::usize p = 0; p < n; ++p)
        {
            for (crd::usize q = 0; q < id.rank; ++q)
            {
                v_out.at(p, q) = id.proj.at(q, p);
            }
        }
        return;
    }
    // QR(uc)=Qu·Ru, QR(vc)=Qv·Rv (tall); SVD the tiny rc×rc Ru·Rvᵀ.
    QR<T, Layout::RowMajor> qu(alloc, m, rc);
    factor_qr<T, Layout::RowMajor>(qu, uc);
    QR<T, Layout::RowMajor> qv(alloc, n, rc);
    factor_qr<T, Layout::RowMajor>(qv, vc);
    Matrix<T> ru(alloc, rc, rc);
    ru.set_zero();
    Matrix<T> rv(alloc, rc, rc);
    rv.set_zero();
    for (crd::usize i = 0; i < rc; ++i)
    {
        for (crd::usize j = i; j < rc; ++j)
        {
            ru.at(i, j) = qu.packed().at(i, j);
            rv.at(i, j) = qv.packed().at(i, j);
        }
    }
    const Matrix<T> rr = mm_bt<T>(alloc, ru, rv, jobs);  // Ru·Rvᵀ (rc×rc)
    const SVD<T> s = svd<T>(alloc, rr);
    crd::usize r = 0;
    if (s.s.size() > 0 && s.s.data()[0] > RealType<T>{0})
    {
        const RealType<T> thresh = tol * s.s.data()[0];
        for (crd::usize i = 0; i < s.s.size(); ++i)
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
    rank_out = r;
    // u_out = Qu·(U_svd[:, :r]·diag(σ));  v_out = Qv·V_svd[:, :r].
    u_out = Matrix<T>(alloc, m, r);
    u_out.set_zero();
    v_out = Matrix<T>(alloc, n, r);
    v_out.set_zero();
    for (crd::usize i = 0; i < rc; ++i)
    {
        for (crd::usize j = 0; j < r; ++j)
        {
            u_out.at(i, j) = s.u.at(i, j) * s.s.data()[j];
            v_out.at(i, j) = s.v.at(i, j);
        }
    }
    if (r > 0)
    {
        apply_q_block<T>(qu.packed().data(), qu.packed().ld(), m, rc, qu.taus().data(), u_out.data(), u_out.ld(), m,
                         r, /*right*/ false, /*transpose*/ false, alloc);
        apply_q_block<T>(qv.packed().data(), qv.packed().ld(), n, rc, qv.taus().data(), v_out.data(), v_out.ld(), n,
                         r, /*right*/ false, /*transpose*/ false, alloc);
    }
}
} // namespace detail

namespace
{
// Apply the trailing Schur update target -= L_ik·L_jkᵀ. The update is built as a
// low-rank factor ub·vbᵀ (all four dense/LR cases reduce to this), then subtracted:
// diagonal/dense targets densely; LR off-diagonal targets via concatenate +
// recompress (the LUAR — the flop-saving path). `wk` = panel width.
template <typename T>
void apply_schur_update(crd::memory::IAllocator* alloc, BlrBlock<T>& target, const BlrBlock<T>& lik,
                        const BlrBlock<T>& ljk, bool is_diag, RealType<T> tol, const detail::FrontJobsPick& jobs)
{
    const crd::usize rows_i = lik.rows;
    const crd::usize rows_j = ljk.rows;
    // Build ub (rows_i × ru), vb (rows_j × ru) with L_ik·L_jkᵀ = ub·vbᵀ.
    Matrix<T> ub(alloc);
    Matrix<T> vb(alloc);
    if (lik.is_lowrank && ljk.is_lowrank)
    {
        const Matrix<T> mmat = mm_at<T>(alloc, lik.v, ljk.v, jobs);  // v_ikᵀ·v_jk (r_ik×r_jk)
        ub = mm<T>(alloc, lik.u, mmat, jobs);                        // u_ik·M (rows_i×r_jk)
        vb = ljk.u.clone();                                    // rows_j×r_jk
    }
    else if (lik.is_lowrank && !ljk.is_lowrank)
    {
        ub = lik.u.clone();                       // rows_i×r_ik
        vb = mm<T>(alloc, ljk.dense, lik.v, jobs);  // L_jk·v_ik (rows_j×r_ik)
    }
    else if (!lik.is_lowrank && ljk.is_lowrank)
    {
        ub = mm<T>(alloc, lik.dense, ljk.v, jobs);  // L_ik·v_jk (rows_i×r_jk)
        vb = ljk.u.clone();                       // rows_j×r_jk
    }
    else  // both dense
    {
        ub = lik.dense.clone();   // rows_i×wk
        vb = ljk.dense.clone();   // rows_j×wk
    }

    if (is_diag || !target.is_lowrank)
    {
        // Dense target (diagonal always; or an off-diagonal kept dense): subtract densely.
        if (target.is_lowrank)
        {
            target.dense = block_dense<T>(alloc, target, jobs);
            target.is_lowrank = false;
        }
        dense_sub_lr<T>(target.dense, ub, vb, jobs);
        return;
    }
    // LR off-diagonal target: [Ua, −Ub]·[Va, Vb]ᵀ then recompress.
    const crd::usize ra = target.rank;
    const crd::usize ru = ub.cols();
    const crd::usize rc = ra + ru;
    Matrix<T> uc(alloc, rows_i, rc);
    Matrix<T> vc(alloc, rows_j, rc);
    for (crd::usize i = 0; i < rows_i; ++i)
    {
        for (crd::usize k = 0; k < ra; ++k)
        {
            uc.at(i, k) = target.u.at(i, k);
        }
        for (crd::usize k = 0; k < ru; ++k)
        {
            uc.at(i, ra + k) = -ub.at(i, k);  // −Ub
        }
    }
    for (crd::usize j = 0; j < rows_j; ++j)
    {
        for (crd::usize k = 0; k < ra; ++k)
        {
            vc.at(j, k) = target.v.at(j, k);
        }
        for (crd::usize k = 0; k < ru; ++k)
        {
            vc.at(j, ra + k) = vb.at(j, k);
        }
    }
    crd::usize r_new = 0;
    Matrix<T> un(alloc);
    Matrix<T> vn(alloc);
    detail::low_rank_recompress<T>(alloc, uc, vc, tol, 0, un, vn, r_new, jobs);
    target.rank = r_new;
    target.u = std::move(un);
    target.v = std::move(vn);
}
} // namespace

template <typename T>
bool blr_cholesky_factor_lr(crd::memory::IAllocator* alloc, const Matrix<T>& a, crd::usize block_size, RealType<T> tol,
                            BlrMatrix<T>& l)
{
    l = compress_blr_sym<T>(alloc, a, block_size, tol);  // front in BLR form (diag dense, off-diag LR)
    const crd::usize nb = l.nb;
    crd::containers::Array<T> col(alloc);  // strided-column scratch for the panel solve

    for (crd::usize k = 0; k < nb; ++k)
    {
        Matrix<T>& lkk = l.at(k, k).dense;
        if (!chol_lower_fast<T>(alloc, lkk))
        {
            return false;
        }
        // Panel TRSM: for i>k, L_ik = A_ik · L_kk⁻ᵀ (LR-preserving: apply to v).
        for (crd::usize i = k + 1; i < nb; ++i)
        {
            BlrBlock<T>& blk = l.at(i, k);
            if (blk.is_lowrank)
            {
                col.resize(blk.cols);
                for (crd::usize q = 0; q < blk.rank; ++q)
                {
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        col[p] = blk.v.at(p, q);
                    }
                    trsv_lower<T>(lkk, col.data());  // L_kk⁻¹·v[:,q]
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        blk.v.at(p, q) = col[p];
                    }
                }
            }
            else
            {
                col.resize(blk.cols);
                for (crd::usize rr = 0; rr < blk.rows; ++rr)  // solve L_kk·X[r,:]ᵀ = A_ik[r,:]ᵀ
                {
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        col[p] = blk.dense.at(rr, p);
                    }
                    trsv_lower<T>(lkk, col.data());
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        blk.dense.at(rr, p) = col[p];
                    }
                }
            }
        }
        // Trailing update (fixed k-ascending order ⇒ deterministic): A_ij -= L_ik·L_jkᵀ.
        for (crd::usize j = k + 1; j < nb; ++j)
        {
            for (crd::usize i = j; i < nb; ++i)
            {
                apply_schur_update<T>(alloc, l.at(i, j), l.at(i, k), l.at(j, k), i == j, tol, detail::FrontJobsPick{});
            }
        }
    }
    return true;
}

template <typename T>
bool factor_front_cholesky_blr(crd::memory::IAllocator* alloc, Matrix<T>& front, crd::usize npiv,
                               crd::usize block_size, RealType<T> tol, const detail::FrontJobsPick& jobs)
{
    const crd::usize m = front.rows();
    if (npiv == 0 || m == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(npiv <= m, "factor_front_cholesky_blr: npiv > m");
    if (block_size == 0)
    {
        block_size = 1;
    }

    // Block partition with `npiv` forced to a hard boundary (fully-summed | Schur).
    crd::containers::Array<crd::usize> bs(alloc);
    bs.push_back(0);
    for (crd::usize b = block_size; b < npiv; b += block_size)
    {
        bs.push_back(b);
    }
    bs.push_back(npiv);
    for (crd::usize b = npiv + block_size; b < m; b += block_size)
    {
        bs.push_back(b);
    }
    if (bs[bs.size() - 1] != m)
    {
        bs.push_back(m);
    }
    const crd::usize nb = bs.size() - 1;
    crd::usize fs_nb = 0;
    while (bs[fs_nb] < npiv)
    {
        ++fs_nb;  // bs[fs_nb] == npiv
    }

    // Build the BLR front (diagonal dense full, off-diagonal compressed).
    BlrMatrix<T> blr(alloc);
    blr.n = m;
    blr.nb = nb;
    blr.bstart.resize(nb + 1);
    for (crd::usize i = 0; i <= nb; ++i)
    {
        blr.bstart[i] = bs[i];
    }
    blr.blocks.reserve(nb * nb);
    for (crd::usize i = 0; i < nb * nb; ++i)
    {
        blr.blocks.push_back(BlrBlock<T>(alloc));
    }
    for (crd::usize i = 0; i < nb; ++i)
    {
        const crd::usize r0 = bs[i];
        const crd::usize r1 = bs[i + 1];
        for (crd::usize j = 0; j <= i; ++j)
        {
            const crd::usize c0 = bs[j];
            const crd::usize c1 = bs[j + 1];
            BlrBlock<T>& blk = blr.at(i, j);
            blk.rows = r1 - r0;
            blk.cols = c1 - c0;
            Matrix<T> sub = gather<T>(alloc, front, r0, r1, c0, c1);
            if (i == j)
            {
                blk.is_lowrank = false;
                blk.dense = std::move(sub);
                continue;
            }
            const InterpDecomp<T, Layout::RowMajor> id = interp_decomp<T, Layout::RowMajor>(alloc, sub, tol);
            const crd::usize r = id.rank;
            if (r > 0 && r * (blk.rows + blk.cols) < blk.rows * blk.cols)
            {
                blk.is_lowrank = true;
                blk.rank = r;
                blk.u = id.cols.clone();
                blk.v = Matrix<T>(alloc, blk.cols, r);
                for (crd::usize p = 0; p < blk.cols; ++p)
                {
                    for (crd::usize q = 0; q < r; ++q)
                    {
                        blk.v.at(p, q) = id.proj.at(q, p);
                    }
                }
            }
            else
            {
                blk.is_lowrank = false;
                blk.dense = std::move(sub);
            }
        }
    }

    // Partial panel loop: eliminate the fully-summed blocks [0, fs_nb); the trailing
    // [fs_nb, nb) blocks accumulate the Schur complement.
    crd::containers::Array<T> col(alloc);
    for (crd::usize k = 0; k < fs_nb; ++k)
    {
        Matrix<T>& lkk = blr.at(k, k).dense;
        if (!chol_lower_fast<T>(alloc, lkk))
        {
            return false;
        }
        for (crd::usize i = k + 1; i < nb; ++i)  // TRSM panel (incl. the Schur rows = L21)
        {
            BlrBlock<T>& blk = blr.at(i, k);
            if (blk.is_lowrank)
            {
                col.resize(blk.cols);
                for (crd::usize q = 0; q < blk.rank; ++q)
                {
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        col[p] = blk.v.at(p, q);
                    }
                    trsv_lower<T>(lkk, col.data());
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        blk.v.at(p, q) = col[p];
                    }
                }
            }
            else
            {
                col.resize(blk.cols);
                for (crd::usize rr = 0; rr < blk.rows; ++rr)
                {
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        col[p] = blk.dense.at(rr, p);
                    }
                    trsv_lower<T>(lkk, col.data());
                    for (crd::usize p = 0; p < blk.cols; ++p)
                    {
                        blk.dense.at(rr, p) = col[p];
                    }
                }
            }
        }
        for (crd::usize j = k + 1; j < nb; ++j)  // trailing update (incl. the Schur blocks)
        {
            for (crd::usize i = j; i < nb; ++i)
            {
                apply_schur_update<T>(alloc, blr.at(i, j), blr.at(i, k), blr.at(j, k), i == j, tol, jobs);
            }
        }
    }

    // Decompress back to the front's LOWER triangle: L (cols j<fs_nb) + Schur (j≥fs_nb).
    for (crd::usize i = 0; i < nb; ++i)
    {
        const crd::usize r0 = bs[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            const crd::usize c0 = bs[j];
            const Matrix<T> dblk = block_dense<T>(alloc, blr.at(i, j), jobs);
            for (crd::usize p = 0; p < dblk.rows(); ++p)
            {
                for (crd::usize q = 0; q < dblk.cols(); ++q)
                {
                    if (r0 + p >= c0 + q)  // lower triangle only
                    {
                        front.at(r0 + p, c0 + q) = dblk.at(p, q);
                    }
                }
            }
        }
    }
    return true;
}

template <typename T>
bool factor_front_cholesky_dense(crd::memory::IAllocator* alloc, Matrix<T>& front, crd::usize npiv,
                                 const detail::FrontJobsPick& jobs)
{
    using crd::hesap::dense::Cholesky;
    const crd::usize m = front.rows();
    if (npiv == 0 || m == 0)
    {
        return true;
    }
    // (1) dpotrf: A11 (npiv×npiv lower) → L11.
    Cholesky<T, Layout::RowMajor> chol(alloc, npiv);
    Matrix<T>& pk = chol.packed();
    for (crd::usize i = 0; i < npiv; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            pk.at(i, j) = front.at(i, j);
        }
    }
    crd::hesap::dense::detail::factor_cholesky_arm<T, Layout::RowMajor>(
        crd::hesap::dense::detail::CholeskyPick{.workers = jobs.workers}, chol, alloc);
    if (chol.info() != 0)
    {
        return false;
    }
    for (crd::usize i = 0; i < npiv; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            front.at(i, j) = pk.at(i, j);
        }
    }
    const crd::usize ms = m - npiv;
    if (ms == 0)
    {
        return true;
    }
    // (2) blocked dtrsm: L21 = A21·L11⁻ᵀ ⇒ solve L11·B = A21ᵀ (B = npiv×ms), then L21 = Bᵀ.
    Matrix<T> l11(alloc, npiv, npiv);
    for (crd::usize i = 0; i < npiv; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            l11.at(i, j) = pk.at(i, j);  // lower; the blocked trsm reads only the lower
        }
    }
    Matrix<T> b(alloc, npiv, ms);
    for (crd::usize i = 0; i < npiv; ++i)
    {
        for (crd::usize c = 0; c < ms; ++c)
        {
            b.at(i, c) = front.at(npiv + c, i);  // A21ᵀ
        }
    }
    blocked_trsm_lower<T>(l11, b, alloc, jobs);  // B = L11⁻¹·A21ᵀ
    Matrix<T> l21(alloc, ms, npiv);
    for (crd::usize c = 0; c < ms; ++c)
    {
        for (crd::usize i = 0; i < npiv; ++i)
        {
            front.at(npiv + c, i) = b.at(i, c);  // L21
            l21.at(c, i) = b.at(i, c);
        }
    }
    // (3) Schur: A22 −= L21·L21ᵀ (lower) — blocked lower-tri syrk (½ the flops of a full gemm).
    syrk_lower_sub<T>(alloc, l21, front, npiv, jobs);
    return true;
}

// ---- explicit instantiations (v5e-3a/3b: real f32/f64) ----------------
template bool factor_front_cholesky_dense<float>(crd::memory::IAllocator*, Matrix<float>&, crd::usize,
                                                 const detail::FrontJobsPick&);
template bool factor_front_cholesky_dense<double>(crd::memory::IAllocator*, Matrix<double>&, crd::usize,
                                                  const detail::FrontJobsPick&);
template bool factor_front_cholesky_blr<float>(crd::memory::IAllocator*, Matrix<float>&, crd::usize, crd::usize,
                                               RealType<float>, const detail::FrontJobsPick&);
template bool factor_front_cholesky_blr<double>(crd::memory::IAllocator*, Matrix<double>&, crd::usize, crd::usize,
                                                RealType<double>, const detail::FrontJobsPick&);
template bool blr_cholesky_factor<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize, RealType<float>,
                                         BlrMatrix<float>&);
template bool blr_cholesky_factor<double>(crd::memory::IAllocator*, const Matrix<double>&, crd::usize,
                                          RealType<double>, BlrMatrix<double>&);
template void blr_cholesky_solve<float>(const BlrMatrix<float>&, float*);
template void blr_cholesky_solve<double>(const BlrMatrix<double>&, double*);
template bool blr_cholesky_factor_lr<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize,
                                            RealType<float>, BlrMatrix<float>&);
template bool blr_cholesky_factor_lr<double>(crd::memory::IAllocator*, const Matrix<double>&, crd::usize,
                                             RealType<double>, BlrMatrix<double>&);
namespace detail
{
template void low_rank_recompress<float>(crd::memory::IAllocator*, const Matrix<float>&, const Matrix<float>&,
                                         RealType<float>, crd::usize, Matrix<float>&, Matrix<float>&, crd::usize&,
                                         const FrontJobsPick&);
template void low_rank_recompress<double>(crd::memory::IAllocator*, const Matrix<double>&, const Matrix<double>&,
                                          RealType<double>, crd::usize, Matrix<double>&, Matrix<double>&,
                                          crd::usize&, const FrontJobsPick&);
} // namespace detail
template BlrMatrix<float> compress_blr_sym<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize,
                                                  RealType<float>);
template BlrMatrix<double> compress_blr_sym<double>(crd::memory::IAllocator*, const Matrix<double>&, crd::usize,
                                                    RealType<double>);
template Matrix<float> blr_to_dense_sym<float>(crd::memory::IAllocator*, const BlrMatrix<float>&);
template Matrix<double> blr_to_dense_sym<double>(crd::memory::IAllocator*, const BlrMatrix<double>&);

} // namespace crd::hesap::direct
