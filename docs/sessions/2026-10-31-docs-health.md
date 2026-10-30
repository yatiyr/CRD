# 2026-10-31 — Documentation health: a whole-documentation drift audit and its guards

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## The user's order (verbatim, 2026-10-31)

<a id="user-order"></a>

> "do not forget to update all the agent related docs and the whole docs. Everything needs to be fully healthy and
> not drifted from now on! Including the project readme and all the other markdowns and docs! Everything needs to be
> fully serious"

The audit ran on the `docs-health` branch beside the serial workflow (SIMD-DISPATCH on `main`) and the workspace
overlay agent; it touched no engine, test, CI-orchestrator or workspace file and ran Python only.

## Scope and classification

All 1,450 Markdown files of the tree were inventoried by their `doc-role` marker and location:

| Class | Where | Treatment |
|---|---|---|
| Entry points | README, START_HERE, AGENTS, MEMORY, context, docs/README, BUILDING, CODING, CONTRIBUTING, PRINCIPLES, SANITY | every claim checked against the tree; corrected |
| Living contracts and maps | undated `docs/design/*`, `docs/systems/*`, `docs/hints/*`, `docs/protocols/*`, top-level `docs/*.md`, every directory README, tool READMEs outside docs/ | facts, paths, commands, presets, tone |
| Decisions | `docs/decisions/*` (131 records) | history kept; supersession notes made reciprocal; index checked |
| Dated evidence | `docs/sessions`, `docs/bench` boards, `docs/archive`, `docs/research`, numbered lessons, band-prefixed design notes, the memory corpus | never rewritten; links and index membership only |

The finished inventory, the drift classes with counts and every fix are recorded below as the work lands.
