# Diagnostics programme planning — 2026-09-14

<!-- doc-role: evidence -->
> Planning evidence. Only [ROADMAP](../ROADMAP.md#slice-diag.0) tracks implementation.

## Purpose and authorization

The user requested deep research and an implementation-ready diagnostic/allocator/instrumentation programme
immediately after REPO.DEV. This follows the [development diagnostics review](2026-09-14-development-diagnostics-review.md).
The request authorizes documentation and roadmap changes; no implementation loop was started, no engine code was
edited by this task, and no commit/push was performed. Existing renderer design gates remain unaccepted.

Used the deep-research skill for a repository Markdown report with primary references. Output, audience and platform
order were already explicit, so no material clarification was needed. No advisor capability was exposed; no advisor
review is claimed. Worked directly in accordance with the no-delegation rule.

## Source and research evidence

Baseline HEAD `2b6dcdb090cea710fbaa7bdc4a94024ab5d2f510`; preserved the existing CMake/preset/tooling/fuzz/document
repairs. Other work changed additional source/tests/third-party records during the turn; none was reverted or included
as this task's implementation. The roadmap was backed up under ignored `build/diagnostics-plan` before inserting rows.

Read current orientation, qualification and test-instrument contracts; inspected public and production memory,
jobs/fiber, crash/assert, log, profiler/registry/capture, app, CEIR diagnostic/executor and GPU validation/timing seams.
DG01–DG18 distinguish observed source, recorded prior experiments and unqualified integration concerns. No new
runtime failure was reproduced and no C++ builds, sanitizer runs or benchmarks were performed for this planning task.

The [research](../research/2026-09-14-diagnostics-and-instrumentation.md) has 34 numbered primary-source notes covering
sanitizers/custom pools, guarded sampling, controlled/weak-memory scheduling, rr/TTD, crash collection/signal safety,
tracing/sampling, DX12/Vulkan/Metal/WebGPU, generated-code symbols, fuzzing, coverage and real-time checks. Some sources
are living upstream docs newer than pinned tools. The main WebGPU spec exceeded retrieval limits; the readable
community explainer is labelled non-normative. No upstream capability is presented as Cerid runtime qualification.

## Documents and sequence

- Added [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md): authorized scope/order, owned-module
  boundaries, bounded evidence, privacy, later platforms and explicit mechanism choices.
- Added the [implementation contract](../design/runtime-diagnostics.md): public/source ownership, complete per-child
  deliverables, failing/valid controls, capture schema, modes/budgets, authorable policies, failure cases and qualification.
- Added **38 immediate DIAG rows** (37 children plus parent) directly after REPO.DEV. All start Open. Known scheduler
  race repair and instrument-model qualification precede shared-state diagnostic work. REPO.3d now depends on DIAG.
- Added **MAC.DIAG** and **WEB.DIAG** before their existing release parents. Windows/Linux evidence is required for
  DIAG; later-platform support is not falsely certified by the early close.
- Preserved every previous slice ID/order/status. Transferred DG05's immediate repair from CORE-USE.2 to DIAG.1a;
  retained renderer integration there. Clarified later MEM-HARD/JOBS-HARD/OPS/SANITY-TLSF work and linked shared
  mechanisms from RAH/LANG/editor rows. No parallel tracker was added.
- Updated entry/rule/index routes, context's next-programme pointer, ADR-0129's dated order amendment and the
  qualification/contribution contract. Corrected BUILDING/test-instruments wording that overclaimed a TSan-verified
  fiber model; preserved the dated probe measurements and explained the new negative-control requirement.

The current pointer remains REPO.DEV. Future implementation follows the one table from its earliest available work;
publication and hardware evidence remain genuine gates. No undocumented mode is presented as an existing command.

## Verification

- `check-master-plan.py`: PASS — 903 rows, 1,058 documents, 9,437 local links; source-ID routes retained.
- Preservation audit: all 863 previous slice IDs, relative order and statuses retained; exactly 38 contiguous DIAG
  rows and two later platform children added; all 37 detail sections match table order; DG01–DG18 and 34 source
  notes resolve. `--next` still returns REPO.DEV and retains its CI waits.
- `git diff --check`: PASS. The two over-budget navigation pages found by the first validation were compacted;
  all mandatory orientation byte budgets now pass.

No runtime or performance result is claimed by this plan. Implementation acceptance lives in the DIAG rows and
linked contract. The task's scratch scripts/backups remain under ignored build output, outside active documentation.

Suggested human commit: `docs: plan runtime diagnostics and allocator hardening after REPO.DEV`.
