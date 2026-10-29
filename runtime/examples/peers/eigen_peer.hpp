#pragma once

// The Eigen 3.4 peer of the vs-reference boards (ADR-0134 §8). Eigen is compiled in a shared library of its own at the
// measuring host's best ISA (crd_add_bench_peer), apart from the bench units, which build at the baseline like all
// Cerid code; no Eigen type and no inline body compiled at the peer's ISA reaches them. Plain data crosses: an object
// is a handle the library owns (Eigen's own aligned storage, filled from the caller's buffer outside the timed
// region), an operation is one exported call holding the Eigen expression the board measures, and the bench times
// both sides with one loop and validates the peer's results from data().

#include "peer_api.hpp"

#include <cstdint>

namespace crd_peer::eigen
{
using Index = std::int64_t;

struct MatF32; // Eigen::MatrixXf, column-major
struct MatF64; // Eigen::MatrixXd
struct MatC64; // Eigen::MatrixXcd, re/im interleaved
struct VecF32; // Eigen::VectorXf
struct VecF64; // Eigen::VectorXd
struct LltF64; // Eigen::LLT<MatrixXd>
struct SymEig; // Eigen::SelfAdjointEigenSolver<MatrixXd>, reused across calls

// The build: the Eigen version, the vector paths it compiled and the library's ISA flags.
CRD_BENCH_PEER_API const char* isa() noexcept;
// Eigen's thread count (Eigen::setNbThreads, 0 restores its default; Eigen::initParallel is called once).
CRD_BENCH_PEER_API void set_threads(int count) noexcept;
CRD_BENCH_PEER_API int threads() noexcept;

// Objects. `data` holds rows x cols values, row-major when `row_major` and column-major otherwise (complex values as
// re/im pairs); nullptr zero-fills. data() is the column-major storage.
CRD_BENCH_PEER_API MatF32* mat(Index rows, Index cols, const float* data, bool row_major);
CRD_BENCH_PEER_API MatF64* mat(Index rows, Index cols, const double* data, bool row_major);
CRD_BENCH_PEER_API MatC64* mat_c64(Index rows, Index cols, const double* data, bool row_major);
CRD_BENCH_PEER_API VecF32* vec(Index size, const float* data);
CRD_BENCH_PEER_API VecF64* vec(Index size, const double* data);
CRD_BENCH_PEER_API SymEig* sym_eig();
CRD_BENCH_PEER_API void destroy(MatF32* object) noexcept;
CRD_BENCH_PEER_API void destroy(MatF64* object) noexcept;
CRD_BENCH_PEER_API void destroy(MatC64* object) noexcept;
CRD_BENCH_PEER_API void destroy(VecF32* object) noexcept;
CRD_BENCH_PEER_API void destroy(VecF64* object) noexcept;
CRD_BENCH_PEER_API void destroy(LltF64* object) noexcept;
CRD_BENCH_PEER_API void destroy(SymEig* object) noexcept;
CRD_BENCH_PEER_API const float* data(const MatF32* m) noexcept;
CRD_BENCH_PEER_API const double* data(const MatF64* m) noexcept;
CRD_BENCH_PEER_API const float* data(const VecF32* v) noexcept;
CRD_BENCH_PEER_API const double* data(const VecF64* v) noexcept;
CRD_BENCH_PEER_API Index size(const VecF64* v) noexcept;

// A handle owned by the caller's scope.
template <typename T> class Owned
{
public:
    explicit Owned(T* object) noexcept : m_object(object) {}
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { destroy(m_object); }
    [[nodiscard]] T* get() const noexcept { return m_object; }

private:
    T* m_object;
};

// ---- Dense BLAS --------------------------------------------------------------------------------------------------
// c.noalias() = a * b
CRD_BENCH_PEER_API void gemm(const MatF32* a, const MatF32* b, MatF32* c);
CRD_BENCH_PEER_API void gemm(const MatF64* a, const MatF64* b, MatF64* c);
// y.noalias() += alpha * x
CRD_BENCH_PEER_API void axpy(double alpha, const VecF64* x, VecF64* y);
CRD_BENCH_PEER_API float dot(const VecF32* x, const VecF32* y);
CRD_BENCH_PEER_API double dot(const VecF64* x, const VecF64* y);
CRD_BENCH_PEER_API float norm(const VecF32* x);
CRD_BENCH_PEER_API double norm(const VecF64* x);
// y.noalias() = alpha * a * x + beta * y, with a.selfadjointView<Lower>() for symv_lower
CRD_BENCH_PEER_API void gemv(float alpha, const MatF32* a, const VecF32* x, float beta, VecF32* y);
CRD_BENCH_PEER_API void gemv(double alpha, const MatF64* a, const VecF64* x, double beta, VecF64* y);
CRD_BENCH_PEER_API void symv_lower(float alpha, const MatF32* a, const VecF32* x, float beta, VecF32* y);
CRD_BENCH_PEER_API void symv_lower(double alpha, const MatF64* a, const VecF64* x, double beta, VecF64* y);
// b = b0, then l.triangularView<Lower>().solveInPlace(b)
CRD_BENCH_PEER_API void trsv_lower(const MatF32* l, const VecF32* b0, VecF32* b);
CRD_BENCH_PEER_API void trsv_lower(const MatF64* l, const VecF64* b0, VecF64* b);

// ---- Dense factorizations ----------------------------------------------------------------------------------------
enum class Solve : std::uint8_t
{
    PartialPivLu,       // PartialPivLU
    Llt,                // LLT
    Ldlt,               // LDLT
    HouseholderQr,      // HouseholderQR (least squares when a is tall)
    CompleteOrthogonal, // CompleteOrthogonalDecomposition
    BdcSvd,             // BDCSVD with the thin U and V
};
// x = <decomposition of a>.solve(b), the decomposition built in the call
CRD_BENCH_PEER_API void solve(Solve method, const MatF64* a, const VecF64* b, VecF64* x);
CRD_BENCH_PEER_API LltF64* llt(const MatF64* a);
// x = llt.solve(b)
CRD_BENCH_PEER_API void solve(const LltF64* llt, const MatF64* b, MatF64* x);
// p = a.completeOrthogonalDecomposition().pseudoInverse()
CRD_BENCH_PEER_API void pseudo_inverse(const MatF64* a, MatF64* p);
// HouseholderQR<MatrixXd>(a), returning matrixQR()(0, 0)
CRD_BENCH_PEER_API double householder_qr(const MatF64* a);

enum class Svd : std::uint8_t
{
    Jacobi, // JacobiSVD
    Bdc,    // BDCSVD
};
// The singular values of a, with the thin U and V when `thin_uv` (values may be nullptr)
CRD_BENCH_PEER_API void svd(Svd method, const MatF64* a, bool thin_uv, VecF64* values);

// SelfAdjointEigenSolver<MatrixXd>(a, vectors ? ComputeEigenvectors : EigenvaluesOnly).eigenvalues()
CRD_BENCH_PEER_API void eig_sym(const MatF64* a, bool vectors, VecF64* values);
// solver.computeFromTridiagonal(diag[0, n), sub[0, n - 1), ComputeEigenvectors), the vectors copied in the call
CRD_BENCH_PEER_API void eig_tridiagonal(SymEig* solver, Index n, const double* diag, const double* sub);
// SelfAdjointEigenSolver<MatrixXcd>(a).eigenvalues()
CRD_BENCH_PEER_API void eig_herm(const MatC64* a, VecF64* values);
// HessenbergDecomposition(a), returning the real part of matrixH()(0, 0)
CRD_BENCH_PEER_API double hessenberg(const MatF64* a);
CRD_BENCH_PEER_API double hessenberg(const MatC64* a);
// RealSchur / ComplexSchur computeFromHessenberg(h, Identity, true), returning the real part of matrixT()(0, 0)
CRD_BENCH_PEER_API double schur_from_hessenberg(const MatF64* h);
CRD_BENCH_PEER_API double schur_from_hessenberg(const MatC64* h);
// EigenSolver / ComplexEigenSolver(a, true), returning the real part of eigenvalues()(0)
CRD_BENCH_PEER_API double eig_general(const MatF64* a);
CRD_BENCH_PEER_API double eig_general(const MatC64* a);
} // namespace crd_peer::eigen
