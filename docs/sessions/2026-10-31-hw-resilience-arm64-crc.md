# 2026-10-31 — HW-RESILIENCE.1: ARM64 CRC32 and PMULL

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), the checksum family's first Left item after the
[family landed](2026-10-31-hw-resilience-checksum-family.md): the ARM64 CRC bodies and their feature detection.

## Detection

CRC32 and PMULL are optional in ARMv8.0, so they are kernel-variant features, not part of the neon level.

- `CpuFeature::Crc32` and `CpuFeature::Pmull` follow the x64 features in the one feature table, as bits of an ARM64
  hwcap word in Linux's `AT_HWCAP` layout (`HWCAP_PMULL` bit 4, `HWCAP_CRC32` bit 7).
- `read_arm64_hwcap()` returns that word on every OS: Linux and Android read `getauxval(AT_HWCAP)`; Windows sets the
  bits from `PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE` and `PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE`, its one flag for
  AES, PMULL and SHA; macOS reads `hw.optional.armv8_crc32` and `hw.optional.arm.FEAT_PMULL`. It returns 0 off ARM64.
  `decode_arm64_hwcap()` decodes it, mirroring `decode_cpuid()`, and `detect_cpus()` uses both on ARM64. The OS
  reports the features every CPU has, so no CPU walk is needed.
- `cpu_level_features(Neon)` stays empty. A forced `neon` arm runs as the smallest neon CPU, without either feature,
  and the lane's unforced run takes the variants.

## Kernels

- `crd_add_cpu_kernels` gains the `neon+crc32` and `neon+pmull` variants. Their flags are `-march=armv8-a+crc` and
  `-march=armv8-a+crc+aes`; cl needs none. The pmull variant selects on CRC32 too, as VPCLMULQDQ implies PCLMULQDQ.
  `kernel_level.hpp` names them `neon_crc32` and `neon_pmull`.
- ARM64's CRC32 instructions compute both polynomials, so the crc32 variant runs both CRCs on `__crc32d`/`__crc32cd`.
- The pmull variant runs the PCLMULQDQ fold. That fold is now written once over eight 128-bit primitives (make, load,
  xor, and, the low and high carry-less products, a byte shift, one 32-bit lane), with an x64 and an ARM64 body each.
  `reduce128` takes its constants in the low lane, so it needs no mixed-half product.
- `vmull_p64` differs by compiler. ACLE takes two `poly64_t` scalars and returns `poly128_t`; cl's macro takes and
  returns vector registers (`neon_pmull_64(__n64, __n64)`). Only the low product branches on it.
- The checksum dispatch lists both tables after portable, with their feature sets inline. Named constants would be
  unused on one of the two architectures.

## ISA isolation guard

- `arm64_above` now reports PMULL and PMULL2 to 128 bits (the `.1q` arrangement), in objdump's and dumpbin's
  spelling. Before this, neither the floor nor a kernel's ceiling saw FEAT_PMULL. The 8-bit polynomial form is base
  NEON and stays allowed.
- ARM64 kernel objects were judged with no allowance at all. They now go through `judge_arm64_kernel_code`: neon may
  hold nothing above the v8.0 floor, a crc32 variant may also hold the CRC32 instructions, and a pmull variant may hold
  CRC32 and PMULL.
- `test-repository-tools.py` covers each branch.

## Verification

- win-debug (i9-14900K): `crd-containers-tests [checksum]` unforced and at portable, sse4.2 and avx2, and at avx512
  under SDE 10.13.1 `-icx`. The x64 folds run the rewritten 128-bit path, so their differential cases cover the shared
  algorithm. `crd-core-tests [cpu]` passed.
- The new decode case has teeth: with `Crc32` moved to bit 6 it fails twice; restored, rebuilt and green.
- No aarch64 sysroot exists on this host. WSL clang 18 with `--target=aarch64-linux-gnu` compiled `crc_kernels.cpp`
  at `-O2 -Wall -Wextra -Werror` as the crc32 variant, the pmull variant and the portable unit, using freestanding
  stand-ins for four standard headers. It refused the crc32 variant without `+crc`.
- `llvm-objdump` of those objects shows `crc32x/cx/b/cb` in both variants and `pmull`/`pmull2 .1q` in the pmull one.
  Fed to the guard, each object passes at its own level and fails as plain `neon`.
- `cpu_features.cpp`'s ARM64 branches and cl's ARM64 build are first compiled on the hosted lanes. So are the
  variants' differential runs on real hardware: Neoverse-class runners with CRC32 and PMULL, in linux-arm64 and
  windows-arm64. That hosted evidence stays on the row.
- Tidy, the format oracle, the Allman check, actionlint (ci.yml comment) and the ctest guards pass.
