#pragma once

// crd-hesap-interp v13-e — scattered N-D interpolation: radial basis functions + Shepard/IDW.
//
//   RbfInterpolant — s(x) = Σ wᵢ φ(ε‖x−xᵢ‖) + Σ c_q p_q(x). The augmented system [Φ P; Pᵀ 0][w;c]=[f;0] is solved over
//     the shipped dense least-squares (SANITY 8). Kernels: Gaussian / InverseMultiquadric / Multiquadric /
//     ThinPlateSpline / Cubic (polyharmonic) / Wendland (★ compact-support, strictly PD in d≤3). The polynomial tail
//     (degree 0/1/2) makes the conditionally-PD kernels well-posed + reproduces low-degree polynomials. Matches
//     scipy.interpolate.RBFInterpolator conventions (kernel formulas, ε on scale-variant kernels only, default degree).
//   ShepardInterpolant — inverse-distance weighting (no solve); bounded by the data (no overshoot).
//
// The interpolant is UNIQUE given (kernel, ε, degree, data), so the eval matches scipy regardless of the RBF sign
// (absorbed by w). Gated by the interpolation property s(xᵢ)=fᵢ + polynomial reproduction + scipy cross-check.
// Build allocates once (the dense solve); eval is allocation-free, noexcept. Kernels use the deterministic crd::math.

#include <crd/hesap/interp/piecewise.hpp>

