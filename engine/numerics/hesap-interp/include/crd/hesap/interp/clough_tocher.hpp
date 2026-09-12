#pragma once

// crd-hesap-interp v13-f — Clough-Tocher C¹ scattered 2-D interpolation (Clough-Tocher 1965; Alfeld 1984; Farin 1986).
// ★ the gold-standard C¹-smooth scattered interpolant: triangulate the sites, fit a piecewise-cubic Bézier
// macro-element per triangle that is globally C¹ and curvature-minimizing. Where Sibson NNI (geometry-delaunay) gives a
// bounded convex blend, Clough-Tocher gives a SMOOTH (continuously-differentiable) surface — the right tool for a
// scalar field a satellite/robot must differentiate (slopes, normals) without creases at the data sites.
//
//   CloughTocher2DInterpolant<T> — fit(points, values): Delaunay-triangulate (crd-geometry-delaunay), estimate the
//     vertex gradients by global curvature minimization (Nielson 1983 / Renka 1984 Gauss-Seidel), then precompute the
//     19 Bézier ordinates of the centroid-split C¹ cubic PER TRIANGLE (they depend only on the data + gradients +
//     neighbor triangles, never the query). eval(q): locate the triangle (jump-walk) and evaluate the cubic via the
//     "extended barycentric" form (one polynomial folds the 3-way sub-triangle selection in). NaN outside the hull.
//
//   The reduced (C¹-closing) direction is AFFINE-INVARIANT — w = (neighbor centroid − this centroid) — so the
//   per-triangle fit peeks at the three neighbor triangles. This matches scipy.interpolate.CloughTocher2DInterpolator
//   bit-for-bit (gradient iteration + macro-element transcribed from its _interpnd.pyx). The gradient is a convex
//   curvature-minimization ⇒ its fixed point is unique and order-independent; the moat (determinism by construction)
//   holds via crd::math (never std::) and a fixed FP order.

