#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>  // detail::spmv_is_zero
#include <crd/jobs/jobs.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// DIA (diagonal) -- the banded / structured-grid format. Each occupied
// diagonal (offset k = col - row) is stored once for ALL rows (diagonal-major:
// data[d*rows + i] = A[i][i+offset_d]); the column-index array vanishes (the
// offset is shared across the whole diagonal), and the spmv's x access x[i+off]
// is CONTIGUOUS in i -> streams beautifully and vectorises. Ideal for stencil /
// finite-difference operators; wasteful for unstructured sparsity (a near-empty
// diagonal still costs `rows` slots) -- use CSR/SELL there.
//
// Determinism (D(sparse)-3): offsets are sorted ascending, so for any row the
// diagonals are visited in ascending-column order == CSR order. Each y[i] is an
// independent reduction; disjoint row partitions -> parallel is bit-exact with
// serial. alpha is applied per-term here (y = beta*y + sum_d alpha*d_i*x): this
// is NOT bit-identical to CSR's alpha*(rowsum) (a different but equally fixed
// rounding), so DIA vs CSR is checked within tolerance, parallel vs serial is
// exact.
// -----------------------------------------------------------------------

template <typename T>
struct DiaMatrix
{
    crd::u32                         rows  = 0;
    crd::u32                         cols  = 0;
    crd::u32                         ndiag = 0;
    crd::containers::Array<crd::i32> offsets;  // ascending; length ndiag (k = col - row)
    crd::containers::Array<T>        data;     // ndiag*rows, diagonal-major; data[d*rows+i] = A[i][i+offsets[d]]

    explicit DiaMatrix(crd::memory::IAllocator* alloc) : offsets(alloc), data(alloc) {}

    [[nodiscard]] crd::usize stored() const noexcept { return data.size(); }
};

namespace detail
{
template <typename T>
void dia_spmv_rows(const DiaMatrix<T>& a, const T* x, T alpha, T beta, T* y, crd::u32 i0, crd::u32 i1)
{
    const crd::u32  rows = a.rows;
    const crd::i32  cols = static_cast<crd::i32>(a.cols);
    const crd::u32  nd   = a.ndiag;
    const crd::i32* off  = a.offsets.data();
    const T*        dat  = a.data.data();
    const bool      bz   = detail::spmv_is_zero(beta);
    for (crd::u32 i = i0; i < i1; ++i)
    {
        y[i] = bz ? T{} : beta * y[i];
    }
    for (crd::u32 d = 0; d < nd; ++d)  // ascending offset == CSR column order
    {
        const crd::i32 k  = off[d];
        const T*       dd = dat + static_cast<crd::usize>(d) * rows;
        // valid rows: 0 <= i+k < cols  ->  i in [max(0,-k), min(rows, cols-k))
        crd::i32 lo = -k > 0 ? -k : 0;
        crd::i32 hi = cols - k < static_cast<crd::i32>(rows) ? cols - k : static_cast<crd::i32>(rows);
        if (lo < static_cast<crd::i32>(i0))
        {
            lo = static_cast<crd::i32>(i0);
        }
        if (hi > static_cast<crd::i32>(i1))
        {
            hi = static_cast<crd::i32>(i1);
        }
        for (crd::i32 i = lo; i < hi; ++i)
        {
            const auto ui = static_cast<crd::u32>(i);
            y[ui]         = y[ui] + alpha * (dd[ui] * x[static_cast<crd::u32>(i + k)]);  // contiguous x[i+k]
        }
    }
}
} // namespace detail

// Serial DIA spmv: y = alpha * A * x + beta * y.
template <typename T>
void spmv_dia(T alpha, const DiaMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() >= static_cast<crd::usize>(a.cols), "spmv_dia: x too short");
    CRD_ASSERT_MSG(y.size() >= static_cast<crd::usize>(a.rows), "spmv_dia: y too short");
    detail::dia_spmv_rows<T>(a, x.data(), alpha, beta, y.data(), 0, a.rows);
}

// Parallel DIA spmv: even row partition over crd::jobs (disjoint y writes ->
// bit-exact with serial at any job count).
template <typename T>
void spmv_dia_parallel(T alpha, const DiaMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta,
                       crd::containers::Span<T> y, crd::u32 num_jobs = 0)
{
    CRD_ASSERT_MSG(x.size() >= static_cast<crd::usize>(a.cols), "spmv_dia: x too short");
    CRD_ASSERT_MSG(y.size() >= static_cast<crd::usize>(a.rows), "spmv_dia: y too short");
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
        const DiaMatrix<T>* a;
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
            detail::dia_spmv_rows<T>(*ctx.a, ctx.x, ctx.alpha, ctx.beta, ctx.y, r0, r1);
        }
    });
    crd::jobs::wait(counter);
}

