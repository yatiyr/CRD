#include <crd/hesap/dense/blas3.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/detail/gemm_arms.hpp>
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
// The Goto/BLIS depth block (alias detail::kGemmKc; a shape's Mc and Nc blocks are detail::gemm_mc and gemm_nc).
constexpr crd::usize kKc = detail::kGemmKc;

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

// C = beta C over rows [r0, r1) of its n columns. beta == 0 STORES zero (does NOT read C), the BLAS contract: when
// beta is zero, C need not be initialized on input. `0 * c` would propagate a NaN/Inf from uninitialized C
// (0*NaN=NaN); the store-zero path is bit-identical for finite C (0*finite=±0.0, and ±0.0+alpha·AB = alpha·AB) yet
// correct for uninitialized scratch, which is why supernodal scratch (ubuf) can be left uninitialized.
template <typename T, Layout L>
void scale_by_beta(MatrixView<T, L> c, crd::usize r0, crd::usize r1, crd::usize n, T beta) noexcept
{
    const bool zero = beta == T{0};
    for (crd::usize i = r0; i < r1; ++i)
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
// The path is taken below an m n k threshold where detail::gemm_resident_admits holds (every cube the thresholds
// admit, k n <= 342K, fits its B budget). The parallel one was tuned on the 14900K vs-reference shootout at 16 threads:
// 200M elements catches the N=512 cube (134M, 0.95x→1.0x+); from N=1024 (1G) the packed path wins (better cache reuse
// via packed Ac); it is detail::GemmParallelPick's resident_mnk. The serial one keeps that value until the one-thread
// board (SIMD-DISPATCH) sets it from both paths' rows at each size (detail::gemm_arm).
// =======================================================================
constexpr crd::usize kSerialResidentThreshold = 200ULL * 1024ULL * 1024ULL;

template <typename T, Layout L>
[[nodiscard]] bool takes_small_gemm(const GemmKernels<T>& kernels, crd::usize threshold, Trans trans_a, crd::usize m,
                                    crd::usize n, crd::usize k) noexcept
{
    return m * n * k < threshold && detail::gemm_resident_admits<T, L>(kernels, trans_a, n, k);
}

// op(B)'s columns [jc, jc + nc) over depth [pc, pc + kc) as Bc panels in kKc depth groups, group g at
// out + g * group_stride (gemm_packed_inner's layout: an nr-column panel at out + col * depth).
template <typename T, Layout L> struct PackB
{
    const GemmKernels<T>* kernels;
    MatrixView<const T, L> b;
    T* out;
    crd::usize pc, kc, jc, nc, group_stride, nr;
    Trans trans_b;
};

template <typename T, Layout L> void pack_b_columns(const PackB<T, L>& s, crd::usize p0, crd::usize p1) noexcept
{
    const crd::usize c0 = p0 * s.nr;
    const crd::usize c1 = std::min(p1 * s.nr, s.nc);
    T* group = s.out;
    for (crd::usize pc = s.pc; pc < s.pc + s.kc; pc += kKc)
    {
        const crd::usize kc = std::min(kKc, s.pc + s.kc - pc);
        detail::pack_b(*s.kernels, s.b, pc, s.jc + c0, kc, c1 - c0, s.trans_b, group + c0 * kc);
        group += s.group_stride;
    }
}

// Every panel of s, split into num_workers jobs of whole panels (disjoint writes: the serial pack's bits) or serial.
template <typename T, Layout L> void pack_b_split(const PackB<T, L>& s, detail::GemmSplit split, crd::u32 num_workers)
{
    const crd::usize panels = (s.nc + s.nr - 1U) / s.nr;
    if (split == detail::GemmSplit::Serial || num_workers <= 1 || panels <= 1)
    {
        pack_b_columns(s, 0, panels);
        return;
    }
    const PackB<T, L>* sp = &s;
    auto* counter = crd::jobs::parallel_for(static_cast<crd::u32>(panels), num_workers,
                                            [sp](crd::u32 begin, crd::u32 end) { pack_b_columns(*sp, begin, end); });
    crd::jobs::wait(counter);
}

template <typename T, Layout L> struct SmallGemm
{
    const GemmKernels<T>* kernels;
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
            detail::gemm_packed_inner(*s.kernels, s.alpha, s.beta, update, r0, 0, resident_end - r0, s.n, kc,
                                      s.a.data + r0 * s.a.row_stride + pc, s.a.row_stride, bg, s.c);
        }
        if (r1 > s.full)
        {
            detail::gemm_packed_inner(*s.kernels, s.alpha, s.beta, update, s.full, 0, s.m - s.full, s.n, kc,
                                      s.a_tail + pc, s.k, bg, s.c);
        }
    }
}

