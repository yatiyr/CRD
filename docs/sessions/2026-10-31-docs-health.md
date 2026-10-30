# 2026-10-31 — Documentation health: a whole-documentation drift audit and its guards

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## The user's order (verbatim, 2026-10-31)

<a id="user-order"></a>

> "do not forget to update all the agent related docs and the whole docs. Everything needs to be fully healthy and
> not drifted from now on! Including the project readme and all the other markdowns and docs! Everything needs to be
> fully serious"

The audit ran on the `docs-health` branch beside the serial workflow (SIMD-DISPATCH on `main`) and the workspace
overlay agent. It touched no engine, test, CI-orchestrator or workspace file, edited no ROADMAP row, and ran Python
only (no configure, build, CTest or benchmark).

## Scope and classification

All 1,451 Markdown files of the tree were inventoried by their `doc-role` marker and location:

| Class | Where | Treatment |
|---|---|---|
| Entry points | README, START_HERE, AGENTS, MEMORY, context, docs/README, BUILDING, CODING, CONTRIBUTING, PRINCIPLES, SANITY | every claim checked against the tree; corrected |
| Living contracts and maps (108 documents with the entry points) | undated `docs/design/*`, `docs/systems/*`, `docs/hints/*`, top-level `docs/*.md`, every directory README, tool READMEs outside docs/ | facts, paths, commands, presets, counts, tone |
| Decisions | `docs/decisions/*` (130 records and the index) | history kept; supersession notes made reciprocal; index checked |
| Dated evidence | `docs/sessions`, `docs/bench` boards, `docs/archive`, `docs/research`, numbered lessons, band-prefixed design notes, the memory corpus, any document whose role is `evidence`, `historical`, `archive` or `session` | never rewritten; links and index membership only |

## Sources of the current process

