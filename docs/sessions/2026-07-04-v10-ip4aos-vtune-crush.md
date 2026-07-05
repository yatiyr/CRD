# 2026-07-04 — v10 FFT: the VTune-guided ip4-AoS engine, built → crushed forward → PROMOTED

> The full round-by-round record (19 rounds, every mechanism + refutation) lives in
> **`docs/research/fft-stockham-v2.md`**; boards in
> **`docs/bench/2026-07-03-v10-fft-remeasure-and-midband.md`** (2026-07-04 sections).
> This log = the session summary + verification state + handoff.

## What shipped (all default-on in the tree, uncommitted)

**`execute_ip4aos` — a new interleaved in-place radix-4 FFT engine** in `crd-hesap-fft`
(`fft.hpp`), now the DEFAULT dispatch for:
- **f64: 1K–64K, both parities, forward AND inverse** (supersedes sh/ds on those rows).
- **f32: {2048, 16K, 32K, 64K}** (per-size dispatch — sh keeps {1024, 4096, 8192} where it
  measured faster; never-regress rule; still a pure function of size ⇒ deterministic plans).

Design: COBRA line-complete digit-reverse gather with a fused 3-layer entry (len-4 P-trick +
len-16 compile-time twiddles), k-inner block-pair radix-4 passes with k-unroll×2, hybrid twiddle
tables (3-set ≤1024, w1-only + in-register powers above — the DTLB fix), last pass fused to the
caller's buffer, odd sizes = two half transforms + a radix-2 combine. Inverse = conjugation folded
into the fold's sgn vector (zero extra ops) + negate-at-load. f32 = the twin-unit port (Vec8f = two
128-bit fold units; new vec8f primitives swap_pairs/addsub/fmaddsub/load_dup_pairs/unpack_c_lo/hi/
blend_c_odd/load_c_quad/store_c_lo/hi; vec4d grew swap_pairs/addsub/fmaddsub/load_dup_pairs/
load_pair128/concat_lo/hi/mix_lo_hi). Radix-8 + huge-page + padding variants: built, measured
slower, retained-but-disabled with mechanisms recorded (rounds 9/10/17).

## The method that won: VTune counters-first

WSL2 hides the PMU ⇒ 9 rounds of hypothesis-elimination moved nothing. Installed VTune 2026.0 +
native MKL (oneAPI offline image; component installs COLON-separated; `--action modify` for an
installed product; VS must be closed), built a native clang++ microbench (verified representative),
and the comparative top-down named everything:
- **Our DTLB overhead 24.5% of clockticks vs MKL 2.3%** — cause: 512KB pre-dup'd twiddle tables.
  Fix (w1-only + in-register powers + non-dup storage): **+8-17% per row in two edits.**
- Huge pages = a TRAP for pow-2 strides (set conflicts; even MKL −20% on huge-paged data).
- Final converged profile: DTLB 2.0 ≡ MKL, L1-lat/FB-full at-or-better, instruction count
  formally refuted as the gap (more instructions ran FASTER in round 15/16).
Memory: `feedback_vtune_counters_first_tables_are_tlb_killers`.

## Boards (docs/bench, measurement-time)

f64 default build vs MKL: 1K 0.66 · 2K 0.94 · 4K 0.72 · 8K 0.73 · 16K 0.76 · 32K 0.71
(**1.05× vs FFTW — WIN**) · 64K 0.93; 16K ≈ FFTW parity; native-to-native 16K = **0.85×**;
oracle ≤7.6e-16. f32 rows-on-winners: 0.58–0.82, ≤1.4e-07. Prior banked paths on these rows were
0.53–0.74 ⇒ every row improved or kept, none regressed.

## Verification state at session close

- linux-gcc-release fft gate: **281 asserts / 29 cases green** (incl. f32+f64 inverse round-trips)
  — ran clean after each increment (~12 gates today; 2 transient gcc ICEs, retry-passed).
- win-debug (MSVC, fresh toolset): **25/25 ctest green** (direct binary run = 281/29).
- win-asan: **27/27 green** (fresh-configured post-VS-update).
- **win-shipping: LTO build IN FLIGHT at close** (10-min tool windows; ninja incremental resumes)
  — ctest pending. **win-tidy: pending.** ⇒ per-slice-check is ¾ green; shipping+tidy must finish
  before the slice is called closed (NEVER-claim-done rule — it is NOT closed yet).
- VS 2026 auto-updated mid-session (toolset 14.50→14.51), deleting the old MSVC dir ⇒ every win
  build cache broke (8.3-short-path CreateProcess failures). Fix: delete + reconfigure
  (win-debug/asan/shipping/tidy all rebuilt fresh).

## Retirement verdict (investigated, adjudicated)

sh/ds are NOT retirable: load-bearing for f32 {1K,4K,8K}, all >64K paths (four-step/six-step), and
the six-step/M13 fused branches reference sh structures directly. f64 sub-plan rows now route
through ip4-AoS automatically via execute() — suite-verified.

## 2026-07-05 addendum — the shipping gate: C1002 root-caused + fixed, ctest GREEN

The resumed win-shipping LTO link FAILED after ~40 min: **fatal C1002 (compiler out of heap,
pass 2) at `fft.hpp(895)` → LNK1257.** Mechanism (not a transient ICE): MSVC honors
`__forceinline` even under LTCG, so all 56 generated codelets in the new 143K-line
`batched_codelets_gen.hpp` (single bodies up to ~9K lines) PLUS both `execute_ip4aos`
instantiations were inlined into the one `FftPlan::execute()` — link-time pass-2 codegen
exhausted the compiler heap building that mega-function (17 GB WS / 24.5 GB peak commit
observed; RAM was never the limit — an optimizer-internal blowup). The /Od stack-bomb scar's
LTCG sibling: giant generated straight-line kernels are hostile to EVERY unbounded inliner.

**Fix at the emitter, MSVC-scoped (gcc/clang preprocessed source unchanged ⇒ their measured
boards + prior greens stand):** `CRD_FFT_GEN_INLINE` = plain `inline` on MSVC /
`CRD_FORCEINLINE` elsewhere, applied to all 56 codelets in `batched_codelets_gen.hpp` AND the
generator `scripts/gen_fft_batched.py` (regen preserves it); `__declspec(noinline)` seam on
`execute_ip4aos` (MSVC only). MSVC's own /O2 cost model declines multi-thousand-line bodies;
one call into a straight-line kernel is noise. **Relink SUCCEEDED; win-shipping ctest 29/29
GREEN (4.40 s).** Gate state: linux ✓ · win-debug ✓ · win-asan ✓ · win-shipping ✓ ·
win-tidy → CI (user direction 2026-07-05: don't spend local time; the 18-config sweep covers it).

## Handoff / next

1. **Finish the slice gate:** resume `build-target.bat build/win-shipping crd-hesap-fft-tests`
   until link, run ctest; then win-tidy build of the same target; then the slice is closable.
   **→ DONE 2026-07-05 (see the addendum above); tidy rides CI per user direction.**
2. The 1K-2K band (0.59-0.66×) + the f32 sh rows are the remaining sub-parity rows; the counter
   trail says the residual is retiring-density on hand-scheduled asm (outside the ADR-0082
   portable-intrinsics mandate) — further pushes are optional, not open bugs, per the recorded
   counter evidence (rounds 16-19).
3. Commit proposal below (user commits; NO AI trailer).
