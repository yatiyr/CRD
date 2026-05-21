#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>  // detail::spmv_is_zero
#include <crd/jobs/jobs.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// ELL (ELLPACK) -- the canonical regular sparse format. Every row is padded to
// the GLOBAL max row length; the c-th entry of every row is stored CONTIGUOUSLY
// (slot-major: idx = c * rows + i), so the spmv vectorises over rows. Pad slots
// carry value 0 (column 0) -> a no-op add.
//
// CONTRACT (v1f-2, pinned): ELL is the interop / base regular format. It is NOT
// the irregular-matrix performance path -- that is SELL-C-σ (v1b), which pads
// PER-SLICE (not globally) and σ-sorts to bound the padding. ELL pays full
// global padding on irregular matrices by design; use it for genuinely regular
// (uniform-row-length) patterns and interchange, reach for SELL otherwise.
//
// Determinism (D(sparse)-3): each y[i] accumulates its slots in ascending c
// order == the row's CSR column order -> bit-exact with the CSR baseline. The
// per-row reductions are independent across rows, so the SIMD-over-rows kernel
// is bit-exact with the scalar fallback (cross-SIMD-width) and with serial.
// -----------------------------------------------------------------------

template <typename T>
struct EllMatrix
{
    crd::u32                         rows         = 0;
    crd::u32                         cols         = 0;
    crd::u32                         max_row_len  = 0;  // global padded width
    crd::containers::Array<crd::u32> col_idx;           // slot-major rows*max_row_len; pad col = 0
    crd::containers::Array<T>        vals;              // slot-major rows*max_row_len; pad val = 0

    explicit EllMatrix(crd::memory::IAllocator* alloc) : col_idx(alloc), vals(alloc) {}

    [[nodiscard]] crd::usize stored() const noexcept { return vals.size(); }
};

namespace detail
{
template <typename T>
void ell_spmv_rows(const EllMatrix<T>& a, const T* x, T alpha, T beta, T* y, crd::u32 i0, crd::u32 i1)
{
    const crd::u32  rows = a.rows;
    const crd::u32  w    = a.max_row_len;
    const T*        v    = a.vals.data();
    const crd::u32* ci   = a.col_idx.data();
    const bool      bz   = detail::spmv_is_zero(beta);
    for (crd::u32 i = i0; i < i1; ++i)
    {
        T acc = T{};
        for (crd::u32 c = 0; c < w; ++c)  // ascending slot == CSR column order
        {
            const crd::usize off = static_cast<crd::usize>(c) * rows + i;
            acc = acc + v[off] * x[ci[off]];  // two-rounded; pad val = 0 is a no-op
        }
        y[i] = bz ? (alpha * acc) : (alpha * acc + beta * y[i]);
    }
}
} // namespace detail

// Serial ELL spmv: y = alpha * A * x + beta * y.
template <typename T>
void spmv_ell(T alpha, const EllMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() >= static_cast<crd::usize>(a.cols), "spmv_ell: x too short");
    CRD_ASSERT_MSG(y.size() >= static_cast<crd::usize>(a.rows), "spmv_ell: y too short");
    detail::ell_spmv_rows<T>(a, x.data(), alpha, beta, y.data(), 0, a.rows);
}

