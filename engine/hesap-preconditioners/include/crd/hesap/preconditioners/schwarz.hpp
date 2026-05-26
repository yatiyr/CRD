#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/ordering/nested_dissection.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/preconditioners/detail/dense_lu.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// SchwarzPreconditioner<T> -- overlapping domain-decomposition preconditioner. v4i-3.
//
//   M⁻¹ = Σ_i  R̃ᵢᵀ · Aᵢᵢ⁻¹ · Rᵢ      (Aᵢᵢ = A restricted to overlapping subdomain Ωᵢ)
//
// [0,n) is partitioned into disjoint base subdomains Ω⁰ᵢ (contiguous chunks, or
// nested-dissection-reordered chunks), each grown by `overlap` graph-neighbour layers
// into the overlapping Ωᵢ. Each local block Aᵢᵢ is factored ONCE (dense LU, partial
// pivot, factor-once/solve-many) and its forward/back solve runs every apply --
// fully PARALLEL across subdomains (the win over IC/ILU's sequential global
// triangular solve).
//
// SchwarzType:
//   - Additive (AS): prolongate the FULL Ωᵢ (overlap summed). M_AS = Σ Rᵢᵀ Aᵢᵢ⁻¹ Rᵢ
//     is SYMMETRIC for symmetric A WITH EXACT LOCAL SOLVES (dense LU is exact) ⇒
//     SPD-compatible (CG/MINRES/SYMMLQ). Overlap writes are summed ⇒ apply is serial
//     in a fixed subdomain order (deterministic). [An INEXACT local solve would break
//     the symmetry -- contract: AS is SPD-valid only with the exact dense-LU solve.]
//   - Restricted (RAS, default; Cai-Sarkis 1999): prolongate only the NON-overlap Ω⁰ᵢ.
//     NOT symmetric (⇒ FGMRES/BiCGSTAB) but converges better AND every output index is
//     written by exactly one subdomain ⇒ the apply is CONTENTION-FREE PARALLEL and
//     bit-exact across threads (the determinism moat).
//
// One-level Schwarz: convergence degrades ~1/H² with subdomain count (no coarse-space
// correction -- the two-level/balancing coarse space lives with AMG at v4k). A subdomain
// whose overlap BFS would exceed kSchwarzLocalMax falls back to its base block (dense-
// column safety net). Sorted-ascending Ωᵢ ⇒ the LU + gather order is deterministic.
// Real + complex (general LU; the adjoint solves the conjugate-transpose blocks).
// -----------------------------------------------------------------------

enum class SchwarzType : crd::u8
{
    Additive,
    Restricted
};

enum class SchwarzPartition : crd::u8
{
    Contiguous,
    NestedDissection
};

namespace detail
{
inline constexpr crd::u32 kSchwarzLocalMax = 1024U; // overlap-grown subdomain cap (dense LU bound)
}

template <typename T>
class SchwarzPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    SchwarzPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, crd::u32 block_size = 64,
                          crd::u32 overlap = 1, SchwarzType type = SchwarzType::Restricted,
                          SchwarzPartition partition = SchwarzPartition::Contiguous)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_sub_ptr(alloc), m_sub_idx(alloc), m_lu_ptr(alloc), m_lu(alloc), m_luh(alloc), m_piv(alloc)
        , m_pivh(alloc), m_base_ptr(alloc), m_base_loc(alloc), m_scratch(alloc)
        , m_n(a.rows()), m_type(type)
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "SchwarzPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "SchwarzPreconditioner: requires a compressed CSR matrix");
        build(a, block_size < 1 ? 1 : block_size, overlap, partition, alloc);
        const crd::u32 workers = crd::jobs::num_workers() == 0 ? 1U : crd::jobs::num_workers();
        m_scratch.resize(static_cast<crd::usize>(workers) * (m_maxm == 0 ? 1U : m_maxm));
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        run(m_lu, m_piv, r, z);
        return true;
    }
    // (Σ Rᵀ Aᵢᵢ⁻¹ R)ᴴ = Σ Rᵀ Aᵢᵢ⁻ᴴ R: solve the conjugate-transpose blocks.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        run(m_luh, m_pivh, r, z);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    [[nodiscard]] crd::u32 num_subdomains() const noexcept { return m_nd; }
    [[nodiscard]] crd::u32 max_subdomain() const noexcept { return m_maxm; }

