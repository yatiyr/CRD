#include <crd/hesap/direct/blr.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/detail/apply_q_block.hpp>
#include <crd/hesap/dense/interp_decomp.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/svd.hpp>
#include <crd/jobs/jobs.hpp>  // frame_reset (reclaim the per-call gemm/parallel_for FrameArena — v5e-3 Leg B)

#include <cmath>
#include <utility>

namespace crd::hesap::direct
{
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::gemm;                 // SERIAL gemm for the dense crush path (syrk/trsm): node-parallel
                                               // within-front scaling measured SUB-1× (0.67× — many small/medium
                                               // fronts hit parallel-dispatch overhead) ⇒ serial is the shipped
                                               // path; the level-scheduled hybrid is deferred (see docs/debt.md
                                               // `gemm-parallel-frame-arena-leak` + the v5e-3 Leg B note).
using crd::hesap::dense::gemm_parallel_auto;   // still used by the (deprecated, moat-tested-at-n=4096) BLR helpers
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
// Reclaim the FrameArena consumed by a just-completed parallel gemm / parallel_for. gemm_parallel
// `frame_alloc`s its JobDecls but deliberately never resets (it can't know the caller holds none);
// a partial-front factor fires HUNDREDS of such calls, so the per-thread arena monotonically fills
// and exhausts mid-front (ASan: frame_arena.hpp:60; release: a write past the arena ⇒ glibc
// "corrupted size vs prev_size"). The node-parallel driver dispatches gemms SERIALLY from the main
// thread + each call `wait()`s before returning ⇒ on return all jobs are dead and NO caller holds
// frame state ⇒ resetting here is a valid frame boundary (correct by construction; never runs on a
// tree-parallel worker since those use serial gemm). Guard on workers>0 (serial path never alloc'd).
// v5e-3 Leg B. NOTE: gemm's own non-resetting frame use is a tracked crd-dense defect for a central
// scoped-marker fix; this is the localized driver-side reclamation.
inline void reclaim_frame_arena() noexcept
{
    if (crd::jobs::num_workers() > 0)
    {
        crd::jobs::frame_reset();
    }
}

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

// All three go through the fast gemm_parallel_auto (the kernel that beats Eigen;
// fixed reduction order ⇒ moat-safe + the speed needed to crush). beta=0 over an
// explicitly-zeroed C (gemm scales beta·C first, so C must not hold NaN garbage).
template <typename T>
Matrix<T> mm(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)  // A·B
{
    Matrix<T> out(alloc, a.rows(), b.cols());
    out.set_zero();
    gemm_parallel_auto<T, Layout::RowMajor>(T{1}, a, b, T{0}, out, Trans::None, Trans::None);
    return out;
}

template <typename T>
Matrix<T> mm_at(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)  // Aᵀ·B
{
    Matrix<T> out(alloc, a.cols(), b.cols());
    out.set_zero();
    gemm_parallel_auto<T, Layout::RowMajor>(T{1}, a, b, T{0}, out, Trans::Transpose, Trans::None);
    return out;
}

template <typename T>
Matrix<T> mm_bt(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b)  // A·Bᵀ
{
    Matrix<T> out(alloc, a.rows(), b.rows());
    out.set_zero();
    gemm_parallel_auto<T, Layout::RowMajor>(T{1}, a, b, T{0}, out, Trans::None, Trans::Transpose);
    return out;
}

// Expand a BLR block to its dense rows×cols matrix (LR: u·vᵀ via the fast gemm).
template <typename T>
Matrix<T> block_dense(crd::memory::IAllocator* alloc, const BlrBlock<T>& blk)
{
    if (!blk.is_lowrank)
    {
        return blk.dense.clone();
    }
    return mm_bt<T>(alloc, blk.u, blk.v);  // u·vᵀ
}

// dst (rows×cols dense) -= ub·vbᵀ  (ub: rows×ru, vb: cols×ru) via the fast gemm.
template <typename T>
void dense_sub_lr(Matrix<T>& dst, const Matrix<T>& ub, const Matrix<T>& vb)
{
    gemm_parallel_auto<T, Layout::RowMajor>(T{-1}, ub, vb, T{1}, dst, Trans::None, Trans::Transpose);
}

// Blocked lower-triangular solve L·X = B in place (X holds B, L npiv×npiv lower
// Explicit-diag, X npiv×nrhs). The trailing panel update goes through the fast gemm
// (cast-as-gemm trsm — hesap-dense's `trsm` is unblocked ~5 GF/s; this is ~gemm rate).
// Small nb diagonal blocks use scalar substitution.
template <typename T>
void blocked_trsm_lower(const Matrix<T>& l, Matrix<T>& x, crd::memory::IAllocator* alloc)
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
            gemm<T, Layout::RowMajor>(T{-1}, lv, xv, T{1}, cv, Trans::None, Trans::None, alloc);  // serial
        }
    }
}

