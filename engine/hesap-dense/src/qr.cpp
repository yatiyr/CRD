#include <crd/hesap/dense/qr.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cmath>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// Block size for compact-WY Householder QR (LAPACK xGEQRT default).
// At nb=32, panel-factor cost is small + the trailing update is the
// dominant cost, which routes through gemm_parallel for full 16-way
// parallelism on the i9-14900K.
constexpr crd::usize kQrBlockSize = 32;

// Crossover (tall/square m≥n): for n at or below this, the unblocked single-panel
// QR beats the blocked compact-WY path (whose panel-transpose + T-build + gemm
// setup don't amortize at small/mid n). Calibrated for tall m≈2n via
// bench_hesap_lstsq_vs_reference (v3c-1c): unblocked wins vs blocked through
// n≈128 (and beats Eigen at n=64, parity at n=96). Square / very-tall shapes may
// prefer a different threshold and will be revisited when a consumer hits it.
constexpr crd::usize kQrUnblockedMax = 128;

// SIMD axpy-negate-scaled: row[p] += s * col[p] over [0,len). f64/f32.
template <typename T>
inline void qr_axpy(T* row, const T* col, T s, crd::usize len) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        const simd::Vec4d sv(s);
        for (; p + 16 <= len; p += 16)
        {
            simd::Vec4d r0 = simd::fma(sv, simd::Vec4d::load(col + p), simd::Vec4d::load(row + p));
            simd::Vec4d r1 =
                simd::fma(sv, simd::Vec4d::load(col + p + 4), simd::Vec4d::load(row + p + 4));
            simd::Vec4d r2 =
                simd::fma(sv, simd::Vec4d::load(col + p + 8), simd::Vec4d::load(row + p + 8));
            simd::Vec4d r3 =
                simd::fma(sv, simd::Vec4d::load(col + p + 12), simd::Vec4d::load(row + p + 12));
            r0.store(row + p);
            r1.store(row + p + 4);
            r2.store(row + p + 8);
            r3.store(row + p + 12);
        }
        for (; p + 4 <= len; p += 4)
        {
            simd::Vec4d r = simd::fma(sv, simd::Vec4d::load(col + p), simd::Vec4d::load(row + p));
            r.store(row + p);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        const simd::Vec8f sv(s);
        for (; p + 8 <= len; p += 8)
        {
            simd::Vec8f r = simd::fma(sv, simd::Vec8f::load(col + p), simd::Vec8f::load(row + p));
            r.store(row + p);
        }
    }
    for (; p < len; ++p)
    {
        row[p] += s * col[p];
    }
}

// SIMD dot product over contiguous [0, len).
template <typename T>
inline T qr_dot(const T* a, const T* b, crd::usize len) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    T acc = T{0};
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        simd::Vec4d v0 = simd::Vec4d::zero();
        simd::Vec4d v1 = simd::Vec4d::zero();
        for (; p + 8 <= len; p += 8)
        {
            v0 = simd::fma(simd::Vec4d::load(a + p), simd::Vec4d::load(b + p), v0);
            v1 = simd::fma(simd::Vec4d::load(a + p + 4), simd::Vec4d::load(b + p + 4), v1);
        }
        acc = simd::horizontal_sum(v0 + v1);
        for (; p < len; ++p)
        {
            acc += a[p] * b[p];
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        simd::Vec8f v0 = simd::Vec8f::zero();
        for (; p + 8 <= len; p += 8)
        {
            v0 = simd::fma(simd::Vec8f::load(a + p), simd::Vec8f::load(b + p), v0);
        }
        acc = simd::horizontal_sum(v0);
        for (; p < len; ++p)
        {
            acc += a[p] * b[p];
        }
    }
    else
    {
        for (; p < len; ++p)
        {
            acc += a[p] * b[p];
        }
    }
    return acc;
}

template <typename T>
inline T sign_or_one(T x) noexcept
{
    return x >= T{0} ? T{1} : T{-1};
}

template <typename T>
inline T sqrt_value(T x) noexcept
{
    return std::sqrt(x);
}

