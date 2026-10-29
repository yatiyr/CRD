# 2026-10-29 — SIMD-DISPATCH: an exact fma on x64 Windows, and the guard's image route in minutes

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `c6aaf4fe`. ADR-0134 §8 (amended).

## CI first

Push run 40784988994 (`92db578d`, still running win-release, win-asan, win-tidy, linux-gcc-asan and tsan) holds the
first hosted results of the forced levels' CRT lowering and of the ISA guard. Its failures beyond the causes
`0df22cdc`, `ccabba8b`, `dc67c9ef` and `c6aaf4fe` fixed:

- win-debug, win-relwithdebinfo, win-shipping(-profile), win-clang-cl and win-clang-cl-shipping: every
  `crd-math-`, `crd-hesap-direct-` and `crd-hesap-tensor-kernels-at-{portable,sse2,sse42}` arm. With FMA3 off the
  UCRT's software `fma` does not round once: `fmaf(0x957ff2a9, 0x167bdf6b, +0)` gave +0 for an underflowing
  negative product (exact: -0) and `fmaf(0x39800004, 0x407ffff8, 0xc66d8469)` returned c, a sum rounded twice
  (exact: `0xc66d8468`); f64 lost the same signs. The engine's portable bodies and every test oracle called it.
- The same Windows IL lanes: `crd-check-isa-isolation` timed out at 1,800 s. Not dumpbin (2.9 s for a 16 MB image):
  `_stream` read the pipe in text mode, and large reads of a Windows text-mode pipe crawl (16.5 s against 0.3 s in
  bytes for a 20 MB dump).
- linux-gcc-debug: zstd's fast Huffman decoders (`HUF_decompress4X{1,2}_usingDTable_internal_fast_c_loop`, `shlx`),
  built with `target("lzcnt,bmi,bmi2")` and reached only behind `HUF_flags_bmi2`.
- linux-gcc-public-checks: nm prints nothing for a header-check object that defines no symbol.

No completed run carries Needs CI evidence: DIAG.11c and DIAG.12 stay Needs CI.

## What changed

- `crd/math/libm.hpp`: `fma` rounds once everywhere. On MSVC and clang-cl x64 an avx2 or avx512 unit issues
  `vfmadd` itself; every other unit calls the UCRT only while its FMA3 bodies run (`crd::detail::ucrt_fma3_active`,
  written by crd-core's `.CRT$XCL` initializer after any lowering) and otherwise runs its own body: f64 by integer
  arithmetic (the 106-bit product and z aligned in 128 bits with a sticky bit, rounded to nearest even, subnormals
  and overflow included), f32 by rounding the exact f64 sum to odd. Elsewhere the C library's fma is kept.
- Engine `std::fma` calls (predicates, hesap-tensor's decomp, nn, sparse CP, TT) and the tests' oracles call
  `crd::math::fma`; `crd-no-std-transcendental-check` now rejects `std::fma` in engine code.
- `check-isa-isolation.py`: pipes are read as bytes and decoded per block (CR LF folded), 10x on win-release images
  with the same verdicts; nm's empty listing passes when no object of the call holds code; the
  `zstd-fast-huf-bmi2` exception names the decoders' runtime check.

## Checks

- win-debug and win-clang-cl `crd-math-tests [kernels]` at portable, sse2, sse4.2 and detected; win-debug direct
  and tensor `[kernels]` at portable and sse4.2: pass. The new case compares both bodies with the UCRT's FMA3 fma on
  2^20 hard triples of each type and checks the three CI cases.
- Guards (`^crd-no-|^crd-check|...`, 16) on win-debug, tidy on the 18 changed C++ files, the tooling tests (146).

## Left

The forced arms' first green run on every Windows lane; the guard's hosted runtime with the byte reads (it then sets
its timeout).
