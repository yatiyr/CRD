#include <crd/hesap/dense/blas2.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/detail/pairwise_sum.hpp>

namespace crd::hesap::dense
{

namespace
{
// Element of A at (i, j) for the layout-dispatched MatrixView. Avoids
// re-deriving the offset inside every BLAS L2 op below.
template <typename T, Layout L>
[[nodiscard]] inline T mat_at(const MatrixView<const T, L>& a, crd::usize i, crd::usize j) noexcept
{
    if constexpr (L == Layout::RowMajor)
    {
        return a.data()[i * a.ld() + j];
    }
    else
    {
        return a.data()[j * a.ld() + i];
    }
}

template <typename T, Layout L>
[[nodiscard]] inline T& mat_at_ref(MatrixView<T, L>& a, crd::usize i, crd::usize j) noexcept
{
    if constexpr (L == Layout::RowMajor)
    {
        return a.data()[i * a.ld() + j];
    }
    else
    {
        return a.data()[j * a.ld() + i];
    }
}

template <typename T>
[[nodiscard]] inline T maybe_conj(T v, bool do_conj) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return do_conj ? crd::hesap::conj(v) : v;
    }
    else
    {
        (void)do_conj;
        return v;
    }
}
} // namespace

// =======================================================================
// gemv: y = alpha * op(A) * x + beta * y
// =======================================================================
template <typename T, Layout L>
void gemv(T alpha, MatrixView<const T, L> a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y, Trans trans)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    if (trans == Trans::None)
    {
        CRD_ASSERT_MSG(x.size() == n && y.size() == m, "gemv(None): size mismatch");
        for (crd::usize i = 0; i < m; ++i)
        {
            T sum = detail::pairwise_sum_produced<T>(
                n, [&](crd::usize k) { return mat_at<T, L>(a, i, k) * x[k]; });
            y[i] = alpha * sum + beta * y[i];
        }
    }
    else
    {
        const bool do_conj = (trans == Trans::ConjTranspose);
        CRD_ASSERT_MSG(x.size() == m && y.size() == n, "gemv(Trans): size mismatch");
        for (crd::usize j = 0; j < n; ++j)
        {
            T sum = detail::pairwise_sum_produced<T>(
                m, [&](crd::usize k) { return maybe_conj<T>(mat_at<T, L>(a, k, j), do_conj) * x[k]; });
            y[j] = alpha * sum + beta * y[j];
        }
    }
}

// =======================================================================
// ger: A += alpha * x * y^T (real)
// =======================================================================
template <typename T, Layout L>
void ger(T alpha, crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, MatrixView<T, L> a)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(x.size() == m && y.size() == n, "ger: size mismatch");
    for (crd::usize i = 0; i < m; ++i)
    {
        const T axi = alpha * x[i];
        for (crd::usize j = 0; j < n; ++j)
        {
            mat_at_ref<T, L>(a, i, j) = mat_at_ref<T, L>(a, i, j) + axi * y[j];
        }
    }
}

// =======================================================================
// geru: A += alpha * x * y^T (complex, unconjugated)
// =======================================================================
template <typename T, Layout L>
void geru(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, MatrixView<Complex<T>, L> a)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(x.size() == m && y.size() == n, "geru: size mismatch");
    for (crd::usize i = 0; i < m; ++i)
    {
        const Complex<T> axi = alpha * x[i];
        for (crd::usize j = 0; j < n; ++j)
        {
            mat_at_ref<Complex<T>, L>(a, i, j) =
                mat_at_ref<Complex<T>, L>(a, i, j) + axi * y[j];
        }
    }
}

// =======================================================================
// gerc: A += alpha * x * conj(y)^T (complex, conjugated y)
// =======================================================================
template <typename T, Layout L>
void gerc(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, MatrixView<Complex<T>, L> a)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(x.size() == m && y.size() == n, "gerc: size mismatch");
    for (crd::usize i = 0; i < m; ++i)
    {
        const Complex<T> axi = alpha * x[i];
        for (crd::usize j = 0; j < n; ++j)
        {
            mat_at_ref<Complex<T>, L>(a, i, j) =
                mat_at_ref<Complex<T>, L>(a, i, j) + axi * crd::hesap::conj(y[j]);
        }
    }
}

