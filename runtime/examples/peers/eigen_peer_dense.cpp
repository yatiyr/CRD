// The Eigen peer's objects and dense operations (eigen_peer.hpp).

#include "eigen_peer.hpp"

#include <Eigen/Dense>
#include <array>
#include <complex>
#include <cstdio>

// The library is built at the host's best ISA; a vector path Eigen did not take would measure a weaker peer.
#if defined(__AVX2__) && !(defined(EIGEN_VECTORIZE_AVX2) && defined(EIGEN_VECTORIZE_FMA))
#error "the Eigen peer is built for AVX2 but Eigen does not vectorize with AVX2 and FMA"
#endif
#if defined(__AVX512F__) && !defined(EIGEN_VECTORIZE_AVX512)
#error "the Eigen peer is built for AVX-512 but Eigen does not vectorize with it"
#endif

namespace crd_peer::eigen
{
struct MatF32
{
    Eigen::MatrixXf m;
};
struct MatF64
{
    Eigen::MatrixXd m;
};
struct MatC64
{
    Eigen::MatrixXcd m;
};
struct VecF32
{
    Eigen::VectorXf v;
};
struct VecF64
{
    Eigen::VectorXd v;
};
struct LltF64
{
    Eigen::LLT<Eigen::MatrixXd> llt;
};
struct SymEig
{
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver;
    Eigen::VectorXd diag;
    Eigen::VectorXd sub;
};

namespace
{
// The vector paths Eigen compiled (its SimdInstructionSetsInUse() names an AVX2 build "AVX").
constexpr const char* kSimd =
#if defined(EIGEN_VECTORIZE_AVX512)
    "AVX-512, AVX2, FMA";
#elif defined(EIGEN_VECTORIZE_AVX2) && defined(EIGEN_VECTORIZE_FMA)
    "AVX2, FMA";
#elif defined(EIGEN_VECTORIZE_AVX)
    "AVX";
#elif defined(EIGEN_VECTORIZE_SSE4_2)
    "SSE4.2";
#elif defined(EIGEN_VECTORIZE_NEON)
    "NEON";
#elif defined(EIGEN_VECTORIZE_SSE2)
    "SSE2";
#else
    "no vectorization";
#endif

template <typename Matrix, typename Scalar>
void fill(Matrix& m, Eigen::Index rows, Eigen::Index cols, const Scalar* data, bool row_major)
{
    m.resize(rows, cols);
    if (data == nullptr)
    {
        m.setZero();
        return;
    }
    for (Eigen::Index j = 0; j < cols; ++j)
    {
        for (Eigen::Index i = 0; i < rows; ++i)
        {
            m(i, j) = data[row_major ? i * cols + j : j * rows + i];
        }
    }
}

template <typename Vector, typename Scalar> void fill(Vector& v, Eigen::Index size, const Scalar* data)
{
    v.resize(size);
    if (data == nullptr)
    {
        v.setZero();
        return;
    }
    for (Eigen::Index i = 0; i < size; ++i)
    {
        v(i) = data[i];
    }
}
} // namespace

const char* isa() noexcept
{
    static const std::array<char, 160> kText = []()
    {
        std::array<char, 160> text{};
        (void)std::snprintf(text.data(), text.size(), "Eigen %d.%d.%d, %s (%s)", EIGEN_WORLD_VERSION,
                            EIGEN_MAJOR_VERSION, EIGEN_MINOR_VERSION, kSimd, CRD_BENCH_PEER_ISA);
        return text;
    }();
    return kText.data();
}

void set_threads(int count) noexcept
{
    static const bool kInitialized = []()
    {
        Eigen::initParallel();
        return true;
    }();
    (void)kInitialized;
    Eigen::setNbThreads(count);
}

int threads() noexcept
{
    return Eigen::nbThreads();
}

MatF32* mat(Index rows, Index cols, const float* data, bool row_major)
{
    auto* object = new MatF32;
    fill(object->m, rows, cols, data, row_major);
    return object;
}

MatF64* mat(Index rows, Index cols, const double* data, bool row_major)
{
    auto* object = new MatF64;
    fill(object->m, rows, cols, data, row_major);
    return object;
}

MatC64* mat_c64(Index rows, Index cols, const double* data, bool row_major)
{
    auto* object = new MatC64;
    fill(object->m, rows, cols, reinterpret_cast<const std::complex<double>*>(data), row_major);
    return object;
}

VecF32* vec(Index size, const float* data)
{
    auto* object = new VecF32;
    fill(object->v, size, data);
    return object;
}

VecF64* vec(Index size, const double* data)
{
    auto* object = new VecF64;
    fill(object->v, size, data);
    return object;
}

SymEig* sym_eig()
{
    return new SymEig;
}

void destroy(MatF32* object) noexcept
{
    delete object;
}

void destroy(MatF64* object) noexcept
{
    delete object;
}

void destroy(MatC64* object) noexcept
{
    delete object;
}

void destroy(VecF32* object) noexcept
{
    delete object;
}

void destroy(VecF64* object) noexcept
{
    delete object;
}

void destroy(LltF64* object) noexcept
{
    delete object;
}

void destroy(SymEig* object) noexcept
{
    delete object;
}

const float* data(const MatF32* m) noexcept
{
    return m->m.data();
}

const double* data(const MatF64* m) noexcept
{
    return m->m.data();
}

const float* data(const VecF32* v) noexcept
{
    return v->v.data();
}

const double* data(const VecF64* v) noexcept
{
    return v->v.data();
}

Index size(const VecF64* v) noexcept
{
    return v->v.size();
}

void gemm(const MatF32* a, const MatF32* b, MatF32* c)
{
    c->m.noalias() = a->m * b->m;
}

void gemm(const MatF64* a, const MatF64* b, MatF64* c)
{
    c->m.noalias() = a->m * b->m;
}

void axpy(double alpha, const VecF64* x, VecF64* y)
{
    y->v.noalias() += alpha * x->v;
}

float dot(const VecF32* x, const VecF32* y)
{
    return x->v.dot(y->v);
}

double dot(const VecF64* x, const VecF64* y)
{
    return x->v.dot(y->v);
}

float norm(const VecF32* x)
{
    return x->v.norm();
}

double norm(const VecF64* x)
{
    return x->v.norm();
}

void gemv(float alpha, const MatF32* a, const VecF32* x, float beta, VecF32* y)
{
    y->v.noalias() = alpha * a->m * x->v + beta * y->v;
}

void gemv(double alpha, const MatF64* a, const VecF64* x, double beta, VecF64* y)
{
    y->v.noalias() = alpha * a->m * x->v + beta * y->v;
}

void symv_lower(float alpha, const MatF32* a, const VecF32* x, float beta, VecF32* y)
{
    y->v.noalias() = alpha * a->m.selfadjointView<Eigen::Lower>() * x->v + beta * y->v;
}

void symv_lower(double alpha, const MatF64* a, const VecF64* x, double beta, VecF64* y)
{
    y->v.noalias() = alpha * a->m.selfadjointView<Eigen::Lower>() * x->v + beta * y->v;
}

void trsv_lower(const MatF32* l, const VecF32* b0, VecF32* b)
{
    b->v = b0->v;
    l->m.triangularView<Eigen::Lower>().solveInPlace(b->v);
}

void trsv_lower(const MatF64* l, const VecF64* b0, VecF64* b)
{
    b->v = b0->v;
    l->m.triangularView<Eigen::Lower>().solveInPlace(b->v);
}

void solve(Solve method, const MatF64* a, const VecF64* b, VecF64* x)
{
    switch (method)
    {
        case Solve::PartialPivLu:
            x->v = Eigen::PartialPivLU<Eigen::MatrixXd>(a->m).solve(b->v);
            break;
        case Solve::Llt:
            x->v = Eigen::LLT<Eigen::MatrixXd>(a->m).solve(b->v);
            break;
        case Solve::Ldlt:
            x->v = Eigen::LDLT<Eigen::MatrixXd>(a->m).solve(b->v);
            break;
        case Solve::HouseholderQr:
            x->v = Eigen::HouseholderQR<Eigen::MatrixXd>(a->m).solve(b->v);
            break;
        case Solve::CompleteOrthogonal:
            x->v = Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(a->m).solve(b->v);
            break;
        case Solve::BdcSvd:
            x->v = Eigen::BDCSVD<Eigen::MatrixXd>(a->m, Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b->v);
            break;
    }
}

LltF64* llt(const MatF64* a)
{
    return new LltF64{Eigen::LLT<Eigen::MatrixXd>(a->m)};
}

void solve(const LltF64* llt, const MatF64* b, MatF64* x)
{
    x->m = llt->llt.solve(b->m);
}

void pseudo_inverse(const MatF64* a, MatF64* p)
{
    p->m = a->m.completeOrthogonalDecomposition().pseudoInverse();
}

double householder_qr(const MatF64* a)
{
    const Eigen::HouseholderQR<Eigen::MatrixXd> qr(a->m);
    return qr.matrixQR()(0, 0);
}

void svd(Svd method, const MatF64* a, bool thin_uv, VecF64* values)
{
    const unsigned int options = thin_uv ? (Eigen::ComputeThinU | Eigen::ComputeThinV) : 0U;
    if (method == Svd::Jacobi)
    {
        const Eigen::JacobiSVD<Eigen::MatrixXd> s(a->m, options);
        if (values != nullptr)
        {
            values->v = s.singularValues();
        }
        return;
    }
    const Eigen::BDCSVD<Eigen::MatrixXd> s(a->m, options);
    if (values != nullptr)
    {
        values->v = s.singularValues();
    }
}

void eig_sym(const MatF64* a, bool vectors, VecF64* values)
{
    const int options = vectors ? Eigen::ComputeEigenvectors : Eigen::EigenvaluesOnly;
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(a->m, options);
    values->v = es.eigenvalues();
}

void eig_tridiagonal(SymEig* solver, Index n, const double* diag, const double* sub)
{
    fill(solver->diag, n, diag);
    fill(solver->sub, n - 1, sub);
    solver->solver.computeFromTridiagonal(solver->diag, solver->sub, Eigen::ComputeEigenvectors);
}

void eig_herm(const MatC64* a, VecF64* values)
{
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(a->m);
    values->v = es.eigenvalues();
}

double hessenberg(const MatF64* a)
{
    const Eigen::HessenbergDecomposition<Eigen::MatrixXd> h(a->m);
    return h.matrixH()(0, 0);
}

double hessenberg(const MatC64* a)
{
    const Eigen::HessenbergDecomposition<Eigen::MatrixXcd> h(a->m);
    return h.matrixH()(0, 0).real();
}

double schur_from_hessenberg(const MatF64* h)
{
    Eigen::RealSchur<Eigen::MatrixXd> rs;
    rs.computeFromHessenberg(h->m, Eigen::MatrixXd::Identity(h->m.rows(), h->m.cols()), true);
    return rs.matrixT()(0, 0);
}

double schur_from_hessenberg(const MatC64* h)
{
    Eigen::ComplexSchur<Eigen::MatrixXcd> cs;
    cs.computeFromHessenberg(h->m, Eigen::MatrixXcd::Identity(h->m.rows(), h->m.cols()), true);
    return cs.matrixT()(0, 0).real();
}

double eig_general(const MatF64* a)
{
    const Eigen::EigenSolver<Eigen::MatrixXd> es(a->m, true);
    return es.eigenvalues()(0).real();
}

double eig_general(const MatC64* a)
{
    const Eigen::ComplexEigenSolver<Eigen::MatrixXcd> es(a->m, true);
    return es.eigenvalues()(0).real();
}
} // namespace crd_peer::eigen
