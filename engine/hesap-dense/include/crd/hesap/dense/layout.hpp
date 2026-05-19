#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Layout / TriangularSide / TriangularDiag — small enums shared by every
// matrix type in the catalog. Lifted into their own header so matrix.hpp
// can consume them without pulling in the full matrix_catalog.hpp (which
// itself pulls matrix.hpp for the Matrix/MatrixView definitions).
// -----------------------------------------------------------------------

enum class Layout : crd::u8
{
    RowMajor = 0,
    ColMajor = 1,
};

enum class TriangularSide : crd::u8
{
    Lower = 0,
    Upper = 1,
};

enum class TriangularDiag : crd::u8
{
    Explicit = 0,  // diagonal stored in the matrix
    UnitDiag = 1,  // diagonal is implicit unit (saves storage)
};

// BLAS transpose mode for gemv / gbmv / trmv / trsv / tbmv / tbsv.
enum class Trans : crd::u8
{
    None = 0,
    Transpose = 1,
    ConjTranspose = 2,
};

} // namespace crd::hesap::dense
