// The Eigen peer's sparse direct solvers (eigen_peer.hpp).

#include "eigen_peer_objects.hpp"

#include <Eigen/OrderingMethods>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>
#include <Eigen/SparseQR>

namespace crd_peer::eigen
{
using SparseColumns = Eigen::SparseMatrix<double>;
static_assert(sizeof(SparseColumns::StorageIndex) == sizeof(std::int32_t));

struct SpSolver
{
    SpSolver() = default;
    SpSolver(const SpSolver&) = delete;
    SpSolver& operator=(const SpSolver&) = delete;
    SpSolver(SpSolver&&) = delete;
    SpSolver& operator=(SpSolver&&) = delete;
    virtual ~SpSolver() = default;
    virtual void analyze(const SparseColumns& a) = 0;
    virtual bool factorize(const SparseColumns& a) = 0;
    virtual void solve(const Eigen::VectorXd& b, Eigen::VectorXd& x) const = 0;
    virtual void solve(const Eigen::MatrixXd& b, Eigen::MatrixXd& x) const = 0;
    [[nodiscard]] virtual Index factor_nnz() const = 0;
    // matrixL() materialised, or nullptr for a decomposition without one
    virtual const SparseColumns* factor_l() = 0;
};

namespace
{
template <typename Decomposition> constexpr bool kCholesky = false;
template <typename Ordering>
constexpr bool kCholesky<Eigen::SimplicialLLT<SparseColumns, Eigen::Lower, Ordering>> = true;
template <typename Ordering>
constexpr bool kCholesky<Eigen::SimplicialLDLT<SparseColumns, Eigen::Lower, Ordering>> = true;
template <typename Decomposition> constexpr bool kLu = false;
template <typename Ordering> constexpr bool kLu<Eigen::SparseLU<SparseColumns, Ordering>> = true;

template <typename Decomposition> class Solver final : public SpSolver
{
public:
    void analyze(const SparseColumns& a) override { m_decomposition.analyzePattern(a); }

    bool factorize(const SparseColumns& a) override
    {
        m_decomposition.analyzePattern(a);
        m_decomposition.factorize(a);
        m_l.resize(0, 0);
        return m_decomposition.info() == Eigen::Success;
    }

    void solve(const Eigen::VectorXd& b, Eigen::VectorXd& x) const override { x = m_decomposition.solve(b); }

    void solve(const Eigen::MatrixXd& b, Eigen::MatrixXd& x) const override { x = m_decomposition.solve(b); }

    [[nodiscard]] Index factor_nnz() const override
    {
        if constexpr (kCholesky<Decomposition>)
        {
            const SparseColumns l = m_decomposition.matrixL();
            return l.nonZeros();
        }
        else if constexpr (kLu<Decomposition>)
        {
            return static_cast<Index>(m_decomposition.nnzL()) + static_cast<Index>(m_decomposition.nnzU());
        }
        else
        {
            return m_decomposition.matrixR().nonZeros();
        }
    }

    const SparseColumns* factor_l() override
    {
        if constexpr (kCholesky<Decomposition>)
        {
            m_l = m_decomposition.matrixL();
            return &m_l;
        }
        else
        {
            return nullptr;
        }
    }

private:
    Decomposition m_decomposition;
    SparseColumns m_l;
};

using Natural = Eigen::NaturalOrdering<int>;
} // namespace

SpSolver* sparse_solver(SparseSolve method)
{
    switch (method)
    {
        case SparseSolve::LltNatural:
            return new Solver<Eigen::SimplicialLLT<SparseColumns, Eigen::Lower, Natural>>;
        case SparseSolve::LltAmd:
            return new Solver<Eigen::SimplicialLLT<SparseColumns, Eigen::Lower, Eigen::AMDOrdering<int>>>;
        case SparseSolve::LdltNatural:
            return new Solver<Eigen::SimplicialLDLT<SparseColumns, Eigen::Lower, Natural>>;
        case SparseSolve::LuNatural:
            return new Solver<Eigen::SparseLU<SparseColumns, Natural>>;
        case SparseSolve::LuColamd:
            return new Solver<Eigen::SparseLU<SparseColumns, Eigen::COLAMDOrdering<int>>>;
        case SparseSolve::QrNatural:
            return new Solver<Eigen::SparseQR<SparseColumns, Natural>>;
    }
    return nullptr;
}

void destroy(SpSolver* object) noexcept
{
    delete object;
}

void analyze(SpSolver* solver, const SpColF64* a)
{
    solver->analyze(a->a);
}

bool factorize(SpSolver* solver, const SpColF64* a)
{
    return solver->factorize(a->a);
}

void solve(const SpSolver* solver, const VecF64* b, VecF64* x)
{
    solver->solve(b->v, x->v);
}

void solve(const SpSolver* solver, const MatF64* b, MatF64* x)
{
    solver->solve(b->m, x->m);
}

Index factor_nnz(const SpSolver* solver)
{
    return solver->factor_nnz();
}

bool factor_pattern(SpSolver* solver, const std::int32_t** outer, const std::int32_t** inner)
{
    const SparseColumns* l = solver->factor_l();
    if (l == nullptr)
    {
        return false;
    }
    *outer = l->outerIndexPtr();
    *inner = l->innerIndexPtr();
    return true;
}
} // namespace crd_peer::eigen
