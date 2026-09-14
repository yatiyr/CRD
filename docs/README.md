# Cerid documentation — one route from question to evidence

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](ROADMAP.md); current rules: [AGENTS](../AGENTS.md).

**[ROADMAP](ROADMAP.md) is the only live roadmap, slice table and bug/finding queue.**
Every human/agent starts at [START_HERE](../START_HERE.md). [Context](../context.md) names the row;
[MEMORY](../MEMORY.md) routes shared lessons.

## Find what you need

| Need | Read |
|---|---|
| Purpose, authorization, conduct and session ritual | [AGENTS](../AGENTS.md) |
| Current slice | [context](../context.md), then its one [master row](ROADMAP.md) |
| Architecture and ownership | [PRINCIPLES](PRINCIPLES.md), then the row's ADR |
| Bug diagnosis and evidence standard | [SANITY](SANITY.md) |
| Runtime diagnostics, allocator/race/crash/capture implementation | [DIAG contract](design/runtime-diagnostics.md) |
| Third-party defects and the registered-failure gate | [Register](third-party-defects.md) |
| Build/test/tidy commands | [BUILDING](BUILDING.md); style in [CODING](CODING.md) |
| Full renderer/UI/editor scope | [Execution contract](design/renderer-ui-execution-contract.md), [catalogue](design/rendering-ui-contracts.md) |
| Whole-system vision, classified gaps, AI/collaboration/science | [System review](research/2026-09-12-cerid-whole-system-review.md) |
| Cross-domain qualification and task closure | [Quality contract](design/system-quality-contract.md) |
| Source owner, dependencies, public API | [Systems/source map](systems/README.md) |
| Contribute a module, fix, asset/program or build change; publication handoff | [Contribution routes](CONTRIBUTING.md) |
| Specific implementation plan or decision | Row's link; reverse lookup in [design](design/README.md) or [ADRs](decisions/README.md) |
| Prior lesson or matching failure | [MEMORY](../MEMORY.md); query one record |
| What actually ran or was measured | Linked [session](sessions/), [benchmark](bench/README.md) or [recipe](recipes/README.md) |
| Old requirements or dated implementation examples | [Archive](archive/README.md); phase/system pointers preserve their route |

Useful commands from the repository root:

```powershell
python scripts/check-master-plan.py --next
python scripts/check-master-plan.py --slice RAH-1
python scripts/check-master-plan.py --find reload
python scripts/check-master-plan.py --find A24
python scripts/check-master-plan.py --memory stale_exe
python scripts/check-master-plan.py
```

Queries read the same table. The validator checks links, IDs/dependencies, completion evidence, size budgets and
[order](ROADMAP.md#strict-sequential-execution): context is the earliest available work; CI-only waits stay visible
without stopping it. No open work is silently skipped. Passing proves documentation structure, not engine correctness.

## Start a slice

Read AGENTS → PRINCIPLES → SANITY → MEMORY index → context → the exact ROADMAP row. Open its detail/ADR and
relevant source; read BUILDING/CODING before code. Name the purpose, inherited scope, prerequisites and evidence
needed. Start with the earliest unfinished work unless the user explicitly directs a future slice. Needs CI never
means Done; continue available work while retaining it. A query cannot start a loop or accept a design decision.

## Track a bug or finding

Search its ID, symptom and owning module first. Use the existing owner row where possible; otherwise add a stable
child ID to this same table. Record reproduction/configuration, expected/actual behaviour, harness checks, confidence
and the blocking acceptance condition in a linked note. **Review** means unverified concern/decision, not a confirmed
runtime defect. A reproduced failure prevents the owning slice from closing. Detail may live elsewhere; live state
and its owner live only in ROADMAP. The audit's A01…A27 IDs are searchable from their owner rows.

## Close and hand off

Applies after implementations, fixes, investigations and documentation tasks. Inspect all affected current documents;
correct changed facts, preserve historical evidence and leave already-correct text compact.

1. Finish the full contract and all children. Verify production asset/runtime behaviour and the relevant test matrix.
2. Write `docs/sessions/YYYY-MM-DD-<slice-or-topic>.md`: purpose, changes, exact verification, evidence and limitations.
   Record continuation as a pointer to ROADMAP, not a competing live Next list.
3. Update the one row's state and evidence link. **Done** requires evidence and completed children. **Recorded** means
   dated earlier evidence, not a new run. Keep unresolved/review/later work visible; never use it to hide a failure.
4. Update context's current ID/handoff; correct affected contracts and source references. New decision → ADR/index;
   new measured board → bench immediately; learned implementation technique → recipe; durable scar → memory reference.
5. Run the documentation validator and scoped code checks if code changed. Propose a Conventional Commit; user commits.

## Keep documents small without losing knowledge

Each file declares its role. Only ROADMAP owns live state. Historical statuses, Next lists, grades, tool versions
and grants are not current instructions. Recheck source facts after code changes. Preserve original evidence when
superseding claims; never rewrite history as though tests ran again.

Entry budgets in bytes: AGENTS 8,000; PRINCIPLES 6,500; SANITY 4,500; BUILDING 7,000; MEMORY 3,000;
context 2,500; this map 6,000; START_HERE 4,500. These are ceilings, not targets. Move examples/scars to linked references before
exceeding them. The master table and full technical/evidence documents have no arbitrary truncation limit.
Compatibility pointer pages stay below 1,200 bytes. New reference docs must be reachable from a row or index.
