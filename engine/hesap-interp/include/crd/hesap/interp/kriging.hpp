#pragma once

// crd-hesap-interp v13-f — Gaussian-process regression / kriging: the scattered interpolant that returns a MEAN and a
// PREDICTIVE VARIANCE. ★ the safety-critical sensor-fusion piece — a satellite/drone/robot fusing noisy measurements
// needs not just an estimate but a calibrated uncertainty on it.
//
//   GaussianProcessInterpolant — squared-exponential (RBF) kernel k(x,x') = exp(−‖x−x'‖²/(2ℓ²)). Fit: K = k(X,X)+αI
//     (α = observation noise), Cholesky factor (the shipped dense SPD solver — SANITY 8), weights α_w = K⁻¹y. Predict
//     mean μ(x) = Σ α_w,i k(x,xᵢ); predict variance σ²(x) = k(x,x) − k*ᵀK⁻¹k*. Matches sklearn GaussianProcessRegressor
//     (RBF kernel, normalize_y=False). Deterministic; the kernel uses the deterministic crd::math exp.

#include <crd/hesap/interp/piecewise.hpp>

#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::interp
{

template <Real T>
class GaussianProcessInterpolant
{
public:
    explicit GaussianProcessInterpolant(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_alpha(alloc), m_chol(alloc), m_kw(alloc)
    {
    }

    // points: n×dim row-major; values: n. length_scale ℓ > 0; noise α ≥ 0 (the diagonal regularizer / observation σ²).
    [[nodiscard]] InterpStatus fit(crd::containers::ConstSpan<T> points, crd::containers::ConstSpan<T> values,
                                   crd::usize n, crd::usize dim, T length_scale, T noise)
    {
        if (n < 1 || dim < 1 || points.size() != n * dim || values.size() != n || !(length_scale > static_cast<T>(0)))
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
        m_points = points;
        m_n = n;
        m_dim = dim;
        m_inv_2l2 = static_cast<T>(1) / (static_cast<T>(2) * length_scale * length_scale);
        m_chol = crd::hesap::dense::Cholesky<T>(m_alloc, n);
        crd::hesap::dense::Matrix<T>& bigk = m_chol.packed(); // fill the LOWER triangle of K = k(X,X) + αI
        for (crd::usize i = 0; i < n; ++i)
        {
            bigk.at(i, i) = static_cast<T>(1) + noise; // k(xᵢ,xᵢ)=1
            for (crd::usize j = 0; j < i; ++j)
            {
                bigk.at(i, j) = kernel_sq(sq_dist(points, i, points, j, dim));
            }
        }
        crd::hesap::dense::factor_cholesky(m_chol);
        if (m_chol.is_singular())
        {
            return InterpStatus::BadInput; // K not PD (duplicate points + zero noise)
        }
        m_alpha.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_alpha[i] = values[i];
        }
        crd::hesap::dense::solve_cholesky(m_chol, crd::containers::Span<T>{m_alpha.data(), n}); // α_w = K⁻¹ y
        m_kw.resize(n);
        return InterpStatus::Ok;
    }

    // Posterior mean only (O(n), allocation-free).
    [[nodiscard]] T mean(crd::containers::ConstSpan<T> query) const noexcept
    {
        T mu = static_cast<T>(0);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            mu += m_alpha[i] * kernel_sq(sq_dist_query(query, i));
        }
        return mu;
    }

    // Posterior mean + variance. variance = k(x,x) − k*ᵀK⁻¹k* = 1 − ‖L⁻¹k*‖²: only the FORWARD triangular solve is
    // needed (the back-substitution of a full solve is wasted work for the variance norm — what sklearn also does).
    void predict(crd::containers::ConstSpan<T> query, T& out_mean, T& out_var) const noexcept
    {
        T mu = static_cast<T>(0);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            const T ks = kernel_sq(sq_dist_query(query, i));
            m_kw[i] = ks;
            mu += m_alpha[i] * ks;
        }
        out_mean = mu;
        const crd::hesap::dense::Matrix<T>& bigl = m_chol.packed(); // L (lower, explicit diagonal)
        for (crd::usize i = 0; i < m_n; ++i)                        // forward substitution L y = k* (in-place)
        {
            T s = m_kw[i];
            for (crd::usize j = 0; j < i; ++j)
            {
                s -= bigl.at(i, j) * m_kw[j];
            }
            m_kw[i] = s / bigl.at(i, i);
        }
        T quad = static_cast<T>(0);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            quad += m_kw[i] * m_kw[i];
        }
        const T var = static_cast<T>(1) - quad;
        out_var = var > static_cast<T>(0) ? var : static_cast<T>(0); // clamp round-off negatives, as sklearn does
    }

private:
    [[nodiscard]] T kernel_sq(T d2) const noexcept { return crd::math::exp(-d2 * m_inv_2l2); }

    [[nodiscard]] static T sq_dist(crd::containers::ConstSpan<T> a, crd::usize ai, crd::containers::ConstSpan<T> b,
                                   crd::usize bi, crd::usize dim) noexcept
    {
        T s = static_cast<T>(0);
        for (crd::usize k = 0; k < dim; ++k)
        {
            const T d = a[ai * dim + k] - b[bi * dim + k];
            s += d * d;
        }
        return s;
    }

    [[nodiscard]] T sq_dist_query(crd::containers::ConstSpan<T> q, crd::usize i) const noexcept
    {
        T s = static_cast<T>(0);
        for (crd::usize k = 0; k < m_dim; ++k)
        {
            const T d = q[k] - m_points[i * m_dim + k];
            s += d * d;
        }
        return s;
    }

    crd::memory::IAllocator* m_alloc;
    crd::containers::ConstSpan<T> m_points{};
    crd::containers::Array<T> m_alpha;        // K⁻¹ y
    crd::hesap::dense::Cholesky<T> m_chol; // L : K = L Lᵀ
    mutable crd::containers::Array<T> m_kw; // k* → L⁻¹ k* scratch (predict)
    crd::usize m_n = 0;
    crd::usize m_dim = 0;
    T m_inv_2l2 = static_cast<T>(0.5);
};

} // namespace crd::hesap::interp
