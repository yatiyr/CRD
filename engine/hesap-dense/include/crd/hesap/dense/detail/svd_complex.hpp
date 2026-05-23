#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b-1c — complex SVD substrate (zgesvd-class). A complex
// Golub-Kahan bidiagonalization reduces A (complex m x n) to a REAL bidiagonal
// B with complex unitary Q, P (A = Q B P^H); the real (d,e) then feed the
// already-shipped real dlasd0/dbdsqr (the D&C crush), and a complex back-
// transform lifts the singular vectors. This header carries the complex
// primitives; the driver lives in svd.cpp.
//
// Lower layer: raw complex f32/f64 (ADR-0078). C = Complex<T>; beta is REAL.
//
// References (build/win-vs-ref/_deps/openblas-src/lapack-netlib/SRC/):
//   zlarfg.f  zgebd2.f  zungbr.f
// -----------------------------------------------------------------------

template <typename C>
struct ComplexHouseholder
{
    C tau;                  // complex scalar; tau == 0 means H == I.
    RealType<C> beta;       // REAL: H*x places beta (real) in position 0.
};

// dlapy3 — overflow-safe sqrt(x^2 + y^2 + z^2).
template <typename T>
[[nodiscard]] inline T hypot3(T x, T y, T z) noexcept
{
    const T ax = std::abs(x);
    const T ay = std::abs(y);
    const T az = std::abs(z);
    const T w = std::max(ax, std::max(ay, az));
    if (w == T{0})
    {
        return T{0};
    }
    const T rx = ax / w;
    const T ry = ay / w;
    const T rz = az / w;
    return w * std::sqrt(rx * rx + ry * ry + rz * rz);
}

// make_complex_householder — faithful port of LAPACK zlarfg. Generates a
// complex elementary reflector  H = I - tau * v * v^H,  v[0] = 1 (implicit),
// such that  H^H * x = beta * e_0  with beta REAL. `x` is contiguous length n
// (x[0] = alpha, x[1..n-1] = the tail to annihilate). On exit x[1..n-1] holds
// the reflector tail (v[0] = 1 NOT written); x[0] is left unchanged (caller
// stores beta into the real bidiagonal). Returns {tau (complex), beta (real)}.
template <typename C>
[[nodiscard]] inline ComplexHouseholder<C> make_complex_householder(C* x, crd::usize n) noexcept
{
    using R = RealType<C>;
    if (n == 0)
    {
        return ComplexHouseholder<C>{C{R{0}, R{0}}, R{0}};
    }
    const C alpha = x[0];
    const R alphr = real(alpha);
    const R alphi = imag(alpha);

    R xnorm_sq = R{0};
    for (crd::usize i = 1; i < n; ++i)
    {
        xnorm_sq += norm_sq(x[i]);
    }
    const R xnorm = std::sqrt(xnorm_sq);

    if (xnorm == R{0} && alphi == R{0})
    {
        // H = I (already a multiple of e_0); beta = alpha (real).
        return ComplexHouseholder<C>{C{R{0}, R{0}}, alphr};
    }

    R beta = -(alphr >= R{0} ? R{1} : R{-1}) * hypot3(alphr, alphi, xnorm);
    // tau = (beta - alpha) / beta  (complex): real part (beta-alphr)/beta,
    // imaginary part -alphi/beta.
    const C tau{(beta - alphr) / beta, -alphi / beta};
    // v = x / (alpha - beta) for the tail; v[0] = 1 implicit.
    const C inv = C{R{1}, R{0}} / (alpha - C{beta, R{0}});
    for (crd::usize i = 1; i < n; ++i)
    {
        x[i] = x[i] * inv;
    }
    return ComplexHouseholder<C>{tau, beta};
}