#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::interp
{

enum class RbfKernel : crd::u8
{
    Gaussian,             // exp(−(εr)²)            strictly PD
    InverseMultiquadric,  // 1/√(1+(εr)²)           strictly PD
    Multiquadric,         // −√(1+(εr)²)            conditionally PD (degree ≥ 0)
    ThinPlateSpline,      // r²·log r               conditionally PD (degree ≥ 1), scale-invariant
    Cubic,                // r³ (polyharmonic)      conditionally PD (degree ≥ 1), scale-invariant
    Wendland,             // (1−εr)₊⁴·(4εr+1)       ★ strictly PD (d≤3), COMPACT support
};

// scipy's default polynomial degree per kernel.
[[nodiscard]] inline crd::usize rbf_default_degree(RbfKernel k) noexcept
{
    switch (k)
    {
    case RbfKernel::ThinPlateSpline:
    case RbfKernel::Cubic:
        return 1;
    default:
        return 0; // Gaussian / IMQ / Multiquadric / Wendland
    }
}

namespace detail
{
template <Real T>
[[nodiscard]] T rbf_phi(RbfKernel k, T r, T eps) noexcept
{
    switch (k)
    {
    case RbfKernel::Gaussian:
    {
        const T er = eps * r;
        return crd::math::exp(-er * er);
    }
    case RbfKernel::InverseMultiquadric:
    {
        const T er = eps * r;
        return static_cast<T>(1) / crd::math::sqrt(static_cast<T>(1) + er * er);
    }
    case RbfKernel::Multiquadric:
    {
        const T er = eps * r;
        return -crd::math::sqrt(static_cast<T>(1) + er * er);
    }
    case RbfKernel::ThinPlateSpline:
        return (r > static_cast<T>(0)) ? r * r * crd::math::log(r) : static_cast<T>(0);
    case RbfKernel::Cubic:
        return r * r * r;
    case RbfKernel::Wendland:
    {
        const T er = eps * r;
        if (er >= static_cast<T>(1))
        {
            return static_cast<T>(0); // compact support
        }
        const T u = static_cast<T>(1) - er;
        const T u2 = u * u;
        return u2 * u2 * (static_cast<T>(4) * er + static_cast<T>(1));
    }
    }
    return static_cast<T>(0);
}

// Kernel evaluated from the SQUARED distance r². Gaussian/IMQ/Multiquadric are intrinsically functions of r² ⇒ no
// sqrt at all (gaussian) or a single sqrt instead of two (IMQ/MQ) — faster AND more accurate (no sqrt-then-square
// double rounding). The r-based kernels (tps/cubic/Wendland) take the one necessary sqrt.
template <Real T>
[[nodiscard]] T rbf_phi_from_sq(RbfKernel k, T r2, T eps) noexcept
{
    switch (k)
    {
    case RbfKernel::Gaussian:
        return crd::math::exp(-eps * eps * r2);
    case RbfKernel::InverseMultiquadric:
        return static_cast<T>(1) / crd::math::sqrt(static_cast<T>(1) + eps * eps * r2);
    case RbfKernel::Multiquadric:
        return -crd::math::sqrt(static_cast<T>(1) + eps * eps * r2);
    default:
        return rbf_phi<T>(k, crd::math::sqrt(r2), eps); // tps / cubic / Wendland need r
    }
}

[[nodiscard]] inline crd::usize poly_count(crd::usize dim, crd::usize degree) noexcept
{
    if (degree == 0)
    {
        return 1;
    }
    if (degree == 1)
    {
        return 1 + dim;
    }
    return 1 + dim + dim * (dim + 1) / 2; // degree 2
}

template <Real T>
[[nodiscard]] T sq_dist(crd::containers::ConstSpan<T> a, crd::usize ai, crd::containers::ConstSpan<T> b, crd::usize bi,
                        crd::usize dim) noexcept
{
    T s = static_cast<T>(0);
    for (crd::usize k = 0; k < dim; ++k)
    {
        const T d = a[ai * dim + k] - b[bi * dim + k];
        s += d * d;
    }
    return s;
}

// Monomial basis of total degree ≤ `degree` at `p` (length dim) into `out` (length poly_count).
template <Real T>
void poly_basis(const T* p, crd::usize dim, crd::usize degree, T* out) noexcept
{
    out[0] = static_cast<T>(1);
    if (degree >= 1)
    {
        for (crd::usize k = 0; k < dim; ++k)
        {
            out[1 + k] = p[k];
        }
    }
    if (degree >= 2)
    {
        crd::usize idx = 1 + dim;
        for (crd::usize i = 0; i < dim; ++i)
        {
            for (crd::usize j = i; j < dim; ++j)
            {
                out[idx++] = p[i] * p[j];
            }
        }
    }
}
} // namespace detail

template <Real T>
class RbfInterpolant
{
public:
    explicit RbfInterpolant(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_w(alloc), m_c(alloc) {}

    // points: n×dim row-major; values: n. epsilon ignored for scale-invariant kernels.
    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> points, crd::containers::ConstSpan<T> values,
                                     crd::usize n, crd::usize dim, RbfKernel kernel, T epsilon, crd::usize degree)
    {
        if (n < 1 || dim < 1 || points.size() != n * dim || values.size() != n || degree > 2)
        {
            return InterpStatus::BadInput;
        }
        for (crd::usize i = 0; i < n * dim; ++i)
        {
            if (!detail::is_finite(points[i]))
            {
                return InterpStatus::BadInput;
            }
        }
        const crd::usize q = detail::poly_count(dim, degree);
        const crd::usize sz = n + q;
        m_n = n;
        m_dim = dim;
        m_kernel = kernel;
        m_epsilon = epsilon;
        m_degree = degree;
        m_points = points;
        crd::hesap::dense::LDLT<T> ldlt(m_alloc, sz);      // symmetric indefinite saddle-point ⇒ Bunch-Kaufman LDLᵀ
        crd::hesap::dense::Matrix<T>& mat = ldlt.packed(); // zero-init; fill the LOWER triangle of [Φ P; Pᵀ 0]
        crd::containers::Array<T> rhsv(m_alloc);
        crd::containers::Array<T> mono(m_alloc);
        rhsv.resize(sz);
        mono.resize(q);
        for (crd::usize i = 0; i < n; ++i) // Φ block (lower triangle)
        {
            mat.at(i, i) = detail::rbf_phi_from_sq<T>(kernel, static_cast<T>(0), epsilon);
            for (crd::usize j = 0; j < i; ++j)
            {
                mat.at(i, j) =
                    detail::rbf_phi_from_sq<T>(kernel, detail::sq_dist<T>(points, i, points, j, dim), epsilon);
            }
        }
        for (crd::usize i = 0; i < n; ++i) // Pᵀ block (rows n+p, cols i<n) — lower triangle
        {
            detail::poly_basis<T>(&points[i * dim], dim, degree, mono.data());
            for (crd::usize p = 0; p < q; ++p)
            {
                mat.at(n + p, i) = mono[p];
            }
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            rhsv[i] = values[i];
        }
        for (crd::usize i = n; i < sz; ++i)
        {
            rhsv[i] = static_cast<T>(0);
        }
        crd::hesap::dense::factor_ldlt(ldlt);
        crd::hesap::dense::solve_ldlt(ldlt, crd::containers::Span<T>{rhsv.data(), sz}); // rhsv: b → solution
        m_w.resize(n);
        m_c.resize(q);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_w[i] = rhsv[i];
        }
        for (crd::usize p = 0; p < q; ++p)
        {
            m_c[p] = rhsv[n + p];
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval(crd::containers::ConstSpan<T> x) const noexcept
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            T d2 = static_cast<T>(0);
            for (crd::usize k = 0; k < m_dim; ++k)
            {
                const T d = x[k] - m_points[i * m_dim + k];
                d2 += d * d;
            }
            s += m_w[i] * detail::rbf_phi_from_sq<T>(m_kernel, d2, m_epsilon);
        }
        s += m_c[0]; // polynomial tail (inline, allocation-free)
        if (m_degree >= 1)
        {
            for (crd::usize k = 0; k < m_dim; ++k)
            {
                s += m_c[1 + k] * x[k];
            }
        }
        if (m_degree >= 2)
        {
            crd::usize idx = 1 + m_dim;
            for (crd::usize i = 0; i < m_dim; ++i)
            {
                for (crd::usize j = i; j < m_dim; ++j)
                {
                    s += m_c[idx++] * x[i] * x[j];
                }
            }
        }
        return s;
    }

private:
    crd::memory::IAllocator* m_alloc;
    crd::containers::ConstSpan<T> m_points{};
    crd::containers::Array<T> m_w;
    crd::containers::Array<T> m_c;
    crd::usize m_n = 0;
    crd::usize m_dim = 0;
    RbfKernel m_kernel = RbfKernel::Gaussian;
    T m_epsilon = static_cast<T>(1);
    crd::usize m_degree = 0;
};

