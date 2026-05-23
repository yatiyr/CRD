#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b-1b-perf — shared compact-WY block-reflector helpers.
//
// The compact-WY representation aggregates `nb` elementary Householder
// reflectors H_j = I - tau_j v_j v_j^T (columnwise, unit-diagonal V) into a
// single block reflector  H = I - V T V^T,  where V is the (rows x nb)
// matrix of unit-diagonal reflector columns and T is an nb x nb upper-
// triangular factor (LAPACK `dlarft`). Applying H to a trailing block then
// becomes three GEMMs (`dlarfb`) — the BLAS-3 lever shared by blocked QR
// (`qr.cpp`) and the bidiagonal vector formation (`orgbr.hpp`, dorgbr).
//
// These are the orientation-independent pieces. The columnwise V
// materialization (the only storage-specific helper QR needs) lives here
// too; the rowwise/offset variants dorgbr-P needs stay local to orgbr.hpp.
//
// Lower layer: raw f32/f64 (ADR-0078). Real T only.
// -----------------------------------------------------------------------

// Build the compact-WY T matrix (nb x nb upper-triangular) from a
// pre-computed vtv = V^T * V (row-major leading dim `vtv_ld`) and the block's
// tau values (`taus_arr[k .. k+nb-1]`). For each column j:
//   T[0:j, j]  = -tau_j * vtv[0:j, j]
//   T[0:j, j] := T[0:j, 0:j] * T[0:j, j]   (in place)
//   T[j, j]    =  tau_j
// Faithful to LAPACK `dlarft` (forward, columnwise).
template <typename T>
inline void build_block_t_from_vtv(const T* vtv, crd::usize vtv_ld, const T* taus_arr, crd::usize k,
                                   crd::usize nb, T* t_block, crd::usize t_ld) noexcept
{
    for (crd::usize j = 0; j < nb; ++j)
    {
        const T tau_j = taus_arr[k + j];
        for (crd::usize i = 0; i < j; ++i)
        {
            t_block[i * t_ld + j] = -tau_j * vtv[i * vtv_ld + j];
        }
        t_block[j * t_ld + j] = tau_j;
        // Zero the strict-lower triangle: dlarft's T is upper-triangular, but a
        // reused/partial-stride buffer may carry garbage there that a full-block
        // T*W (or T^T*W) GEMM would read. Keep T clean regardless of caller reuse.
        for (crd::usize i = j + 1; i < nb; ++i)
        {
            t_block[i * t_ld + j] = T{0};
        }
        // Triangle-multiply T[0:j, j] = T[0:j, 0:j] * T[0:j, j] in place.
        for (crd::usize ii = 0; ii < j; ++ii)
        {
            T s = T{0};
            for (crd::usize kk = ii; kk < j; ++kk)
            {
                s += t_block[ii * t_ld + kk] * t_block[kk * t_ld + j];
            }
            t_block[ii * t_ld + j] = s;
        }
    }
}

} // namespace crd::hesap::dense::detail
