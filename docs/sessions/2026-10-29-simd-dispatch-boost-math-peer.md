# 2026-10-29 — SIMD-DISPATCH: the pinned Boost.Math peer of the special-function boards

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [pow within an ulp](2026-10-29-simd-dispatch-pow-accuracy.md) and
[the pocketfft peer](2026-10-29-simd-dispatch-pocketfft-peer.md) (its Left: the Boost peers).

## CI first

Scheduled run 40849112570 (fea7ee0a) is still running (win-asan, the Linux GCC asan and public-checks lanes, TSan,
linux-gcc-arm64-asan); nothing was cancelled. Its newly completed failures were read from their logs: every one is the
refs manifest pair fixed by 74a95125 (`check-generated.py` passes at HEAD), plus, on the GCC release lanes, the
specimen and probe failures fixed by a721d147 and d334c12f. linux-clang-arm64-asan failed only that pair (2472 of
2474). The push runs since were replaced by newer pushes. DIAG.11c and DIAG.12 stay Needs CI (the final complete-tier
run); no run was dispatched while the scheduled one runs.

## Order

The row's Left lists the GEMM board's candidates (a portable 4 by 4 tile, the serial threshold) before the peers.
This batch took the Boost peer first because the user approved and handed over its archive today; GEMM's code is next.

## What landed

- **Pin.** `boost_math` in cmake/pins.json: boostorg/math `boost-1.92.0` at commit f12be767, the commit-addressed
  archive, SHA-256 d58ee0ad…4818, BSL-1.0 (read from the archive), gated by `CRD_BUILD_HESAP_VS_BOOST`. The file the
  user approved was verified against the pin (`pins.py fetch --archive`) and seeded into the tree's
  `CRD_INPUT_ARCHIVES`; the configure used it ("using local archive"), nothing was downloaded. The complete tier's
  cold lanes fetch and verify every unselected pin, this one included. The license manifest is regenerated.
- **`crd-bench-boost-peer`** (`crd_add_bench_peer`, `examples/peers/boost_peer*.{hpp,cpp}`): the only unit that
  includes Boost.Math, standalone (`BOOST_MATH_STANDALONE` on the interface target), at the host's ISA (/arch:AVX2
  here). One exported `eval(Special, Arguments, out, n)` per array, the switch outside a templated loop; `isa()` names
  the version and the significand Boost's default `promote_double` policy evaluates a double with (53 on MSVC, 64 with
  GCC and Clang on x64, binary128 in software on AArch64 Linux).
- **The three boards** (bench_special_vs_refs, bench_bessel_vs_refs, bench_special_v12d) call it and build with the
  tree's warnings as errors at the baseline. Both sides now write the same arrays (the Cerid loops summed before), and
  each row compares them (`special_peer_rows.hpp`): a value outside the function's reference-test gate prints
  `!MISMATCH!` and the bench exits 1. The elliptic rows hand Cerid the parameter m and Boost the modulus √m, both
  prepared outside the timed passes (the old loop took the square root inside Boost's). Each bench takes a size or
  pass count for a smoke run. They leave REPO.3's unrouted list for a `built_when` route;
  `scripts/run_bench_special.sh` builds and runs them at every level.
- A tidy finding in the Lanczos microbench (local constant `kM`) is fixed.

## Checks

- win-debug with the option on: the peer and the three benches at /W4 /WX; each ran once small: every row inside its
  bound (largest relative differences 4.4e-16 to 3.6e-13 where the value is not near a zero; lgamma's and digamma's
  1.5e-10 and 4.0e-10 sit at their zeros, inside the absolute gate). The tree is back to the option off.
- WSL GCC 13, objects only, the tree's warning set with -Werror at -O2: the peer at -march=native, the benches at the
  baseline. Hosted CI does not build these programs (local only, as the other peers).
- tidy on every changed file, clang-format on the added lines, the ctest guards (source routes with the option on
  and off), the tooling tests, the validators. No boards, following the fast-row order.

## Left

- The distribution and interpolation boards (bench_dist_boost, bench_interp_boost) through the same peer.
- bench_ode_vs_refs's odeint arm: Boost.Odeint is not part of Boost.Math and is not standalone (Config, Range,
  Fusion, MPL), so it is not in the approved archive. Its peer needs either another approved Boost archive or the host's
  headers; that is a user decision.
- The check that no bench unit includes a peer header library, once the three are through peers.
