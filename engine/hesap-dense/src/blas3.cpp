#include <crd/hesap/dense/blas3.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/detail/gemm_microkernel.hpp>
#include <crd/hesap/dense/detail/gemm_pack.hpp>
#include <crd/hesap/dense/detail/pairwise_sum.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/memory.hpp>

namespace crd::hesap::dense
{

namespace
{
// Goto/BLIS cache-block defaults (alias detail::kGemmMc / Kc / Nc).
constexpr crd::usize kMc = detail::kGemmMc;
constexpr crd::usize kKc = detail::kGemmKc;
constexpr crd::usize kNc = detail::kGemmNc;

template <typename T, Layout L>
[[nodiscard]] inline T view_at(MatrixView<const T, L> v, crd::usize i, crd::usize j) noexcept
{
    if constexpr (L == Layout::RowMajor)
    {
        return v.data()[i * v.ld() + j];
    }
    else
    {
        return v.data()[j * v.ld() + i];
    }
}

template <typename T, Layout L>
inline T& view_at_ref(MatrixView<T, L> v, crd::usize i, crd::usize j) noexcept
{
    if constexpr (L == Layout::RowMajor)
    {
        return v.data()[i * v.ld() + j];
    }
    else
    {
        return v.data()[j * v.ld() + i];
    }
}

template <typename T>
[[nodiscard]] inline T maybe_conj(T v, bool do_conj) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return do_conj ? crd::hesap::conj(v) : v;
    }
    else
    {
        (void)do_conj;
        return v;
    }
}

// Effective A[i, k] honoring trans_a. Returns A[i, k] (None), A[k, i] (T),
// or conj(A[k, i]) (H). The shape mapping is handled by callers passing
// the right indices.
template <typename T, Layout L>
[[nodiscard]] inline T eff_a(MatrixView<const T, L> a, crd::usize r, crd::usize c, Trans tr) noexcept
{
    switch (tr)
    {
    case Trans::None:
        return view_at(a, r, c);
    case Trans::Transpose:
        return view_at(a, c, r);
    case Trans::ConjTranspose:
        return maybe_conj<T>(view_at(a, c, r), true);
    }
    return view_at(a, r, c);
}

} // namespace

// =======================================================================
// gemm — Goto/BLIS 5-loop. v0d-perf (ADR-0082 intrinsics backend).
//
// Loop structure (outer-to-inner):
//   jc: outer column slab (Nc-wide)         — cache C
//   pc: outer K slab (Kc-deep)              — cache the K dimension
//   ic: outer row slab (Mc-tall)            — cache A panel into L2
//     pack Ac panel (mc × kc) into a_pack
//     pack Bc panel (kc × nc) into b_pack
//     gemm_packed_inner: mr × nr loops calling gemm_microkernel<T> on
//                       (Ac panel, Bc panel) → produces mr × nr C tile
//
// Pack buffers come from the `scratch` IAllocator (Matrix overload passes
// `a.allocator()`; raw view-form callers may pass nullptr to fall back to
// a per-thread growable pool if nullptr, but that is discouraged — see
// memory/feedback_hesap_propagate_allocator). The maximum Ac size is
// `ceil(Mc/MR) * MR * Kc` = ceil(120/8)*8*256 = 30720 entries = 120 KB
// (f32) / 240 KB (f64). Max Bc is `ceil(Nc/NR) * Kc * NR =
// ceil(4080/8)*256*8 = 1044480` ≈ 4 MB (f32) / 8 MB (f64). Total scratch:
// ~4-8 MB per gemm call.
// =======================================================================
template <typename T, Layout L>
void gemm(T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta,
          MatrixView<T, L> c, Trans trans_a, Trans trans_b, crd::memory::IAllocator* scratch)
{
    const crd::usize m = (trans_a == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans_a == Trans::None) ? a.cols() : a.rows();
    [[maybe_unused]] const crd::usize k2 = (trans_b == Trans::None) ? b.rows() : b.cols();
    const crd::usize n = (trans_b == Trans::None) ? b.cols() : b.rows();
    CRD_ASSERT_MSG(k == k2, "gemm: inner dimensions of A and B must match");
    CRD_ASSERT_MSG(c.rows() == m && c.cols() == n, "gemm: C must be m*n");

    // Scale C by beta first (or zero if beta == 0).
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            view_at_ref(c, i, j) = beta * view_at_ref(c, i, j);
        }
    }

    // Allocate pack buffers once per gemm call from the caller-supplied
    // scratch allocator (or a per-thread growable pool if nullptr — discouraged).
    // Size to MIN(actual_dim, macro_block) to avoid wasting tens of MB on
    // small inputs (e.g. an 8x8 GEMM doesn't need 8 MB of Bc scratch).
