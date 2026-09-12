#pragma once

// ode_krylov_solver.hpp — Phase 3.1.6 v9-j (Krylov follow-on): the MATRIX-FREE implementation of the
// OdeLinearSolver seam — Newton-Krylov, exactly CVODE's SPGMR mode. The iteration matrix (I − c·J) is NEVER
// assembled: the stiff driver (BDF) records the linearization point and the inner linear solve runs
// hesap-iterative FGMRES on the operator v ↦ v − c·(J·v), with J·v supplied by OdeFunction::jacobian_vector
// (vtable slot 4 — reserved since v9-a). This is the O(n)-memory path for large-n method-of-lines systems
// where even a sparse factorization's fill is prohibitive (CVODE-KLU vs CVODE-SPGMR — the same fork).
//
// PRECONDITIONER SEAM = CVODE PrecSetup / PrecSolve: an optional OdeKrylovPreconditioner is setup() once per
// linearization (per factor_iteration_matrix_matfree) and applied as the FGMRES right-preconditioner M⁻¹
// (the v4 flexible-GMRES seam). MOAT: FGMRES is serial on the calling thread (Arnoldi/Givens/back-solve);
// the only parallel step is the operator's jac-vec, which is bit-identical across worker counts ⇒ the whole
// Newton-Krylov solve inherits the determinism moat. ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_linear_solver.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

// CVODE PrecSetup/PrecSolve analog. setup() is invoked once per linearization (c or point change); apply()
// approximates z ≈ (I − c·J)⁻¹·r. The caller owns the preconditioner's storage.
template <typename T> class OdeKrylovPreconditioner
{
public:
    virtual ~OdeKrylovPreconditioner() = default;
    // Build/refresh the preconditioner for (I − c·J) at (t, y). Returns false if setup failed.
    [[nodiscard]] virtual bool setup(T c, T t, crd::containers::ConstSpan<T> y) = 0;
    // z = M⁻¹·r.
    virtual void apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const = 0;
};

