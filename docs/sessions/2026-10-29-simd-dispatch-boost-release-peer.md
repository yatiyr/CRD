# 2026-10-29 — SIMD-DISPATCH: the Boost release pin and the odeint peer

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [the Boost.Math peer](2026-10-29-simd-dispatch-boost-math-peer.md) (its Left: odeint, the interpolation
board, the peer-header check).

## CI first

Scheduled run 40849112570 (fea7ee0a) is still running (win-asan, linux-gcc-asan, TSan, linux-gcc-arm64-asan); nothing
was cancelled. Its one newly completed failure, linux-gcc-public-checks, is the refs manifest pair
(`crd-repository-tools`, `crd-generated-sources` on `transcendental_refs.inc`) that 74a95125 fixed. Push runs
40869334591 and 40875025034 have no failed job yet. DIAG.11c and DIAG.12 stay Needs CI (the final complete-tier run).

## What landed

- **One Boost pin.** The user approved the full Boost 1.92.0 release and one pin. `boost` replaces `boost_math` in
  cmake/pins.json: the release archive from archives.boost.io, SHA-256 5c1d40cb…193cbc6c, 199,030,664 bytes, BSL-1.0
  (LICENSE_1_0.txt read from the archive), provenance the release's `.json` manifest. That manifest names commit
  afdfa325…; the archive is not commit-addressed, and check-pins requires a `commit` field to appear in the URL, so
  the commit stays in the note rather than a field. The approved file was verified (`pins.py fetch`) and seeded into
  the local archive directory in place of the standalone archive; the configure used it, nothing was downloaded.
  The license manifest is regenerated. The complete tier's cold lanes now download this archive (199 MB) to verify
  it, as they do every unselected pin.
- **`boost_headers`** is the release root, header-only, with `BOOST_ALL_NO_LIB`; `BOOST_MATH_STANDALONE` is gone, so
  Boost.Math is configured as the release configures it, and `isa()` reads the version from `<boost/version.hpp>`.
- **The odeint arm.** `boost_peer_odeint.cpp` exports one `dopri5(VanDerPol, Tolerances)`: odeint's controlled
  `runge_kutta_dopri5` over the whole interval with the system compiled in the peer, returning the state, the
  right-hand-side calls and the steps. bench_ode_vs_refs calls it once per timed run and no longer includes Boost or
  a standard container; it leaves REPO.3's unrouted list for a route built when both `CRD_BUILD_HESAP_VS_SUNDIALS`
  and `CRD_BUILD_HESAP_VS_BOOST` hold, links CVODE's KLU solver and SuiteSparse's KLU (which its large-n MOL rows
  always needed), and gets `scripts/run_bench_ode_refs.sh`. Its rows keep their agreement measure: each side's error
  against the tight-tolerance reference.

## Checks

- win-debug with the option on: the peer (odeint included) at /W4 /WX and the four Boost benches; each bench ran once
  small with every row inside its gate. The tree is back to the option off.
- WSL GCC 13, objects only, the tree's warning set with -Werror at -O2: the three peer units at -march=native and
  bench_ode_vs_refs at the baseline (SUNDIALS is on WSL only). A probe linked against the odeint object integrated
  Van der Pol mu = 1 to t = 100 at 1e-8 (12,973 calls, 1,771 steps).
- The ctest guards (source routes with the option on and off), the tooling tests, the validators. No boards.
