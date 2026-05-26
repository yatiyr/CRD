#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp> // transpose (for the true non-Hermitian adjoint)
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// SsorPreconditioner<T> -- Symmetric Successive Over-Relaxation. Phase 3.1.6 v4a-2.
//
// M⁻¹ = (D/ω + U)⁻¹ · [((2−ω)/ω) D] · (D/ω + L)⁻¹, with A = L + D + U (L/U the
// strict lower/upper parts). For symmetric A (U = Lᵀ) and ω∈(0,2), M is SPD; for
// Hermitian A (U = Lᴴ), M is HPD -- so it is a valid CG preconditioner. ω = 1 is
// symmetric Gauss-Seidel.
//
// ADJOINT (M⁻ᴴ): M_SSOR(A)ᴴ = M_SSOR(Aᴴ) (the SSOR operator of the conjugate
// transpose), so apply_adjoint runs the SAME forward/backward sweep on a stored
// Aᴴ = conj(Aᵀ). This is the TRUE adjoint for ANY A (Hermitian or not); for
// Hermitian A, Aᴴ = A so it coincides with apply. Required by two-sided solvers
// (QMR) on general nonsymmetric A.
//
// Precondition: nonzero diagonal. The forward + backward triangular sweeps are
// inherently SEQUENTIAL (each row depends on earlier ones), hence trivially
// bit-deterministic + thread-count independent. Owns a conjugate-transpose copy.
// -----------------------------------------------------------------------

template <typename T>
class SsorPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    SsorPreconditioner(const Csr& a, R omega, crd::memory::IAllocator* alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_a(&a)
        , m_ah(build_adjoint(a, alloc))
        , m_diag(alloc)
        , m_diag_h(alloc)
        , m_t(alloc)
        , m_n(a.rows())
        , m_omega(omega)
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "SsorPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "SsorPreconditioner: requires a compressed CSR matrix");
        CRD_ASSERT_MSG(omega > static_cast<R>(0) && omega < static_cast<R>(2), "SsorPreconditioner: omega in (0,2)");
        m_diag.resize(m_n);
        m_diag_h.resize(m_n);
        m_t.resize(m_n);
        extract_diag(a, m_diag);
        // Aᴴ's diagonal is conj of A's diagonal.
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            m_diag_h[i] = ssor_conj(m_diag[i]);
        }
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        run_sweep(*m_a, m_diag, r, z);
        return true;
    }

    // M⁻ᴴ r = M_SSOR(Aᴴ)⁻¹ r: the same sweep on the conjugate transpose.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        run_sweep(m_ah, m_diag_h, r, z);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    [[nodiscard]] static T ssor_conj(T v) noexcept
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

    // Aᴴ = conj(Aᵀ): structural transpose then conjugate the values.
    [[nodiscard]] static Csr build_adjoint(const Csr& a, crd::memory::IAllocator* alloc)
    {
        Csr   at   = crd::hesap::sparse::transpose<T>(a, alloc);
        auto& vals = at.values().values;
        for (crd::usize k = 0; k < vals.size(); ++k)
        {
            vals[k] = ssor_conj(vals[k]);
        }
        return at;
    }

    static void extract_diag(const Csr& a, crd::containers::Array<T>& diag)
    {
        const auto&    pat   = a.pattern();
        const auto*    outer = pat.outer_ptr.data();
        const auto*    inner = pat.inner_idx.data();
        const T*       vals  = a.values().values.data();
        const crd::u32 n     = a.rows();
        for (crd::u32 i = 0; i < n; ++i)
        {
            T d = T{};
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                if (inner[k] == i)
                {
                    d = vals[k];
                    break;
                }
            }
            CRD_ASSERT_MSG(!(d == T{}), "SsorPreconditioner: zero (or missing) diagonal entry");
            diag[i] = d;
        }
    }

    // M_SSOR(mat)⁻¹ r → z: forward (D/ω+L), diagonal scale ((2−ω)/ω)D, backward (D/ω+U).
    void run_sweep(const Csr& mat, const crd::containers::Array<T>& diag, crd::containers::ConstSpan<T> r,
                   crd::containers::Span<T> z) const
    {
        const auto& pat   = mat.pattern();
        const auto* outer = pat.outer_ptr.data();
        const auto* inner = pat.inner_idx.data();
        const T*    vals  = mat.values().values.data();
        const R     inv_w = static_cast<R>(1) / m_omega;
        const R     scale = (static_cast<R>(2) - m_omega) * inv_w; // (2−ω)/ω

        for (crd::u32 i = 0; i < m_n; ++i)
        {
            T acc = r[i];
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                const crd::u32 j = inner[k];
                if (j < i)
                {
                    acc = acc - vals[k] * m_t[j];
                }
            }
            m_t[i] = acc / (diag[i] * inv_w);
        }
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            m_t[i] = (scale * diag[i]) * m_t[i];
        }
        for (crd::u32 ii = 0; ii < m_n; ++ii)
        {
            const crd::u32 i   = m_n - 1 - ii;
            T              acc = m_t[i];
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                const crd::u32 j = inner[k];
                if (j > i)
                {
                    acc = acc - vals[k] * z[j];
                }
            }
            z[i] = acc / (diag[i] * inv_w);
        }
    }

    const Csr*                       m_a;     // A (non-owning)
    Csr                              m_ah;    // Aᴴ = conj(Aᵀ) (owned, for the adjoint sweep)
    crd::containers::Array<T>        m_diag;  // diag(A)
    crd::containers::Array<T>        m_diag_h; // diag(Aᴴ) = conj(diag(A))
    mutable crd::containers::Array<T> m_t;    // sweep scratch (sized once)
    crd::u32                         m_n;
    R                                m_omega;
};

} // namespace crd::hesap::preconditioners
