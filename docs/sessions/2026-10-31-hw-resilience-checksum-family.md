# 2026-10-31 — HW-RESILIENCE.1: the foundation checksum family

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), its first item: the one dispatched checksum family that
replaces the three CRC-32 copies (OWN.1's duplicate record, the [delivery plan](../design/delivery-plan.md)'s owner).

## The dispatch mechanism moves to crd-core

The family belongs beside `crd/containers/hash.hpp`. crd-containers sits below crd-math, and crd-math's headers
include crd-containers, so a containers family could not use crd-math's dispatch.

- `crd/math/kernel_level.hpp` is now `crd/core/kernel_level.hpp`; its macros are unchanged.
- `KernelLevel`, `select_kernel_level`, `process_kernel_level` and `kernel_level_cap` moved to
  `crd/core/kernel_dispatch.hpp`, in namespace `crd`. crd-core takes the cap privately from CrdSimd.cmake, so it
  takes none of crd-simd-flags' ISA or floating-point options. `crd/math/kernel_dispatch.hpp` keeps the kernel
  determinism mode.
- ADR-0134 §9 is amended. `CRD_MATH_LEVEL_NS` keeps its name; renaming it would touch every kernel header for no
  behaviour change.
- The refactor commit changed four generated FFT headers' include lines without refreshing
  `scripts/generated-sources.json`, which the tooling suite caught. The FFT generators reproduce the new bytes
  (`--check` on WSL, the reference host) and the manifest is refreshed.

## The family

`crd/containers/checksum.hpp`: `crc32` (CRC-32/ISO-HDLC), `crc32c` (Castagnoli) and `xxh3_64` (XXH3-64 of the
xxHash 0.8 specification, default secret, seeded). The CRCs chain: `crc32(b, crc32(a)) == crc32(a ++ b)`.

- **crc family:** portable slicing by 8, sse4.2 with the CRC32 instruction for CRC-32C, and the carry-less multiply
  folds (Gopal et al., Intel 2009) as variants: sse4.2 + PCLMULQDQ (four 128-bit lanes), avx2 + VPCLMULQDQ (four
  256-bit registers) and avx512 + VPCLMULQDQ (four 512-bit registers). The fold and Barrett constants are computed
  from the polynomial at compile time and checked against the published CRC-32 constants.
- **hash family:** XXH3's long-input loop at portable, sse2, avx2, avx512 and neon; the short inputs and the merge
  are general code.
- PCLMULQDQ is in no psABI level, so `crd_add_cpu_kernels` now takes a variant whose level has no plain table, and
  `kernel_level.hpp`, the ISA isolation guard (PCLMULQDQ allowed in the sse4.2 variant) and ADR-0134 §1 name the
  new variants. An avx2 or avx512 CPU without VPCLMULQDQ runs the sse4.2 fold.
- `crd/core/wide_multiply.hpp` is the one 128-bit product of general code; crd-time's clock scaling uses it too, and
  its portable branch is now exact.
- Deleted: `perf/src/bundle.cpp`'s table, `resources`' `png_crc32` (PNG decode and encode and ZIP now call
  `crc32`; the encoder chains the chunk type and payload without a copy) and `hesap-tensor`'s `iodetail::crc32`. The
  perf tests and the bundle fuzz harness drop their own CRC copies too.

ARM64's CRC32 and PMULL instructions are optional ARMv8.0 features crd-core does not detect yet, so neon runs the
portable CRCs; that stays on the row.

## Verification (win-debug, i9-14900K)

- `crd-containers-tests [checksum]`: the check values; 40 reference vectors from 0 to 65,537 bytes (XXH3 from
  libxxhash 0.8.2 on WSL through ctypes with seeds 0, 1 and 0x9E3779B97F4A7C15, CRC-32 from Python's zlib, CRC-32C
  from a bit-serial reference); chaining at every split of 1,000 bytes; every crc level against a bitwise register
  at every length to 1,100 (all 64 alignments at the step boundaries) and up to 1 MiB; every hash level against the
  portable accumulators at every length from 241 to 1,400 and up to 1 MiB, with guard words around the accumulators.
- Levels run: portable, sse4.2, sse4.2 + PCLMULQDQ and avx2 + VPCLMULQDQ natively, each forced arm, and avx512 under
  SDE 10.13.1 (`-icx` ran avx512 + VPCLMULQDQ; `-skx` ran the hash at avx512).
- Teeth: a wrong 1024-bit fold distance and a wrong avx2 scramble shift each failed the differential cases (and the
  vectors); both restored, rebuilt and rerun green.
- Consumers: crd-perf-tests, crd-resources-tests, crd-hesap-tensor-io-tests, crd-time-tests, the cooker texture
  cases, the bundle fuzz corpus replay and the Vulkan textured glTF case passed; the ctest guards (the ISA isolation
  guard judged all nine checksum units), tidy, the format oracle, `check-generated.py` and the tooling suite pass.

Hosted CI has not run any of this: the GitLab to GitHub mirror is still at d9fdeae3.
