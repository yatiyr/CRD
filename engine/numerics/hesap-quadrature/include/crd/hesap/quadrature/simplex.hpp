#pragma once

// crd-hesap-quadrature v13-k - SIMPLEX (triangle) cubature: Dunavant 1985 symmetric rules, exact to degree
// d in {1..6}, for FEM/CFD element integration on unstructured 2-D meshes (the single most-reused v13 cubature
// for the engineering-calc thesis). int_T f dA = Area * sum_i w_i f(P_i), P_i = lam1*v0 + lam2*v1 + lam3*v2.
// Barycentric points/weights verified by polynomial exactness (int x^a y^b = a!b!/(a+b+2)! on the ref tri).
// No clean scipy/MATLAB/Boost/GSL peer (quadpy deprecated) - the gate IS the analytic exactness degree.
// Moat: determinism + allocation-free + the error-tier QuadResult (Tier-0 fixed rule).

#include <crd/core/types.hpp>
#include <crd/hesap/quadrature/integrate.hpp>
#include <crd/math/cmath.hpp>

#include <utility>

namespace crd::hesap::quadrature
{
namespace detail
{
template <typename T> struct Dunavant1
{
    static constexpr int kNpoints = 1;
    static constexpr T kData[1 * 4] = {
        // lam1, lam2, lam3, weight
        static_cast<T>(0.3333333333333333),
        static_cast<T>(0.3333333333333333),
        static_cast<T>(0.3333333333333333),
        static_cast<T>(1.0),
    };
};
template <typename T> struct Dunavant2
{
    static constexpr int kNpoints = 3;
    static constexpr T kData[3 * 4] = {
        // lam1, lam2, lam3, weight
        static_cast<T>(0.16666666666666666), static_cast<T>(0.16666666666666666), static_cast<T>(0.6666666666666667),
        static_cast<T>(0.3333333333333333),  static_cast<T>(0.6666666666666667),  static_cast<T>(0.16666666666666666),
        static_cast<T>(0.16666666666666666), static_cast<T>(0.3333333333333333),  static_cast<T>(0.16666666666666666),
        static_cast<T>(0.6666666666666667),  static_cast<T>(0.16666666666666666), static_cast<T>(0.3333333333333333),
    };
};
template <typename T> struct Dunavant3
{
    static constexpr int kNpoints = 4;
    static constexpr T kData[4 * 4] = {
        // lam1, lam2, lam3, weight
        static_cast<T>(0.3333333333333333),
        static_cast<T>(0.3333333333333333),
        static_cast<T>(0.3333333333333333),
        static_cast<T>(-0.5625),
        static_cast<T>(0.6),
        static_cast<T>(0.2),
        static_cast<T>(0.2),
        static_cast<T>(0.520833333333333),
        static_cast<T>(0.2),
        static_cast<T>(0.6),
        static_cast<T>(0.2),
        static_cast<T>(0.520833333333333),
        static_cast<T>(0.2),
        static_cast<T>(0.2),
        static_cast<T>(0.6),
        static_cast<T>(0.520833333333333),
    };
};
template <typename T> struct Dunavant4
{
    static constexpr int kNpoints = 6;
    static constexpr T kData[6 * 4] = {
        // lam1, lam2, lam3, weight
        static_cast<T>(0.445948490915965), static_cast<T>(0.10810301816807),  static_cast<T>(0.445948490915965),
        static_cast<T>(0.223381589678011), static_cast<T>(0.10810301816807),  static_cast<T>(0.445948490915965),
        static_cast<T>(0.445948490915965), static_cast<T>(0.223381589678011), static_cast<T>(0.445948490915965),
        static_cast<T>(0.445948490915965), static_cast<T>(0.10810301816807),  static_cast<T>(0.223381589678011),
        static_cast<T>(0.091576213509771), static_cast<T>(0.816847572980458), static_cast<T>(0.091576213509771),
        static_cast<T>(0.109951743655322), static_cast<T>(0.091576213509771), static_cast<T>(0.091576213509771),
        static_cast<T>(0.816847572980458), static_cast<T>(0.109951743655322), static_cast<T>(0.816847572980458),
        static_cast<T>(0.091576213509771), static_cast<T>(0.091576213509771), static_cast<T>(0.109951743655322),
    };
};
template <typename T> struct Dunavant5
{
    static constexpr int kNpoints = 7;
    static constexpr T kData[7 * 4] = {
        // lam1, lam2, lam3, weight
        static_cast<T>(0.3333333333333333), static_cast<T>(0.3333333333333333),
        static_cast<T>(0.3333333333333333), static_cast<T>(0.225),
        static_cast<T>(0.470142064105115),  static_cast<T>(0.470142064105115),
        static_cast<T>(0.05971587178977),   static_cast<T>(0.132394152788506),
        static_cast<T>(0.05971587178977),   static_cast<T>(0.470142064105115),
        static_cast<T>(0.470142064105115),  static_cast<T>(0.132394152788506),
        static_cast<T>(0.470142064105115),  static_cast<T>(0.05971587178977),
        static_cast<T>(0.470142064105115),  static_cast<T>(0.132394152788506),
        static_cast<T>(0.101286507323456),  static_cast<T>(0.101286507323456),
        static_cast<T>(0.797426985353088),  static_cast<T>(0.125939180544827),
        static_cast<T>(0.797426985353088),  static_cast<T>(0.101286507323456),
        static_cast<T>(0.101286507323456),  static_cast<T>(0.125939180544827),
        static_cast<T>(0.101286507323456),  static_cast<T>(0.797426985353088),
        static_cast<T>(0.101286507323456),  static_cast<T>(0.125939180544827),
    };
};
template <typename T> struct Dunavant6
{
    static constexpr int kNpoints = 12;
    static constexpr T kData[12 * 4] = {
        // lam1, lam2, lam3, weight
        static_cast<T>(0.063089014491502), static_cast<T>(0.873821971016996), static_cast<T>(0.063089014491502),
        static_cast<T>(0.050844906370207), static_cast<T>(0.063089014491502), static_cast<T>(0.063089014491502),
        static_cast<T>(0.873821971016996), static_cast<T>(0.050844906370207), static_cast<T>(0.873821971016996),
        static_cast<T>(0.063089014491502), static_cast<T>(0.063089014491502), static_cast<T>(0.050844906370207),
        static_cast<T>(0.24928674517091),  static_cast<T>(0.50142650965818),  static_cast<T>(0.24928674517091),
        static_cast<T>(0.116786275726379), static_cast<T>(0.50142650965818),  static_cast<T>(0.24928674517091),
        static_cast<T>(0.24928674517091),  static_cast<T>(0.116786275726379), static_cast<T>(0.24928674517091),
        static_cast<T>(0.24928674517091),  static_cast<T>(0.50142650965818),  static_cast<T>(0.116786275726379),
        static_cast<T>(0.310352451033785), static_cast<T>(0.053145049844816), static_cast<T>(0.636502499121399),
        static_cast<T>(0.082851075618374), static_cast<T>(0.310352451033785), static_cast<T>(0.636502499121399),
        static_cast<T>(0.053145049844816), static_cast<T>(0.082851075618374), static_cast<T>(0.053145049844816),
        static_cast<T>(0.310352451033785), static_cast<T>(0.636502499121399), static_cast<T>(0.082851075618374),
        static_cast<T>(0.053145049844816), static_cast<T>(0.636502499121399), static_cast<T>(0.310352451033785),
        static_cast<T>(0.082851075618374), static_cast<T>(0.636502499121399), static_cast<T>(0.310352451033785),
        static_cast<T>(0.053145049844816), static_cast<T>(0.082851075618374), static_cast<T>(0.636502499121399),
        static_cast<T>(0.053145049844816), static_cast<T>(0.310352451033785), static_cast<T>(0.082851075618374),
    };
};
} // namespace detail

// Dunavant degree-d triangle cubature: int over the triangle (x0,y0),(x1,y1),(x2,y2) of f(x,y) dA, d in {1..6}.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_triangle(F&& f, T x0, T y0, T x1, T y1, T x2, T y2, int degree)
{
    const T* data = nullptr;
    int n = 0;
    switch (degree)
    {
        case 1:
            data = detail::Dunavant1<T>::kData;
            n = detail::Dunavant1<T>::kNpoints;
            break;
        case 2:
            data = detail::Dunavant2<T>::kData;
            n = detail::Dunavant2<T>::kNpoints;
            break;
        case 3:
            data = detail::Dunavant3<T>::kData;
            n = detail::Dunavant3<T>::kNpoints;
            break;
        case 4:
            data = detail::Dunavant4<T>::kData;
            n = detail::Dunavant4<T>::kNpoints;
            break;
        case 5:
            data = detail::Dunavant5<T>::kData;
            n = detail::Dunavant5<T>::kNpoints;
            break;
        case 6:
            data = detail::Dunavant6<T>::kData;
            n = detail::Dunavant6<T>::kNpoints;
            break;
        default:
            return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T area = crd::math::fabs((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)) / T{2};
    T acc = T{0};
    for (int i = 0; i < n; ++i)
    {
        const T l1 = data[i * 4 + 0];
        const T l2 = data[i * 4 + 1];
        const T l3 = data[i * 4 + 2];
        const T px = l1 * x0 + l2 * x1 + l3 * x2;
        const T py = l1 * y0 + l2 * y1 + l3 * y2;
        acc += data[i * 4 + 3] * f(px, py);
    }
    QuadResult<T> r;
    r.value = area * acc;
    r.eval_count = static_cast<crd::u32>(n);
    return r;
}

} // namespace crd::hesap::quadrature
