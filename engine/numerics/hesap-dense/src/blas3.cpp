#include <crd/hesap/dense/blas3.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/detail/gemm_pack.hpp>
#include <crd/hesap/dense/detail/pairwise_sum.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/construct.hpp> // allocate_array_or_fatal (DIAG.3a)
#include <crd/memory/memory.hpp>

namespace crd::hesap::dense
{

namespace
{
// Goto/BLIS cache-block defaults (alias detail::kGemmKc / Nc; a shape's Mc and Nc blocks are detail::gemm_mc and
// gemm_nc).
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

// C = beta C over its m by n elements. beta == 0 STORES zero (does NOT read C), the BLAS contract: when beta is zero,
// C need not be initialized on input. `0 * c` would propagate a NaN/Inf from uninitialized C (0*NaN=NaN); the
// store-zero path is bit-identical for finite C (0*finite=±0.0, and ±0.0+alpha·AB = alpha·AB) yet correct for
// uninitialized scratch, which is why supernodal scratch (ubuf) can be left uninitialized.
template <typename T, Layout L> void scale_by_beta(MatrixView<T, L> c, crd::usize m, crd::usize n, T beta) noexcept
{
    const bool zero = beta == T{0};
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T& e = view_at_ref(c, i, j);
            e = zero ? T{0} : beta * e;
        }
    }
}

// The caller's scratch allocator, or a PER-THREAD growable pool when it passes none (discouraged:
// feedback_hesap_propagate_allocator), never MallocAllocator (no malloc allocators in the engine).
[[nodiscard]] crd::memory::IAllocator& pack_allocator(crd::memory::IAllocator* scratch) noexcept
{
    if (scratch != nullptr)
    {
        return *scratch;
    }
    thread_local crd::memory::GrowableTlsfAllocator s_fallback;
    return s_fallback;
}

// n values rounded up to whole 64-byte lines, so a buffer carved after them starts on a line of its own.
template <typename T> [[nodiscard]] constexpr crd::usize whole_lines(crd::usize n) noexcept
{
    constexpr crd::usize per_line = 64U / sizeof(T);
    return (n + per_line - 1U) / per_line * per_line;
}

// One packing buffer of `count` values, line-aligned (DIAG.3a: a refusal is fatal).
template <typename T> [[nodiscard]] T* allocate_packs(crd::memory::IAllocator& alloc, crd::usize count)
{
    return crd::memory::allocate_array_or_fatal<T>(alloc, count, 64U, detail::kGemmPackOutOfMemory);
}

} // namespace

// =======================================================================
// small_gemm — the resident-A path for small f32/f64 GEMM, serial or parallel.
//
// v0d-small-gemm-fastpath: at small M*N*K, the Goto/BLIS pipeline's per-(jc, pc) A packing and its Mc-tile
// parallelism dominate the compute (~290 us of overhead against ~40 us at N=256 f64 on 16 P-threads). This path:
//   - reads A's whole row panels in place (op(A) effectively row-major: its rows at a + i * ld), packing only the
//     last partial panel, which the tile would otherwise read past the matrix;
//   - packs B once, every depth group of it (any layout and transpose), in one buffer with that panel;
//   - parallelizes over the row panels, not Mc tiles, so the work scales with m / mr (load balance on a hybrid CPU);
//   - sums the depth in the kKc groups gemm() sums by, beta folded into the first, so its bits equal the packed
//     path's at every worker count.
// Threshold tuned on the 14900K vs-reference shootout (parallel): 200M elements catches the N=512 cube (134M,
// 0.95x→1.0x+); from N=1024 (1G) the packed path wins (better cache reuse via packed Ac). The whole packed B must also
// fit the packed path's Bc budget (Kc by Nc), so a skinny product (m = 1, n = k = 10^4: 800 MB of f64) never packs a
// large B whole; every cube the threshold admits (k n <= 342K) fits.
// =======================================================================
constexpr crd::usize kSmallGemmThreshold = 200ULL * 1024ULL * 1024ULL;

template <typename T, Layout L>
[[nodiscard]] bool takes_small_gemm(Trans trans_a, crd::usize m, crd::usize n, crd::usize k) noexcept
{
    if constexpr (kHasDenseKernels<T>)
    {
        const GemmShape shape = detail::gemm_shape_of<T>();
        return (L == Layout::RowMajor) == (trans_a == Trans::None) && m * n * k < kSmallGemmThreshold &&
               detail::packed_b_size(shape, k, n) <= kKc * (kNc + shape.nr);
    }
    else
    {
        return false;
    }
}

