# 2026-06-14 — FFT-vs-MKL deep profiling (the reframe: codelet ≥ MKL, the gap is overhead)

**Mandate (user, emphatic):** beat or match MKL on 1D FFT throughput — "it's all software, think outside the box, do not proceed until parity or crush." Took the step back and PROFILED instead of guessing.

## Tools
- `perf` unavailable under WSL2 (virtualized PMU). Used **`llvm-mca` (Windows LLVM 20.1.8, `-mcpu=alderlake`)** for static port-pressure + the generated **clang `-S` asm** of the isolated radix-8 codelet (`build/micro8.cpp` → `loop8.s`). Ground-truth, no hardware counters.

## Fresh baseline (the true ratio)
Cerid 1D complex f64 vs MKL: ~**0.33×** (NOT the 0.45× a stale note claimed — that was small-N best-case). MKL holds ~30–49 GFLOP/s flat L1→512K then degrades 1.85×; Cerid degrades 2.33× the whole way (24.5→8.5).

## THE FINDING — the codelet is not the bottleneck
- **Instruction mix of the radix-8 twiddle codelet** (clang, one unrolled k-iteration): **36 vmulpd + 36 vaddpd + 33 vsubpd = 105 FP vec ops, ZERO packed FMA, ZERO shuffles.** The `w.re*a − w.im*b` twiddle pattern is NOT contracted to FMA (clang won't without `-ffast-math`).
- **llvm-mca on the clean hot k-loop:** Block RThroughput **35.3 cycles**; resource pressure p1=45 / p5=42 / p0=25 (FP-port bound, imbalanced toward p1), loads p2/p3/p11 ≈ 16, stores ≈ 10.
- That hot loop does **4 radix-8 butterflies ≈ 480 reference-flops in ~35–45 cycles ≈ 50–69 GFLOPS in isolation — AT OR ABOVE MKL's 56.**

**⇒ The kernel is competitive. The whole-FFT 27 GFLOPS (0.33× MKL) means ~55% of the time is OVERHEAD around the codelet, not the codelet.** This reframes "MKL is an unbeatable Spiral-class kernel wall" → "we are wasting the cycles around a good kernel, which is fixable."

## The overhead (measured/derived), and the attack ladder
1. **Per-pass twiddle setup** — `idx = m*j*r` (7 integer `imul`/group) + 14 `vbroadcastsd`, recomputed per group; dominates small-`r` (late) passes. **Fix A: precompute per-pass twiddle tables in linear order** (no index math, sequential pre-broadcast loads) — MKL/FFTW's standard technique. Highest value.
2. **Interleave shuffles** in radix-4 first/last passes (the 730 vpermpd/vunpck in the whole-TU count are here, NOT in the radix-8 codelet). **Fix C: radix-8/16 first+last passes** ⇒ fewer passes + fewer shuffles.
3. **FMA fusion** of the codelet (`fnmadd`) lowers p1 from 105→~87 ops (~1.2× codelet floor). **Fix B.**
4. **Ping-pong memory** — Stockham 2-buffer reads+writes the whole array every pass (~4 L1 round-trips @N=1024; ~6 DRAM passes @2^20). **Fix D: register-blocked recursion** (fuse passes, keep data in registers) — the large-N lever, and the one to do RIGHT this time (block transpose = unit-stride, NOT the strided transpose-free recursion that the 14900K prefetcher punished 4×).

## Lever B LANDED — FMA fusion (first measured step)
Added `fnmadd(a,b,c)=c−a·b` to `Vec4d`/`Vec8f` (`_mm256_fnmadd_pd/ps`); the generator's `tw_rt`/`mulc` nodes now emit `fnmadd`/`fma` for the complex twiddle mul in the SIMD path (scalar tail keeps mul-add). Regenerated `codelets.hpp` (396 fnmadd). **Measured +3–11%** (262144 10.5→11.7, 524288 8.5→9.3, 1024 24.5→25.6 GFLOPS) — bigger at mid/large N. Gate green (53/11 [fft], suite 109/20; the single-rounding absorbed by the DFT tolerance; run-twice determinism holds — the moat is intact since it's the same code across thread counts). Confirms the profiling: the codelet was already good, so FMA is a small (but free) win; the BIG levers are the overhead (A/C/D).

