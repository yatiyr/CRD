#pragma once

// The Eigen peer's objects (eigen_peer.hpp), shared by the peer library's units only: no bench unit includes it.

#include "eigen_peer.hpp"

#include <Eigen/Dense>

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

} // namespace crd_peer::eigen
