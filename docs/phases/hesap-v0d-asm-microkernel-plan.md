# hesap GEMM floor: framework wins + hand-tuned asm microkernels (plan)

> Planned 2026-05-30. **Re-opens ADR-0082** (intrinsics-only) by explicit user decision: make the
> GEMM kernel floor a *permanent foundation* before building more hesap on top of it. The ADR-0082
> three-condition revisit gate is NOT fully met (intrinsic gemm is ~79% of HW peak, > the 70%
> threshold) — this is a deliberate strategic override for the engine-wide gemm floor + multiplatform
> durability, recorded in a new superseding ADR. Slot: AFTER committing v5a-5 + the bmwcra routing+cap
> fix, BEFORE v5b sparse LU (so v5b and all later hesap ride the raised floor).

## Why (measured, honest — three buckets, not one)
The full-gemm gap to OpenBLAS is NOT a single thing:
1. **C++ framework (portable, WASM-safe, no asm):** `gemm_packed_inner` temp-micro + scalar-merge
   (~10%), Ac/Bc cache feeding, Mc/Kc/Nc block sizes. The square-NN microkernel *leaf* is already
   ~98% of peak standalone (`_uk_spike`), so the square gap is here, not in the kernel.
2. **asm-addressable:** the modest **NT-ColMajor cmod (~52 GF/s)** and **cdiv (0.40–0.45× OpenBLAS)**
   shapes that dominate the sparse factor are NOT near peak; plus in-situ prefetch + instruction
   scheduling the compiler won't emit from intrinsics.
3. **True asm-vs-OpenBLAS ceiling:** the residual after 1+2.

bmwcra factor proved the stakes: Cerid flops == CHOLMOD flops (measured 1.286e11 ≈ 1.287e11), so the
factor ceiling == the gemm per-flop ratio (~0.71–0.82×). Raising that ratio is the ONLY ≥1.0× path
for bmwcra AND a ~20% win for every gemm-bound hesap op (eig/SVD/LU/dense-Chol/AMG).

## Settled decisions (user, 2026-05-30)
- **Sequencing:** framework-first → asm vs the *measured* residual (bucket 1 is on the asm critical
  path anyway — asm needs the direct-to-C store; ships portable value even if asm slips).
- **Dispatch:** **runtime CPUID → function-pointer** (one binary picks AVX2/AVX-512/NEON/scalar);
  WASM uses its fixed SIMD path at compile time. Replaces the compile-time CRD_HESAP_MICROKERNEL_BACKEND
  switch (kept as the build-time *fallback* selector).
- **MSVC coverage:** **dual-syntax** — MASM `.asm` (ml64) for MSVC + GAS `.S` for gcc/clang. The
  primary ship compiler gets asm too. ~2× asm maintenance per µarch; both bit-identical to each other
  AND to the intrinsic kernel.

## Hard invariant (the ABI contract, now cross-compiler AND cross-ISA)
Every backend — intrinsic-f64, asm-f64 (MASM≡GAS), future AVX-512/NEON — must be **bit-identical**:
single-rounded FMA (`vfmadd231`-equivalent), the SAME p-order, and the SAME NR/accumulation grouping
(6×8 f64, 8×8 f32). AVX-512's wider tile is NOT free — it must hold the accumulation grouping or it
breaks the determinism moat. A direct microkernel-vs-intrinsic bit-compare test is the gate, run on
every backend the dispatch can select.

## Slice breakdown (the "v0d-asm-microkernel" cluster)
- **v0d-asm-0 — ADR + framework wins (portable, WASM-safe).** New ADR superseding 0082 (gate-override
  rationale + runtime dispatch + cross-ISA bit-identity contract). Direct-to-C microkernel store
  (kill the temp-micro + scalar-merge), Mc/Kc/Nc retune (measured). Bench square + NT-ColMajor + cdiv
  shapes; record the residual gap vs OpenBLAS. Bit-identical ⇒ full hesap-suite + determinism moats
  re-verify with zero value change.
- **v0d-asm-1 — toolchain spike (de-risk integration BEFORE real FMAs).** Runtime CPUID detect +
  function-pointer table on the locked `gemm_microkernel<T>` signature. A trivial no-op kernel in BOTH
  MASM `.asm` and GAS `.S` exporting the symbol; CMake assembles the right one per compiler; link +
  call. Green on MSVC + clang-cl + gcc; confirm WASM/ARM/scalar cleanly select the intrinsic path (asm
  NOT selected). Pins the build pipeline + dispatch before any real kernel. **Highest-risk item.**
- **v0d-asm-2 — f64 AVX2 asm microkernel (6×8).** Hand-tuned MASM + GAS matching the locked signature,
  packing layout (A: MR×K row-major, B: K×NR row-major, C: MR×NR strided), and bit-identical FMA
  p-order. Microkernel-vs-intrinsic bit-compare gate. Wire into runtime dispatch. Bench square +
  NT-ColMajor/cdiv vs intrinsic + OpenBLAS (the in-situ lift). Full suite + moats + cross-config.
- **v0d-asm-3 — f32 AVX2 asm microkernel (8×8).** Same shape.
- **v0d-asm-4 — integrate + measure the engine-wide win.** Re-run the CHOLMOD crush (does asm+framework
  flip bmwcra ≥1.0×?), the dense gemm benches (eig/SVD/LU/dense-Chol lift), determinism moats, full
  18-config sweep. Lock the ADR.
- **Future (filed, NOT this cluster):** AVX-512 microkernel (server µarchs; pin the accumulation
  grouping for the moat), NEON (ARM/Apple), WASM-SIMD path validation, the genuine ceiling
  characterization.

## Risks / open items
- **Build integration (the long pole):** MSVC has no x64 inline asm; MASM (.asm/ml64) vs GAS (.S)
  syntaxes diverge → dual sources. v0d-asm-1 proves it before investment.
- **CPUID/runtime dispatch home:** does crd-core / crd-math::simd already expose feature detection? If
  not, add a minimal `cpu_features()` (AVX2/FMA/AVX-512/NEON) — leaf, core-only.
- **Determinism across the dispatch set** is the moat's new failure mode — bit-compare gate mandatory.
- **14900K has NO AVX-512** (Raptor Lake consumer) → AVX-512 kernel can't be dev-box-tested; defer to
  a CI runner that has it, or a later µarch pass.

## Out of scope (kept separate, land first)
- Verified **v5a-5 checkpoint** (commit before this cluster starts piling on).
- The cheap **bmwcra routing+cap fix** (recovers 0.66→~0.78× — its own small bankable slice).
