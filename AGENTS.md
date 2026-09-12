# Cerid — instructions for every agent

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

**Purpose:** build Cerid as a modular, authorable C++20 engine/application substrate. Real-time and offline
rendering, games, engineering tools and later scientific/creative applications share public modules.

## Start here — same process for every tool or model

Universal entry: [START_HERE](START_HERE.md). It routes the same rules and single plan for every agent.

1. Read this file, [PRINCIPLES](docs/PRINCIPLES.md), [SANITY](docs/SANITY.md) and [MEMORY](MEMORY.md).
2. Read [context](context.md) for the current pointer. Find that exact ID in [ROADMAP](docs/ROADMAP.md).
3. Read the row's detail and ADR, then the relevant public source via [systems](docs/systems/README.md).
4. Before code, read [BUILDING](docs/BUILDING.md) and [CODING](docs/CODING.md). Before hot-kernel/performance work,
   also read [KERNEL-CRUSH-MANDATE](docs/KERNEL-CRUSH-MANDATE.md) and [crush playbook](docs/hints/crush-playbook.md).
5. State the task's purpose, exact slice, full deliverable and verification scope before acting.

Continue within the user's current request and existing authorization; do not ask again merely because an old session ritual says to. Ask when scope/requirements
are genuinely unresolved or an explicit design gate is still unaccepted. Never invent approval or tool results.

## One owner for each fact

- **ROADMAP is the only live roadmap, slice table and bug/finding queue.** All new child slices and findings get
  an owning row there. Only context carries the current pointer; it does not duplicate row status.
- ADRs and design notes define contracts. Source code determines implementation facts. Sessions, benches,
  recipes and archived plans preserve dated evidence. Old Next lists/status/loop grants have no live authority.
- Read only the active contract and relevant evidence unless the user requests a broad audit. Do not load whole
  archives on entry. [Documentation workflow](docs/README.md) explains lookup, bug handling and close-out.
- If sources conflict: report the exact conflict, follow the latest applicable user decision, and correct the
  stale pointer. An unaccepted proposal is not an accepted decision. Historical measurements are never rewritten.

## Conduct — mandatory

- **Ordered work:** start at the earliest unfinished work unless the user explicitly directs a future slice.
  Record CI-only waits as Needs CI and continue available work; never close or drop the pending gate. The
  [sequence rule](docs/ROADMAP.md#strict-sequential-execution) binds every agent and loop.
- Re-anchor to the task's purpose at every prompt. Never silently narrow a slice, drop an inherited requirement,
  weaken its tests or replace the requested quality bar with a smaller feature. Ask before a real scope reduction.
- **Everything executable is an authorable asset.** Follow PRINCIPLES' exact form and deletion requirements.
  A shipped algorithm hidden in a C++ graph builder violates the purpose even if the pixels look correct.
- Reuse before implementing: search public APIs, implementations and actual consumers. Extend the owning module;
  do not duplicate a helper in a consumer or introduce dependency cycles. Same path for tests, sandbox and real apps.
- Fix verified failures; calling them pre-existing, recording debt or obtaining one retry-pass does not close them.
  An unresolved failure blocks its owning slice. Respect the user's module boundaries while identifying its owner.
- Preserve uncommitted work. HEAD is not this session's baseline. Back up affected files before large moves;
  retain unique source/recipes before deletion. Never restore unrelated files to HEAD to simplify a diff.
- Work directly; do not fork/delegate to subagents (standing user instruction). Seek the `advisor` for non-trivial
  implementation plans when that capability exists. If unavailable, record it honestly; never claim a review ran.
- Keep useful progress updates. Describe facts, evidence and uncertainty plainly. Do not call something broken
  until the harness is verified (SANITY #11). A source concern is not a reproduced runtime failure.
- Settled substrate follow-ons with a real consumer and bounded gates ship with that slice. Speculative design
  waits for its consumer as an explicit ROADMAP row; it is never silently discarded.
- Prefer lifecycle hooks over cleanup calls consumers must remember. A stub/displayed field is not integration:
  a real input must cause observable behaviour through the public production path.
- Studied technique implemented in code → an educative [recipe](docs/recipes/README.md): parameters/units/defaults/
  ranges first, cited math, reproducible assembly, pitfalls, code and benchmark links. Document paper divergence
  as a numbered decision with rationale in its ADR/system reference.
- Every measured benchmark board goes in [bench](docs/bench/README.md) **at measurement time**, including all
  required peers and losses. Numerical crush work includes scipy, MATLAB, Boost and GSL where applicable;
  check/install missing peers or document checked N/A. A tie/loss cannot close a full-victory contract.

## Completion and verification

- Fulfill the entire row, children and inherited contract. Complete public downstream behaviour, error paths,
  authored assets, persistence/reload and relevant backend evidence; cooking alone is insufficient.
- Keep warnings zero. Match surrounding hand formatting; **do not run `clang-format -i`**. Run the LLVM-20
  incremental tidy helper for changed C++ headers/TUs and confirm it parsed them. Full commands: BUILDING.
- Locally use one primary configuration for changed modules **and affected consumers**, scoped CTest/guards and
  incremental tidy. Add configurations/platforms only for a discriminating risk or reproduced failure; use BUILDING's
  decision table. CI owns broader Windows/Linux/backend and full-matrix qualification. Preserve validation/oracles,
  nonzero execution counts, real exit codes and timeouts. Never run full repository/configuration sweeps locally.
- Use testable invariants, adversarial boundaries and measured budgets. Do not claim a platform from its emitter
  or a completed feature from a schema. Parent completion requires every child and its evidence.
- Every task, including fixes and investigations, ends with a session record and accurate affected documents.
  Follow the [quality/close-out contract](docs/design/system-quality-contract.md): update the owning ROADMAP row,
  context and changed contracts/API examples/indexes; inspect orientation docs for affected facts. Preserve historical
  evidence and still-correct text. Add benches/recipes/lessons when warranted and run the documentation validator.
- Qualify actual OS/ISA/API/adapter/driver/workload tuples. Design for later targets now; unavailable target evidence
  leaves its gate open. Model/agent/plugin inputs never grant authority; commands enforce capabilities and budgets.

## Git and workspace

- **Never run `git commit` or `git push`.** The user commits. Propose a Conventional Commits message without any
  AI co-author trailer. Do not bypass hooks/signing; solve the underlying issue.
- Windows uses PowerShell. Use `rg` for searches, UTF-8 for text, absolute paths for executables, and verify
  deletion/move targets remain inside the intended directory. Never overwrite HOME/CODEX_HOME variables.
- Source/project structure changes use the [synchronizer contract](docs/design/project-structure-sync.md).
  Inspect its status before moving files; preserve saved IDE edits, tracked membership overrides and recovery journals.
- Keep implementation within the current authorized scope. Repository organization does not authorize
  geometry/physics algorithm changes; retained work lives in ROADMAP. Historical grants or roadmap order never restart a stopped loop.
