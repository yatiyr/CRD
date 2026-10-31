# 2026-11-01 — HW-RESILIENCE.2: the hosted runners' identity, the tidy lane's kernel ISA and linux-gcc-asan's bound

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2). Before: [classification](2026-10-31-hw-resilience-2-classification.md).

## CI read

Push run 41231558128 (d5cc2ec0, still running) failed 18 jobs. Fixed on main before this batch: the GREASE compare
(2782ab9e), the journal's `ftruncate` result (be0adf3f), the ARM64 MIDR shadowing (1667ad60), the wire writer's
overflow under GCC's LTO (f635efb1) and the real-binary comply command (ee040d0f). Two were still live:

- **cpu identity, hardware identity** (every Windows x64 lane). The hosted runner is a Hyper-V guest on an EPYC 7763:
  its `ProcessorNameString` keeps the CPUID brand string's trailing spaces, which crd-core trims, and its registry
  holds no 4- or 8-byte `Update Revision`, so the microcode is honestly unknown. The tests assumed an unpadded
  name and a microcode on every Windows host. The core case now trims the registry text and reads
  `Update Revision` itself (known exactly when it is 4 or 8 bytes and not all ones, equal to its decoded word);
  the hardware-identity case
  holds the bundle's microcode to crd-core's reading. Teeth on win-debug: a crd-core that reports unknown fails the
  core case; an identity that drops the flag fails the bundle case.
- **win-tidy** (`simd_level_probe.cpp`: "crd-core's FMA report disagrees"). clang-tidy drops every `/`-spelled flag
  of the command CMake hands it, so cl's avx2 and avx512 units were analysed without `/arch` while
  `CRD_KERNEL_LEVEL` arrived; the probe's check from 97268ef3 exposed it. `_crd_add_kernel_unit` now restates each
  level in clang-cl's spelling through `--extra-arg` (`/arch:AVX2`, `/arch:AVX512`, `/clang:-march=x86-64-v2`;
  cl's `/arch:SSE4.2` is no clang `/arch`). The reconfigured win-tidy tree analysed all 77 kernel units clean;
  a temporary `#error` marker proved the analysis runs there and sees `__AVX2__`. `tidy-files.py` was not wrong:
  through the database clang keeps `/arch`.

Complete-tier run 41215667488 (c2645814) passed every job but linux-gcc-asan, stopped at the 6 h cap after 6,811 of
8,766 cases in 20,331 s of serial tests (about 26,000 s for all). Its test preset now runs three cases at a time, as
win-asan does ([ci-tiers](../design/ci-tiers.md#parallelism-and-shards)).

Needs CI audit: HW-RESILIENCE.1, DIAG.11c and DIAG.12 need a complete-tier run after this push (none at or after
HW-RESILIENCE.1's close-out exists). Account lanes: the Firebase Test Lab names are present; AMD and Mac cloud
lanes wait on their accounts.

## Checks

win-debug: the two cases, with teeth; the ctest guards; win-tidy: the 77 kernel units; `tidy-files.py` and
clang-format on the changed tests; the brace, tier, plan and repository checks.
