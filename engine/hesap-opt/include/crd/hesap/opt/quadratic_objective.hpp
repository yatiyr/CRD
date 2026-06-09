#pragma once

// quadratic_objective.hpp — Phase 3.1.6 v7-a: f(x) = ½·xᵀ·A·x − bᵀ·x over a (SPD) LinearOp A. ∇f = A·x − b
// (minimizer x* solves A·x = b); Hessian = A (constant) ⇒ Hessian-vector = A·v. The canonical substrate test
// objective AND the determinism-moat vehicle: with A = ParallelSparseLinearOp, the A·x is parallel-but-bit-exact,
// so the optimization trajectory is bit-identical across worker counts. ADR-0090.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::opt
{

template <typename T>
class QuadraticObjective final : public Objective<T>
{
public:
    // `a` must be square SPD (n×n); `b` length n (non-owning — must outlive this object).
    QuadraticObjective(const crd::hesap::LinearOp<T>& a, crd::containers::ConstSpan<T> b,
                       crd::memory::IAllocator* alloc)
        : Objective<T>(/*has_gradient=*/true, /*has_hessian_vector=*/true)
        , m_a(&a)
        , m_b(b)
        , m_ax(alloc)
    {
        m_ax.resize(a.n_rows());
    }

    [[nodiscard]] T value(crd::containers::ConstSpan<T> x) const override
    {
        namespace dn = crd::hesap::dense;
        (void)m_a->apply(x, {m_ax.data(), m_ax.size()}); // m_ax = A·x
        return static_cast<T>(0.5) * dn::dot<T>(x, {m_ax.data(), m_ax.size()}) - dn::dot<T>(m_b, x);
    }

    [[nodiscard]] crd::usize n() const noexcept override { return static_cast<crd::usize>(m_a->n_rows()); }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<T> x, crd::containers::Span<T> g) const override
    {
        (void)m_a->apply(x, g); // g = A·x
        for (crd::usize i = 0; i < g.size(); ++i)
        {
            g[i] -= m_b[i]; // g = A·x − b
        }
        return true;
    }

    [[nodiscard]] bool hessian_vector(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> v,
                                      crd::containers::Span<T> hv) const override
    {
        (void)x; // Hessian = A (constant for a quadratic)
        (void)m_a->apply(v, hv);
        return true;
    }

private:
    const crd::hesap::LinearOp<T>*    m_a;
    crd::containers::ConstSpan<T>     m_b;
    mutable crd::containers::Array<T> m_ax; // A·x scratch (value()); serial use only (the optimizer is serial)
};

} // namespace crd::hesap::opt
