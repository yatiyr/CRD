> **Dated phases reference.** Current order/status: [ROADMAP](../../ROADMAP.md). Read [AGENTS](../../../AGENTS.md) for current rules.
> Old Next lists, API examples and implementation claims below require current-source verification. Unique requirements remain in force through their owning master rows.

# Phase 1 — Foundations

<!-- doc-role: archive -->
> Historical reference; old status/schedules/grants are not current instructions. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> **Tracking consolidated 2026-09-12:** this file retains detailed requirements and dated evidence.
> Live order/status/subslices are only in [ROADMAP](../../ROADMAP.md#master-table). Earlier schedules and Next
> pointers below are historical; current ownership/sequence follows ADR-0129 and the execution contract.

**Status:** ✅ shipped (2026-04)

A working substrate where every module can log, allocate, do math, hold
containers, and talk to the OS.

## Slices

| Slice | Module / Topic         | Notes                                                                    |
| :---: | ---------------------- | ------------------------------------------------------------------------ |
| 1.0a  | `crd-core`             | types, platform, assert, build_config                                    |
| 1.0b  | `crd-log`              | levels, channels, sinks, sync/async dispatch, source_location capture    |
| 1.0c  | assert ↔ log bridge    | one-way: core defines hook, log installs handler                         |
| 1.0d  | `crd-memory`           | IAllocator + Malloc / Linear / Stack / Pool, alignment, stats            |
| 1.0e  | `crd-containers`       | Array, FixedArray, Span, String, RingBuffer, HashMap, HashSet            |
| 1.0f  | mid-phase evaluation   | scorecard + risk review (no code session)                                |
| 1.0g  | quality pass           | CI matrix, benchmarks, PCH, runtime split, clang-cl, tidy, assert E2E    |
| 1.0h  | `crd-math`             | scalar-first vector / matrix / quaternion / transform / geometry         |
| 1.0i  | `crd-platform`         | window, timer, input, filesystem, dynlib, threading helpers              |

## Decisions

- ADR-0001 — Build & language
- ADR-0002 — Logging
- ADR-0003 — Memory v1
- ADR-0004 — Containers v1
- ADR-0005 — Math v1
- ADR-0006 — Platform v1

## Long-range math slices (parked for Phase 4.1)

M0–M5 shipped (foundations + core geometry). M6 SIMD, M7 dense, M8 sparse +
iterative, M9 robust geometry — see Phase 4.