// Unblocked Householder factor on the panel A[k:m, k:k+nb], performed on
// a TRANSPOSED scratch `pt` (nb × (m-k), row-major) where pt[c][r] =
// packed[k+r][k+c]. Working on the transpose makes every per-column
// operation (norm, scale, dot, axpy) a CONTIGUOUS row sweep → fully
// SIMD-vectorizable. The caller transposes the panel in/out.
//
// Writes tau values into taus[k..k+nb-1]. On exit, pt holds R (upper
// reflected) + V subvecs in the transposed layout.
template <typename T>
void panel_factor_qr_transposed(T* pt, crd::usize pt_ld, crd::usize panel_rows, crd::usize k,
                                crd::usize nb, T* taus_arr)
{
    for (crd::usize j = 0; j < nb; ++j)
    {
        // Column j of the panel = pt row j, sub-diagonal part [j, panel_rows).
        T* vj = pt + j * pt_ld;  // contiguous row
        const T alpha_orig = vj[j];
        const crd::usize tail = panel_rows - j - 1;  // elements below diagonal
        const T xnorm_sq = (tail > 0) ? qr_dot<T>(vj + j + 1, vj + j + 1, tail) : T{0};
        if (xnorm_sq == T{0})
        {
            taus_arr[k + j] = T{0};
            continue;
        }
        const T beta = -sign_or_one<T>(alpha_orig) *
                       sqrt_value<T>(alpha_orig * alpha_orig + xnorm_sq);
        const T tau = (beta - alpha_orig) / beta;
        const T inv_diff = T{1} / (alpha_orig - beta);
        // Scale v subvec (contiguous).
        for (crd::usize r = j + 1; r < panel_rows; ++r)
        {
            vj[r] *= inv_diff;
        }
        vj[j] = beta;
        taus_arr[k + j] = tau;

        // Apply H_j to remaining panel columns jj (= pt rows jj).
        for (crd::usize jj = j + 1; jj < nb; ++jj)
        {
            T* vjj = pt + jj * pt_ld;
            // w = vjj[j] (the diagonal-aligned element, v_j[j]=1 implicit)
            //   + sum_{r>j} vj[r] * vjj[r]  (contiguous dot)
            T w = vjj[j] + ((tail > 0) ? qr_dot<T>(vj + j + 1, vjj + j + 1, tail) : T{0});
            w *= tau;
            vjj[j] -= w;
            // vjj[r] -= w * vj[r] for r in (j, panel_rows)  (contiguous axpy)
            if (tail > 0)
            {
                qr_axpy<T>(vjj + j + 1, vj + j + 1, -w, tail);
            }
        }
    }
}

// Build the compact-WY T matrix (nb × nb upper-triangular) from a
// PRE-MATERIALIZED V (rows × nb, row-major, with implicit diag = 1 made
// explicit, strict-upper zeroed).
//
// vtv = V^T · V is a precomputed nb × nb matrix (row-major leading dim
// nb). For each col j, T[0:j, j] = -tau_j · vtv[0:j, j], then
// T[0:j, j] = T[0:j, 0:j] · T[0:j, j] in place.
template <typename T>
void build_block_t_from_vtv(const T* vtv, crd::usize vtv_ld, const T* taus_arr, crd::usize k,
                            crd::usize nb, T* t_block, crd::usize t_ld)
{
    for (crd::usize j = 0; j < nb; ++j)
    {
        const T tau_j = taus_arr[k + j];
        for (crd::usize i = 0; i < j; ++i)
        {
            t_block[i * t_ld + j] = -tau_j * vtv[i * vtv_ld + j];
        }
        t_block[j * t_ld + j] = tau_j;
        // Triangle-multiply T[0:j, j] = T[0:j, 0:j] · T[0:j, j] in place.
        for (crd::usize ii = 0; ii < j; ++ii)
        {
            T s = T{0};
            for (crd::usize kk = ii; kk < j; ++kk)
            {
                s += t_block[ii * t_ld + kk] * t_block[kk * t_ld + j];
            }
            t_block[ii * t_ld + j] = s;
        }
    }
}

