#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::direct
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v5e-1c — Hierarchically Semi-Separable (HSS) matrix (Xia,
// Chandrasekaran, Gu, Li 2010). A rank-structured representation of a dense
// matrix whose OFF-DIAGONAL blocks are low rank, stored with NESTED bases so
// the storage + apply are near-linear. This is the substrate the STRUMPACK-
// style HSS-embedded multifrontal (v5e-2) compresses large fronts into.
//
// SYMMETRIC HSS (this slice): U == V, R == W, B_{c2,c1} == B_{c1,c2}ᵀ — the
// 3D-elliptic SPD-front case (BLR covers the general production path; ADR
// dossier). General (nonsymmetric) HSS is the consumer-driven extension; the
// generator storage is laid out so adding V/W/B_lower later is ADDITIVE.
//
// Binary cluster tree over the index set [0, n). Per node:
//   - leaf k:      D_k (n_k × n_k diagonal block), U_k (n_k × rank_k basis)
//   - internal p:  B_p = B_{left,right} (rank_left × rank_right sibling coupling)
//   - non-root k:  R_k (rank_k × rank_parent translation): the parent basis is
//                  U_parent = diag(U_left, U_right) · [R_left; R_right].
//
// INVARIANT (relied on by the v5e-1d ULV factorization — ULV is Unitary-Lower-
// Unitary, the solve applies orthogonal transforms to the bases): `U_k` has
// ORTHONORMAL columns. The from-dense construction (v5e-1c-2) produces these
// via the leading left singular vectors of the HSS block row.
//
// The matvec (`hss_matvec`) and the dense expansion (`hss_to_dense`) are TWO
// INDEPENDENT code paths (the latter is an explicit nested-generator expansion,
// NOT matvec-on-identity) so each cross-checks the other against ground truth.
//
// Real f32/f64. Lower layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T>
struct HssNode
{
    bool is_leaf = false;
    crd::i64 parent = -1;             // -1 for the root
    crd::i64 left = -1;               // child node ids (-1 for a leaf)
    crd::i64 right = -1;
    crd::usize i0 = 0;                // index span [i0, i1) this node covers
    crd::usize i1 = 0;
    crd::usize rank = 0;              // basis dimension r_k

    crd::hesap::dense::Matrix<T> d;   // leaf: n_k × n_k diagonal block
    crd::hesap::dense::Matrix<T> u;   // leaf: n_k × rank_k orthonormal column basis
    crd::hesap::dense::Matrix<T> r;   // non-root: rank_k × rank_parent translation
    crd::hesap::dense::Matrix<T> b;   // internal: rank_left × rank_right coupling

    explicit HssNode(crd::memory::IAllocator* alloc)
        : d(alloc), u(alloc), r(alloc), b(alloc)
    {
    }

    HssNode(HssNode&&) noexcept = default;
    HssNode& operator=(HssNode&&) noexcept = default;
    HssNode(const HssNode&) = delete;
    HssNode& operator=(const HssNode&) = delete;

    [[nodiscard]] crd::usize size() const noexcept { return i1 - i0; }
};

template <typename T>
struct HssMatrix
{
    crd::usize n = 0;                          // matrix dimension
    crd::memory::IAllocator* alloc = nullptr;
    crd::containers::Array<HssNode<T>> nodes;  // node 0 = root; parent id < child ids

    explicit HssMatrix(crd::memory::IAllocator* a) : alloc(a), nodes(a) {}

    HssMatrix(HssMatrix&&) noexcept = default;
    HssMatrix& operator=(HssMatrix&&) noexcept = default;
    HssMatrix(const HssMatrix&) = delete;
    HssMatrix& operator=(const HssMatrix&) = delete;

    [[nodiscard]] crd::usize num_nodes() const noexcept { return nodes.size(); }
    [[nodiscard]] const HssNode<T>& root() const noexcept { return nodes[0]; }
};

// =======================================================================
// build_cluster_tree — populate `h.nodes` with a balanced binary cluster tree
// over [0, n) by recursive index bisection down to `leaf_size`. Node ids are
// assigned PRE-ORDER so a parent's id is always less than its children's (the
// upward/downward matvec sweeps rely on this). Topology only: is_leaf / parent
// / left / right / i0 / i1 are set; ranks + generators are filled by the
// caller (the hand-built test, or the v5e-1c-2 from-dense construction).
// =======================================================================
template <typename T>
void build_cluster_tree(HssMatrix<T>& h, crd::usize n, crd::usize leaf_size);

// =======================================================================
// hss_matvec — y = H · x. Upward sweep g (leaf: g = Uᵀx; internal: g =
// R_leftᵀ g_left + R_rightᵀ g_right), downward sweep f (root children from the
// coupling B; deeper nodes add the parent translation R·f_parent), then leaf
// y(I_k) = D_k x(I_k) + U_k f_k. Caller guarantees x.size() == y.size() == n.
// =======================================================================
template <typename T>
void hss_matvec(const HssMatrix<T>& h, crd::containers::ConstSpan<T> x, crd::containers::Span<T> y);

// =======================================================================
// hss_to_dense — expand the HSS representation to a dense n × n matrix by an
// INDEPENDENT nested-generator expansion (diagonal = D_k; off-diagonal block
// (I_k, I_l) = U_k · gUp(k) · B^{orient} · gUp(l)ᵀ · U_lᵀ, the R-chains up to
// the lowest common ancestor). Distinct from `hss_matvec` ⇒ a genuine
// cross-check. For testing / small matrices.
// =======================================================================
template <typename T>
[[nodiscard]] crd::hesap::dense::Matrix<T> hss_to_dense(crd::memory::IAllocator* alloc, const HssMatrix<T>& h);

// =======================================================================
// build_hss_from_dense — construct a symmetric HSS approximation of a dense
// SYMMETRIC matrix A (n × n) to RELATIVE tolerance `tol` (a node's block-row
// singular vectors with σ_i ≤ tol·σ_max are dropped), with leaf blocks of size
// ≤ `leaf_size` and an optional per-node rank cap `max_rank` (0 = none).
//
// Telescoping (deterministic, no RNG — exact SVD of the FORMED block row, the
// matrix-free randomized construction is v5e-2): each node's orthonormal basis
// Ufull = leading left singular vectors of its block row A(I_k, I_k^c); the
// sibling coupling B = Ufull_c1ᵀ·A(I_c1,I_c2)·Ufull_c2; the translation
// R_ci = Ufull_ciᵀ·(Ufull_parent restricted to c_i's rows) — valid because
// I_parentᶜ ⊆ I_childᶜ makes the parent block-row range nest in the children's.
// Orthonormal bases ⇒ the HssMatrix ULV invariant holds. Requires A symmetric
// (U serves as both row and column basis). O(n³)-class. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] HssMatrix<T> build_hss_from_dense(crd::memory::IAllocator* alloc,
                                                const crd::hesap::dense::Matrix<T>& a, crd::usize leaf_size,
                                                crd::hesap::dense::RealType<T> tol, crd::usize max_rank = 0);

} // namespace crd::hesap::direct
