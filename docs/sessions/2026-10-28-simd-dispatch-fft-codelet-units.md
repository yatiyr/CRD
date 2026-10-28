# 2026-10-28 — SIMD-DISPATCH: fft codelet units, the complex Hessenberg subdiagonal and two CI fixes

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `c073ea77`.

## CI first

Push run 40623100798 (`bedfc2bb`) was still running; its finished jobs held four groups:

- Every Windows lane that runs the tooling suite: the SDE installer case read `pin.ld.so` as an invalid path. The hosted
  runner may create symlinks, and a Windows symlink cannot resolve the kit's POSIX-separated relative target (this host
  lacks the privilege, so tarfile copied the target and the case passed). On Windows an accepted link now becomes a hard
  link to its in-archive target, links are extracted after every file, and an extraction error raises
  (`errorlevel=2`). The case puts the link before its target and checks the link kind; it passes on Windows and WSL.
- win-tidy: `l` beside the `BinnedIsa` alias `I` (misc-confusable-identifiers, tensor_kernels.cpp), and two variable
  templates whose deduced array bounds hid their constness from readability-identifier-naming
  (test_dense_vector_kernels.cpp). The loops say `lane`; the arrays carry their bounds. Local tidy reproduced the
  naming finding on HEAD's file and is clean after.
- win-release, win-relwithdebinfo, win-shipping, win-shipping-profile: C1002/C1060 out of heap in the fft sse2 and
  portable units (below).
- linux-gcc-release, -relwithdebinfo, -shipping: the runner shut down during the build (exit 143), the fft units'
  phase; read with the next complete run.

Fixed in `21ccf7c2`.

## The last complex Hessenberg subdiagonal (`bedfc2bb`)

The complex reduction stopped one reflector short of zgehd2, so H's last subdiagonal kept its imaginary part, and the
zlarfg port returned only Re(alpha) for a one-element vector. The reduction now runs the length-1 reflector, Q applies
it, and that reflector maps the element to the real beta as zlarfg does. The rank-2 and rank-4 eig fixtures had hidden
it (their trailing subdiagonal was zero); every nonsymmetric case now runs on full-rank matrices with distinct
eigenvalues.

## Codelet units

The generated codelets were inline in the one engine unit per level, so the whole unit took 799 s and 4.9 GB at sse2
(MSVC 14.51 `/O2`) even with split bodies, and portable ran past 47 min at 23 GB. Now:

- `scripts/fft_codegen_split.py` cuts each tile body into non-inlined internal parts of at most 150 lines (the values
  that cross a cut go through a slot array; no operation moves, so every level keeps its bits) and leaves the header
  with the declarations. The definitions go to codelet units balanced by line count.
- `gen_fft_batched.py` runs the split itself and writes `batched_codelets_gen.hpp` plus
  `kernels/fft_batched_codelets_0..6.cpp`; `--check` compares all eight. On WSL (the reference host) it reproduces the
  committed bytes; `check-generated --regenerate` runs it. Against HEAD's header the 92,200 value definitions and the
  185 distinct constants are identical.
- The portable level takes the lane-scalar edition, as the generator comment allows (bit-identical: the same schedule
  per column).
- `hier_codelets.hpp` (generator-absent) was split once, from HEAD's bytes, into `kernels/fft_hier_codelets_0..1.cpp`.
- One call per codelet covers a batch. `CRD_FFT_GEN_INLINE`, `CRD_FFT_CODELET_INLINE` and `codelet_policy.hpp` are
  retired (deleted through `project-sync.py edit`); the canonicalizer annotates `void codelet*` definitions.

win-debug: crd-hesap-fft-tests built in 75 s and `[kernels]` passes (2,919 assertions).

## Measurements

win-release, each unit compiled directly (no launcher), four at a time like the hosted runner; wall time and peak
working set of cl.exe:

| Unit | portable | sse2 | avx2 | avx512 |
|---|---|---|---|---|
| fft_batched_codelets (each of 7) | 18-44 s, 0.21-0.25 GB | 50-63 s, 0.80-1.03 GB | 2.7-3.2 s, 0.13 GB | 2.7-3.7 s, 0.13 GB |
| fft_hier_codelets (each of 2) | 140-207 s, 10.8-10.9 GB | 16-19 s, 0.60-0.63 GB | 1.6 s, 0.10 GB | 1.6 s, 0.10 GB |
| fft_kernels.cpp (the engine) | stopped at 11.5 min, 32.2 GB | 480 s, 4.61 GB | 9.7 s, 0.44 GB | 8.6 s, 0.45 GB |

`/d2cgsummary`:

- sse2 engine: `radix32_pass<f32>` 148 s (166,091 instructions) and `<f64>` 101 s (297,859), `twiddle32_fwd/inv<f32>`
  89 s each (83,000), `<f64>` 47 s each (146,000). twiddle32 is codelets.hpp's force-inlined scalar template.
- portable hier unit 0: the f32 parts of `codelet64_stage1_fused_64x32` at 44 to 75 s each (15,000 to 20,000
  instructions), 88 s elapsed over 506 s of codegen threads; the Vec8f arrays.

The codelets are solved at sse2, avx2 and avx512. The engine unit at sse2 and portable and the portable hierarchical
units are not: a hosted runner has 16 GB.

## Combine codelets and lanes (HEAD before: `3952d310`)

CI first: push run 40623100798's failures were the groups above; its win-shipping and win-relwithdebinfo
`test_type.cpp`/`test_builder.cpp` heap errors (C1060, C1076, C3859) came from the fft units filling the host, and
the Linux release lanes' exit 143 sat in the same phase. No completed run since; no Needs CI row changed.

- `gen_fft_codelets.py [--check]` writes the leaf codelets (`codelets.hpp`, unchanged), and the radix-8/16/32 combine
  codelets as per-type functions declared in `twiddle_codelets.hpp` and split into
  `kernels/fft_twiddle_codelets_0..1.cpp`.
  The 7,744 operation and store lines of the old templates reappear in the same order (renamed values, `t` for `k`).
  The scalar tail of the last `r % W` columns keeps its two-rounding products.
- `codelet_lanes.hpp` owns `CRD_FFT_GEN_SIMD_BODY` and `lanes::Lane<T>`: Vec4d/Vec8f, or the scalar itself with
  `libm::fma` on portable and in unoptimised builds. A lanes codelet's tile loop runs its body once per Lane step, so
  every column of a tile computes the same operations. `fft_codegen_split.py --lanes` re-split the hierarchical header
  from 20c9f2a1^ onto the lanes; without `--lanes` the splitter still reproduces the committed units byte for byte.
  A part that writes no slot reads them through a const pointer (25 tidy findings on the scalar lanes).
- win-release, each unit alone, wall and peak working set: engine unit sse2 1.8 s / 0.26 GB, portable 6.8 s / 1.37 GB
  (`execute_ip4aos<f32>` 3.6 s the longest function), avx2 1.6 s; combine units sse2 5.2-5.8 s / 0.67 GB, portable
  0.3 s; hierarchical unit sse2 13.9 s / 0.60 GB, portable 0.8 s / 0.11 GB.
- crd-hesap-fft-tests pass on win-debug and on win-release forced to portable, sse2 and avx2 (the kernels cases compare
  every level with portable's bits).

## Left

The 512-bit types and the zmm bodies.
