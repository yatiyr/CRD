# 2026-10-28 — SIMD-DISPATCH: the avx512 level of every family and the pinned SDE arms

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `f57ced95`.

## CI first

- No completed run has a failed job that needs a fix: push runs 40586446192 to 40596800317 were cancelled before any
  job failed; push run 40581144513 (`59c79f2f`) fails on the `GemmBlock` `beta` initializers `90a87169` fixed, and its
  win-release, win-shipping, win-shipping-profile and win-relwithdebinfo jobs have run since 21:08 (see the fft
  finding: the cause of those hours and of complete run 40515770381's six-hour win-relwithdebinfo). Complete run
  40592200636 is queued. No row is Needs CI; DIAG.11c and DIAG.12 wait on a finished complete-tier run.

## The avx512 level of every family

- All eleven families list `avx512`: one more unit at `/arch:AVX512` or `-march=x86-64-v4`, its table
  `k<Family>KernelsAvx512` and its dispatcher entry after avx2 (the tensor entry after the AVX-VNNI variant).
  `CRD_SIMD_HAS_AVX2` holds at avx512, so the units compile the avx2 bodies, bit-exact with them; the 512-bit types
  and bodies follow. Each family's presence case checks the avx512 table; hesap-direct gains the case it lacked.
- Hosted runners with AVX-512 now select these tables in every suite; a failure there is a v4 codegen defect.

## Intel SDE arms

- `scripts/install-sde.py` installs the pinned kit (`tools/sde-windows`, `tools/sde-linux`: Intel's URL and published
  SHA-256, matched by the archives the user approved). It extracts everything below the pin's root through tarfile's
  `data` filter (the Linux kit's six inner links kept, Pin's loader among them; a member outside the root or an
  absolute or escaping link refused), stages beside the destination, checks `-version`, stamps the install and never
  replaces a directory without its stamp. Here (Windows archive) and on WSL (Linux archive) the kit reports 10.13.1.
- `CRD_SDE` (cache, from the environment at configure) makes `crd_add_kernel_level_tests` add
  `<suite>-kernels-at-avx512-sde-skx` (`sde -skx -- <exe> <filter>,[cpu-level-applied]`, `CRD_CPU_LEVEL=avx512`,
  TIMEOUT 1800). skx is the level's smallest CPU; the spr arm comes with the avx512 VNNI variant, gnr and dmr when a
  body selects on AVX10 or APX.
- A refused level ran the detected one, so an emulated arm whose level SDE did not expose would pass on avx2 tables.
  The hidden `[cpu-level-applied]` case (`tests/support/kernel-levels/`, linked into every kernel suite, not listed by
  discovery) fails unless `CRD_CPU_LEVEL` is the selected level: native avx512 on this i9-14900K fails it
  (`above-hardware`), `sde -hsw` fails it, `sde -skx` passes (detected and selected avx512).
- Lanes: win-release and linux-gcc-release (`Install pinned Intel SDE`, mapped in `.github/ci-tiers.json`; the Linux
  step sets Yama's ptrace scope 0, as Intel's README requires). On win-debug the 13 other SDE arms passed but the fft
  arm timed out at 1800 s (its native debug arm takes 212 s, direct's SDE arm 688 s); on win-release every SDE arm but
  fft's (not built, below) passes in at most 24 s, and all 78 non-fft kernel arms pass.

## Findings fixed

- Kernel units no longer compile to link-time IL (`INTERPROCEDURAL_OPTIMIZATION OFF`, ADR-0134 §5 and §6 amended).
  Entered only through their tables, they gain nothing from LTCG, yet every image linking an archive regenerated its
  units' code: the LTCG link of `crd-hesap-fft-fatal-specimen` on win-release passed 42 GB here before it was stopped.
- MSVC 14.51 folds a vector add of a constant +0 to its other operand (`_mm_add_pd(_mm_setzero_pd(), x)`,
  `_mm256_add_pd`, `_mm_add_ps`, either order; a scalar `0.0 + x`, `_mm_sub_pd(x, -0)` and `0 - x` are kept). The
  GEMM `Zero` merge (`+0 + alpha p`) therefore kept -0 on win-release: sse2 always, avx2 once the unit was compiled
  outside LTCG (2,836 to 3,382 wrong elements per case, all signed zeros). It now computes `alpha p - (-0)`, the same
  IEEE value; dense and direct `[kernels]` pass at every level on win-release and win-debug.
- The GEMM tile prefetched the next B panel through an integer address (tidy `performance-no-int-to-ptr`): the block
  now passes the next tile's panel (`b_next`, the first panel after the last), so the hint is a valid pointer.
- `test_dense_kernels.cpp` nested a conditional (tidy). The 36 changed files are tidy-clean (one clang-tidy crash on
  the first pass, clean on the rerun).

## Tidy needs no per-unit ISA flag

The scout brief held that win-tidy analyses avx512 units under `/arch:AVX2`, so the first `_mm512` call would fail.
clang reports an always-inline intrinsic's missing target feature in code generation, which clang-tidy never runs:
`_mm512_add_ps` and `_mm256_dpbusd_avx_epi32` analyse clean under `--extra-arg=/arch:AVX2` (LLVM 20.1.8), and hosted
win-tidy 122241968105 passed with the VNNI unit's dpbusd. The bodies gate on `CRD_KERNEL_LEVEL`, a `-D` that reaches
the analysis, so every level's branch is analysed.

## Open: the fft units' code generation

On win-release (`/O2`) one `fft_kernels.cpp` unit takes about an hour: avx2 and avx512 finished after 57 to 58 min
(1.3 to 4 GB), portable and sse2 had not after 60 min (up to 25 GB and 31 GB). That is why the hosted Windows LTCG
lanes run for hours. The 240,462-line generated codelets are straight-line bodies scheduled for 16 ymm registers; on
sse2 each `Vec4d`/`Vec8f` is two registers and on portable four-element arrays. The next batch measures
`/d2cgsummary` on the sse2 unit and gives each level code it can generate.

## Left

The fft code generation above; the 512-bit `Vec16f`, `Vec8d` and `Vec16i` types with masked partial loads and their
probe; each family's zmm body; whether other kernels start a vector sum from a constant +0 (the MSVC fold); the
`-Wpsabi` notes.