// Materialize V from the strict lower triangle of the panel into an
// explicit (m-k) × nb scratch matrix with V[i, i] = 1 (diagonal).
template <typename T>
void materialize_panel_v(const T* data, crd::usize m, crd::usize ld, crd::usize k,
                         crd::usize nb, T* v_out, crd::usize v_ld)
{
    const crd::usize rows = m - k;
    for (crd::usize i = 0; i < rows; ++i)
    {
        for (crd::usize j = 0; j < nb; ++j)
        {
            if (i < j)
            {
                v_out[i * v_ld + j] = T{0};
            }
            else if (i == j)
            {
                v_out[i * v_ld + j] = T{1};
            }
            else
            {
                v_out[i * v_ld + j] = data[(k + i) * ld + (k + j)];
            }
        }
    }
}

} // namespace

template <typename T, Layout L>
void factor_qr(QR<T, L>& qr)
{
    static_assert(L == Layout::RowMajor, "factor_qr currently supports RowMajor only");

    Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize n = packed.cols();
    const crd::usize k_count = m < n ? m : n;
    auto& taus = qr.taus();
    CRD_ASSERT_MSG(taus.size() == k_count, "factor_qr: taus size mismatch");
    T* data = packed.data();
    const crd::usize ld = packed.ld();

    if (k_count == 0)
    {
        return;
    }

    // Below the crossover (tall/square only), the unblocked single-panel QR
    // wins — dispatch there. Wide (m<n) always uses the blocked path.
    if (m >= n && n <= kQrUnblockedMax)
    {
        factor_qr_unblocked<T, L>(qr);
        return;
    }

    crd::memory::IAllocator* alloc = packed.allocator();

    // Scratch buffers reused across panels.
    crd::containers::Array<T> t_buf(alloc);
    t_buf.resize(kQrBlockSize * kQrBlockSize);
    crd::containers::Array<T> v_buf(alloc);
    v_buf.resize(m * kQrBlockSize);
    crd::containers::Array<T> w_buf(alloc);
    w_buf.resize(kQrBlockSize * n);
    crd::containers::Array<T> w_tmp(alloc);
    w_tmp.resize(kQrBlockSize * n);
    crd::containers::Array<T> vtv_buf(alloc);
    vtv_buf.resize(kQrBlockSize * kQrBlockSize);
    crd::containers::Array<T> pt_buf(alloc);  // transposed panel scratch (nb × m)
    pt_buf.resize(kQrBlockSize * m);

    for (crd::usize k = 0; k < k_count; k += kQrBlockSize)
    {
        const crd::usize nb = (k + kQrBlockSize <= k_count) ? kQrBlockSize : (k_count - k);
        const crd::usize panel_rows = m - k;

        // Step 1: transpose the panel into pt (nb × panel_rows), factor with
        // contiguous SIMD sweeps, transpose back.
        const crd::usize pt_ld = panel_rows;
        for (crd::usize c = 0; c < nb; ++c)
        {
            for (crd::usize r = 0; r < panel_rows; ++r)
            {
                pt_buf[c * pt_ld + r] = data[(k + r) * ld + (k + c)];
            }
        }
        panel_factor_qr_transposed<T>(pt_buf.data(), pt_ld, panel_rows, k, nb, taus.data());
        for (crd::usize c = 0; c < nb; ++c)
        {
            for (crd::usize r = 0; r < panel_rows; ++r)
            {
                data[(k + r) * ld + (k + c)] = pt_buf[c * pt_ld + r];
            }
        }

        // Step 2: apply panel reflectors to the trailing matrix A[k:m, k+nb:n]
        // via the compact-WY identity H_panel = I - V·T·V^T:
        //   W = V^T · A_trail     (nb × (n-k-nb))
        //   W = T^T · W
        //   A_trail -= V · W
        const crd::usize trail_cols = n - (k + nb);
        if (trail_cols == 0)
        {
            continue;
        }
        const crd::usize rows = m - k;

        // Materialize V (rows × nb) with implicit diagonal made explicit.
        materialize_panel_v<T>(data, m, ld, k, nb, v_buf.data(), kQrBlockSize);

        // Compute vtv = V^T · V (nb × nb) via gemm — SIMD path replaces the
        // scalar inner-product loops of build_block_T. V is rows × nb,
        // result is nb × nb.
        MatrixView<const T, L> v_full_for_t{v_buf.data(), rows, nb, kQrBlockSize};
        MatrixView<T, L> vtv_view{vtv_buf.data(), nb, nb, kQrBlockSize};
        gemm<T, L>(T{1}, v_full_for_t, v_full_for_t, T{0}, vtv_view, Trans::Transpose,
                   Trans::None, alloc);

        // Build T from vtv (small nb² in-cache work).
        build_block_t_from_vtv<T>(vtv_buf.data(), kQrBlockSize, taus.data(), k, nb, t_buf.data(),
                                   kQrBlockSize);

        // W = V^T · A_trail. Use gemm_parallel with TransA=Transpose.
        // V is rows × nb (RowMajor with ld=kQrBlockSize).
        // A_trail is rows × trail_cols (RowMajor with ld=ld, base at data + k*ld + k+nb).
        // W goes to w_buf nb × trail_cols (RowMajor with ld=trail_cols).
        MatrixView<const T, L> v_view{v_buf.data(), rows, nb, kQrBlockSize};
        MatrixView<const T, L> a_trail_const{data + k * ld + (k + nb), rows, trail_cols, ld};
        MatrixView<T, L> w_view{w_buf.data(), nb, trail_cols, trail_cols};
        gemm_parallel_auto<T, L>(T{1}, v_view, a_trail_const, T{0}, w_view, Trans::Transpose,
                                  Trans::None, alloc);

        // W' = T^T · W via small gemm (T is nb × nb upper-triangular, treated
        // as a square nb × nb for the transpose). Result lands in w_tmp.
        MatrixView<const T, L> t_view{t_buf.data(), nb, nb, kQrBlockSize};
        MatrixView<const T, L> w_in{w_buf.data(), nb, trail_cols, trail_cols};
        MatrixView<T, L> w_tmp_view{w_tmp.data(), nb, trail_cols, trail_cols};
        gemm<T, L>(T{1}, t_view, w_in, T{0}, w_tmp_view, Trans::Transpose, Trans::None, alloc);

        // A_trail -= V · w_tmp.
        MatrixView<const T, L> w_const{w_tmp.data(), nb, trail_cols, trail_cols};
        MatrixView<T, L> a_trail_mut{data + k * ld + (k + nb), rows, trail_cols, ld};
        gemm_parallel_auto<T, L>(T{-1}, v_view, w_const, T{1}, a_trail_mut, Trans::None,
                                  Trans::None, alloc);
    }
}

