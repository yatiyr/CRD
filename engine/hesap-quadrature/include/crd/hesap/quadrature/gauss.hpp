#pragma once

// crd-hesap-quadrature v12-c (Part 2) — Gauss quadrature nodes/weights via Golub-Welsch.
//
// The Golub-Welsch theorem: the Gauss nodes are the eigenvalues of the symmetric tridiagonal Jacobi matrix J of the
// orthogonal-polynomial recurrence (diagonal aₙ, off-diagonal √bₙ), and the weight wᵢ = μ₀·(first component of the
// i-th normalized eigenvector)², μ₀ = ∫ w(x) dx. This module REUSES the engine's gold-standard symmetric eigensolver
// (`crd::hesap::dense::eig_sym` — dsytrd → dqds/MRRR; see SANITY.md rule 8 "search before you build") rather than a
// bespoke QL. Recurrence coefficients come from the classical families; Γ/B from crd-hesap-special. Gated vs scipy
// roots_legendre / roots_hermite / roots_laguerre / roots_jacobi / roots_chebyt.
//
// This module is NOT a leaf (it needs the eigensolver) — which is exactly why it is separate from the leaf
// crd-hesap-special: gamma/erf/bessel consumers must not transitively link the dense-LA stack.

#include <crd/hesap/dense/eig_sym.hpp>   // eig_sym, Symmetric, EigSym, Matrix, Vector
#include <crd/hesap/special/gamma.hpp>   // gamma, beta (μ₀ for Jacobi/Laguerre)

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::quadrature
{
// Golub-Welsch core. a[0..n-1] = recurrence diagonal; b[1..n-1] = recurrence off-diagonal SQUARED (b[0] ignored);
// mu0 = zeroth moment ∫w. Fills nodes (ascending) + weights. Reuses crd::hesap::dense::eig_sym.
template <typename T>
void golub_welsch(crd::memory::IAllocator* alloc, int n, const T* a, const T* b, T mu0, T* nodes, T* weights)
{
    if (n == 1)
    {
        nodes[0] = a[0];
        weights[0] = mu0;
        return;
    }
    crd::hesap::dense::Symmetric<T> jacobi(alloc, static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        jacobi.at(static_cast<crd::usize>(i), static_cast<crd::usize>(i)) = a[i];
    }
    for (int i = 1; i < n; ++i)
    {
        jacobi.at(static_cast<crd::usize>(i), static_cast<crd::usize>(i - 1)) = crd::math::sqrt(b[i]); // √βᵢ off-diagonal
    }
    const crd::hesap::dense::EigSym<T> eig = crd::hesap::dense::eig_sym(alloc, jacobi); // values ascending
    for (int k = 0; k < n; ++k)
    {
        nodes[k] = eig.values(static_cast<crd::usize>(k));
        const T v0 = eig.vectors.at(0, static_cast<crd::usize>(k)); // first component of eigenvector k
        weights[k] = mu0 * v0 * v0;
    }
}

// Gauss-Legendre on [−1,1], weight 1. aₙ=0, bₙ=n²/(4n²−1), μ₀=2.
template <typename T>
void gauss_legendre(crd::memory::IAllocator* alloc, int n, T* nodes, T* weights)
{
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> b(alloc);
    a.resize(static_cast<crd::usize>(n));
    b.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        a[static_cast<crd::usize>(i)] = T{0};
    }
    for (int i = 1; i < n; ++i)
    {
        const T ii = static_cast<T>(i);
        b[static_cast<crd::usize>(i)] = ii * ii / (T{4} * ii * ii - T{1});
    }
    golub_welsch<T>(alloc, n, a.data(), b.data(), T{2}, nodes, weights);
}

// Gauss-Hermite (physicist), weight e^{−x²} on ℝ. aₙ=0, bₙ=n/2, μ₀=√π.
template <typename T>
void gauss_hermite(crd::memory::IAllocator* alloc, int n, T* nodes, T* weights)
{
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> b(alloc);
    a.resize(static_cast<crd::usize>(n));
    b.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        a[static_cast<crd::usize>(i)] = T{0};
    }
    for (int i = 1; i < n; ++i)
    {
        b[static_cast<crd::usize>(i)] = static_cast<T>(i) / T{2};
    }
    golub_welsch<T>(alloc, n, a.data(), b.data(), static_cast<T>(1.7724538509055160272981674833411452), nodes,
                    weights); // √π
}

// Generalized Gauss-Laguerre, weight x^α e^{−x} on [0,∞). aₙ=2n+α+1, bₙ=n(n+α), μ₀=Γ(α+1). (α=0 ⇒ ordinary.)
template <typename T>
void gauss_laguerre(crd::memory::IAllocator* alloc, int n, T alpha, T* nodes, T* weights)
{
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> b(alloc);
    a.resize(static_cast<crd::usize>(n));
    b.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        a[static_cast<crd::usize>(i)] = static_cast<T>(2 * i + 1) + alpha;
    }
    for (int i = 1; i < n; ++i)
    {
        b[static_cast<crd::usize>(i)] = static_cast<T>(i) * (static_cast<T>(i) + alpha);
    }
    golub_welsch<T>(alloc, n, a.data(), b.data(), crd::hesap::special::gamma(alpha + T{1}), nodes, weights);
}

// Gauss-Jacobi, weight (1−x)^α (1+x)^β on [−1,1]. Monic recurrence (DLMF 18.9) + μ₀ = 2^{α+β+1} B(α+1,β+1).
template <typename T>
void gauss_jacobi(crd::memory::IAllocator* alloc, int n, T alpha, T beta, T* nodes, T* weights)
{
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> b(alloc);
    a.resize(static_cast<crd::usize>(n));
    b.resize(static_cast<crd::usize>(n));
    const T ab = alpha + beta;
    a[0] = (beta - alpha) / (ab + T{2});
    for (int i = 1; i < n; ++i)
    {
        const T ii = static_cast<T>(i);
        const T d = T{2} * ii + ab;
        a[static_cast<crd::usize>(i)] = (beta * beta - alpha * alpha) / (d * (d + T{2}));
        b[static_cast<crd::usize>(i)] = T{4} * ii * (ii + alpha) * (ii + beta) * (ii + ab) /
                                        (d * d * (d + T{1}) * (d - T{1}));
    }
    const T mu0 = crd::math::pow(T{2}, ab + T{1}) * crd::hesap::special::beta(alpha + T{1}, beta + T{1});
    golub_welsch<T>(alloc, n, a.data(), b.data(), mu0, nodes, weights);
}

// Gauss-Gegenbauer, weight (1−x²)^{λ−½} = Jacobi with α=β=λ−½.
template <typename T>
void gauss_gegenbauer(crd::memory::IAllocator* alloc, int n, T lambda, T* nodes, T* weights)
{
    gauss_jacobi<T>(alloc, n, lambda - static_cast<T>(0.5), lambda - static_cast<T>(0.5), nodes, weights);
}

// Gauss-Chebyshev 1st kind, weight 1/√(1−x²): closed form xᵢ=cos((2i−1)π/2n) (ascending), wᵢ=π/n.
template <typename T>
void gauss_chebyshev_t(int n, T* nodes, T* weights)
{
    const T pi = static_cast<T>(3.14159265358979323846264338327950288);
    for (int i = 0; i < n; ++i)
    {
        nodes[i] = -crd::math::cos(static_cast<T>(2 * i + 1) * pi / static_cast<T>(2 * n)); // ascending
        weights[i] = pi / static_cast<T>(n);
    }
}

} // namespace crd::hesap::quadrature
