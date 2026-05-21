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
// BSR -- Block Sparse Row (b x b dense blocks, CSR-of-blocks). The native
// format for FEM / multi-DOF physics where the sparsity is inherently blocked
// (3 DOF/node in 3D elasticity -> 3x3 blocks; 6 for shells). A block is stored
// DENSE (all b*b scalars, row-major within the block) so block-spmv:
//   * reads each x-block ONCE (b contiguous scalars) and reuses it across the
//     b output rows (vs scalar CSR's b^2 separate x[col] gathers),
//   * carries b independent output accumulators -> ILP that breaks the serial
//     reduction chain bottlenecking scalar CSR spmv,
//   * stores one block-column index per b*b values (1/b^2 the index traffic).
//
// Eigen has no first-class BSR, so the honest gate is BSR spmv vs Eigen scalar
// CSR spmv on the SAME inherently-block-structured matrix (bench, v1f-1).
//
// Kernel decision (D(sparse)-6): a dedicated fully-unrolled small-block GEMV
// (compile-time b in {1,2,3,4,6} + a runtime fallback), NOT the hesap-dense
// v0d GEMM microkernel -- that leaf is sized for large-N register tiling and
// its packing/prologue cost dominates a 3x3 block (~9 FMAs). No hesap-dense
// dependency.
//
// Determinism (ADR-0063 / D(sparse)-3): blocks are visited block-column
// ascending; within a block the dot over columns is fixed-order; two-rounded
// mul+add (NOT a single-rounded FMA). beta==0 is NaN-safe (prior y untouched).
// Block-row-parallel writes disjoint y-blocks -> bit-exact with serial.
// -----------------------------------------------------------------------

template <typename T>
struct BsrMatrix
{
    crd::u32                         rows       = 0;  // scalar rows  = block_rows * block_size
    crd::u32                         cols       = 0;  // scalar cols  = block_cols * block_size
    crd::u32                         block_size = 1;  // b
    crd::u32                         block_rows = 0;
    crd::u32                         block_cols = 0;
    crd::containers::Array<crd::u32> block_row_ptr;  // length block_rows+1 (CSR-of-blocks)
    crd::containers::Array<crd::u32> block_col_idx;  // length nnz_blocks; ascending per block-row
    crd::containers::Array<T>        vals;           // length nnz_blocks * b * b; row-major within block

    explicit BsrMatrix(crd::memory::IAllocator* alloc)
        : block_row_ptr(alloc), block_col_idx(alloc), vals(alloc)
    {
    }

    [[nodiscard]] crd::u32   nnz_blocks() const noexcept { return static_cast<crd::u32>(block_col_idx.size()); }
    [[nodiscard]] crd::usize stored() const noexcept { return vals.size(); }
};