// m, n and k are nonzero; num_workers <= 1 runs on the calling thread without crd::jobs.
template <typename T, Layout L>
void small_gemm(const GemmKernels<T>& kernels, crd::u32 num_workers, detail::GemmSplit pack_split, T alpha,
                MatrixView<const T, L> a, MatrixView<const T, L> b, T beta, MatrixView<T, L> c, Trans trans_a,
                Trans trans_b, crd::memory::IAllocator* scratch)
{
    const crd::usize m = c.rows();
    const crd::usize n = c.cols();
    const crd::usize k = (trans_a == Trans::None) ? a.cols() : a.rows();
    crd::memory::IAllocator& alloc = pack_allocator(scratch);
    const GemmShape shape = kernels.shape;
    const crd::usize b_row = detail::packed_b_size(shape, 1U, n);
    const crd::usize full = m - m % shape.mr;
    const crd::usize b_capacity = whole_lines<T>(b_row * k);
    T* b_pack = allocate_packs<T>(alloc, b_capacity + detail::packed_a_size(shape, m - full, k));
    T* a_tail = b_pack + b_capacity;
    const PackB<T, L> pack{.kernels = &kernels,
                           .b = b,
                           .out = b_pack,
                           .pc = 0,
                           .kc = k,
                           .jc = 0,
                           .nc = n,
                           .group_stride = kKc * b_row,
                           .nr = shape.nr,
                           .trans_b = trans_b};
    pack_b_split(pack, pack_split, num_workers);
    if (full < m)
    {
        detail::pack_a(kernels, a, full, 0, m - full, k, trans_a, a_tail);
    }
    const SmallGemm<T, L> s{.kernels = &kernels,
                            .a = detail::gemm_source(a, 0, 0, trans_a),
                            .c = c,
                            .b_pack = b_pack,
                            .a_tail = a_tail,
                            .alpha = alpha,
                            .beta = beta,
                            .m = m,
                            .n = n,
                            .k = k,
                            .mr = shape.mr,
                            .full = full,
                            .b_row = b_row};
    if (num_workers <= 1)
    {
        small_gemm_rows(s, 0, m);
    }
    else
    {
        const SmallGemm<T, L>* sp = &s;
        auto* counter =
            crd::jobs::parallel_for(static_cast<crd::u32>((m + shape.mr - 1U) / shape.mr), num_workers,
                                    [sp](crd::u32 begin, crd::u32 end)
                                    {
                                        small_gemm_rows(*sp, static_cast<crd::usize>(begin) * sp->mr,
                                                        std::min(static_cast<crd::usize>(end) * sp->mr, sp->m));
                                    });
        crd::jobs::wait(counter);
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
//     pack Ac panel (mc × kc) and Bc panel (kc × nc) on the tile shape of the GEMM kernels it runs
//     gemm_packed_inner: the GEMM block (mr × nr register tiles) on (Ac panel, Bc panel); the first group merges
//                        beta C in (GemmUpdate::Scale), so C is read and written once there
//
// Both packs come from one buffer of the `scratch` IAllocator (Matrix overload passes `a.allocator()`), sized to
// MIN(actual dim, cache block): at most (Mc + mr) Kc values of Ac and (Nc + nr) Kc of Bc. A small product whose
// op(A) is row-major takes small_gemm on this thread instead (GemmPath::Auto; detail::gemm_arm forces a path).
// =======================================================================
namespace
{
template <typename T, Layout L>
void gemm_serial(const GemmKernels<T>& kernels, detail::GemmPath path, T alpha, MatrixView<const T, L> a,
                 MatrixView<const T, L> b, T beta, MatrixView<T, L> c, Trans trans_a, Trans trans_b,
                 crd::memory::IAllocator* scratch)
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
        scale_by_beta(c, 0, m, n, beta); // no depth group to fold beta into
        return;
    }
    CRD_ASSERT_MSG((path != detail::GemmPath::Resident || detail::gemm_resident_admits<T, L>(kernels, trans_a, n, k)),
                   "gemm: the resident-A path does not admit this product");
    if (path == detail::GemmPath::Resident ||
        (path == detail::GemmPath::Auto && takes_small_gemm<T, L>(kernels, kSerialResidentThreshold, trans_a, m, n, k)))
    {
        small_gemm<T, L>(kernels, 1U, detail::GemmSplit::Serial, alpha, a, b, beta, c, trans_a, trans_b, scratch);
        return;
    }

    crd::memory::IAllocator& alloc = pack_allocator(scratch);
    const GemmShape shape = kernels.shape;
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
            detail::pack_b(kernels, b, pc, jc, kc, nc, trans_b, b_pack);

            for (crd::usize ic = 0; ic < m; ic += mc_block)
            {
                const crd::usize mc = (ic + mc_block < m) ? mc_block : (m - ic);

                detail::pack_a(kernels, a, ic, pc, mc, kc, trans_a, a_pack);
                detail::gemm_packed_inner(kernels, alpha, beta, update, ic, jc, mc, nc, kc, a_pack, kc, b_pack, c);
            }
        }
    }

    alloc.deallocate(a_pack);
}
} // namespace