// =======================================================================
// bidiagonalize_complex — complex Golub-Kahan reduction (zgebd2-faithful,
// m >= n, UPPER bidiagonal). A (complex m x n, RowMajor ld) -> REAL bidiagonal
// (d, e) + complex reflectors stored in A + complex tauq/taup. The first
// `ZLACGV` (conjugate the row) is what forces e[i] REAL; the SECOND ZLACGV
// (un-conjugate) is DELIBERATELY SKIPPED (D(svd)-16) so the stored right-
// reflector tail a(i,i+2:n) is the actual w-tail that form_p_complex reads
// directly — internally consistent (not LAPACK-storage-compatible, which we
// don't need). Left reflectors: tauq[i] + v in column i (rows i+1:m); right:
// taup[i] + w in row i (cols i+2:n); v[0]/w[0] = 1 implicit.
// =======================================================================
template <typename C>
inline void bidiagonalize_complex(C* a, crd::usize m, crd::usize n, crd::usize lda, RealType<C>* d,
                                  RealType<C>* e, C* tauq, C* taup, crd::memory::IAllocator* scratch) noexcept
{
    using R = RealType<C>;
    if (n == 0 || m == 0)
    {
        return;
    }
    crd::containers::Array<C> colbuf(scratch);
    colbuf.resize(m);

    for (crd::usize i = 0; i < n; ++i)
    {
        // --- Left reflector H(i): annihilate a(i+1:m, i) (strided column). ---
        const crd::usize collen = m - i;
        for (crd::usize k = 0; k < collen; ++k)
        {
            colbuf[k] = a[(i + k) * lda + i];
        }
        const auto h = make_complex_householder<C>(colbuf.data(), collen);
        tauq[i] = h.tau;
        d[i] = h.beta;
        for (crd::usize k = 1; k < collen; ++k)
        {
            a[(i + k) * lda + i] = colbuf[k];  // v tail
        }
        // Apply H(i)^H = I - conj(tau) v v^H to a(i:m, i+1:n). v[0]=1 at row i.
        if (i + 1 < n)
        {
            for (crd::usize cc = i + 1; cc < n; ++cc)
            {
                C vhc = a[i * lda + cc];  // conj(v[i]=1) * a(i,cc)
                for (crd::usize k = i + 1; k < m; ++k)
                {
                    vhc += conj(a[k * lda + i]) * a[k * lda + cc];
                }
                vhc = conj(h.tau) * vhc;
                a[i * lda + cc] -= vhc;
                for (crd::usize k = i + 1; k < m; ++k)
                {
                    a[k * lda + cc] -= a[k * lda + i] * vhc;
                }
            }

            // --- Right reflector G(i): annihilate a(i, i+2:n) (contiguous row). ---
            const crd::usize rowlen = n - (i + 1);
            for (crd::usize cc = i + 1; cc < n; ++cc)  // ZLACGV: conjugate the row
            {
                a[i * lda + cc] = conj(a[i * lda + cc]);
            }
            const auto h2 = make_complex_householder<C>(&a[i * lda + (i + 1)], rowlen);
            taup[i] = h2.tau;
            e[i] = h2.beta;
            a[i * lda + (i + 1)] = C{R{1}, R{0}};  // w[0]=1
            // Apply G(i) = I - tau w w^H from the RIGHT to a(i+1:m, i+1:n):
            //   C := C - tau (C w) w^H.  w = a(i, i+1:n).
            for (crd::usize r = i + 1; r < m; ++r)
            {
                C cw = C{R{0}, R{0}};
                for (crd::usize cc = i + 1; cc < n; ++cc)
                {
                    cw += a[r * lda + cc] * a[i * lda + cc];  // (C w)_r
                }
                cw = h2.tau * cw;
                for (crd::usize cc = i + 1; cc < n; ++cc)
                {
                    a[r * lda + cc] -= cw * conj(a[i * lda + cc]);
                }
            }
            // (D(svd)-16) skip the un-conjugate; a(i,i+2:n) keeps the w tail.
            a[i * lda + (i + 1)] = C{e[i], R{0}};  // real super-diagonal slot
        }
        else
        {
            taup[i] = C{R{0}, R{0}};
        }
    }
}

// form_q_complex — build U_init = Q (m x n) = H(0) H(1) ... H(n-1) applied to
// [I_n; 0]. Apply H(i) (NOT H^H) in reverse: Q := H(i) Q. v[0]=1 at row i,
// tail a(i+1:m, i).
template <typename C>
inline void form_q_complex(const C* a, crd::usize m, crd::usize n, crd::usize lda, const C* tauq, C* q) noexcept
{
    using R = RealType<C>;
    for (crd::usize i = 0; i < m * n; ++i)
    {
        q[i] = C{R{0}, R{0}};
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        q[j * n + j] = C{R{1}, R{0}};
    }
    for (crd::usize ii = n; ii-- > 0;)
    {
        const C tau = tauq[ii];
        if (real(tau) == R{0} && imag(tau) == R{0})
        {
            continue;
        }
        for (crd::usize c = 0; c < n; ++c)
        {
            C vhq = q[ii * n + c];  // conj(v[ii]=1) * q(ii,c)
            for (crd::usize k = ii + 1; k < m; ++k)
            {
                vhq += conj(a[k * lda + ii]) * q[k * n + c];
            }
            vhq = tau * vhq;
            q[ii * n + c] -= vhq;
            for (crd::usize k = ii + 1; k < m; ++k)
            {
                q[k * n + c] -= a[k * lda + ii] * vhq;
            }
        }
    }
}

// form_p_complex — build P (n x n) = G(0) G(1) ... G(n-2) applied to I_n
// (V_A = P V_b). Apply G(i) in reverse: P := G(i) P. w[0]=1 at row i+1, tail
// a(i, i+2:n).
template <typename C>
inline void form_p_complex(const C* a, crd::usize n, crd::usize lda, const C* taup, C* p) noexcept
{
    using R = RealType<C>;
    for (crd::usize i = 0; i < n * n; ++i)
    {
        p[i] = C{R{0}, R{0}};
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        p[j * n + j] = C{R{1}, R{0}};
    }
    if (n < 2)
    {
        return;
    }
    for (crd::usize ii = n - 1; ii-- > 0;)  // ii = n-2 .. 0
    {
        const C tau = taup[ii];
        if (real(tau) == R{0} && imag(tau) == R{0})
        {
            continue;
        }
        for (crd::usize c = 0; c < n; ++c)
        {
            C whp = p[(ii + 1) * n + c];  // conj(w[ii+1]=1) * p(ii+1,c)
            for (crd::usize k = ii + 2; k < n; ++k)
            {
                whp += conj(a[ii * lda + k]) * p[k * n + c];
            }
            whp = tau * whp;
            p[(ii + 1) * n + c] -= whp;
            for (crd::usize k = ii + 2; k < n; ++k)
            {
                p[k * n + c] -= a[ii * lda + k] * whp;
            }
        }
    }
}

} // namespace crd::hesap::dense::detail
