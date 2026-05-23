#pragma once

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/block_reflector.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b-1b-perf — blocked dorgbr (form the orthogonal factors of a
// Golub-Kahan bidiagonalization in BLAS-3).
//
//   orgbr_q : build U (m x n) = Q[:, :n], Q = H(0) H(1) ... H(n-1) from the
//             LEFT reflectors (sub-diagonal columns of the reduced A).
//   orgbr_p : build VT (n x n) = P^T, P = G(0) G(1) ... G(n-2) from the RIGHT
//             reflectors (super-diagonal rows of the reduced A).
//
// Both reduce to the SAME columnwise compact-WY apply (`dlarfb`): the right
// reflector G(g) (unit at column g+1, tail in row g) is, read as a column
// vector, identical to a columnwise reflector C(g) with unit at ROW g+1. So
// P = C(0)...C(n-2) is a columnwise product; we build M = P with the exact Q
// machinery (just a +1 row offset and reading the tail along a matrix row),
// then transpose: VT = M^T. This is why orgbr_p reuses build_block_t_from_vtv
// and the three GEMMs unchanged.
//
// Replaces the serial scalar reflector-apply (the v3b-1b form_q_bidiag /
// form_pt_bidiag), which ran at ~1.5 GFLOPS and dominated full-SVD cost
// (~49% at N=512). The blocked path lifts it to BLAS-3 (GEMM throughput +
// the MT gemm_parallel_auto trailing apply).
//
// Lower layer: raw f32/f64 (ADR-0078). Real T only (complex is v3b-1c).
// -----------------------------------------------------------------------

// Panel width for the blocked path; below 2*nb columns the scalar reflector
// apply wins (block-WY setup overhead, matches LAPACK's unblocked crossover).
constexpr crd::usize kOrgbrBlock = 32;

// ---- orgbr_q: scalar (small-n / oracle) -------------------------------
// U (m x n) = first n columns of Q = H(0)...H(n-1). Reflector H(ii) has v[0]=1
// at row ii, tail a[(ii+1..m-1)*lda + ii]. Start from [I_n; 0] and apply the
// reflectors from the LEFT in reverse order ii = n-1 .. 0.
template <typename T>
inline void orgbr_q_scalar(const T* a, crd::usize m, crd::usize n, crd::usize lda, const T* tauq, T* u) noexcept
{
    for (crd::usize i = 0; i < m * n; ++i)
    {
        u[i] = T{0};
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        u[j * n + j] = T{1};
    }
    for (crd::usize ii = n; ii-- > 0;)
    {
        const T tau = tauq[ii];
        if (tau == T{0})
        {
            continue;
        }
        for (crd::usize col = 0; col < n; ++col)
        {
            T dot = u[ii * n + col];  // v[0]=1 at row ii
            for (crd::usize k = ii + 1; k < m; ++k)
            {
                dot += a[k * lda + ii] * u[k * n + col];
            }
            const T s = tau * dot;
            u[ii * n + col] -= s;
            for (crd::usize k = ii + 1; k < m; ++k)
            {
                u[k * n + col] -= a[k * lda + ii] * s;
            }
        }
    }
}

// ---- orgbr_p: scalar (small-n / oracle) -------------------------------
// VT (n x n) = P^T = G(n-2)...G(0). Reflector G(i) has v[0]=1 at column i+1,
// tail a[i*lda + (i+2..n-1)]. Start from I_n and apply G(i) from the LEFT in
// FORWARD order i = 0 .. n-2 (= P^T).
template <typename T>
inline void orgbr_p_scalar(const T* a, crd::usize n, crd::usize lda, const T* taup, T* vt) noexcept
{
    for (crd::usize i = 0; i < n * n; ++i)
    {
        vt[i] = T{0};
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        vt[j * n + j] = T{1};
    }
    if (n < 2)
    {
        return;
    }
    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        const T tau = taup[i];
        if (tau == T{0})
        {
            continue;
        }
        for (crd::usize col = 0; col < n; ++col)
        {
            T dot = vt[(i + 1) * n + col];  // v[0]=1 at row i+1
            for (crd::usize k = i + 2; k < n; ++k)
            {
                dot += a[i * lda + k] * vt[k * n + col];
            }
            const T s = tau * dot;
            vt[(i + 1) * n + col] -= s;
            for (crd::usize k = i + 2; k < n; ++k)
            {
                vt[k * n + col] -= a[i * lda + k] * s;
            }
        }
    }
}

