# 2026-11-01 — HW-RESILIENCE.3: known-answer self-checks of the kernel families

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[HW-RESILIENCE.2's decoder](2026-11-01-hw-resilience-2-aarch64-decoder.md).

## CI read

- Complete-tier run 41254049879 (`31404ec4`) is still running. Its clang-tidy job failed on
  `tests/foundation/platform/test_arguments.cpp` (track C's UTF-8 arguments, e18deed5): three escaped literals
  (`modernize-raw-string-literal`) and a concatenated literal read as a missing comma
  (`bugprone-suspicious-missing-comma`). Fixed in a40af1bf: raw literals, and an octal escape for the letter after a
  hex escape; the local LLVM 20 gate reproduced all four findings on the old file and passes the new one.
- Its linux-gcc-release and linux-gcc-shipping jobs failed on the `test_deflate.cpp` null dereference that 95ec6965
  already fixed after `31404ec4`.
- That tidy job stopped at the first failure, so nothing after it had a hosted strict-tidy pass since run 41215667488
  (`c2645814`). The local gate ran over all 263 C++ files changed since then: one finding, `kStageCount` in
  `crd/kir/ckir.hpp` unused when the header is analysed alone; it is now `inline constexpr`, as a namespace-scope
  constant in a header should be.
  Three package-consumer sources are outside win-debug's compile database (their own package build), so the gate
  could not analyse them there.
- Needs CI audit: no complete-tier run covers current main; 41247045494 and 41254049879 are still running and both
  end red on fixed causes, so HW-RESILIENCE.2, DIAG.11c and DIAG.12 are unchanged. A complete-tier run is dispatched
  when neither is running.

## Kernel self-checks

- Mechanism (`crd/core/kernel_dispatch.hpp`): `select_checked_kernel_level` walks the eligible tables from the
  selected one down, runs the family's `KernelSelfCheck` on each until one passes (portable last), and fills a
  `KernelCheckResult` (selected and running tables, status `passed`, `demoted` or `floor-failed`, the first failing
  routine, injected or not, tables failed). `process_checked_kernel_level` does it at the process selection from the
  accessor's function-local static. Each family is a `constinit` `KernelFamily` with a `KernelFamilyListing` beside
  it (an atomic list built at image start), so `run_kernel_self_checks` resolves every linked family.
- Seam: `install_kernel_check_fault_hook` (crd/core/fault_seam.hpp, compiled out of shipping) fails a named family's
  tables before its first use.
- Report: `report_kernel_self_checks` (crd-perf) runs at a diagnostic host's start and at an Application's without
  one, one stderr line per demotion; the doctor carries `kernel_families` and `kernel_demotions`;
  `diag.capabilities` adds `kernel_demotions` and a `kernel-check` item per family. `format_kernel_table` names a
  table ("sse4.2 +pclmulqdq") beside `format_cpu_selection`; the checksum test's own formatting is gone.
- Checksum families: `crc_self_check` runs CRC-32 and CRC-32C over the checksum tests' reference stream (1025 bytes,
  every fold loop and a tail; 63 bytes, the byte paths; one byte off alignment), `hash_self_check` XXH3-64 over the
  1025 bytes through the candidate's accumulate and the scalar merge. The answers are the tests' zlib, bit-serial and
  libxxhash 0.8.2 values.

## Tests

- `kernel self-check:` cases (crd-math-tests, beside the selection cases): a passing selection, a failing variant
  falling to its plain level and then to sse2, ineligible tables never checked, every table failing to
  `floor-failed`, the seam (one hook, other families untouched, uninstall; refused in shipping) and the table names.
- `checksum self-checks:` every table the host runs passes (a `[kernels]` case, so every forced arm and SDE runs it),
  and a table with one wrong routine fails in that routine (one result bit, and a wrong result only below 64 bytes).
- `doctor:` every linked family passed on the table it selected; a child process whose crc tables above portable
  fail through the seam keeps running on portable with the right CRC, prints the startup line and reports the
  demotion in the doctor; `diag commands:` the capabilities items.

## Evidence

- win-debug: the cases above pass; the doctor child resolved and checked every family its image links in 61 us and
  printed `[crd] kernel family containers.crc failed its known answers on avx2 +pclmulqdq +vpclmulqdq in injected
  (fault seam); it runs portable (demoted, 3 tables failed)`. Forced portable, sse2 and sse4.2 arms and SDE icx (crc
  `avx512 +pclmulqdq +vpclmulqdq`, hash avx512) pass the checksum self-checks.
- Teeth: a wrong CRC-32C known answer (`0x5DEDF5BA`) made every crc table fail (`crc32c`), the family resolve
  `floor-failed` and the process selection case fail; restored, rebuilt, passing.
- Guards (`crd-no-*`, `crd-check*`, diag coverage and seeded faults) pass; tidy and clang-format on the changed lines.

## Left on the row

Known-answer checks for the other twelve families, the device path, `ceridc doctor --hardware` and the state-hash
divergence check.