crd::memory::IAllocator* alloc = scratch;
    if (alloc == nullptr)
    {
        // No caller allocator (discouraged -- feedback_hesap_propagate_allocator). Fall back to a
        // PER-THREAD growable pool, never MallocAllocator (no malloc allocators in the engine).
        thread_local crd::memory::GrowableTlsfAllocator s_fallback;
        alloc = &s_fallback;
    }
    const crd::usize a_pack_dim_m = std::min<crd::usize>(m, kMc);
    const crd::usize a_pack_dim_k = std::min<crd::usize>(k, kKc);
    const crd::usize b_pack_dim_n = std::min<crd::usize>(n, kNc);
    const crd::usize a_pack_capacity =
        ((a_pack_dim_m + detail::GemmTraits<T>::MR - 1) / detail::GemmTraits<T>::MR) * detail::GemmTraits<T>::MR * a_pack_dim_k;
    const crd::usize b_pack_capacity =
        ((b_pack_dim_n + detail::GemmTraits<T>::NR - 1) / detail::GemmTraits<T>::NR) * a_pack_dim_k * detail::GemmTraits<T>::NR;
    const crd::usize align = alignof(T) > 32 ? alignof(T) : 32;
    auto* a_pack = static_cast<T*>(alloc->allocate(a_pack_capacity * sizeof(T), align));
    auto* b_pack = static_cast<T*>(alloc->allocate(b_pack_capacity * sizeof(T), align));

    for (crd::usize jc = 0; jc < n; jc += kNc)
    {
        const crd::usize nc = (jc + kNc < n) ? kNc : (n - jc);
        for (crd::usize pc = 0; pc < k; pc += kKc)
        {
            const crd::usize kc = (pc + kKc < k) ? kKc : (k - pc);

            // Pack Bc panel once per (jc, pc) — reused across all ic blocks.
            // No explicit template args: T/L are deduced from b's MatrixView<const T, L>
            // type — avoids MSVC's `<T, L>` template-arg parsing ambiguity inside
            // a templated function body (sibling D27 v0c quirk).
            detail::pack_b(b, pc, jc, kc, nc, trans_b, b_pack);

            for (crd::usize ic = 0; ic < m; ic += kMc)
            {
                const crd::usize mc = (ic + kMc < m) ? kMc : (m - ic);

                detail::pack_a(a, ic, pc, mc, kc, trans_a, a_pack);
                detail::gemm_packed_inner(alpha, ic, jc, mc, nc, kc, a_pack, b_pack, c);
            }
        }
    }

    alloc->deallocate(a_pack);
    alloc->deallocate(b_pack);
}

// =======================================================================
// small_gemm — direct unpacked fast-path for small RowMajor f32/f64 GEMM.
//
// v0d-small-gemm-fastpath: at small M*N*K, the Goto/BLIS pipeline's
// allocator + pack_a + pack_b overhead (~290 us at N=256 f64) dominates
// the actual compute (~40 us at 16 P-threads). This direct path:
//   - skips A packing entirely (calls the existing microkernel directly
//     on the source A row-panel, since `a_packed[i*k+p]` matches
//     `a.data()[(i_start+i)*lda + p]` when lda == k)
//   - packs B once (small enough at small N; reused across A panels)
//   - parallelizes over Mr=8 row-panels of A, NOT Mc-tiles, so num_panels
//     scales with m/8 instead of m/Mc — perfect load balance for hybrid CPU
//   - no per-worker scratch allocation
//
// Preconditions: RowMajor, no transpose, tight strides (lda==k, ldb==n,
// ldc==n). f32 or f64. Caller validates via `small_gemm_eligible`.
// =======================================================================
template <typename T, Layout L>
inline bool small_gemm_eligible(MatrixView<const T, L> a, MatrixView<const T, L> b,
                                MatrixView<T, L> c, Trans trans_a, Trans trans_b) noexcept
{
    if constexpr (L != Layout::RowMajor)
    {
        return false;
    }
    if constexpr (!(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>))
    {
        return false;
    }
    if (trans_a != Trans::None || trans_b != Trans::None)
    {
        return false;
    }
    // Tight strides — required for unpacked A read to alias the microkernel's
    // packed-A index formula.
    if (a.ld() != a.cols() || b.ld() != b.cols() || c.ld() != c.cols())
    {
        return false;
    }
    // Mr-aligned m: edge-row scalar fallback adds complexity for small win.
    if (a.rows() % detail::GemmTraits<T>::MR != 0)
    {
        return false;
    }
    return true;
}

template <typename T, Layout L>
void small_gemm_parallel(crd::u32 num_workers, T alpha, MatrixView<const T, L> a,
                        MatrixView<const T, L> b, T beta, MatrixView<T, L> c,
                        crd::memory::IAllocator* scratch)
{
    const crd::usize m = a.rows();
    const crd::usize n = b.cols();
    const crd::usize k = a.cols();

    // Scale C by beta (serial; small enough at fast-path sizes).
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            view_at_ref(c, i, j) = beta * view_at_ref(c, i, j);
        }
    }

    // Pack B once. Smaller than the worst-case b_pack_capacity since we use
    // the actual n×k bounds (not full Nc×Kc). At N=256 f64: 256×256×8 = 524KB.