template <typename T, Layout L>
void factor_qr_unblocked(QR<T, L>& qr)
{
    static_assert(L == Layout::RowMajor, "factor_qr_unblocked currently supports RowMajor only");
    Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize n = packed.cols();
    const crd::usize k_count = m < n ? m : n;
    auto& taus = qr.taus();
    CRD_ASSERT_MSG(taus.size() == k_count, "factor_qr_unblocked: taus size mismatch");
    CRD_ASSERT_MSG(m >= n, "factor_qr_unblocked: requires m >= n (tall/square)");
    if (k_count == 0)
    {
        return;
    }
    T* data = packed.data();
    const crd::usize ld = packed.ld();
    crd::memory::IAllocator* alloc = packed.allocator();

    // Factor the WHOLE matrix as a single panel (nb = k_count = n) on a
    // transposed scratch pt (n × m): pt[c][r] = A[r][c]. Each column op then
    // sweeps a contiguous row of pt (SIMD + the ADR-0083 layout escape). For
    // m≥n this is a complete unblocked QR — the inner reflector-apply loop in
    // panel_factor_qr_transposed already updates every trailing column.
    crd::containers::Array<T> pt_buf(alloc);
    pt_buf.resize(k_count * m);
    const crd::usize pt_ld = m;
    for (crd::usize c = 0; c < k_count; ++c)
    {
        for (crd::usize r = 0; r < m; ++r)
        {
            pt_buf[c * pt_ld + r] = data[r * ld + c];
        }
    }
    panel_factor_qr_transposed<T>(pt_buf.data(), pt_ld, m, 0, k_count, taus.data());
    for (crd::usize c = 0; c < k_count; ++c)
    {
        for (crd::usize r = 0; r < m; ++r)
        {
            data[r * ld + c] = pt_buf[c * pt_ld + r];
        }
    }
}

// ---- apply_q_transpose / apply_q / solve_qr unchanged --------------

