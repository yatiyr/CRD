# Cerid — instructions for every agent

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

**Purpose:** build Cerid as a modular, authorable C++20 engine/application substrate. Real-time and offline
rendering, games, engineering tools and later scientific/creative applications share public modules.

## Start here — one process for every tool

Universal entry: [START_HERE](START_HERE.md).

1. Read this file, [PRINCIPLES](docs/PRINCIPLES.md), [SANITY](docs/SANITY.md) and [MEMORY](MEMORY.md).
2. Read [context](context.md) for the current pointer. Find that exact ID in [ROADMAP](docs/ROADMAP.md).
3. Read the row's detail and ADR, then the relevant public source via [systems](docs/systems/README.md).
4. Before code, read [BUILDING](docs/BUILDING.md) and [CODING](docs/CODING.md). Before hot-kernel/performance work,
   also read [KERNEL-CRUSH-MANDATE](docs/KERNEL-CRUSH-MANDATE.md) and [crush playbook](docs/hints/crush-playbook.md).
5. State the task's purpose, exact slice, full deliverable and verification scope before acting.

Continue within the user's request and authorization; ask only when scope or requirements are genuinely
unresolved or a design gate is unaccepted. Never invent approval or tool results.

## One owner for each fact

- **ROADMAP is the only live roadmap, slice table and bug/finding queue.** New child slices and findings get an
  owning row there. Only context carries the current pointer; it does not duplicate row status.
- ADRs and design notes define contracts; source code determines implementation facts. Sessions, benches, recipes
  and archives preserve dated evidence; old Next lists, status and loop grants have no live authority.
- Read the active contract and its evidence, not whole archives; [docs workflow](docs/README.md) covers lookup,
  bugs and close-out.
- If sources conflict: report the exact conflict, follow the latest applicable user decision and correct the stale
  document. An unaccepted proposal is not a decision. Historical measurements are never rewritten.

## Conduct — mandatory

- **Ordered work:** start at the earliest unfinished row unless the user explicitly directs another. Record CI-only
  waits as Needs CI and continue; never close or drop a pending gate. The
  [sequence rule](docs/ROADMAP.md#strict-sequential-execution) binds every agent and loop.
- Re-anchor to the task's purpose at every prompt. Never silently narrow a slice, drop an inherited requirement,
  weaken its tests or lower the quality bar. Recommend the complete, gold-standard option, never the easy one.
- **Everything executable is an authorable asset.** Follow PRINCIPLES' exact form and deletion requirements.
  A shipped algorithm hidden in a C++ graph builder violates the purpose even if the pixels look correct.
- Reuse before implementing: search public APIs, implementations and consumers. Extend the owning module; never
  duplicate a helper or introduce dependency cycles. Same path for tests, sandbox and real apps.
- **Defer nothing.** Fix every verified failure and finding on the spot (warnings, flakes, weak tests, stale docs,
  third-party defects via the register). An open failure blocks its slice; "pre-existing" or a retry-pass closes
  nothing. Only physical user actions (elevation, absent hardware, accounts) stay with the user, as one command.
- Preserve uncommitted work; HEAD is not this session's baseline. Back up files before large moves; never reset
  unrelated files to simplify a diff.
- Work directly; do not delegate on your own initiative. Workflows and parallel agents run only by user order, each
  in its own worktree and branch with an explicit file scope; verify any delegated claim before relying on it. Use
  the `advisor` for non-trivial plans; never claim a review that did not run.
- Describe facts, evidence and uncertainty plainly. Do not call something broken until the harness is verified
  (SANITY #11); a source concern is not a reproduced failure.
- Settled follow-ons with a real consumer ship with their slice; speculative design waits as a ROADMAP row.
- Prefer lifecycle hooks over cleanup calls consumers must remember. A stub or displayed field is not integration:
  a real input must cause observable behaviour through the public production path.
- Implemented research gets an educative [recipe](docs/recipes/README.md): parameters, units, ranges, cited math,
  pitfalls, code and benchmark links; paper divergences are numbered decisions in the ADR/system reference.
- Every benchmark board goes in [bench](docs/bench/README.md) **at measurement time**, with every required peer
  (scipy, MATLAB, Boost, GSL where applicable) and every loss. Run boards on a busy host with `--allow-contended`:
  record the contention and judge by ratios of interleaved arms against peers and the pre-row engine. A tie or loss
  cannot close a full-victory contract.
- Downloads: testing tools, SDKs, emulators and peer libraries for local or CI testing are pre-approved (user,
  2026-10-30); pin each in `cmake/pins.json`, unpacked into its own directory. Ask first for anything paid,
  non-testing or licence-restricted. Accounts, billing and credentials are user actions.

## Completion and verification

- Fulfil the entire row, its children and inherited contract: public behaviour, error paths, authored assets,
  persistence/reload and backend evidence; cooking alone is insufficient.
- Keep warnings zero. Match surrounding hand formatting; **do not run `clang-format -i`**. Run the LLVM-20
  incremental tidy helper on changed C++ and confirm it parsed them (BUILDING).
- **CI first:** each item starts by reading the completed CI runs and fixing their failures. Locally build and test
  only changed targets and affected consumers on one primary configuration, plus scoped guards and tidy; CI runs the
  full matrix. Never run full local sweeps; preserve oracles, nonzero test counts, real exit codes and timeouts.
- Use [diagnostic controls](docs/design/runtime-diagnostics.md#shared-acceptance-rules), adversarial cases and
  measured budgets. A schema or emitter is not runtime proof. A parent closes only with every child's evidence.
- Every task ends with a session record and accurate documents per the
  [close-out contract](docs/design/system-quality-contract.md): owning row, context, changed contracts, examples and
  indexes. Preserve historical evidence; add benches, recipes and lessons when warranted; run the validators.
- Qualify actual OS/ISA/API/adapter/driver/workload tuples; unavailable target evidence leaves its gate open.
  Model, agent and plugin inputs never grant authority; commands enforce capabilities and budgets.

## Git and workspace

- Commits are plain Conventional Commits under the configured git identity, with no mention of AI, assistants or
  agents and no `Co-Authored-By` or "Generated with" trailer. Never bypass hooks or signing; fix the cause.
- The serial roadmap workflow commits and pushes each finished item to `main` (user authorization, 2026-10-06).
  A parallel agent commits on its own branch, rebases on `origin/main` before each push and never pushes `main`.
  Any other agent proposes the commit message and the user commits.
- Agent files live only in the NAS [overlay](docs/recipes/workspace-sync.md); code pushes go to GitLab, which
  mirrors to GitHub.
- Windows uses PowerShell. Use `rg` for searches, UTF-8 for text, absolute paths for executables, and verify
  deletion/move targets stay inside the intended directory. Never overwrite HOME/CODEX_HOME.
- Source/project structure changes follow the [synchronizer contract](docs/design/project-structure-sync.md);
  inspect its status before moving files and preserve saved IDE edits, membership overrides and recovery journals.
- Repository organization does not authorize geometry/physics algorithm changes; retained work lives in ROADMAP.
  Historical grants or roadmap order never restart a stopped loop.
