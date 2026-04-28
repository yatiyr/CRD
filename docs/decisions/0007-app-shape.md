# ADR-0007 — `crd-app` shape

**Date:** 2026-04
**Status:** Accepted
**Tags:** [app] [event]

## Decision

- Hazel-style virtual `Event` hierarchy, not tagged-union. RTTI-free via
  per-type static token. Stack-allocated, dispatcher takes a reference.
- Layer ownership: `Application::push_layer(unique_ptr<Layer>)`.
- `crd-app` depends only on `crd-core / crd-log / crd-containers /
  crd-platform`. Render-aware layers are written downstream.
- EventBus is sync for now. Async stays a later extension.
- Handled semantics belong only to propagated events. Bus events are
  broadcast, not consumable.
- Application is NOT a singleton.
- LayerStack is application composition, not a catch-all engine
  architecture rule.

## References

- `docs/phases/phase-1.5-app.md`
- `docs/systems/app.md`
