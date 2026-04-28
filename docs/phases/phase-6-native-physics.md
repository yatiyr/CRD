# Phase 6 — Native physics

**Status:** ⏳ planned

Cerid replaces PhysX with its own backend. PhysX remains pluggable.

## Slices

| Slice | Topic                                | Notes                                                                |
| :---: | ------------------------------------ | -------------------------------------------------------------------- |
| 6.0a  | broadphase                           | dynamic AABB tree / SAP                                              |
| 6.0b  | narrowphase + contact manifolds      | GJK / EPA, primitive specializations                                  |
| 6.0c  | constraint solver                    | sequential impulses → eventually nonlinear; islands; sleeping         |
| 6.0d  | continuous collision                 | TOI for fast bodies                                                   |
| 6.0e  | determinism + fixed step             | deterministic across machines under fixed step                        |
| 6.0f  | parity tests vs PhysX                | side-by-side regression suite for switching                           |

## Decisions

- ADR-0018 — Physics architecture (this is the native-backend phase)