## Lever A LANDED — precomputed per-pass twiddle tables
The combine radix is fixed by the size, so the ctor replays the size-aware planner and precomputes the twiddles in the exact linear (j,m) order the passes consume (`build_combine_twiddles` → `m_ptw_re/im`); `execute` + the four combine passes (radix-4/8/16/32) walk it with a running pointer — **killing the per-group `m*j*r` imul + the strided `m_tw` gather**, now a sequential load. Per-phase profiling (rdtsc, `CRD_FFT_PROFILE`) drove this: combine passes were **58%/74%/83%** of the time at N=1024/65536/2^20. **Measured +2–23%** (8M 5.5→6.8, 262144 11.7→12.75, 65536 14.3→15.6 GFLOPS) — biggest at large N (the small-r late passes had the most setup). Gate 53/11 + ASan clean + suite 109/20.

## Cumulative this session (FMA + Lever A): ~0.33× → ~0.40–0.45× MKL
1024 24.5→26.2 · 65536 ~14→15.6 · 262144 10.5→12.75 · 1M 7.6→8.2 · 8M 5.0→6.8 GFLOPS. Real, gate-green, moat-intact progress. NOT parity yet.

## Lever D — grounded in Van Loan (`docs/books/Charles Van Loan - Computational Frameworks...pdf`)
The user supplied the book; poppler installed (`pdftotext`) to read it. **Thm 2.1.3 (radix-p splitting):** `F_n = (F_p ⊗ I_m)·diag(twiddles)·(I_p ⊗ F_m)·Π_{p,n}` for n=pm — the four-step. Ch.1 §1.7 = the **transposed Stockham** (unit-stride autosort); Ch.3 = "The Multiple DFT Problem" (the batched primitive). ⭐ **THE EXISTING `execute_four_step` scaffold is the correct structure but carries exactly the flaws that made it lose:** (1) it constructs the sub-plans `FftPlan p1(n1)`, `p2(n2)` **every call** ⇒ recomputes their O(n1)+O(n2) cos/sin twiddle tables per transform; (2) it calls `p1.execute`/`p2.execute` **per row** (n2 + n1 calls, each paying the deinterleave/reinterleave); (3) the 3 transposes are not cache-blocked. **THE FIX (Lever D, next focused effort):** hoist `p1`/`p2` to ctor-built MEMBERS — ⚠ but `std::optional<FftPlan<T>>` as a member of `FftPlan<T>` is SELF-REFERENTIAL (incomplete type, ill-formed); use a pointer allocated via the crd allocator (placement-new + manual dtor) OR a separate non-recursive `SubFftPlan` type holding just the twiddle table + the pass machinery · replace the per-row execute with a BATCHED sub-FFT (the cache-resident √n sub-transforms, decoupling compute from DRAM = MKL's actual win) · cache-block the transposes (tile so they're bandwidth-bound, not random) · lower `kFourStepMin` from 2^62 to the measured crossover and gate vs the oracle. This is THE large-N parity lever (compute-in-cache, only transposes touch DRAM); it must be done carefully (a rushed naive six-step already lost once — that's the cautionary tale, not a reason to avoid the proper version).

## Verdict
Parity/crush IS reachable — the codelet already matches MKL; the work is overhead elimination (B + A landed, 0.33→0.42×; C + D remain, D is the large-N lever and is now book-grounded with a precise scaffold-fix plan). **C = radix-8/16 first+last passes (the 11–29% last-pass radix-4+reinterleave shuffles). D = register-blocked recursion / cache-blocked unit-stride sub-FFTs (the LARGE-N memory wall — Cerid still does ~6 ping-pong DRAM passes; this is the biggest remaining lever and the hardest).** Honest ceiling of overhead elimination ≈ the codelet rate (~0.9× MKL / near-parity); BEATING MKL also needs a codelet edge (split-radix / better schedule). Multi-session, but the levers are landing and measured. NOT a kernel-codegen wall. (Supersedes the earlier "deferred Spiral-class project" framing for the OVERHEAD portion; the residual codelet-scheduling gap is small.) **Next: Lever A (precomputed per-pass twiddle tables, linear order, kill the `imul`+broadcast setup) — the highest-value remaining lever; then C (radix-8/16 first/last) and D (register-blocked recursion for the large-N memory passes).**