// =======================================================================
// gbmv: y = alpha * op(A) * x + beta * y (banded)
// =======================================================================
template <typename T>
void gbmv(T alpha, const Banded<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y, Trans trans)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const bool do_conj = (trans == Trans::ConjTranspose);
    if (trans == Trans::None)
    {
        CRD_ASSERT_MSG(x.size() == n && y.size() == m, "gbmv(None): size mismatch");
        for (crd::usize i = 0; i < m; ++i)
        {
            T sum{};
            for (crd::usize j = 0; j < n; ++j)
            {
                if (a.in_band(i, j))
                {
                    sum = sum + a.at_value(i, j) * x[j];
                }
            }
            y[i] = alpha * sum + beta * y[i];
        }
    }
    else
    {
        CRD_ASSERT_MSG(x.size() == m && y.size() == n, "gbmv(Trans): size mismatch");
        for (crd::usize j = 0; j < n; ++j)
        {
            T sum{};
            for (crd::usize i = 0; i < m; ++i)
            {
                if (a.in_band(i, j))
                {
                    sum = sum + maybe_conj<T>(a.at_value(i, j), do_conj) * x[i];
                }
            }
            y[j] = alpha * sum + beta * y[j];
        }
    }
}

// =======================================================================
// symv / hemv
// =======================================================================
template <typename T>
void symv(T alpha, const Symmetric<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "symv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        T sum = detail::pairwise_sum_produced<T>(n, [&](crd::usize k) { return a.at(i, k) * x[k]; });
        y[i] = alpha * sum + beta * y[i];
    }
}

template <typename T>
void hemv(Complex<T> alpha, const Hermitian<Complex<T>>& a, crd::containers::ConstSpan<Complex<T>> x,
          Complex<T> beta, crd::containers::Span<Complex<T>> y)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "hemv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        Complex<T> sum = detail::pairwise_sum_produced<Complex<T>>(
            n, [&](crd::usize k) { return a.at_value(i, k) * x[k]; });
        y[i] = alpha * sum + beta * y[i];
    }
}

// =======================================================================
// syr / her  rank-1 updates
// =======================================================================
template <typename T>
void syr(T alpha, crd::containers::ConstSpan<T> x, Symmetric<T>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "syr: size mismatch");
    // Update only the canonical (lower) triangle; symmetric access mirrors.
    for (crd::usize i = 0; i < n; ++i)
    {
        const T axi = alpha * x[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            a.at(i, j) = a.at(i, j) + axi * x[j];
        }
    }
}

template <typename T>
void her(T alpha, crd::containers::ConstSpan<Complex<T>> x, Hermitian<Complex<T>>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "her: size mismatch");
    // A += alpha * x * x^H. Update only lower triangle of A.
    // Diagonal: A_ii += alpha * |x_i|^2 (real-valued).
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            // Real-alpha rank-1 update: alpha * x_i * conj(x_j).
            const Complex<T> update = Complex<T>{alpha, T{}} * x[i] * crd::hesap::conj(x[j]);
            a.at_lower(i, j) = a.at_lower(i, j) + update;
        }
    }
}

// =======================================================================
// syr2 / her2  rank-2 updates
// =======================================================================
template <typename T>
void syr2(T alpha, crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, Symmetric<T>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "syr2: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        const T axi = alpha * x[i];
        const T ayi = alpha * y[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            a.at(i, j) = a.at(i, j) + axi * y[j] + ayi * x[j];
        }
    }
}

template <typename T>
void her2(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, Hermitian<Complex<T>>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "her2: size mismatch");
    // A += alpha * x * y^H + conj(alpha) * y * x^H
    const Complex<T> alpha_conj = crd::hesap::conj(alpha);
    for (crd::usize i = 0; i < n; ++i)
    {
        const Complex<T> axi = alpha * x[i];
        const Complex<T> acyi = alpha_conj * y[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            const Complex<T> update = axi * crd::hesap::conj(y[j]) + acyi * crd::hesap::conj(x[j]);
            a.at_lower(i, j) = a.at_lower(i, j) + update;
        }
    }
}

// =======================================================================
// sbmv / hbmv  banded symmetric / Hermitian
// =======================================================================
template <typename T>
void sbmv(T alpha, const Banded<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "sbmv: A must be square");
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "sbmv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        T sum{};
        for (crd::usize j = 0; j < n; ++j)
        {
            // Symmetric: use stored band entry, mirror for j > i if stored as lower band.
            // Banded<T> stores entries in-band only; sym implies A_ij = A_ji.
            if (a.in_band(i, j))
            {
                sum = sum + a.at_value(i, j) * x[j];
            }
            else if (a.in_band(j, i))
            {
                sum = sum + a.at_value(j, i) * x[j];
            }
        }
        y[i] = alpha * sum + beta * y[i];
    }
}

