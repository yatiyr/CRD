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
using crd_peer::Owned;
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

// ---- Sparse -------------------------------------------------------------------------------------------------------
// One coordinate of a matrix; repeated coordinates are summed (setFromTriplets).
struct Entry
{
    std::int32_t row;
    std::int32_t col;
    double value;
};
struct SpF32;     // Eigen::SparseMatrix<float, RowMajor>
struct SpF64;     // Eigen::SparseMatrix<double, RowMajor>
struct SpColF64;  // Eigen::SparseMatrix<double>, column-major
struct RowMatF64; // Eigen::Matrix<double, Dynamic, Dynamic, RowMajor>

// rows x cols from `count` entries through Eigen's triplet list and setFromTriplets, compressed (values rounded to
// float for csr_f32). csc_f64 is the assembly board's timed path: the triplet list is built in the call.
CRD_BENCH_PEER_API SpF32* csr_f32(Index rows, Index cols, const Entry* entries, Index count);
CRD_BENCH_PEER_API SpF64* csr_f64(Index rows, Index cols, const Entry* entries, Index count);
CRD_BENCH_PEER_API SpColF64* csc_f64(Index rows, Index cols, const Entry* entries, Index count);
// The same from a compressed row-major matrix: outer holds rows + 1 offsets into inner (columns) and values.
CRD_BENCH_PEER_API SpColF64* csc_f64(Index rows, Index cols, const std::uint32_t* outer, const std::uint32_t* inner,
                                     const double* values);
// rows x cols row-major values; nullptr zero-fills
CRD_BENCH_PEER_API RowMatF64* row_mat(Index rows, Index cols, const double* data);
CRD_BENCH_PEER_API void destroy(SpF32* object) noexcept;
CRD_BENCH_PEER_API void destroy(SpF64* object) noexcept;
CRD_BENCH_PEER_API void destroy(SpColF64* object) noexcept;
CRD_BENCH_PEER_API void destroy(RowMatF64* object) noexcept;
CRD_BENCH_PEER_API Index nnz(const SpF64* a) noexcept;
CRD_BENCH_PEER_API Index nnz(const SpColF64* a) noexcept;

// y.noalias() = a * x
CRD_BENCH_PEER_API void spmv(const SpF32* a, const VecF32* x, VecF32* y);
CRD_BENCH_PEER_API void spmv(const SpF64* a, const VecF64* x, VecF64* y);
// c = a * b, not pruned
CRD_BENCH_PEER_API void spgemm(const SpF64* a, const SpF64* b, SpF64* c);
// c.noalias() = a * b
CRD_BENCH_PEER_API void spmm(const SpF64* a, const RowMatF64* b, RowMatF64* c);
// SDDMM the way an Eigen user writes it (Eigen has none): out[k] = x.row(i).dot(y.row(inner[k])) for each k in
// [outer[i], outer[i + 1]) of every row i < rows of a CSR pattern.
CRD_BENCH_PEER_API void sampled_dots(const RowMatF64* x, const RowMatF64* y, Index rows, const std::uint32_t* outer,
                                     const std::uint32_t* inner, double* out);
// The dense way: d = x * y^T, then the sum of d over the same pattern.
CRD_BENCH_PEER_API double masked_dense_sum(const RowMatF64* x, const RowMatF64* y, Index rows,
                                           const std::uint32_t* outer, const std::uint32_t* inner);

// ---- Sparse direct solvers ---------------------------------------------------------------------------------------
enum class SparseSolve : std::uint8_t
{
    LltNatural,  // SimplicialLLT<SparseMatrix<double>, Lower, NaturalOrdering<int>>
    LltAmd,      // SimplicialLLT<SparseMatrix<double>, Lower, AMDOrdering<int>>
    LdltNatural, // SimplicialLDLT<SparseMatrix<double>, Lower, NaturalOrdering<int>>
    LuNatural,   // SparseLU<SparseMatrix<double>, NaturalOrdering<int>>
    LuColamd,    // SparseLU<SparseMatrix<double>, COLAMDOrdering<int>>, Eigen's default
    QrNatural,   // SparseQR<SparseMatrix<double>, NaturalOrdering<int>>
};
struct SpSolver; // one of SparseSolve's decompositions, kept across calls