crd::memory::IAllocator* alloc = scratch;
    if (alloc == nullptr)
    {
        // No caller allocator (discouraged -- feedback_hesap_propagate_allocator). Fall back to a
        // PER-THREAD growable pool, never MallocAllocator (no malloc allocators in the engine).
        thread_local crd::memory::GrowableTlsfAllocator s_fallback;
        alloc = &s_fallback;
    }
    const crd::usize b_pack_capacity =
        ((n + detail::GemmTraits<T>::NR - 1) / detail::GemmTraits<T>::NR) * k * detail::GemmTraits<T>::NR;
    const crd::usize align = alignof(T) > 32 ? alignof(T) : 32;
    auto* b_pack = static_cast<T*>(alloc->allocate(b_pack_capacity * sizeof(T), align));
    detail::pack_b(b, 0, 0, k, n, Trans::None, b_pack);

    // Parallel over Mr=8 row-panels of A. At m=256 → 32 panels; with 16
    // P-threads each worker handles 2 panels → perfect load balance.
    constexpr crd::usize k_mr = detail::GemmTraits<T>::MR;
    const crd::u32 num_panels = static_cast<crd::u32>(m / k_mr);

    struct State
    {
        MatrixView<const T, L> a;
        MatrixView<T, L> c;
        const T* b_pack;
        T alpha;
        crd::usize m, n, k;
    };
    State s{a, c, b_pack, alpha, m, n, k};
    State* sp = &s;

    auto* counter = crd::jobs::parallel_for(
        num_panels, num_workers, [sp](crd::u32 begin, crd::u32 end) {
            for (crd::u32 panel = begin; panel < end; ++panel)
            {
                const crd::usize i_start = static_cast<crd::usize>(panel) * detail::GemmTraits<T>::MR;
                // a_panel: source A row-panel, treated as if packed (works
                // because a.ld() == k, asserted via small_gemm_eligible).
                const T* a_panel = sp->a.data() + i_start * sp->a.ld();
                detail::gemm_packed_inner(sp->alpha, i_start, 0, detail::GemmTraits<T>::MR, sp->n, sp->k,
                                          a_panel, sp->b_pack, sp->c);
            }
        });
    crd::jobs::wait(counter);

    alloc->deallocate(b_pack);
}

// =======================================================================
// gemm_parallel — BLIS-style ic-loop parallelism over crd::jobs workers.
// =======================================================================
template <typename T, Layout L>
void gemm_parallel(crd::u32 num_workers, T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b,
                   T beta, MatrixView<T, L> c, Trans trans_a, Trans trans_b,
                   crd::memory::IAllocator* scratch)
{
    if (num_workers <= 1)
    {
        gemm<T, L>(alpha, a, b, beta, c, trans_a, trans_b, scratch);
        return;
    }

    // v0d-small-gemm-fastpath: dispatch to direct unpacked GEMM for small
    // RowMajor f32/f64 matrices where Goto/BLIS overhead dominates compute.
    // Threshold: m*n*k < 32M elements (e.g. N=256 cube = 16M). Empirical:
    // above this, packed BLIS path wins; below, direct path wins.
    // Wrapped in `if constexpr` so the dispatch is fully elided for
    // ColMajor / Complex / other non-eligible instantiations (avoids C4702
    // "unreachable code" warnings under MSVC /WX).
    if constexpr (L == Layout::RowMajor &&
                  (std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>))
    {
        // Threshold tuned empirically on the 14900K vs-reference shootout:
        // - 32M elements catches N=256 cube (16M) — biggest win there.
        // - 200M elements catches N=512 cube (134M) — closes 0.95x→1.0x+ gap.
        // - At N=1024 (1G elements) and above, the packed BLIS path wins
        //   (better cache reuse via packed Ac dominates the savings).
        constexpr crd::usize small_gemm_threshold = 200ULL * 1024ULL * 1024ULL;
        if (a.rows() * b.cols() * a.cols() < small_gemm_threshold &&
            small_gemm_eligible(a, b, c, trans_a, trans_b))
        {
            small_gemm_parallel<T, L>(num_workers, alpha, a, b, beta, c, scratch);
            return;
        }
    }

    const crd::usize m = (trans_a == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans_a == Trans::None) ? a.cols() : a.rows();
    [[maybe_unused]] const crd::usize k2 = (trans_b == Trans::None) ? b.rows() : b.cols();
    const crd::usize n = (trans_b == Trans::None) ? b.cols() : b.rows();
    CRD_ASSERT_MSG(k == k2, "gemm_parallel: inner dimensions must match");
    CRD_ASSERT_MSG(c.rows() == m && c.cols() == n, "gemm_parallel: C must be m*n");

    // Scale C by beta on the calling thread. Parallel beta scaling was
    // measured net-negative across all sizes due to parallel_for fiber
    // overhead being comparable to or exceeding the memory-bandwidth-bound
    // scale work. Reverted to serial. Beta scale is at most O(m*n) and
    // negligible compared to GEMM's O(m*n*k) inner work for k >= 64.
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            view_at_ref(c, i, j) = beta * view_at_ref(c, i, j);
        }
    }

    // Auto-tune Mc (v0d-small-gemm-fastpath): default kMc=120 was set for
    // L2-cache fit at large M, but it gives `num_ic = ceil(m/120)` chunks
    // per (jc, pc), which leaves workers idle when m is small. E.g. at
    // m=256 with kMc=120: num_ic=3 → only 3 of 16 workers active.
    // Pick a smaller Mc such that num_ic >= num_workers (capped above by
    // kMc to preserve cache reuse for large m). Round up to a multiple of
    // Mr so the packed-A layout stays clean.
    const crd::usize eff_mc = [&]() {
        const crd::usize ideal = (m + num_workers - 1U) / num_workers;
        const crd::usize rounded =
            ((ideal + detail::GemmTraits<T>::MR - 1U) / detail::GemmTraits<T>::MR) * detail::GemmTraits<T>::MR;
        const crd::usize floored = std::max<crd::usize>(detail::GemmTraits<T>::MR, rounded);
        return std::min<crd::usize>(floored, kMc);
    }();

    // Per-worker Ac scratch + single shared Bc scratch.
    //
    // Indexing rationale: `parallel_for(num_ic, num_workers, ...)` is the
    // CHUNKING factor; the actual worker thread that runs each chunk can be
    // any of `crd::jobs::num_workers()`. `worker_index() % num_workers`
    // would alias two threads onto the same buffer → corrupted packed-A.
    // Allocate one buffer per ACTUAL worker thread and index by
    // `worker_index()` directly.
    const crd::u32 total_workers = crd::jobs::num_workers();
