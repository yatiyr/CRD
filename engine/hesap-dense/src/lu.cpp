#include <crd/hesap/dense/lu.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/jobs/jobs.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::dense
{
namespace
{
// Block size for the right-looking blocked LU. 64 is the LAPACK default
// for double precision on modern AVX2 hardware; matches our gemm_parallel
// Mc heuristic for trailing updates of typical matrix sizes.
constexpr crd::usize kBlockSize = 64;

// Pivot magnitude: |x| as the real type. Complex modulus for complex T (partial
// pivoting selects the largest-modulus entry); real path bit-identical (if constexpr).
template <typename T>
inline RealType<T> abs_value(T x) noexcept
{
    if constexpr (is_complex_v<T>) { return crd::math::sqrt(x.re * x.re + x.im * x.im); }
    else { return x < T{0} ? -x : x; }
}

// Swap row a and row b across columns [c0, c1).
template <typename T, Layout L>
inline void swap_rows_range(T* data, crd::usize ld, crd::usize a, crd::usize b, crd::usize c0,
                            crd::usize c1) noexcept
{
    if (a == b)
    {
        return;
    }
    if constexpr (L == Layout::RowMajor)
    {
        T* pa = data + a * ld;
        T* pb = data + b * ld;
        for (crd::usize j = c0; j < c1; ++j)
        {
            T tmp = pa[j];
            pa[j] = pb[j];
            pb[j] = tmp;
        }
    }
    else
    {
        for (crd::usize j = c0; j < c1; ++j)
        {
            T* col = data + j * ld;
            T tmp = col[a];
            col[a] = col[b];
            col[b] = tmp;
        }
    }
}

// Unblocked LU factorization on a tall panel A[0:n, 0:nb] (rows are global
// indices into the LU matrix; columns are restricted to the panel). On
// each step j in [0, nb): find pivot, swap full rows (cols [0, n)), scale
// L below diagonal, rank-1 update the rest of the panel.
//
// Returns 0 on success, k+1 on first zero pivot (k = absolute column index).
template <typename T, Layout L>
crd::usize panel_factor(T* lu_data, crd::usize n, crd::usize ld, crd::usize k, crd::usize nb,
                        crd::usize* pivots)
{
    for (crd::usize j = k; j < k + nb; ++j)
    {
        // Find pivot row (max |A[i, j]| for i in [j, n)).
        RealType<T> max_abs = abs_value<T>(lu_data[j * ld + j]);
        crd::usize pivot_row = j;
        for (crd::usize i = j + 1; i < n; ++i)
        {
            const RealType<T> v = abs_value<T>(lu_data[i * ld + j]);
            if (v > max_abs)
            {
                max_abs = v;
                pivot_row = i;
            }
        }

        pivots[j] = pivot_row;

        // Detect exact singularity.
        if (lu_data[pivot_row * ld + j] == T{0})
        {
            return j + 1;
        }

        // Swap entire rows j and pivot_row across columns [0, n).
        if (pivot_row != j)
        {
            swap_rows_range<T, L>(lu_data, ld, j, pivot_row, 0, n);
        }

        // Scale L21 := L21 / U[j,j].
        const T inv_ujj = T{1} / lu_data[j * ld + j];
        for (crd::usize i = j + 1; i < n; ++i)
        {
            lu_data[i * ld + j] *= inv_ujj;
        }

        // Rank-1 update on the rest of the panel only.
        // For each row i in [j+1, n), for each col jj in [j+1, k+nb):
        //   A[i, jj] -= A[i, j] * A[j, jj]
        for (crd::usize i = j + 1; i < n; ++i)
        {
            const T lij = lu_data[i * ld + j];
            if (lij == T{0})
            {
                continue;
            }
            for (crd::usize jj = j + 1; jj < k + nb; ++jj)
            {
                lu_data[i * ld + jj] -= lij * lu_data[j * ld + jj];
            }
        }
    }
    return 0;
}

// Inner trsm: solve L11 * U12 = A12, in-place at A[k:k+nb, k+nb:n].
// L11 is the unit-diagonal lower triangular block at A[k:k+nb, k:k+nb].
template <typename T, Layout L>
void inner_trsm_lower_unit(T* lu_data, crd::usize ld, crd::usize k, crd::usize nb, crd::usize n)
{
    if (k + nb >= n)
    {
        return;
    }
    // Walk each column of U12 independently.
    for (crd::usize j = k + nb; j < n; ++j)
    {
        // Forward substitution within the L11 block.
        for (crd::usize i = k; i < k + nb; ++i)
        {
            T sum = lu_data[i * ld + j];
            for (crd::usize p = k; p < i; ++p)
            {
                sum -= lu_data[i * ld + p] * lu_data[p * ld + j];
            }
            // L11 diagonal is implicit unit — no divide.
            lu_data[i * ld + j] = sum;
        }
    }
}

} // namespace

template <typename T, Layout L>
void factor_lu(LU<T, L>& lu, crd::memory::IAllocator* scratch)
{
    static_assert(L == Layout::RowMajor, "factor_lu currently supports RowMajor only");

    Matrix<T, L>& packed = lu.packed();
    Permutation& perm = lu.permutation();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(packed.cols() == n, "factor_lu: matrix must be square");
    CRD_ASSERT_MSG(perm.n() == n, "factor_lu: permutation size mismatch");

    lu.set_info(0);
    perm.set_identity();

    if (n == 0)
    {
        return;
    }

    T* data = packed.data();
    const crd::usize ld = packed.ld();
    crd::usize* pivots = perm.pivots();

    crd::memory::IAllocator* alloc = (scratch != nullptr) ? scratch : lu.allocator();

    // Auto-pick worker count for trailing-update gemm.
    const crd::u32 num_workers = crd::jobs::num_workers();

    for (crd::usize k = 0; k < n; k += kBlockSize)
    {
        const crd::usize nb = (k + kBlockSize <= n) ? kBlockSize : (n - k);

        // Step 1: panel factor (unblocked LU + row pivoting; full-row swaps).
        const crd::usize info = panel_factor<T, L>(data, n, ld, k, nb, pivots);
        if (info != 0)
        {
            lu.set_info(info);
            return;
        }

        // Step 2: inner trsm L11 \ A12 -> U12 (in-place).
        inner_trsm_lower_unit<T, L>(data, ld, k, nb, n);

        // Step 3: trailing update A22 -= L21 * U12 via gemm_parallel.
        if (k + nb < n)
        {
            const crd::usize tr_rows = n - k - nb;
            const crd::usize tr_cols = n - k - nb;
            T* a21_ptr = data + (k + nb) * ld + k;
            T* u12_ptr = data + k * ld + (k + nb);
            T* a22_ptr = data + (k + nb) * ld + (k + nb);
            MatrixView<const T, L> a21{a21_ptr, tr_rows, nb, ld};
            MatrixView<const T, L> u12{u12_ptr, nb, tr_cols, ld};
            MatrixView<T, L> a22{a22_ptr, tr_rows, tr_cols, ld};
            gemm_parallel<T, L>(num_workers, T{-1}, a21, u12, T{1}, a22, Trans::None, Trans::None,
                                alloc);
        }
    }
}

template <typename T, Layout L>
void solve_lu(const LU<T, L>& lu, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "solve_lu currently supports RowMajor only");

    const Matrix<T, L>& packed = lu.packed();
    const Permutation& perm = lu.permutation();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(x.size() == n, "solve_lu: RHS size != n");
    CRD_ASSERT_MSG(!lu.is_singular(), "solve_lu: factor is singular (info != 0)");

    if (n == 0)
    {
        return;
    }

    // Apply permutation: x_perm = P * x (replay pivots forward).
    apply_permutation(perm, x);

    const T* data = packed.data();
    const crd::usize ld = packed.ld();

    // Forward substitution: L * y = x_perm (unit-diagonal lower triangle).
    for (crd::usize i = 0; i < n; ++i)
    {
        T sum = x[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            sum -= data[i * ld + j] * x[j];
        }
        x[i] = sum;
    }

    // Back substitution: U * z = y (explicit-diagonal upper triangle).
    for (crd::usize i = n; i > 0; --i)
    {
        const crd::usize idx = i - 1;
        T sum = x[idx];
        for (crd::usize j = idx + 1; j < n; ++j)
        {
            sum -= data[idx * ld + j] * x[j];
        }
        x[idx] = sum / data[idx * ld + idx];
    }
}

