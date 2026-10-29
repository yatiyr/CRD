// The dense factors' crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): factor_lu and factor_cholesky
// are factor_lu_arm(LuPick{}) and factor_cholesky_arm(CholeskyPick{}), so production and the arms share one body; the
// thread-sweep board (bench_hesap_factor_parallel) sets the defaults and a lever that loses goes with its field.
//
// Determinism (ADR-0063): the worker count and the split thresholds decide only which job computes a row or column
// of the result, never its value, so for one block and unblocked cut every worker count and threshold gives the
// one-job factor's bits. The block and the unblocked cut regroup the trailing sums: those arms are held to the
// factor's backward-error bound, not to its bits.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense::detail
{

// trsm_min compares the solve's columns (LU) or rows (Cholesky) times the panel width; workers 0 is
// crd::jobs::num_workers().
struct LuPick
{
    crd::usize block = 64;                // the panel width
    crd::usize trailing_min = 0;          // the trailing update A22 -= L21 U12 runs on the jobs from this m n k on
    crd::usize trsm_min = ~crd::usize{0}; // U12 = L11^-1 A12 splits its columns across the jobs from this on
    crd::u32 workers = 0;                 // the jobs
};

struct CholeskyPick
{
    crd::usize unblocked_max = 256;         // the unblocked kernel factors the whole matrix up to this n
    crd::usize block = 64;                  // the panel width of the blocked factor
    crd::usize trsm_min = 64ULL * 1024ULL;  // L21 = A21 L11^-T splits its rows across the jobs from this on
    crd::usize syrk_mmk = 256ULL * 1024ULL; // the trailing syrk splits its column panels above this m m k
    crd::u32 workers = 0;                   // the jobs
};

// factor_lu (lu.hpp) on pick's levers, every real and complex type; more than one job needs crd::jobs initialised.
template <typename T, Layout L> void factor_lu_arm(const LuPick& pick, LU<T, L>& lu, crd::memory::IAllocator* scratch);

// factor_cholesky (cholesky.hpp) on pick's levers, f32 and f64; more than one job needs crd::jobs initialised.
template <typename T, Layout L>
void factor_cholesky_arm(const CholeskyPick& pick, Cholesky<T, L>& chol, crd::memory::IAllocator* scratch);

} // namespace crd::hesap::dense::detail
