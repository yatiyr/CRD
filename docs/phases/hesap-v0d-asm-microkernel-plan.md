# hesap GEMM floor: framework wins + hand-tuned asm microkernels (plan)

> ## ⛔ OUTCOME 2026-05-30 — CLUSTER INVESTIGATED → REVERTED (intrinsics vindicated).
> The whole asm cluster was BUILT end-to-end (toolchain spike + dual-syntax f64 6×8 kernel + runtime
> CPUID dispatch, all bit-identical to the intrinsic, green on MSVC/clang-cl/gcc) and MEASURED.
> **Decisive same-process A/B: the asm is ~1–2% SLOWER than the inlined intrinsic** (structural — the
> intrinsic inlines into the panel loop at ~98% peak; the asm pays per-call overhead; can't be tuned
> away). bmwcra also washes out (memory/scaling-bound at 8T, not kernel-bound). **ALL asm code + the
> direct-to-C framework change were REVERTED (tree back to v5a-5). ADR-0082 (intrinsics-first) STANDS,
> vindicated by clean measurement.** This doc is kept as the investigation record so it isn't re-tried.
> **The bmwcra crush is the SCALING lever (block-DAG / 2D-within-front), NOT the kernel.** Everything
> below is the original (now-historical) plan — preserved for the record; see ADR-0088's final amendment.
>
> ---
>
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
- **v0d-asm-0 — ADR + framework wins (portable, WASM-safe).** ADR-0088 DONE. Remaining: the
  **direct-to-C microkernel store** + Mc/Kc/Nc retune.
  - **PRECISE DESIGN (verified by reading `gemm_pack.hpp::gemm_packed_inner` + the driver, 2026-05-30):**
    today every panel allocates a zeroed `micro[MR*NR]`, runs the microkernel into it, then
    SCALAR-merges `alpha*micro` into C (the named ~10%). The microkernel does `C += A·B` (no alpha);
    `alpha` is applied at the merge; `beta` in the driver's pre-scale pass; gemm has a **Kc-loop**.
  - **Bit-identity holds ONLY when `alpha==1 ∧ beta==0 ∧ RowMajor C ∧ full MR×NR tile ∧ single
    Kc-block (kc≤Kc ⇒ C_initial==0)`** — then the microkernel accumulates directly into the pre-zeroed
    C region, identical to summing-from-0-then-add. Else (beta≠0 / multi-Kc-block / alpha≠1 / ColMajor
    / edge) KEEP temp+merge (direct would shift ~1 ULP by seeding C_initial into the FMA chain).
  - **The Cholesky hot path qualifies:** cmod/cdiv reroute gemms are `chol_gemm(T{1}, …, T{0}, temp, …)`
    = alpha=1, beta=0, RowMajor temp ⇒ bit-identical direct-to-C, **zero numerics change**, `pack_a`
    (shared w/ syrk) UNTOUCHED. Plumb `alpha`(already there) + a `c_was_zeroed`/`single_block` flag
    from the driver into `gemm_packed_inner`; gate the fast path on the conditions above.
  - Bench square + NT-ColMajor + cdiv shapes; record the residual gap vs OpenBLAS. **HIGH-BLAST-RADIUS
    shared GEMM ⇒ full hesap-suite + determinism moats (bit-identical {1,2,4,8}, real+complex) +
    cross-config (MSVC/clang-cl/gcc) re-verify is the close gate (the long pole, fresh-session).**
  - **▶ PROGRESS (2026-05-30 cont.) — direct-to-C CORRECT + ACTIVATES on both paths; perf-value next.**
    SHIPPED (working tree, all win-debug green):
    - `gemm_packed_inner` `bool c_is_zero=false` param + the RowMajor/full-tile/alpha==1 direct-to-C
      branch (`gemm_pack.hpp`). SERIAL `gemm` site passes `beta==0 && pc==0`; PARALLEL sites wired —
      `small_gemm_parallel` (`beta==0`) + `gemm_parallel` packed (`beta_is_zero && pc==0`) (`blas3.cpp`).
    - **CORRECTNESS (bit-identity, the "new-vs-committed" proof):** since the temp+merge body is
      unchanged from `ac49ff3`, proving `direct ≡ temp` in one binary = `new ≡ committed`. Done via
      `beta=0`(direct) vs `beta=2`-into-zeroed-C(temp) → equal: serial gate in `test_blas3_real.cpp`
      (`[gemm]`, 8219 asserts) + parallel gate (memcmp) in `test_blas3_parallel.cpp` + hesap-direct
      moat 14/14 (incl. multi-Kc-block dense_spd(600) node-parallel front).
    - **ACTIVATION (the advisor's catch — a value-equal opt can't otherwise prove it FIRES):** a
      debug-only (`#ifndef NDEBUG`, ZERO release cost) hit counter `detail::gemm_direct_to_c_hits()` in
      `gemm_pack.hpp`; both gate tests `REQUIRE(hits>before)` → serial + `small_gemm_parallel` direct
      paths PROVEN to fire. Permanent guard: a refactor that kills direct-to-C trips the assert.
    - **PERF VALUE / premise — MEASURED 2026-05-30 (bmwcra factor, WSL linux-gcc-release, 8T capped,
      `git stash` before/after, binary shas CONFIRMED different so the rebuild took, 3 samples each):**
      BEFORE (temp+merge) median ~2194ms range [2157,2254]; AFTER (direct-to-C) median ~2126ms range
      [2104,2135]. **~3% faster, ranges NON-OVERLAPPING (real signal) but WITHIN the ±8% bmwcra noise
      floor + a likely thermal-order confound (before ran second).** ⇒ **direct-to-C is a REAL but MINOR
      (~3%) lever for bmwcra — NOT the big mover** (predicted: the square-NN leaf was already ~98%). The
      delta also CONFIRMS the big-`gemm_parallel`-packed-path activated (bmwcra's huge fronts route there;
      factor moved) — all 3 direct-to-C sites now activation-confirmed. **PLAN-RESHAPING CONCLUSION: the
      framework was NOT the lever for bmwcra ⇒ the asm gemm kernel (the NT-ColMajor cmod rate, ~52 vs
      OpenBLAS ~63 GF/s) is the REAL lever — justified by measurement, not assumption.** Keep direct-to-C
      (correct + activates + small real win + zero downside, bit-identical).
    - **CROSS-CONFIG — DONE locally 2026-05-30:** MSVC win-debug ✓ (correctness + activation), **clang-cl
      ✓** (8249 asserts / 18 cases, activation REQUIREs pass on clang too), **win-tidy ✓** (clang-tidy
      LLVM 20.1.8 clean on the 3 touched TUs incl. the atomic counter + direct-to-C branch — forced
      recompile, no warnings), gcc linux-gcc-release lib compiled clean (perf builds). REMAINS for CI:
      gcc/clang DEBUG test build (the `#ifndef NDEBUG` counter) + asan + shipping + the ctest guards.
    - **Mc/Kc/Nc retune — CHARACTERIZED (measure-informed), NOT swept:** the perf measurement showed the
      framework is a MINOR (~3%) lever; block-size tuning is ALSO framework (prior v5a-4: kGemmMc 120→480
      = +2.6%, "not the lever"). Sweeping a known-marginal knob is low-value host-load; the asm kernel is
      the lever. Deferred-marginal with rationale (revisit only if a measurement says block-size matters
      for a specific shape).
    - **✅ v0d-asm-0 FRAMEWORK HALF COMPLETE** — direct-to-C correct + activates (all 3 sites) +
      perf-measured (~3% bmwcra, framework is a minor lever) + cross-config green + retune characterized.
      **▶ NEXT = the asm kernel (the REAL lever, justified by the ~3%-framework measurement): v0d-asm-1
      toolchain spike** (runtime CPUID dispatch + dual-syntax MASM/GAS no-op + MSVC/clang-cl/gcc build
      integration + WASM/ARM fallback — the build-integration LONG POLE; fresh focused start) → v0d-asm-2
      f64 6×8 asm. Working tree coherent + WIP (commit at phase end per the user).