template <typename T, Layout L>
void gemm(T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta, MatrixView<T, L> c, Trans trans_a,
          Trans trans_b, crd::memory::IAllocator* scratch)
{
    gemm_serial<T, L>(detail::gemm_kernels_of<T>(), detail::GemmPath::Auto, alpha, a, b, beta, c, trans_a, trans_b,
                      scratch);
}

template <typename T, Layout L>
void detail::gemm_arm(const GemmKernels<T>& k, GemmPath path, T alpha, MatrixView<const T, L> a,
                      MatrixView<const T, L> b, T beta, MatrixView<T, L> c, Trans trans_a, Trans trans_b,
                      crd::memory::IAllocator* scratch)
{
    gemm_serial<T, L>(k, path, alpha, a, b, beta, c, trans_a, trans_b, scratch);
}

// =======================================================================
// gemm_parallel — BLIS-style ic-loop parallelism over crd::jobs workers, on detail::GemmParallelPick's levers.
// =======================================================================
template <typename T, Layout L>
void detail::gemm_parallel_arm(const GemmParallelPick& pick, crd::u32 num_workers, T alpha, MatrixView<const T, L> a,
                               MatrixView<const T, L> b, T beta, MatrixView<T, L> c, Trans trans_a, Trans trans_b,
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
    if (k == 0) // no depth group to fold beta into
    {
        if (pick.beta_only == GemmSplit::Serial)
        {
            scale_by_beta(c, 0, m, n, beta);
            return;
        }
        struct BetaRows
        {
            MatrixView<T, L> c;
            crd::usize n;
            T beta;
        };
        const BetaRows rows{.c = c, .n = n, .beta = beta};
        const BetaRows* rp = &rows;
        auto* counter =
            crd::jobs::parallel_for(static_cast<crd::u32>(m), num_workers, [rp](crd::u32 begin, crd::u32 end)
                                    { scale_by_beta(rp->c, begin, end, rp->n, rp->beta); });
        crd::jobs::wait(counter);
        return;
    }
    const GemmKernels<T>& kernels = detail::gemm_kernels_of<T>();
    if (takes_small_gemm<T, L>(kernels, pick.resident_mnk, trans_a, m, n, k))
    {
        small_gemm<T, L>(kernels, num_workers, pick.pack_b, alpha, a, b, beta, c, trans_a, trans_b, scratch);
        return;
    }

    // Auto-tune Mc (v0d-small-gemm-fastpath, GemmMcSplit::PerWorker): the shape's Mc block gives
    // `num_ic = ceil(m / mc)` chunks per (jc, pc), which leaves workers idle when m is small. Pick a smaller Mc such
    // that num_ic >= num_workers (capped above by the block to keep the cache reuse at large m), a multiple of the
    // level's mr so the packed-A layout stays clean.
    const GemmShape shape = kernels.shape;
    const crd::usize nc_block = detail::gemm_nc(shape);
    const crd::usize eff_mc = [&]() {
        if (pick.mc == GemmMcSplit::Block)
        {
            return detail::gemm_mc(shape);
        }
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
        const GemmKernels<T>* kernels;
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
    state.kernels = &kernels;
    state.a = a;
    state.c = c;
    state.a_pack_pool = a_pack_pool;
    state.b_pack = b_pack;
    state.a_pack_per_worker = a_pack_per_worker;
    state.m = m;
    state.alpha = alpha;
    state.beta = beta;
    state.trans_a = trans_a;

    // Outer jc + pc loops sequential; ic loop parallelized inside. pack_b stays serial by default (GemmSplit): a
    // parallel pack_b was measured net-negative in v0d for cache locality (workers warm lines that the inner loop on a
    // different worker then fetches; the serial pack at N=4096 f64 is ~5 ms, <2% of the wall), a claim the
    // thread-sweep board re-measures from the forced arm.
    for (crd::usize jc = 0; jc < n; jc += nc_block)
    {
        const crd::usize nc = (jc + nc_block < n) ? nc_block : (n - jc);
        for (crd::usize pc = 0; pc < k; pc += kKc)
        {
            const crd::usize kc = (pc + kKc < k) ? kKc : (k - pc);
            const PackB<T, L> pack{.kernels = &kernels,
                                   .b = b,
                                   .out = b_pack,
                                   .pc = pc,
                                   .kc = kc,
                                   .jc = jc,
                                   .nc = nc,
                                   .group_stride = 0,
                                   .nr = shape.nr,
                                   .trans_b = trans_b};
            pack_b_split(pack, pick.pack_b, num_workers);

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
                        detail::pack_a(*st->kernels, st->a, ic_local, st->pc, mc, st->kc, st->trans_a, a_pack);
                        detail::gemm_packed_inner(*st->kernels, st->alpha, st->beta, st->update, ic_local, st->jc, mc,
                                                  st->nc, st->kc, a_pack, st->kc, st->b_pack, st->c);
                    }
                });
            crd::jobs::wait(counter);
        }
    }

    alloc.deallocate(a_pack_pool);
}