// Apply one block reflector (I - V T V^T) from the LEFT to the `rows x nfull`
// trailing block C (row-major, ld = nfull), using pre-built V (rows x nb,
// ld = nb, unit-diagonal made explicit) and T (nb x nb, ld = nb upper-tri).
// Three GEMMs (dlarfb): W = V^T C ; W = T^T W ; C -= V W. Scratch wbuf/wtmp
// each >= nb*nfull.
template <typename T>
inline void apply_block_reflector_left(const T* vbuf, crd::usize rows, crd::usize nb, const T* tbuf,
                                       T* cblock, crd::usize nfull, T* wbuf, T* wtmp,
                                       crd::memory::IAllocator* alloc) noexcept
{
    using L = Layout;
    MatrixView<const T, L::RowMajor> v_view{vbuf, rows, nb, nb};
    MatrixView<const T, L::RowMajor> c_const{cblock, rows, nfull, nfull};
    MatrixView<T, L::RowMajor> w_view{wbuf, nb, nfull, nfull};
    // W = V^T * C   (nb x nfull)
    gemm_parallel_auto<T, L::RowMajor>(T{1}, v_view, c_const, T{0}, w_view, Trans::Transpose, Trans::None, alloc);
    // W' = T * W  (nb x nfull). NOTE: forming Q applies the block reflector
    // H = I - V T V^T itself (T, NOT T^T). qr.cpp uses T^T because it applies
    // Q_panel^T to reduce A->R; here we build Q, the opposite direction.
    MatrixView<const T, L::RowMajor> t_view{tbuf, nb, nb, nb};
    MatrixView<const T, L::RowMajor> w_in{wbuf, nb, nfull, nfull};
    MatrixView<T, L::RowMajor> wtmp_view{wtmp, nb, nfull, nfull};
    gemm<T, L::RowMajor>(T{1}, t_view, w_in, T{0}, wtmp_view, Trans::None, Trans::None, alloc);
    // C -= V * W'
    MatrixView<const T, L::RowMajor> wtmp_const{wtmp, nb, nfull, nfull};
    MatrixView<T, L::RowMajor> c_mut{cblock, rows, nfull, nfull};
    gemm_parallel_auto<T, L::RowMajor>(T{-1}, v_view, wtmp_const, T{1}, c_mut, Trans::None, Trans::None, alloc);
}

// ---- orgbr_q: blocked dispatch ----------------------------------------
template <typename T>
inline void orgbr_q(const T* a, crd::usize m, crd::usize n, crd::usize lda, const T* tauq, T* u,
                    crd::memory::IAllocator* scratch)
{
    if (n == 0 || m == 0)
    {
        return;
    }
    const crd::usize nb = kOrgbrBlock;
    if (n <= 2 * nb)
    {
        orgbr_q_scalar<T>(a, m, n, lda, tauq, u);
        return;
    }

    // U = [I_n; 0]  (m x n, RowMajor ld = n).
    for (crd::usize i = 0; i < m * n; ++i)
    {
        u[i] = T{0};
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        u[j * n + j] = T{1};
    }

    crd::containers::Array<T> vbuf(scratch);
    crd::containers::Array<T> vtv(scratch);
    crd::containers::Array<T> tbuf(scratch);
    crd::containers::Array<T> wbuf(scratch);
    crd::containers::Array<T> wtmp(scratch);
    vbuf.resize(m * nb);
    vtv.resize(nb * nb);
    tbuf.resize(nb * nb);
    wbuf.resize(nb * n);
    wtmp.resize(nb * n);

    // Process blocks of reflector columns from last to first.
    crd::usize kb = (n / nb) * nb;  // start of the last block
    if (kb == n)
    {
        kb -= nb;
    }
    while (true)
    {
        const crd::usize nbk = (kb + nb <= n) ? nb : (n - kb);
        const crd::usize rows = m - kb;  // active trailing rows [kb, m)

        // Materialize V (rows x nbk, ld = nb): columnwise reflectors from
        // origin (kb, kb). V[r][c]: 0 above unit, 1 on diagonal, else A.
        for (crd::usize r = 0; r < rows; ++r)
        {
            for (crd::usize c = 0; c < nbk; ++c)
            {
                T val;
                if (r < c)
                {
                    val = T{0};
                }
                else if (r == c)
                {
                    val = T{1};
                }
                else
                {
                    val = a[(kb + r) * lda + (kb + c)];
                }
                vbuf[r * nbk + c] = val;  // leading dim nbk (consistent with apply)
            }
        }

        // vtv = V^T V (nbk x nbk), then T.
        MatrixView<const T, Layout::RowMajor> v_for_t{vbuf.data(), rows, nbk, nbk};
        MatrixView<T, Layout::RowMajor> vtv_view{vtv.data(), nbk, nbk, nbk};
        gemm<T, Layout::RowMajor>(T{1}, v_for_t, v_for_t, T{0}, vtv_view, Trans::Transpose, Trans::None,
                                  scratch);
        build_block_t_from_vtv<T>(vtv.data(), nbk, tauq, kb, nbk, tbuf.data(), nbk);

        // Apply (I - V T V^T) to U[kb:m, 0:n].
        apply_block_reflector_left<T>(vbuf.data(), rows, nbk, tbuf.data(), u + kb * n, n, wbuf.data(),
                                      wtmp.data(), scratch);

        if (kb == 0)
        {
            break;
        }
        kb -= nb;
    }
}