template <typename T, Layout L>
void solve_lu(const LU<T, L>& lu, MatrixView<T, L> b)
{
    static_assert(L == Layout::RowMajor, "solve_lu currently supports RowMajor only");
    const crd::usize n = lu.packed().rows();
    CRD_ASSERT_MSG(b.rows() == n, "solve_lu: RHS rows != n");
    // Solve each column of B independently.
    const crd::usize nrhs = b.cols();
    for (crd::usize r = 0; r < nrhs; ++r)
    {
        // Gather column r into a contiguous scratch span? For RowMajor B, a
        // column has stride ld(); we can solve in-place with strided access,
        // but the simplest correct first pass is a gather/scatter via a
        // local scratch buffer.
        //
        // For v0e-a we keep this simple: walk one column with manual stride.
        // The per-column substitution is O(n^2); scattered access is OK.

        // Forward apply permutation on column r.
        for (crd::usize k = 0; k < lu.permutation().n(); ++k)
        {
            const crd::usize rk = lu.permutation().pivot_at(k);
            if (rk != k)
            {
                T tmp = b.at(k, r);
                b.at(k, r) = b.at(rk, r);
                b.at(rk, r) = tmp;
            }
        }

        const T* data = lu.packed().data();
        const crd::usize ld = lu.packed().ld();

        // Forward sub L.
        for (crd::usize i = 0; i < n; ++i)
        {
            T sum = b.at(i, r);
            for (crd::usize j = 0; j < i; ++j)
            {
                sum -= data[i * ld + j] * b.at(j, r);
            }
            b.at(i, r) = sum;
        }
        // Back sub U.
        for (crd::usize i = n; i > 0; --i)
        {
            const crd::usize idx = i - 1;
            T sum = b.at(idx, r);
            for (crd::usize j = idx + 1; j < n; ++j)
            {
                sum -= data[idx * ld + j] * b.at(j, r);
            }
            b.at(idx, r) = sum / data[idx * ld + idx];
        }
    }
}