#include <crd/hesap/interp/piecewise.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/cmath.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::hesap::interp
{

template <Real T>
class CloughTocher2DInterpolant
{
public:
    explicit CloughTocher2DInterpolant(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_points(alloc), m_tri(alloc), m_tri_nbr(alloc), m_coeffs(alloc)
    {
    }

    // Build from scattered sites + values (same length, ≥ 3 points). tol / maxiter control the curvature-minimizing
    // gradient iteration (scipy defaults 1e-6 / 400; tighten for bit-exact reference agreement). Allocates once.
    [[nodiscard]] InterpStatus fit(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                                   crd::containers::ConstSpan<T> values, T tol = static_cast<T>(1e-6),
                                   int maxiter = 400)
    {
        const crd::usize n = points.size();
        if (n < 3 || values.size() != n)
        {
            return (m_status = InterpStatus::BadInput);
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            if (!detail::is_finite(points[i].x) || !detail::is_finite(points[i].y) || !detail::is_finite(values[i]))
            {
                return (m_status = InterpStatus::BadInput);
            }
        }
        m_points.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_points[i] = points[i];
        }

        auto del = crd::geometry::delaunay::delaunay_2d<T>(points, m_alloc);
        if (!del.ok())
        {
            return (m_status = InterpStatus::BadInput); // too-few / non-finite / duplicate → BadInput
        }
        m_tri_count = del.triangle_count;
        m_tri       = std::move(del.triangle_indices);
        build_tri_adjacency();

        // Gradients by global curvature minimization, then the 19 per-triangle Bézier ordinates.
        crd::containers::Array<T> grad(m_alloc);
        grad.resize(2 * n, static_cast<T>(0));
        estimate_gradients(values, grad, tol, maxiter);
        precompute_ordinates(values, grad);
        return (m_status = InterpStatus::Ok);
    }

    [[nodiscard]] InterpStatus status() const noexcept { return m_status; }
    [[nodiscard]] crd::u32 triangle_count() const noexcept { return m_tri_count; }

    // Evaluate at a query. Returns NaN if the interpolator is unbuilt, the query is non-finite, or the query lies
    // outside the convex hull (matching scipy's default fill_value=nan). noexcept, allocation-free, real-time.
    [[nodiscard]] T eval(const crd::math::Vec2<T>& q) const noexcept
    {
        const T nan = std::numeric_limits<T>::quiet_NaN();
        if (m_status != InterpStatus::Ok || !detail::is_finite(q.x) || !detail::is_finite(q.y))
        {
            return nan;
        }
        const crd::u32 t = locate(q);
        if (t == kNull)
        {
            return nan;
        }
        m_last_tri = t;

        const crd::u32 i0 = m_tri[3 * t + 0], i1 = m_tri[3 * t + 1], i2 = m_tri[3 * t + 2];
        T b0, b1, b2;
        barycentric(m_points[i0], m_points[i1], m_points[i2], q, b0, b1, b2);

        // Extended barycentric (scipy _interpnd.pyx): fold the centroid-split sub-triangle selection into one cubic.
        T mn = b0;
        mn   = b1 < mn ? b1 : mn;
        mn   = b2 < mn ? b2 : mn;
        const T u1 = b0 - mn, u2 = b1 - mn, u3 = b2 - mn, u4 = static_cast<T>(3) * mn;
        const T* c       = &m_coeffs[19 * static_cast<crd::usize>(t)];
        const T  three   = static_cast<T>(3);
        const T  six     = static_cast<T>(6);
        return u1 * u1 * u1 * c[0] + three * u1 * u1 * u2 * c[1] + three * u1 * u1 * u3 * c[2]
               + three * u1 * u1 * u4 * c[3] + three * u1 * u2 * u2 * c[4] + six * u1 * u2 * u4 * c[5]
               + three * u1 * u3 * u3 * c[6] + six * u1 * u3 * u4 * c[7] + three * u1 * u4 * u4 * c[8]
               + u2 * u2 * u2 * c[9] + three * u2 * u2 * u3 * c[10] + three * u2 * u2 * u4 * c[11]
               + three * u2 * u3 * u3 * c[12] + six * u2 * u3 * u4 * c[13] + three * u2 * u4 * u4 * c[14]
               + u3 * u3 * u3 * c[15] + three * u3 * u3 * u4 * c[16] + three * u3 * u4 * u4 * c[17]
               + u4 * u4 * u4 * c[18];
    }

private:
    static constexpr crd::u32 kNull = std::numeric_limits<crd::u32>::max();

    // Barycentric of P in triangle (A,B,C). Inside ⇒ all ≥ 0.
    static void barycentric(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b, const crd::math::Vec2<T>& cc,
                            const crd::math::Vec2<T>& p, T& b0, T& b1, T& b2) noexcept
    {
        const T d = (b.y - cc.y) * (a.x - cc.x) + (cc.x - b.x) * (a.y - cc.y);
        const T inv = static_cast<T>(1) / d;
        b0 = ((b.y - cc.y) * (p.x - cc.x) + (cc.x - b.x) * (p.y - cc.y)) * inv;
        b1 = ((cc.y - a.y) * (p.x - cc.x) + (a.x - cc.x) * (p.y - cc.y)) * inv;
        b2 = static_cast<T>(1) - b0 - b1;
    }

    // Triangle adjacency via sort-and-scan over 3T half-edges (D109 — same as geometry-delaunay NNI). Edge-indexed:
    // m_tri_nbr[3t+k] = the triangle across edge (v[k], v[(k+1)%3]) of triangle t, or kNull on a hull edge.
    void build_tri_adjacency()
    {
        m_tri_nbr.resize(static_cast<crd::usize>(m_tri_count) * 3, kNull);
        struct HE
        {
            crd::u32 a, b, tri, k;
        };
        crd::containers::Array<HE> he(m_alloc);
        he.reserve(static_cast<crd::usize>(m_tri_count) * 3);
        for (crd::u32 t = 0; t < m_tri_count; ++t)
        {
            for (crd::u32 k = 0; k < 3; ++k)
            {
                const crd::u32 u = m_tri[3 * t + k];
                const crd::u32 v = m_tri[3 * t + (k + 1) % 3];
                he.push_back(HE{u < v ? u : v, u < v ? v : u, t, k});
            }
        }
        crd::containers::sort(he.data(), he.data() + he.size(),
                              [](const HE& l, const HE& r) noexcept
                              {
                                  if (l.a != r.a)
                                  {
                                      return l.a < r.a;
                                  }
                                  if (l.b != r.b)
                                  {
                                      return l.b < r.b;
                                  }
                                  if (l.tri != r.tri)
                                  {
                                      return l.tri < r.tri;
                                  }
                                  return l.k < r.k;
                              });
        for (crd::u32 i = 0; i + 1 < he.size(); ++i)
        {
            if (he[i].a == he[i + 1].a && he[i].b == he[i + 1].b)
            {
                m_tri_nbr[3 * he[i].tri + he[i].k]         = he[i + 1].tri;
                m_tri_nbr[3 * he[i + 1].tri + he[i + 1].k] = he[i].tri;
                ++i;
            }
        }
    }

    // Non-adaptive orient2d (signed twice-area). The locate walk only needs the SIGN; a wrong sign near a degenerate
    // edge merely costs an extra step, and the CT is C⁰ across edges so either adjacent triangle gives the same value.
    // (The Delaunay BUILD uses the exact Shewchuk predicate; this fast form is purely for the query-time walk.)
    [[nodiscard]] static T fast_orient(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                                       const crd::math::Vec2<T>& c) noexcept
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    // Jump-walk point location, starting from the last-located triangle (clustered-query cache). The fast non-adaptive
    // orient2d makes the common case ~O(√T) cheap, but it gives up the exact-arithmetic termination guarantee of
    // Lawson's walk: a query near a degenerate edge can be misrouted to a hull edge or cycle. On ANY walk failure we
    // fall back to an EXACT linear scan (adaptive predicate) — off the hot path, but it guarantees no interior query
    // ever spuriously returns kNull/NaN. (The exhaustive scan also resolves the rare cycle deterministically.)
    [[nodiscard]] crd::u32 locate(const crd::math::Vec2<T>& q) const noexcept
    {
        crd::u32       cur      = m_last_tri < m_tri_count ? m_last_tri : 0;
        const crd::u32 max_step = m_tri_count * 4 + 16;
        for (crd::u32 step = 0; step < max_step; ++step)
        {
            if (cur == kNull)
            {
                break; // walked off a hull edge — confirm via the exact scan (a fast-orient misroute is possible)
            }
            const crd::u32 ia = m_tri[3 * cur + 0], ib = m_tri[3 * cur + 1], ic = m_tri[3 * cur + 2];
            const T s0 = fast_orient(m_points[ia], m_points[ib], q);
            const T s1 = fast_orient(m_points[ib], m_points[ic], q);
            const T s2 = fast_orient(m_points[ic], m_points[ia], q);
            if (s0 >= static_cast<T>(0) && s1 >= static_cast<T>(0) && s2 >= static_cast<T>(0))
            {
                return cur;
            }
            crd::u32 e = 3;
            if (s0 < static_cast<T>(0))
            {
                e = 0;
            }
            else if (s1 < static_cast<T>(0))
            {
                e = 1;
            }
            else if (s2 < static_cast<T>(0))
            {
                e = 2;
            }
            if (e >= 3)
            {
                return cur;
            }
            cur = m_tri_nbr[3 * cur + e];
        }
        return locate_scan(q);
    }

    // Exact O(T) containment scan with the adaptive Shewchuk predicate — the fallback that guarantees correctness when
    // the fast walk fails. Returns the containing triangle, or kNull if the query is genuinely outside the hull.
    [[nodiscard]] crd::u32 locate_scan(const crd::math::Vec2<T>& q) const noexcept
    {
        for (crd::u32 t = 0; t < m_tri_count; ++t)
        {
            const crd::u32 ia = m_tri[3 * t + 0], ib = m_tri[3 * t + 1], ic = m_tri[3 * t + 2];
            const T s0 = crd::geometry::primitives::orient2d(m_points[ia], m_points[ib], q);
            const T s1 = crd::geometry::primitives::orient2d(m_points[ib], m_points[ic], q);
            const T s2 = crd::geometry::primitives::orient2d(m_points[ic], m_points[ia], q);
            if (s0 >= static_cast<T>(0) && s1 >= static_cast<T>(0) && s2 >= static_cast<T>(0))
            {
                return t;
            }
        }
        return kNull;
    }

    // Global curvature-minimizing gradient estimate (Nielson 1983 / Renka 1984). Gauss-Seidel over vertices; for each,
    // a 2×2 solve from the edge-curvature sums of its incident edges. Transcribed from scipy _estimate_gradients_2d_global
    // (the convex problem ⇒ unique order-independent fixed point ⇒ tight tol matches scipy bit-close).
    void estimate_gradients(crd::containers::ConstSpan<T> data, crd::containers::Array<T>& y, T tol, int maxiter)
    {
        const crd::u32 n = static_cast<crd::u32>(m_points.size());
        // Vertex-neighbor CSR from the triangle edges (sorted, deduped — deterministic).
        crd::containers::Array<crd::u64> pairs(m_alloc); // (u<<32)|v, both directions
        pairs.reserve(static_cast<crd::usize>(m_tri_count) * 6);
        for (crd::u32 t = 0; t < m_tri_count; ++t)
        {
            for (crd::u32 k = 0; k < 3; ++k)
            {
                const crd::u64 u = m_tri[3 * t + k];
                const crd::u64 v = m_tri[3 * t + (k + 1) % 3];
                pairs.push_back((u << 32) | v);
                pairs.push_back((v << 32) | u);
            }
        }
        crd::containers::sort(pairs.data(), pairs.data() + pairs.size(),
                              [](crd::u64 l, crd::u64 r) noexcept { return l < r; });
        crd::containers::Array<crd::u32> indptr(m_alloc), indices(m_alloc);
        indptr.resize(static_cast<crd::usize>(n) + 1, 0);
        crd::u64 prev = std::numeric_limits<crd::u64>::max();
        for (crd::usize i = 0; i < pairs.size(); ++i)
        {
            if (pairs[i] == prev)
            {
                continue;
            }
            prev               = pairs[i];
            const crd::u32 u   = static_cast<crd::u32>(pairs[i] >> 32);
            const crd::u32 v   = static_cast<crd::u32>(pairs[i] & 0xFFFFFFFFu);
            indices.push_back(v);
            ++indptr[u + 1];
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            indptr[i + 1] += indptr[i];
        }

        for (int it = 0; it < maxiter; ++it)
        {
            T err = static_cast<T>(0);
            for (crd::u32 ip = 0; ip < n; ++ip)
            {
                T q0 = 0, q1 = 0, q3 = 0, s0 = 0, s1 = 0;
                for (crd::u32 jj = indptr[ip]; jj < indptr[ip + 1]; ++jj)
                {
                    const crd::u32 ip2 = indices[jj];
                    const T ex = m_points[ip2].x - m_points[ip].x, ey = m_points[ip2].y - m_points[ip].y;
                    const T l  = crd::math::sqrt(ex * ex + ey * ey);
                    const T l3 = l * l * l;
                    const T f1 = data[ip], f2 = data[ip2];
                    const T df2 = -ex * y[2 * ip2 + 0] - ey * y[2 * ip2 + 1];
                    q0 += static_cast<T>(4) * ex * ex / l3;
                    q1 += static_cast<T>(4) * ex * ey / l3;
                    q3 += static_cast<T>(4) * ey * ey / l3;
                    s0 += (static_cast<T>(6) * (f1 - f2) - static_cast<T>(2) * df2) * ex / l3;
                    s1 += (static_cast<T>(6) * (f1 - f2) - static_cast<T>(2) * df2) * ey / l3;
                }
                const T det = q0 * q3 - q1 * q1;
                const T r0 = (q3 * s0 - q1 * s1) / det, r1 = (-q1 * s0 + q0 * s1) / det;
                T change   = crd::math::fabs(y[2 * ip + 0] + r0);
                const T c1 = crd::math::fabs(y[2 * ip + 1] + r1);
                change     = c1 > change ? c1 : change;
                y[2 * ip + 0] = -r0;
                y[2 * ip + 1] = -r1;
                const T ar0 = crd::math::fabs(r0), ar1 = crd::math::fabs(r1);
                T scale     = ar0 > ar1 ? ar0 : ar1;
                scale       = scale > static_cast<T>(1) ? scale : static_cast<T>(1);
                change /= scale;
                err = change > err ? change : err;
            }
            if (err < tol)
            {
                break;
            }
        }
    }

    // Precompute the 19 Bézier ordinates of the C¹ macro-element per triangle (scipy _clough_tocher_2d_single).
    void precompute_ordinates(crd::containers::ConstSpan<T> values, const crd::containers::Array<T>& g)
    {
        m_coeffs.resize(19 * static_cast<crd::usize>(m_tri_count));
        const T two = static_cast<T>(2), three = static_cast<T>(3), half = static_cast<T>(0.5);
        for (crd::u32 t = 0; t < m_tri_count; ++t)
        {
            const crd::u32 i0 = m_tri[3 * t + 0], i1 = m_tri[3 * t + 1], i2 = m_tri[3 * t + 2];
            const T x0 = m_points[i0].x, y0 = m_points[i0].y;
            const T x1 = m_points[i1].x, y1 = m_points[i1].y;
            const T x2 = m_points[i2].x, y2 = m_points[i2].y;
            const T e12x = x1 - x0, e12y = y1 - y0, e23x = x2 - x1, e23y = y2 - y1, e31x = x0 - x2, e31y = y0 - y2;
            const T f1 = values[i0], f2 = values[i1], f3 = values[i2];
            const T df12 = g[2 * i0] * e12x + g[2 * i0 + 1] * e12y;
            const T df21 = -(g[2 * i1] * e12x + g[2 * i1 + 1] * e12y);
            const T df23 = g[2 * i1] * e23x + g[2 * i1 + 1] * e23y;
            const T df32 = -(g[2 * i2] * e23x + g[2 * i2 + 1] * e23y);
            const T df31 = g[2 * i2] * e31x + g[2 * i2 + 1] * e31y;
            const T df13 = -(g[2 * i0] * e31x + g[2 * i0 + 1] * e31y);

            const T c3000 = f1, c2100 = (df12 + three * c3000) / three, c2010 = (df13 + three * c3000) / three;
            const T c0300 = f2, c1200 = (df21 + three * c0300) / three, c0210 = (df23 + three * c0300) / three;
            const T c0030 = f3, c1020 = (df31 + three * c0030) / three, c0120 = (df32 + three * c0030) / three;
            const T c2001 = (c2100 + c2010 + c3000) / three;
            const T c0201 = (c1200 + c0300 + c0210) / three;
            const T c0021 = (c1020 + c0120 + c0030) / three;

            T gk[3];
            for (crd::u32 k = 0; k < 3; ++k)
            {
                const crd::u32 nbr = m_tri_nbr[3 * t + (k + 1) % 3]; // neighbor opposite vertex k
                if (nbr == kNull)
                {
                    gk[k] = -half;
                    continue;
                }
                const crd::u32 j0 = m_tri[3 * nbr + 0], j1 = m_tri[3 * nbr + 1], j2 = m_tri[3 * nbr + 2];
                const T cx = (m_points[j0].x + m_points[j1].x + m_points[j2].x) / three;
                const T cy = (m_points[j0].y + m_points[j1].y + m_points[j2].y) / three;
                T cb0, cb1, cb2;
                barycentric(m_points[i0], m_points[i1], m_points[i2], crd::math::Vec2<T>{cx, cy}, cb0, cb1, cb2);
                if (k == 0)
                {
                    gk[k] = (two * cb2 + cb1 - static_cast<T>(1)) / (two - three * cb2 - three * cb1);
                }
                else if (k == 1)
                {
                    gk[k] = (two * cb0 + cb2 - static_cast<T>(1)) / (two - three * cb0 - three * cb2);
                }
                else
                {
                    gk[k] = (two * cb1 + cb0 - static_cast<T>(1)) / (two - three * cb1 - three * cb0);
                }
            }
            const T c0111 = (gk[0] * (-c0300 + three * c0210 - three * c0120 + c0030)
                             + (-c0300 + two * c0210 - c0120 + c0021 + c0201))
                            / two;
            const T c1011 = (gk[1] * (-c0030 + three * c1020 - three * c2010 + c3000)
                             + (-c0030 + two * c1020 - c2010 + c2001 + c0021))
                            / two;
            const T c1101 = (gk[2] * (-c3000 + three * c2100 - three * c1200 + c0300)
                             + (-c3000 + two * c2100 - c1200 + c2001 + c0201))
                            / two;
            const T c1002 = (c1101 + c1011 + c2001) / three;
            const T c0102 = (c1101 + c0111 + c0201) / three;
            const T c0012 = (c1011 + c0111 + c0021) / three;
            const T c0003 = (c1002 + c0102 + c0012) / three;

            T* o  = &m_coeffs[19 * static_cast<crd::usize>(t)];
            o[0]  = c3000;
            o[1]  = c2100;
            o[2]  = c2010;
            o[3]  = c2001;
            o[4]  = c1200;
            o[5]  = c1101;
            o[6]  = c1020;
            o[7]  = c1011;
            o[8]  = c1002;
            o[9]  = c0300;
            o[10] = c0210;
            o[11] = c0201;
            o[12] = c0120;
            o[13] = c0111;
            o[14] = c0102;
            o[15] = c0030;
            o[16] = c0021;
            o[17] = c0012;
            o[18] = c0003;
        }
    }

    crd::memory::IAllocator*                   m_alloc;
    crd::containers::Array<crd::math::Vec2<T>> m_points;
    crd::containers::Array<crd::u32>           m_tri;     // 3 per triangle (CCW)
    crd::containers::Array<crd::u32>           m_tri_nbr; // 3 per triangle, edge-indexed, kNull on hull
    crd::containers::Array<T>                  m_coeffs;  // 19 per triangle (precomputed macro-element ordinates)
    crd::u32                                   m_tri_count = 0;
    mutable crd::u32                           m_last_tri  = 0;
    InterpStatus                               m_status    = InterpStatus::BadInput;
};

} // namespace crd::hesap::interp
