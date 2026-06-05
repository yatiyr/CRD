#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdlib>

namespace crd::hesap::direct
{
// =======================================================================
// v5b-3b PREREQUISITE — shared dense-LU kernel helpers.
//
// These were file-local to the anon namespace of the PROVEN supernodal_lu.cpp
// (the v5a/v5b-2 determinism moat). The multifrontal LU front factorization
// (factor_front, below) needs the SAME deterministic GESP/static-pivot helpers
// and the unit-lower TRSM, so they live here, consumed by BOTH supernodal_lu.cpp
// and multifrontal_lu.cpp (DRY + the single-path quality bar). Moving them is a
// bit-identical refactor: the bodies are verbatim — the supernodal numeric (and
// thus the {1,2,4,8}-worker bit-identity moat) is unchanged.
//
// Element (row r, col c) of a ColMajor block d with leading dim `ld` = d[c*ld + r].
// =======================================================================

// |x| for the static-pivot threshold test (real path = abs; complex = modulus).
template <typename T> [[nodiscard]] inline dense::RealType<T> lu2_mag(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return crd::hesap::abs(x);
    }
    else
    {
        return x < T(0) ? -x : x;
    }
}

// Unit (multiplicative identity) for the L diagonal.
template <typename T> [[nodiscard]] inline T lu2_one() noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return T{dense::RealType<T>(1), dense::RealType<T>(0)};
    }
    else
    {
        return T(1);
    }
}

// Construct a value of type T from a real magnitude (real path = the scalar; complex = {m, 0}).
template <typename T> [[nodiscard]] inline T lu2_from_real(dense::RealType<T> m) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return T{m, dense::RealType<T>(0)};
    }
    else
    {
        return m;
    }
}

// X = L11⁻¹ · X (forward substitution), L11 unit-lower nc×nc (ColMajor, ldL), X is nc×ncol
// (ColMajor, ldX). The within-block sequential dependency of the blocked LU's U block-row.
template <typename T>
void trsm_unit_lower_left(const T* l, crd::u32 ldl, crd::u32 nc, T* x, crd::u32 ldx, crd::u32 ncol) noexcept
{
    for (crd::u32 cc = 0; cc < ncol; ++cc)
    {
        T* xc = x + static_cast<crd::usize>(cc) * ldx;
        for (crd::u32 i = 0; i < nc; ++i)
        {
            const T xi = xc[i];
            for (crd::u32 r = i + 1; r < nc; ++r) // L unit diagonal ⇒ no divide
            {
                xc[r] = xc[r] - l[static_cast<crd::usize>(i) * ldl + r] * xi;
            }
        }
    }
}

// getenv wrapper for the dev-override knobs (`CRD_MF_PANEL` / `CRD_MF_FRONTPAR_K` / `CRD_MF_FORCE_MC64` /
// `CRD_MF_NO_MC64`). C4996-safe on MSVC: these are fixed, documented dev knobs — not a security surface (no
// user-controlled paths), so the deprecated-getenv warning is suppressed at this single site.
[[nodiscard]] inline const char* mf_getenv(const char* name) noexcept
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv — dev override knobs only
#endif
    return std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

// Blocked-LU panel width nb. Env-tunable (CRD_MF_PANEL) for the node-level sweep; a FIXED value is
// bit-identical across worker counts (both factor_front branches use the same nb; gemm_parallel_auto is
// bit-exact) ⇒ the {1,2,4,8} moat holds. Wider nb ⇒ the trailing rank-nb GEMM becomes compute-bound (better
// throughput + parallel scaling) at the cost of more serial panel + TRSM work (panel hidden by lookahead;
// TRSM parallelized below).
[[nodiscard]] inline crd::u32 mf_panel_width() noexcept
{
    static const crd::u32 w = []() noexcept -> crd::u32
    {
        if (const char* e = mf_getenv("CRD_MF_PANEL"))
        {
            const int v = std::atoi(e);
            if (v >= 8 && v <= 1024)
            {
                return static_cast<crd::u32>(v);
            }
        }
        return 128U; // measured best on the CFD targets: wider nb ⇒ compute-bound trailing GEMM (better scaling)
    }();
    return w;
}

