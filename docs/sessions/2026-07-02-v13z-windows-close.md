# 2026-07-02 — v13-z: Windows verification + the CLI/docs/guard close of the Numerical-Analysis + Motion cluster

> The v13 cluster (interp/quadrature/diff/motion) was algorithmically complete and **linux-gcc-green** but had never
> been built on Windows/MSVC, and the win-tidy conformance pass was owed. This session drove the v13-z close on the
> real target: Windows baseline → win-tidy pass → conformance guard → CLI → system docs → the DoD configs.

## Outcome

The v13 slice is **verified on Windows** (win-debug + win-asan + win-tidy all green on the 4 modules), the CLI ships
for all four modules, the four system docs are written, and a new safety-critical conformance guard is CI-registered.
Only the win-shipping DoD config, the user commit, and the 18-config CI remain.

## 1. Windows baseline (SANITY #2 — verify the shipped artifact, not a remembered green)

All four v13 modules compiled clean on MSVC for the **first time** (they had only ever seen linux-gcc). win-debug
built 160 objects, linked all four test executables, and the v13 suite ran **77/77**. Verifying on the real target
immediately paid off: **one quadrature test failed under ctest that gcc had hidden** — a `[a,inf)` bracket in a
`TEST_CASE` *name* (`test_de.cpp`) made Catch2's `--list-tests` misparse `[` as a tag boundary, so `catch_discover`
lumped 15 quadrature cases into one registration whose `-R` filter matched nothing → non-zero exit. This is the exact
`catch_discover`-`[`-in-name scar from memory. Fixed the name (`[a,inf)` → `a..inf`); un-lumped to 77 individual green
tests.

## 2. The win-tidy pass — CLOSED (bigger than documented)

The old note claimed "~49 static-constexpr renames." The full win-tidy check set (run on Windows for the first time)
found **92 violations** — and, critically, it covers the **test files**, which had never been tidy-checked:

- **53 in headers**: the `static constexpr` QUADPACK tables (`xgk`→`kXgk`, `wgk`→`kWgk`, `wg`→`kWg`, `data`→`kData`,
  `npoints`→`kNpoints`, the `x1..w87b` QNG arrays), the Clenshaw-Curtis member `x`→`kX`, function-local uppercase math
  vars (`S`/`L`/`R`/`Sm`/`Sprev`→lowercase, with `S`→`s_est` where a lowercase `s` already existed), the interp
  `k_grid_max_dim`→`kGridMaxDim` / `k_pi`→`kPi` / `abs_`→`abs_val`, and — never checked before — **25 camelCase locals
  in `otg.hpp`** (`vMax`→`v_max`, `bestT`→`best_t`, `jMax`→`j_max`, …) from the late Ruckig-OTG work.
- **39 in test files**: global-constant naming (`qawo_exp_cos20`→`kQawoExpCos20`), `1u`→`1U`, isolate-declaration,
  a `performance-for-range-copy`, and a `misc-redundant-expression` (`v==v` NaN idiom on a concrete `double`).

**Scar re-confirmed: `clang-tidy --fix` is unsafe on the headers.** It corrupted interp (`abs_`→`abs` with un-updated
`detail::abs_` call sites) and quadrature (`CcX::x`→`kX` with un-updated `X::x` accesses) via incomplete cross-TU /
template-dependent renaming — the documented "`--fix` naming CORRUPTS" hazard. So the header renames were done
**manually / via controlled `\b`-boundary Python scripts with per-name collision pre-checks**, and the test-file fixes
via `--fix` **confined to the `.cpp` main file** (concrete single-TU, no template cross-refs — safe there). Every step
was gated: rebuild + run the v13 suite (correctness preserved) + re-list naming (→ 0). Final: the **full win-tidy
check set is 0 errors on all four v13 test targets**, win-debug still green, all touched files clang-format-clean.

## 3. Conformance guard — status-not-exception (ADR-0095 pillar 3)