crd::memory::IAllocator* alloc = scratch;
    if (alloc == nullptr)
    {
        // No caller allocator (discouraged -- feedback_hesap_propagate_allocator). Fall back to a
        // PER-THREAD growable pool, never MallocAllocator (no malloc allocators in the engine).
        thread_local crd::memory::GrowableTlsfAllocator s_fallback;
        alloc = &s_fallback;
    }
    // Size to MIN(actual_dim, macro_block) — same reasoning as gemm().
    const crd::usize ap_kc = std::min<crd::usize>(k, kKc);
    const crd::usize bp_nc = std::min<crd::usize>(n, kNc);
    const crd::usize a_pack_per_worker =
        ((eff_mc + detail::GemmTraits<T>::MR - 1) / detail::GemmTraits<T>::MR) * detail::GemmTraits<T>::MR * ap_kc;
    const crd::usize b_pack_capacity =
        ((bp_nc + detail::GemmTraits<T>::NR - 1) / detail::GemmTraits<T>::NR) * ap_kc * detail::GemmTraits<T>::NR;
    const crd::usize align = alignof(T) > 32 ? alignof(T) : 32;
    auto* a_pack_pool =
        static_cast<T*>(alloc->allocate(a_pack_per_worker * total_workers * sizeof(T), align));
    auto* b_pack = static_cast<T*>(alloc->allocate(b_pack_capacity * sizeof(T), align));

    // Shared task state. The fiber-SBO budget is 41 bytes total per Task, so the
    // lambda can only capture ~33 bytes (after begin/end). Bundle everything
    // into a stack-allocated struct and capture only the pointer (8 bytes).
    // Lifetime: wait() blocks the dispatching thread on the same stack frame.
    struct IcLoopState
    {
        MatrixView<const T, L> a;
        MatrixView<T, L> c;
        T* a_pack_pool;
        const T* b_pack;
        crd::usize a_pack_per_worker;
        crd::usize m, kc, pc, jc, nc;
        crd::usize eff_mc;
        T alpha;
        Trans trans_a;
    };
    IcLoopState state{};
    state.a = a;
    state.c = c;
    state.a_pack_pool = a_pack_pool;
    state.b_pack = b_pack;
    state.a_pack_per_worker = a_pack_per_worker;
    state.m = m;
    state.alpha = alpha;
    state.trans_a = trans_a;

    // Outer jc + pc loops sequential; ic loop parallelized inside.
    // pack_b is kept serial: parallel pack_b was measured net-negative due
    // to cache locality (workers warm cache lines that the inner loop on a
    // different worker then needs to fetch). The serial pack_b at N=4096
    // f64 costs ~5ms total across all (jc,pc) iters — <2% of GEMM wall.
    for (crd::usize jc = 0; jc < n; jc += kNc)
    {
        const crd::usize nc = (jc + kNc < n) ? kNc : (n - jc);
        for (crd::usize pc = 0; pc < k; pc += kKc)
        {
            const crd::usize kc = (pc + kKc < k) ? kKc : (k - pc);

            detail::pack_b(b, pc, jc, kc, nc, trans_b, b_pack);

            const crd::u32 num_ic = static_cast<crd::u32>((m + eff_mc - 1) / eff_mc);
            if (num_ic == 0)
            {
                continue;
            }

            // Update per-(jc, pc) state then fire workers.
            state.kc = kc;
            state.pc = pc;
            state.jc = jc;
            state.nc = nc;
            state.eff_mc = eff_mc;

            IcLoopState* st = &state;
            auto* counter = crd::jobs::parallel_for(
                num_ic, num_workers,
                [st](crd::u32 begin, crd::u32 end)
                {
                    const crd::u32 worker_id = crd::jobs::worker_index();
                    T* a_pack = st->a_pack_pool + worker_id * st->a_pack_per_worker;
                    for (crd::u32 t = begin; t < end; ++t)
                    {
                        const crd::usize ic_local =
                            static_cast<crd::usize>(t) * st->eff_mc;
                        const crd::usize mc = (ic_local + st->eff_mc < st->m) ? st->eff_mc
                                                                              : (st->m - ic_local);
                        detail::pack_a(st->a, ic_local, st->pc, mc, st->kc, st->trans_a, a_pack);
                        detail::gemm_packed_inner(st->alpha, ic_local, st->jc, mc, st->nc, st->kc,
                                                  a_pack, st->b_pack, st->c);
                    }
                });
            crd::jobs::wait(counter);
            // No frame_reset here — it would invalidate frame_alloc state the
            // CALLER may hold. Per-call frame footprint at N=4096 is ~24 KB
            // (3 parallel_for x 32 outer iters x ~256 B JobDecl arrays),
            // well within the 1 MB default arena.
        }
    }

    alloc->deallocate(a_pack_pool);
    alloc->deallocate(b_pack);
}

