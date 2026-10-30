// SaAmg's crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): the build's sparse products (the smoothing
// A T and the Galerkin R (A P) at each level, spgemm_arm) and every CSR product of the build and the cycle
// (csr_apply: the residuals, restriction, prolongation, the K-cycle's products, the candidate relaxation and the
// power iteration's D^-1 A x). Both default to serial, the module's behaviour before the levers; the thread-sweep
// board (bench_hesap_amg_parallel) sets the cuts, and a lever that loses goes with its field.
//
// Kept serial, each order-dependent or below a job's cost: aggregation and the C/F split (D(amg)-1, -2), the
// Gauss-Seidel sweeps, the K-cycle's dots and the power iteration's norms (fixed-order sums), the transposes, the
// prolongator's TripletBuilder compress and the coarsest dense LU's solve.
//
// Determinism (ADR-0063): both levers are scheduling. Each output row is computed by one job in the serial order, so
// every setting gives the one-job hierarchy and cycle bits. A forced parallel product inside a job nests a
// parallel_for under that job's wait; the defaults never do.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/sparse/detail/spgemm_arms.hpp>

namespace crd::hesap::amg::detail
{

struct AmgPick
{
    crd::hesap::sparse::detail::SpgemmPick spgemm = {}; // each level's A T and R (A P)
    crd::usize spmv_min_nnz = ~crd::usize{0};           // scheduling: a CSR product over this many stored entries
                                                        // splits its rows across the jobs; the default is serial
    crd::u32 spmv_workers = 0;                          // scheduling: 0 is crd::jobs::num_workers(), otherwise the
                                                        // forced jobs
};

} // namespace crd::hesap::amg::detail