template <typename T, Layout L> struct SmallGemm
{
    GemmSource<T> a; // op(A): row i at a.data + i * a.row_stride
    MatrixView<T, L> c;
    const T* b_pack; // depth group pc at b_pack + pc * b_row: its kc by n panels
    const T* a_tail; // the last partial row panel, packed (row stride k)
    T alpha;
    T beta;
    crd::usize m, n, k, mr, full, b_row; // rows [0, full) are read in place
};

// Rows [r0, r1) of C, r0 on a panel boundary: the whole panels as one block and the packed partial panel, per group.
template <typename T, Layout L> void small_gemm_rows(const SmallGemm<T, L>& s, crd::usize r0, crd::usize r1) noexcept
{
    const crd::usize resident_end = std::min(r1, s.full);
    for (crd::usize pc = 0; pc < s.k; pc += kKc)
    {
        const crd::usize kc = (pc + kKc < s.k) ? kKc : (s.k - pc);
        const GemmUpdate update = pc == 0 ? GemmUpdate::Scale : GemmUpdate::Add;
        const T* bg = s.b_pack + pc * s.b_row;
        if (resident_end > r0)
        {
            detail::gemm_packed_inner(s.alpha, s.beta, update, r0, 0, resident_end - r0, s.n, kc,
                                      s.a.data + r0 * s.a.row_stride + pc, s.a.row_stride, bg, s.c);
        }
        if (r1 > s.full)
        {
            detail::gemm_packed_inner(s.alpha, s.beta, update, s.full, 0, s.m - s.full, s.n, kc, s.a_tail + pc, s.k, bg,
                                      s.c);
        }
    }
}

// m, n and k are nonzero; num_workers <= 1 runs on the calling thread without crd::jobs.
template <typename T, Layout L>
void small_gemm(crd::u32 num_workers, T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta,
                MatrixView<T, L> c, Trans trans_a, Trans trans_b, crd::memory::IAllocator* scratch)
{
    const crd::usize m = c.rows();
    const crd::usize n = c.cols();
    const crd::usize k = (trans_a == Trans::None) ? a.cols() : a.rows();
    crd::memory::IAllocator& alloc = pack_allocator(scratch);
    const GemmShape shape = detail::gemm_shape_of<T>();
    const crd::usize b_row = detail::packed_b_size(shape, 1U, n);
    const crd::usize full = m - m % shape.mr;
    const crd::usize b_capacity = whole_lines<T>(b_row * k);
    T* b_pack = allocate_packs<T>(alloc, b_capacity + detail::packed_a_size(shape, m - full, k));
    T* a_tail = b_pack + b_capacity;
    for (crd::usize pc = 0; pc < k; pc += kKc)
    {
        detail::pack_b(b, pc, 0, (pc + kKc < k) ? kKc : (k - pc), n, trans_b, b_pack + pc * b_row);
    }
    if (full < m)
    {
        detail::pack_a(a, full, 0, m - full, k, trans_a, a_tail);
    }
    const SmallGemm<T, L> s{
        detail::gemm_source(a, 0, 0, trans_a), c, b_pack, a_tail, alpha, beta, m, n, k, shape.mr, full, b_row};
    if (num_workers <= 1)
    {
        small_gemm_rows(s, 0, m);
    }
    else
    {
        const SmallGemm<T, L>* sp = &s;
        // v5f-c (gemm-parallel-frame-arena-leak fix): reclaim this parallel_for's JobDecl array in place.
        const crd::usize frame_mark = crd::jobs::frame_get_mark();
        auto* counter =
            crd::jobs::parallel_for(static_cast<crd::u32>((m + shape.mr - 1U) / shape.mr), num_workers,
                                    [sp](crd::u32 begin, crd::u32 end)
                                    {
                                        small_gemm_rows(*sp, static_cast<crd::usize>(begin) * sp->mr,
                                                        std::min(static_cast<crd::usize>(end) * sp->mr, sp->m));
                                    });
        crd::jobs::wait(counter);
        crd::jobs::frame_set_mark(frame_mark); // reclaim the parallel_for JobDecls in place (leak fix)
    }
    alloc.deallocate(b_pack);
}

