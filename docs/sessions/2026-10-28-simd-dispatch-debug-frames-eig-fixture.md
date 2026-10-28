# 2026-10-28 — SIMD-DISPATCH: unoptimised forced inlining and the eig complex-pair fixture

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `702838ce`.

## CI first

Complete run 40592200636 (`d57b11cd`) is still in progress, but its finished jobs hold three real failure groups:

- Every lane: `dtrevc: T*v = lambda*v ...` and `eig: A*v = lambda*v ... (f64)` fail `CHECK(saw_complex)` at n = 8.
- win-clang-cl: the fft `[kernels]` cases and all five `crd-hesap-fft-kernels-at-<level>` arms die of a stack overflow.
- linux-gcc-debug-sse2: SIGSEGV in `sparse: the 1 2 4 8 16 worker moat` and `batched: small-SVD Jacobi`.

win-tidy's `performance-no-int-to-ptr` at dense_kernels.cpp:35 is already fixed (`88540c19`). linux-gcc-shipping and
linux-gcc-release ended with exit 143 ("the runner has received a shutdown signal"), and linux-gcc-relwithdebinfo's
build step has no conclusion: runner losses, not code. The push runs since then were cancelled before any job failed.
No row is Needs CI; DIAG.11c and DIAG.12 still wait on a finished complete-tier run.

## Forced inlining only in optimised builds

GCC and Clang (clang-cl too) honour `always_inline` at -O0, where every inlined body keeps its own stack slots. MSVC's
/Od inlines nothing, which is why win-debug and win-debug-sse2 passed.

- linux-gcc-debug-sse2 (`-fstack-usage`, the lane's command at the SSE2 baseline): `leaf_fiber_fused<double>` took
  70,944 bytes and the batched SVD lambda 57,376, past the 64 KB fiber stack. These are the 12 and 18 `simd::fma`
  sites whose SSE2 body is the exact fma chain (`fma_exact.hpp`). At avx2 the largest frames were 33 KB.
- win-clang-cl (`-Wframe-larger-than`, the sse2 fft unit): 19 functions were above 256 KB, up to 2.17 MB in
  `codelet64_stage1_fused_64x64`, past the 1 MB main stack. The generator's own unoptimised clause (REPO.3c.8) covered
  only `CRD_FFT_GEN_INLINE`; the hierarchical codelets and the engine passes use `CRD_FORCEINLINE`.

`CRD_FORCEINLINE` now forces inlining only under `__OPTIMIZE__` (clang-cl /O2 defines it, /Od does not; probed) and
stays `__forceinline` for cl. Unoptimised GCC and Clang therefore build as MSVC /Od does. The fft generator's clause is
reduced to the cl LTCG case and the header is regenerated (WSL, byte-identical apart from those lines).

After the change:

- GCC sse2 frames equal the avx2 ones: largest 10.6 KB in test_sparse and 33.5 KB in test_batched.
- The clang-cl sse2 fft unit's largest frame is 144 KB. Rebuilt on win-clang-cl, the fft `[kernels]` cases pass (2,919
  assertions).
- An -O0 avx512 sparse kernel object defines nothing outside its level namespace except its table and
  `DW.ref.__gxx_personality_v0`.
- One `-Wpsabi` note appears per sse2 test unit.

Regression (hosted linux-clang-tsan, push run 40623100798 at `bedfc2bb`): the TSan lane builds at -O0, so the
sanitizer fiber-switch hooks went out of line, the defect DIAG.1b fixed on 2026-10-06 (the hook's TSan exit frame popped
on the target fiber). TSan then reported races on fiber dispatch in four tests (the JD and LOBPCG determinism moats and
`fiber keep: with fewer processors than threads`). The hooks now use `CRD_ALWAYS_INLINE`, which forces inlining at
every level and is meant only for bodies that must run in their caller's frame; the tooling suite fails if a hook
loses it or the macro tests `__OPTIMIZE__`.

`platform.hpp`'s compiler notes said clang-cl resolves as `CRD_COMPILER_CLANG`; it resolves as `CRD_COMPILER_MSVC`, and
the notes now say so.

## The eig fixture's complex pairs were rounding

`fill_general(a, 0.5)` is a rank-2 matrix plus 0.5 I, so its spectrum is real (numpy): 0.5 has multiplicity n - 2.
The two tests saw complex pairs only because rounding split that eigenvalue into a pair with an imaginary part near
±3e-15. The per-level arithmetic now leaves them real. The dtrevc, eig f64 and eig f32 cases take
`fill_complex_spectrum`, a full-rank matrix with a quadratic phase. Their checks are stricter than the old flag: each
case must find exactly numpy's number of pairs with |Im| > 1e-6, which is 2, 6, 8 and 19 at n = 8, 16, 20 and 50
(every genuine |Im| is at least 3e-3). `[eig][nonsym]` passes on win-debug, and the real cases also pass forced to
portable and sse2.

## Also fixed

`0d126a9d` changed docs/generated/dependency-licenses.md (the SDE pins) without refreshing its manifest hash.
check-generated and the tooling suite's provenance case failed on that, and both pass after the refresh.

## Left

As the row says. The follow-up this note named (`fill_general`, rank 2 plus a shift, also the fixture of the balance,
AED, dlaqr5 and reorder cases) was done in `bedfc2bb`: every nonsymmetric case runs on full-rank matrices.
