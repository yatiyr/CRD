#pragma once

// model.hpp — Phase 3.1.6 v7-o: the ALGEBRAIC MODELING LAYER (the JuMP / CasADi pattern, the ergonomic façade):
//
//     Model<f64> m(alloc);
//     const usize x = m.add_variables(2, /*start=*/0.0);             // bounds optional
//     m.minimize([](auto v) { return (v[0] - 1) * (v[0] - 1) + ...; });   // scalar-generic lambda
//     m.subject_to_eq([](auto v) { return v[0] + v[1] - 1; });           // c(x) = 0
//     m.subject_to_ge([](auto v) { return v[0]; });                      // c(x) ≥ 0  (le(...) negates into ge)
//     OptResult<f64> r = m.solve(opts);                                   // derivatives + solver dispatch: automatic
//
// • DECLARATIVE: variables (+ bounds + starts), one objective, any number of scalar equality/inequality
//   constraints — each a scalar-generic functor (the v7-b `DiffFunctor` contract: one templated
//   `operator()(ConstSpan<S>) const` instantiable on T and Dual<T>).
// • AUTO-DERIVATIVES: gradients and constraint Jacobians by EXACT forward-mode AD (`Dual<T>`, n passes per
//   function — the dense-small ergonomic path; large-scale work uses the raw Objective/Constraints API with
//   analytic or sparse derivatives).
// • DETERMINISTIC DISPATCH (overridable via ModelMethod): general constraints → damped-BFGS SQP (v7-n);
//   bounds only → L-BFGS-B (v7-d); unconstrained → L-BFGS. Auglag selectable. When general constraints are
//   present, finite variable bounds FOLD into c_I rows (user inequalities first in add order, then lower-bound
//   rows ascending, then upper-bound rows ascending — the multiplier ordering in `OptResult::multipliers`).
//
// HONEST SCOPE (named): Hessian-free members only — the modeling layer does not synthesize second derivatives
// (forward-over-forward is a future refinement; the exact-Hessian Newton/TR/filter-IPM members are raw-API).
// No expression graph / reverse-mode (the separate ADR-0065 autodiff module) — declarativeness comes from
// scalar-generic lambdas, which C++ gives for free. ADR-0090.
//
// DETERMINISM: fixed row order + serial forward-AD passes ⇒ the assembled problem and the trajectory are
// bit-identical run-to-run (and across workers wherever the dispatched solver carries the {1..16} moat).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/forward_ad.hpp>
#include <crd/hesap/opt/lbfgs.hpp>
#include <crd/hesap/opt/lbfgsb.hpp>
#include <crd/hesap/opt/nlp_auglag.hpp>
#include <crd/hesap/opt/nlp_sqp.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace crd::hesap::opt
{

enum class ModelMethod : crd::u8
{
    Auto,   // constraints → Sqp · bounds-only → Lbfgsb · else → Lbfgs
    Lbfgs,  // unconstrained only
    Lbfgsb, // bounds only
    Sqp,    // general (bounds folded into c_I)
    Auglag, // general (bounds folded into c_I)
};

namespace detail
{

// Type-erased scalar-generic function: the two instantiations (T and Dual<T>) behind one vtable.
template <typename T> class IModelFn
{
public:
    virtual ~IModelFn() = default;
    [[nodiscard]] virtual T eval(crd::containers::ConstSpan<T> x) const = 0;
    [[nodiscard]] virtual Dual<T> eval_dual(crd::containers::ConstSpan<Dual<T>> x) const = 0;
};

template <typename T, typename F> class ModelFn final : public IModelFn<T>
{
public:
    explicit ModelFn(F f) : m_f(std::move(f)) {}
    [[nodiscard]] T eval(crd::containers::ConstSpan<T> x) const override { return m_f(x); }
    [[nodiscard]] Dual<T> eval_dual(crd::containers::ConstSpan<Dual<T>> x) const override { return m_f(x); }

private:
    F m_f;
};

// A DiffFunctor view over a type-erased fn — lets FunctorObjective / forward_ad_gradient drive it unchanged.
template <typename T> struct ErasedFnView
{
    const IModelFn<T>* fn = nullptr;
    [[nodiscard]] T operator()(crd::containers::ConstSpan<T> x) const { return fn->eval(x); }
    [[nodiscard]] Dual<T> operator()(crd::containers::ConstSpan<Dual<T>> x) const { return fn->eval_dual(x); }
};

// The Constraints<T> the dispatched solver sees: user eq rows, user ineq rows, then the folded bound rows
// (lower then upper, ascending variable index). Jacobians by forward AD per row (±e_j for bound rows).
template <typename T> class ModelConstraintsAdapter final : public Constraints<T>
{
public:
    ModelConstraintsAdapter(crd::containers::ConstSpan<IModelFn<T>*> eq_fns,
                            crd::containers::ConstSpan<IModelFn<T>*> ineq_fns,
                            crd::containers::ConstSpan<crd::u32> lo_vars, crd::containers::ConstSpan<T> lo_vals,
                            crd::containers::ConstSpan<crd::u32> up_vars, crd::containers::ConstSpan<T> up_vals,
                            crd::usize n, crd::memory::IAllocator* alloc)
        : Constraints<T>(/*has_jacobians=*/true), m_eq(eq_fns), m_ineq(ineq_fns), m_lo_vars(lo_vars),
          m_lo_vals(lo_vals), m_up_vars(up_vars), m_up_vals(up_vals), m_n(n), m_alloc(alloc)
    {
    }

    [[nodiscard]] crd::usize num_eq() const noexcept override { return m_eq.size(); }
    [[nodiscard]] crd::usize num_ineq() const noexcept override
    {
        return m_ineq.size() + m_lo_vars.size() + m_up_vars.size();
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

    void eval(crd::containers::ConstSpan<T> x, crd::containers::Span<T> ce, crd::containers::Span<T> ci) const override
    {
        for (crd::usize i = 0; i < m_eq.size(); ++i)
        {
            ce[i] = m_eq[i]->eval(x);
        }
        crd::usize w = 0;
        for (crd::usize i = 0; i < m_ineq.size(); ++i)
        {
            ci[w++] = m_ineq[i]->eval(x);
        }
        for (crd::usize i = 0; i < m_lo_vars.size(); ++i)
        {
            ci[w++] = x[m_lo_vars[i]] - m_lo_vals[i]; // x_j − lo ≥ 0
        }
        for (crd::usize i = 0; i < m_up_vars.size(); ++i)
        {
            ci[w++] = m_up_vals[i] - x[m_up_vars[i]]; // up − x_j ≥ 0
        }
    }

    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<T> x, crd::containers::Span<T> je,
                                 crd::containers::Span<T> ji) const override
    {
        for (crd::usize i = 0; i < m_eq.size(); ++i)
        {
            fn_grad(m_eq[i], x, {je.data() + i * m_n, m_n});
        }
        crd::usize w = 0;
        for (crd::usize i = 0; i < m_ineq.size(); ++i)
        {
            fn_grad(m_ineq[i], x, {ji.data() + w * m_n, m_n});
            ++w;
        }
        for (crd::usize i = 0; i < m_lo_vars.size(); ++i) // row +e_j
        {
            T* row = ji.data() + w * m_n;
            for (crd::usize j = 0; j < m_n; ++j)
            {
                row[j] = static_cast<T>(0);
            }
            row[m_lo_vars[i]] = static_cast<T>(1);
            ++w;
        }
        for (crd::usize i = 0; i < m_up_vars.size(); ++i) // row −e_j
        {
            T* row = ji.data() + w * m_n;
            for (crd::usize j = 0; j < m_n; ++j)
            {
                row[j] = static_cast<T>(0);
            }
            row[m_up_vars[i]] = static_cast<T>(-1);
            ++w;
        }
        return true;
    }

private:
    void fn_grad(const IModelFn<T>* fn, crd::containers::ConstSpan<T> x, crd::containers::Span<T> g) const
    {
        const ErasedFnView<T> view{fn};
        (void)forward_ad_gradient<T>(view, x, g, m_alloc);
    }

    crd::containers::ConstSpan<IModelFn<T>*> m_eq;
    crd::containers::ConstSpan<IModelFn<T>*> m_ineq;
    crd::containers::ConstSpan<crd::u32> m_lo_vars;
    crd::containers::ConstSpan<T> m_lo_vals;
    crd::containers::ConstSpan<crd::u32> m_up_vars;
    crd::containers::ConstSpan<T> m_up_vals;
    crd::usize m_n;
    crd::memory::IAllocator* m_alloc;
};

} // namespace detail

template <typename T> class Model
{
public:
    explicit Model(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_start(alloc), m_lo(alloc), m_up(alloc), m_eq(alloc), m_ineq(alloc)
    {
        CRD_ASSERT_MSG(alloc != nullptr, "Model: allocator required");
    }

    ~Model()
    {
        if (m_objective != nullptr)
        {
            crd::memory::destroy(*m_alloc, m_objective);
        }
        for (crd::usize i = 0; i < m_eq.size(); ++i)
        {
            crd::memory::destroy(*m_alloc, m_eq[i]);
        }
        for (crd::usize i = 0; i < m_ineq.size(); ++i)
        {
            crd::memory::destroy(*m_alloc, m_ineq[i]);
        }
    }

    Model(const Model&) = delete; // owns raw type-erased functions
    Model(Model&&) = delete;
    Model& operator=(const Model&) = delete;
    Model& operator=(Model&&) = delete;

    // ---- Variables -------------------------------------------------------------------------------------

    // Add one variable; returns its index. Bounds default to free (±inf).
    crd::usize add_variable(T start = static_cast<T>(0), T lower = -std::numeric_limits<T>::infinity(),
                            T upper = std::numeric_limits<T>::infinity())
    {
        CRD_ASSERT_MSG(lower <= upper, "Model::add_variable: lower > upper");
        m_start.push_back(start);
        m_lo.push_back(lower);
        m_up.push_back(upper);
        return m_start.size() - 1;
    }

    // Add `count` variables sharing start/bounds; returns the FIRST index.
    crd::usize add_variables(crd::usize count, T start = static_cast<T>(0),
                             T lower = -std::numeric_limits<T>::infinity(),
                             T upper = std::numeric_limits<T>::infinity())
    {
        CRD_ASSERT_MSG(count > 0, "Model::add_variables: count == 0");
        const crd::usize first = m_start.size();
        for (crd::usize i = 0; i < count; ++i)
        {
            (void)add_variable(start, lower, upper);
        }
        return first;
    }

    void set_start(crd::usize i, T value) { m_start[i] = value; }
    void set_bounds(crd::usize i, T lower, T upper)
    {
        CRD_ASSERT_MSG(lower <= upper, "Model::set_bounds: lower > upper");
        m_lo[i] = lower;
        m_up[i] = upper;
    }

    [[nodiscard]] crd::usize num_variables() const noexcept { return m_start.size(); }

    // ---- Objective + constraints (scalar-generic functors; v7-b DiffFunctor contract) -------------------

    template <typename F>
        requires DiffFunctor<F, T>
    void minimize(F f)
    {
        CRD_ASSERT_MSG(m_objective == nullptr, "Model::minimize: objective already set");
        m_objective = crd::memory::construct<detail::ModelFn<T, F>>(*m_alloc, std::move(f));
    }

    template <typename F>
        requires DiffFunctor<F, T>
    void subject_to_eq(F f) // c(x) = 0
    {
        m_eq.push_back(crd::memory::construct<detail::ModelFn<T, F>>(*m_alloc, std::move(f)));
    }

    template <typename F>
        requires DiffFunctor<F, T>
    void subject_to_ge(F f) // c(x) ≥ 0
    {
        m_ineq.push_back(crd::memory::construct<detail::ModelFn<T, F>>(*m_alloc, std::move(f)));
    }

    template <typename F>
        requires DiffFunctor<F, T>
    void subject_to_le(F f) // c(x) ≤ 0, stored negated (the pinned c_I ≥ 0 convention)
    {
        subject_to_ge([g = std::move(f)](const auto& x) { return -g(x); });
    }

    [[nodiscard]] crd::usize num_eq_constraints() const noexcept { return m_eq.size(); }
    [[nodiscard]] crd::usize num_ineq_constraints() const noexcept { return m_ineq.size(); }

    // ---- Solve ------------------------------------------------------------------------------------------

    [[nodiscard]] OptResult<T> solve(const OptOptions<T>& opts, ModelMethod method = ModelMethod::Auto,
                                     const AuglagOptions<T>& aopts = {}) const
    {
        CRD_ASSERT_MSG(m_objective != nullptr, "Model::solve: no objective (call minimize)");
        const crd::usize n = m_start.size();
        const bool has_cons = m_eq.size() + m_ineq.size() > 0;
        bool has_bounds = false;
        for (crd::usize j = 0; j < n; ++j)
        {
            has_bounds = has_bounds || std::isfinite(m_lo[j]) || std::isfinite(m_up[j]);
        }
        if (method == ModelMethod::Auto)
        {
            method = has_cons ? ModelMethod::Sqp : (has_bounds ? ModelMethod::Lbfgsb : ModelMethod::Lbfgs);
        }
        CRD_ASSERT_MSG(!(method == ModelMethod::Lbfgs && (has_cons || has_bounds)),
                       "Model::solve: Lbfgs cannot handle constraints or bounds");
        CRD_ASSERT_MSG(!(method == ModelMethod::Lbfgsb && has_cons),
                       "Model::solve: Lbfgsb handles bounds only, not general constraints");

        const detail::ErasedFnView<T> view{m_objective};
        const FunctorObjective<detail::ErasedFnView<T>, T> obj(view, n, m_alloc);
        const crd::containers::ConstSpan<T> x0{m_start.data(), n};

        if (method == ModelMethod::Lbfgs)
        {
            return minimize_lbfgs<T>(obj, x0, opts, m_alloc);
        }
        if (method == ModelMethod::Lbfgsb)
        {
            return minimize_lbfgsb<T>(obj, x0, {m_lo.data(), n}, {m_up.data(), n}, opts, m_alloc);
        }

        // General path: fold finite bounds into c_I rows (lower ascending, then upper ascending).
        crd::containers::Array<crd::u32> lo_vars(m_alloc);
        crd::containers::Array<T> lo_vals(m_alloc);
        crd::containers::Array<crd::u32> up_vars(m_alloc);
        crd::containers::Array<T> up_vals(m_alloc);
        for (crd::usize j = 0; j < n; ++j)
        {
            if (std::isfinite(m_lo[j]))
            {
                lo_vars.push_back(static_cast<crd::u32>(j));
                lo_vals.push_back(m_lo[j]);
            }
            if (std::isfinite(m_up[j]))
            {
                up_vars.push_back(static_cast<crd::u32>(j));
                up_vals.push_back(m_up[j]);
            }
        }
        const detail::ModelConstraintsAdapter<T> cons(
            {m_eq.data(), m_eq.size()}, {m_ineq.data(), m_ineq.size()}, {lo_vars.data(), lo_vars.size()},
            {lo_vals.data(), lo_vals.size()}, {up_vars.data(), up_vars.size()}, {up_vals.data(), up_vals.size()}, n,
            m_alloc);
        if (method == ModelMethod::Auglag)
        {
            return minimize_auglag<T>(obj, cons, x0, opts, m_alloc, aopts);
        }
        return minimize_sqp<T>(obj, cons, x0, opts, m_alloc);
    }

private:
    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<T> m_start;
    crd::containers::Array<T> m_lo;
    crd::containers::Array<T> m_up;
    detail::IModelFn<T>* m_objective = nullptr;
    crd::containers::Array<detail::IModelFn<T>*> m_eq;
    crd::containers::Array<detail::IModelFn<T>*> m_ineq;
};

} // namespace crd::hesap::opt