// CSR -> DIA. One stored diagonal per offset (col-row) that appears at least
// once; absent positions on a stored diagonal are 0. Offsets sorted ascending.
template <typename T>
[[nodiscard]] DiaMatrix<T> to_dia(const SparseMatrix<T, SparseFormat::Csr>& csr, crd::memory::IAllocator* alloc)
{
    const SparsePattern& pat = csr.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "to_dia requires a compressed CSR matrix");
    DiaMatrix<T> out(alloc);
    out.rows = pat.rows;
    out.cols = pat.cols;

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        srcv  = csr.values().values.data();

    // Discover distinct offsets (touched-list, marker keyed by shifted offset).
    const crd::u32                   span = pat.rows + pat.cols;  // offsets in [-(rows-1), cols-1] -> shift by rows
    crd::containers::Array<crd::u32> marker(alloc);
    marker.resize(span + 1);
    for (crd::u32 z = 0; z <= span; ++z)
    {
        marker[z] = 0;
    }
    crd::containers::Array<crd::i32> offs(alloc);
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::i32 d   = static_cast<crd::i32>(inner[k]) - static_cast<crd::i32>(i);
            const crd::u32 key = static_cast<crd::u32>(d + static_cast<crd::i32>(pat.rows));
            if (marker[key] == 0)
            {
                marker[key] = 1;
                offs.push_back(d);
            }
        }
    }
    crd::containers::sort(offs.data(), offs.data() + offs.size());
    out.ndiag = static_cast<crd::u32>(offs.size());
    out.offsets.resize(out.ndiag);
    for (crd::u32 d = 0; d < out.ndiag; ++d)
    {
        out.offsets[d] = offs[d];
        // remap marker to 1-based diagonal slot for the scatter pass
        marker[static_cast<crd::u32>(offs[d] + static_cast<crd::i32>(pat.rows))] = d + 1;
    }
    const crd::usize total = static_cast<crd::usize>(out.ndiag) * pat.rows;
    out.data.resize(total);
    for (crd::usize z = 0; z < total; ++z)
    {
        out.data[z] = T{};
    }
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::i32 d    = static_cast<crd::i32>(inner[k]) - static_cast<crd::i32>(i);
            const crd::u32 slot = marker[static_cast<crd::u32>(d + static_cast<crd::i32>(pat.rows))] - 1;
            out.data[static_cast<crd::usize>(slot) * pat.rows + i] = srcv[k];
        }
    }
    return out;
}

// DIA -> CSR. Emits in-range NON-ZERO diagonal entries per row, ascending column
// (offsets are ascending). A stored zero on a diagonal is dropped (DIA cannot
// distinguish a structural absent from a stored zero) -- round-trips exactly for
// zero-free matrices.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> from_dia(const DiaMatrix<T>& a, crd::memory::IAllocator* alloc)
{
    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = a.rows;
    pat.cols       = a.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;

    const crd::u32  rows = a.rows;
    const crd::i32  cols = static_cast<crd::i32>(a.cols);
    const crd::u32  nd   = a.ndiag;
    const crd::i32* off  = a.offsets.data();
    const T*        dat  = a.data.data();

    pat.outer_ptr.resize(static_cast<crd::usize>(rows) + 1);
    pat.outer_ptr[0] = 0;
    for (crd::u32 i = 0; i < rows; ++i)
    {
        crd::u32 cnt = 0;
        for (crd::u32 d = 0; d < nd; ++d)
        {
            const crd::i32 j = static_cast<crd::i32>(i) + off[d];
            if (j >= 0 && j < cols && !(dat[static_cast<crd::usize>(d) * rows + i] == T{}))
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
        crd::u32 w = pat.outer_ptr[i];
        for (crd::u32 d = 0; d < nd; ++d)
        {
            const crd::i32 j = static_cast<crd::i32>(i) + off[d];
            if (j >= 0 && j < cols)
            {
                const T v = dat[static_cast<crd::usize>(d) * rows + i];
                if (!(v == T{}))
                {
                    pat.inner_idx[w] = static_cast<crd::u32>(j);
                    vals.values[w]   = v;
                    ++w;
                }
            }
        }
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