// =======================================================================
// gemm_parallel_auto — heuristic worker-count picker (v0d-parallelism-auto-
// dispatch). For tiny matrices (mnk < 256K), parallel_for overhead exceeds
// the work → serial. Above that, use all available workers and let the
// internal Mc auto-tune + small-gemm fast-path pick the right block layout.
//
// Empirically validated on i9-14900K (8 P + 16 E cores, 32 logical
// threads): the bench's "best of {1, 8, 16, 24, 32}" picker chose nw=
// num_workers() for N≥1024 and nw=8 for very small N. The 256K threshold
// captures the second case (N=64 cube = 262K elements is right at the
// boundary).
// =======================================================================
template <typename T, Layout L>
void gemm_parallel_auto(T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta,
                        MatrixView<T, L> c, Trans trans_a, Trans trans_b,
                        crd::memory::IAllocator* scratch)
{
    constexpr crd::usize serial_threshold = 256ULL * 1024ULL;
    const crd::usize mnk = a.rows() * b.cols() * a.cols();
    const crd::u32 num_workers = (mnk < serial_threshold) ? 1U : crd::jobs::num_workers();
    gemm_parallel<T, L>(num_workers, alpha, a, b, beta, c, trans_a, trans_b, scratch);
}

// =======================================================================
// syrk — C = alpha * A * A^T + beta * C (real, symmetric).
// For trans=None: A is m×k, C is m×m. trans=Transpose: A is k×m, C is m×m.
// Only lower triangle of C is updated.
// =======================================================================
template <typename T>
void syrk(T alpha, MatrixView<const T, Layout::RowMajor> a, T beta, Symmetric<T>& c, Trans trans)
{
    const crd::usize m = (trans == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans == Trans::None) ? a.cols() : a.rows();
    CRD_ASSERT_MSG(c.n() == m, "syrk: C must be m×m");

    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            T sum{};
            for (crd::usize p = 0; p < k; ++p)
            {
                const T a_ip = (trans == Trans::None) ? view_at(a, i, p) : view_at(a, p, i);
                const T a_jp = (trans == Trans::None) ? view_at(a, j, p) : view_at(a, p, j);
                sum = sum + a_ip * a_jp;
            }
            c.at(i, j) = beta * c.at(i, j) + alpha * sum;
        }
    }
}

// =======================================================================
// herk — C = alpha * A * A^H + beta * C (Hermitian; alpha REAL).
// =======================================================================
template <typename T>
void herk(T alpha, MatrixView<const Complex<T>, Layout::RowMajor> a, T beta,
          Hermitian<Complex<T>>& c, Trans trans)
{
    const crd::usize m = (trans == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans == Trans::None) ? a.cols() : a.rows();
    CRD_ASSERT_MSG(c.n() == m, "herk: C must be m×m");
    const Complex<T> beta_c{beta, T{}};
    const Complex<T> alpha_c{alpha, T{}};

    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            Complex<T> sum{};
            for (crd::usize p = 0; p < k; ++p)
            {
                // None: A * A^H => sum_p A[i,p] * conj(A[j,p])
                // Trans-style (ConjTranspose treated): sum_p conj(A[p,i]) * A[p,j]
                if (trans == Trans::None)
                {
                    sum = sum + view_at(a, i, p) * crd::hesap::conj(view_at(a, j, p));
                }
                else
                {
                    sum = sum + crd::hesap::conj(view_at(a, p, i)) * view_at(a, p, j);
                }
            }
            c.at_lower(i, j) = beta_c * c.at_lower(i, j) + alpha_c * sum;
        }
    }
}

// =======================================================================
// syr2k — C = alpha * (A*B^T + B*A^T) + beta * C (real).
// =======================================================================
template <typename T>
void syr2k(T alpha, MatrixView<const T, Layout::RowMajor> a, MatrixView<const T, Layout::RowMajor> b,
           T beta, Symmetric<T>& c, Trans trans)
{
    const crd::usize m = (trans == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans == Trans::None) ? a.cols() : a.rows();
    CRD_ASSERT_MSG(c.n() == m, "syr2k: C must be m×m");

    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            T sum{};
            for (crd::usize p = 0; p < k; ++p)
            {
                const T a_ip = (trans == Trans::None) ? view_at(a, i, p) : view_at(a, p, i);
                const T b_jp = (trans == Trans::None) ? view_at(b, j, p) : view_at(b, p, j);
                const T b_ip = (trans == Trans::None) ? view_at(b, i, p) : view_at(b, p, i);
                const T a_jp = (trans == Trans::None) ? view_at(a, j, p) : view_at(a, p, j);
                sum = sum + a_ip * b_jp + b_ip * a_jp;
            }
            c.at(i, j) = beta * c.at(i, j) + alpha * sum;
        }
    }
}

// =======================================================================
// her2k — C = alpha * A*B^H + conj(alpha) * B*A^H + beta * C.
// =======================================================================
template <typename T>
void her2k(Complex<T> alpha, MatrixView<const Complex<T>, Layout::RowMajor> a,
           MatrixView<const Complex<T>, Layout::RowMajor> b, T beta, Hermitian<Complex<T>>& c,
           Trans trans)
{
    const crd::usize m = (trans == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans == Trans::None) ? a.cols() : a.rows();
    CRD_ASSERT_MSG(c.n() == m, "her2k: C must be m×m");
    const Complex<T> beta_c{beta, T{}};
    const Complex<T> alpha_conj = crd::hesap::conj(alpha);

    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            Complex<T> ab{};
            Complex<T> ba{};
            for (crd::usize p = 0; p < k; ++p)
            {
                if (trans == Trans::None)
                {
                    ab = ab + view_at(a, i, p) * crd::hesap::conj(view_at(b, j, p));
                    ba = ba + view_at(b, i, p) * crd::hesap::conj(view_at(a, j, p));
                }
                else
                {
                    ab = ab + crd::hesap::conj(view_at(a, p, i)) * view_at(b, p, j);
                    ba = ba + crd::hesap::conj(view_at(b, p, i)) * view_at(a, p, j);
                }
            }
            c.at_lower(i, j) = beta_c * c.at_lower(i, j) + alpha * ab + alpha_conj * ba;
        }
    }
}

