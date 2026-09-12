# Cerid shared memory — every agent

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

This is the portable lesson index. **Current rules: [AGENTS](AGENTS.md). Current work: [context](context.md) →
[ROADMAP](docs/ROADMAP.md).** Memory stores lessons, not another plan or an active loop grant.
Universal entry: [START_HERE](START_HERE.md). Broader product/quality obligations:
[system review](docs/research/2026-09-12-cerid-whole-system-review.md) and [quality contract](docs/design/system-quality-contract.md).
The former host-specific index and all 457 source records were preserved in the reference corpus below.

## Read only what the task needs

- Build, stale executables, PCH, sanitizer or tidy symptoms → [build/verification](docs/lessons/memory/build-and-verification.md).
- CEIR/CHIR compiler, optimizer or execution semantics → [execution IR](docs/lessons/memory/execution-ir.md).
- CKIR, GPU providers and emitted programs → [device programs](docs/lessons/memory/device-programs.md).
- Frames, materials, lighting, reload and image correctness → [rendering](docs/lessons/memory/rendering.md).
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
No hidden host-only memory is required for normal repository work. Tool-specific memory entry points redirect here.