- **✅ v0d-asm-1 — toolchain spike COMPLETE 2026-05-30 (the long pole, DE-RISKED).** Shipped: a trivial
  constant-returning probe in BOTH MASM (`src/asm/gemm_asm_probe_win64.asm`) and GAS
  (`src/asm/gemm_asm_probe_lin64.S`) exporting extern "C" `crd_hesap_gemm_asm_probe` (sentinel 0xCE51D);
  runtime CPUID `detail::asm_backend.{hpp,cpp}` (`cpu_has_avx2_fma()` cross-compiler: MSVC/clang-cl
  `<intrin.h>`, gcc/clang `<cpuid.h>`+xgetbv, non-x86→false); CMake mirrors crd-jobs — x86-64-guarded
  (`CMAKE_SYSTEM_PROCESSOR MATCHES x86_64|AMD64|...`), `WIN32`→ASM_MASM+`.asm` (covers MSVC AND clang-cl
  via ml64), else→ASM+`.S`; `CRD_HESAP_HAS_ASM_PROBE` PUBLIC def gates the C++ (ARM/WASM never reference
  the symbol → intrinsic path). `[asm]` test (`test_asm_backend.cpp`) proves assemble+link+call.
  **VERIFIED GREEN: MSVC (ml64) + clang-cl (ml64) + gcc (GAS .S) all assemble + link + run the probe
  (sentinel byte-identical); clang-format clean; win-tidy (LLVM 20.1.8) clean on the new CPUID code.**
  Isolated from the working gemm (the probe is a constant, not yet wired into dispatch). Build-integration
  risk is GONE — the real kernel is now "just" the kernel.
