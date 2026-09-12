#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SparseFormat — storage layout tag carried by SparsePattern and used as
// the compile-time NTTP on SparseMatrix<T, Format> (D21 precedent from the
// dense Layout NTTP).
//
// For sparse matrices the *orientation* IS the format: CSR is row-oriented,
// CSC is column-oriented. There is therefore no separate `Layout` parameter
// (unlike dense Matrix<T, Layout>) — the phase-doc shorthand
// `SparseMatrix<T, Format, Layout>` collapses to `SparseMatrix<T, Format>`.
// Pinned as a v1a decision.
//
// v1a ships COO (builder side) + CSR + CSC. The remaining tags are reserved
// so the enum is stable across the v1 cluster and `topology_hash` /
// conversion-graph code can switch over a fixed value set from day 1.
// -----------------------------------------------------------------------

enum class SparseFormat : crd::u8
{
    Coo = 0,   // coordinate triplets (assembly only; not a compressed storage)
    Csr = 1,   // compressed sparse row     (v1a)
    Csc = 2,   // compressed sparse column  (v1a)
    Bsr = 3,   // block sparse row          (v1f)
    Ell = 4,   // ELLPACK                    (v1f)
    Sell = 5,  // SELL-C-sigma               (v1b)
    Dia = 6,   // diagonal / banded          (v1f)
};

} // namespace crd::hesap::sparse
