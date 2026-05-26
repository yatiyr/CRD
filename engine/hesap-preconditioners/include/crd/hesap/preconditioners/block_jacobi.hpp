#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// BlockJacobiPreconditioner<T> -- block-diagonal (block-Jacobi) preconditioner.
// Phase 3.1.6 v4a-2.
//
// Partitions [0,n) into contiguous blocks of size `block_size` (the last block
// may be shorter). Each diagonal block D_k is densely extracted and explicitly
// inverted (small blocks ⇒ a stored explicit inverse makes the apply a per-block
// dense matvec; a preconditioner is approximate so the inverse's conditioning is
// a non-issue). The action z = M⁻¹ r solves each block independently ⇒
// embarrassingly parallel AND bit-deterministic (disjoint blocks; fixed-order
// per-block reduction).
//
// The inversion is a self-contained Gauss-Jordan with partial pivoting (cabs1
// magnitude), complex-capable for ALL four T -- a dedicated small-block kernel
// (cf. BSR's D(sparse)-6) rather than dragging the blocked dense-LU machinery
// for tiny b×b blocks. Works for any matrix with nonsingular diagonal blocks.
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
[[nodiscard]] inline T bj_conj(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return T{v.re, -v.im};
    }
    else
    {
        return v;
    }
}

template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> bj_cabs1(T v) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        const R re = v.re < R{0} ? -v.re : v.re;
        const R im = v.im < R{0} ? -v.im : v.im;
        return re + im;
    }
    else
    {
        return v < R{0} ? -v : v;
    }
}

// Invert `bs`×`bs` row-major `a` (destroyed) into row-major `inv`. Returns
// false if a pivot magnitude is exactly zero (singular block).
template <typename T>
[[nodiscard]] inline bool bj_invert(T* a, T* inv, crd::u32 bs) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    for (crd::u32 i = 0; i < bs; ++i)
    {
        for (crd::u32 j = 0; j < bs; ++j)
        {
            inv[i * bs + j] = (i == j) ? T(1) : T{};
        }
    }
    for (crd::u32 c = 0; c < bs; ++c)
    {
        crd::u32 piv  = c;
        R        best = bj_cabs1<T>(a[c * bs + c]);
        for (crd::u32 r = c + 1; r < bs; ++r)
        {
            const R m = bj_cabs1<T>(a[r * bs + c]);
            if (m > best)
            {
                best = m;
                piv  = r;
            }
        }
        if (best == R{0})
        {
            return false;
        }
        if (piv != c)
        {
            for (crd::u32 j = 0; j < bs; ++j)
            {
                T t1 = a[piv * bs + j];
                a[piv * bs + j] = a[c * bs + j];
                a[c * bs + j]   = t1;
                T t2 = inv[piv * bs + j];
                inv[piv * bs + j] = inv[c * bs + j];
                inv[c * bs + j]   = t2;
            }
        }
        const T inv_pivot = T(1) / a[c * bs + c];
        for (crd::u32 j = 0; j < bs; ++j)
        {
            a[c * bs + j]   = a[c * bs + j] * inv_pivot;
            inv[c * bs + j] = inv[c * bs + j] * inv_pivot;
        }
        for (crd::u32 r = 0; r < bs; ++r)
        {
            if (r == c)
            {
                continue;
            }
            const T f = a[r * bs + c];
            if (f == T{})
            {
                continue;
            }
            for (crd::u32 j = 0; j < bs; ++j)
            {
                a[r * bs + j]   = a[r * bs + j] - f * a[c * bs + j];
                inv[r * bs + j] = inv[r * bs + j] - f * inv[c * bs + j];
            }
        }
    }
    return true;
}
} // namespace detail

