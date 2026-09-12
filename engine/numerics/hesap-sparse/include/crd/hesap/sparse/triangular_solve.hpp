#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Level-scheduled parallel sparse triangular solve. Phase 3.1.6 v4g-tri-solve-parallel.
//
// The sparse triangular solves inside an incomplete factorization (IC/ILU/ILUT apply,
// SSOR sweeps) are the per-iteration bottleneck when the factor is dense, and are
// inherently SEQUENTIAL row-by-row. Level scheduling recovers parallelism: row i's
// dependency level = 1 + max(level of the rows it references); rows in the same level
// are mutually independent and solve in parallel, level by level. The available
// parallelism is a property of the MATRIX (a chain-like factor has n width-1 levels and
// no parallelism; a 2D-stencil factor has ~√n levels of ~√n width) — the bench reports
// levels/n and max_level_width so the regime is visible.
//
// SHAPE (locked): the factor is OFF-DIAGONAL CSR (ptr/col/val, NO diagonal entries) + an
// `inv_diag` array. Lower: y_i = (b_i − Σ_{cols<i} val·y_col)·inv_diag[i]; upper (back):
// z_i = (y_i − Σ_{cols>i} val·z_col)·inv_diag[i]. `inv_diag == nullptr` ⇒ unit diagonal.
// ILUT's L (unit lower) and U (off-diag + 1/U_ii) fit natively; IC(0)/ILU(0) split their
// stored diagonal into inv_diag at construction.
//
// DETERMINISM: each output element y_i is computed by exactly one worker as a fixed-order
// (CSR) reduction; the level partition is deterministic ⇒ the result is bit-identical to
// the sequential solve at any worker count (the v4 moat).
//
// SIZE-ADAPTIVE (D-pin): parallel only when max_level_width ≥ 64 AND n ≥ 8192 — below
// that, the per-level job-dispatch + barrier cost exceeds the sequential solve.
// -----------------------------------------------------------------------

struct TriSchedule
{
    crd::containers::Array<crd::u32> level_set; // row indices grouped by level (length n)
    crd::containers::Array<crd::u32> level_ptr; // length n_levels+1; level k = [level_ptr[k], level_ptr[k+1])
    crd::usize                       n         = 0;
    crd::u32                         max_width = 0; // widest level (= achievable parallelism)

    explicit TriSchedule(crd::memory::IAllocator* alloc) : level_set(alloc), level_ptr(alloc) {}

    [[nodiscard]] crd::u32 n_levels() const noexcept
    {
        return level_ptr.empty() ? 0U : static_cast<crd::u32>(level_ptr.size() - 1);
    }
    // Parallel is worthwhile only with BOTH enough width (wide wavefronts to fill workers)
    // AND enough size (n ≥ 8192) — at small n the per-level dispatch+barrier cost, summed
    // over many narrow levels, exceeds the sequential solve (measured: sherman3 n=5005 with
    // 689 levels ran 2× SLOWER parallel). The crush regime is large dense factors.
    [[nodiscard]] bool worth_parallel() const noexcept { return max_width >= 256U && n >= 8192U; }
};

