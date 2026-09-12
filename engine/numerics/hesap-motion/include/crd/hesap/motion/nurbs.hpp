#pragma once

// crd-hesap-motion v13-o — NURBS (Non-Uniform Rational B-Splines): the rational generalization of B-splines whose
// per-control-point WEIGHTS let a piecewise-polynomial curve represent conics EXACTLY — a true circle, ellipse, or
// circular arc (which no polynomial spline can). The CAD/CAM standard and the exact-arc primitive for tool paths and
// geometric path planning. Cox-de Boor basis (The NURBS Book A2.1/A2.2) + the rational combination. Gate: exact-circle
// reproduction (points land on x²+y²=1 to machine precision). Moat: determinism (crd::math) + allocation-free (bounded
// stack basis buffers, degree ≤ kNurbsMaxDegree).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::motion
{

constexpr int kNurbsMaxDegree = 10;

namespace detail
{
// The NURBS Book A2.1 FindSpan: the knot span index of parameter u (n = #ctrl−1, p = degree, knots length n+p+2).
template <typename T>
[[nodiscard]] int nurbs_find_span(int n, int p, T u, const T* knots) noexcept
{
    if (u >= knots[n + 1])
    {
        return n;
    }
    if (u <= knots[p])
    {
        return p;
    }
    int low  = p;
    int high = n + 1;
    int mid  = (low + high) / 2;
    while (u < knots[mid] || u >= knots[mid + 1])
    {
        if (u < knots[mid])
        {
            high = mid;
        }
        else
        {
            low = mid;
        }
        mid = (low + high) / 2;
    }
    return mid;
}

// The NURBS Book A2.2 BasisFuns: the p+1 non-zero basis functions at span `span`, into out[0..p].
template <typename T>
void nurbs_basis_funs(int span, T u, int p, const T* knots, T* out) noexcept
{
    T left[kNurbsMaxDegree + 1];
    T right[kNurbsMaxDegree + 1];
    out[0] = T{1};
    for (int j = 1; j <= p; ++j)
    {
        left[j]  = u - knots[span + 1 - j];
        right[j] = knots[span + j] - u;
        T saved  = T{0};
        for (int r = 0; r < j; ++r)
        {
            const T temp = out[r] / (right[r + 1] + left[j - r]);
            out[r]       = saved + right[r + 1] * temp;
            saved        = left[j - r] * temp;
        }
        out[j] = saved;
    }
}
} // namespace detail

// Evaluate a 2-D NURBS curve (control points cx/cy, weights w, knot vector `knots`, degree p) at parameter u, writing
// the point (x, y). #control-points = cx.size(); knots.size() must be #ctrl + p + 1. Rational: point = Σ N_i w_i P_i / Σ N_i w_i.
template <typename T>
void nurbs_eval2(crd::containers::ConstSpan<T> cx, crd::containers::ConstSpan<T> cy, crd::containers::ConstSpan<T> w,
                 crd::containers::ConstSpan<T> knots, int p, T u, T& x, T& y)
{
    const int nctrl = static_cast<int>(cx.size());
    const int n     = nctrl - 1;
    if (nctrl < p + 1 || p < 1 || p > kNurbsMaxDegree)
    {
        x = T{0};
        y = T{0};
        return;
    }
    const int span = detail::nurbs_find_span<T>(n, p, u, knots.data());
    T         basis[kNurbsMaxDegree + 1];
    detail::nurbs_basis_funs<T>(span, u, p, knots.data(), basis);
    T numx  = T{0};
    T numy  = T{0};
    T denom = T{0};
    for (int k = 0; k <= p; ++k)
    {
        const int       idx = span - p + k;
        const T         nw  = basis[k] * w[static_cast<crd::usize>(idx)];
        numx += nw * cx[static_cast<crd::usize>(idx)];
        numy += nw * cy[static_cast<crd::usize>(idx)];
        denom += nw;
    }
    x = numx / denom;
    y = numy / denom;
}

} // namespace crd::hesap::motion