// =======================================================================
// gemm — Goto/BLIS 5-loop. v0d-perf (ADR-0082 intrinsics backend).
//
// Loop structure (outer-to-inner):
//   jc: outer column slab (Nc-wide)         — cache C
//   pc: outer K slab (Kc-deep)              — cache the K dimension; the depth grouping every path sums by
//   ic: outer row slab (Mc-tall)            — cache A panel into L2
//     pack Ac panel (mc × kc) and Bc panel (kc × nc) on the level's tile shape (detail::gemm_shape_of)
//     gemm_packed_inner: the GEMM block (mr × nr register tiles) on (Ac panel, Bc panel); the first group merges
//                        beta C in (GemmUpdate::Scale), so C is read and written once there
//
// Both packs come from one buffer of the `scratch` IAllocator (Matrix overload passes `a.allocator()`), sized to
// MIN(actual dim, cache block): at most (Mc + mr) Kc values of Ac and (Nc + nr) Kc of Bc. A small product whose
// op(A) is row-major takes small_gemm on this thread instead.
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
    if (m == 0 || n == 0)
    {
        return;
    }
    if (k == 0)
    {
        scale_by_beta(c, m, n, beta); // no depth group to fold beta into
        return;
    }
    if (takes_small_gemm<T, L>(trans_a, m, n, k))
    {
        small_gemm<T, L>(1U, alpha, a, b, beta, c, trans_a, trans_b, scratch);
        return;
    }

    crd::memory::IAllocator& alloc = pack_allocator(scratch);
    const GemmShape shape = detail::gemm_shape_of<T>();
    const crd::usize mc_block = detail::gemm_mc(shape);
    const crd::usize nc_block = detail::gemm_nc(shape);
    const crd::usize a_pack_capacity =
        whole_lines<T>(detail::packed_a_size(shape, std::min<crd::usize>(m, mc_block), std::min<crd::usize>(k, kKc)));
    const crd::usize b_pack_capacity =
        detail::packed_b_size(shape, std::min<crd::usize>(k, kKc), std::min<crd::usize>(n, nc_block));
    T* a_pack = allocate_packs<T>(alloc, a_pack_capacity + b_pack_capacity);
    T* b_pack = a_pack + a_pack_capacity;

    for (crd::usize jc = 0; jc < n; jc += nc_block)
    {
        const crd::usize nc = (jc + nc_block < n) ? nc_block : (n - jc);
        for (crd::usize pc = 0; pc < k; pc += kKc)
        {
            const crd::usize kc = (pc + kKc < k) ? kKc : (k - pc);
            const GemmUpdate update = pc == 0 ? GemmUpdate::Scale : GemmUpdate::Add;

            // Pack Bc panel once per (jc, pc) — reused across all ic blocks.
            // No explicit template args: T/L are deduced from b's MatrixView<const T, L>
            // type — avoids MSVC's `<T, L>` template-arg parsing ambiguity inside
            // a templated function body (sibling D27 v0c quirk).
            detail::pack_b(b, pc, jc, kc, nc, trans_b, b_pack);

            for (crd::usize ic = 0; ic < m; ic += mc_block)
            {
                const crd::usize mc = (ic + mc_block < m) ? mc_block : (m - ic);

                detail::pack_a(a, ic, pc, mc, kc, trans_a, a_pack);
                detail::gemm_packed_inner(alpha, beta, update, ic, jc, mc, nc, kc, a_pack, kc, b_pack, c);
            }
        }
    }

    alloc.deallocate(a_pack);
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

    const crd::usize m = (trans_a == Trans::None) ? a.rows() : a.cols();
    const crd::usize k = (trans_a == Trans::None) ? a.cols() : a.rows();
    [[maybe_unused]] const crd::usize k2 = (trans_b == Trans::None) ? b.rows() : b.cols();
    const crd::usize n = (trans_b == Trans::None) ? b.cols() : b.rows();
    CRD_ASSERT_MSG(k == k2, "gemm_parallel: inner dimensions must match");
    CRD_ASSERT_MSG(c.rows() == m && c.cols() == n, "gemm_parallel: C must be m*n");
    if (m == 0 || n == 0)
    {
        return;
    }
    if (k == 0)
    {
        scale_by_beta(c, m, n, beta); // no depth group to fold beta into
        return;
    }
    if (takes_small_gemm<T, L>(trans_a, m, n, k))
    {
        small_gemm<T, L>(num_workers, alpha, a, b, beta, c, trans_a, trans_b, scratch);
        return;
    }

    // Auto-tune Mc (v0d-small-gemm-fastpath): the shape's Mc block gives `num_ic = ceil(m / mc)` chunks per (jc, pc),
    // which leaves workers idle when m is small. Pick a smaller Mc such that num_ic >= num_workers (capped above by
    // the block to keep the cache reuse at large m), a multiple of the level's mr so the packed-A layout stays clean.
    const GemmShape shape = detail::gemm_shape_of<T>();
    const crd::usize nc_block = detail::gemm_nc(shape);
    const crd::usize eff_mc = [&]() {
        const crd::usize ideal = (m + num_workers - 1U) / num_workers;
        const crd::usize rounded = (ideal + shape.mr - 1U) / shape.mr * shape.mr;
        const crd::usize floored = std::max<crd::usize>(shape.mr, rounded);
        return std::min<crd::usize>(floored, detail::gemm_mc(shape));
    }();

    // Per-worker Ac scratch + single shared Bc scratch, in one buffer.
    //
    // Indexing rationale: `parallel_for(num_ic, num_workers, ...)` is the
    // CHUNKING factor; the actual worker thread that runs each chunk can be
    // any of `crd::jobs::num_workers()`. `worker_index() % num_workers`
    // would alias two threads onto the same buffer → corrupted packed-A.
    // Allocate one buffer per ACTUAL worker thread and index by
    // `worker_index()` directly; each starts on a cache line of its own.
    const crd::u32 total_workers = crd::jobs::num_workers();
    crd::memory::IAllocator& alloc = pack_allocator(scratch);
    // Size to MIN(actual_dim, macro_block) — same reasoning as gemm().
    const crd::usize ap_kc = std::min<crd::usize>(k, kKc);
    const crd::usize a_pack_per_worker = whole_lines<T>(detail::packed_a_size(shape, eff_mc, ap_kc));
    const crd::usize b_pack_capacity = detail::packed_b_size(shape, ap_kc, std::min<crd::usize>(n, nc_block));
    // One block-bounded panel per worker: a_pack_per_worker <= (gemm_mc + mr) * kKc + a line, so the product cannot
    // overflow.
    T* a_pack_pool = allocate_packs<T>(alloc, a_pack_per_worker * total_workers + b_pack_capacity);
    T* b_pack = a_pack_pool + a_pack_per_worker * total_workers;

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
        T beta;
        GemmUpdate update;
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
    state.beta = beta;
    state.trans_a = trans_a;

    // Outer jc + pc loops sequential; ic loop parallelized inside.
    // pack_b is kept serial: parallel pack_b was measured net-negative due
    // to cache locality (workers warm cache lines that the inner loop on a
    // different worker then needs to fetch). The serial pack_b at N=4096
    // f64 costs ~5ms total across all (jc,pc) iters — <2% of GEMM wall.
    //
    // v5f-c (gemm-parallel-frame-arena-leak fix): scope the per-iteration parallel_for JobDecl arrays to
    // this gemm call. Save the dispatching thread's frame-arena mark before the loop and restore it after
    // each wait() — reclaims only THIS call's JobDecls, leaving any caller frame_alloc state below the mark
    // intact (nest-safe, unlike frame_reset()). Fixes the cross-call arena exhaustion in heavy-gemm loops.
    const crd::usize frame_mark = crd::jobs::frame_get_mark();
    for (crd::usize jc = 0; jc < n; jc += nc_block)
    {
        const crd::usize nc = (jc + nc_block < n) ? nc_block : (n - jc);
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
            state.update = pc == 0 ? GemmUpdate::Scale : GemmUpdate::Add; // the first group merges beta C in

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
                        detail::gemm_packed_inner(st->alpha, st->beta, st->update, ic_local, st->jc, mc, st->nc, st->kc,
                                                  a_pack, st->kc, st->b_pack, st->c);
                    }
                });
            crd::jobs::wait(counter);
            // Reclaim this parallel_for's JobDecl array in place (back to the gemm-entry mark): bounded
            // arena use across the (jc,pc) iterations + no cross-call accumulation. The packed buffers come
            // from `alloc` (an IAllocator), not the frame arena, so they are untouched.
            crd::jobs::frame_set_mark(frame_mark);
        }
    }

    alloc.deallocate(a_pack_pool);
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

    // beta == 0 STORES zero (BLAS contract: C not read on input) — bit-identical for finite C, correct for
    // uninitialized scratch (see gemm() above).
    if (beta == TAcc{0})
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                view_at_ref(c, i, j) = TAcc{0};
            }
        }
    }
    else
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                view_at_ref(c, i, j) = beta * view_at_ref(c, i, j);
            }
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
