// The Eigen peer's sparse iterative solvers (eigen_peer.hpp).

#include "eigen_peer_objects.hpp"

#include <Eigen/IterativeLinearSolvers>
#include <algorithm>
#include <cstdlib>
#include <type_traits>
#include <unsupported/Eigen/IterativeSolvers>

namespace crd_peer::eigen
{
using SparseColumns = Eigen::SparseMatrix<double>;

struct SpIterative
{
    SpIterative() = default;
    SpIterative(const SpIterative&) = delete;
    SpIterative& operator=(const SpIterative&) = delete;
    SpIterative(SpIterative&&) = delete;
    SpIterative& operator=(SpIterative&&) = delete;
    virtual ~SpIterative() = default;
    virtual bool compute(const SparseColumns& a) = 0;
    virtual IterativeResult solve(const Eigen::VectorXd& b, Eigen::VectorXd& x) const = 0;
    virtual IterativeResult solve(const Eigen::MatrixXd& b, Eigen::MatrixXd& x) const = 0;
};

namespace
{
// One shadow space for every IDRS solve, so each repetition measures the same iteration.
constexpr unsigned kIdrsShadowSeed = 1U;

using Diagonal = Eigen::DiagonalPreconditioner<double>;
using Identity = Eigen::IdentityPreconditioner;
using Ilut = Eigen::IncompleteLUT<double>;
constexpr int kBothTriangles = Eigen::Lower | Eigen::Upper;

template <typename Solver> constexpr bool kGmres = false;
template <typename Preconditioner> constexpr bool kGmres<Eigen::GMRES<SparseColumns, Preconditioner>> = true;
template <typename Solver> constexpr bool kIdrs = false;
template <typename Preconditioner> constexpr bool kIdrs<Eigen::IDRS<SparseColumns, Preconditioner>> = true;

template <typename Preconditioner> void configure(Preconditioner& preconditioner, const IterativeSettings& settings)
{
    if constexpr (std::is_same_v<Preconditioner, Ilut>)
    {
        preconditioner.setDroptol(settings.drop_tolerance);
        preconditioner.setFillfactor(settings.fill_factor);
    }
    else
    {
        (void)preconditioner;
        (void)settings;
    }
}

template <typename Solver> class Iterative final : public SpIterative
{
public:
    explicit Iterative(const IterativeSettings& settings)
    {
        m_solver.setMaxIterations(settings.max_iterations);
        m_solver.setTolerance(settings.tolerance);
        if constexpr (kGmres<Solver>)
        {
            m_solver.set_restart(settings.restart);
        }
        if constexpr (kIdrs<Solver>)
        {
            m_solver.setS(settings.s);
        }
        configure(m_solver.preconditioner(), settings);
    }

    bool compute(const SparseColumns& a) override
    {
        m_solver.compute(a);
        m_ready = m_solver.info() == Eigen::Success;
        return m_ready;
    }

    IterativeResult solve(const Eigen::VectorXd& b, Eigen::VectorXd& x) const override
    {
        seed();
        x = m_solver.solve(b);
        return {m_solver.iterations(), m_ready && m_solver.info() == Eigen::Success};
    }

    IterativeResult solve(const Eigen::MatrixXd& b, Eigen::MatrixXd& x) const override
    {
        IterativeResult result{0, m_ready};
        for (Index j = 0; j < b.cols(); ++j)
        {
            seed();
            const Eigen::VectorXd column = m_solver.solve(b.col(j));
            x.col(j) = column;
            result.iterations += m_solver.iterations();
            result.converged = result.converged && m_solver.info() == Eigen::Success;
        }
        return result;
    }

private:
    static void seed()
    {
        if constexpr (kIdrs<Solver>)
        {
            std::srand(kIdrsShadowSeed); // NOLINT(cert-msc32-c,cert-msc51-cpp): a fixed seed is the point
        }
    }

    Solver m_solver;
    bool m_ready = false;
};

template <typename Solver> SpIterative* make(const IterativeSettings& settings)
{
    return new Iterative<Solver>(settings);
}

template <typename Preconditioner> bool build(const IterativeSettings& settings, const SparseColumns& a)
{
    Preconditioner preconditioner;
    configure(preconditioner, settings);
    preconditioner.compute(a);
    return preconditioner.info() == Eigen::Success;
}
} // namespace

SpIterative* iterative_solver(IterativeSolve method, const IterativeSettings& settings)
{
    switch (method)
    {
        case IterativeSolve::CgDiagonal:
            return make<Eigen::ConjugateGradient<SparseColumns, kBothTriangles, Diagonal>>(settings);
        case IterativeSolve::CgIncompleteCholesky:
            return make<Eigen::ConjugateGradient<SparseColumns, kBothTriangles, Eigen::IncompleteCholesky<double>>>(
                settings);
        case IterativeSolve::MinresDiagonal:
            return make<Eigen::MINRES<SparseColumns, kBothTriangles, Diagonal>>(settings);
        case IterativeSolve::BicgstabIdentity:
            return make<Eigen::BiCGSTAB<SparseColumns, Identity>>(settings);
        case IterativeSolve::BicgstabDiagonal:
            return make<Eigen::BiCGSTAB<SparseColumns, Diagonal>>(settings);
        case IterativeSolve::BicgstabIlut:
            return make<Eigen::BiCGSTAB<SparseColumns, Ilut>>(settings);
        case IterativeSolve::GmresIdentity:
            return make<Eigen::GMRES<SparseColumns, Identity>>(settings);
        case IterativeSolve::GmresDiagonal:
            return make<Eigen::GMRES<SparseColumns, Diagonal>>(settings);
        case IterativeSolve::GmresIlut:
            return make<Eigen::GMRES<SparseColumns, Ilut>>(settings);
        case IterativeSolve::IdrsIdentity:
            return make<Eigen::IDRS<SparseColumns, Identity>>(settings);
        case IterativeSolve::IdrsDiagonal:
            return make<Eigen::IDRS<SparseColumns, Diagonal>>(settings);
    }
    return nullptr;
}

void destroy(SpIterative* object) noexcept
{
    delete object;
}

bool compute(SpIterative* solver, const SpColF64* a)
{
    return solver->compute(a->a);
}

IterativeResult solve(const SpIterative* solver, const VecF64* b, VecF64* x)
{
    return solver->solve(b->v, x->v);
}

IterativeResult solve(const SpIterative* solver, const MatF64* b, MatF64* x)
{
    return solver->solve(b->m, x->m);
}

double relative_residual(const SpColF64* a, const VecF64* b, const VecF64* x)
{
    return (b->v - a->a * x->v).norm() / b->v.norm();
}

double relative_residual(const SpColF64* a, const MatF64* b, const MatF64* x)
{
    double worst = 0.0;
    for (Index j = 0; j < b->m.cols(); ++j)
    {
        worst = std::max(worst, (b->m.col(j) - a->a * x->m.col(j)).norm() / b->m.col(j).norm());
    }
    return worst;
}

bool build_preconditioner(IterativeSolve method, const IterativeSettings& settings, const SpColF64* a)
{
    switch (method)
    {
        case IterativeSolve::CgIncompleteCholesky:
            return build<Eigen::IncompleteCholesky<double>>(settings, a->a);
        case IterativeSolve::BicgstabIlut:
        case IterativeSolve::GmresIlut:
            return build<Ilut>(settings, a->a);
        case IterativeSolve::CgDiagonal:
        case IterativeSolve::MinresDiagonal:
        case IterativeSolve::BicgstabDiagonal:
        case IterativeSolve::GmresDiagonal:
        case IterativeSolve::IdrsDiagonal:
            return build<Diagonal>(settings, a->a);
        case IterativeSolve::BicgstabIdentity:
        case IterativeSolve::GmresIdentity:
        case IterativeSolve::IdrsIdentity:
            return build<Identity>(settings, a->a);
    }
    return false;
}
} // namespace crd_peer::eigen
