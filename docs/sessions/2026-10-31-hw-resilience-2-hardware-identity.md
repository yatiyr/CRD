# 2026-10-31 — HW-RESILIENCE.2: the hardware identity

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2), its first part: bundles record the CPU model, microcode,
BIOS and memory configuration. Design: [runtime diagnostics](../design/runtime-diagnostics.md#hw-resilience--hardware-fault-resilience).

## Boundary

The bundle format and `bundle.inspect` are DIAG's (this track). [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1)
(track C) consumes the identity in its reproduction pack, and [CRASH-FIX.2](../ROADMAP.md#slice-crash-fix.2), which
took the crash service from OPS.1, owns the clustering by CPU model and microcode; that clause is relayed to track C.

## CPU identity (crd-core)

`cpu.detection` owns every CPU query, so the identity sits beside the features in `cpu_features.hpp`:
`read_cpu_identity()` (vendor, model, signature, microcode), `x64_signature_parts()`, the MIDR implementer and part
names (a table: Arm's Cortex and Neoverse parts, Ampere, Qualcomm Oryon) and `windows_update_revision()`. This host's
registry holds `Update Revision` as 4 bytes (`33 01 00 00`, microcode 0x133); older Windows wrote 8 bytes with the
revision in the high word, and both forms decode. Hosted Linux VMs report microcode `0xffffffff`, which counts as
unknown. crd-core now links advapi32 by name for the registry reads.

## Hardware identity (crd-perf)

`crd/perf/hardware_identity.hpp`, bundle section 8 (`CHW1`), capability `diagnostics.hardware-identity` (owner perf;
signature: a firmware table or DMI read). One SMBIOS walker serves Windows' `RSMB` table and Linux's DMI table:
types 0, 1 and 2 for firmware, system and board, type 17 for each populated module (the 16-bit size with its KiB bit
and the 0x7FFF extended size, the 0xFFFF extended speeds), type 127 as the only proof the module list is complete.
GPU entries are in the format; filling them from the GPU contexts is the next part. The section codecs of the build
identity, the flight record and the hardware identity now share `src/payload_codec.hpp`; the two copies of the
little-endian helpers and cursor are deleted.

## Verification (win-debug, linux-gcc-debug build)

- `crd-core-tests [hw-resilience]`: signature parts (Raptor Lake, Zen 4, family 0xF, family 5), MIDR names, both
  registry layouts; this CPU against the OS's own description (Windows: `VendorIdentifier`, `ProcessorNameString` and
  the `Identifier` family/model/stepping; Linux: `/proc/cpuinfo`, the microcode included).
- `crd-perf-tests [hw-resilience]`: a desktop table (empty slot skipped, extended size and speeds, padded strings
  trimmed), every cut of it (never complete), hostile structures, the codec's round trip and refusals at every cut,
  the encoder's clipping, the field comparison, and this machine. The flight bundle and `bundle.inspect` and
  `diag.capabilities` cases check the new section and items. The whole `crd-perf-tests` passes.
- Both host cases print an evidence line on every run, so hosted lanes record their machines.
- linux-gcc-debug (WSL): both targets build warning-free; the cases pass. WSL exposes no DMI and hides the
  microcode, so its machine reads as memory and CPU only, every other part unknown.
- Teeth, each restored, rebuilt and rerun green: a walk that calls a cut table complete failed every cut; dropping
  the KiB size's rounding failed the hostile-structures case; reading the low registry word always failed the
  8-byte layout.