// Blocked lower-triangular syrk Schur update: front[off:, off:] (lower) −= l21·l21ᵀ. Only the
// each over the lower TRAPEZOID — one `gemm_parallel_auto` per block-COLUMN (rows [bj,ms) × cols
// [bj,bj+rj), k=kk): ~ms/nb BIG parallel gemms (dispatch overhead amortizes; v5e-3 Leg B B2a — the
// prior nb×nb tiling fired ~ms²/nb² TINY parallel dispatches ⇒ 0.2× scaling) while still ~½ the
// flops of a full ms×ms gemm. Reuses gemm_parallel's per-worker pack pool (one gemm at a time, main-
// thread dispatch ⇒ no concurrent-alloc hazard). The diagonal block-row writes its unused upper half
// (harmless: the front's upper trailing block is never read — cb[f]/L-extract take the lower only).
// l21 is (ms × kk); off = npiv. THE serial+parallel Schur lever (the public `syrk` is naive scalar).
template <typename T>
void syrk_lower_sub(crd::memory::IAllocator* alloc, const Matrix<T>& l21, Matrix<T>& front, crd::usize off)
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
        gemm<T, Layout::RowMajor>(T{-1}, ai, aj, T{1}, cv, Trans::None, Trans::Transpose, alloc);  // serial
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
                         crd::usize max_rank, Matrix<T>& u_out, Matrix<T>& v_out, crd::usize& rank_out)
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
        Matrix<T> d = mm_bt<T>(alloc, uc, vc);  // m×n
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
    const Matrix<T> rr = mm_bt<T>(alloc, ru, rv);  // Ru·Rvᵀ (rc×rc)
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
                        const BlrBlock<T>& ljk, bool is_diag, RealType<T> tol)
{
    const crd::usize rows_i = lik.rows;
    const crd::usize rows_j = ljk.rows;
    // Build ub (rows_i × ru), vb (rows_j × ru) with L_ik·L_jkᵀ = ub·vbᵀ.
    Matrix<T> ub(alloc);
    Matrix<T> vb(alloc);
    if (lik.is_lowrank && ljk.is_lowrank)
    {
        const Matrix<T> mmat = mm_at<T>(alloc, lik.v, ljk.v);  // v_ikᵀ·v_jk (r_ik×r_jk)
        ub = mm<T>(alloc, lik.u, mmat);                        // u_ik·M (rows_i×r_jk)
        vb = ljk.u.clone();                                    // rows_j×r_jk
    }
    else if (lik.is_lowrank && !ljk.is_lowrank)
    {
        ub = lik.u.clone();                       // rows_i×r_ik
        vb = mm<T>(alloc, ljk.dense, lik.v);      // L_jk·v_ik (rows_j×r_ik)
    }
    else if (!lik.is_lowrank && ljk.is_lowrank)
    {
        ub = mm<T>(alloc, lik.dense, ljk.v);      // L_ik·v_jk (rows_i×r_jk)
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
            target.dense = block_dense<T>(alloc, target);
            target.is_lowrank = false;
        }
        dense_sub_lr<T>(target.dense, ub, vb);
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
    detail::low_rank_recompress<T>(alloc, uc, vc, tol, 0, un, vn, r_new);
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
                apply_schur_update<T>(alloc, l.at(i, j), l.at(i, k), l.at(j, k), i == j, tol);
            }
        }
    }
    return true;
}

template <typename T>
bool factor_front_cholesky_blr(crd::memory::IAllocator* alloc, Matrix<T>& front, crd::usize npiv,
                               crd::usize block_size, RealType<T> tol)
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
                apply_schur_update<T>(alloc, blr.at(i, j), blr.at(i, k), blr.at(j, k), i == j, tol);
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
            const Matrix<T> dblk = block_dense<T>(alloc, blr.at(i, j));
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
bool factor_front_cholesky_dense(crd::memory::IAllocator* alloc, Matrix<T>& front, crd::usize npiv)
{
    using crd::hesap::dense::Cholesky;
    using crd::hesap::dense::factor_cholesky;
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
    factor_cholesky<T, Layout::RowMajor>(chol, alloc);
    reclaim_frame_arena();  // factor_cholesky parallel_for's frame use (bound it too)
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
    blocked_trsm_lower<T>(l11, b, alloc);  // B = L11⁻¹·A21ᵀ
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
    syrk_lower_sub<T>(alloc, l21, front, npiv);
    return true;
}

// ---- explicit instantiations (v5e-3a/3b: real f32/f64) ----------------
template bool factor_front_cholesky_dense<float>(crd::memory::IAllocator*, Matrix<float>&, crd::usize);
template bool factor_front_cholesky_dense<double>(crd::memory::IAllocator*, Matrix<double>&, crd::usize);
template bool factor_front_cholesky_blr<float>(crd::memory::IAllocator*, Matrix<float>&, crd::usize, crd::usize,
                                               RealType<float>);
template bool factor_front_cholesky_blr<double>(crd::memory::IAllocator*, Matrix<double>&, crd::usize, crd::usize,
                                                RealType<double>);
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
                                         RealType<float>, crd::usize, Matrix<float>&, Matrix<float>&, crd::usize&);
template void low_rank_recompress<double>(crd::memory::IAllocator*, const Matrix<double>&, const Matrix<double>&,
                                          RealType<double>, crd::usize, Matrix<double>&, Matrix<double>&,
                                          crd::usize&);
} // namespace detail
template BlrMatrix<float> compress_blr_sym<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize,
                                                  RealType<float>);
template BlrMatrix<double> compress_blr_sym<double>(crd::memory::IAllocator*, const Matrix<double>&, crd::usize,
                                                    RealType<double>);
template Matrix<float> blr_to_dense_sym<float>(crd::memory::IAllocator*, const BlrMatrix<float>&);
template Matrix<double> blr_to_dense_sym<double>(crd::memory::IAllocator*, const BlrMatrix<double>&);

} // namespace crd::hesap::direct
