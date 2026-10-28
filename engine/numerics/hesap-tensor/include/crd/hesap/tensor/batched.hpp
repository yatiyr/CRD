#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-h: BATCHED dense linear algebra over rank-3 views
// [B, rows, cols] — the EKF/sensor-fusion/skinning regime (small matrices,
// huge batches). Reuse per SANITY #8: per-matrix compute = hesap-dense
// (gemm + the unblocked factorization algorithms); batch parallelism =
// crd-jobs deterministic partition (grain a function of shape ONLY, disjoint
// outputs) ⇒ results are BIT-IDENTICAL at any worker count. Peers on the
// board: torch-CPU (bmm/linalg on stacks), MATLAB pagemtimes, native MKL
// cblas_?gemm_batch (docs/bench 2026-07-05 v14-h sections).
//
// v1 layout contract: every operand is rank-3 with a TIGHT inner matrix
// (stride(2)==1, stride(1)==cols — row-major per matrix); the BATCH stride
// (stride(0)) is arbitrary ⇒ strided/sliced batch views work. Returns
// BadInput otherwise (the general-strided fallback rides a later increment
// with the einsum copy-avoidance machinery).
//
// The per-matrix work runs the per-level tensor kernels (tensor_kernels.hpp
// BatchedKernels, SIMD-DISPATCH): the allocation-free tiny GEMM, the
// lane-batched factorizations with their scalar tier (batched_core.hpp) and
// the solves. The drivers here check shapes, partition the batch and run the
// dense GEMM above the tiny tier.
// ---------------------------------------------------------------------------
#include "tensor.hpp"
#include "tensor_dispatch.hpp"

#include <crd/hesap/dense/blas3.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <type_traits>

namespace crd::hesap::tensor
{

namespace batcheddetail
{

// tight [B, r, c] check (see the layout contract above)
template <typename T>
[[nodiscard]] inline bool tight3(const TensorView<T>& v) noexcept
{
    return v.rank() == 3U && v.stride(2) == 1 &&
           v.stride(1) == static_cast<crd::i64>(v.shape(2));
}

// deterministic batch grain: a function of shape ONLY (Tier-D rule) — never
// of the live worker count, so partitions are stable across pools.
[[nodiscard]] inline crd::u32 batch_grain(crd::u64 batch, crd::u64 per_matrix_flops) noexcept
{
    // target ~64K flops per task minimum so tiny matrices amortize task setup
    const crd::u64 per = per_matrix_flops > 0U ? per_matrix_flops : 1U;
    crd::u64 g = (64ULL * 1024ULL + per - 1ULL) / per;
    if (g < 1U)
    {
        g = 1U;
    }
    if (g > batch)
    {
        g = batch;
    }
    return static_cast<crd::u32>(g);
}

// The batched kernels of T at the level the process runs.
template <typename T> [[nodiscard]] inline const BatchedKernels<T>& kernels() noexcept
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "batched LA runs f32 and f64");
    const TensorKernels& k = *tensor_kernels().table;
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        return k.batched_f32;
    }
    else
    {
        return k.batched_f64;
    }
}

// fn(lo, hi) over [0, count): one call with one worker (num_workers 0 = the pool's) or below two items, else a
// parallel_for of disjoint ranges.
template <typename F> inline void run_ranges(crd::u32 count, crd::u32 num_workers, const F& fn) noexcept
{
    const crd::u32 nw = num_workers == 0U ? crd::jobs::num_workers() : num_workers;
    if (nw <= 1U || count < 2U)
    {
        fn(0U, count);
        return;
    }
    const F* const fp = &fn;
    auto* const counter = crd::jobs::parallel_for(count, nw, [fp](crd::u32 lo, crd::u32 hi) { (*fp)(lo, hi); });
    crd::jobs::wait(counter);
}

// A factorization kernel over the batch's lane groups (kEwLanes matrices each, the last one short), each job a whole
// number of groups as a matrix range.
template <typename T, typename Ctx>
inline void run_groups(void (*kernel)(const Ctx&, crd::u64, crd::u64) noexcept, const Ctx& ctx, crd::u64 batch,
                       crd::u32 num_workers) noexcept
{
    constexpr crd::u64 w = kEwLanes<T>;
    const auto groups = [kernel, &ctx, batch](crd::u32 gb, crd::u32 ge) noexcept
    {
        const crd::u64 hi = static_cast<crd::u64>(ge) * w;
        kernel(ctx, static_cast<crd::u64>(gb) * w, hi < batch ? hi : batch);
    };
    run_ranges(static_cast<crd::u32>((batch + w - 1U) / w), num_workers, groups);
}

} // namespace batcheddetail

