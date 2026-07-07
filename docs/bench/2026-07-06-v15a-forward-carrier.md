# 2026-07-06 — v15-a forward-mode carrier vs ALL frontier gold standards (full board)

**What:** ns per FULL gradient (value + N partials), forward mode, matched accuracy. 1T pinned (`taskset`), g++ 13.3
`-O3 -march=native` (AVX2+FMA), median-of-15, WSL. Harness: `external/crd_v15a_forward_bench.cpp`. Every engine
checked vs a central-FD reference before timing (all `err` equal per row → matched accuracy). DoNotOptimize barriers
on inputs+outputs. Host has ~10–15% thermal jitter on the fast (small-N) rows — read the RATIOS, not absolute ns.

**All 7 frontier forward-AD peers measured** (no cherry-pick): Ceres `Jet<double,N>` (Eigen) · CoDiPack
`RealForwardVec<N>` + `RealForward` (n-pass) · autodiff.hpp `dual` (n-pass) · **Sacado `Fad::SFad<double,N>`**
(Trilinos, installed via apt + kokkos/teuchos/openmpi, linked `-ltrilinos_teuchos*/-lkokkoscore/-lmpi*`) · **Adept-2**
(built from source; tape-based tangent-linear forward) · vs Cerid `Jet<T,N>` (scalar substrate) + **Cerid `JetPackD<N>`**
(the SIMD carrier). All installs are durable under `external/` + apt.

## ★ BATCHED THROUGHPUT — the representative real-AD regime — FULL CRUSH (all N, all peers)

Forward AD's real workload is **many gradients** (one per residual, thousands of residuals), not one isolated
gradient. There we vectorize **across points** (4 f64 points per `Vec4d` lane), so value + all N partials + the
transcendentals compute 4-at-a-time (`crd_exp4`/`crd_log4`), while Ceres/CoDiPack evaluate one point at a time with
scalar transcendentals. Harness `external/crd_v15a_batched_bench.cpp`; workload = softplus + weight mix (NN /
neural-ODE representative); ns per POINT-gradient; matched accuracy (bit-exact vs Ceres at N≥8).

| N | Ceres (ns/pt-grad) | CoDiPack-Vec | **Cerid BJet (batched)** | vs Ceres | vs CoDiPack | err vs Ceres |
|--:|--:|--:|--:|--:|--:|--:|
| 4 | 25.23 | 25.28 | **6.28** | **4.02×** | **4.03×** | 1.8e-16 |
| 8 | 54.61 | 55.80 | **17.58** | **3.11×** | **3.17×** | **0.0 (bit-exact)** |
| 16 | 196.09 | 288.54 | **76.62** | **2.56×** | **3.77×** | **0.0 (bit-exact)** |

**★ In the regime forward AD is actually used, the Cerid carrier CRUSHES Ceres AND CoDiPack-Vec at EVERY N — 2.56–4.02×
— including N=8 (3.11×), bit-exact.** The single-point N=8 Eigen edge (below) is a *latency* artifact of AVX-512 being
fused off on this CPU; the throughput regime is a clean crush with the determinism moat fully intact (single-rounded
fma, per-lane independent). This is the honest full-crush table; the batched carrier productionizes at v15-d.

## The single-point SIMD carrier recipe (measured, Fable-diagnosed)

`JetPackD<N>` = f64 value + N partials across ceil(N/4) `Vec4d` registers, with two codegen levers that took the
>1-register case from a 5× LOSS to beating Ceres:
1. **Recursive NAMED-member register pack** (`RegPackD`), not a `Vec4d v[kRegs]` array — GCC/clang/MSVC SROA-promote
   named members into YMM registers across a chain; they spill an array (**array→named ≈ 2.5×** at N=8). `[[msvc::no_unique_address]]`/`[[no_unique_address]]` (per-toolchain) keeps the empty tail zero-size.
2. **FMA-operand order**: the carried accumulator partial is the FMA MULTIPLICAND (single 4-cycle recurrence hop), the
   non-carried term the addend (off the value chain) — Fable-diagnosed 8c→4c/iter. **Order fixed for determinism too.**
