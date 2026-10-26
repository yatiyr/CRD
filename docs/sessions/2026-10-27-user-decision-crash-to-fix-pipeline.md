# User decision: crash reports to guarded fixes (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Decision

When an application built on Cerid crashes on any machine, Cerid records where, what and how it went wrong with every
fact a developer needs to debug it, including platform-specific causes and cases the team missed. A local agent later
works through the pooled reports and fixes them under strict, machine-enforced guidance that keeps it from acting on its
own judgment or producing slop. The user approved the four rows on 2026-10-27:
[CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1) to [.4](../ROADMAP.md#slice-crash-fix.4) and the
[CRASH-FIX](../ROADMAP.md#slice-crash-fix) parent, placed directly after [OPS.1](../ROADMAP.md#slice-ops.1) and moving
with it (the research amendment brings OPS.1 before the first desktop release, QUAL.2).

## What already exists and what the rows add

- Existing: crash capture and dumps (DIAG.5a), versioned bounded bundles with exact symbol identities (DIAG.5d), the
  flight recorder's last events, `bundle.inspect` for agents, hardware identity and classification (HW-RESILIENCE.2)
  and the collector service owner (OPS.1).
- CRASH-FIX.1: a complete, consented reproduction pack that replays the failure on another machine.
- CRASH-FIX.2: the crash pool: grouping, regressions per release, code defects split from hardware faults.
- CRASH-FIX.3: the guarded fix agent: reproduce first, failing test first, root cause in the owning module, no weakened
  checks, no code "fix" for a hardware fault, pull requests with the evidence chain for human review, never a push.
- CRASH-FIX.4: qualification on a seeded-defect fleet that includes slop attempts, and full documentation.

## Same session

- RESEARCH-ADOPT moved ahead of SIMD-DISPATCH ([decisions](2026-10-26-user-decisions-platform-floors-and-research.md)).
- Intel SDE 10.13.1 downloaded with the user's approval and verified against Intel's published SHA256 values.
