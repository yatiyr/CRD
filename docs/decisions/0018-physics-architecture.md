# ADR-0018 — Physics architecture

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Date:** 2026-04
**Status:** ⚠ **Superseded by ADR-0062 (2026-05-10)** — Cerid no longer
plans to wrap PhysX as a first backend. The Cerid-native physics module
(name: **eylem**) is built from day 1. Reasoning + revised slice plan
in ADR-0062 + ADR-0063 + `docs/phases/phase-3.1-eylem.md` +
`docs/research/cerid-eylem.md`.
**Tags:** [physics] [arch] [superseded]

## Decision

- `crd-physics` is the Cerid-owned interface. PhysX is a backend, not the
  API. Public surface knows nothing about `Px*` types.
- `crd-physics-physx` is the first backend so the interface is exercised
  against a mature implementation early.
- `crd-physics-native` is Phase 6, replacing PhysX as the default while
  keeping PhysX pluggable. Determinism + fixed step are first-class
  native goals.
- Parity test suite lives alongside both backends to catch divergences.

## References

- `docs/phases/phase-3-simulation.md`
- `docs/archive/2026-09-12-superseded-plans.md#phase-6-native-physics`
