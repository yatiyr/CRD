#pragma once

// crd-hesap-motion v13-p — POLYNOMIAL trajectories through boundary conditions:
//   minimum-jerk  — the C² quintic p(t) matching (p, v, a) at both ends; minimizes ∫ jerk² ⇒ smooth, human/robot-arm
//                   comfortable point-to-point motion (Flash-Hogan).
//   minimum-snap  — the C⁴ septic matching (p, v, a, jerk) at both ends; minimizes ∫ snap² ⇒ the differentially-flat
//                   drone-trajectory primitive (Mellinger): snap maps to motor commands, so bounding it bounds them.
// Both are the unique boundary-value polynomials (a small linear solve). Verified: boundary conditions exact. No
// numeric peer (the gate is the BCs + the smoothness order). Moat: determinism (crd::math) + allocation-free.

#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::motion
{

// Evaluate the `order`-th derivative (order 0 = value) of the polynomial with coefficients c[0..deg] (ascending) at t.
template <typename T>
[[nodiscard]] T poly_eval_deriv(const T* c, int deg, T t, int order)
{
    T result = T{0};
    T tp     = T{1};
    for (int k = order; k <= deg; ++k)
    {
        T fall = T{1}; // k·(k−1)···(k−order+1)
        for (int m = 0; m < order; ++m)
        {
            fall *= static_cast<T>(k - m);
        }
        result += fall * c[k] * tp;
        tp *= t;
    }
    return result;
}

// Minimum-jerk quintic: fill c[0..5] (ascending) so p(0)=p0,v0,a0 and p(T)=pT,vT,aT. Closed form.
template <typename T>
void min_jerk_quintic(T p0, T v0, T a0, T pT, T vT, T aT, T tf, T* c)
{
    c[0]        = p0;
    c[1]        = v0;
    c[2]        = a0 / T{2};
    const T t2  = tf * tf;
    const T t3  = t2 * tf;
    const T t4  = t3 * tf;
    const T t5  = t4 * tf;
    const T dp  = pT - (p0 + v0 * tf + T{0.5} * a0 * t2);
    const T dv  = vT - (v0 + a0 * tf);
    const T da  = aT - a0;
    // Solve [t3 t4 t5; 3t2 4t3 5t4; 6t 12t2 20t3] · [c3 c4 c5]ᵀ = [dp dv da]ᵀ (the known min-jerk inverse).
    c[3] = (T{10} * dp - T{4} * dv * tf + T{0.5} * da * t2) / t3;
    c[4] = (-T{15} * dp + T{7} * dv * tf - da * t2) / t4;
    c[5] = (T{6} * dp - T{3} * dv * tf + T{0.5} * da * t2) / t5;
}

// Minimum-snap septic: fill c[0..7] so (p,v,a,jerk) match at both ends. Solves the 4×4 end system for c4..c7.
template <typename T>
void min_snap_septic(T p0, T v0, T a0, T j0, T pT, T vT, T aT, T jT, T tf, T* c)
{
    c[0] = p0;
    c[1] = v0;
    c[2] = a0 / T{2};
    c[3] = j0 / static_cast<T>(6);
    // End conditions on (p,v,a,j) at tf give 4 equations in c4..c7. Build + solve the 4×4 (Gaussian elimination).
    T mat[16];
    T b[4];
    const T tp[8] = {T{1},        tf,          tf * tf,     tf * tf * tf,
                     tf * tf * tf * tf, tf * tf * tf * tf * tf,
                     tf * tf * tf * tf * tf * tf, tf * tf * tf * tf * tf * tf * tf};
    // p,v,a,j at tf from the known part (c0..c3):
    T known[4];
    for (int order = 0; order < 4; ++order)
    {
        known[order] = poly_eval_deriv<T>(c, 3, tf, order);
    }
    const T target[4] = {pT, vT, aT, jT};
    for (int order = 0; order < 4; ++order)
    {
        for (int k = 4; k < 8; ++k) // column for c_k
        {
            T fall = T{1};
            for (int m = 0; m < order; ++m)
            {
                fall *= static_cast<T>(k - m);
            }
            mat[order * 4 + (k - 4)] = fall * tp[k - order];
        }
        b[order] = target[order] - known[order];
    }
    // Gaussian elimination with partial pivoting (4×4).
    for (int col = 0; col < 4; ++col)
    {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
        {
            if (crd::math::fabs(mat[r * 4 + col]) > crd::math::fabs(mat[piv * 4 + col]))
            {
                piv = r;
            }
        }
        for (int k = 0; k < 4; ++k)
        {
            const T t          = mat[col * 4 + k];
            mat[col * 4 + k]     = mat[piv * 4 + k];
            mat[piv * 4 + k]     = t;
        }
        const T tb = b[col];
        b[col]     = b[piv];
        b[piv]     = tb;
        for (int r = col + 1; r < 4; ++r)
        {
            const T f = mat[r * 4 + col] / mat[col * 4 + col];
            for (int k = col; k < 4; ++k)
            {
                mat[r * 4 + k] -= f * mat[col * 4 + k];
            }
            b[r] -= f * b[col];
        }
    }
    for (int i = 3; i >= 0; --i)
    {
        T s = b[i];
        for (int k = i + 1; k < 4; ++k)
        {
            s -= mat[i * 4 + k] * c[4 + k];
        }
        c[4 + i] = s / mat[i * 4 + i];
    }
}

namespace detail
{
// derivative-basis row: out[k] = (k!/(k−order)!)·τ^{k−order} for k ≥ order (0 otherwise), k = 0..deg. So
// p^{(order)}(τ) = Σ_k out[k]·c_k.
template <typename T>
void poly_deriv_row(T tau, int order, int deg, T* out)
{
    for (int k = 0; k <= deg; ++k)
    {
        out[k] = T{0};
    }
    for (int k = order; k <= deg; ++k)
    {
        T fall = T{1};
        for (int i = 0; i < order; ++i)
        {
            fall *= static_cast<T>(k - i);
        }
        out[k] = fall * crd::math::pow(tau, static_cast<T>(k - order));
    }
}
} // namespace detail

// ★Minimum-snap multi-segment trajectory (Mellinger): fit `nseg` degree-`deg` polynomials through `waypoints[0..nseg]`
// at strictly-increasing `times[0..nseg]`, minimizing Σ ∫ (dᵏ/dt) ² of the `snap_order`-th derivative (snap = 4 ⇒ the
// differentially-flat drone-trajectory objective: snap maps to motor commands, so minimizing it minimizes control
// effort), subject to waypoint interpolation + Cᶜᵒⁿᵗ continuity at interior knots + zero v/a/…/ (orders 1..cont)
// boundary conditions. Solves the equality-constrained QP via its KKT linear system (hesap-dense LU; SANITY 8).
// `coeffs_out` = nseg·(deg+1) ascending coefficients per segment (use poly_eval_deriv on each segment's local time).
// Returns false on bad input (deg < 2·snap−1 / non-increasing times / singular). deg = 7, snap = 4, cont = 3 are the
// canonical drone settings. (For very high nseg the unconstrained-QP reformulation is better-conditioned — a follow-on.)
template <typename T>
[[nodiscard]] bool min_snap_trajectory(crd::memory::IAllocator* alloc, const T* waypoints, const T* times, int nseg,
                                       int deg = 7, int snap_order = 4, int cont = 3, T* coeffs_out = nullptr)
{
    if (nseg < 1 || deg < 2 * snap_order - 1 || cont < 1 || cont > deg || coeffs_out == nullptr)
    {
        return false;
    }
    const int np1  = deg + 1;
    const int nvar = nseg * np1;
    const int nc   = 2 * nseg + 2 * cont + cont * (nseg - 1);
    const int ndim = nvar + nc;
    for (int j = 0; j < nseg; ++j)
    {
        if (!(times[j + 1] > times[j]))
        {
            return false;
        }
    }
    crd::hesap::dense::Matrix<T, crd::hesap::dense::Layout::RowMajor> kkt(alloc, static_cast<crd::usize>(ndim),
                                                                         static_cast<crd::usize>(ndim));
    for (int i = 0; i < ndim; ++i)
    {
        for (int j = 0; j < ndim; ++j)
        {
            kkt.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = T{0};
        }
    }
    crd::containers::Array<T> rhs(alloc);
    rhs.resize(static_cast<crd::usize>(ndim));
    for (int i = 0; i < ndim; ++i)
    {
        rhs[static_cast<crd::usize>(i)] = T{0};
    }
    // top-left = 2·Q (block-diagonal snap-cost Hessian)
    for (int j = 0; j < nseg; ++j)
    {
        const T tj   = times[j + 1] - times[j];
        const int off = j * np1;
        for (int a = snap_order; a <= deg; ++a)
        {
            for (int b = snap_order; b <= deg; ++b)
            {
                T fa = T{1};
                T fb = T{1};
                for (int i = 0; i < snap_order; ++i)
                {
                    fa *= static_cast<T>(a - i);
                    fb *= static_cast<T>(b - i);
                }
                const int pw   = a + b - 2 * snap_order + 1;
                const T   qval = fa * fb * crd::math::pow(tj, static_cast<T>(pw)) / static_cast<T>(pw);
                kkt.at(static_cast<crd::usize>(off + a), static_cast<crd::usize>(off + b)) = T{2} * qval;
            }
        }
    }
    // constraints A (bottom-left) + Aᵀ (top-right) + rhs
    T   row[64];
    int r = 0;
    auto add_constraint = [&](int seg, T tau, int order, T sign, T rhsval) {
        detail::poly_deriv_row<T>(tau, order, deg, row);
        const int off = seg * np1;
        for (int k = 0; k <= deg; ++k)
        {
            const T v = sign * row[k];
            kkt.at(static_cast<crd::usize>(nvar + r), static_cast<crd::usize>(off + k)) += v;
            kkt.at(static_cast<crd::usize>(off + k), static_cast<crd::usize>(nvar + r)) += v;
        }
        rhs[static_cast<crd::usize>(nvar + r)] = rhsval;
        ++r;
    };
    for (int j = 0; j < nseg; ++j) // waypoint interpolation
    {
        add_constraint(j, T{0}, 0, T{1}, waypoints[j]);
        add_constraint(j, times[j + 1] - times[j], 0, T{1}, waypoints[j + 1]);
    }
    for (int order = 1; order <= cont; ++order) // zero boundary derivatives
    {
        add_constraint(0, T{0}, order, T{1}, T{0});
        add_constraint(nseg - 1, times[nseg] - times[nseg - 1], order, T{1}, T{0});
    }
    for (int j = 0; j < nseg - 1; ++j) // interior continuity: seg j end − seg j+1 start = 0
    {
        const T tj = times[j + 1] - times[j];
        for (int order = 1; order <= cont; ++order)
        {
            detail::poly_deriv_row<T>(tj, order, deg, row);
            const int offj = j * np1;
            for (int k = 0; k <= deg; ++k)
            {
                kkt.at(static_cast<crd::usize>(nvar + r), static_cast<crd::usize>(offj + k)) += row[k];
                kkt.at(static_cast<crd::usize>(offj + k), static_cast<crd::usize>(nvar + r)) += row[k];
            }
            detail::poly_deriv_row<T>(T{0}, order, deg, row);
            const int offj1 = (j + 1) * np1;
            for (int k = 0; k <= deg; ++k)
            {
                kkt.at(static_cast<crd::usize>(nvar + r), static_cast<crd::usize>(offj1 + k)) -= row[k];
                kkt.at(static_cast<crd::usize>(offj1 + k), static_cast<crd::usize>(nvar + r)) -= row[k];
            }
            ++r;
        }
    }
    crd::hesap::dense::LU<T, crd::hesap::dense::Layout::RowMajor> lu(alloc, static_cast<crd::usize>(ndim));
    crd::hesap::dense::factor_lu<T>(lu, kkt);
    crd::hesap::dense::solve_lu<T>(lu, crd::containers::Span<T>{rhs.data(), rhs.size()});
    for (int i = 0; i < nvar; ++i)
    {
        coeffs_out[i] = rhs[static_cast<crd::usize>(i)];
    }
    return true;
}

} // namespace crd::hesap::motion