CRD_BENCH_PEER_API SpSolver* sparse_solver(SparseSolve method);
CRD_BENCH_PEER_API void destroy(SpSolver* object) noexcept;
// solver.analyzePattern(a)
CRD_BENCH_PEER_API void analyze(SpSolver* solver, const SpColF64* a);
// solver.analyzePattern(a), then solver.factorize(a); true when info() is Success
CRD_BENCH_PEER_API bool factorize(SpSolver* solver, const SpColF64* a);
// x = solver.solve(b)
CRD_BENCH_PEER_API void solve(const SpSolver* solver, const VecF64* b, VecF64* x);
CRD_BENCH_PEER_API void solve(const SpSolver* solver, const MatF64* b, MatF64* x);
// The factor's nonzeros: matrixL() materialised (LLT, LDLT), nnzL() + nnzU() (LU), matrixR().nonZeros() (QR)
CRD_BENCH_PEER_API Index factor_nnz(const SpSolver* solver);
// matrixL() materialised in the solver, column-major (cols + 1 outer offsets); false for LU and QR
CRD_BENCH_PEER_API bool factor_pattern(SpSolver* solver, const std::int32_t** outer, const std::int32_t** inner);

// ---- Sparse iterative solvers ------------------------------------------------------------------------------------
// The solver and preconditioner of each configuration the boards measure, on SparseMatrix<double> (column-major).
enum class IterativeSolve : std::uint8_t
{
    CgDiagonal,           // ConjugateGradient<Lower | Upper, DiagonalPreconditioner<double>>
    CgIncompleteCholesky, // ConjugateGradient<Lower | Upper, IncompleteCholesky<double>>
    MinresDiagonal,       // MINRES<Lower | Upper, DiagonalPreconditioner<double>>
    BicgstabIdentity,     // BiCGSTAB<IdentityPreconditioner>
    BicgstabDiagonal,     // BiCGSTAB<DiagonalPreconditioner<double>>
    BicgstabIlut,         // BiCGSTAB<IncompleteLUT<double>>
    GmresIdentity,        // GMRES<IdentityPreconditioner>
    GmresDiagonal,        // GMRES<DiagonalPreconditioner<double>>
    GmresIlut,            // GMRES<IncompleteLUT<double>>
    IdrsIdentity,         // IDRS<IdentityPreconditioner>
    IdrsDiagonal,         // IDRS<DiagonalPreconditioner<double>>
};
// setMaxIterations and setTolerance, and where the configuration has them: GMRES's set_restart, IDRS's setS and
// IncompleteLUT's setDroptol and setFillfactor. The defaults are Eigen's.
struct IterativeSettings
{
    Index max_iterations = 0;
    double tolerance = 0.0;
    Index restart = 30;
    Index s = 4;
    double drop_tolerance = 1e-12; // NumTraits<double>::dummy_precision()
    int fill_factor = 10;
};
struct IterativeResult
{
    Index iterations;
    bool converged; // the preconditioner was built and the solve's info() is Success
};
struct SpIterative; // one of IterativeSolve's solvers, kept across calls

CRD_BENCH_PEER_API SpIterative* iterative_solver(IterativeSolve method, const IterativeSettings& settings);
CRD_BENCH_PEER_API void destroy(SpIterative* object) noexcept;
// solver.compute(a): binds a, which must outlive the solver's use (Eigen keeps a reference), and builds the
// preconditioner; true when info() is Success
CRD_BENCH_PEER_API bool compute(SpIterative* solver, const SpColF64* a);
// x = solver.solve(b) from a zero guess; IDRS draws its shadow space from std::rand, seeded the same before each solve
CRD_BENCH_PEER_API IterativeResult solve(const SpIterative* solver, const VecF64* b, VecF64* x);
// Each column of b solved on its own (Eigen has no block method): the iterations summed, converged when all are
CRD_BENCH_PEER_API IterativeResult solve(const SpIterative* solver, const MatF64* b, MatF64* x);
// (b - a * x).norm() / b.norm(), the worst column's for a matrix
CRD_BENCH_PEER_API double relative_residual(const SpColF64* a, const VecF64* b, const VecF64* x);
CRD_BENCH_PEER_API double relative_residual(const SpColF64* a, const MatF64* b, const MatF64* x);
// The configuration's preconditioner alone, built from a and dropped; true when its info() is Success
CRD_BENCH_PEER_API bool build_preconditioner(IterativeSolve method, const IterativeSettings& settings,
                                             const SpColF64* a);
} // namespace crd_peer::eigen
