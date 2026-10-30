// The sparse LinearOps' crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): ParallelSparseLinearOp and
// ParallelSpmvLeastSquaresOp (parallel_sparse_linear_op.hpp) and ParallelSpmmLinearOp (block_linear_op.hpp), the
// operators the Krylov solvers run on. Each op's bytes constructor is the pick with only the cut set, so production
// and the arms share one body. The thread-sweep board (bench_hesap_operator_parallel) sets the defaults, and a lever
// that loses goes with its field.
//
// The cut is compared with each op's own measure: the SELL ops' stored_bytes() (values and every index array), the
// spmm op's A values only (nnz * sizeof(T), no index arrays and no X or Y columns). The board decides whether both
// move to the bytes one apply streams; until then each keeps its measure so production's choices stay as they were.
//
// Kept serial: every Krylov reduction and vector update (the KBN-pairwise blas1 on the calling thread); the board
// times the solve outside the operator.
//
// Determinism (ADR-0063): both levers are scheduling. Each output row is computed by one job in the serial order, so
// every pick gives the serial spmv's and spmm's bits.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>
#include <crd/jobs/jobs.hpp>

namespace crd::hesap::sparse::detail
{

struct OperatorPick
{
    crd::usize parallel_min_bytes = sparse_resident_bytes(); // scheduling: an op whose measure reaches this applies on
                                                             // the jobs; the default is the L2
    crd::u32 workers = 0; // scheduling: 0 is crd::jobs::num_workers(), otherwise the forced jobs
};

// The jobs an op past its cut applies on; 1 (the serial kernel on the calling thread, no parallel_for) on a pool that
// is down or for one job.
[[nodiscard]] inline crd::u32 operator_jobs(const OperatorPick& pick) noexcept
{
    const crd::u32 workers = crd::jobs::num_workers();
    if (workers == 0)
    {
        return 1;
    }
    return pick.workers == 0 ? workers : pick.workers;
}

} // namespace crd::hesap::sparse::detail
