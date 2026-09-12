# Start here — Cerid for every agent

<!-- doc-role: navigation -->
> Entry route. Rules: [AGENTS](AGENTS.md). Only live plan: [ROADMAP](docs/ROADMAP.md).

**Purpose:** one modular, authorable architecture for games, real-time/offline rendering, creative and engineering
applications. CR-D007 is its native editor. The same public services serve humans, application code and agents.
CHIR authors high-level programs; CEIR defines execution; CKIR defines device kernels. Built-in algorithms are
replaceable assets. A working demo that bypasses those assets does not meet the purpose.

## Orient before acting

1. Read [AGENTS](AGENTS.md), [PRINCIPLES](docs/PRINCIPLES.md), [SANITY](docs/SANITY.md) and the [MEMORY index](MEMORY.md).
2. Read [context](context.md), then its exact [ROADMAP](docs/ROADMAP.md) row, every child, and the linked contract/ADR.
   Check decision status. Historical sessions explain evidence; they cannot grant current approval or redefine scope.
3. Find source ownership in [systems](docs/systems/README.md); inspect the public API, implementation, callers and tests.
   Before code read [BUILDING](docs/BUILDING.md), [CODING](docs/CODING.md) and the relevant
   [quality contract](docs/design/system-quality-contract.md). Load performance mandates for hot-kernel work.
4. State: **purpose; slice ID; complete deliverable; dependencies; evidence required; unresolved decisions**.
   Continue within existing authorization. Ask about material ambiguity, not routine reversible work.

Current delivery order is in [ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md): the entire retained
renderer library, then crd-ui/CR-D007, then the scientific notebook, followed by preserved programmes. Windows/Linux
qualify first; macOS/web follow. Broader [system review](docs/research/2026-09-12-cerid-whole-system-review.md) explains
AI, collaboration, simulation and manufacturing contracts. It is a reference, not another roadmap.

## Work and close the loop

Use existing modules and canonical execution. Implement the complete consumer behaviour, adverse paths, asset
replacement and lifecycle. A source suspicion needs investigation before being called a bug. A verified failure
blocks its owning gate. Track findings and new children in ROADMAP, with exact evidence and ownership.

Before ending **every implementation, fix, investigation or documentation task**, follow the
[close-out procedure](docs/README.md#close-and-hand-off): write a dated session, update the owning row and current
pointer, and inspect every affected contract, API example, index and orientation document for truth. Update changed
facts; retain still-correct text. Record measured full peer boards immediately and write recipes for implemented
research. Preserve historical evidence instead of rewriting it as a new result. Unfinished work gets an honest handoff.

Use [BUILDING's local/CI decision table](docs/BUILDING.md#fast-local-workflow--one-primary-configuration), not every
local configuration/platform. Run the documentation validator. Record unavailable hardware/configurations
as unqualified, with an owning gate; do not turn an emitter, schema, compile or mock into a runtime support claim.
No agent commits or pushes. The user owns the final commit.

```powershell
python scripts/check-master-plan.py --next
python scripts/check-master-plan.py --slice RAH-1
python scripts/check-master-plan.py --find browser
python scripts/check-master-plan.py --memory stale_exe
python scripts/check-master-plan.py
```

If interrupted: leave the exact row, reproduction/commands, files changed, results and next discriminating action
in the session. Never mark work Done merely to make the handoff tidy. The full navigation map is [docs/README](docs/README.md).
