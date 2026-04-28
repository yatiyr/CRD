# Phase 4 — Extensibility

**Status:** ⏳ planned

Cerid stops being "the engine" and becomes "the substrate other people
build on."

## Slices

| Slice | Module / Topic                       | Notes                                                                |
| :---: | ------------------------------------ | -------------------------------------------------------------------- |
| 4.0a  | `crd-scripting` C++ hot-reload       | DLL reload via DynamicLibrary; type-stable boundaries                |
| 4.0b  | C ABI plugin boundary                | stable C facade for third-party extensions; versioned                |
| 4.0c  | scripting cookbook                   | published patterns: gameplay tick, custom layers, asset hooks        |
| 4.1a  | `crd-math` dense numerical           | small dense solves, factorisations, least squares                    |
| 4.1b  | `crd-math` sparse + iterative        | CSR / CSC, CG, BiCGSTAB, GMRES, preconditioners                      |
| 4.1c  | `crd-math` parallel solvers          | task-graph driven via `crd-jobs`; SIMD specialization                |
| 4.1d  | `crd-math` robust geometry           | exact predicates, clipping, hulls, intersection robustness           |

## Decisions

(none yet — slices will produce ADRs as they're designed)
