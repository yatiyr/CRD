# ADR-0002 — Logging

**Date:** 2026-04
**Status:** Accepted
**Tags:** [log]

## Decision

- `std::format` as the formatting backend. No fmtlib dependency.
- Hybrid sync + async dispatch. Default sync; async opt-in via `LoggerConfig`.
- `Critical` always bypasses async.
- Compile-time channels via `CRD_DEFINE_LOG_CHANNEL`, registered into a
  global lock-free intrusive list.
- `std::source_location` captures call site automatically.
- Compile-time level stripping via `CRD_LOG_MIN_LEVEL`: Trace in Debug,
  Info in Release. Critical never strips.
- Default sinks at startup are NOT auto-attached. User code adds sinks.
- Shipped graph: `crd-log → crd-core, crd-containers`. `crd-core` never
  depends on log; assert bridge is one-way.

## References

- `docs/phases/phase-1-foundations.md`
- `docs/log/LOG_FILE.md`