namespace detail
{
// One block-row: y[I*B .. I*B+B) = alpha * sum_k block_k * x[J_k*B ..) (+ beta*y).
// B is the compile-time block size; `bsz` lets a runtime-B caller pass the same
// path with B==0 sentinel handled by the runtime overload below.
template <typename T, crd::u32 B>
CRD_FORCEINLINE void bsr_block_row(const crd::u32* col_idx, const T* vals, const T* x, T* y_blk, crd::u32 k0,
                                   crd::u32 k1, T alpha, T beta, bool bzero)
{
    T acc[B];
    for (crd::u32 r = 0; r < B; ++r)
    {
        acc[r] = T{};
    }
    for (crd::u32 k = k0; k < k1; ++k)
    {
        const T* xb  = x + static_cast<crd::usize>(col_idx[k]) * B;
        const T* blk = vals + static_cast<crd::usize>(k) * B * B;
        for (crd::u32 r = 0; r < B; ++r)
        {
            const T* row = blk + static_cast<crd::usize>(r) * B;
            T        d   = acc[r];
            for (crd::u32 c = 0; c < B; ++c)
            {
                d = d + row[c] * xb[c];  // two-rounded, fixed order
            }
            acc[r] = d;
        }
    }
    for (crd::u32 r = 0; r < B; ++r)
    {
        y_blk[r] = bzero ? (alpha * acc[r]) : (alpha * acc[r] + beta * y_blk[r]);
    }
}

// Runtime-B fallback (b not in the compile-time set). Same order + rounding.
template <typename T>
void bsr_block_row_dyn(const crd::u32* col_idx, const T* vals, const T* x, T* y_blk, crd::u32 k0, crd::u32 k1,
                       crd::u32 b, T alpha, T beta, bool bzero)
{
    constexpr crd::u32 kMaxDynB = 64;
    CRD_ASSERT_MSG(b <= kMaxDynB, "BSR runtime block size exceeds kMaxDynB");
    T acc[kMaxDynB];
    for (crd::u32 r = 0; r < b; ++r)
    {
        acc[r] = T{};
    }
    for (crd::u32 k = k0; k < k1; ++k)
    {
        const T* xb  = x + static_cast<crd::usize>(col_idx[k]) * b;
        const T* blk = vals + static_cast<crd::usize>(k) * b * b;
        for (crd::u32 r = 0; r < b; ++r)
        {
            const T* row = blk + static_cast<crd::usize>(r) * b;
            T        d   = acc[r];
            for (crd::u32 c = 0; c < b; ++c)
            {
                d = d + row[c] * xb[c];
            }
            acc[r] = d;
        }
    }
    for (crd::u32 r = 0; r < b; ++r)
    {
        y_blk[r] = bzero ? (alpha * acc[r]) : (alpha * acc[r] + beta * y_blk[r]);
    }
}

template <typename T>
void bsr_spmv_rows(const BsrMatrix<T>& a, const T* x, T alpha, T beta, T* y, crd::u32 ib0, crd::u32 ib1)
{
    const crd::u32* ptr  = a.block_row_ptr.data();
    const crd::u32* cidx = a.block_col_idx.data();
    const T*        v     = a.vals.data();
    const crd::u32  b      = a.block_size;
    const bool      bzero  = detail::spmv_is_zero(beta);
    for (crd::u32 ib = ib0; ib < ib1; ++ib)
    {
        T*             y_blk = y + static_cast<crd::usize>(ib) * b;
        const crd::u32 k0    = ptr[ib];
        const crd::u32 k1    = ptr[ib + 1];
        switch (b)
        {
        case 1: bsr_block_row<T, 1>(cidx, v, x, y_blk, k0, k1, alpha, beta, bzero); break;
        case 2: bsr_block_row<T, 2>(cidx, v, x, y_blk, k0, k1, alpha, beta, bzero); break;
        case 3: bsr_block_row<T, 3>(cidx, v, x, y_blk, k0, k1, alpha, beta, bzero); break;
        case 4: bsr_block_row<T, 4>(cidx, v, x, y_blk, k0, k1, alpha, beta, bzero); break;
        case 6: bsr_block_row<T, 6>(cidx, v, x, y_blk, k0, k1, alpha, beta, bzero); break;
        default: bsr_block_row_dyn<T>(cidx, v, x, y_blk, k0, k1, b, alpha, beta, bzero); break;
        }
    }
}
} // namespace detail

// Serial BSR spmv: y = alpha * A * x + beta * y.
template <typename T>
void spmv_bsr(T alpha, const BsrMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() >= static_cast<crd::usize>(a.cols), "spmv_bsr: x too short");
    CRD_ASSERT_MSG(y.size() >= static_cast<crd::usize>(a.rows), "spmv_bsr: y too short");
    detail::bsr_spmv_rows<T>(a, x.data(), alpha, beta, y.data(), 0, a.block_rows);
}