// =======================================================================
// v5b-3b-2 — factor_front: blocked partial LU of an m×n COL-MAJOR front (leading dim `ld`).
//
// Factors the leading `npiv` pivot columns/rows with a static diagonal pivot (GESP √ε
// perturbation `tiny` ⇒ deterministic ⇒ moat-safe; MC64 made B I-matrix-like so the
// diagonal is a safe pivot) and leaves the Schur complement (the contribution block) in
// the trailing (m-npiv)×(n-npiv) block. THE crush kernel: the trailing update is the
// LAPACK right-looking blocked pattern (rank-`nb` TRSM + ONE big dense `dl::gemm`),
// exactly UMFPACK's umf_blas3_update on a dense front.
//
// A clean generalization of supernodal_lu.cpp's blocked `dense_lu_nopivot` (rows→m,
// stop at npiv, trailing=Schur instead of square nc×nc). On exit:
//   d[0:npiv , 0:npiv]  packed LU of the pivot block (unit-lower L11 + upper U11).
//   d[npiv:m , 0:npiv]  L21 (the L foot below the pivots).
//   d[0:npiv , npiv:n]  U12 (the U to the right of the pivots).
//   d[npiv:m , npiv:n]  S = the Schur complement = the contribution block.
// Deterministic per call ⇒ bit-identical across workers (the moat holds).
// `gemm_par` parallelizes the trailing-Schur GEMM (the dominant cost on big near-root fronts) via
// `gemm_parallel_auto` — BLIS row-slab split, BIT-EXACT vs serial `gemm` (ADR-0063) ⇒ the {1,2,4,8} moat
// holds. Set it ONLY when factor_front runs on the main thread (the multifrontal's narrow top levels), never
// inside a parallel_for over fronts (that would nest). Default false (serial GEMM via `scratch`).
// v5f-(a): `pivot_threshold > 0` enables THRESHOLD PARTIAL PIVOTING restricted to the fully-summed rows
// [k, npiv) — pick the largest-magnitude candidate (tie-break by lowest index ⇒ deterministic ⇒ moat-safe),
// swap the full front rows, and record the swap in `ipiv` (LAPACK getf2 convention: ipiv[k] = the row swapped
// into position k). NEVER delays: if even the max candidate is below `tiny`, the existing GESP perturbation
// takes over (GMRES-IR mops up the rare perturbed pivot). The structure is invariant under restricted
// pivoting (no-delay), so the caller only remaps index LABELS (driver step). `pivot_threshold == 0` (default)
// ⇒ the byte-unchanged static-diagonal path (SupernodalLU + the non-pivoting MultifrontalLU path).
template <typename T>
void factor_front(T* d, crd::u32 ld, crd::u32 m, crd::u32 n, crd::u32 npiv, dense::RealType<T> tiny,
                  crd::memory::IAllocator* scratch = nullptr, bool gemm_par = false,
                  dense::RealType<T> pivot_threshold = dense::RealType<T>(0), crd::u32* ipiv = nullptr) noexcept
{
    namespace dl = crd::hesap::dense;
    const crd::u32 panel = mf_panel_width();
    const bool     do_pivot = pivot_threshold > dense::RealType<T>(0);
    if (npiv == 0)
    {
        return;
    }

    // Factor panel columns [a,b) over the FULL row height [a,m): static pivot + GESP, scale L below the
    // diagonal, rank-1 trailing updates restricted to the panel columns. (The prior inline step 1.)
    auto factor_panel = [&](crd::u32 a, crd::u32 b) noexcept
    {
        for (crd::u32 k = a; k < b; ++k)
        {
            if (do_pivot) // threshold partial pivoting restricted to the fully-summed rows [k, npiv)
            {
                crd::u32 pmax = k;
                dense::RealType<T> vmax = lu2_mag<T>(d[static_cast<crd::usize>(k) * ld + k]);
                for (crd::u32 r = k + 1; r < npiv; ++r)
                {
                    const dense::RealType<T> vr = lu2_mag<T>(d[static_cast<crd::usize>(k) * ld + r]);
                    if (vr > vmax) // strict > ⇒ ties keep the LOWER index (deterministic ⇒ moat-safe)
                    {
                        vmax = vr;
                        pmax = r;
                    }
                }
                if (ipiv != nullptr)
                {
                    ipiv[k] = pmax;
                }
                if (pmax != k) // swap the FULL front rows k <-> pmax across all n columns (LAPACK getf2 + laswp)
                {
                    for (crd::u32 c = 0; c < n; ++c)
                    {
                        const T tmp = d[static_cast<crd::usize>(c) * ld + k];
                        d[static_cast<crd::usize>(c) * ld + k] = d[static_cast<crd::usize>(c) * ld + pmax];
                        d[static_cast<crd::usize>(c) * ld + pmax] = tmp;
                    }
                }
            }
            else if (ipiv != nullptr)
            {
                ipiv[k] = k;
            }
            T pivot = d[static_cast<crd::usize>(k) * ld + k];
            const dense::RealType<T> pm = lu2_mag<T>(pivot);
            if (pm < tiny)
            {
                pivot = (pm == dense::RealType<T>(0)) ? lu2_from_real<T>(tiny) : pivot * (tiny / pm);
                d[static_cast<crd::usize>(k) * ld + k] = pivot;
            }
            for (crd::u32 r = k + 1; r < m; ++r) // L(r,k) = d(r,k)/pivot
            {
                d[static_cast<crd::usize>(k) * ld + r] = d[static_cast<crd::usize>(k) * ld + r] / pivot;
            }
            for (crd::u32 c = k + 1; c < b; ++c) // rank-1 update of the remaining panel columns
            {
                const T ukc = d[static_cast<crd::usize>(c) * ld + k];
                for (crd::u32 r = k + 1; r < m; ++r)
                {
                    d[static_cast<crd::usize>(c) * ld + r] =
                        d[static_cast<crd::usize>(c) * ld + r] - d[static_cast<crd::usize>(k) * ld + r] * ukc;
                }
            }
        }
    };

    // Schur update of a COLUMN SLICE: C[j1:m, c0:c1] -= L[j1:m, j0:j1] · U[j0:j1, c0:c1]. Splitting the
    // trailing update by columns is bit-identical to one wide update (each output column is independent).
    auto schur = [&](crd::u32 j0, crd::u32 j1, crd::u32 c0, crd::u32 c1, bool par) noexcept
    {
        const crd::u32 nb = j1 - j0;
        const crd::u32 tr = m - j1;
        const crd::u32 tc = c1 - c0;
        if (tr == 0 || tc == 0)
        {
            return;
        }
        const dl::MatrixView<const T, dl::Layout::ColMajor> l21(&d[static_cast<crd::usize>(j0) * ld + j1], tr, nb, ld);
        const dl::MatrixView<const T, dl::Layout::ColMajor> u12(&d[static_cast<crd::usize>(c0) * ld + j0], nb, tc, ld);
        dl::MatrixView<T, dl::Layout::ColMajor> c22(&d[static_cast<crd::usize>(c0) * ld + j1], tr, tc, ld);
        if (par)
        {
            dl::gemm_parallel_auto<T, dl::Layout::ColMajor>(lu2_from_real<T>(dense::RealType<T>(-1)), l21, u12,
                                                           lu2_one<T>(), c22, dl::Trans::None, dl::Trans::None, nullptr);
        }
        else
        {
            dl::gemm<T, dl::Layout::ColMajor>(lu2_from_real<T>(dense::RealType<T>(-1)), l21, u12, lu2_one<T>(), c22,
                                              dl::Trans::None, dl::Trans::None, scratch);
        }
    };

    // X = L11⁻¹·X for the panel's U block-row [j0:j1, j1:n], split across workers by COLUMN (each trailing
    // column is independent ⇒ bit-identical vs the serial TRSM ⇒ the moat holds). Serial below threshold (the
    // fork/join exceeds a thin TRSM). Removes the ~20%-serial TRSM from the big-front critical path, and lets
    // the panel widen (wider nb grows TRSM work linearly) without the serial TRSM dominating.
    auto trsm_stage = [&](crd::u32 j0, crd::u32 j1) noexcept
    {
        const crd::u32 ncol = n - j1;
        if (ncol == 0)
        {
            return;
        }
        const crd::u32 nc = j1 - j0;
        T* const l11 = &d[static_cast<crd::usize>(j0) * ld + j0];
        T* const xb = &d[static_cast<crd::usize>(j1) * ld + j0];
        const crd::u32 nw = crd::jobs::num_workers();
        if (nw <= 1 || ncol < 256)
        {
            trsm_unit_lower_left<T>(l11, ld, nc, xb, ld, ncol);
            return;
        }
        crd::jobs::Counter* c = crd::jobs::parallel_for(
            ncol, nw,
            [l11, ld, nc, xb](crd::u32 b, crd::u32 e) noexcept
            { trsm_unit_lower_left<T>(l11, ld, nc, xb + static_cast<crd::usize>(b) * ld, ld, e - b); });
        crd::jobs::wait(c);
    };

    // Partial pivoting routes through the SERIAL within-front path: the parallel lookahead swaps full rows
    // (incl. trailing columns updated concurrently) ⇒ a data race. Cross-FRONT tree parallelism (where the
    // {1,2,4,8} moat lives) is unaffected — each front factors serially-deterministically, fronts in parallel.
    if (do_pivot || !gemm_par)
    {
        // Serial path: the simple right-looking blocked LU (bit-identical to the prior implementation).
        for (crd::u32 j0 = 0; j0 < npiv; j0 += panel)
        {
            const crd::u32 j1 = (j0 + panel < npiv) ? j0 + panel : npiv;
            factor_panel(j0, j1);
            if (n - j1 > 0)
            {
                trsm_unit_lower_left<T>(&d[static_cast<crd::usize>(j0) * ld + j0], ld, j1 - j0,
                                        &d[static_cast<crd::usize>(j1) * ld + j0], ld, n - j1);
            }
            schur(j0, j1, j1, n, false);
        }
        return;
    }

    // PARALLEL path, depth-1 LOOKAHEAD (the dense-LU fix for the fork-join/serial-panel bottleneck): overlap
    // the NEXT panel's factorization (one async worker) with the trailing update of the REST of the columns
    // (the big parallel GEMM). They touch DISJOINT column ranges ([j1:j2) vs [j2:n)) ⇒ no data race; the work
    // is identical to the serial path, only reordered ⇒ BIT-IDENTICAL (the {1,2,4,8} moat holds).
    factor_panel(0, (panel < npiv) ? panel : npiv); // pre-factor the first panel
    for (crd::u32 j0 = 0; j0 < npiv; j0 += panel)
    {
        const crd::u32 j1 = (j0 + panel < npiv) ? j0 + panel : npiv; // panel [j0,j1) is already factored
        if (n - j1 > 0)
        {
            trsm_stage(j0, j1); // parallel TRSM (independent trailing columns) — bit-identical to serial
        }
        if (m - j1 == 0 || n - j1 == 0)
        {
            continue;
        }
        if (j1 < npiv)
        {
            const crd::u32 j2 = (j1 + panel < npiv) ? j1 + panel : npiv;
            schur(j0, j1, j1, j2, false); // update the next panel's columns first (small) so it can be factored
            if (n - j2 > 0)
            {
                // factor next panel [j1,j2) async on one worker, WHILE the rest [j2:n) updates in parallel.
                crd::jobs::Counter* counter =
                    crd::jobs::run(crd::jobs::make_job([&, j1, j2]() noexcept { factor_panel(j1, j2); }));
                schur(j0, j1, j2, n, true);
                crd::jobs::wait(counter);
            }
            else
            {
                factor_panel(j1, j2); // no rest columns to overlap
            }
        }
        else
        {
            schur(j0, j1, j1, n, true); // last panel: update the full remaining trailing in parallel
        }
    }
}

} // namespace crd::hesap::direct