private:
    [[nodiscard]] static T schwarz_conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }

    struct Ctx
    {
        const crd::u32* sub_ptr;
        const crd::u32* sub_idx;
        const crd::usize* lu_ptr;
        const T*        lu;
        const crd::u32* piv;
        const crd::u32* base_ptr;
        const crd::u32* base_loc;
        T*              scratch;
        const T*        rin;
        T*              zout;
        crd::u32        maxm;
    };

    // z = Σ R̃ᵀ Aᵢᵢ⁻¹ R r. RAS (default): disjoint base writes ⇒ parallel + bit-exact.
    // AS: full-Ωᵢ summed ⇒ serial (fixed order) for determinism.
    void run(const crd::containers::Array<T>& lu, const crd::containers::Array<crd::u32>& piv,
             crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const
    {
        if (m_type == SchwarzType::Restricted)
        {
            Ctx ctx{m_sub_ptr.data(), m_sub_idx.data(), m_lu_ptr.data(), lu.data(),    piv.data(),
                    m_base_ptr.data(), m_base_loc.data(), m_scratch.data(),  r.data(), z.data(), m_maxm};
            const crd::u32 jobs    = crd::jobs::num_workers() == 0 ? 1U : (crd::jobs::num_workers() < m_nd ? crd::jobs::num_workers() : m_nd);
            auto*          counter = crd::jobs::parallel_for(m_nd == 0 ? 1U : m_nd, jobs == 0 ? 1U : jobs,
                                                             [pc = &ctx](crd::u32 b, crd::u32 e) {
                for (crd::u32 i = b; i < e; ++i) { ras_subdomain(*pc, i); }
            });
            crd::jobs::wait(counter);
            crd::jobs::frame_reset();
            return;
        }
        // Additive: serial summed.
        for (crd::u32 g = 0; g < m_n; ++g) { z[g] = T{}; }
        T* scratch = m_scratch.data();
        for (crd::u32 i = 0; i < m_nd; ++i)
        {
            const crd::u32 s  = m_sub_ptr[i];
            const crd::u32 mi = m_sub_ptr[i + 1] - s;
            for (crd::u32 t = 0; t < mi; ++t) { scratch[t] = r[m_sub_idx[s + t]]; }
            detail::dense_lu_solve_factored<T>(lu.data() + m_lu_ptr[i], piv.data() + s, mi, scratch);
            for (crd::u32 t = 0; t < mi; ++t) { z[m_sub_idx[s + t]] = z[m_sub_idx[s + t]] + scratch[t]; }
        }
    }

    static void ras_subdomain(const Ctx& c, crd::u32 i)
    {
        const crd::u32 s  = c.sub_ptr[i];
        const crd::u32 mi = c.sub_ptr[i + 1] - s;
        T*             sc = c.scratch + static_cast<crd::usize>(crd::jobs::worker_index()) * c.maxm;
        for (crd::u32 t = 0; t < mi; ++t) { sc[t] = c.rin[c.sub_idx[s + t]]; } // gather R r
        detail::dense_lu_solve_factored<T>(c.lu + c.lu_ptr[i], c.piv + s, mi, sc); // Aᵢᵢ⁻¹
        for (crd::u32 k = c.base_ptr[i]; k < c.base_ptr[i + 1]; ++k) // scatter base Ω⁰ᵢ (disjoint)
        {
            const crd::u32 bl = c.base_loc[k];
            c.zout[c.sub_idx[s + bl]] = sc[bl];
        }
    }

    void build(const Csr& a, crd::u32 bs, crd::u32 overlap, SchwarzPartition partition,
               crd::memory::IAllocator* alloc)
    {
        const crd::u32  n     = m_n;
        const auto*     outer = a.pattern().outer_ptr.data();
        const auto*     inner = a.pattern().inner_idx.data();
        const T*        vals  = a.values().values.data();
        if (n == 0) { m_sub_ptr.push_back(0); m_lu_ptr.push_back(0); m_base_ptr.push_back(0); m_nd = 0; return; }

        // base_owner[g] = subdomain chunk owning g (contiguous, or ND-reordered chunk).
        crd::containers::Array<crd::u32> base_owner(alloc);
        base_owner.resize(n);
        if (partition == SchwarzPartition::NestedDissection)
        {
            crd::hesap::ordering::Permutation p = crd::hesap::ordering::nd_order(a.pattern(), alloc);
            for (crd::u32 g = 0; g < n; ++g) { base_owner[g] = p.inv_perm[g] / bs; }
        }
        else
        {
            for (crd::u32 g = 0; g < n; ++g) { base_owner[g] = g / bs; }
        }
        m_nd = (n + bs - 1) / bs;

        // Base lists by counting-sort (ascending g within each chunk ⇒ deterministic).
        crd::containers::Array<crd::u32> base_ptr(alloc), base_glb(alloc), wp(alloc);
        base_ptr.resize(static_cast<crd::usize>(m_nd) + 1);
        for (crd::u32 i = 0; i <= m_nd; ++i) { base_ptr[i] = 0; }
        for (crd::u32 g = 0; g < n; ++g) { ++base_ptr[base_owner[g] + 1]; }
        for (crd::u32 i = 0; i < m_nd; ++i) { base_ptr[i + 1] += base_ptr[i]; }
        base_glb.resize(n);
        wp.resize(m_nd);
        for (crd::u32 i = 0; i < m_nd; ++i) { wp[i] = base_ptr[i]; }
        for (crd::u32 g = 0; g < n; ++g) { base_glb[wp[base_owner[g]]++] = g; }

        // Per-subdomain scratch markers (reused, reset after each).
        crd::containers::Array<crd::i32> visited(alloc), lmap(alloc);
        crd::containers::Array<crd::u32> omega(alloc);
        crd::containers::Array<T>        ah(alloc); // adjoint local block scratch
        visited.resize(n);
        lmap.resize(n);
        for (crd::u32 g = 0; g < n; ++g) { visited[g] = -1; lmap[g] = -1; }

        m_sub_ptr.push_back(0);
        m_lu_ptr.push_back(0);
        m_base_ptr.push_back(0);

        for (crd::u32 i = 0; i < m_nd; ++i)
        {
            // ---- overlap BFS from the base set ----
            omega.clear();
            for (crd::u32 k = base_ptr[i]; k < base_ptr[i + 1]; ++k)
            {
                const crd::u32 g = base_glb[k];
                visited[g]       = 1;
                omega.push_back(g);
            }
            crd::u32 layer_start = 0;
            for (crd::u32 lay = 0; lay < overlap; ++lay)
            {
                const crd::u32 layer_end = static_cast<crd::u32>(omega.size());
                bool           overflow  = false;
                for (crd::u32 k = layer_start; k < layer_end && !overflow; ++k)
                {
                    const crd::u32 g = omega[k];
                    for (crd::u32 q = outer[g]; q < outer[g + 1]; ++q)
                    {
                        const crd::u32 nb = inner[q];
                        if (visited[nb] < 0)
                        {
                            if (omega.size() >= detail::kSchwarzLocalMax) { overflow = true; break; }
                            visited[nb] = 1;
                            omega.push_back(nb);
                        }
                    }
                }
                layer_start = layer_end;
                if (overflow) { break; }
            }
            std::sort(omega.data(), omega.data() + omega.size());
            const crd::u32 mi = static_cast<crd::u32>(omega.size());
            for (crd::u32 r = 0; r < mi; ++r) { lmap[omega[r]] = static_cast<crd::i32>(r); }

            // ---- extract A_ii (dense m×m row-major) + its conjugate transpose ----
            const crd::usize base_off = m_lu.size();
            m_lu.resize(base_off + static_cast<crd::usize>(mi) * mi);
            ah.resize(static_cast<crd::usize>(mi) * mi);
            for (crd::usize t = 0; t < static_cast<crd::usize>(mi) * mi; ++t) { m_lu[base_off + t] = T{}; ah[t] = T{}; }
            for (crd::u32 r = 0; r < mi; ++r)
            {
                const crd::u32 gr = omega[r];
                for (crd::u32 q = outer[gr]; q < outer[gr + 1]; ++q)
                {
                    const crd::i32 lc = lmap[inner[q]];
                    if (lc >= 0)
                    {
                        const crd::u32 c                                       = static_cast<crd::u32>(lc);
                        m_lu[base_off + static_cast<crd::usize>(r) * mi + c]    = vals[q];
                        ah[static_cast<crd::usize>(c) * mi + r]                 = schwarz_conj(vals[q]); // Aᴴ[c,r]=conj(A[r,c])
                    }
                }
            }
            // factor A_ii (in m_lu) and Aᴴ (append to m_luh)
            for (crd::u32 t = 0; t < mi; ++t) { m_piv.push_back(0); m_pivh.push_back(0); }
            detail::dense_lu_factor<T>(m_lu.data() + base_off, mi, m_piv.data() + m_sub_ptr.back());
            const crd::usize h_off = m_luh.size();
            m_luh.resize(h_off + static_cast<crd::usize>(mi) * mi);
            for (crd::usize t = 0; t < static_cast<crd::usize>(mi) * mi; ++t) { m_luh[h_off + t] = ah[t]; }
            detail::dense_lu_factor<T>(m_luh.data() + h_off, mi, m_pivh.data() + m_sub_ptr.back());

            // ---- store Ωᵢ + base-local indices; reset markers ----
            for (crd::u32 r = 0; r < mi; ++r) { m_sub_idx.push_back(omega[r]); }
            for (crd::u32 k = base_ptr[i]; k < base_ptr[i + 1]; ++k)
            {
                m_base_loc.push_back(static_cast<crd::u32>(lmap[base_glb[k]])); // local idx of a base vertex
            }
            for (crd::u32 r = 0; r < mi; ++r) { visited[omega[r]] = -1; lmap[omega[r]] = -1; }

            m_sub_ptr.push_back(static_cast<crd::u32>(m_sub_idx.size()));
            m_lu_ptr.push_back(m_lu.size());
            m_base_ptr.push_back(static_cast<crd::u32>(m_base_loc.size()));
            if (mi > m_maxm) { m_maxm = mi; }
        }
    }

    crd::containers::Array<crd::u32>  m_sub_ptr;  // [nd+1] offsets into m_sub_idx / m_piv
    crd::containers::Array<crd::u32>  m_sub_idx;  // [Σmᵢ] sorted global indices of Ωᵢ
    crd::containers::Array<crd::usize> m_lu_ptr;  // [nd+1] offsets into m_lu / m_luh
    crd::containers::Array<T>         m_lu;       // [Σmᵢ²] LU of Aᵢᵢ
    crd::containers::Array<T>         m_luh;      // [Σmᵢ²] LU of Aᵢᵢᴴ (adjoint)
    crd::containers::Array<crd::u32>  m_piv;      // [Σmᵢ] pivots (offset = m_sub_ptr)
    crd::containers::Array<crd::u32>  m_pivh;     // [Σmᵢ] adjoint pivots
    crd::containers::Array<crd::u32>  m_base_ptr; // [nd+1] offsets into m_base_loc
    crd::containers::Array<crd::u32>  m_base_loc; // [n] local indices of Ω⁰ᵢ within Ωᵢ
    mutable crd::containers::Array<T> m_scratch;  // [workers × maxm] per-worker local rhs/sol
    crd::u32                          m_n;
    crd::u32                          m_nd   = 0;
    crd::u32                          m_maxm = 0;
    SchwarzType                       m_type;
};

} // namespace crd::hesap::preconditioners
