# Cerid — current pointer

<!-- doc-role: pointer -->
> Current-work pointer. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

<!-- current-slice: REPO.3c.7 -->
**Review handoff:** [REPO.3c.7](docs/ROADMAP.md#slice-repo.3c.7), native inner-coverage mismatch and exact reproduction.
**Only tracker:** [ROADMAP](docs/ROADMAP.md). Rules: [AGENTS](AGENTS.md); lessons: [MEMORY](MEMORY.md).

## Product direction

Entire retained real-time/offline renderer → complete crd-ui and **CR-D007** → hesap GPU/notebook → media and
preserved programmes. Windows/Linux first; macOS/web follow. Notebook = one CR-D007 workspace plus standalone host.
[ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) owns that order; the master table owns its state.
CHIR authors behaviour, CEIR executes, CKIR expresses device programs. Every shipping algorithm is an editable asset.

## Evidence and boundaries

Last engine milestone: [CEIR-35 close](docs/sessions/2026-09-11-ceir-35z-band-close.md). This establishes its recorded
execution-foundation contract, not complete renderer/UI/editor quality. The old CEIR autonomous grant is complete.
Latest handoff: [Atomic/coverage verification](docs/sessions/2026-09-13-atomic-and-coverage-verification.md).
IDE: [CMake configurations](docs/design/visual-studio-configurations.md); [sync/recovery](docs/design/project-structure-sync.md).
Entry: [START_HERE](START_HERE.md). Architecture findings: [A01–A27](docs/research/2026-09-12-system-audit.md) and
[G01–G30](docs/research/2026-09-12-cerid-whole-system-review.md). All owners are in ROADMAP.
AI inference/training qualify together. Project collaboration and multiplayer have separate authoritative contracts
([ADR-0130](docs/decisions/0130-system-qualification-and-agent-driven-products.md)); detailed mechanisms remain proposals.

The full ADR-0107 and RAH-0 migration review remain gates. No unavailable approval or test is invented.
[Large-C++ research](docs/research/2026-09-12-large-cpp-development-and-ci.md) decomposes remaining work in the same table.
User stopped this repository loop on 2026-09-13; its heartbeat is paused. This Codex task is now review-only.
Future implementation needs a new user assignment and follows [order](docs/ROADMAP.md#strict-sequential-execution).
Retain every pending gate. Existing hardware; no renderer implementation. User alone commits/pushes.
