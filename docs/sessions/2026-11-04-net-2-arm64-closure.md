# 2026-11-04: NET.2, crd-crypto's closure on the ARM64 lanes (Track C batch 93)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Design: [P3](../design/network-foundation.md#provider).

## CI first

Complete-tier dispatch 41754591577 and push run 41754581673 (both at f387ced1) were still queued: nothing to audit.

## What landed

No ARM64 lane built crd-crypto: its closure reached crd-jobs, which has no ARM64 fiber context before JOBS-PORT.2,
along two edges that were wrong ownership, not real needs.

- **crd-platform-async** (capability `io.async-reads`, ADR-0041's amendment): `AsyncFile` left crd-platform, which
  links no job system since. The raw positioned read both use is `crd/platform/detail/range_read.hpp`; header and
  namespace are unchanged, and the module stays outside the relocatable package (its diagnostic family reads no file
  asynchronously). crd-resources and crd-diag-overhead link it.
- **crd-json** (capability `text.json`, namespace `crd::json`): the JSON reader and writer left crd-asset-io (whose
  geometry importers reach crd-geometry-bvh's parallel build and crd-jobs). ceridc, its tests, the GPU diagnostic tests
  and the crypto vectors link it instead; crd-asset-io's glTF and OTIO code read and write through it.
- **ARM64 presets**: `CRD_MODULES_WITHOUT=jobs,gpu-context-vulkan,platform-window`. The two new names keep the lanes
  off what they cannot provision yet (no Vulkan SDK or Wayland development files), which crd-jobs used to hide. The
  set the lanes gain: json, platform, config, comply, crypto (AWS-LC, tpm2-tss and pkcs11 headers, the vectors) and
  kir_autotune, with their tests; nothing they built before is dropped.

## Evidence

- WSL linux-gcc-debug with the ARM64 exclusion (`~/cerid-nojobs-build`): the module closure went from 36 to 42 of
  112 modules, crypto among them; the tree built and its CTests passed (see the row for counts).

## Next

Audit 41754591577 and the complete-tier dispatch at this batch's head: win-arm64-debug's configure line
`[crd] AWS-LC's ARM64 assembly: ... --target=aarch64-pc-windows-msvc`, and crd-crypto-tests on win-arm64-debug,
linux-gcc-arm64-debug/-asan and linux-clang-arm64-debug/-asan. Their pass closes NET.2.
