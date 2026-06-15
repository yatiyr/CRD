# 2026-06-15 — FFT small-N codelets wired into the engine (parity + a narrow N=8 batched win) — corrected

> ⚠ **HONESTY ADDENDUM at the foot of this file (same day).** The body below was written before a cleanup
> pass that the user mandated: the file `crush_codelets.hpp` was renamed `small_n_codelets.hpp`, the oversold
> source comments were corrected to the measured truth (single-transform = PARITY, not a crush — the "1.70×"
> was a partial-hoist artifact), and the one real win (N=8 batched 1.49×) was re-verified. Read the addendum.

> Live detail + every number: `docs/research/fft-mkl-crush.md`. Memory: `project_v10_fft_plan`.
> Prior session (`2026-06-15-fft-genfft-atom-parity.md`) banked parity + the probe-only N≤32 crushes; THIS
> session converts the proven probe crush into a SHIPPED, gated engine capability and pins the honest scoreboard.

## The arc

Standing mandate: crush MKL on FFT. The N≤32 crushes existed only in `build/crush*.cpp` probes; the shipped
engine used the SoA leaf codelets (below MKL). This session ported the winning construction into the engine and
measured the **shipped** artifact honestly (the probe numbers had a hoist artifact — see below).

## What shipped (engine, committable)

New header **`engine/hesap-fft/include/crd/hesap/fft/detail/small_n_codelets.hpp`** (renamed from the original
`crush_codelets.hpp` in the cleanup addendum) — AoS **lane-trick** single-
transform codelets for N ∈ {8,16,32}, f64, AVX2-guarded (`CRD_SIMD_HAS_AVX2`; SoA path is the SSE2/scalar/NEON
+ f32 fallback). Construction = DIT radix-2 with the even/odd decimation in the two ymm LANES (a contiguous
256-bit load `[x[2e],x[2e+1]]` puts evens in lane0 / odds in lane1 ⇒ the half-size sub-DFT runs over-2, zero
`permute2f128`), then a paired lane-merge combine (2 outputs/iter, 256-bit stores). Inverse = the conj trick
`ifft_unnorm = conj(fft(conj·))` reusing the EXACT gated forward path. Plus `dft8_over2`/`dft16_over2` helpers
and a batched N=8 driver.

Two wirings in `fft.hpp`:
- **`execute()` (single transform), N∈{8,16,32} f64**: in-place AoS codelet replaces the deinterleave→SoA-codelet
  →reinterleave round-trip.
- **`execute_batched()`, N=8 even batch f64**: the over-2 codelet drives the batch (element-major layout makes
  adjacent transforms contiguous = the over-2 ymm). four-step never calls execute_batched with n≤32 (it splits
  n≥2¹⁹ into ~2⁹ factors), so this fast path is reached only by direct small-N batched callers (v10-e).

Gate: `tests/hesap-fft/test_fft.cpp` — inverse test generalized across sizes (covers 8/16/32 both directions),
new even-batch test (engages the N=8 path, fwd+inv). Suite **126/21 green**.

## The honest scoreboard (measured on the SHIPPED header, 14900K AVX2 WSL+MKL)

⚠ **Method correction (the artifact the dossier warns about):** a "single-transform 1.70×" first reading was a
PARTIAL-HOIST artifact — the anti-hoist feedback touched only `in[0]`, so the compiler hoisted the rest of the
codelet out of the timing loop. The honest harness (pool of bounded inputs, data-dependent index, no hoist)
splits the result by regime:

| N | single-call **latency** (vs MKL single) | **throughput** (vs MKL **batched**) |
|---|---|---|
| 8  | ~1.00× parity | **2.39× CRUSH** (engine execute_batched: **1.46×**) |
| 16 | ~0.96× parity | 1.17× (engine batched: 0.30× — strided wall, see below) |
| 32 | ~0.98× parity | 1.09× (engine batched: 0.37× — strided wall) |

- **execute() single call = parity + correctness.** This construction is throughput-by-nature (the
  `permute2f128` combine is a latency chain hidden only when independent calls overlap); a non-inlined
  `execute()` single call is latency-bound. BUT it is **1.50×/1.76×/2.54× FASTER than the prior SoA leaf path**
  (`old_vs_new_execute.cpp`) — so it lifts small-N single-call from ~0.4–0.6× MKL up to ~parity. Real, measured,
  committable as "parity + correct + faster-than-prior", NOT a crush.
- **execute_batched N=8 = 1.46× CRUSH over MKL-batched** (was 0.52× on the SoA path). The first **user-accessible**
  MKL crush in the shipped engine. Gated 2.44e-16.

## The N=16/32 batched wall (named, left for fresh context — the v10-e lever)