// In-place batched Cholesky: A[b] (SPD, row-major tight) -> its lower factor
// L[b] (strict upper zeroed). info[b] = 0 on success, or 1-based pivot column
// where positivity failed (that matrix's factor content is then unspecified;
// SIBLING matrices in its SIMD group are UNAFFECTED — gated). Lane-batched
// AoSoA kernel for n <= kBatchedCholMaxN, scalar tier otherwise;
// the tiers are BIT-IDENTICAL (gated). Deterministic at any worker count.
template <typename T>
[[nodiscard]] TensorStatus batched_cholesky_factor(TensorView<T> a, crd::containers::Span<crd::i32> info,
                                                   crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || a.shape(1) != a.shape(2))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    if (info.size() != batch)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const BatchedFactor<T> f{a.data(), a.stride(0), info.data(), nullptr, a.shape(1)};
    run_groups<T>(kernels<T>().cholesky, f, batch, num_workers);
    return TensorStatus::Ok;
}

// Batched triangular solves with the batched factor: X = L^-T (L^-1 RHS)
// — the SPD solve. RHS = [B, n, r] tight, solved in place. Same op order in
// both tiers (bit-identical, gated).
template <typename T>
[[nodiscard]] TensorStatus batched_cholesky_solve(TensorView<const T> l, TensorView<T> rhs,
                                                  crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(l) || !tight3(rhs) || l.shape(1) != l.shape(2) || rhs.shape(1) != l.shape(1) ||
        rhs.shape(0) != l.shape(0))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = l.shape(0);
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const BatchedSolve<T> s{l.data(), rhs.data(), nullptr, l.stride(0), rhs.stride(0), l.shape(1), rhs.shape(2)};
    const auto solve = [&s](crd::u32 lo, crd::u32 hi) noexcept
    {
        kernels<T>().cholesky_solve(s, lo, hi);
    };
    run_ranges(static_cast<crd::u32>(batch), num_workers, solve);
    return TensorStatus::Ok;
}

// Batched small-SVD (square n <= kBatchedSvdMaxN lane-batched, scalar above):
// A[b] = U[b] diag(sigma[b]) V[b]^T. sigma descending (stable ties), U/V
// column-orthonormal; info[b] = 0 converged, 1 = still rotating at max_sweeps
// (bounded iteration — never spins). Tier bit-identity gated.
template <typename T>
[[nodiscard]] TensorStatus batched_svd_small(TensorView<const T> a, TensorView<T> u,
                                             crd::containers::Span<T> sigma, TensorView<T> v,
                                             crd::containers::Span<crd::i32> info, crd::u32 max_sweeps = 30,
                                             crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || !tight3(u) || !tight3(v) || a.shape(1) != a.shape(2))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 n = a.shape(1);
    if (u.shape(0) != batch || u.shape(1) != n || u.shape(2) != n || v.shape(0) != batch || v.shape(1) != n ||
        v.shape(2) != n || sigma.size() != batch * n || info.size() != batch || max_sweeps == 0U)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const BatchedSvd<T> s{a.data(),    u.data(),    v.data(), sigma.data(), info.data(), a.stride(0),
                          u.stride(0), v.stride(0), n,        max_sweeps};
    run_groups<T>(kernels<T>().svd, s, batch, num_workers);
    return TensorStatus::Ok;
}

// In-place batched LU with partial pivoting: A[b] -> L (unit lower, strict) +
// U (upper). piv = [B*n] 0-based pivot ROW chosen at each column (LAPACK-style
// sequential swaps); info[b] = 0 or the 1-based first singular column. Lane
// tier for n <= kBatchedCholMaxN, scalar otherwise; tiers BIT-IDENTICAL
// (gated, incl. the pivot tie rule).
template <typename T>
[[nodiscard]] TensorStatus batched_lu_factor(TensorView<T> a, crd::containers::Span<crd::i32> piv,
                                             crd::containers::Span<crd::i32> info,
                                             crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || a.shape(1) != a.shape(2))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 n = a.shape(1);
    if (info.size() != batch || piv.size() != batch * n)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const BatchedFactor<T> f{a.data(), a.stride(0), info.data(), piv.data(), n};
    run_groups<T>(kernels<T>().lu, f, batch, num_workers);
    return TensorStatus::Ok;
}

