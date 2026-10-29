// The one-thread GEMM board's arms (SIMD-DISPATCH): a serial GEMM on given kernels (the level the process runs, or
// kDenseGemmArmsPortable's 4 by 4 tile, crd/hesap/dense/dense_kernels.hpp) down a forced path, so one binary times
// both portable tiles and both small-size paths at every size. Board scaffolding: the board keeps one portable tile
// and sets blas3's serial resident threshold, and this header goes.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/dense_kernels.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense::detail
{

// The path a serial real GEMM takes; every path gives the same bits on the same kernels (dense_kernels.hpp).
enum class GemmPath : crd::u8
{
    Auto,     // gemm's: resident A below blas3's serial threshold where gemm_resident_admits holds, otherwise packed
    Resident, // op(A) read in place as whole row panels; gemm_resident_admits must hold (asserted)
    Packed,   // the Goto/BLIS loop packing every Ac and Bc block
};

// C = alpha op(A) op(B) + beta C on the calling thread, on kernels k down path (f64 and f32).
template <typename T, Layout L>
void gemm_arm(const GemmKernels<T>& k, GemmPath path, T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b,
              T beta, MatrixView<T, L> c, Trans trans_a, Trans trans_b, crd::memory::IAllocator* scratch);

} // namespace crd::hesap::dense::detail
