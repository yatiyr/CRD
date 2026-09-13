# Cerid — current pointer

<!-- doc-role: pointer -->
> Current-work pointer. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

<!-- current-slice: REPO.DEV.3b.3 -->
**Current work:** [REPO.DEV.3b.3](docs/ROADMAP.md#slice-repo.dev.3b.3), the first truly Open row: portable strict LLVM-20 analysis.
**Only tracker:** [ROADMAP](docs/ROADMAP.md). Rules: [AGENTS](AGENTS.md); lessons: [MEMORY](MEMORY.md).

## Product direction

Entire retained real-time/offline renderer → complete crd-ui and **CR-D007** → hesap GPU/notebook → media and
preserved programmes. Windows/Linux first; macOS/web follow. Notebook = one CR-D007 workspace plus standalone host.
[ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) owns that order; the master table owns its state.
CHIR authors behaviour, CEIR executes, CKIR expresses device programs. Every shipping algorithm is an editable asset.

## Evidence and boundaries

Last engine milestone: [CEIR-35 close](docs/sessions/2026-09-11-ceir-35z-band-close.md); execution foundation only.
Latest handoffs: [guard/tidy repairs](docs/sessions/2026-09-13-ci-guard-tidy-repairs.md),
[route + pinned WARP](docs/sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md), [REPO.DEV audit](docs/sessions/2026-09-13-repo-dev-audit.md).
IDE: [CMake configurations](docs/design/visual-studio-configurations.md); [sync/recovery](docs/design/project-structure-sync.md).
Entry: [START_HERE](START_HERE.md). Architecture findings: [A01–A27](docs/research/2026-09-12-system-audit.md) and
[G01–G30](docs/research/2026-09-12-cerid-whole-system-review.md). All owners are in ROADMAP.
Collaboration and multiplayer contracts: [ADR-0130](docs/decisions/0130-system-qualification-and-agent-driven-products.md).

User assignment 2026-09-13: fix every CI failure, then complete every row before the first truly Open slice, in
[order](docs/ROADMAP.md#strict-sequential-execution), driven by a per-minute session loop. REPO.3c.3 through 3c.9 closed
on run 34757652779; REPO.3c.10 and REPO.3c wait for the [third-party register](docs/third-party-defects.md) gate's first
hosted run. The full ADR-0107 and RAH-0 reviews remain gates. Existing hardware; user alone commits/pushes; no invented approval or test.