template <typename T>
void hbmv(Complex<T> alpha, const Banded<Complex<T>>& a, crd::containers::ConstSpan<Complex<T>> x,
          Complex<T> beta, crd::containers::Span<Complex<T>> y)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "hbmv: A must be square");
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "hbmv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        Complex<T> sum{};
        for (crd::usize j = 0; j < n; ++j)
        {
            if (a.in_band(i, j))
            {
                sum = sum + a.at_value(i, j) * x[j];
            }
            else if (a.in_band(j, i))
            {
                // Hermitian: A_ij = conj(A_ji).
                sum = sum + crd::hesap::conj(a.at_value(j, i)) * x[j];
            }
        }
        y[i] = alpha * sum + beta * y[i];
    }
}

// =======================================================================
// trmv / trsv  triangular
// =======================================================================
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trmv(const Triangular<T, Side, Diag>& a, crd::containers::Span<T> x, Trans trans)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "trmv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    if (trans == Trans::None)
    {
        // x = A * x (in-place; must traverse such that we don't clobber unread values).
        if constexpr (Side == TriangularSide::Lower)
        {
            // Lower: y_i = sum_{j<=i} A_ij * x_j. Traverse i from N-1 down to 0
            // so x_0 ... x_{i-1} aren't yet updated when computing y_i.
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + a.at_value(ii, j) * x[j];
                }
                x[ii] = sum;
            }
        }
        else  // Upper
        {
            // Upper: y_i = sum_{j>=i} A_ij * x_j. Traverse i ascending.
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + a.at_value(i, j) * x[j];
                }
                x[i] = sum;
            }
        }
    }
    else
    {
        // x = A^T * x or A^H * x. For Lower-T, equivalent to Upper-N traversal.
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + maybe_conj<T>(a.at_value(j, i), do_conj) * x[j];
                }
                x[i] = sum;
            }
        }
        else  // Upper
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + maybe_conj<T>(a.at_value(j, ii), do_conj) * x[j];
                }
                x[ii] = sum;
            }
        }
    }
}

template <typename T, TriangularSide Side, TriangularDiag Diag>
void trsv(const Triangular<T, Side, Diag>& a, crd::containers::Span<T> x, Trans trans)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "trsv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    if (trans == Trans::None)
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            // Forward substitution: L * x = b.  x_i = (b_i - sum_{j<i} L_ij*x_j) / L_ii
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - a.at_value(i, j) * x[j];
                }
                if constexpr (Diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / a.at_value(i, i);
                }
            }
        }
        else
        {
            // Back substitution: U * x = b.  x_i = (b_i - sum_{j>i} U_ij*x_j) / U_ii
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - a.at_value(ii, j) * x[j];
                }
                if constexpr (Diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / a.at_value(ii, ii);
                }
            }
        }
    }
    else
    {
        // op(A) = A^T or A^H. For Lower-T, equivalent to Upper-N solve.
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - maybe_conj<T>(a.at_value(j, ii), do_conj) * x[j];
                }
                if constexpr (Diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / maybe_conj<T>(a.at_value(ii, ii), do_conj);
                }
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - maybe_conj<T>(a.at_value(j, i), do_conj) * x[j];
                }
                if constexpr (Diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / maybe_conj<T>(a.at_value(i, i), do_conj);
                }
            }
        }
    }
}

// =======================================================================
// tbmv / tbsv  triangular banded
//
// Banded triangular: the band represents only the canonical (Side) half.
// For Lower: kl = number of sub-diagonals, ku = 0.
// For Upper: kl = 0, ku = number of super-diagonals.
// =======================================================================
template <typename T>
void tbmv(const Banded<T>& a, TriangularSide side, TriangularDiag diag, crd::containers::Span<T> x,
          Trans trans)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "tbmv: A must be square");
    CRD_ASSERT_MSG(x.size() == n, "tbmv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    auto a_diag_aware = [&](crd::usize i, crd::usize j) -> T
    {
        if (diag == TriangularDiag::UnitDiag && i == j)
        {
            return T{1};
        }
        if (!a.in_band(i, j))
        {
            return T{};
        }
        return a.at_value(i, j);
    };

    if (trans == Trans::None)
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + a_diag_aware(ii, j) * x[j];
                }
                x[ii] = sum;
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + a_diag_aware(i, j) * x[j];
                }
                x[i] = sum;
            }
        }
    }
    else
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + maybe_conj<T>(a_diag_aware(j, i), do_conj) * x[j];
                }
                x[i] = sum;
            }
        }
        else
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + maybe_conj<T>(a_diag_aware(j, ii), do_conj) * x[j];
                }
                x[ii] = sum;
            }
        }
    }
}

