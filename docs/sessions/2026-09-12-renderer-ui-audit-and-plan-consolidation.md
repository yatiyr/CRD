# Renderer/UI audit and plan consolidation — 2026-09-12

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Documentation and architecture review only.** The objective is a fully authorable renderer, complete
Cerid-owned UI and CR-D007 game/engineering editor, followed by the shared scientific notebook and remaining work.
No engine source, geometry, physics or numerical implementation was changed by this audit. No build, GPU run or
benchmark result is claimed. The existing uncommitted CEIR/assets work remains the user's working tree.

## Confirmed decisions

- Finish the entire retained renderer library before UI, including virtual geometry, hybrid GI and production
  offline rendering. Runtime/asset qualification precedes UI; visual authoring closes after I2D-9. Original
  feature parents remain open until both stages pass.
- Windows/Linux first; macOS and web explicitly follow.
- Notebook as one reusable CR-D007 workspace plus a thin standalone host.
- Remove redundant documents after preserving unique content; one linear table owns live slice status.

[ADR-0129](../decisions/0129-renderer-ui-editor-delivery-order.md) records these decisions. The full ADR-0107
remains Proposed; RAH-0's no-loss review remains a gate. The completed CEIR loop was not restarted and no
unavailable advisor sign-off was invented.

## Durable result

[ROADMAP](../ROADMAP.md#master-table) contains 658 ordered rows with stable IDs, prerequisites, detail and ADR
references. [Context](../../context.md) is a pointer. The [execution contract](../design/renderer-ui-execution-contract.md)
and [inherited catalogue](../design/rendering-ui-contracts.md) define requirements without duplicate live status.
The [audit](../research/2026-09-12-system-audit.md) records 27 findings, primary-source research, review limits
and all 96 engine module manifests. Its [JSON record](../research/2026-09-12-system-audit.json) preserves the
pre-edit document inventory/hashes and routes all 210 original D-007 IDs plus 16 original hesap-GPU v17 IDs.

Ten superseded/redundant files were removed. Full source material from the old roadmap, renderer/UI proposal,
UI/editor/font/platform plans and D-007 programme was consolidated in [the archive](../archive/README.md).
Unique sessions, ADR evidence, recipes and benchmark boards remain references. The long old detour index and
stale CEIR/CHIR/rendering overviews were preserved there before updating their current entry points. Old phase
tables explicitly relinquish live scheduling/status authority to ROADMAP.

Corrections cover separate UiWorld identity, CEIR ownership, owned fonts, existing app propagation, private
reload-service ownership, platform maturity, unfiled future ADR links and superseded sequencing. PRESENT and
LSH-3.core resolve renderer ordering gaps. Editor-triggered SCEN/öbek follow-ons precede EDITOR.1. ROCm and
original v17/D-004/D-005 obligations remain explicit.

## Verification and continuation

`python scripts/check-master-plan.py` passed: one table, 658 unique/ordered rows, valid prerequisites and parent
order, renderer-before-UI, editor-before-notebook, source routing and current local links. The broader audit
checked local links in every new/changed Markdown document against the pre-audit snapshot.
`git diff --check -- docs AGENTS.md README.md scripts/check-master-plan.py` passed, with only Git line-ending
normalization notices for existing files. These documentation checks do not claim runtime/performance acceptance.

Continue from [AUD-2](../ROADMAP.md#slice-aud-2), not an older session's Next paragraph. Refresh the asset/consumer/
backend no-loss census, pin PQP baselines and review RAH-0. Legacy G-buffer deletion is only part of RAH-1;
resident bindings and atomic shared reload remain substantial foundation work.

Suggested commit: `docs: consolidate master plan and audit renderer UI foundations`. The user commits;
no agent commit/push or AI co-author trailer.