// Parallel BSR spmv: block-row-balanced over crd::jobs (each job writes disjoint
// y-blocks -> bit-exact with serial at any job count).
template <typename T>
void spmv_bsr_parallel(T alpha, const BsrMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta,
                       crd::containers::Span<T> y, crd::u32 num_jobs = 0)
{
    CRD_ASSERT_MSG(x.size() >= static_cast<crd::usize>(a.cols), "spmv_bsr: x too short");
    CRD_ASSERT_MSG(y.size() >= static_cast<crd::usize>(a.rows), "spmv_bsr: y too short");
    const crd::u32 nbr = a.block_rows;
    if (nbr == 0)
    {
        return;
    }
    const crd::u32* ptr = a.block_row_ptr.data();
    const crd::u32  nbk = ptr[nbr];

    crd::u32 jobs = (num_jobs == 0) ? crd::jobs::num_workers() : num_jobs;
    if (jobs == 0)
    {
        jobs = 1;
    }
    jobs = jobs < nbr ? jobs : nbr;
    constexpr crd::u32 kMaxJobs = 1024;
    if (jobs > kMaxJobs)
    {
        jobs = kMaxJobs;
    }
    crd::u32 bnd[kMaxJobs + 1];
    bnd[0]    = 0;
    bnd[jobs] = nbr;
    {
        crd::u32 i = 0;
        for (crd::u32 jb = 1; jb < jobs; ++jb)
        {
            const crd::u32 target = static_cast<crd::u32>((static_cast<crd::u64>(nbk) * jb) / jobs);
            while (i < nbr && ptr[i + 1] <= target)
            {
                ++i;
            }
            bnd[jb] = i;
        }
    }

    struct Ctx
    {
        const BsrMatrix<T>* a;
        const T*            x;
        T*                  y;
        T                   alpha, beta;
        const crd::u32*     bnd;
    };
    Ctx ctx{&a, x.data(), y.data(), alpha, beta, bnd};

    auto* counter = crd::jobs::parallel_for(jobs, jobs, [&ctx](crd::u32 jb0, crd::u32 jb1) {
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            detail::bsr_spmv_rows<T>(*ctx.a, ctx.x, ctx.alpha, ctx.beta, ctx.y, ctx.bnd[jb], ctx.bnd[jb + 1]);
        }
    });
    crd::jobs::wait(counter);
}

// CSR -> BSR. Tiles the matrix into block_size x block_size blocks; a block is
// emitted (dense) iff it contains >=1 stored CSR entry. Partial edge blocks are
// zero-padded (D(sparse)-7): a BSR block is dense by definition. Requires the
// scalar dims to be multiples of b (asserted) -- callers pad the matrix first
// if needed.
template <typename T>
[[nodiscard]] BsrMatrix<T> to_bsr(const SparseMatrix<T, SparseFormat::Csr>& csr, crd::u32 b,
                                  crd::memory::IAllocator* alloc)
{
    const SparsePattern& pat = csr.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "to_bsr requires a compressed CSR matrix");
    CRD_ASSERT_MSG(b >= 1, "to_bsr: block size must be >= 1");
    CRD_ASSERT_MSG(pat.rows % b == 0 && pat.cols % b == 0, "to_bsr: scalar dims must be multiples of b");

    BsrMatrix<T> out(alloc);
    out.rows       = pat.rows;
    out.cols       = pat.cols;
    out.block_size = b;
    out.block_rows = pat.rows / b;
    out.block_cols = pat.cols / b;

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        srcv  = csr.values().values.data();

    out.block_row_ptr.resize(static_cast<crd::usize>(out.block_rows) + 1);
    out.block_row_ptr[0] = 0;

    // marker[block_col] = 1-based slot index within the current block-row's
    // emitted blocks (touched-list cleared O(blocks-in-row), no full memset).
    crd::containers::Array<crd::u32> marker(alloc);
    marker.resize(out.block_cols);
    for (crd::u32 j = 0; j < out.block_cols; ++j)
    {
        marker[j] = 0;
    }
    crd::containers::Array<crd::u32> touched(alloc);

    for (crd::u32 ib = 0; ib < out.block_rows; ++ib)
    {
        touched.clear();
        const crd::u32 base = out.nnz_blocks();  // first block slot of this block-row
        // Pass 1: discover which block-columns this block-row touches (ascending).
        for (crd::u32 rr = 0; rr < b; ++rr)
        {
            const crd::u32 row = ib * b + rr;
            for (crd::u32 k = outer[row]; k < outer[row + 1]; ++k)
            {
                const crd::u32 jb = inner[k] / b;
                if (marker[jb] == 0)
                {
                    touched.push_back(jb);
                    marker[jb] = 1;
                }
            }
        }
        crd::containers::sort(touched.data(), touched.data() + touched.size());
        // Assign dense slots in ascending block-column order; zero-fill values.
        for (crd::u32 t = 0; t < touched.size(); ++t)
        {
            marker[touched[t]] = base + t + 1;  // 1-based slot
            out.block_col_idx.push_back(touched[t]);
        }
        const crd::usize new_blocks = touched.size();
        const crd::usize voff       = static_cast<crd::usize>(base) * b * b;
        out.vals.resize(voff + new_blocks * b * b);
        for (crd::usize z = voff; z < out.vals.size(); ++z)
        {
            out.vals[z] = T{};
        }
        // Pass 2: scatter CSR entries into their dense block slots.
        for (crd::u32 rr = 0; rr < b; ++rr)
        {
            const crd::u32 row = ib * b + rr;
            for (crd::u32 k = outer[row]; k < outer[row + 1]; ++k)
            {
                const crd::u32  jb   = inner[k] / b;
                const crd::u32  cc   = inner[k] % b;
                const crd::u32  slot = marker[jb] - 1;  // absolute block slot
                const crd::usize off = static_cast<crd::usize>(slot) * b * b + static_cast<crd::usize>(rr) * b + cc;
                out.vals[off] = srcv[k];
            }
        }
        out.block_row_ptr[ib + 1] = out.nnz_blocks();
        // Clear markers for the touched block-columns only.
        for (crd::u32 t = 0; t < touched.size(); ++t)
        {
            marker[touched[t]] = 0;
        }
    }
    return out;
}