template <typename T, Layout L>
void apply_q_transpose(const QR<T, L>& qr, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "apply_q_transpose currently supports RowMajor only");
    const Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize k_count = qr.num_reflectors();
    CRD_ASSERT_MSG(x.size() == m, "apply_q_transpose: x size != m");
    const T* data = packed.data();
    const crd::usize ld = packed.ld();
    const auto& taus = qr.taus();

    for (crd::usize k = 0; k < k_count; ++k)
    {
        const T tau = taus[k];
        if (tau == T{0})
        {
            continue;
        }
        T w = x[k];
        for (crd::usize i = k + 1; i < m; ++i)
        {
            w += data[i * ld + k] * x[i];
        }
        w *= tau;
        x[k] -= w;
        for (crd::usize i = k + 1; i < m; ++i)
        {
            x[i] -= w * data[i * ld + k];
        }
    }
}

template <typename T, Layout L>
void apply_q(const QR<T, L>& qr, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "apply_q currently supports RowMajor only");
    const Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize k_count = qr.num_reflectors();
    CRD_ASSERT_MSG(x.size() == m, "apply_q: x size != m");
    const T* data = packed.data();
    const crd::usize ld = packed.ld();
    const auto& taus = qr.taus();

    for (crd::usize kk = k_count; kk-- > 0;)
    {
        const T tau = taus[kk];
        if (tau == T{0})
        {
            continue;
        }
        T w = x[kk];
        for (crd::usize i = kk + 1; i < m; ++i)
        {
            w += data[i * ld + kk] * x[i];
        }
        w *= tau;
        x[kk] -= w;
        for (crd::usize i = kk + 1; i < m; ++i)
        {
            x[i] -= w * data[i * ld + kk];
        }
    }
}

template <typename T, Layout L>
void solve_qr(const QR<T, L>& qr, crd::containers::Span<T> b)
{
    static_assert(L == Layout::RowMajor, "solve_qr currently supports RowMajor only");
    const Matrix<T, L>& packed = qr.packed();
    [[maybe_unused]] const crd::usize m = packed.rows();
    const crd::usize n = packed.cols();
    CRD_ASSERT_MSG(b.size() == m, "solve_qr: b size != m");
    CRD_ASSERT_MSG(m >= n, "solve_qr: requires m >= n (over- or square-determined)");

    apply_q_transpose<T, L>(qr, b);

    const T* data = packed.data();
    const crd::usize ld = packed.ld();
    for (crd::usize ii = n; ii-- > 0;)
    {
        T s = b[ii];
        for (crd::usize j = ii + 1; j < n; ++j)
        {
            s -= data[ii * ld + j] * b[j];
        }
        const T diag = data[ii * ld + ii];
        CRD_ASSERT_MSG(diag != T{0}, "solve_qr: R has zero diagonal (rank-deficient)");
        b[ii] = s / diag;
    }
}

template void factor_qr<float, Layout::RowMajor>(QR<float, Layout::RowMajor>&);
template void factor_qr<double, Layout::RowMajor>(QR<double, Layout::RowMajor>&);
template void factor_qr_unblocked<float, Layout::RowMajor>(QR<float, Layout::RowMajor>&);
template void factor_qr_unblocked<double, Layout::RowMajor>(QR<double, Layout::RowMajor>&);
template void apply_q_transpose<float, Layout::RowMajor>(const QR<float, Layout::RowMajor>&,
                                                          crd::containers::Span<float>);
template void apply_q_transpose<double, Layout::RowMajor>(const QR<double, Layout::RowMajor>&,
                                                           crd::containers::Span<double>);
template void apply_q<float, Layout::RowMajor>(const QR<float, Layout::RowMajor>&,
                                                crd::containers::Span<float>);
template void apply_q<double, Layout::RowMajor>(const QR<double, Layout::RowMajor>&,
                                                 crd::containers::Span<double>);
template void solve_qr<float, Layout::RowMajor>(const QR<float, Layout::RowMajor>&,
                                                 crd::containers::Span<float>);
template void solve_qr<double, Layout::RowMajor>(const QR<double, Layout::RowMajor>&,
                                                  crd::containers::Span<double>);

} // namespace crd::hesap::dense