The process statements now in [AGENTS](../../AGENTS.md) come from the user's own recorded words: the commit and push
authorization of the serial roadmap workflow and the no-attribution rule (2026-10-06, "push as a real person, no AI or
Claude will be mentioned in push commits"; already reflected in [context](../../context.md) as "serial agent pushes
per user"); push every batch and let CI run the matrix, fixing CI failures first (2026-10-18); defer nothing
(2026-10-17); the gold-standard recommendation rule and the SSE2/GPU floors
([decisions](2026-10-26-user-decisions-platform-floors-and-research.md)); the testing-tool download grant
([CI platform session](2026-10-30-ci-platform.md#user-order), 2026-10-30); parallel agents by explicit order
(the CI rows, 2026-10-30); and boards on a busy host judged by interleaved ratios (2026-10-31: "we are looking for
relationship between peers! ... it does not matter whether the pc is busy or not!"). The 2026-10-06, 2026-10-17,
2026-10-18 and 2026-10-31 words were recorded in the operator's tool memory rather than in a session; this note is
their dated repository record.

## Drift found and fixed

| Drift class | Instances | Fix |
|---|---|---|
| Retired commit/push and delegation rules stated as current | 10 statements in 9 living files (AGENTS, START_HERE, BUILDING, CONTRIBUTING twice, docs/README, the quality contract, MEMORY, the runbooks' publication section, context's 2026-10-04 "no parallel agents") | AGENTS owns the rule; the others route to it; ADR-0133 carries an amendment note |
| Pre-2026-10-27 delivery order (renderer, then UI, then hesap GPU) | README, START_HERE, context | route to the [delivery plan](../design/delivery-plan.md) and the ADR-0129 amendment |
| Hand-maintained counts that drifted | systems map 102/96/6 modules (truth 105/98/7); module registry 96 engine modules, four host tools, 103 test directories (98, 5, 109); "eight native profiles" in BUILDING, the VS configuration contract and the design index (7) | generated fact blocks; BUILDING no longer states a number |
| Documents no index or link reached | 8 design contracts, 7 recipes, 2 research parts, 7 system pages and the jobs verification, 4 detours, 38 phase pointers and the phase/detour indexes, 50 boards, the benchmarking policy, the paper index, a GPU hints file, a superseded protocol | indexes completed; the per-slice protocol marked historical and archived |
| ADR supersession without the reciprocal note | ADR-0020, 0023 and 0057 superseded by ADR-0107 D2 (0107 named none); ADR-0080 by 0103/0104/0105 (0104 silent); ADR-0034 superseded by ADR-0081 but still "Accepted" with no note | notes added to ADR-0034, 0104 and 0107; decisions unchanged |
| ADR index rows hiding a recorded supersession | 13 rows (0018, 0020, 0023, 0032, 0034, 0036, 0042, 0047, 0057, 0080, 0081, 0085, 0099) | each row names its supersession |
| Pictographs in living documents | 84 lines in 8 files (rendering-UI catalogue, GPU hints, crush playbook, kernel mandate, benchmarking policy, bench index, debt catalogue, CEIR op schema) | plain words; captured tool output in code blocks kept verbatim |
| References to things not in the repository | the delivery plan named a JSON and an applier in the ignored build tree as its machine form; the structure-sync example named a nonexistent `example.cpp`; the bench index cited a nonexistent AGENTS section; the lessons index routed conventions to CLAUDE.md | reworded as a one-time local tool, a real source, a link, and BUILDING/CODING |
| Stale pointer | context's 2026-10-04 handoff link | the current row's latest session |
| README tone and gaps | marketing claims; no CPU levels, determinism modes, platforms, testing, CI transition or tracking section | rewritten from the tree with generated facts |
| CI provider statement | BUILDING named only the GitHub workflow | GitHub Actions until CI-4, then the orchestrator |

## The guards

[scripts/check-docs.py](../../scripts/check-docs.py), run by `check-master-plan.py` (so every lane and the
`crd-master-plan` CTest run it) and on its own, holds six classes:

- `links`: links and anchors in Markdown outside `docs/` and the root entry points (the master-plan validator covers
  those).
- `refs`: paths, script names, presets (and hosted job names), `-DCRD_*` options and `crd-check-*`/`crd-no-*` guards
  named in living documents exist; a tool in the ignored build tree is not a source; lines narrating history and
  placeholders are exempt.
- `facts`: `<!-- fact:NAME -->value<!-- /fact -->` blocks equal their source (CPU levels from `cpu_features.hpp`,
  `cpu_features.cpp` and `CrdSimd.cmake`, which must agree; determinism modes; module, engine-module, tool and
  test-directory counts from the `crd_module()` registry; native profiles from the presets; entry budgets from the
  validator; the copyright line from LICENSE); `--refresh` rewrites them; a hand-written count of the same thing fails;
  the README must keep its licence, CPU, tracking, build and CI statements.
- `indexes`: indexed directories name every document; every other document under `docs/` outside the sessions is
  linked from somewhere.
- `adrs`: the index links every record once and shows a recorded supersession; supersession statements are
  reciprocal (one documented exemption: ADR-0105 names a module, not a decision, "superseded by ADR-0104 deploy").
- `phrases`: no retired commit, delegation or Buildkite statement, no pre-2026-10-27 delivery order, no routing of
  rules to CLAUDE.md, no pictograph and no TODO/TBD/FIXME without an owning row.

[scripts/test-check-docs.py](../../scripts/test-check-docs.py): 21 cases on synthetic trees (each class has a failing
and a passing case) plus the clean-repository case; disabling any one of the seven check functions fails one to four
of them. Its CTest
registration belongs to the serial workflow, which owns `tests/`:

```cmake
add_test(NAME crd-docs-drift
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-check-docs.py")
set_tests_properties(crd-docs-drift PROPERTIES TIMEOUT 300 ENVIRONMENT "PYTHONUTF8=1")
```

## Verification

`python scripts/check-docs.py`, `python scripts/test-check-docs.py`, `python scripts/check-master-plan.py`,
`python scripts/check-repository.py`, `python scripts/check-allman-braces.py`, `python scripts/check-generated.py`,
`python scripts/check-pins.py`, `python scripts/check-ci-tiers.py`, `python scripts/check-diag-runbooks.py --static`
and the CMake-free `test-repository-tools.py` classes MasterPlanSequence, Provenance, DiagRunbooks, AllmanBraces and
CiTiers (33 tests): all pass. The master-plan validator with the drift guard takes about 8 s on this host.

## Limits and open items

- Facts that exist only on unmerged branches were not written into living documents: ADR-0135 §16 (polling and the
  Google IAP tunnel by default) is on `ci-orchestrator-elite`; the GitLab hosting and the agent workspace overlay have
  no merged record yet. The overlay agent owns the repository-layout paragraph on agent entry files.
- Dated design notes, research, sessions and boards keep their historical paths and wording by rule; only their
  links and index membership are checked.
- The operator's tool memory holds operational lessons (WSL lanes, sccache ports, watchdogs) that have no repository
  record beyond the build-performance contract; whether to port them into `docs/lessons/memory` before the overlay
  move is the user's choice.
