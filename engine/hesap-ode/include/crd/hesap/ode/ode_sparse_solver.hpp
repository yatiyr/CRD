#pragma once

// ode_sparse_solver.hpp — Phase 3.1.6 v9-j: the SPARSE implementation of the OdeLinearSolver seam —
// (I − c·J) assembled in CSR and factored by the v5b hesap-direct MULTIFRONTAL LU (static-pivot,
// deterministic, the {1..16}-moat-proven factorization). This is what makes large-n method-of-lines
// stiff systems tractable (dense LU is O(n³) per refactor — n = 10⁴ is out of reach; sparse is the
// CVODE-KLU role). First cut factors symbolic+numeric per call (the symbolic-reuse `refactorize` lever
// is named — the BDF Jacobian-reuse policy already keeps nlu small). Workers fixed at 1 (serial; the
// multifrontal's own {1..16} bit-identity proof makes a worker knob a later free win). ADR-0091.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/ode/ode_linear_solver.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

template <typename T> class SparseOdeLinearSolver final : public OdeLinearSolver<T>
{
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

public:
    explicit SparseOdeLinearSolver(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    // The dense entry points are deliberately unsupported (use DenseOdeLinearSolver for dense systems).
    [[nodiscard]] bool factor_iteration_matrix(T, crd::containers::ConstSpan<T>, crd::usize) override
    {
        return false;
    }

    [[nodiscard]] bool factor_iteration_matrix_sparse(T c, const Csr& jac) override
    {
        const crd::u32 n = static_cast<crd::u32>(jac.rows());
        // Assemble M = I − c·J (diagonal union handled by the triplet builder's accumulation).
        crd::hesap::sparse::TripletBuilder<T> tb(m_alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            tb.add(i, i, static_cast<T>(1));
        }
        const auto& pat = jac.pattern();
        const auto& vals = jac.values();
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::usize lo = pat.outer_ptr[i];
            const crd::usize hi = pat.outer_ptr[i + 1];
            for (crd::usize k = lo; k < hi; ++k)
            {
                tb.add(i, pat.inner_idx[k], -c * vals.values[k]);
            }
        }
        Csr m = tb.compress();
        m_lu.factorize(m, /*num_workers*/ 1);
        m_valid = true;
        return true;
    }

    void solve(crd::containers::Span<T> b) override
    {
        CRD_ASSERT(m_valid);
        const bool ok = m_lu.solve(b, 1);
        CRD_ASSERT(ok);
        (void)ok;
    }

private:
    crd::memory::IAllocator* m_alloc;
    direct::MultifrontalLU<T> m_lu{m_alloc};
    bool m_valid = false;
};

} // namespace crd::hesap::ode
