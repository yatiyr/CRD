#pragma once

// Umbrella header for crd-hesap-dense.
// v0a: matrix-type catalog header (shells).
// v0b: Vector<T> + BLAS L1 + CLI.
// v0c: Matrix + MatrixView + Symmetric/Hermitian/Triangular/Banded + BLAS L2.

#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_catalog.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
