# 2026-11-06: HESAP-NET, the erasure code against ISA-L's assembly

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 99.
Board: [erasure against ISA-L](../bench/2026-11-06-hesap-net-erasure-vs-isal.md).

## CI first (all at 0294cdef, all in hosted runs, pushed first as three commits)

- linux-clang-arm64-asan, every run: crd-check-provider-symbols read Clang's `__odr_asan_gen_<name>` as 172 unprefixed
  AWS-LC symbols. It is now judged by the name it derives from.
- win-debug, three of four runs: cancelled at the 300-minute cap inside the sse2 arm (serial suite 12,508 s, arm about
  110 min). Its test presets now run at 3 jobs, as win-asan runs the same tests; the bound table has its row.
- crd-ci-orchestrator-e2e (41797302943, win-shipping-profile): `store path escapes the store`. From ntpath.realpath's
  source, a file replaced between its two lookups keeps the `\\?\` form; not reproduced, a unit test holds the mechanism.
- win-asan once (41797302943): `Provider: one verifier and one key serve several threads` failed one check of 32 rounds.
  No lazily initialised or mutable state on that path (read); not fixed. The test now reports each thread's first fault
  (iteration, CryptoError, Verification) after the joins, so a repeat names the call; a cause in AWS-LC goes to the register.

## What landed

- **Peer:** `crd-isal-peer`: ec_base.c plus ISA-L's own x86-64 source list on x86-64, assembled by the pinned NASM
  (`tools/nasm-linux-x64`, scripts/install-nasm.py, CI step; nasm.exe on Windows). The oracle holds every ISA body and the
  dispatch (`isal_arms`) to hesap's bytes, not the portable one alone.
- **Board:** `crd-hesap-erasure-board` (`--quick` is the smoke CTest): encode, cold encode, decode at 1200, 1500, 64 KiB, 1 MiB.
- **The board's first run lost 2x on datagrams; fixed, not recorded:**
  - a remainder went byte by byte (16 bytes x 40 coefficients): now the last whole vector again, or padded vectors;
  - new `avx2+gfni` and `avx512+gfni` variants of `hesap.erasure` (GF2P8AFFINEQB, `GfCoefficient::affine`);
  - coefficients come from a compile-time table instead of 32 multiplications each;
  - each pass over the sources covers several vector columns, so a coefficient is broadcast once for all of them;
  - `prepare_encode` and `encode_prepared` keep the rows across blocks, as an application keeps ISA-L's tables.

## Evidence

- Result: 1.2x to 1.6x ISA-L's dispatch on prepared encode (a tie at k=4, r=2), 1.3x to 13.7x on decode. One loss stays,
  cold encode at k=4, r=2 (0.78), explained on the board. avx512 rates are not measured (no such CPU here).
- linux-gcc-public-checks: 69 CTests (oracle, kernels, corpora, every `crd-no-`/`crd-check` guard, isa-isolation) pass.
  linux-gcc-release (LTO) passes. win-debug (MSVC, option on, NASM win64): `[erasure]` 1314 assertions, the oracle with all
  arms, the board smoke and the provider case pass; it found C4324 on the padded struct, fixed. The option is off again.
- SDE: `[kernels]` and the oracle at icx (Avx512Gfni, ISA-L's avx512 bodies) and skx pass; hosted arms gain `SDE_CPUS icx`.
- Teeth, each caught by `[erasure]`: tail not re-covered, padded column losing accumulate, prepared rows at the wrong
  point, bit matrix reversed, a wide pass reading one column twice, the wide loop overrunning (abort under its guard).
- libFuzzer `hesap-erasure` (now also the prepared path): 240 s at 2 jobs, 83,622 executions, no finding.

## Next

OpenFEC's RFC 5510 oracle; FlexFEC and RED against libwebrtc; RaptorQ; the estimators and interpolation kernels; the real
avx512 throughput on the smallest spot machine with other avx512 rows.