// Batched LU solve (in place on rhs = [B, n, r] tight): apply the pivot
// sequence, unit-lower forward substitution, upper backward substitution.
template <typename T>
[[nodiscard]] TensorStatus batched_lu_solve(TensorView<const T> lu, crd::containers::ConstSpan<crd::i32> piv,
                                            TensorView<T> rhs, crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(lu) || !tight3(rhs) || lu.shape(1) != lu.shape(2) || rhs.shape(1) != lu.shape(1) ||
        rhs.shape(0) != lu.shape(0))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = lu.shape(0);
    const crd::u64 n = lu.shape(1);
    if (piv.size() != batch * n)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const BatchedSolve<T> s{lu.data(), rhs.data(), piv.data(), lu.stride(0), rhs.stride(0), n, rhs.shape(2)};
    const auto solve = [&s](crd::u32 lo, crd::u32 hi) noexcept
    {
        kernels<T>().lu_solve(s, lo, hi);
    };
    run_ranges(static_cast<crd::u32>(batch), num_workers, solve);
    return TensorStatus::Ok;
}

// C[b] = alpha * A[b] @ B[b] + beta * C[b]  for b in [0, batch)
// A=[B,m,k] B=[B,k,n] C=[B,m,n], tight inner dims (see the layout contract).
// num_workers: 0 = use the jobs pool when profitable (serial without a pool);
// any worker count produces BIT-IDENTICAL results (disjoint outputs, fixed
// per-matrix kernels).
template <typename T>
[[nodiscard]] TensorStatus batched_gemm(T alpha, TensorView<const T> a, TensorView<const T> b, T beta,
                                        TensorView<T> c, crd::memory::IAllocator* scratch,
                                        crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || !tight3(b) || !tight3(c))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 m = a.shape(1);
    const crd::u64 k = a.shape(2);
    const crd::u64 n = b.shape(2);
    if (b.shape(0) != batch || c.shape(0) != batch || b.shape(1) != k || c.shape(1) != m || c.shape(2) != n)
    {
        return TensorStatus::ShapeMismatch;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const BatchedGemm<T> g{alpha, beta, a.data(), b.data(), c.data(), a.stride(0), b.stride(0), c.stride(0), m, k, n};
    // the tiny tier's direct kernel allocates nothing: below 32K flops the packing pipeline costs more than the work,
    // and the batch loop stays off the scratch allocator
    const bool tiny = 2ULL * m * n * k <= 32ULL * 1024ULL;
    // the dense-gemm tier packs via `scratch` — concurrent workers get a
    // thread-safe wrap (contended once per pack, amortized at these sizes)
    crd::memory::ThreadSafeAllocator ts_scratch(scratch);
    const auto run_range = [&g, tiny, scratch, &ts_scratch](crd::u32 begin, crd::u32 end, bool threaded) noexcept
    {
        if (tiny)
        {
            kernels<T>().gemm_tiny(g, begin, end);
            return;
        }
        using Rm = crd::hesap::dense::MatrixView<const T, crd::hesap::dense::Layout::RowMajor>;
        for (crd::u32 i = begin; i < end; ++i)
        {
            const Rm av{g.a + static_cast<crd::i64>(i) * g.sa, static_cast<crd::usize>(g.m),
                        static_cast<crd::usize>(g.k), static_cast<crd::usize>(g.k)};
            const Rm bv{g.b + static_cast<crd::i64>(i) * g.sb, static_cast<crd::usize>(g.k),
                        static_cast<crd::usize>(g.n), static_cast<crd::usize>(g.n)};
            crd::hesap::dense::MatrixView<T, crd::hesap::dense::Layout::RowMajor> cv{
                g.c + static_cast<crd::i64>(i) * g.sc, static_cast<crd::usize>(g.m), static_cast<crd::usize>(g.n),
                static_cast<crd::usize>(g.n)};
            crd::hesap::dense::gemm<T, crd::hesap::dense::Layout::RowMajor>(
                g.alpha, av, bv, g.beta, cv, crd::hesap::dense::Trans::None, crd::hesap::dense::Trans::None,
                threaded ? static_cast<crd::memory::IAllocator*>(&ts_scratch) : scratch);
        }
    };
    const crd::u32 nw = num_workers == 0U ? crd::jobs::num_workers() : num_workers;
    if (nw <= 1U || batch < 2U)
    {
        run_range(0U, static_cast<crd::u32>(batch), false);
        return TensorStatus::Ok;
    }
    // across-batch parallel: disjoint C slices per task; grain = f(shape) only
    const crd::u32 grain = batch_grain(batch, 2ULL * m * n * k);
    const crd::u32 count = static_cast<crd::u32>(batch);
    const auto tasks = [&run_range, grain, count](crd::u32 tb, crd::u32 te) noexcept
    {
        for (crd::u32 t = tb; t < te; ++t)
        {
            const crd::u32 lo = t * grain;
            const crd::u32 hi = count - lo < grain ? count : lo + grain;
            run_range(lo, hi, true);
        }
    };
    run_ranges((count + grain - 1U) / grain, nw, tasks);
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
