# Cerid — current pointer

<!-- doc-role: pointer -->
> Current-work pointer. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

<!-- current-slice: REPO.DEV -->
**Current work:** [REPO.DEV](docs/ROADMAP.md#slice-repo.dev), the first Open row: the infrastructure close, gated on
qualification of the first complete-tier hosted run and its repairs.
Next programme: [DIAG](docs/ROADMAP.md#slice-diag.0) immediately after REPO.DEV;
[implementation contract](docs/design/runtime-diagnostics.md). Planning did not start implementation or a loop.
**Only tracker:** [ROADMAP](docs/ROADMAP.md). Rules: [AGENTS](AGENTS.md); lessons: [MEMORY](MEMORY.md).

## Product direction

Entire retained real-time/offline renderer → complete crd-ui and **CR-D007** → hesap GPU/notebook → media and
preserved programmes. Windows/Linux first; macOS/web follow. Notebook = a CR-D007 workspace plus a host.
[ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) owns that order; the master table owns its state.
CHIR authors behaviour, CEIR executes, CKIR expresses device programs. Every shipping algorithm is an editable asset.

## Evidence and boundaries

Last engine milestone: [CEIR-35 close](docs/sessions/2026-09-11-ceir-35z-band-close.md); execution foundation only.
Handoff: [nightly CI repairs](docs/sessions/2026-10-04-nightly-ci-repairs.md); earlier:
[complete-tier repairs](docs/sessions/2026-09-14-first-complete-tier-run-repairs.md).
IDE: [configurations](docs/design/visual-studio-configurations.md); [sync/recovery](docs/design/project-structure-sync.md).
Entry: [START_HERE](START_HERE.md); routes: [CONTRIBUTING](docs/CONTRIBUTING.md).
Collaboration/multiplayer contracts: [ADR-0130](docs/decisions/0130-system-qualification-and-agent-driven-products.md).

User assignment 2026-10-04: work serially, no parallel agents; make CI fully green first. Changes go to CI without
local whole-tree builds/tests; read CI, repair, repeat. Every lane failed on `660a085` since 2026-09-17; the handoff
records the twelve causes, the repairs awaiting the next push plus a nightly/manual run, and two open user
decisions (DIAG.1b hosted TSan; DIAG.6c/7a closure). Pending gates remain in ROADMAP; the
[audit](docs/sessions/2026-09-14-infrastructure-audit.md) maps child evidence and retained qualification.
ADR-0107 and RAH-0 reviews remain gates. Existing hardware; user alone commits/pushes; no invented approval or test.
