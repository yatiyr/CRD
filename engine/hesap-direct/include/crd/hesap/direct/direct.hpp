#pragma once

// crd-hesap-direct — sparse DIRECT factorization (Phase 3.1.6 v5).
//
// Umbrella header. v5a-1 ships the substrate: IFactorization<T> (the common
// factor-once / solve-many interface) + Frontal<T> / extend_add (the
// multifrontal assembly kernel). The per-family factorizations land in their
// own headers as the cluster progresses:
//   - supernodal_cholesky.hpp (v5a) — left-looking supernodal Cholesky
//   - sparse_lu.hpp           (v5b) — Gilbert-Peierls + supernodal LU
//   - multifrontal_qr.hpp     (v5c) — SuiteSparseQR-class
//   - multifrontal_ldlt.hpp   (v5d) — Duff-Reid symmetric indefinite

#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/direct/frontal.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
