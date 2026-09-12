#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-a — state-space (A, B, C, D) representation, SISO.
//
//   x[n+1] = A x[n] + B u[n]
//   y[n]   = C x[n] + D u[n]
//
// tf -> ss uses the CONTROLLABLE CANONICAL form; ss -> tf uses Faddeev-LeVerrier
// (the characteristic polynomial + adjugate in one O(n^4) recursion — no
// root-finding, well-conditioned for the moderate orders DSP uses). Round-trips
// tf -> ss -> tf to the original (b, a). MATLAB tf2ss / ss2tf. f32/f64.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dsp/filter.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dsp
{

// SISO state-space: A (n x n), B (n), C (n), D scalar. Stored row-major flat.
template <typename T> struct StateSpace
{
    crd::usize n = 0;
    crd::containers::Array<T> A; // n*n row-major
    crd::containers::Array<T> B; // n
    crd::containers::Array<T> C; // n
    T D = T(0);
    explicit StateSpace(crd::memory::IAllocator* alloc) : A(alloc), B(alloc), C(alloc) {}
    [[nodiscard]] T& a(crd::usize i, crd::usize j) noexcept { return A[i * n + j]; }
    [[nodiscard]] const T& a(crd::usize i, crd::usize j) const noexcept { return A[i * n + j]; }
};

// tf -> ss : controllable canonical form. b is padded to the denominator length.
template <typename T>
[[nodiscard]] StateSpace<T> tf_to_ss(crd::memory::IAllocator* alloc, const TransferFunction<T>& tf)
{
    StateSpace<T> ss(alloc);
    const crd::usize na = tf.a.size();
    if (na <= 1)
    {
        ss.n = 0;
        ss.D = tf.b.empty() ? T(0) : tf.b[0];
        return ss;
    }
    const T a0 = tf.a[0];
    const crd::usize n = na - 1;
    ss.n = n;
    ss.A.resize(n * n);
    ss.B.resize(n);
    ss.C.resize(n);
    // normalized denominator (monic) + padded numerator, both length n+1.
    crd::containers::Array<T> an(alloc), bn(alloc);
    an.resize(n + 1);
    bn.resize(n + 1);
    for (crd::usize i = 0; i <= n; ++i)
    {
        an[i] = tf.a[i] / a0;
        bn[i] = (i < tf.b.size()) ? tf.b[i] / a0 : T(0);
    }
    for (crd::usize i = 0; i < n * n; ++i)
    {
        ss.A[i] = T(0);
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        ss.a(0, j) = -an[j + 1]; // top row = -a1..-an
    }
    for (crd::usize i = 1; i < n; ++i)
    {
        ss.a(i, i - 1) = T(1); // sub-diagonal ones
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        ss.B[i] = (i == 0) ? T(1) : T(0);
        ss.C[i] = bn[i + 1] - bn[0] * an[i + 1]; // b_k - b0 a_k
    }
    ss.D = bn[0];
    return ss;
}

// ss -> tf : Faddeev-LeVerrier. charpoly p(z)=det(zI-A)=z^n+p1 z^{n-1}+...; with
// adj(zI-A)=sum_k M_k z^{n-k}, num(z)=sum_k (C M_k B) z^{n-k}; H = (num + D p)/p.
// In the filter convention (divide by z^n): a=[1,p1,..,pn], b[0]=D, b[k]=C M_k B + D p_k.
template <typename T>
[[nodiscard]] TransferFunction<T> ss_to_tf(crd::memory::IAllocator* alloc, const StateSpace<T>& ss)
{
    TransferFunction<T> tf(alloc);
    const crd::usize n = ss.n;
    if (n == 0)
    {
        tf.b.push_back(ss.D);
        tf.a.push_back(T(1));
        return tf;
    }
    tf.a.resize(n + 1);
    tf.b.resize(n + 1);
    tf.a[0] = T(1);
    tf.b[0] = ss.D;

    crd::containers::Array<T> M(alloc), AM(alloc); // M_k and A*M_k, n*n
    M.resize(n * n);
    AM.resize(n * n);
    for (crd::usize i = 0; i < n * n; ++i)
    {
        M[i] = T(0);
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        M[i * n + i] = T(1); // M_1 = I
    }
    for (crd::usize k = 1; k <= n; ++k)
    {
        // C * M_k * B  (numerator coefficient)
        T cmb = T(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            T mb = T(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                mb += M[i * n + j] * ss.B[j];
            }
            cmb += ss.C[i] * mb;
        }
        // AM = A * M_k
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                T s = T(0);
                for (crd::usize l = 0; l < n; ++l)
                {
                    s += ss.a(i, l) * M[l * n + j];
                }
                AM[i * n + j] = s;
            }
        }
        // p_k = -trace(AM)/k
        T tr = T(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            tr += AM[i * n + i];
        }
        const T pk = -tr / static_cast<T>(k);
        tf.a[k] = pk;
        tf.b[k] = cmb + ss.D * pk;
        // M_{k+1} = AM + p_k I
        for (crd::usize i = 0; i < n * n; ++i)
        {
            M[i] = AM[i];
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            M[i * n + i] += pk;
        }
    }
    return tf;
}

} // namespace crd::hesap::dsp