namespace detail
{
// Build the dependency-level schedule. `lower` ⇒ deps are cols < row (sweep rows
// ascending); else deps are cols > row (sweep descending). O(nnz). Deterministic.
inline TriSchedule build_tri_schedule(const crd::u32* ptr, const crd::u32* col, crd::usize n, bool lower,
                                      crd::memory::IAllocator* alloc)
{
    TriSchedule                      s(alloc);
    s.n = n;
    crd::containers::Array<crd::u32> level(alloc);
    level.resize(n);
    crd::u32 maxlev = 0;
    for (crd::usize ii = 0; ii < n; ++ii)
    {
        const crd::usize i  = lower ? ii : (n - 1 - ii); // dependency-respecting sweep order
        crd::u32         lv = 0;
        for (crd::u32 p = ptr[i]; p < ptr[i + 1]; ++p)
        {
            const crd::u32 dep = level[col[p]] + 1; // dep already finalized in this sweep order
            if (dep > lv) { lv = dep; }
        }
        level[i] = lv;
        if (lv > maxlev) { maxlev = lv; }
    }
    const crd::u32 n_levels = maxlev + 1;
    s.level_ptr.resize(n_levels + 1);
    for (crd::u32 k = 0; k <= n_levels; ++k) { s.level_ptr[k] = 0; }
    for (crd::usize i = 0; i < n; ++i) { ++s.level_ptr[level[i] + 1]; } // counts
    for (crd::u32 k = 0; k < n_levels; ++k) { s.level_ptr[k + 1] += s.level_ptr[k]; } // prefix sum
    s.level_set.resize(n);
    crd::containers::Array<crd::u32> cursor(alloc);
    cursor.resize(n_levels);
    for (crd::u32 k = 0; k < n_levels; ++k) { cursor[k] = s.level_ptr[k]; }
    for (crd::usize i = 0; i < n; ++i) { s.level_set[cursor[level[i]]++] = static_cast<crd::u32>(i); } // stable by row
    for (crd::u32 k = 0; k < n_levels; ++k)
    {
        const crd::u32 w = s.level_ptr[k + 1] - s.level_ptr[k];
        if (w > s.max_width) { s.max_width = w; }
    }
    return s;
}

// One row of the off-diagonal solve: out[i] = (rhs[i] − Σ val·out[col]) · (inv_diag ? inv_diag[i] : 1).
template <typename T>
inline void tri_solve_row(crd::u32 i, const crd::u32* ptr, const crd::u32* col, const T* val, const T* inv_diag,
                          const T* rhs, T* out) noexcept
{
    T acc = rhs[i];
    for (crd::u32 p = ptr[i]; p < ptr[i + 1]; ++p) { acc = acc - val[p] * out[col[p]]; }
    out[i] = (inv_diag != nullptr) ? acc * inv_diag[i] : acc;
}

template <typename T>
inline void tri_solve_levelsched(const crd::u32* ptr, const crd::u32* col, const T* val, const T* inv_diag,
                                 const TriSchedule& s, const T* rhs, T* out)
{
    const bool parallel = s.worth_parallel();
    const crd::u32 nlev = s.n_levels();
    for (crd::u32 k = 0; k < nlev; ++k)
    {
        const crd::u32 lo = s.level_ptr[k];
        const crd::u32 hi = s.level_ptr[k + 1];
        const crd::u32 w  = hi - lo;
        if (!parallel || w < 256U) // narrow levels: serial (no barrier); wide levels: parallel_for
        {
            for (crd::u32 t = lo; t < hi; ++t) { tri_solve_row<T>(s.level_set[t], ptr, col, val, inv_diag, rhs, out); }
        }
        else
        {
            struct Ctx { const crd::u32* ptr; const crd::u32* col; const T* val; const T* inv_diag; const T* rhs; T* out; const crd::u32* lset; crd::u32 lo; };
            Ctx   ctx{ptr, col, val, inv_diag, rhs, out, s.level_set.data(), lo};
            auto* counter = crd::jobs::parallel_for(w, crd::jobs::num_workers(), [&ctx](crd::u32 a, crd::u32 b) {
                for (crd::u32 t = a; t < b; ++t)
                {
                    tri_solve_row<T>(ctx.lset[ctx.lo + t], ctx.ptr, ctx.col, ctx.val, ctx.inv_diag, ctx.rhs, ctx.out);
                }
            });
            crd::jobs::wait(counter);
            crd::jobs::frame_reset(); // per-level arena hygiene (a chain-y factor has many levels)
        }
    }
}
} // namespace detail

// y = L⁻¹ b, L lower-triangular off-diagonal CSR + inv_diag (nullptr ⇒ unit). Schedule built
// for `lower=true`. Level-scheduled parallel (size-adaptive), bit-exact vs sequential.
template <typename T>
inline void tri_solve_lower_levelsched(const crd::u32* ptr, const crd::u32* col, const T* val, const T* inv_diag,
                                       const TriSchedule& schedule, const T* b, T* y)
{
    detail::tri_solve_levelsched<T>(ptr, col, val, inv_diag, schedule, b, y);
}

// z = U⁻¹ y, U upper-triangular off-diagonal CSR + inv_diag (= 1/U_ii). Schedule `lower=false`.
template <typename T>
inline void tri_solve_upper_levelsched(const crd::u32* ptr, const crd::u32* col, const T* val, const T* inv_diag,
                                       const TriSchedule& schedule, const T* y, T* z)
{
    detail::tri_solve_levelsched<T>(ptr, col, val, inv_diag, schedule, y, z);
}

[[nodiscard]] inline TriSchedule build_lower_tri_schedule(const crd::u32* ptr, const crd::u32* col, crd::usize n,
                                                          crd::memory::IAllocator* alloc)
{
    return detail::build_tri_schedule(ptr, col, n, /*lower=*/true, alloc);
}
[[nodiscard]] inline TriSchedule build_upper_tri_schedule(const crd::u32* ptr, const crd::u32* col, crd::usize n,
                                                          crd::memory::IAllocator* alloc)
{
    return detail::build_tri_schedule(ptr, col, n, /*lower=*/false, alloc);
}

} // namespace crd::hesap::sparse
