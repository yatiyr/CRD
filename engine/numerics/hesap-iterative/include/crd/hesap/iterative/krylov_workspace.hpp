#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// KrylovWorkspace<T> -- pre-allocated dense vector bank for a Krylov solve.
//
// The determinism moat (ADR-0084-adjacent; the v4 thesis): the work vectors
// are allocated ONCE here and reused every iteration, and every inner product
// / norm routes through the bit-exact KBN-pairwise `blas1` reductions. With
// no per-iteration allocation and a fixed reduction tree, a solve yields a
// bit-identical {iteration count, residual sequence, solution} across thread
// counts {1,2,4,8,16}.
//
// v4a fields cover CG / PCG: r (residual), z (preconditioned residual; for
// unpreconditioned CG it aliases r and stays unused), p (search direction),
// Ap (operator applied to p). Later Krylov methods (v4b+) extend the bank.
// -----------------------------------------------------------------------

template <typename T>
struct KrylovWorkspace
{
    crd::hesap::dense::Vector<T> r;
    crd::hesap::dense::Vector<T> z;
    crd::hesap::dense::Vector<T> p;
    crd::hesap::dense::Vector<T> ap;

    KrylovWorkspace(crd::memory::IAllocator* alloc, crd::usize n) : r(alloc, n), z(alloc, n), p(alloc, n), ap(alloc, n)
    {
    }

    [[nodiscard]] crd::usize size() const noexcept { return r.size(); }
};

} // namespace crd::hesap::iterative