// BSR -> CSR. Emits every stored block's b*b entries (a stored block is dense,
// including explicit/padding zeros -- BSR -> CSR is structural, not a prune).
// Within each scalar row, columns come out ascending (blocks are ascending and
// the within-block columns are scanned ascending).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> from_bsr(const BsrMatrix<T>& a, crd::memory::IAllocator* alloc)
{
    const crd::u32 b = a.block_size;
    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = a.rows;
    pat.cols       = a.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;

    const crd::u32* ptr  = a.block_row_ptr.data();
    const crd::u32* cidx = a.block_col_idx.data();
    const T*        v     = a.vals.data();

    pat.outer_ptr.resize(static_cast<crd::usize>(a.rows) + 1);
    pat.outer_ptr[0] = 0;
    // Each scalar row in block-row ib has (blocks-in-ib * b) entries.
    for (crd::u32 ib = 0; ib < a.block_rows; ++ib)
    {
        const crd::u32 nb = ptr[ib + 1] - ptr[ib];
        for (crd::u32 rr = 0; rr < b; ++rr)
        {
            const crd::u32 row     = ib * b + rr;
            pat.outer_ptr[row + 1] = pat.outer_ptr[row] + nb * b;
        }
    }
    const crd::u32 nnz = pat.outer_ptr[a.rows];
    pat.inner_idx.resize_uninitialized(nnz);
    vals.values.resize_uninitialized(nnz);

    for (crd::u32 ib = 0; ib < a.block_rows; ++ib)
    {
        const crd::u32 k0 = ptr[ib];
        const crd::u32 k1 = ptr[ib + 1];
        for (crd::u32 rr = 0; rr < b; ++rr)
        {
            const crd::u32 row = ib * b + rr;
            crd::u32       w   = pat.outer_ptr[row];
            for (crd::u32 k = k0; k < k1; ++k)
            {
                const crd::u32  jb  = cidx[k];
                const T*        row_blk = v + (static_cast<crd::usize>(k) * b + rr) * b;
                for (crd::u32 cc = 0; cc < b; ++cc)
                {
                    pat.inner_idx[w] = jb * b + cc;
                    vals.values[w]   = row_blk[cc];
                    ++w;
                }
            }
        }
    }
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
