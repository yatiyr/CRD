# Cerid shared memory — every agent

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

Portable lessons; [context](context.md) owns current work. Entry: [START_HERE](START_HERE.md).

## Read only what the task needs

- Earliest unfinished work; explicit user overrides; retain CI waits while continuing → [order](docs/ROADMAP.md#strict-sequential-execution).
- Scoped local checks, broad CI, human-only publication → [BUILDING](docs/BUILDING.md#fast-local-workflow--one-primary-configuration).
- Native VS configuration names require matching flags, headers, CRT and ISA; presets own the matrix →
  [configuration contract and evidence](docs/design/visual-studio-configurations.md).
- Test ownership, sync readiness and decoder outcomes →
  [scoped verification recipe](docs/recipes/2026-09-12-affected-build-selection.md).
- Build, stale executables, PCH, sanitizer or tidy symptoms → [build/verification](docs/lessons/memory/build-and-verification.md).
- DX12 startup-to-teardown capture and checked fences →
  [validation/lifetime recipe](docs/recipes/2026-09-12-dx12-validation-and-command-lifetimes.md).
- CEIR/CHIR compiler, optimizer or execution semantics → [execution IR](docs/lessons/memory/execution-ir.md).
- CKIR/providers → [device programs](docs/lessons/memory/device-programs.md); expression growth/live loads →
  [snapshot ordering](docs/recipes/2026-09-12-kernel-snapshot-ordering.md).
- Frames, materials, lighting, reload and image correctness → [rendering](docs/lessons/memory/rendering.md).
- DX12 cache/clear lifetimes → [consumer contracts](docs/recipes/2026-09-13-dx12-consumer-contracts.md).
- Oracles, precision, numerical kernels and benchmark methodology → [numerics/performance](docs/lessons/memory/numerics-and-performance.md).
- Scope, ownership, allocation and diagnostic discipline → [workflow/correctness](docs/lessons/memory/workflow-and-correctness.md).
- Older strategic scope and CEIR grant history → [project history](docs/lessons/memory/project-history.md).

Use `python scripts/check-master-plan.py --memory <symptom-or-record-name>` to locate a record without loading
the corpus. An exact record name prints that one record. The JSON lookup is generated reference metadata, not status.
For source details use [systems](docs/systems/README.md); measured boards live in [bench](docs/bench/README.md).

## Maintenance

Write new durable lessons with the rule, why and how to apply it in the appropriate reference/recipe. Keep this
index small. Link evidence from the owning ROADMAP row and session. Do not copy a Next list here. Retire stale
directions explicitly; historical dates or authorizations cannot override current rules or restart the CEIR loop.
Diagnostic gaps and detector controls: [DIAG](docs/design/runtime-diagnostics.md). Tool memories redirect here.
