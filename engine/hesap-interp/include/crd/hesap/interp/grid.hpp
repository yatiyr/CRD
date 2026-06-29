#pragma once

// crd-hesap-interp v13-f — gridded N-D interpolation on a regular (uniform-per-axis) grid.
//
//   RegularGridInterpolant — values on a Cartesian product of uniform axes (origin/spacing/count per axis), stored
//     flat row-major. eval_linear is N-linear (bilinear/trilinear/…): the 2^d-corner multilinear blend, allocation-free
//     and deterministic. Reproduces any multilinear function exactly; matches scipy.RegularGridInterpolator('linear')
//     and MATLAB interpn('linear') bit-for-bit. (Cubic / tensor-B-spline land in the next v13-f part.)
//
// dim ≤ 8. Out-of-range queries clamp to the edge cell (linear extrapolation), matching no peer's NaN policy by design
// — a robotics/LUT consumer wants a bounded value, never a NaN.

#include <crd/hesap/interp/piecewise.hpp>

namespace crd::hesap::interp
{

inline constexpr crd::usize k_grid_max_dim = 8;

template <Real T>
class RegularGridInterpolant
{
public:
    explicit RegularGridInterpolant(crd::memory::IAllocator* alloc) noexcept
        : m_origin(alloc), m_spacing(alloc), m_inv_spacing(alloc), m_count(alloc), m_stride(alloc), m_values(alloc)
    {
    }