// =======================================================================
// trmm — B = alpha * op(A) * B (in-place B). A triangular.
// =======================================================================
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trmm(T alpha, const Triangular<T, Side, Diag>& a, MatrixView<T, Layout::RowMajor> b, Trans trans)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(b.rows() == n, "trmm: B must have n rows");
    const crd::usize cols = b.cols();
    const bool do_conj = (trans == Trans::ConjTranspose);

    // Process each column of B independently with the existing trmv logic
    // pattern (in-place; traverse so unread values aren't clobbered).
    if (trans == Trans::None)
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize ii = n; ii-- > 0;)
                {
                    T sum{};
                    for (crd::usize p = 0; p <= ii; ++p)
                    {
                        sum = sum + a.at_value(ii, p) * b.data()[p * b.ld() + jc];
                    }
                    b.data()[ii * b.ld() + jc] = alpha * sum;
                }
            }
        }
        else
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    T sum{};
                    for (crd::usize p = i; p < n; ++p)
                    {
                        sum = sum + a.at_value(i, p) * b.data()[p * b.ld() + jc];
                    }
                    b.data()[i * b.ld() + jc] = alpha * sum;
                }
            }
        }
    }
    else
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    T sum{};
                    for (crd::usize p = i; p < n; ++p)
                    {
                        sum = sum + maybe_conj<T>(a.at_value(p, i), do_conj) * b.data()[p * b.ld() + jc];
                    }
                    b.data()[i * b.ld() + jc] = alpha * sum;
                }
            }
        }
        else
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize ii = n; ii-- > 0;)
                {
                    T sum{};
                    for (crd::usize p = 0; p <= ii; ++p)
                    {
                        sum = sum + maybe_conj<T>(a.at_value(p, ii), do_conj) * b.data()[p * b.ld() + jc];
                    }
                    b.data()[ii * b.ld() + jc] = alpha * sum;
                }
            }
        }
    }
}

// =======================================================================
// trsm — B = alpha * op(A)^-1 * B  (in-place). A triangular.
// =======================================================================
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trsm(T alpha, const Triangular<T, Side, Diag>& a, MatrixView<T, Layout::RowMajor> b, Trans trans)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(b.rows() == n, "trsm: B must have n rows");
    const crd::usize cols = b.cols();
    const bool do_conj = (trans == Trans::ConjTranspose);

    // First scale B by alpha.
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < cols; ++j)
        {
            b.data()[i * b.ld() + j] = alpha * b.data()[i * b.ld() + j];
        }
    }

    // Solve op(A) * X = B' (where B' is the scaled B above), column-by-column.
    if (trans == Trans::None)
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    T s = b.data()[i * b.ld() + jc];
                    for (crd::usize p = 0; p < i; ++p)
                    {
                        s = s - a.at_value(i, p) * b.data()[p * b.ld() + jc];
                    }
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        b.data()[i * b.ld() + jc] = s;
                    }
                    else
                    {
                        b.data()[i * b.ld() + jc] = s / a.at_value(i, i);
                    }
                }
            }
        }
        else
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize ii = n; ii-- > 0;)
                {
                    T s = b.data()[ii * b.ld() + jc];
                    for (crd::usize p = ii + 1; p < n; ++p)
                    {
                        s = s - a.at_value(ii, p) * b.data()[p * b.ld() + jc];
                    }
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        b.data()[ii * b.ld() + jc] = s;
                    }
                    else
                    {
                        b.data()[ii * b.ld() + jc] = s / a.at_value(ii, ii);
                    }
                }
            }
        }
    }
    else
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize ii = n; ii-- > 0;)
                {
                    T s = b.data()[ii * b.ld() + jc];
                    for (crd::usize p = ii + 1; p < n; ++p)
                    {
                        s = s - maybe_conj<T>(a.at_value(p, ii), do_conj) * b.data()[p * b.ld() + jc];
                    }
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        b.data()[ii * b.ld() + jc] = s;
                    }
                    else
                    {
                        b.data()[ii * b.ld() + jc] = s / maybe_conj<T>(a.at_value(ii, ii), do_conj);
                    }
                }
            }
        }
        else
        {
            for (crd::usize jc = 0; jc < cols; ++jc)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    T s = b.data()[i * b.ld() + jc];
                    for (crd::usize p = 0; p < i; ++p)
                    {
                        s = s - maybe_conj<T>(a.at_value(p, i), do_conj) * b.data()[p * b.ld() + jc];
                    }
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        b.data()[i * b.ld() + jc] = s;
                    }
                    else
                    {
                        b.data()[i * b.ld() + jc] = s / maybe_conj<T>(a.at_value(i, i), do_conj);
                    }
                }
            }
        }
    }
}

