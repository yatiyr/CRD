#pragma once

// dae.hpp — Phase 3.1.6 v9-l: higher-index DAE — INDEX REDUCTION for constrained mechanical systems (the
// multibody/eylem consumer the v9-l rationale names). A constrained system
//     M(q)·q̈ = f(t, q, q̇) − G(t,q)ᵀ·λ,   c(q) = 0   (G = ∂c/∂q)
// is index 3 in (q, λ). The classic reduction differentiates c twice to the ACCELERATION constraint
//     G·q̈ = γ,   γ = −(d/dt G)·q̇,
// at which point λ is determined ALGEBRAICALLY (index 1). Eliminating λ through the KKT/Schur system
//     (G·M⁻¹·Gᵀ)·λ = G·M⁻¹·f − γ,   q̈ = M⁻¹(f − Gᵀ·λ)
// yields a plain first-order ODE in [q, q̇] integrable by ANY v9 driver — the Mattsson-Söderlind dummy-
// derivative idea specialized to mechanics (the acceleration-level constraint is the dummy-derivative
// selection for this class). Started from CONSISTENT initial conditions (c=0, G·q̇=0) the position
// constraint holds to the integrator's accuracy (long-horizon drift-free GGL/projection = the named
// enhancement; general symbolic reduction of arbitrary DAEs needs an AD-residual layer = the named bridge,
// since hesap-ode may not edge to hesap-opt's Dual). The dense KKT solves use hesap-dense LU. ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

// A constrained mechanical system M·q̈ = f − Gᵀλ, c(q)=0. nq generalized coordinates, nc holonomic
// constraints. All spans are caller-validated to the stated sizes.
template <typename T> class ConstrainedMechanicalSystem
{
public:
    virtual ~ConstrainedMechanicalSystem() = default;
    [[nodiscard]] virtual crd::usize n_coords() const noexcept = 0;       // nq
    [[nodiscard]] virtual crd::usize n_constraints() const noexcept = 0;  // nc
    // Mass matrix M(q) (nq×nq, ROW-MAJOR, SPD).
    virtual void mass(T t, crd::containers::ConstSpan<T> q, crd::containers::Span<T> m) const = 0;
    // Applied force f(t, q, v) (nq).
    virtual void force(T t, crd::containers::ConstSpan<T> q, crd::containers::ConstSpan<T> v,
                       crd::containers::Span<T> f) const = 0;
    // Constraint Jacobian G = ∂c/∂q (nc×nq, ROW-MAJOR).
    virtual void constraint_jacobian(T t, crd::containers::ConstSpan<T> q, crd::containers::Span<T> g) const = 0;
    // The acceleration-constraint RHS γ = −(d/dt G)·v (nc) so that G·q̈ = γ.
    virtual void gamma(T t, crd::containers::ConstSpan<T> q, crd::containers::ConstSpan<T> v,
                       crd::containers::Span<T> gam) const = 0;
    // Constraint value c(q) (nc) — for the consistency check / drift monitor (not used in the RHS).
    virtual void constraint(T t, crd::containers::ConstSpan<T> q, crd::containers::Span<T> c) const = 0;
};

