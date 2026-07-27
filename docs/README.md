# Cerid — Documentation Map (Start Here)

> **The single entry point to the whole doc system.** New here — agent or human?
> Read this first, then follow the reading order below. This file is a **map: pointers
> only.** It never duplicates the content it links to, and it is the *one* canonical
> reading order — every other doc points here instead of carrying its own copy.

---

## Reading order (onboarding)

Read these in order; they go from stable rules → live state → the task at hand.

1. **`AGENTS.md`** (repo root) — the rulebook: principles, conduct, coding standards (canonical), Definition of Done, git policy, architectural cornerstones.
2. **`docs/BUILDING.md`** — build presets, verification protocol, smoke tests, platform notes, and ALL troubleshooting. (An untracked local `CLAUDE.md` may exist as an AI-session pointer into these generic docs; project knowledge never lives there.)
3. **`docs/PRINCIPLES.md`** — non-negotiable engineering principles + pinned cornerstones. Short.
4. **`docs/SANITY.md`** — engineering sanity doctrine + the living **Sanity Ledger**. Short. Claim one ledger item when you have slack.
5. **`context.md`** (repo root) — live "where we are now": current focus, last shipped, next up, test counts.
6. **`docs/ROADMAP.md`** — navigation hub: phase status, decision log (tagged), detour queue, glossary. A hub — don't read end-to-end; follow the link for your task.
7. **The active phase doc** under `docs/phases/` — its name is in `context.md`. (If the active work is a
   DETOUR, the detour file under `docs/detours/` is the equivalent — `context.md` names it.)
8. **The slice's design spec** under `docs/design/`, if it has one — the slice's ROW links it by path, and
   `docs/design/README.md` is the reverse lookup. **If you were told "read the slice and implement it", this
   is the document you were sent to**: it carries the reuse audit (what already exists, so you don't rebuild
   it), the sequenced increments, and the gate for each.

Then, as your task needs, pull the relevant ADR(s), system overview(s), and research dossier(s) from the map below.

---

## The doc system — what lives where

| Area | Where | Purpose | Index / entry | Class |
|---|---|---|---|---|
| Project rules | `AGENTS.md`, `docs/BUILDING.md` | Stable rulebook + build/verification reference | — | living |
| Principles | `docs/PRINCIPLES.md` | Engineering compass, pinned cornerstones | — | living |
| Benchmarking policy | `docs/PRINCIPLES_reference_class_benchmarking.md` | Head-to-head-vs-strongest-references rule for every numerical kernel | — | living |
| Crush hints | `docs/hints/crush-playbook.md` | Living playbook of crush/optimization levers + traps (bench fairness, algorithmic wins, SIMD/memory, cross-config miscompiles) — read before a crush, append after one | self | living |
| Sanity | `docs/SANITY.md` | Sanity doctrine + ledger (the *operational* rules) | self | living |
| Lessons | `docs/lessons/` | Meta-reflective teaching lessons — the *why* + reusable patterns (sibling of sessions; deeper than SANITY) | `docs/lessons/README.md` | append-only |
| Live status | `context.md` | Where we are now | — | living |
| Navigation hub | `docs/ROADMAP.md` | Phases, decision log, detours, glossary | self | living |
| Decisions (ADRs) | `docs/decisions/` | One decision per file (`NNNN-slug.md`) | `docs/decisions/README.md` | append-only |
| System overviews | `docs/systems/` | One short overview per shipped module | `docs/systems/README.md` | mixed |
| Module deep-dives | `docs/<module>/<MODULE>_FILE.md` | Long-form per major module (log, memory, containers) | — | append-only |
| Phase plans | `docs/phases/` | One file per phase, detailed slices | ROADMAP status table | living (active) / append-only (closed) |
| **Slice design specs** | `docs/design/` | **The implementation CONTRACT for one slice — the mandatory REUSE AUDIT (what already exists, so it is not rebuilt), the sequenced increments, the gate per increment, named risks + non-goals. Sits between the row and the session log. The slice's ROW links it by path; this index is the reverse lookup.** | `docs/design/README.md` | living until the slice closes |
| **Authored GPU assets** | `assets/frame/*.frame.toml` · `assets/technique/*.crdt` · `assets/vertex/*.crdv` · `assets/lighting/*.crdl` · `.crdm` | **The five declarations that describe every GPU program the engine runs — SCHEDULE (frame graph) · LIGHTING MODEL (technique) · GEOMETRY STAGE · LIGHT SET · SURFACE. Cooked by `crd-frame-cook` / `crd-technique-cook` / `crd-vertex-cook` / `crd-light-cook` / `crd-material-cook` into CKIR. ⛔ If a rendering change needs a recompile, it belongs in one of these instead.** | the module’s header comment | living |
| Session logs | `docs/sessions/` | One per work session (`YYYY-MM-DD-slug.md`) | — | append-only |
| Research dossiers | `docs/research/` | Deep research per topic | — | append-only |
| **Recipes** | `docs/recipes/` | **Educative build-recipes — teach a studied technique end to end (parameters first, then physics, assembly, traps). Written whenever we study + build something (AGENTS.md rule).** | — | append-only |
| Open debt | `docs/debt.md` | Cleanups not yet done; pruned to a session log when closed | self | living |
| Detours | `docs/detours/README.md` | Side-mission queue | self | living |
| Protocols | `docs/protocols/` | Process protocols (e.g. per-slice verification) | — | reference |
| Benchmarks | `docs/bench/` | Captured baseline numbers | — | append-only |
| Templates | `docs/sessions/SESSION_TEMPLATE.md`, `docs/research/RESEARCH_TEMPLATE.md` | Format starters | — | reference |

---

## Doc design rules (so the system stays lean, not uniformly short)

The docs fall into **two classes**, and only one gets size discipline:

- **Living / scannable** — `context.md`, `docs/ROADMAP.md`, this map, the index READMEs, `MEMORY.md`, `docs/debt.md`, `docs/detours/README.md`. These **rot** when bloated (you skim them constantly), so they get **soft budgets + prune discipline**: keep them lean, delete stale content, never let one grow into a wall of text. Soft budgets: `context.md` ≤ 300 lines; this map + the index READMEs ≤ ~150 lines.
- **Append-only historical records** — session logs, ADRs, research dossiers, module deep-dives. **Length is fine; do NOT truncate them** — truncating destroys history. A 600-line dossier is healthy; a 600-line `context.md` is rot.

Cross-cutting rules:
- **One home per fact; everywhere else points to it.** Don't copy a reading-order list, a status table, or an explanation into a second file — link instead. (SANITY rule #3: an artifact too verbose or duplicated to trust is worse than none.)
- **Per Definition of Done:** a new ADR adds a one-line row to `docs/decisions/README.md`; a new shipped module adds a row to `docs/systems/README.md`.
- **"Carefully designed" ≠ "uniformly short."** Prune the scannable; preserve the historical.

---

Known living-doc bloat (which scannable docs need pruning, and how) is tracked — dated and
actionable — in the **Sanity Ledger** (`docs/SANITY.md`). Not restated here: point-in-time
sizes don't belong in an evergreen map.
