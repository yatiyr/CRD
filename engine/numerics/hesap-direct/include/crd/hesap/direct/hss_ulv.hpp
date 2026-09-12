#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/direct/hss.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v5e-1d — ULV factorization + solve of a symmetric POSITIVE-
// DEFINITE HSS matrix (Chandrasekaran-Gu-Pals 2006 / Xia 2010). ULV =
// "Unitary-Lower-Unitary": at each node an orthogonal transform splits the
// basis into a coupled "skeleton" part and a decoupled "fully-summed" part;
// the fully-summed part is eliminated by a partial Cholesky; its Schur
// complement merges into the parent. O(r²N) factor + O(rN) solve vs O(N³)
// dense — the factor-once/solve-many lever for rank-structured operators.
//
// Implements `IFactorization<T>` (multi-RHS from day one). SPD-only: a
// non-positive Cholesky pivot sets `info() != 0` and `solve` returns false
// (the dense-Cholesky contract). Relies on the HssMatrix orthonormal-U
// invariant (v5e-1c). Serial; RNG-free ⇒ reproducible (the cross-thread moat
// is v5e-2's, front-parallel). Real f32/f64. Lower layer: raw scalars.
//
// Skeleton-top convention: QR(basis) ⇒ Qᵀ·basis = [T_k; 0], so the TOP r_k
// rotated coords couple to the outside (skeleton) and the BOTTOM p = m−r_k are
// fully summed and eliminated. Merge: D_parent = [[Ŝ_c1, M],[Mᵀ, Ŝ_c2]] with
// M = T_c1·B_{c1,c2}·T_c2ᵀ, basis_parent = [T_c1·R_c1; T_c2·R_c2].
// -----------------------------------------------------------------------

template <typename T>
struct UlvNodeFactor
{
    crd::usize m = 0;  // node working size (n_k leaf; r_c1+r_c2 internal)
    crd::usize p = 0;  // fully-summed size (= m - rank)
    crd::usize r = 0;  // skeleton size (= node rank)
    // The basis rotation is the QR of the m×r basis, stored as IMPLICIT
    // Householder reflectors (Qᵀ·basis = [T; 0]) — applied via apply_q_block /
    // apply_q_transpose in O(m²r) / O(mr), never materialised as a dense m×m Q.
    crd::hesap::dense::QR<T, crd::hesap::dense::Layout::RowMajor> qr;
    crd::hesap::dense::Matrix<T> l;  // Cholesky lower: p × p (non-root) or m × m (root)
    crd::hesap::dense::Matrix<T> w;  // p × r = L⁻¹·D21: lets the solve do 2 tri-solves not 4

    explicit UlvNodeFactor(crd::memory::IAllocator* alloc) : qr(alloc), l(alloc), w(alloc) {}
    UlvNodeFactor(UlvNodeFactor&&) noexcept = default;
    UlvNodeFactor& operator=(UlvNodeFactor&&) noexcept = default;
    UlvNodeFactor(const UlvNodeFactor&) = delete;
    UlvNodeFactor& operator=(const UlvNodeFactor&) = delete;
};

template <typename T>
class HssUlv final : public IFactorization<T>
{
public:
    explicit HssUlv(crd::memory::IAllocator* alloc)
        : m_alloc(alloc), m_is_leaf(alloc), m_parent(alloc), m_left(alloc), m_right(alloc), m_i0(alloc),
          m_i1(alloc), m_rank(alloc), m_fac(alloc)
    {
    }

    HssUlv(HssUlv&&) noexcept = default;
    HssUlv& operator=(HssUlv&&) noexcept = default;
    HssUlv(const HssUlv&) = delete;
    HssUlv& operator=(const HssUlv&) = delete;

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_nnz; }
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    // Populated by factor_hss_ulv (in the .cpp, same namespace).
    crd::memory::IAllocator* m_alloc;
    crd::usize m_n = 0;
    crd::usize m_info = 0;
    crd::u64 m_nnz = 0;
    crd::containers::Array<crd::u8> m_is_leaf;
    crd::containers::Array<crd::i64> m_parent;
    crd::containers::Array<crd::i64> m_left;
    crd::containers::Array<crd::i64> m_right;
    crd::containers::Array<crd::usize> m_i0;
    crd::containers::Array<crd::usize> m_i1;
    crd::containers::Array<crd::usize> m_rank;
    crd::containers::Array<UlvNodeFactor<T>> m_fac;  // per node, parent id < child ids
};

// =======================================================================
// factor_hss_ulv — ULV factorization of a symmetric SPD HSS matrix. On a
// non-positive pivot the returned factorization has `info() != 0`. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] HssUlv<T> factor_hss_ulv(crd::memory::IAllocator* alloc, const HssMatrix<T>& h);

} // namespace crd::hesap::direct