template <typename T, Layout L>
void gemm_parallel(crd::u32 num_workers, T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta,
                   MatrixView<T, L> c, Trans trans_a, Trans trans_b, crd::memory::IAllocator* scratch)
{
    detail::gemm_parallel_arm<T, L>(detail::GemmParallelPick{}, num_workers, alpha, a, b, beta, c, trans_a, trans_b,
                                    scratch);
}

// =======================================================================
// gemm_parallel_auto — heuristic worker-count picker (v0d-parallelism-auto-
// dispatch). For tiny products (m n k < 256K), parallel_for overhead exceeds
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
    constexpr crd::usize serial_mnk = 256ULL * 1024ULL;
    const crd::usize k = (trans_a == Trans::None) ? a.cols() : a.rows();
    const crd::u32 num_workers = (c.rows() * c.cols() * k < serial_mnk) ? 1U : crd::jobs::num_workers();
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

// detail::gemm_arm — the real types × 2 layouts
template void detail::gemm_arm<crd::f32, Layout::RowMajor>(const GemmKernels<crd::f32>&, detail::GemmPath, crd::f32,
                                                           MatrixView<const crd::f32, Layout::RowMajor>,
                                                           MatrixView<const crd::f32, Layout::RowMajor>, crd::f32,
                                                           MatrixView<crd::f32, Layout::RowMajor>, Trans, Trans,
                                                           crd::memory::IAllocator*);
template void detail::gemm_arm<crd::f64, Layout::RowMajor>(const GemmKernels<crd::f64>&, detail::GemmPath, crd::f64,
                                                           MatrixView<const crd::f64, Layout::RowMajor>,
                                                           MatrixView<const crd::f64, Layout::RowMajor>, crd::f64,
                                                           MatrixView<crd::f64, Layout::RowMajor>, Trans, Trans,
                                                           crd::memory::IAllocator*);
