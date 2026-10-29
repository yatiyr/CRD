// The symmetric eigensolvers' crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): eigvals_sym is
// eigvals_sym_arm(EigvalsPick{}) and mrrr_compute_vectors (mrrr_vectors.hpp) takes MrrrPick{} by default, so
// production and the arms share one body; the thread-sweep board (bench_hesap_eig_parallel) sets the defaults and a
// lever that loses goes with its field.
//
// Determinism (ADR-0063, ADR-0065 D(dense-eig)-MRRR-multisection-global-tree): every multisection chunk descends the
// one bisection tree rooted at the Gershgorin bracket, and the MRRR segments write disjoint columns, so the chunk and
// job counts, the granularity and the serial cut decide only which job computes an eigenvalue or a vector, never its
// bits. multisection_min swaps dqds for the multisection, two algorithms: that arm is held to a declared bound.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense::detail
{

// chunks 0 is the job count; workers 0 is crd::jobs::num_workers().
struct MultisectionPick
{
    crd::u32 chunks = 0;  // the eigenvalue index range splits into this many chunks
    crd::u32 workers = 0; // the jobs the chunks run on
};

struct EigvalsPick
{
    crd::usize multisection_min = 512; // the multisection replaces dqds from this n on
    MultisectionPick multisection = {};
};

// workers 0 is crd::jobs::num_workers(); the segments' tasks per job are n / gran_div, clamped to [1, gran_max].
struct MrrrPick
{
    crd::u32 workers = 0;      // the jobs
    int gran_div = 256;        // n per task per job
    int gran_max = 8;          // the most tasks per job
    crd::usize serial_max = 0; // the segments run on the calling thread up to this n
};

// The ascending eigenvalues w of the symmetric tridiagonal (d, e length n - 1) by shared-Sturm multisection on
// pick's chunks and jobs, f32 and f64; more than one job needs crd::jobs initialised. A non-finite Gershgorin bracket
// (a NaN or an infinity in d or e) gives every eigenvalue a quiet NaN.
template <typename R>
void tridiag_multisection_arm(const MultisectionPick& pick, crd::memory::IAllocator* alloc, const R* d, const R* e,
                              int n, R* w);

// eigvals_sym (eig_sym.hpp) on pick's levers, f32 and f64.
template <typename T>
[[nodiscard]] Vector<RealType<T>> eigvals_sym_arm(const EigvalsPick& pick, crd::memory::IAllocator* alloc,
                                                  const Symmetric<T>& a);

} // namespace crd::hesap::dense::detail
