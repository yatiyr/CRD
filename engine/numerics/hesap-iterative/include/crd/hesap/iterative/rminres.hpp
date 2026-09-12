#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/gcrot.hpp> // detail::gcrot_core<T,true>, RecycleSpace, GcrotWorkspace, GcrotTruncate
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// RMINRES -- recycling MINRES (deflated MINRES; Wang-de Sturler-Paulino 2007).
// Phase 3.1.6 v4e-3b.
//
// For SYMMETRIC / HERMITIAN (possibly INDEFINITE) A -- the regime CG cannot touch.
// Built on the shared recycling core (`detail::gcrot_core`) with the inner basis
// driven by the 3-term SYMMETRIC LANCZOS recurrence instead of full Arnoldi MGS:
// each Lanczos vector is kept orthogonal to the recycle space C (= A·U), which for
// symmetric A preserves the cheap 3-term recurrence (⟨vᵢ,Avⱼ⟩ = 0 for i < j−1).
// The recycle machinery -- C-projection (B = CᴴAV), the GCR minimal-residual step,
// the SVD-optimal recycle truncation, and the cross-solve C-rebuild -- is identical
// to GCROT; only the inner orthogonalization changes. The tridiagonal is real
// symmetric even for complex Hermitian A (α = Re⟨vⱼ,Avⱼ⟩, β real).
//
// CROSS-SOLVE RECYCLING (the de Sturler payoff): `rminres_recycled` takes a
// persistent `RecycleSpace<T>`; the deflation space built on one symmetric A_i
// accelerates the next (C = A·U rebuilt on entry, safe across operators). RMINRES
// reuses `GcrotWorkspace<T>` / `RecycleSpace<T>`. Determinism: every reduction is
// KBN-pairwise; the only parallel step is the operator spmv (bit-exact across
// threads). Eigen ships no recycling Krylov -- the win is iterations saved across a
// symmetric-indefinite sequence (the recycling-literature metric).
// -----------------------------------------------------------------------

// RMINRES reuses the GCROT workspace + recycle-space types verbatim.
template <typename T>
using RminresWorkspace = GcrotWorkspace<T>;

// Single-solve RMINRES (fresh recycle space each call). A must be symmetric/Hermitian.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> rminres(const crd::hesap::LinearOp<T>&  a,
                                                        crd::containers::ConstSpan<T>   b,
                                                        crd::containers::Span<T>        x,
                                                        const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                        RminresWorkspace<T>&            ws,
                                                        crd::memory::IAllocator*        result_alloc)
{
    ws.own.clear();
    return detail::gcrot_core<T, /*SymLanczos=*/true>(a, nullptr, b, x, opts, ws, ws.own, /*rebuild_c=*/false,
                                                      result_alloc);
}

// Cross-solve RMINRES: a PERSISTENT recycle space `rs` carried across a sequence of
// symmetric/Hermitian systems. C = A·U is rebuilt on entry, so reusing `rs` across
// DIFFERENT symmetric operators A_i is safe.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> rminres_recycled(const crd::hesap::LinearOp<T>&  a,
                                                                 crd::containers::ConstSpan<T>   b,
                                                                 crd::containers::Span<T>        x,
                                                                 const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                                 RminresWorkspace<T>&            ws,
                                                                 RecycleSpace<T>&                rs,
                                                                 crd::memory::IAllocator*        result_alloc)
{
    return detail::gcrot_core<T, /*SymLanczos=*/true>(a, nullptr, b, x, opts, ws, rs,
                                                      /*rebuild_c=*/(rs.dimension() > 0), result_alloc);
}

} // namespace crd::hesap::iterative