// The index-1 reduced ODE over Y = [q, v] (size 2·nq): solves the KKT system for λ each evaluation and
// returns [v, M⁻¹(f − Gᵀλ)]. A plain OdeFunction (no analytic Jacobian — the reduced field is smooth;
// drive with RK45 for non-stiff multibody, or BDF with the FD fallback).
template <typename T> class IndexReducedMechanicalOde final : public OdeFunction<T>
{
public:
    IndexReducedMechanicalOde(crd::memory::IAllocator* alloc, const ConstrainedMechanicalSystem<T>& sys)
        : m_sys(&sys), m_nq(sys.n_coords()), m_nc(sys.n_constraints()), m_alloc(alloc), m_m(alloc, sys.n_coords(),
                                                                                            sys.n_coords()),
          m_lu_m(alloc, sys.n_coords()), m_s(alloc, sys.n_constraints(), sys.n_constraints()),
          m_lu_s(alloc, sys.n_constraints()), m_g(alloc), m_f(alloc), m_gam(alloc), m_minv_f(alloc), m_minv_gt(alloc),
          m_lam(alloc), m_col(alloc)
    {
        m_g.resize(m_nc * m_nq);
        m_f.resize(m_nq);
        m_gam.resize(m_nc);
        m_minv_f.resize(m_nq);
        m_minv_gt.resize(m_nq * m_nc); // columns of M⁻¹Gᵀ (nq × nc), column-major-ish stored row-major nq×nc
        m_lam.resize(m_nc);
        m_col.resize(m_nq);
    }

    void rhs(T t, crd::containers::ConstSpan<T> Y, crd::containers::Span<T> dY) const override
    {
        namespace cont = crd::containers;
        const crd::containers::ConstSpan<T> q(Y.data(), m_nq);
        const crd::containers::ConstSpan<T> v(Y.data() + m_nq, m_nq);

        m_sys->mass(t, q, cont::Span<T>(m_m.data(), m_nq * m_nq));
        m_sys->force(t, q, v, cont::Span<T>(m_f.data(), m_nq));
        m_sys->constraint_jacobian(t, q, cont::Span<T>(m_g.data(), m_nc * m_nq));
        m_sys->gamma(t, q, v, cont::Span<T>(m_gam.data(), m_nc));

        // Factor M once.
        dense::factor_lu(m_lu_m, m_m);

        // M⁻¹·f
        for (crd::usize i = 0; i < m_nq; ++i)
        {
            m_minv_f[i] = m_f[i];
        }
        dense::solve_lu(m_lu_m, cont::Span<T>(m_minv_f.data(), m_nq));

        // M⁻¹·Gᵀ column by column (column k = M⁻¹·(row k of G)).
        for (crd::usize k = 0; k < m_nc; ++k)
        {
            for (crd::usize i = 0; i < m_nq; ++i)
            {
                m_col[i] = m_g[k * m_nq + i]; // (Gᵀ)_{i,k} = G_{k,i}
            }
            dense::solve_lu(m_lu_m, cont::Span<T>(m_col.data(), m_nq));
            for (crd::usize i = 0; i < m_nq; ++i)
            {
                m_minv_gt[i * m_nc + k] = m_col[i];
            }
        }

        // Schur S = G·(M⁻¹Gᵀ) (nc×nc) and rhs_λ = G·(M⁻¹f) − γ.
        for (crd::usize a = 0; a < m_nc; ++a)
        {
            for (crd::usize b = 0; b < m_nc; ++b)
            {
                T acc = static_cast<T>(0);
                for (crd::usize i = 0; i < m_nq; ++i)
                {
                    acc += m_g[a * m_nq + i] * m_minv_gt[i * m_nc + b];
                }
                m_s.at(a, b) = acc;
            }
            T r = static_cast<T>(0);
            for (crd::usize i = 0; i < m_nq; ++i)
            {
                r += m_g[a * m_nq + i] * m_minv_f[i];
            }
            m_lam[a] = r - m_gam[a];
        }
        // Solve S·λ = rhs_λ.
        if (m_nc > 0)
        {
            dense::factor_lu(m_lu_s, m_s);
            dense::solve_lu(m_lu_s, cont::Span<T>(m_lam.data(), m_nc));
        }

        // q̇ = v; v̇ = M⁻¹(f − Gᵀλ) = M⁻¹f − (M⁻¹Gᵀ)λ.
        for (crd::usize i = 0; i < m_nq; ++i)
        {
            dY[i] = v[i];
            T a_i = m_minv_f[i];
            for (crd::usize k = 0; k < m_nc; ++k)
            {
                a_i -= m_minv_gt[i * m_nc + k] * m_lam[k];
            }
            dY[m_nq + i] = a_i;
        }
    }

    [[nodiscard]] crd::usize dim() const noexcept override { return 2 * m_nq; }

private:
    const ConstrainedMechanicalSystem<T>* m_sys;
    crd::usize m_nq;
    crd::usize m_nc;
    crd::memory::IAllocator* m_alloc;
    mutable dense::Matrix<T, dense::Layout::RowMajor> m_m;
    mutable dense::LU<T, dense::Layout::RowMajor> m_lu_m;
    mutable dense::Matrix<T, dense::Layout::RowMajor> m_s;
    mutable dense::LU<T, dense::Layout::RowMajor> m_lu_s;
    mutable crd::containers::Array<T> m_g;
    mutable crd::containers::Array<T> m_f;
    mutable crd::containers::Array<T> m_gam;
    mutable crd::containers::Array<T> m_minv_f;
    mutable crd::containers::Array<T> m_minv_gt;
    mutable crd::containers::Array<T> m_lam;
    mutable crd::containers::Array<T> m_col;
};

} // namespace crd::hesap::ode
