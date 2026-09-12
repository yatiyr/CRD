> **Dated phases reference.** Current order/status: [ROADMAP](../../ROADMAP.md). Read [AGENTS](../../../AGENTS.md) for current rules.
> Old Next lists, API examples and implementation claims below require current-source verification. Unique requirements remain in force through their owning master rows.

# Phase 1.5 — Application skeleton

<!-- doc-role: archive -->
> Historical reference; old status/schedules/grants are not current instructions. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> **Tracking consolidated 2026-09-12:** this file retains detailed requirements and dated evidence.
> Live order/status/subslices are only in [ROADMAP](../../ROADMAP.md#master-table). Earlier schedules and Next
> pointers below are historical; current ownership/sequence follows ADR-0129 and the execution contract.

**Status:** ✅ shipped (2026-04)

Sits between core foundations and graphics. Lands before RHI so the main
loop, layer stack, and event routing exist when the first triangle arrives.

## Slices

| Slice | Topic                  | Notes                                                                |
| :---: | ---------------------- | -------------------------------------------------------------------- |
| 1.5a  | `Event` hierarchy      | virtual, RTTI-free, per-type static token; stack-allocated dispatch  |
| 1.5b  | `LayerStack`           | overlays at tail; update bottom-up, dispatch top-down                |
| 1.5c  | `Application`          | owns loop, lifts platform `InputEvent` to `Event`; not a singleton   |
| 1.5d  | `EventBus` (sync)      | typed broadcast bus; user-defined event types via static tokens      |
| 1.5e  | first real layer       | smoke runtime wires a custom layer through Application               |

## Decisions

- ADR-0007 — `crd-app` shape

## Notes

- EventBus is sync for now. Async stays a later extension.
- `crd-app` depends only on `crd-core / crd-log / crd-containers /
  crd-platform`. Render-aware layers are written downstream by user code.
- Application is NOT a singleton — ownership stays explicit and testable.