The over-2 codelet on **element-major** layout gathers each transform's elements strided by `b` (the batch
stride). N=8 (8 ymm) survives the striding (1.46×); N=16 spills (`dft16_over2` needs 32 ymm) AND the strided
gather thrashes cache ⇒ **0.32×** (vs the contiguous transform-major probe's 1.17×). The fix is the
block-transpose-to-contiguous "AoS-codelet-assembly" — delicate, fresh-context (dossier §14 class). N=32 same.
Existing execute_batched stays the (correct, slower 0.30–0.37×) path for 16/32. **Do not start it at depth.**

## DoD

win-debug ✓ (126/21) · win-asan ✓ (126, **zero ASan errors** on the new intrinsics/loadu/storeu/in-place) ·
win-shipping/LTCG ✓ (126) · win-tidy ✓ (clang-tidy strict clean on the new header + wiring) · clang-format ✓
(my changes; pre-existing fft.hpp/test_fft.cpp violations untouched) · gcc strict-conversion ✓ (my code:
`-Wall -Wextra -Wconversion -Wfloat-conversion -Wsign-conversion -Werror`).

⚠ **Pre-existing DEBT found (not mine, not fixed here):** the WSL-gcc build of `crd-hesap-fft-tests` fails on
`dct.hpp:244` `-Werror=conversion` (`usize→double` in the **f32** instantiation of `DctPlan::direct_dst3`,
triggered by `test_dct.cpp`). This is the documented `feedback_T_double_literal` gcc-f32 hazard; the v10-f DCT
slice was only DoD'd on the 4 Windows configs, so it's latent. Blocks the gcc test-binary build (not my FFT
code, which compiles gcc-strict-clean). Fix: explicit `static_cast<double>` on the integer subexpressions in the
DST-III/DCT-III direct loops. → `docs/debt.md`.

## Honest framing (what to claim)

- **execute() small-N: parity with MKL + correct + 1.5–2.5× faster than the prior engine path.** Not a crush.
- **execute_batched N=8: a real, gated, user-accessible 1.46× MKL-batched crush.** Narrow (N=8 only).
- N=16/32 batched + the large-N gap remain the open levers (large-N = the converged person-weeks genfft engine).

## Seeds (build/)

`crush_single_all.cpp` (single lane-trick 8/16/32 fwd+inv gate + vs MKL-single), `old_vs_new_execute.cpp`
(new-vs-old SoA latency discriminator), `engine_crush_bench2.cpp` (throughput vs latency, shipped header),
`engine_batched_bench.cpp` (FftPlan::execute_batched vs MKL-batched), `batched_codelet_probe.cpp` (over-2
element-major, the N=16 strided-wall evidence).

## Proposed commit (superseded — see the addendum's commit message)

The original block here is kept for history but its claims/filename are corrected below. Use the addendum's.

---

## ⚠ ADDENDUM 2026-06-15 — honesty cleanup + genfft engine build started

The user reviewed the SOURCE (not just these docs) and flagged that it oversold MKL: a file named
`crush_codelets.hpp` whose header comment claimed a single-transform "BEAT MKL … N=32 ~1.70×" — a number this
very session log had already debunked as a partial-hoist artifact (the honest single-transform result is
PARITY). The code contradicted its own corrected notes. Fixed:

- **Renamed** `crush_codelets.hpp` → `small_n_codelets.hpp`; de-branded the symbols (`CrushTw`→`SmallNTwiddles`,
  `crush_fft_f64`→`small_n_fft_f64`, `crush_batched8_f64`→`small_n_batched8_f64`).
- **Corrected the header + `fft.hpp` wiring comments** to the measured truth: single-transform N∈{8,16,32} =
  **PARITY with MKL (~0.96–1.00×), NOT a crush**, ~1.5–2.5× faster than the SoA leaf it replaces; the "1.70×"
  is named as a measurement artifact. `execute_batched` N=8 = the one real win.
- **Re-verified the one surviving claim** on the SHIPPED header (bracketed Cerid–MKL–Cerid, core-pinned, 3
  runs): **N=8 batched = 1.49×** (gate 2.44e-16); N=16/32 batched = 0.31×/0.37× (below MKL, strided wall).
  Build + `ctest` (win-debug): **17/17 fft tests pass.**
- De-branded the two `test_fft.cpp` comments; fixed stale `crush_codelets.hpp` pointers in the dossier +
  context.md. The 56 `build/*.cpp` probes are gitignored (never committed) — kept as resume seeds (user choice).

**FULL-CRUSH decision (user-ratified):** build the genfft codegen engine (E1–E5, dossier Part 11). Honest
framing carried in: on this AVX2 14900K the documented large-N ceiling is **parity** (the 1.6× existence proof
was AVX-512 Sapphire Rapids); E3 register-residence is the lever; person-weeks, fresh-context.
**E1 DONE this session** (`build/e1_strided_leaf.cpp`, gate 2.87e-16): the inlined N=64 leaf extracted into two
reusable strided twiddle-codelets (`sr8_tw_contig` + `sr8_tw_gather`); N=64 re-composed from them stays at
parity (~0.99–1.02× MKL) — the composable building block E3 will assemble. **NEXT (fresh session): E3.**

### Proposed commit (use this one)

```
refactor(hesap-fft): honest small-N codelets — rename crush_codelets.hpp, correct MKL claims to measured truth

- Rename detail/crush_codelets.hpp -> small_n_codelets.hpp; de-brand symbols (CrushTw->SmallNTwiddles,
  crush_fft_f64->small_n_fft_f64, crush_batched8_f64->small_n_batched8_f64). Same verified code.
- Correct header + fft.hpp wiring comments: single-transform N<=32 = PARITY with MKL (NOT a crush; the prior
  "1.70x" was a partial-hoist measurement artifact), ~1.5-2.5x faster than the SoA leaf it replaces.
- execute_batched N=8 = 1.49x over MKL-batched, re-verified on the shipped header (gated 2.44e-16); N=16/32
  batched stay on the SoA path (below MKL, strided-gather wall).
- test_fft + dossier + context.md de-branded / pointers fixed. ctest win-debug 17/17.
- Start the user-ratified genfft engine (E1: build/e1_strided_leaf.cpp, composable strided leaf, N=64 parity).
```
