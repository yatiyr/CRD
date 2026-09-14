# Cerid — current pointer

<!-- doc-role: pointer -->
> Current-work pointer. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

<!-- current-slice: REPO.DEV -->
**Current work:** [REPO.DEV](docs/ROADMAP.md#slice-repo.dev), the first Open row: the infrastructure close, gated on
the first complete-tier hosted run of the maintainer's next push.
**Only tracker:** [ROADMAP](docs/ROADMAP.md). Rules: [AGENTS](AGENTS.md); lessons: [MEMORY](MEMORY.md).

## Product direction

Entire retained real-time/offline renderer → complete crd-ui and **CR-D007** → hesap GPU/notebook → media and
preserved programmes. Windows/Linux first; macOS/web follow. Notebook = a CR-D007 workspace plus a host.
[ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) owns that order; the master table owns its state.
CHIR authors behaviour, CEIR executes, CKIR expresses device programs. Every shipping algorithm is an editable asset.

## Evidence and boundaries

Last engine milestone: [CEIR-35 close](docs/sessions/2026-09-11-ceir-35z-band-close.md); execution foundation only.
Handoff: [route + pinned WARP](docs/sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md).
IDE: [configurations](docs/design/visual-studio-configurations.md); [sync/recovery](docs/design/project-structure-sync.md).
Entry: [START_HERE](START_HERE.md); routes: [CONTRIBUTING](docs/CONTRIBUTING.md).
Collaboration/multiplayer contracts: [ADR-0130](docs/decisions/0130-system-qualification-and-agent-driven-products.md).

User assignment 2026-09-13: fix every CI failure, then complete every row before the first truly Open slice, in
[order](docs/ROADMAP.md#strict-sequential-execution), driven by a per-minute loop. REPO.3c.3 through 3c.10 closed
(runs 34757652779, 34766787633); REPO.3c waits for the
[jobs end-hook repair](docs/sessions/2026-09-13-jobs-end-hook-ordering.md) on the next push; REPO.DEV.3b through
DEV.11 are Needs CI; the [audit](docs/sessions/2026-09-14-infrastructure-audit.md) tabulates every child's evidence,
what the first complete-tier run must show and the retained gates, and links each child's session.
ADR-0107 and RAH-0 reviews remain gates. Existing hardware; user alone commits/pushes; no invented approval or test.