template <typename T>
class BlockJacobiPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    BlockJacobiPreconditioner(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                              crd::u32 block_size, crd::memory::IAllocator* alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_inv(alloc)
        , m_block_start(alloc)
        , m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "BlockJacobiPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "BlockJacobiPreconditioner: requires a compressed CSR matrix");
        CRD_ASSERT_MSG(block_size >= 1, "BlockJacobiPreconditioner: block_size must be >= 1");

        const auto&    pat   = a.pattern();
        const auto*    outer = pat.outer_ptr.data();
        const auto*    inner = pat.inner_idx.data();
        const T*       vals  = a.values().values.data();
        const crd::u32 nb    = (m_n + block_size - 1) / block_size;

        m_block_start.reserve(nb + 1);
        crd::usize total_inv = 0;
        for (crd::u32 k = 0; k < nb; ++k)
        {
            const crd::u32 s  = k * block_size;
            const crd::u32 e  = (s + block_size < m_n) ? (s + block_size) : m_n;
            const crd::u32 bs = e - s;
            m_block_start.push_back(s);
            total_inv += static_cast<crd::usize>(bs) * bs;
        }
        m_block_start.push_back(m_n);
        m_inv.resize(total_inv);

        crd::containers::Array<T> dense(alloc);
        crd::usize                inv_off = 0;
        for (crd::u32 k = 0; k < nb; ++k)
        {
            const crd::u32 s  = m_block_start[k];
            const crd::u32 e  = m_block_start[k + 1];
            const crd::u32 bs = e - s;

            dense.resize(static_cast<crd::usize>(bs) * bs);
            for (crd::usize t = 0; t < dense.size(); ++t)
            {
                dense[t] = T{};
            }
            for (crd::u32 i = s; i < e; ++i)
            {
                for (crd::u32 t = outer[i]; t < outer[i + 1]; ++t)
                {
                    const crd::u32 j = inner[t];
                    if (j >= s && j < e)
                    {
                        dense[static_cast<crd::usize>(i - s) * bs + (j - s)] = vals[t];
                    }
                }
            }
            [[maybe_unused]] const bool ok = detail::bj_invert<T>(dense.data(), m_inv.data() + inv_off, bs);
            CRD_ASSERT_MSG(ok, "BlockJacobiPreconditioner: singular diagonal block");
            inv_off += static_cast<crd::usize>(bs) * bs;
        }
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        crd::usize     inv_off = 0;
        const crd::u32 nb      = static_cast<crd::u32>(m_block_start.size()) - 1;
        for (crd::u32 k = 0; k < nb; ++k)
        {
            const crd::u32 s  = m_block_start[k];
            const crd::u32 e  = m_block_start[k + 1];
            const crd::u32 bs = e - s;
            for (crd::u32 i = 0; i < bs; ++i)
            {
                T acc{};
                for (crd::u32 j = 0; j < bs; ++j)
                {
                    acc = acc + m_inv[inv_off + static_cast<crd::usize>(i) * bs + j] * x[s + j];
                }
                y[s + i] = acc;
            }
            inv_off += static_cast<crd::usize>(bs) * bs;
        }
        return true;
    }

    // Mᴴ action: each block applies the conjugate-transpose of its stored inverse,
    // (Bᴴ)[i,j] = conj(B[j,i]). Required by two-sided solvers (QMR) where the block
    // inverse is not Hermitian for general A. For real T this is the plain transpose.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        crd::usize     inv_off = 0;
        const crd::u32 nb      = static_cast<crd::u32>(m_block_start.size()) - 1;
        for (crd::u32 k = 0; k < nb; ++k)
        {
            const crd::u32 s  = m_block_start[k];
            const crd::u32 e  = m_block_start[k + 1];
            const crd::u32 bs = e - s;
            for (crd::u32 i = 0; i < bs; ++i)
            {
                T acc{};
                for (crd::u32 j = 0; j < bs; ++j)
                {
                    acc = acc + detail::bj_conj<T>(m_inv[inv_off + static_cast<crd::usize>(j) * bs + i]) * x[s + j];
                }
                y[s + i] = acc;
            }
            inv_off += static_cast<crd::usize>(bs) * bs;
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    crd::containers::Array<T>        m_inv;         // concatenated row-major bs×bs block inverses
    crd::containers::Array<crd::u32> m_block_start; // length nb+1
    crd::u32                         m_n;
};

} // namespace crd::hesap::preconditioners