template <typename T>
void tbsv(const Banded<T>& a, TriangularSide side, TriangularDiag diag, crd::containers::Span<T> x,
          Trans trans)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "tbsv: A must be square");
    CRD_ASSERT_MSG(x.size() == n, "tbsv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    auto a_diag_aware = [&](crd::usize i, crd::usize j) -> T
    {
        if (diag == TriangularDiag::UnitDiag && i == j)
        {
            return T{1};
        }
        if (!a.in_band(i, j))
        {
            return T{};
        }
        return a.at_value(i, j);
    };

    if (trans == Trans::None)
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - a_diag_aware(i, j) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / a_diag_aware(i, i);
                }
            }
        }
        else
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - a_diag_aware(ii, j) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / a_diag_aware(ii, ii);
                }
            }
        }
    }
    else
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - maybe_conj<T>(a_diag_aware(j, ii), do_conj) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / maybe_conj<T>(a_diag_aware(ii, ii), do_conj);
                }
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - maybe_conj<T>(a_diag_aware(j, i), do_conj) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / maybe_conj<T>(a_diag_aware(i, i), do_conj);
                }
            }
        }
    }
}

// =======================================================================
// Explicit instantiations
// =======================================================================

// gemv: 4 types × 2 layouts
template void gemv<crd::f32, Layout::RowMajor>(crd::f32, MatrixView<const crd::f32, Layout::RowMajor>,
    crd::containers::ConstSpan<crd::f32>, crd::f32, crd::containers::Span<crd::f32>, Trans);
template void gemv<crd::f64, Layout::RowMajor>(crd::f64, MatrixView<const crd::f64, Layout::RowMajor>,
    crd::containers::ConstSpan<crd::f64>, crd::f64, crd::containers::Span<crd::f64>, Trans);
template void gemv<Complex32, Layout::RowMajor>(Complex32, MatrixView<const Complex32, Layout::RowMajor>,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>, Trans);
template void gemv<Complex64, Layout::RowMajor>(Complex64, MatrixView<const Complex64, Layout::RowMajor>,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>, Trans);
template void gemv<crd::f32, Layout::ColMajor>(crd::f32, MatrixView<const crd::f32, Layout::ColMajor>,
    crd::containers::ConstSpan<crd::f32>, crd::f32, crd::containers::Span<crd::f32>, Trans);
template void gemv<crd::f64, Layout::ColMajor>(crd::f64, MatrixView<const crd::f64, Layout::ColMajor>,
    crd::containers::ConstSpan<crd::f64>, crd::f64, crd::containers::Span<crd::f64>, Trans);
template void gemv<Complex32, Layout::ColMajor>(Complex32, MatrixView<const Complex32, Layout::ColMajor>,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>, Trans);
template void gemv<Complex64, Layout::ColMajor>(Complex64, MatrixView<const Complex64, Layout::ColMajor>,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>, Trans);

// ger (real): 2 types × 2 layouts (4 total)
template void ger<crd::f32, Layout::RowMajor>(crd::f32, crd::containers::ConstSpan<crd::f32>,
    crd::containers::ConstSpan<crd::f32>, MatrixView<crd::f32, Layout::RowMajor>);
template void ger<crd::f64, Layout::RowMajor>(crd::f64, crd::containers::ConstSpan<crd::f64>,
    crd::containers::ConstSpan<crd::f64>, MatrixView<crd::f64, Layout::RowMajor>);
template void ger<crd::f32, Layout::ColMajor>(crd::f32, crd::containers::ConstSpan<crd::f32>,
    crd::containers::ConstSpan<crd::f32>, MatrixView<crd::f32, Layout::ColMajor>);
template void ger<crd::f64, Layout::ColMajor>(crd::f64, crd::containers::ConstSpan<crd::f64>,
    crd::containers::ConstSpan<crd::f64>, MatrixView<crd::f64, Layout::ColMajor>);

// geru / gerc (complex): 2 types × 2 layouts each (8 total)
template void geru<crd::f32, Layout::RowMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::RowMajor>);
template void geru<crd::f64, Layout::RowMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::RowMajor>);
template void geru<crd::f32, Layout::ColMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::ColMajor>);
template void geru<crd::f64, Layout::ColMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::ColMajor>);
template void gerc<crd::f32, Layout::RowMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::RowMajor>);
template void gerc<crd::f64, Layout::RowMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::RowMajor>);
template void gerc<crd::f32, Layout::ColMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::ColMajor>);
template void gerc<crd::f64, Layout::ColMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::ColMajor>);