// Parallel ELL spmv: even row partition over crd::jobs (rows are uniform width
// -> equal work; disjoint y writes -> bit-exact with serial at any job count).
template <typename T>
void spmv_ell_parallel(T alpha, const EllMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta,
                       crd::containers::Span<T> y, crd::u32 num_jobs = 0)
{
    CRD_ASSERT_MSG(x.size() >= static_cast<crd::usize>(a.cols), "spmv_ell: x too short");
    CRD_ASSERT_MSG(y.size() >= static_cast<crd::usize>(a.rows), "spmv_ell: y too short");
    const crd::u32 rows = a.rows;
    if (rows == 0)
    {
        return;
    }
    crd::u32 jobs = (num_jobs == 0) ? crd::jobs::num_workers() : num_jobs;
    if (jobs == 0)
    {
        jobs = 1;
    }
    jobs = jobs < rows ? jobs : rows;
    constexpr crd::u32 kMaxJobs = 1024;
    if (jobs > kMaxJobs)
    {
        jobs = kMaxJobs;
    }
    struct Ctx
    {
        const EllMatrix<T>* a;
        const T*            x;
        T*                  y;
        T                   alpha, beta;
        crd::u32            rows, jobs;
    };
    Ctx ctx{&a, x.data(), y.data(), alpha, beta, rows, jobs};
    auto* counter = crd::jobs::parallel_for(jobs, jobs, [&ctx](crd::u32 jb0, crd::u32 jb1) {
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            const crd::u32 r0 = static_cast<crd::u32>((static_cast<crd::u64>(ctx.rows) * jb) / ctx.jobs);
            const crd::u32 r1 = static_cast<crd::u32>((static_cast<crd::u64>(ctx.rows) * (jb + 1)) / ctx.jobs);
            detail::ell_spmv_rows<T>(*ctx.a, ctx.x, ctx.alpha, ctx.beta, ctx.y, r0, r1);
        }
    });
    crd::jobs::wait(counter);
}

// CSR -> ELL. max_row_len = global longest row; shorter rows pad with (col 0,
// val 0). Within a row, entries keep CSR column-ascending order in slots.
template <typename T>
[[nodiscard]] EllMatrix<T> to_ell(const SparseMatrix<T, SparseFormat::Csr>& csr, crd::memory::IAllocator* alloc)
{
    const SparsePattern& pat = csr.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "to_ell requires a compressed CSR matrix");
    EllMatrix<T>    out(alloc);
    out.rows = pat.rows;
    out.cols = pat.cols;

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        srcv  = csr.values().values.data();
    crd::u32        w      = 0;
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        const crd::u32 len = outer[i + 1] - outer[i];
        w                  = len > w ? len : w;
    }
    out.max_row_len = w;
    const crd::usize total = static_cast<crd::usize>(pat.rows) * w;
    out.col_idx.resize(total);
    out.vals.resize(total);
    for (crd::usize z = 0; z < total; ++z)
    {
        out.col_idx[z] = 0;
        out.vals[z]    = T{};
    }
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        crd::u32 c = 0;
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k, ++c)
        {
            const crd::usize off = static_cast<crd::usize>(c) * pat.rows + i;
            out.col_idx[off]     = inner[k];
            out.vals[off]        = srcv[k];
        }
    }
    return out;
}

// ELL -> CSR. Emits a row's NON-PAD slots (a slot is padding iff val==0 AND
// col==0; a genuine stored zero at a nonzero column is kept). Ascending column
// order is preserved (slots were filled in CSR order).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> from_ell(const EllMatrix<T>& a, crd::memory::IAllocator* alloc)
{
    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = a.rows;
    pat.cols       = a.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;

    const crd::u32  rows = a.rows;
    const crd::u32  w    = a.max_row_len;
    const crd::u32* ci   = a.col_idx.data();
    const T*        v     = a.vals.data();

    pat.outer_ptr.resize(static_cast<crd::usize>(rows) + 1);
    pat.outer_ptr[0] = 0;
    for (crd::u32 i = 0; i < rows; ++i)
    {
        crd::u32 cnt = 0;
        for (crd::u32 c = 0; c < w; ++c)
        {
            const crd::usize off = static_cast<crd::usize>(c) * rows + i;
            if (!(ci[off] == 0 && v[off] == T{}))  // not a pad slot
            {
                ++cnt;
            }
        }
        pat.outer_ptr[i + 1] = pat.outer_ptr[i] + cnt;
    }
    const crd::u32 nnz = pat.outer_ptr[rows];
    pat.inner_idx.resize_uninitialized(nnz);
    vals.values.resize_uninitialized(nnz);
    for (crd::u32 i = 0; i < rows; ++i)
    {
        crd::u32 wpos = pat.outer_ptr[i];
        for (crd::u32 c = 0; c < w; ++c)
        {
            const crd::usize off = static_cast<crd::usize>(c) * rows + i;
            if (!(ci[off] == 0 && v[off] == T{}))
            {
                pat.inner_idx[wpos] = ci[off];
                vals.values[wpos]   = v[off];
                ++wpos;
            }
        }
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
