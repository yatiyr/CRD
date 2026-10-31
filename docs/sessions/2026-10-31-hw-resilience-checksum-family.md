# 2026-10-31 — HW-RESILIENCE.1: the dispatch mechanism moves to crd-core

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1).

## Why the move

The row's checksum family belongs to the foundation hashing owner beside `crd/containers/hash.hpp`
([delivery plan](../design/delivery-plan.md), OWN.1's duplicate record). crd-containers sits below crd-math, and
crd-math's headers include crd-containers, so a containers family could not use crd-math's dispatch mechanism.

## What changed

- `crd/math/kernel_level.hpp` is now `crd/core/kernel_level.hpp`; its macros are unchanged.
- `KernelLevel`, `select_kernel_level`, `process_kernel_level` and `kernel_level_cap` moved from
  `crd/math/kernel_dispatch.hpp` to `crd/core/kernel_dispatch.hpp`, in namespace `crd`. crd-core takes the build
  cap privately from CrdSimd.cmake, so it takes none of crd-simd-flags' ISA or floating-point options.
- `crd/math/kernel_dispatch.hpp` keeps the kernel determinism mode and includes the core header.
- ADR-0134 §9 is amended. The macro `CRD_MATH_LEVEL_NS` keeps its name; renaming it would touch every kernel header
  for no behaviour change.

Verification: crd-core, crd-math, crd-math-tests and crd-hesap-stats-tests built on win-debug, and their dispatch
and level cases pass.