// ---- orgbr_p: blocked dispatch ----------------------------------------
template <typename T>
inline void orgbr_p(const T* a, crd::usize n, crd::usize lda, const T* taup, T* vt,
                    crd::memory::IAllocator* scratch)
{
    if (n == 0)
    {
        return;
    }
    const crd::usize nb = kOrgbrBlock;
    if (n <= 2 * nb)
    {
        orgbr_p_scalar<T>(a, n, lda, taup, vt);
        return;
    }

    // Build M = P = C(0)...C(n-2) into mbuf (n x n), start from I_n; reflector
    // C(g) acts on rows [g+1, n) (unit at row g+1). There are n-1 reflectors.
    crd::containers::Array<T> mbuf(scratch);
    mbuf.resize(n * n);
    for (crd::usize i = 0; i < n * n; ++i)
    {
        mbuf[i] = T{0};
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        mbuf[j * n + j] = T{1};
    }

    crd::containers::Array<T> vbuf(scratch);
    crd::containers::Array<T> vtv(scratch);
    crd::containers::Array<T> tbuf(scratch);
    crd::containers::Array<T> wbuf(scratch);
    crd::containers::Array<T> wtmp(scratch);
    vbuf.resize(n * nb);
    vtv.resize(nb * nb);
    tbuf.resize(nb * nb);
    wbuf.resize(nb * n);
    wtmp.resize(nb * n);

    const crd::usize kcount = n - 1;  // reflectors g = 0 .. n-2
    crd::usize kb = (kcount / nb) * nb;
    if (kb == kcount && kcount > 0)
    {
        kb -= nb;
    }
    while (true)
    {
        const crd::usize nbk = (kb + nb <= kcount) ? nb : (kcount - kb);
        if (nbk == 0)
        {
            if (kb == 0)
            {
                break;
            }
            kb -= nb;
            continue;
        }
        const crd::usize r0 = kb + 1;       // first active row (unit of C(kb))
        const crd::usize rows = n - r0;     // active trailing rows [r0, n)

        // Materialize V (rows x nbk, ld = nb): reflector g = kb+c has unit at
        // row g+1 = (kb+c)+1; tail a[g*lda + (kb+1+r)] for r > c. Active row
        // index r is measured from r0 = kb+1.
        for (crd::usize r = 0; r < rows; ++r)
        {
            for (crd::usize c = 0; c < nbk; ++c)
            {
                T val;
                if (r < c)
                {
                    val = T{0};
                }
                else if (r == c)
                {
                    val = T{1};
                }
                else
                {
                    val = a[(kb + c) * lda + (r0 + r)];
                }
                vbuf[r * nbk + c] = val;  // leading dim nbk (consistent with apply)
            }
        }

        MatrixView<const T, Layout::RowMajor> v_for_t{vbuf.data(), rows, nbk, nbk};
        MatrixView<T, Layout::RowMajor> vtv_view{vtv.data(), nbk, nbk, nbk};
        gemm<T, Layout::RowMajor>(T{1}, v_for_t, v_for_t, T{0}, vtv_view, Trans::Transpose, Trans::None,
                                  scratch);
        build_block_t_from_vtv<T>(vtv.data(), nbk, taup, kb, nbk, tbuf.data(), nbk);

        // Apply (I - V T V^T) to M[r0:n, 0:n].
        apply_block_reflector_left<T>(vbuf.data(), rows, nbk, tbuf.data(), mbuf.data() + r0 * n, n,
                                      wbuf.data(), wtmp.data(), scratch);

        if (kb == 0)
        {
            break;
        }
        kb -= nb;
    }

    // VT = M^T (= P^T).
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            vt[i * n + j] = mbuf[j * n + i];
        }
    }
}

} // namespace crd::hesap::dense::detail