// =======================================================================
// gemm_mixed — mixed-precision GEMM. Reads A and B in TIn, accumulates
// and writes in TAcc.
// =======================================================================
template <typename TIn, typename TAcc, Layout L>
void gemm_mixed(TAcc alpha, MatrixView<const TIn, L> a, MatrixView<const TIn, L> b, TAcc beta,
                MatrixView<TAcc, L> c)
{
    const crd::usize m = a.rows();
    const crd::usize k = a.cols();
    const crd::usize n = b.cols();
    CRD_ASSERT_MSG(b.rows() == k, "gemm_mixed: inner dims must match");
    CRD_ASSERT_MSG(c.rows() == m && c.cols() == n, "gemm_mixed: C must be m×n");

    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            view_at_ref(c, i, j) = beta * view_at_ref(c, i, j);
        }
    }

    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            TAcc sum{};
            for (crd::usize p = 0; p < k; ++p)
            {
                const TAcc a_ip = static_cast<TAcc>(view_at<TIn, L>(a, i, p));
                const TAcc b_pj = static_cast<TAcc>(view_at<TIn, L>(b, p, j));
                sum = sum + a_ip * b_pj;
            }
            view_at_ref(c, i, j) = view_at_ref(c, i, j) + alpha * sum;
        }
    }
}

// =======================================================================
// Explicit instantiations
// =======================================================================