// Explicit instantiations: real f32 / f64 + complex c32 / c64, RowMajor.
// Complex added v4k-c: first consumer = complex SA-AMG coarse solve (the LU body is
// complex-generic; only pivot magnitude uses the real modulus, see abs_value).
using C32 = crd::hesap::Complex<float>;
using C64 = crd::hesap::Complex<double>;
template void factor_lu<float, Layout::RowMajor>(LU<float, Layout::RowMajor>&,
                                                  crd::memory::IAllocator*);
template void factor_lu<double, Layout::RowMajor>(LU<double, Layout::RowMajor>&,
                                                   crd::memory::IAllocator*);
template void factor_lu<C32, Layout::RowMajor>(LU<C32, Layout::RowMajor>&, crd::memory::IAllocator*);
template void factor_lu<C64, Layout::RowMajor>(LU<C64, Layout::RowMajor>&, crd::memory::IAllocator*);
template void solve_lu<float, Layout::RowMajor>(const LU<float, Layout::RowMajor>&,
                                                 crd::containers::Span<float>);
template void solve_lu<double, Layout::RowMajor>(const LU<double, Layout::RowMajor>&,
                                                  crd::containers::Span<double>);
template void solve_lu<C32, Layout::RowMajor>(const LU<C32, Layout::RowMajor>&, crd::containers::Span<C32>);
template void solve_lu<C64, Layout::RowMajor>(const LU<C64, Layout::RowMajor>&, crd::containers::Span<C64>);
template void solve_lu<float, Layout::RowMajor>(const LU<float, Layout::RowMajor>&,
                                                 MatrixView<float, Layout::RowMajor>);
template void solve_lu<double, Layout::RowMajor>(const LU<double, Layout::RowMajor>&,
                                                  MatrixView<double, Layout::RowMajor>);
template void solve_lu<C32, Layout::RowMajor>(const LU<C32, Layout::RowMajor>&, MatrixView<C32, Layout::RowMajor>);
template void solve_lu<C64, Layout::RowMajor>(const LU<C64, Layout::RowMajor>&, MatrixView<C64, Layout::RowMajor>);

} // namespace crd::hesap::dense
