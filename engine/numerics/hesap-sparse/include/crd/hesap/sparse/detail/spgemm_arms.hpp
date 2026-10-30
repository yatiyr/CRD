// The sparse product's crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): spgemm_arm (spgemm_parallel.hpp)
// runs the serial product below the flop cut and the row-parallel two-phase body from it, so the serial and parallel
// products share one entry; spgemm_parallel is the arm with the cut at 0. The thread-sweep board
// (bench_hesap_amg_parallel, the Galerkin products) sets the defaults, and a lever that loses goes with its field.
//
// Determinism (ADR-0063): both levers are scheduling. Each row of C is accumulated by one job in the serial order
// (A's row in stored order, each B row in stored order, the first touch stored, then added) and emitted column
// sorted, so every setting gives the serial product's bits.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::sparse::detail
{

struct SpgemmPick
{
    crd::u64 parallel_min_flop = ~0ULL; // scheduling: a product of this many multiply-adds runs on the jobs; the
                                        // default keeps every product serial until the board sets the cut
    crd::u32 workers = 0;               // scheduling: 0 is crd::jobs::num_workers(), otherwise the forced jobs
};

} // namespace crd::hesap::sparse::detail