// gemm — 4 types × 2 layouts
template void gemm<crd::f32, Layout::RowMajor>(crd::f32, MatrixView<const crd::f32, Layout::RowMajor>,
    MatrixView<const crd::f32, Layout::RowMajor>, crd::f32, MatrixView<crd::f32, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<crd::f64, Layout::RowMajor>(crd::f64, MatrixView<const crd::f64, Layout::RowMajor>,
    MatrixView<const crd::f64, Layout::RowMajor>, crd::f64, MatrixView<crd::f64, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<Complex32, Layout::RowMajor>(Complex32, MatrixView<const Complex32, Layout::RowMajor>,
    MatrixView<const Complex32, Layout::RowMajor>, Complex32, MatrixView<Complex32, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<Complex64, Layout::RowMajor>(Complex64, MatrixView<const Complex64, Layout::RowMajor>,
    MatrixView<const Complex64, Layout::RowMajor>, Complex64, MatrixView<Complex64, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<crd::f32, Layout::ColMajor>(crd::f32, MatrixView<const crd::f32, Layout::ColMajor>,
    MatrixView<const crd::f32, Layout::ColMajor>, crd::f32, MatrixView<crd::f32, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<crd::f64, Layout::ColMajor>(crd::f64, MatrixView<const crd::f64, Layout::ColMajor>,
    MatrixView<const crd::f64, Layout::ColMajor>, crd::f64, MatrixView<crd::f64, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<Complex32, Layout::ColMajor>(Complex32, MatrixView<const Complex32, Layout::ColMajor>,
    MatrixView<const Complex32, Layout::ColMajor>, Complex32, MatrixView<Complex32, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm<Complex64, Layout::ColMajor>(Complex64, MatrixView<const Complex64, Layout::ColMajor>,
    MatrixView<const Complex64, Layout::ColMajor>, Complex64, MatrixView<Complex64, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);

// gemm_parallel — 4 types × 2 layouts (mirror of gemm instantiations)
template void gemm_parallel<crd::f32, Layout::RowMajor>(crd::u32, crd::f32,
    MatrixView<const crd::f32, Layout::RowMajor>, MatrixView<const crd::f32, Layout::RowMajor>,
    crd::f32, MatrixView<crd::f32, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<crd::f64, Layout::RowMajor>(crd::u32, crd::f64,
    MatrixView<const crd::f64, Layout::RowMajor>, MatrixView<const crd::f64, Layout::RowMajor>,
    crd::f64, MatrixView<crd::f64, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<Complex32, Layout::RowMajor>(crd::u32, Complex32,
    MatrixView<const Complex32, Layout::RowMajor>, MatrixView<const Complex32, Layout::RowMajor>,
    Complex32, MatrixView<Complex32, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<Complex64, Layout::RowMajor>(crd::u32, Complex64,
    MatrixView<const Complex64, Layout::RowMajor>, MatrixView<const Complex64, Layout::RowMajor>,
    Complex64, MatrixView<Complex64, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<crd::f32, Layout::ColMajor>(crd::u32, crd::f32,
    MatrixView<const crd::f32, Layout::ColMajor>, MatrixView<const crd::f32, Layout::ColMajor>,
    crd::f32, MatrixView<crd::f32, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<crd::f64, Layout::ColMajor>(crd::u32, crd::f64,
    MatrixView<const crd::f64, Layout::ColMajor>, MatrixView<const crd::f64, Layout::ColMajor>,
    crd::f64, MatrixView<crd::f64, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<Complex32, Layout::ColMajor>(crd::u32, Complex32,
    MatrixView<const Complex32, Layout::ColMajor>, MatrixView<const Complex32, Layout::ColMajor>,
    Complex32, MatrixView<Complex32, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel<Complex64, Layout::ColMajor>(crd::u32, Complex64,
    MatrixView<const Complex64, Layout::ColMajor>, MatrixView<const Complex64, Layout::ColMajor>,
    Complex64, MatrixView<Complex64, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);

// gemm_parallel_auto — same 4 types × 2 layouts.
template void gemm_parallel_auto<crd::f32, Layout::RowMajor>(crd::f32,
    MatrixView<const crd::f32, Layout::RowMajor>, MatrixView<const crd::f32, Layout::RowMajor>,
    crd::f32, MatrixView<crd::f32, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<crd::f64, Layout::RowMajor>(crd::f64,
    MatrixView<const crd::f64, Layout::RowMajor>, MatrixView<const crd::f64, Layout::RowMajor>,
    crd::f64, MatrixView<crd::f64, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<Complex32, Layout::RowMajor>(Complex32,
    MatrixView<const Complex32, Layout::RowMajor>, MatrixView<const Complex32, Layout::RowMajor>,
    Complex32, MatrixView<Complex32, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<Complex64, Layout::RowMajor>(Complex64,
    MatrixView<const Complex64, Layout::RowMajor>, MatrixView<const Complex64, Layout::RowMajor>,
    Complex64, MatrixView<Complex64, Layout::RowMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<crd::f32, Layout::ColMajor>(crd::f32,
    MatrixView<const crd::f32, Layout::ColMajor>, MatrixView<const crd::f32, Layout::ColMajor>,
    crd::f32, MatrixView<crd::f32, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<crd::f64, Layout::ColMajor>(crd::f64,
    MatrixView<const crd::f64, Layout::ColMajor>, MatrixView<const crd::f64, Layout::ColMajor>,
    crd::f64, MatrixView<crd::f64, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<Complex32, Layout::ColMajor>(Complex32,
    MatrixView<const Complex32, Layout::ColMajor>, MatrixView<const Complex32, Layout::ColMajor>,
    Complex32, MatrixView<Complex32, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);
template void gemm_parallel_auto<Complex64, Layout::ColMajor>(Complex64,
    MatrixView<const Complex64, Layout::ColMajor>, MatrixView<const Complex64, Layout::ColMajor>,
    Complex64, MatrixView<Complex64, Layout::ColMajor>, Trans, Trans, crd::memory::IAllocator*);

// syrk / syr2k (real)
template void syrk<crd::f32>(crd::f32, MatrixView<const crd::f32, Layout::RowMajor>, crd::f32,
    Symmetric<crd::f32>&, Trans);
template void syrk<crd::f64>(crd::f64, MatrixView<const crd::f64, Layout::RowMajor>, crd::f64,
    Symmetric<crd::f64>&, Trans);
template void syr2k<crd::f32>(crd::f32, MatrixView<const crd::f32, Layout::RowMajor>,
    MatrixView<const crd::f32, Layout::RowMajor>, crd::f32, Symmetric<crd::f32>&, Trans);
template void syr2k<crd::f64>(crd::f64, MatrixView<const crd::f64, Layout::RowMajor>,
    MatrixView<const crd::f64, Layout::RowMajor>, crd::f64, Symmetric<crd::f64>&, Trans);

// herk / her2k (complex)
template void herk<crd::f32>(crd::f32, MatrixView<const Complex32, Layout::RowMajor>, crd::f32,
    Hermitian<Complex32>&, Trans);
template void herk<crd::f64>(crd::f64, MatrixView<const Complex64, Layout::RowMajor>, crd::f64,
    Hermitian<Complex64>&, Trans);
template void her2k<crd::f32>(Complex32, MatrixView<const Complex32, Layout::RowMajor>,
    MatrixView<const Complex32, Layout::RowMajor>, crd::f32, Hermitian<Complex32>&, Trans);
template void her2k<crd::f64>(Complex64, MatrixView<const Complex64, Layout::RowMajor>,
    MatrixView<const Complex64, Layout::RowMajor>, crd::f64, Hermitian<Complex64>&, Trans);

// trmm / trsm — 4 types × 2 sides × Explicit diag
template void trmm<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>(crd::f32,
    const Triangular<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<crd::f32, Layout::RowMajor>, Trans);
template void trmm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(crd::f64,
    const Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<crd::f64, Layout::RowMajor>, Trans);
template void trmm<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>(Complex32,
    const Triangular<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<Complex32, Layout::RowMajor>, Trans);
template void trmm<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(Complex64,
    const Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<Complex64, Layout::RowMajor>, Trans);
template void trmm<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>(crd::f32,
    const Triangular<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<crd::f32, Layout::RowMajor>, Trans);
template void trmm<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(crd::f64,
    const Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<crd::f64, Layout::RowMajor>, Trans);
template void trmm<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>(Complex32,
    const Triangular<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<Complex32, Layout::RowMajor>, Trans);
template void trmm<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>(Complex64,
    const Triangular<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<Complex64, Layout::RowMajor>, Trans);

template void trsm<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>(crd::f32,
    const Triangular<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<crd::f32, Layout::RowMajor>, Trans);
template void trsm<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(crd::f64,
    const Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<crd::f64, Layout::RowMajor>, Trans);
template void trsm<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>(Complex32,
    const Triangular<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<Complex32, Layout::RowMajor>, Trans);
template void trsm<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(Complex64,
    const Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    MatrixView<Complex64, Layout::RowMajor>, Trans);
template void trsm<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>(crd::f32,
    const Triangular<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<crd::f32, Layout::RowMajor>, Trans);
template void trsm<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(crd::f64,
    const Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<crd::f64, Layout::RowMajor>, Trans);
template void trsm<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>(Complex32,
    const Triangular<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<Complex32, Layout::RowMajor>, Trans);
template void trsm<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>(Complex64,
    const Triangular<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    MatrixView<Complex64, Layout::RowMajor>, Trans);

// gemm_mixed — f32 in / f64 acc (the HPL-AI canonical instantiation)
template void gemm_mixed<crd::f32, crd::f64, Layout::RowMajor>(crd::f64,
    MatrixView<const crd::f32, Layout::RowMajor>, MatrixView<const crd::f32, Layout::RowMajor>,
    crd::f64, MatrixView<crd::f64, Layout::RowMajor>);

} // namespace crd::hesap::dense
