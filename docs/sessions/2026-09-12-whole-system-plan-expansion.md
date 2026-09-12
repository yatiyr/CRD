# Whole-system review and master-plan expansion

<!-- doc-role: evidence -->
> Dated session evidence. Live state: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

## Purpose and authorization

Expand the existing single roadmap to the complete Cerid product vision and provide one explicit agent entry with
serious cross-platform implementation, bug diagnosis and documentation close-out obligations. This was a documentation,
code-inspection and primary-research task. Geometry/physics and unrelated engine implementations were not modified.
No engine build, runtime qualification or benchmark is claimed. No advisor capability was available; no review was invented.

Confirmed decisions: inference and training qualify together; projects use an authoritative host plus retained local/
offline edits, while multiplayer simulation keeps a separate authoritative contract. Renderer → crd-ui/CR-D007 →
scientific notebook order and Windows/Linux-first platform sequencing remain intact.

## Changes and evidence

- [START_HERE](../../START_HERE.md) is the common entry. [Quality contract](../design/system-quality-contract.md)
  covers ownership, jobs/memory, targets, correctness/performance/beauty, data/security and every-task close-out.
- [Whole-system review](../research/2026-09-12-cerid-whole-system-review.md) records 30 classified G findings and
  29 primary sources, with [96-module census](../research/2026-09-12-whole-system-census.json), sampled source hashes
  and master ownership. Existing A01–A27 findings and historical evidence remain intact.
- The master table retains all 666 prior IDs and adds 146 slices/subslices, for 812 total: allocator/jobs/platform use, reflection/
  scripting/agents/extensions, scientific CHIR, combined training/inference, arbitrary-skeleton posing, security,
  collaboration, DAW, modeling, manufacturing, sensors, simulation, embedded generation and cross-domain qualification.
- [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md) separates confirmed directions
  from proposed mechanism choices. Amendments make old C++-only, inference-only and weaker physics-performance
  wording visibly subordinate. Root entries, context, rulebooks and reference indexes route the expanded contracts.

## Verification

- `python scripts/check-master-plan.py`: PASS, 812 rows, 988 documents, 8,225 local links, 210 original D-007 routes
  and 16 original v17 routes. All 666 prior IDs and 146 additions remain; G01–G30 owner references pass.
- Read-only `--next`, `--find G09` and `--slice SCRIPT` queries returned the intended rows; current pointer AUD-2.
- In-memory negative checks deliberately removed a finding-owner link and renamed a retained ID. Both were rejected
  by the relevant guards; these checks changed no files.
- 96/96 current module manifests matched the classified census and hashes. All sampled source hashes remained unchanged.
  All eight entry/navigation documents are within their enforced byte budgets, including START_HERE at 3,669 bytes.
- Scoped `git diff --check`: exit 0. Git emitted line-ending normalization notices, with no whitespace failures.

The dependency review separated WORLD.1 foundations from WORLD.2's later actual sensor/physics qualification and placed
the final portfolio after retained debt/maintenance. Input action mapping and multichannel/audio-clock qualification
received explicit children. Documentation validation does not qualify runtime, security, physical accuracy, hardware
support or performance. Original document snapshots and scratch tooling are in ignored `build/system-review-20260912/`;
durable routes/evidence are in repository documents. No new benchmark board is warranted because no measurement ran.

## Handoff

Continue from [context](../../context.md) → [AUD-2](../ROADMAP.md#slice-aud-2). RAH-0 and full ADR-0107 remain design
gates; mechanism proposals in ADR-0130 need their owning slice review. No autonomous loop was restarted. No commit or
push was run; the user owns commits.