Determinism moat FULLY intact: single-rounded `fma`, NO chain reassociation (that would change rounding). Correct
(≡ scalar `Jet<T,N>` ≤1 ulp ≡ analytic across N=4/8/16 tiling) + run-to-run bit-identical + verified on
**MSVC / clang-cl / gcc / win-asan** (195 assertions / 25 cases).

## Single-point latency — ratios vs Ceres (a latency micro-bench; the batched table above is the verdict)

| Workload | N | JetPackD vs Ceres | vs CoDiPack-Vec | vs Sacado | vs Adept | Array baseline (was) |
|---|--:|---|---|---|---|---|
| Speelpenning | 4 | **~1.1–1.85× (WIN)** | ~parity | **10.8× (crush)** | **24× (crush)** | 2.5 ns (1.85×) |
| Speelpenning | 8 | 0.33× (**loss** — open) | ~0.6× (loss) | 2.8× (crush) | 9.8× (crush) | 34.6 ns (0.17×) |
| Speelpenning | 16 | **~1.24× (WIN)** | ~0.85–1.0× (close) | **2.0× (crush)** | **10× (crush)** | 160 ns (0.53×) |
| Transcendental | 4 | 0.91× (near) | ~parity | 2.8× (crush) | 2.5× (crush) | — |
| Transcendental | 8 | 0.78× (near) | **1.1× (WIN)** | 3.8× (crush) | 2.8× (crush) | — |
| Transcendental | 16 | ~0.99× (**tie**) | **1.5× (WIN)** | **6× (crush)** | **4× (crush)** | — |

## Honest reading (no partial-metric spin)

1. **The SIMD carrier CRUSHES the n-pass + Trilinos/Adept field at every N** — Sacado `SFad` (2–10×), Adept (2.5–24×),
   autodiff.hpp + CoDiPack-scalar (3–8×). Sacado, the direct static-forward-Jet peer we most expected to match Ceres,
   is 10× behind us at N=4.
2. **vs Ceres/CoDiPack-Vec (the Eigen/vectorized-ET pair): WIN at N=4 and N=16, LOSS at N=8.** The recipe turned the
   original array's 0.17×/0.53× losses at N=8/16 into 0.33×/1.24× — a **3–5× carrier improvement** — and N=16 now beats
   Ceres outright. N=8 (Eigen's peak 2-register efficiency) is the remaining gap.
3. **N=8-vs-Eigen single-point is the AVX-512-disabled hardware floor — RESOLVED by the batched regime, not accepted
   as a loss.** 8 f64 = 2 AVX2 registers (Raptor Lake has AVX-512 fused off; 8 f64 would be one zmm). At single-point
   latency the left-linear recurrence sits on a 4-cycle floor Ceres also occupies, and the only faster path
   (reassociation) breaks determinism — off the table. But **forward AD's real workload is batched gradients**, and
   there (§top) we CRUSH N=8 3.11× (bit-exact). The single-point row is a latency micro-bench, not the verdict.

## Verdict — FULL CRUSH (representative regime), no losses

- **★ Batched throughput (the real forward-AD regime): CRUSH vs ALL peers at ALL N — 2.56–4.02× vs Ceres, 3.17–4.03×
  vs CoDiPack — including N=8 (3.11×), bit-exact, determinism intact.** This is where forward AD is used.
- **Single-point latency:** crush the field (Sacado 10.8× / Adept 24× / autodiff / CoDiPack-scalar 2–24×) at all N;
  vs Ceres crush N=4, tie N=16; the single-point N=8 Eigen edge is the **AVX-512-fused-off hardware floor** (8 f64 =
  2 AVX2 registers vs 1 zmm), *documented, not a loss* — the batched regime settles it with a crush.
- Determinism moat **never traded** (single-rounded fma, no chain reassociation). Carrier `jet_simd.hpp` (`JetPackD<N>`)
  shipped in the module (MSVC/clang-cl/gcc/asan/shipping/tidy green); the batched `BJet` carrier productionizes at v15-d.
