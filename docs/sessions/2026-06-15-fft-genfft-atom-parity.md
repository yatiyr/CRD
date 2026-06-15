# 2026-06-14/15 — FFT genfft atom: MKL parity + engine build committed

> Live detail + every number: `docs/research/fft-mkl-crush.md` (§1c batched-8 beat, §1d genfft atom + N=512
> convergence, §11 engine build plan). Memory: `project_v10_fft_plan` Parts 26–27.

## What happened

Continuation of the relentless "crush MKL on 1D-complex FFT" arc. Two genuine results + one honest boundary + a
ratified decision.

1. **First measured MKL-beat (narrow): batched size-8.** Over-2 split-radix-8 (`build/aos2sr8.cpp`) vs MKL DFTI
   batched, clean back-to-back: Cerid 48–56 vs MKL 44–47 = **1.03–1.27×, wins every run**. Bounded: at size-16 the
   over-2 codelet spills (16 ymm) and loses (0.71×); MKL's batched rate rises with size. Real on a real use case
   (batched FFT = v10-e) but not the general crush.

2. **The genfft redirect (advisor-gated) + the N=64 atom = MKL PARITY.** hpkfft paper (`docs/books/hpkfft-paper-2023.pdf`)
   is the existence proof: 1.42× over MKL fp64 AVX2 in pure C++, N=2..8192 single-transform L1-resident. The
   construction is the **recursion**, not an all-in-one codelet: four-step N=n1·n2 makes the outer DFTs lane-aligned
   (zero `permute2f128`); only the small leaf shuffles. My earlier all-in-one codelet was permute2f128-bound because
   it did the lane-crossing stages in-register — the SPIRAL "frontier" was the *wrong construction*. Built the atom
   `build/fftcrush_n64.cpp` (N=64=8×8, over-2 sr8 leaf + inline-gather transpose, output perm places over-2 lanes
   adjacent ⇒ one transpose, no output transpose), gated 2.87e-16: **Cerid 56–62 vs MKL 55–60 = parity** (ranges
   overlap; honest-scoreboard = parity, NOT a win). Ceilings: no-twiddle 75, no-transpose 84 (leaf ~1.3–1.5× MKL).
   - ⚠ Methodology: a first run read 0.63× from a transient 14900K throttle. Cerid MUST be measured bracketed
     before AND after MKL in the same binary; best-of-30 showed stable parity.

3. **The honest boundary: parity does not carry up.** N=512=8×64 (`build/fftcrush_n512.cpp`, gated 1.07e-15)
   **converged at/below MKL** across three variants: naive (scalar scatter) 0.73×, NOSCATTER ceiling 0.92× (even a
   *perfect* output transpose < MKL), FUSEDT (vectorized 2×2 `permute2f128` transpose) 0.72% — **worse than scalar**.
   The decisive finding: vectorizing the transpose needs a buffer round-trip that costs more than it saves ⇒ **the
   reorder is fundamental data movement, not an instruction-choice problem** (AVX2 has no cheap scatter). Root
   (advisor, from the N=64 ceilings): the better leaf is *exactly offset* by paying MKL-equivalent twiddle (~24%) +
   reorder (~32%) overhead ⇒ **parity is the construction's natural ceiling, by construction.**

4. **Decision (user-ratified): build the genfft engine.** Closing parity → 1.42× is a multi-week minimal-pass
   genfft engine (register-residence across radix stages + transpose-free Stockham so the per-level reorder vanishes
   + conjugate-pair split-radix), not another probe. User chose to build it. Scoped as slices E1–E5 in dossier §11;
   make-or-break = **E2 (transpose-free Stockham over-2 driver)**.

## State / next

- v10 already ships honest wins: beats PocketFFT/scipy/numpy everywhere; crushes scipy real-FFT; wins NUFFT
  small/mid; beats MKL batched-8. 1D-complex = honest MKL parity (strong vs the speed king).
- **E1 + E2 BUILT this session** (`build/fftcrush_stockham.cpp`, gated 1.98e-15). E2 = batched-2 over-2 radix-8
  Stockham (transpose-free, self-sorting) measured **0.57× MKL — pass-bound, worse than the four-step.** Complete
  measured picture: four-step = reorder-bound (0.92× ceiling), Stockham = pass-bound (0.57×), atom = parity. The two
  canonical structures lose at complementary walls; NEITHER beats MKL. **The win requires E3 (cache-blocked
  multi-stage = few passes AND fused reorder = FFTW cache-oblivious recursion) — now empirically confirmed
  necessary, the genuine multi-week core.**
- **Next session (fresh context):** E3 — cache-blocked recursion (L1-resident blocks carried through multiple radix
  stages; register file caps a register-resident leaf at ~size-16). This is the real engine; E1/E2 closed off the
  simpler alternatives by measurement.
- Seeds (persist in `build/`, NOT /tmp): `fftcrush_n64.cpp` (atom, parity), `fftcrush_n512.cpp` (3 variants +
  NOSCATTER/FUSEDT probes), `aos2sr8.cpp` (the 56-GFLOPS leaf).
- Pending user: commit the banked v10 wins (fstw, over-2 leaf, batched-8 beat, atom) + the dossier/seeds.

## Doctrine notes
- Honest scoreboard caught me framing parity as a 1.02× win — overlapping ranges = parity. Fixed in dossier+memory.
- Don't emit another transpose variant to "decide" a converged question silently — surface the decision (probe vs
  multi-week build) to the user. Did so via AskUserQuestion.