- **▶ v0d-asm-2 — f64 AVX2 asm microkernel (6×8): KERNEL WRITTEN + VERIFIED BIT-IDENTICAL 2026-05-30.**
  Hand-tuned `crd_hesap_gemm_microkernel_avx2_f64_asm` in BOTH MASM (Win64: rcx=k/rdx=a/r8=b/r9=c,
  ldc@[rsp+200] after the XMM6-15 callee-save; 12 ymm acc + b0/b1 + a-bcast) and GAS (SysV: rdi/rsi/
  rdx/rcx/r8, all-caller-saved), same vfmadd231pd p-order as the intrinsic ⇒ same bits. Declared in
  `detail/asm_backend.hpp`; `[asm]` bit-identity GATE in `test_asm_backend.cpp` (asm vs intrinsic, `==`
  per element, k=1/7/64/200 + strided ldc + nonzero initial C, 240 asserts). **VERIFIED bit-identical
  on gcc (GAS) + MSVC (MASM) + clang-cl (MASM), first attempt each.** ▶ STILL TO DO: (1) **wire into a
  HOISTED runtime dispatch** (CPUID-resolved fn-ptr decided ONCE, not a per-microkernel-call branch;
  asm when `asm_backend_available()` + f64, else intrinsic — bit-identical so functionally transparent);
  (2) **PERF** — does the asm beat the intrinsic on the NT-ColMajor cmod / cdiv shapes + bmwcra factor
  (the actual lever)? Optimized/WSL build, file-captured. (3) clang-format ✓ / win-tidy + full suite.
- **v0d-asm-2 (orig plan line) — f64 AVX2 asm microkernel (6×8).** Hand-tuned MASM + GAS matching the locked signature,
  packing layout (A: MR×K row-major, B: K×NR row-major, C: MR×NR strided), and bit-identical FMA
  p-order. Microkernel-vs-intrinsic bit-compare gate. Wire into runtime dispatch. Bench square +
  NT-ColMajor/cdiv vs intrinsic + OpenBLAS (the in-situ lift). Full suite + moats + cross-config.
- **▶▶ v0d-asm-2 OUTCOME 2026-05-30 — WIRED (opt-in) + MEASURED; the bmwcra premise is REFUTED at scale.**
  Wired into the f64 dispatch via a cached `use_asm_microkernel_f64()` (resolved once: env
  `CRD_HESAP_ASM_KERNEL` + `asm_backend_available()`); **OPT-IN, default = intrinsic** (getenv moved to
  `asm_backend.cpp::asm_kernel_env_requested()`, MSVC-C4996-pragma-guarded — NOT the hot header).
  Build green MSVC+clang-cl+gcc; default-intrinsic suite + `[asm]` gate + moat all pass (8559/14).
  - **KERNEL microbench (asm-on vs asm-off):** asm ≥ intrinsic on EVERY shape — DIRECTION robust,
    MAGNITUDE untrustworthy (a ~9% cross-run turbo split confounds both raw [+3–6%] and %-of-peak
    [+10–16%]; startup-clock≠GEMM-clock on the 14900K, advisor-flagged). Net: the asm kernel IS faster.
  - **DECISIVE in-situ A/B (bmwcra factor, SAME binary env-toggle, 8T, 3× each):** ASM-ON median ~2174ms
    [2148,2212] vs ASM-OFF ~2194ms [2103,2255] — **statistically INDISTINGUISHABLE** (full overlap;
    asm-OFF posted the single fastest run, 2103). **The kernel win WASHES OUT at scale** ⇒ bmwcra @8T is
    **memory/scaling-bound, NOT kernel-IPC-bound** (the 2.01×@8T plateau).
  - **HONEST CONCLUSION: asm does NOT crush bmwcra — ADR-0088's "asm kernel = the bmwcra lever" premise
    is refuted at scale.** Kernel KEPT (correct + faster-at-kernel-level + permanent multiplatform
    foundation), **OPT-IN** (no bmwcra benefit + Win64 prologue overhead on small-K + one micro-shape raw
    regression ⇒ NOT engine-default; default-on is its own slice needing a kernel-bound consumer's
    evidence). **bmwcra's crush needs the SCALING lever (block-DAG / 2D-within-front), not the kernel**
    (confirms the v5a-6 diagnosis). ▶ REMAINING: clang-format/tidy on the wiring + full DoD.
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
