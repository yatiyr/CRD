// The GEMM boards' arms (SIMD-DISPATCH). gemm_arm: a serial GEMM on given kernels (the level the process runs, or
// kDenseGemmArmsPortable's 4 by 4 tile, crd/hesap/dense/dense_kernels.hpp) down a forced path, so one binary times
// both portable tiles and both small-size paths at every size; the board keeps one portable tile and sets blas3's
// serial resident threshold, and GemmPath goes. gemm_parallel_arm: gemm_parallel's crd-jobs levers (the folded
// HESAP-JOBS-RETUNE), production's being GemmParallelPick{}; the thread-sweep board sets the defaults and a lever
// that loses goes with its field.

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

// How gemm_parallel splits its work among the jobs of one parallel_for (every choice gives the serial gemm's bits:
// each job writes disjoint rows of C or disjoint panels of the packed B).
enum class GemmMcSplit : crd::u8
{
    PerWorker, // the Mc block shrunk to ceil(m / num_workers) rows (whole tiles), so a small m still fills the workers
    Block,     // the shape's whole Mc block, one per job
};

enum class GemmSplit : crd::u8
{
    Serial,   // on the calling thread
    Parallel, // whole tiles (B: nr-column panels, C: rows) across the num_workers jobs
};

struct GemmParallelPick
{
    crd::usize resident_mnk = 200ULL * 1024ULL * 1024ULL; // the resident-A path below this m n k where admitted
    GemmMcSplit mc = GemmMcSplit::PerWorker;
    GemmSplit pack_b = GemmSplit::Serial;    // the packed B of each depth group (resident path: of the whole depth)
    GemmSplit beta_only = GemmSplit::Serial; // C = beta C when the depth k is 0
};

// gemm_parallel (blas3.hpp) on pick's levers, every real and complex type; from 2 workers on (one is serial gemm).
template <typename T, Layout L>
void gemm_parallel_arm(const GemmParallelPick& pick, crd::u32 num_workers, T alpha, MatrixView<const T, L> a,
                       MatrixView<const T, L> b, T beta, MatrixView<T, L> c, Trans trans_a, Trans trans_b,
                       crd::memory::IAllocator* scratch);

} // namespace crd::hesap::dense::detail
