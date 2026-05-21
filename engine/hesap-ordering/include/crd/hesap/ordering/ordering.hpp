#pragma once

// -----------------------------------------------------------------------
// crd-hesap-ordering umbrella header. Phase 3.1.6 v2.
//
// Fill-reducing reorderings + symbolic factorisation — the bridge from v1
// sparse storage to v5 sparse direct solvers. v2a: graph + permutation + RCM +
// minimal nnz(L) fill metric. (AMD = v2b, full symbolic = v2c, multilevel-METIS
// nested dissection = v2d/e.) Determinism: D(ord)-1..4 (see CMakeLists / docs).
// -----------------------------------------------------------------------

#include <crd/hesap/ordering/adjacency_graph.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/ordering/rcm.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
