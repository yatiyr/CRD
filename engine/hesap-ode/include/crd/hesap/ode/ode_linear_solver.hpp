#pragma once

// ode_linear_solver.hpp — Phase 3.1.6 v9-d: THE LINEAR-SOLVER SEAM (the ADR-0091 day-1 contract; CVODE's
// SUNLinSol lesson). Stiff drivers (BDF now; Radau/Rosenbrock/SDIRK next) factor and solve iteration
// matrices (I − c·J) through this interface only — the dense implementation below ships with v9-d
// (hesap-dense partial-pivoting LU); the SPARSE (hesap-direct multifrontal `refactorize`) and MATRIX-FREE
// KRYLOV (hesap-iterative GMRES = CVODE SPGMR) implementations slot in at v9-j WITHOUT touching the
// methods. New capabilities append at the END (vtable discipline). ADR-0091.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

template <typename T> class OdeLinearSolver
{
public:
    virtual ~OdeLinearSolver() = default;

    // Factor M = I − c·J (J dense ROW-MAJOR n×n). Returns false if singular. Counted by the driver (nlu).
    [[nodiscard]] virtual bool factor_iteration_matrix(T c, crd::containers::ConstSpan<T> jac, crd::usize n) = 0;

    // Solve M·x = b in place with the LAST factorization. Counted by the driver (nsol).
    virtual void solve(crd::containers::Span<T> b) = 0;

    // v9-h append (vtable END): factor (Mass − c·J) for mass-matrix / index-1 DAE systems (`mass` dense
    // ROW-MAJOR n×n, possibly singular — the ITERATION matrix stays regular for c ≠ 0 when the DAE is
    // index 1). Default = unsupported (Newton-based drivers check the return).
    [[nodiscard]] virtual bool factor_iteration_matrix_mass(T c, crd::containers::ConstSpan<T> jac,
                                                            crd::containers::ConstSpan<T> mass, crd::usize n)
    {
        (void)c;
        (void)jac;
        (void)mass;
        (void)n;
        return false;
    }

    // v9-j append (vtable END): factor (I − c·J) from a SPARSE CSR Jacobian — the large-n MOL path
    // (implemented by SparseOdeLinearSolver over the hesap-direct multifrontal LU; CVODE-KLU's role).
    // Default = unsupported.
    [[nodiscard]] virtual bool factor_iteration_matrix_sparse(
        T c, const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& jac)
    {
        (void)c;
        (void)jac;
        return false;
    }
};

// The v9-d dense implementation: hesap-dense partial-pivoting LU.
template <typename T> class DenseOdeLinearSolver final : public OdeLinearSolver<T>
{
public:
    explicit DenseOdeLinearSolver(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_m(alloc), m_lu(alloc) {}

    [[nodiscard]] bool factor_iteration_matrix(T c, crd::containers::ConstSpan<T> jac, crd::usize n) override
    {
        CRD_ASSERT(jac.size() == n * n);
        if (m_m.rows() != n)
        {
            m_m = dense::Matrix<T, dense::Layout::RowMajor>(m_alloc, n, n);
            m_lu = dense::LU<T, dense::Layout::RowMajor>(m_alloc, n);
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                const T iden = (i == j) ? static_cast<T>(1) : static_cast<T>(0);
                m_m.at(i, j) = iden - c * jac[i * n + j];
            }
        }
        dense::factor_lu(m_lu, m_m);
        return m_lu.info() == 0;
    }

    void solve(crd::containers::Span<T> b) override { dense::solve_lu(m_lu, b); }

    [[nodiscard]] bool factor_iteration_matrix_mass(T c, crd::containers::ConstSpan<T> jac,
                                                    crd::containers::ConstSpan<T> mass, crd::usize n) override
    {
        CRD_ASSERT(jac.size() == n * n && mass.size() == n * n);
        if (m_m.rows() != n)
        {
            m_m = dense::Matrix<T, dense::Layout::RowMajor>(m_alloc, n, n);
            m_lu = dense::LU<T, dense::Layout::RowMajor>(m_alloc, n);
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                m_m.at(i, j) = mass[i * n + j] - c * jac[i * n + j];
            }
        }
        dense::factor_lu(m_lu, m_m);
        return m_lu.info() == 0;
    }

private:
    crd::memory::IAllocator* m_alloc;
    dense::Matrix<T, dense::Layout::RowMajor> m_m;
    dense::LU<T, dense::Layout::RowMajor> m_lu;
};

} // namespace crd::hesap::ode