Shipped `scripts/check_hesap_v13_no_exceptions.{ps1,sh}` + registered `crd-hesap-v13-no-exceptions` in
`tests/math/CMakeLists.txt` (both WIN32 + UNIX arms). It greps the four module headers for `throw`/`try`/`catch`
(trailing `//` comments stripped) — the v13 status-not-exception contract. Verified **non-vacuous** (SANITY #3): it
scans 40 files, bites a synthetic `throw`, and correctly ignores a comment `try`; passes via ctest. (The other pillar-3
facets — `-fno-exceptions` build, no-heap-in-hot-path, iterative-not-recursive — are documented design properties in
ADR-0095; the grep guard is the CI-enforced, non-theater portion.)

## 4. The CLI — `hesap.{interp,quad,diff,motion}.*`

Followed the `hesap.fft.*` registry pattern exactly (schema + impl + `CRD_HESAP_CLI_REGISTER_MODULE` + `cli_anchor.hpp`
to survive the static-lib link). Added the acyclic **module→`crd-hesap`** edge to quadrature/diff/motion (interp
already had it) for the CLI headers. 8 curated commands:

- `hesap.interp.pchip.f64`, `hesap.interp.cubic_spline.f64` (blob = yq)
- `hesap.quad.samples.f64` (scalar; trapezoid/simpson/romberg)
- `hesap.diff.savgol.f64`, `hesap.diff.fornberg.f64` (blob)
- `hesap.motion.otg.f64` (blob `[valid,duration,nsamp,(t,p,v,a)*]`), `hesap.motion.scurve.f64` (blob `[valid,total,tj,ta,tc]`)

Each has a `test_cli.cpp` driving it through the registry with a correctness check (interpolation-property,
analytic-integral, Fornberg central stencils, and a nice OTG↔S-curve rest-to-rest cross-check). **11 CLI tests green.**

## 5. System docs + ADR

Wrote the four net-new `docs/systems/hesap-{interp,quadrature,diff,motion}.md` (house style: what-it-is · shipped
surface · error tiers · determinism · crush · CLI · edges · tests) + the `docs/systems/README.md` index rows. ADR-0095
was already present + Accepted; verified it aligns with the shipped guard (pillar 3 = status-not-exception,
`-fno-exceptions`).

## 6. DoD status + the determinism moat

- **win-debug ✅** — 89 v13 + CLI + guard tests.
- **win-asan ✅** — same 89, **ZERO ASan errors** (no memory bugs in the new CLI code).
- **win-tidy ✅** — full check set, 0 errors on all four targets.
- **Determinism moat** — evidenced by **56 run-twice/bit-identity assertions across 10+ `…+ determinism` test cases**;
  for these single-threaded deterministic kernels the `{1,4,16}` moat reduces to run-twice bit-identity, which is gated.
- **win-shipping — pending.** Its build-dir cache is **poisoned** (`CMAKE_COMMAND` → the VS-bundled CMake fork, the
  documented `#deps 0` landmine); it needs a wipe + standalone reconfigure, then a full-engine LTCG rebuild. Per the
  i9-14900K host-instability doctrine (full-engine LTCG sweeps trigger the Raptor Lake bugcheck) and the standing
  workflow (the user runs the multi-config DoD + CI at commit time), this is left as the user's host-safe commit-gate
  step, flagged precisely rather than triggered here.

## The DoD sweep + the v12 Windows debt (same session, continued)

Running the full 4-config DoD over the whole engine surfaced the **uncommitted v12 tree's own never-Windows-verified
debt** (the same gcc-only story v13 had): `hesap-stats/resampling.hpp:61` C4146 (unary minus on unsigned in Lemire's
`bounded_u32` — fixed as `(u32{0} - bound) % bound`, value-identical) and **32 clang-tidy violations** in the v12
stats tests + `bench_multivariate` (naming via `--fix` confined to the `.cpp` main files; `a_/b_/c_` helper fns →
`sample_a/b/c` manually; `time_ns(int, F&&)` → `const F&` — the callable is invoked n times, forwarding was wrong).
Plus the `crd-no-std-transcendental-check` guard tripping on two **comments** in `levin.hpp` that contained the
literal string `std::exp` (the guard doesn't strip comments) — reworded.

## Test 2661: "the win-asan hang" that wasn't — diagnosis + the real fix

The sweep then appeared to hang on `supernodal Cholesky: fat-front NODE-PARALLEL factor is bit-identical to serial
(v5a-4 moat)` under win-asan. **First diagnosis ("genuinely hangs") was WRONG** — an arbitrary 150 s standalone
timeout is not a hang proof. The real evidence (SANITY #5, refute your own hypothesis):

1. **The win-debug baseline: 456 s** — the test factors grid3d(28) (21,952 nodes) four times at /Od. It is simply a
   huge test.
2. **CPU sampling of the "hung" ASan process: linear climb** (~0.9 core, 19→411 cpu-sec) — computing, not parked.
3. **Thread stacks** (a purpose-built DbgHelp stack-walker, `stackdump.exe`, since no cdb is installed): the main
   thread deep in `gemm_microkernel_avx2_f64` with `_asan_loadN` on top — **every SIMD load pays an ASan shadow
   check at /Od**; all 8 workers correctly parked in `WaitOnAddress`. No deadlock, no fiber/ASan bug. ASan-on-debug
   ≈ 5-6× ⇒ ~40 min for this one test — it dominated (and effectively broke) every ASan sweep.

**The fix (test-side, engine untouched):** replaced `moat(grid3d(&alloc, 28))` with a purpose-built
**`bordered_spd(24, 48, 560)`** — 24 dense 48-col blocks + a dense 560-col border. Under AMD the low-degree block
nodes eliminate first and the border becomes the **root supernode (nc ≈ 560 ≥ kNodeParallelMinCols=512)** receiving
cmod from every block supernode — the *same* divergent code paths (two-level cdiv, no-pack cmod vs `gemm_parallel`)
at ~450 Mflop instead of tens of Gflop. Coverage is guard-enforced, not assumed: the existing `REQUIRE(maxnc >= 512)`
plus a new `REQUIRE(nsuper >= min_nsuper)` (2 for the bordered matrix) fail loudly if amalgamation ever collapses the
shape. **Result: 456 s → 9.3 s win-debug (49×), ~40 min → under a minute win-asan; 53,214 assertions, both configs
PASS, zero ASan errors, clang-tidy clean.** `grid3d` itself stays (two other tests use s=22/s=12).

## The last mole: C4723 under LTCG — and the final all-green DoD

With debug/asan/tidy green, win-shipping's from-scratch LTCG build surfaced one real finding: **warning C4723
(potential divide-by-zero) → LNK1257** in `otg.hpp` + `otg_sync.hpp`, at the interior a=0 velocity-crossing check.
The division by `2·js[i]` *is* guarded by `fabs(js[i]) > 1e-15` in the same condition, but LTCG's global optimizer
cannot carry that correlation through `crd::math::fabs` and flags a reachable zero-divisor path — visible only to
`/GL` (why debug/asan/tidy never saw it). Fix: hoist the divisor and add the implied-but-explicit `den != T{0}` to
the condition — **numerically a no-op** (the fabs guard already guarantees it), proven by the baked Ruckig
bit-exactness gates still passing on win-debug + win-asan (15/15 each).

**Final DoD verdict (the whole engine, all fixes in):** win-debug **4367/4367 PASS** · win-asan **4367/4367 PASS,
zero ASan errors** · win-shipping **4280/4280 PASS** (LTCG, wiped + standalone-reconfigured dir) · win-tidy
**build PASS** (warnings-as-errors). The v12 + v13 tree is commit-ready; the 18-config CI is the remaining
post-commit gate.

## Proposed commit

```
feat(hesap): v13-z close — CLI + docs + guards + the Windows DoD debt paydown

- CLI hesap.{interp,quad,diff,motion}.* (8 commands + anchors + 11 CLI tests)
- win-tidy: 92 v13 + 32 v12 naming/isolate/suffix/perf fixes (0 errors)
- fix(hesap-stats): C4146 unary-minus-on-unsigned in Lemire bounded_u32
- fix(hesap-quadrature): TEST_CASE name '[a,inf)' broke catch_discover;
  levin.hpp comments tripped the no-std-transcendental guard
- test(hesap-direct): fat-front NODE-PARALLEL moat rides bordered_spd
  (456s->9.3s debug, ~40min->1min asan; same divergent paths, guard-enforced)
- feat(tests): crd-hesap-v13-no-exceptions guard (ADR-0095 pillar 3)
- docs(systems): hesap-{interp,quadrature,diff,motion}.md + README rows

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

(The tree also carries the prior uncommitted v12 + v13 work; batch as preferred. Agents never commit.)