template void detail::gemm_arm<crd::f32, Layout::ColMajor>(const GemmKernels<crd::f32>&, detail::GemmPath, crd::f32,
                                                           MatrixView<const crd::f32, Layout::ColMajor>,
                                                           MatrixView<const crd::f32, Layout::ColMajor>, crd::f32,
                                                           MatrixView<crd::f32, Layout::ColMajor>, Trans, Trans,
                                                           crd::memory::IAllocator*);
template void detail::gemm_arm<crd::f64, Layout::ColMajor>(const GemmKernels<crd::f64>&, detail::GemmPath, crd::f64,
                                                           MatrixView<const crd::f64, Layout::ColMajor>,
                                                           MatrixView<const crd::f64, Layout::ColMajor>, crd::f64,
                                                           MatrixView<crd::f64, Layout::ColMajor>, Trans, Trans,
                                                           crd::memory::IAllocator*);

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

// detail::gemm_parallel_arm — the same 4 types × 2 layouts
template void detail::gemm_parallel_arm<crd::f32, Layout::RowMajor>(const detail::GemmParallelPick&, crd::u32, crd::f32,
                                                                    MatrixView<const crd::f32, Layout::RowMajor>,
                                                                    MatrixView<const crd::f32, Layout::RowMajor>,
                                                                    crd::f32, MatrixView<crd::f32, Layout::RowMajor>,
                                                                    Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<crd::f64, Layout::RowMajor>(const detail::GemmParallelPick&, crd::u32, crd::f64,
                                                                    MatrixView<const crd::f64, Layout::RowMajor>,
                                                                    MatrixView<const crd::f64, Layout::RowMajor>,
                                                                    crd::f64, MatrixView<crd::f64, Layout::RowMajor>,
                                                                    Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<Complex32, Layout::RowMajor>(const detail::GemmParallelPick&, crd::u32,
                                                                     Complex32,
                                                                     MatrixView<const Complex32, Layout::RowMajor>,
                                                                     MatrixView<const Complex32, Layout::RowMajor>,
                                                                     Complex32, MatrixView<Complex32, Layout::RowMajor>,
                                                                     Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<Complex64, Layout::RowMajor>(const detail::GemmParallelPick&, crd::u32,
                                                                     Complex64,
                                                                     MatrixView<const Complex64, Layout::RowMajor>,
                                                                     MatrixView<const Complex64, Layout::RowMajor>,
                                                                     Complex64, MatrixView<Complex64, Layout::RowMajor>,
                                                                     Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<crd::f32, Layout::ColMajor>(const detail::GemmParallelPick&, crd::u32, crd::f32,
                                                                    MatrixView<const crd::f32, Layout::ColMajor>,
                                                                    MatrixView<const crd::f32, Layout::ColMajor>,
                                                                    crd::f32, MatrixView<crd::f32, Layout::ColMajor>,
                                                                    Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<crd::f64, Layout::ColMajor>(const detail::GemmParallelPick&, crd::u32, crd::f64,
                                                                    MatrixView<const crd::f64, Layout::ColMajor>,
                                                                    MatrixView<const crd::f64, Layout::ColMajor>,
                                                                    crd::f64, MatrixView<crd::f64, Layout::ColMajor>,
                                                                    Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<Complex32, Layout::ColMajor>(const detail::GemmParallelPick&, crd::u32,
                                                                     Complex32,
                                                                     MatrixView<const Complex32, Layout::ColMajor>,
                                                                     MatrixView<const Complex32, Layout::ColMajor>,
                                                                     Complex32, MatrixView<Complex32, Layout::ColMajor>,
                                                                     Trans, Trans, crd::memory::IAllocator*);
template void detail::gemm_parallel_arm<Complex64, Layout::ColMajor>(const detail::GemmParallelPick&, crd::u32,
                                                                     Complex64,
                                                                     MatrixView<const Complex64, Layout::ColMajor>,
                                                                     MatrixView<const Complex64, Layout::ColMajor>,
                                                                     Complex64, MatrixView<Complex64, Layout::ColMajor>,
                                                                     Trans, Trans, crd::memory::IAllocator*);

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
