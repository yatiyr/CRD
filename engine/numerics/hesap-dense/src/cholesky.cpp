#include <crd/hesap/dense/cholesky.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/detail/factor_arms.hpp>
#include <crd/hesap/dense/detail/syrk_microkernel.hpp>
#include <crd/jobs/jobs.hpp>

#include <crd/math/cmath.hpp>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// The unblocked left-looking Cholesky of the n by n block at data (row stride ld) on the level's kernel: each column
// takes its dots with the factored prefix. It beats the blocked path for small n (no fork and join, no thin-depth
// GEMM). Returns 0, or k + 1 when column k is not positive definite.
template <typename T> crd::usize unblocked_cholesky(T* data, crd::usize n, crd::usize ld) noexcept
{
    return dense_kernels_of<T>().cholesky(data, n, ld);
}

// L21 = A21 L11^-T for rows [first, end) of the panel at column k (each row a forward substitution of contiguous
// row dots).
template <typename T>
void trsm_rows(T* lu_data, crd::usize ld, crd::usize k, crd::usize nb, crd::usize first, crd::usize end) noexcept
{
    dense_kernels_of<T>().cholesky_trsm(lu_data, ld, k, nb, first, end);
}

// In-place trsm: L21 = A21 · L11^{-T}. Rows are independent: whole rows across nw jobs from rows nb of trsm_min on
// (the default 64K: fork/join overhead dominates at small n; measured: regresses N=256, helps N=1024).
template <typename T>
void inner_trsm_right(T* lu_data, crd::usize n, crd::usize ld, crd::usize k, crd::usize nb, crd::u32 nw,
                      crd::usize trsm_min)
{
    if (k + nb >= n)
    {
        return;
    }
    const crd::usize first = k + nb;
    const crd::usize num_rows = n - first;
    if (nw > 1 && num_rows * nb >= trsm_min)
    {
        struct State
        {
            T* lu_data;
            crd::usize ld;
            crd::usize k;
            crd::usize nb;
            crd::usize first;
        };
        State st{lu_data, ld, k, nb, first};
        State* sp = &st;
        auto* counter = crd::jobs::parallel_for(
            static_cast<crd::u32>(num_rows), nw, [sp](crd::u32 begin, crd::u32 end)
            { trsm_rows<T>(sp->lu_data, sp->ld, sp->k, sp->nb, sp->first + begin, sp->first + end); });
        crd::jobs::wait(counter);
    }
    else
    {
        trsm_rows<T>(lu_data, ld, k, nb, first, n);
    }
}

} // namespace

template <typename T, Layout L>
void factor_cholesky(Cholesky<T, L>& chol, crd::memory::IAllocator* scratch)
{
    detail::factor_cholesky_arm<T, L>(detail::CholeskyPick{}, chol, scratch);
}

template <typename T, Layout L>
void detail::factor_cholesky_arm(const CholeskyPick& pick, Cholesky<T, L>& chol, crd::memory::IAllocator* scratch)
{
    static_assert(L == Layout::RowMajor, "factor_cholesky currently supports RowMajor only");
    CRD_ASSERT_MSG(pick.block != 0, "factor_cholesky_arm: the block is at least one column");

    Matrix<T, L>& packed = chol.packed();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(packed.cols() == n, "factor_cholesky: matrix must be square");
    chol.set_info(0);

    if (n == 0)
    {
        return;
    }

    T* data = packed.data();
    const crd::usize ld = packed.ld();

    crd::memory::IAllocator* alloc = (scratch != nullptr) ? scratch : chol.allocator();

    // Small/mid matrices: pure unblocked SIMD. Blocking overhead (per-panel
    // diagonal factor + syrk packing) isn't amortized until the trailing
    // update dominates (n > 256 on the i9-14900K). Measured: unblocked wins
    // ≤256, blocked-syrk wins ≥512.
    if (n <= pick.unblocked_max)
    {
        const crd::usize info = unblocked_cholesky<T>(data, n, ld);
        chol.set_info(info);
        return;
    }

    // Right-looking blocked Cholesky. The bulk of FLOPs flows through the
    // PACKED register-tiled SYRK trailing update (high arithmetic intensity,
    // no horizontal sums) — the principled fix for the small/mid-N regime
    // where the per-column dot was latency-bound. Each step:
    //   1. factor the nb×nb diagonal block via the SIMD unblocked kernel,
    //   2. trsm the below-diagonal L21 panel (parallel for big trailing),
    //   3. A22 -= L21·L21ᵀ via `syrk_lower_minus` (register-tiled, lower
    //      triangle only = half a gemm's FLOPs; serial at small n, parallel
    //      at large n).
    const crd::u32 nw = pick.workers != 0 ? pick.workers : crd::jobs::num_workers();
    const SyrkPick syrk{.parallel_mmk = pick.syrk_mmk, .workers = nw};
    for (crd::usize k = 0; k < n; k += pick.block)
    {
        const crd::usize nb = (k + pick.block <= n) ? pick.block : (n - k);

        // Step 1: factor diagonal block L11 via SIMD unblocked Cholesky on
        // the nb×nb sub-block at offset (k, k).
        const crd::usize blk_info = unblocked_cholesky<T>(data + k * ld + k, nb, ld);
        if (blk_info != 0)
        {
            chol.set_info(k + blk_info);
            return;
        }

        // Step 2: solve L21 · L11ᵀ = A21 for the below-diagonal block.
        inner_trsm_right<T>(data, n, ld, k, nb, nw, pick.trsm_min);

        // Step 3: syrk trailing update A22 -= L21 · L21ᵀ (lower triangle).
        if (k + nb < n)
        {
            const crd::usize tr_dim = n - k - nb;
            MatrixView<const T, L> l21{data + (k + nb) * ld + k, tr_dim, nb, ld};
            MatrixView<T, L> a22{data + (k + nb) * ld + (k + nb), tr_dim, tr_dim, ld};
            detail::syrk_lower_minus<T, L>(l21, a22, alloc, syrk);
        }
    }
}

