# Strict sequential slice execution

<!-- doc-role: historical -->
> Dated user-direction and maintenance record. Live state: [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Purpose and decision

The user asked what REPO.3c owns, why later rows were active/complete while earlier rows were unfinished, and required
every slice/subslice to finish completely one after another. This supersedes the earlier REPO.DEV.1 permission to work
on independent children while publication or hardware waited. The full repository programme and renderer stop boundary
remain intact. This turn applies the explicit documentation/workflow correction; it does not advance a later engine row.

REPO.3c owns repository-verification repairs: native CI environment, Linux validation acquisition, strict analysis,
actual DX12 adapter classification and failed GPU workloads, plus the observed FFT stack-overflow repair. Later rows
had progress because diagnosis exposed related failures and the earlier authorization allowed independent tooling
work. Some parent rows also used In progress as an aggregate label. That presentation did not express one execution
position; the new rule removes that ambiguity without discarding source changes or claiming missing qualification.

The canonical [strict sequence rule](../ROADMAP.md#strict-sequential-execution) binds every agent and loop. Only the
first unfinished row is eligible; a publication/hardware/design gate stops advancement. Parent rows are close-out gates.
Proven missing prerequisites may be inserted before their owner with a referenced reason, never as a route to unrelated
work. An order/scope change needs an explicit user decision. Recorded remains historical/documentation scope only.

## First unfinished row

At adoption, the first unfinished row is REPO.3c.4.a: classify the actual DX12 device with kernel evidence when DXGI
flags are insufficient, preserve Unknown and retain exact-provider qualification. Its
[implementation evidence](2026-09-12-dx12-adapter-classification.md#subsequent-qualification-and-completed-ci-census)
and [subsequent hardware/WARP checks](2026-09-12-atomic-abuffer-emitter-repair.md) remain valid dated local evidence.
They do not establish the repaired classifier on the hosted flag-0 BasicRender driver 10.0.26100.33296.

Read-only `gh run list --repo yatiyr/CRD --limit 5 --json databaseId,headSha,status,conclusion,url,createdAt` still showed
[34700773738](https://github.com/yatiyr/CRD/actions/runs/34700773738) as the latest run, completed with failure at
`2ed89c487215c91ffc2f667c19b2d97f01f5598c`. That revision predates the local classifier repair. Therefore the row's
publication/provider gate remains open; context returns to that row and it is Blocked. No missing pass is invented.
The human must publish before the repaired hosted tuple can be qualified. Agents never commit or push.

After publication, inspect the exact run and classifier census/affected evidence against this row's contract.
Only after full closure advance to the next table row. Do not make all future workload repairs prerequisites of this
classification child; their own acceptance remains in the existing later rows. A genuine defect in classification
itself remains this child's responsibility. Required oracles and previously declared qualification are unchanged.

## Preserved later work

Six later Done rows became Partial pending sequential acceptance: REPO.3c.8, REPO.DEV.2, REPO.DEV.3a,
REPO.DEV.3b.1, REPO.DEV.3b.4 and REPO.DEV.3b.5. Their original implementation and passing evidence were preserved.
This is a sequencing hold, not a claim that those measurements failed. At their turn inspect the full contract,
current inputs and existing evidence, then perform only missing or newly justified verification before closing.

Nine later In progress rows became Partial: REPO.3c.4, REPO.3c.6, REPO.3c.9, REPO.3c.10, REPO.3c,
REPO.DEV.3b.2, REPO.DEV.3b, REPO.DEV.3 and REPO.3. Original Open/Review/Later and historical Recorded rows retain
their meaning. No slice ID, requirement, implementation or historical test record was removed; the table has 859 rows.
The preceding [ASan investigation](2026-09-12-dx12-asan-investigation.md) is captured completely, then parked with its
owner. No further SDK probe or engine repair was started after the user required strict sequencing.

## Enforcement and close-out

Updated ROADMAP, AGENTS, START_HERE, context, MEMORY, SANITY, BUILDING, the documentation map and affected execution,
quality and large-C++ contracts. The existing heartbeat keeps its identity, thread, 15-minute cadence and quiet
unchanged-state behavior; its prompt now follows strict order and waits at the current external gate. No second loop
or tracker was created. PRINCIPLES and CODING were inspected; their architectural/style rules remain correct.

The documentation validator now rejects a missing/duplicate/out-of-order current pointer, later In progress or Done
rows beyond an unfinished gap. `--next` fails as well when the pointer/sequence conflicts. Regressions cover all six
unfinished states, retained Partial evidence, historical Recorded entries, stale/duplicate pointers, complete tables
and the query's actual exit/output. Existing six repository-tool fixtures remain included.

Verification on Windows/Python 3.14.4:

- `python scripts/check-master-plan.py`: **859 rows, 1,013 documents, 8,658 local links**, 210 D-007 and 16 v17
  routes passed; every compact orientation document remained within its byte budget.
- `python scripts/test-repository-tools.py -v`: **11/11 passed**, including five new sequencing regressions and
  the six existing repository-tool fixtures. This is tooling evidence, not a new engine/platform qualification.
- `python scripts/check-repository.py`: all 96 module/layout/registration/ignore contracts passed.
- `git diff --check`: passed. The harmless CRLF-to-LF notice for the touched documentation map was resolved by
  writing that file with its canonical LF encoding.

The normalization helper initially expected 16 affected later rows; its pre-write assertion showed the actual 15
(six completed, nine active). The count was corrected before the guarded write. No partial table mutation occurred.
Readback confirmed the existing heartbeat retained its ID/thread/cadence and now contains the strict-order instruction.
No engine rebuild was needed. No commit, push, new architecture acceptance or unavailable advisor review occurred.
This goal turn made progress on the user-requested rule/enforcement correction; the next eligible work remains the
first row's publication/provider gate, not a later engine investigation.
