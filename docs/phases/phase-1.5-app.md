# Phase 1.5 — Application skeleton

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