// gbmv: 4 types
template void gbmv<crd::f32>(crd::f32, const Banded<crd::f32>&, crd::containers::ConstSpan<crd::f32>,
    crd::f32, crd::containers::Span<crd::f32>, Trans);
template void gbmv<crd::f64>(crd::f64, const Banded<crd::f64>&, crd::containers::ConstSpan<crd::f64>,
    crd::f64, crd::containers::Span<crd::f64>, Trans);
template void gbmv<Complex32>(Complex32, const Banded<Complex32>&, crd::containers::ConstSpan<Complex32>,
    Complex32, crd::containers::Span<Complex32>, Trans);
template void gbmv<Complex64>(Complex64, const Banded<Complex64>&, crd::containers::ConstSpan<Complex64>,
    Complex64, crd::containers::Span<Complex64>, Trans);

// symv: real-only
template void symv<crd::f32>(crd::f32, const Symmetric<crd::f32>&, crd::containers::ConstSpan<crd::f32>,
    crd::f32, crd::containers::Span<crd::f32>);
template void symv<crd::f64>(crd::f64, const Symmetric<crd::f64>&, crd::containers::ConstSpan<crd::f64>,
    crd::f64, crd::containers::Span<crd::f64>);

// hemv: complex-only
template void hemv<crd::f32>(Complex32, const Hermitian<Complex32>&,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>);
template void hemv<crd::f64>(Complex64, const Hermitian<Complex64>&,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>);

// syr / syr2: real-only
template void syr<crd::f32>(crd::f32, crd::containers::ConstSpan<crd::f32>, Symmetric<crd::f32>&);
template void syr<crd::f64>(crd::f64, crd::containers::ConstSpan<crd::f64>, Symmetric<crd::f64>&);
template void syr2<crd::f32>(crd::f32, crd::containers::ConstSpan<crd::f32>,
    crd::containers::ConstSpan<crd::f32>, Symmetric<crd::f32>&);
template void syr2<crd::f64>(crd::f64, crd::containers::ConstSpan<crd::f64>,
    crd::containers::ConstSpan<crd::f64>, Symmetric<crd::f64>&);

// her / her2: complex-only (T = f32/f64 = underlying real)
template void her<crd::f32>(crd::f32, crd::containers::ConstSpan<Complex32>, Hermitian<Complex32>&);
template void her<crd::f64>(crd::f64, crd::containers::ConstSpan<Complex64>, Hermitian<Complex64>&);
template void her2<crd::f32>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, Hermitian<Complex32>&);
template void her2<crd::f64>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, Hermitian<Complex64>&);

// sbmv: real-only banded
template void sbmv<crd::f32>(crd::f32, const Banded<crd::f32>&, crd::containers::ConstSpan<crd::f32>,
    crd::f32, crd::containers::Span<crd::f32>);
template void sbmv<crd::f64>(crd::f64, const Banded<crd::f64>&, crd::containers::ConstSpan<crd::f64>,
    crd::f64, crd::containers::Span<crd::f64>);

// hbmv: complex-only banded
template void hbmv<crd::f32>(Complex32, const Banded<Complex32>&,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>);
template void hbmv<crd::f64>(Complex64, const Banded<Complex64>&,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>);

// trmv / trsv: 4 types × 2 sides × 2 diags = 16 each, but unit-diag-upper
// is rare; for v0c we ship the 4 type variants for {Lower, Upper} × {Explicit}.
// UnitDiag variants are filed as v0c-unitdiag follow-on (matches LAPACK API
// shape where unit-diag is a runtime flag, not a type parameter).
template void trmv<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trmv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trmv<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trmv<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);
template void trmv<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trmv<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trmv<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trmv<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);

template void trsv<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trsv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trsv<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trsv<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);
template void trsv<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trsv<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trsv<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trsv<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);

// tbmv / tbsv: 4 types each (Side / Diag are runtime params here for ergonomics
// since banded storage doesn't compile-time-vary).
template void tbmv<crd::f32>(const Banded<crd::f32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f32>, Trans);
template void tbmv<crd::f64>(const Banded<crd::f64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f64>, Trans);
template void tbmv<Complex32>(const Banded<Complex32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex32>, Trans);
template void tbmv<Complex64>(const Banded<Complex64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex64>, Trans);

template void tbsv<crd::f32>(const Banded<crd::f32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f32>, Trans);
template void tbsv<crd::f64>(const Banded<crd::f64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f64>, Trans);
template void tbsv<Complex32>(const Banded<Complex32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex32>, Trans);
template void tbsv<Complex64>(const Banded<Complex64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex64>, Trans);

} // namespace crd::hesap::dense