template <typename T, Layout L>
void solve_cholesky(const Cholesky<T, L>& chol, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "solve_cholesky currently supports RowMajor only");

    const Matrix<T, L>& packed = chol.packed();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(x.size() == n, "solve_cholesky: RHS size != n");
    CRD_ASSERT_MSG(!chol.is_singular(), "solve_cholesky: factor is singular (info != 0)");

    if (n == 0)
    {
        return;
    }

    const T* data = packed.data();
    const crd::usize ld = packed.ld();

    // Forward sub: L · y = b (explicit diagonal lower triangle).
    for (crd::usize i = 0; i < n; ++i)
    {
        T sum = x[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            sum -= data[i * ld + j] * x[j];
        }
        x[i] = sum / data[i * ld + i];
    }

    // Back sub: L^T · x = y (transposed lower triangle = upper triangle
    // walk with the same diagonal).
    for (crd::usize i = n; i > 0; --i)
    {
        const crd::usize idx = i - 1;
        T sum = x[idx];
        for (crd::usize j = idx + 1; j < n; ++j)
        {
            sum -= data[j * ld + idx] * x[j];
        }
        x[idx] = sum / data[idx * ld + idx];
    }
}

template <typename T, Layout L>
void solve_cholesky(const Cholesky<T, L>& chol, MatrixView<T, L> b)
{
    static_assert(L == Layout::RowMajor, "solve_cholesky currently supports RowMajor only");
    const crd::usize n = chol.packed().rows();
    CRD_ASSERT_MSG(b.rows() == n, "solve_cholesky: RHS rows != n");
    const crd::usize nrhs = b.cols();
    const T* data = chol.packed().data();
    const crd::usize ld = chol.packed().ld();
    for (crd::usize r = 0; r < nrhs; ++r)
    {
        // Forward sub.
        for (crd::usize i = 0; i < n; ++i)
        {
            T sum = b.at(i, r);
            for (crd::usize j = 0; j < i; ++j)
            {
                sum -= data[i * ld + j] * b.at(j, r);
            }
            b.at(i, r) = sum / data[i * ld + i];
        }
        // Back sub.
        for (crd::usize i = n; i > 0; --i)
        {
            const crd::usize idx = i - 1;
            T sum = b.at(idx, r);
            for (crd::usize j = idx + 1; j < n; ++j)
            {
                sum -= data[j * ld + idx] * b.at(j, r);
            }
            b.at(idx, r) = sum / data[idx * ld + idx];
        }
    }
}

// Explicit instantiations — f32 / f64 RowMajor only.
template void factor_cholesky<float, Layout::RowMajor>(Cholesky<float, Layout::RowMajor>&,
                                                       crd::memory::IAllocator*);
template void factor_cholesky<double, Layout::RowMajor>(Cholesky<double, Layout::RowMajor>&,
                                                        crd::memory::IAllocator*);
template void detail::factor_cholesky_arm<float, Layout::RowMajor>(const detail::CholeskyPick&,
                                                                   Cholesky<float, Layout::RowMajor>&,
                                                                   crd::memory::IAllocator*);
template void detail::factor_cholesky_arm<double, Layout::RowMajor>(const detail::CholeskyPick&,
                                                                    Cholesky<double, Layout::RowMajor>&,
                                                                    crd::memory::IAllocator*);
template void solve_cholesky<float, Layout::RowMajor>(const Cholesky<float, Layout::RowMajor>&,
                                                      crd::containers::Span<float>);
template void solve_cholesky<double, Layout::RowMajor>(const Cholesky<double, Layout::RowMajor>&,
                                                       crd::containers::Span<double>);
template void solve_cholesky<float, Layout::RowMajor>(const Cholesky<float, Layout::RowMajor>&,
                                                      MatrixView<float, Layout::RowMajor>);
template void solve_cholesky<double, Layout::RowMajor>(const Cholesky<double, Layout::RowMajor>&,
                                                       MatrixView<double, Layout::RowMajor>);

} // namespace crd::hesap::dense