template <typename T> class KrylovOdeLinearSolver final : public OdeLinearSolver<T>
{
    using R = crd::hesap::dense::RealType<T>;

    // The matrix-free iteration operator A = I − c·J_lin.
    class IterMatOp final : public crd::hesap::LinearOp<T>
    {
    public:
        explicit IterMatOp(crd::memory::IAllocator* alloc) : m_jv(alloc) {}
        void configure(const OdeFunction<T>* fn, T t, crd::containers::ConstSpan<T> y, T c, crd::usize n,
                       crd::u64* matvecs)
        {
            m_fn = fn;
            m_t = t;
            m_y = y;
            m_c = c;
            m_n = n;
            m_matvecs = matvecs;
            m_jv.resize(n);
        }
        [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> out) const override
        {
            const bool ok = m_fn->jacobian_vector(m_t, m_y, x, crd::containers::Span<T>(m_jv.data(), m_n));
            CRD_ASSERT(ok);
            (void)ok;
            for (crd::usize i = 0; i < m_n; ++i)
            {
                out[i] = x[i] - m_c * m_jv[i];
            }
            if (m_matvecs != nullptr)
            {
                ++(*m_matvecs);
            }
            return true;
        }
        [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
        [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    private:
        mutable crd::containers::Array<T> m_jv;
        const OdeFunction<T>* m_fn = nullptr;
        T m_t{};
        crd::containers::ConstSpan<T> m_y{};
        T m_c{};
        crd::usize m_n = 0;
        crd::u64* m_matvecs = nullptr;
    };

    // Adapter wrapping the user preconditioner as a LinearOp (the FGMRES M⁻¹).
    class PrecOp final : public crd::hesap::LinearOp<T>
    {
    public:
        void configure(const OdeKrylovPreconditioner<T>* p, crd::usize n)
        {
            m_p = p;
            m_n = n;
        }
        [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> out) const override
        {
            m_p->apply(x, out);
            return true;
        }
        [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
        [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    private:
        const OdeKrylovPreconditioner<T>* m_p = nullptr;
        crd::usize m_n = 0;
    };

public:
    // `rel_tol` is the INEXACT-NEWTON FORCING tolerance: the inner FGMRES solves (I − c·J)·dy = r only to
    // ‖r‖ ≤ rel_tol·‖r₀‖ — as tightly as the Newton step needs, not to machine precision (CVODE's eplifac
    // default 0.05). Over-solving the inner system (a tight rel_tol like 1e-7) inflates the GMRES iteration
    // count ~5× for no accuracy gain — the Newton converges to its own tolerance regardless. Pass a tight
    // rel_tol only when you specifically want to REPRODUCE a direct-solve trajectory bit-for-bit.
    explicit KrylovOdeLinearSolver(crd::memory::IAllocator* alloc, crd::usize restart = 30,
                                   R rel_tol = static_cast<R>(0.05), crd::usize max_iter = 1000,
                                   OdeKrylovPreconditioner<T>* precond = nullptr)
        : m_alloc(alloc), m_restart(restart), m_rel_tol(rel_tol), m_max_iter(max_iter), m_precond(precond),
          m_op(alloc), m_ws(alloc, 0, restart), m_rhs(alloc), m_x(alloc), m_ylin(alloc)
    {
    }

    [[nodiscard]] bool is_matrix_free() const noexcept override { return true; }

    // Dense/sparse entry points are unsupported — this solver is matrix-free only.
    [[nodiscard]] bool factor_iteration_matrix(T, crd::containers::ConstSpan<T>, crd::usize) override { return false; }

    [[nodiscard]] bool factor_iteration_matrix_matfree(const OdeFunction<T>& fn, T t,
                                                       crd::containers::ConstSpan<T> y, T c) override
    {
        const crd::usize n = fn.dim();
        CRD_ASSERT(fn.has_jacobian_vector());
        if (m_n != n)
        {
            m_n = n;
            m_ylin.resize(n);
            m_rhs.resize(n);
            m_x.resize(n);
            m_ws = crd::hesap::iterative::GmresWorkspace<T>(m_alloc, n, m_restart);
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            m_ylin[i] = y[i];
        }
        m_op.configure(&fn, t, crd::containers::ConstSpan<T>(m_ylin.data(), n), c, n, &m_matvecs);
        if (m_precond != nullptr)
        {
            const bool ok = m_precond->setup(c, t, crd::containers::ConstSpan<T>(m_ylin.data(), n));
            if (!ok)
            {
                return false;
            }
            m_precop.configure(m_precond, n);
        }
        return true;
    }

    void solve(crd::containers::Span<T> b) override
    {
        const crd::usize n = m_n;
        for (crd::usize i = 0; i < n; ++i)
        {
            m_rhs[i] = b[i];
            m_x[i] = static_cast<T>(0);
        }
        crd::hesap::iterative::IterativeOptions<R> opts;
        opts.rel_tol = m_rel_tol;
        opts.abs_tol = static_cast<R>(0);
        opts.max_iter = m_max_iter;
        const crd::hesap::LinearOp<T>* minv = (m_precond != nullptr) ? &m_precop : nullptr;
        const auto res = crd::hesap::iterative::fgmres<T>(m_op, minv, crd::containers::ConstSpan<T>(m_rhs.data(), n),
                                                          crd::containers::Span<T>(m_x.data(), n), opts, m_ws, m_alloc);
        m_total_iters += res.iterations;
        ++m_total_solves;
        for (crd::usize i = 0; i < n; ++i)
        {
            b[i] = m_x[i];
        }
    }

    // Work telemetry for the v9-z work-precision scoreboard (NOT in OdeWork — matrix-free work is the
    // jac-vec/GMRES-iteration count, which lives below the driver's seam).
    [[nodiscard]] crd::u64 total_gmres_iterations() const noexcept { return m_total_iters; }
    [[nodiscard]] crd::u64 total_matvecs() const noexcept { return m_matvecs; }
    [[nodiscard]] crd::u64 total_solves() const noexcept { return m_total_solves; }

private:
    crd::memory::IAllocator* m_alloc;
    crd::usize m_restart;
    R m_rel_tol;
    crd::usize m_max_iter;
    OdeKrylovPreconditioner<T>* m_precond;
    IterMatOp m_op;
    PrecOp m_precop;
    crd::hesap::iterative::GmresWorkspace<T> m_ws;
    crd::containers::Array<T> m_rhs;
    crd::containers::Array<T> m_x;
    crd::containers::Array<T> m_ylin;
    crd::usize m_n = 0;
    crd::u64 m_matvecs = 0;
    crd::u64 m_total_iters = 0;
    crd::u64 m_total_solves = 0;
};

} // namespace crd::hesap::ode