// Shepard / inverse-distance weighting: s(x) = Σ wᵢ(x)·fᵢ / Σ wᵢ(x), wᵢ = 1/‖x−xᵢ‖^power. Bounded by the data
// (no overshoot); cusps at the nodes. No solve.
template <Real T>
class ShepardInterpolant
{
public:
    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> points, crd::containers::ConstSpan<T> values,
                                     crd::usize n, crd::usize dim, T power = static_cast<T>(2)) noexcept
    {
        if (n < 1 || dim < 1 || points.size() != n * dim || values.size() != n)
        {
            return InterpStatus::BadInput;
        }
        m_points = points;
        m_values = values;
        m_n = n;
        m_dim = dim;
        m_power = power;
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval(crd::containers::ConstSpan<T> x) const noexcept
    {
        T num = static_cast<T>(0);
        T den = static_cast<T>(0);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            T d2 = static_cast<T>(0);
            for (crd::usize k = 0; k < m_dim; ++k)
            {
                const T d = x[k] - m_points[i * m_dim + k];
                d2 += d * d;
            }
            if (d2 == static_cast<T>(0)) // exact at a node
            {
                return m_values[i];
            }
            const T w = static_cast<T>(1) / crd::math::pow(d2, m_power / static_cast<T>(2));
            num += w * m_values[i];
            den += w;
        }
        return num / den;
    }

private:
    crd::containers::ConstSpan<T> m_points{};
    crd::containers::ConstSpan<T> m_values{};
    crd::usize m_n = 0;
    crd::usize m_dim = 0;
    T m_power = static_cast<T>(2);
};

} // namespace crd::hesap::interp