    // origin/spacing/count: length dim. values: flat row-major, axis 0 outermost (size = ∏ count[d]).
    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> origin, crd::containers::ConstSpan<T> spacing,
                                     crd::containers::ConstSpan<crd::usize> count, crd::usize dim,
                                     crd::containers::ConstSpan<T> values)
    {
        if (dim < 1 || dim > k_grid_max_dim || origin.size() != dim || spacing.size() != dim || count.size() != dim)
        {
            return InterpStatus::BadInput;
        }
        crd::usize total = 1;
        for (crd::usize d = 0; d < dim; ++d)
        {
            if (count[d] < 2 || !(spacing[d] > static_cast<T>(0)) || !detail::is_finite(origin[d]) ||
                !detail::is_finite(spacing[d]))
            {
                return InterpStatus::BadInput;
            }
            total *= count[d];
        }
        if (values.size() != total)
        {
            return InterpStatus::BadInput;
        }
        m_dim = dim;
        m_origin.resize(dim);
        m_spacing.resize(dim);
        m_inv_spacing.resize(dim);
        m_count.resize(dim);
        m_stride.resize(dim);
        for (crd::usize d = 0; d < dim; ++d)
        {
            m_origin[d] = origin[d];
            m_spacing[d] = spacing[d];
            m_inv_spacing[d] = static_cast<T>(1) / spacing[d]; // precomputed reciprocal ⇒ locate multiplies, not divides
            m_count[d] = count[d];
        }
        m_stride[dim - 1] = 1; // row-major strides
        for (crd::usize d = dim - 1; d-- > 0;)
        {
            m_stride[d] = m_stride[d + 1] * count[d + 1];
        }
        m_values.resize(total);
        for (crd::usize i = 0; i < total; ++i)
        {
            m_values[i] = values[i];
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval_linear(crd::containers::ConstSpan<T> query) const noexcept
    {
        const T* CRD_RESTRICT v = m_values.data();
        if (m_dim == 2) // bilinear fast path (no corner loop) — matches Boost's specialized bilinear_uniform
        {
            crd::usize b0;
            crd::usize b1;
            T f0;
            T f1;
            locate(0, query[0], b0, f0);
            locate(1, query[1], b1, f1);
            const crd::usize s0 = m_stride[0];
            const crd::usize i0 = b0 * s0 + b1; // stride[1] == 1
            const T g0 = static_cast<T>(1) - f0;
            const T g1 = static_cast<T>(1) - f1;
            return g0 * (g1 * v[i0] + f1 * v[i0 + 1]) + f0 * (g1 * v[i0 + s0] + f1 * v[i0 + s0 + 1]);
        }
        if (m_dim == 3) // trilinear fast path
        {
            crd::usize b0;
            crd::usize b1;
            crd::usize b2;
            T f0;
            T f1;
            T f2;
            locate(0, query[0], b0, f0);
            locate(1, query[1], b1, f1);
            locate(2, query[2], b2, f2);
            const crd::usize s0 = m_stride[0];
            const crd::usize s1 = m_stride[1];
            const crd::usize i0 = b0 * s0 + b1 * s1 + b2; // stride[2] == 1
            const T g0 = static_cast<T>(1) - f0;
            const T g1 = static_cast<T>(1) - f1;
            const T g2 = static_cast<T>(1) - f2;
            const T e0 = g1 * (g2 * v[i0] + f2 * v[i0 + 1]) + f1 * (g2 * v[i0 + s1] + f2 * v[i0 + s1 + 1]);
            const T e1 =
                g1 * (g2 * v[i0 + s0] + f2 * v[i0 + s0 + 1]) + f1 * (g2 * v[i0 + s0 + s1] + f2 * v[i0 + s0 + s1 + 1]);
            return g0 * e0 + f0 * e1;
        }
        crd::usize base[k_grid_max_dim]; // general N-linear (dim ≥ 4 / dim 1): 2^d-corner blend
        T frac[k_grid_max_dim];
        for (crd::usize d = 0; d < m_dim; ++d)
        {
            locate(d, query[d], base[d], frac[d]);
        }
        T sum = static_cast<T>(0);
        const crd::usize corners = static_cast<crd::usize>(1) << m_dim;
        for (crd::usize c = 0; c < corners; ++c)
        {
            T w = static_cast<T>(1);
            crd::usize idx = 0;
            for (crd::usize d = 0; d < m_dim; ++d)
            {
                const crd::usize bit = (c >> d) & 1u;
                w *= bit ? frac[d] : (static_cast<T>(1) - frac[d]);
                idx += (base[d] + bit) * m_stride[d];
            }
            sum += w * v[idx];
        }
        return sum;
    }

    // Keys cubic-convolution interpolation (a = −0.5): bicubic / tricubic / general N-cubic. C¹, interpolating,
    // reproduces linear exactly. Matches MATLAB interpn('cubic'). The 4 taps per axis (base−1..base+2) clamp to the
    // edge (replicate). Allocation-free, deterministic.
    [[nodiscard]] T eval_cubic(crd::containers::ConstSpan<T> query) const noexcept
    {
        const T* CRD_RESTRICT v = m_values.data();
        if (m_dim == 2) // bicubic fast path: row-wise 4×4 (no base-4 digit decode)
        {
            crd::usize bx;
            crd::usize by;
            T fx;
            T fy;
            locate(0, query[0], bx, fx);
            locate(1, query[1], by, fy);
            T wx[4];
            T wy[4];
            cubic_conv_weights(fx, wx);
            cubic_conv_weights(fy, wy);
            const crd::usize s0 = m_stride[0];
            if (bx >= 1 && bx + 2 < m_count[0] && by >= 1 && by + 2 < m_count[1]) // interior: 4 taps/axis contiguous
            {
                const T* CRD_RESTRICT b = v + (bx - 1) * s0 + (by - 1);
                T sum = static_cast<T>(0);
                for (crd::usize i = 0; i < 4; ++i)
                {
                    const T* CRD_RESTRICT row = b + i * s0; // row[0..3] contiguous ⇒ the dot SLP-vectorizes
                    sum += wx[i] * (wy[0] * row[0] + wy[1] * row[1] + wy[2] * row[2] + wy[3] * row[3]);
                }
                return sum;
            }
            const crd::usize hix = m_count[0] - 1; // boundary: clamp the taps
            const crd::usize hiy = m_count[1] - 1;
            crd::usize tx[4];
            crd::usize ty[4];
            for (crd::usize k = 0; k < 4; ++k)
            {
                const crd::usize a = (bx + k >= 1) ? bx + k - 1 : 0;
                tx[k] = (a < hix ? a : hix) * s0;
                const crd::usize b = (by + k >= 1) ? by + k - 1 : 0;
                ty[k] = b < hiy ? b : hiy; // stride[1] == 1
            }
            T sum = static_cast<T>(0);
            for (crd::usize i = 0; i < 4; ++i)
            {
                const T* CRD_RESTRICT row = v + tx[i];
                sum += wx[i] * (wy[0] * row[ty[0]] + wy[1] * row[ty[1]] + wy[2] * row[ty[2]] + wy[3] * row[ty[3]]);
            }
            return sum;
        }
        crd::usize tap[k_grid_max_dim][4];
        T cw[k_grid_max_dim][4];
        for (crd::usize d = 0; d < m_dim; ++d)
        {
            crd::usize base;
            T frac;
            locate(d, query[d], base, frac);
            cubic_conv_weights(frac, cw[d]);
            const crd::usize hi = m_count[d] - 1;
            for (crd::usize k = 0; k < 4; ++k)
            {
                crd::usize tk = (base + k >= 1) ? base + k - 1 : 0; // base−1+k, no usize underflow
                tk = tk < hi ? tk : hi;                             // clamp to the last node
                tap[d][k] = tk * m_stride[d];
            }
        }
        T sum = static_cast<T>(0);
        const crd::usize combos = static_cast<crd::usize>(1) << (2 * m_dim); // 4^dim
        for (crd::usize c = 0; c < combos; ++c)
        {
            T w = static_cast<T>(1);
            crd::usize idx = 0;
            crd::usize cc = c;
            for (crd::usize d = 0; d < m_dim; ++d)
            {
                const crd::usize k = cc & 3u; // base-4 digit = the tap on axis d
                cc >>= 2;
                w *= cw[d][k];
                idx += tap[d][k];
            }
            sum += w * v[idx];
        }
        return sum;
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_dim; }

private:
    // Locate the cell index + fractional offset along axis d, clamped to the edge cell (no NaN on out-of-range).
    // Branchless (cmov): no per-query mispredict. Bit-identical to the clamped two-branch form for in-range queries.
    void locate(crd::usize d, T q, crd::usize& base, T& frac) const noexcept
    {
        const T t = (q - m_origin[d]) * m_inv_spacing[d];
        const T tf = t > static_cast<T>(0) ? t : static_cast<T>(0); // clamp below origin (cmov)
        crd::usize ib = static_cast<crd::usize>(tf);                 // floor (tf ≥ 0)
        const crd::usize maxb = m_count[d] - 2;
        ib = ib < maxb ? ib : maxb; // clamp above the last cell (cmov)
        base = ib;
        frac = t - static_cast<T>(ib);
    }

    // Keys cubic-convolution weights (a = −0.5) for the 4 taps base−1..base+2 at fractional offset t∈[0,1].
    // Interpolating (t=0 ⇒ {0,1,0,0}), partition of unity, reproduces linear.
    static void cubic_conv_weights(T t, T w[4]) noexcept
    {
        const T t2 = t * t;
        const T t3 = t2 * t;
        w[0] = static_cast<T>(-0.5) * t3 + t2 - static_cast<T>(0.5) * t;
        w[1] = static_cast<T>(1.5) * t3 - static_cast<T>(2.5) * t2 + static_cast<T>(1);
        w[2] = static_cast<T>(-1.5) * t3 + static_cast<T>(2) * t2 + static_cast<T>(0.5) * t;
        w[3] = static_cast<T>(0.5) * t3 - static_cast<T>(0.5) * t2;
    }

    crd::usize m_dim = 0;
    crd::containers::Array<T> m_origin;
    crd::containers::Array<T> m_spacing;
    crd::containers::Array<T> m_inv_spacing;
    crd::containers::Array<crd::usize> m_count;
    crd::containers::Array<crd::usize> m_stride;
    crd::containers::Array<T> m_values;
};

} // namespace crd::hesap::interp
