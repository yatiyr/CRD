#pragma once

// -----------------------------------------------------------------------
// crd-hesap-sparse umbrella header. Phase 3.1.6 v1a.
//
// Elite, multi-threaded, deterministic sparse-matrix substrate. Goal: beat
// Eigen's SparseCore on the kernels that matter (spmv, spgemm). v1a-1 ships
// the pattern / values / analysis trinity + handles + format tag +
// topology hash + 'HSPM' CRDR pin. Builders (COO->CSR/CSC) land in v1a-2/3;
// kernels (spmv) in v1b. Determinism spec: docs/systems/hesap-sparse.md.
// -----------------------------------------------------------------------

#include <crd/hesap/sparse/analysis_handle.hpp>
#include <crd/hesap/sparse/bsr.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/dia.hpp>
#include <crd/hesap/sparse/element_wise.hpp>
#include <crd/hesap/sparse/ell.hpp>
#include <crd/hesap/sparse/matrix_market.hpp>
#include <crd/hesap/sparse/queries.hpp>
#include <crd/hesap/sparse/sddmm.hpp>
#include <crd/hesap/sparse/sell.hpp>
#include <crd/hesap/sparse/sell_parallel.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_id.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/hesap/sparse/sparse_values.hpp>
#include <crd/hesap/sparse/spgemm.hpp>
#include <crd/hesap/sparse/spgemm_hash.hpp>
#include <crd/hesap/sparse/spgemm_parallel.hpp>
#include <crd/hesap/sparse/spmm.hpp>
#include <crd/hesap/sparse/spmv.hpp>
#include <crd/hesap/sparse/structural.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
