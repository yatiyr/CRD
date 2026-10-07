# DIAG.8a intrinsic refusals name their authored op and native provider, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [entries, generations and CHIR reload](2026-10-07-diag-8a-entries-generations-and-chir-reload.md).

## Goal

Close the DIAG.8a intrinsic clause: an error at an ADR-0110 intrinsic (`[op.native]`) op names its authored op and its
native provider. The CKIR/GPU clause stays open (see the end).

## What was there (checked before coding)

- `OpInfo` already carried `intrinsic` and `native_provider` (promoted for the section 106 dependency record), but no
  diagnostic read them. The reference interpreter's `NoSemantics` and the plan compiler's `UnsupportedOp` named the op
  and nothing about its binding. The crd-jobs provider runs the same `Interpreter::eval_op`, so it reports the same op.
- The host scene resolver `evaluate_scene_resolve` (crd-ceir-gpu, the `RenderResolvers` callbacks that implement the
  `scene.resolve_*` host intrinsics) returned a bare `UnresolvedSceneHandle` for an unwired callback and for a
  callback that returned 0, and a bare `SceneChainMisuse` that dropped the op `find_scene_misuse` had named.
- The text printer and parser round-trip the scene ops with their Extern handle types, and a blob holding them loads
  into a Context where the scene dialect is not registered (unregistered kinds and classes are preserved opaquely).

## What landed

- **Native binding** (`crd/ceir/provenance.hpp`, `context.cpp`): `native_binding(ctx, op)` returns the op name and a
  `NativeKind`: `Intrinsic` with the declared provider, `NotIntrinsic`, `Unregistered` (the kind is not registered in
  this Context, so its binding cannot be known there; never reported as non-intrinsic) or `NoOperation`.
  `render_op_site(ctx, op, out)` prints `<op name>[ native <provider>| native unregistered] at <render_provenance>`,
  so one helper serves every reporter (interpreter, plan compiler, provider, scene resolver) and none of the result
  structs changed.
- **Scene resolver** (`crd/ceir/gpu/render_materialize.hpp`, `render_materialize.cpp`): `SceneResolvedHandles` gains
  `fault` (last member; the struct stays an aggregate and is reset on entry). Every `UnresolvedSceneHandle` sets it to
  the resolve op whose callback is unwired or returned 0, and `SceneChainMisuse` sets it to the op the verifier named.
  A resolved chain leaves it null.

## Tests

Expected positions come from scanning the printed text, never from the parser.

- `tests/execution/ceir/test_provenance.cpp`, new case (`[ceir][diag]`): a program with a `scene.resolve_*` chain, a
  `core.foreach` and an unregistered `vendor.blob` is printed, parsed under a named file, run through the production
  CSE, serialized and loaded into a fresh Context. The reference interpreter refuses `scene.resolve_material` with
  `NoSemantics`; the op resolves to its authored line and column in the file, `native_binding` says `Intrinsic`,
  provider `host`, and the rendered site reads `scene.resolve_material native host at <file>:...`. The plan compiler's
  `UnsupportedOp` blames the same op and renders the same site. Controls: `core.foreach` is `NotIntrinsic` with no
  provider in its rendering; `vendor.blob` is `Unregistered`; the same blob loaded into a Context without the scene
  dialect reports the resolve op as `Unregistered` at the same authored line; a null op renders `no-operation`.
- `tests/execution/ceir-gpu/test_scene_provenance.cpp`, new (2 cases, `[ceir][ceir-gpu][scene][diag]`, device-free):
  the chain is printed, parsed under a named file and loaded from a blob. A technique callback that returns 0, an
  unwired program callback and an unwired first stage are each blamed on their own resolve op, at its authored line
  and column, rendered with `native host`; the stages before the refusal still resolved. A mistyped chain (the draw fed
  to `resolve_technique`) is blamed on the op `find_scene_misuse` names, with no callback run. Control: a resolved
  chain through the same output struct after a refusal leaves `fault` null.

Teeth (win-debug; each restored, the lane rebuilt and both suites rerun green):
- `native_binding` skips the `intrinsic` check: the non-intrinsic control fails (3 assertions);
- the scene refusal helper no longer records the op: the 0-return, unwired and unwired-first-stage sections and the
  control fail (4 assertions);
- the misuse op is dropped: the mistyped-chain case fails.

## Evidence

- **win-debug:** the whole tree builds (`provenance.hpp` is reached through `context.hpp`; `render_materialize.hpp`
  through the crd-ceir-gpu consumers). `crd-ceir-tests` 542 cases (12,805 assertions), `crd-ceir-gpu-tests` 134
  (2,435), `crd-ceir-host-tests` 29 (922), `crd-ceir-cook-tests` 41 (1,913) and `crd-chir-tests` 28 (5,226) pass.
- **win-shipping, win-clang-cl-shipping, win-asan:** `crd-ceir-tests` and `crd-ceir-gpu-tests` build and pass with the
  same counts; no ASan report; the clang-cl link was clean first time. One win-asan compile hit an MSVC internal
  compiler error (C1001, 0xC0000005) on the unchanged generated `test_units_gen_smoke.cpp`; the retry was clean.
- **WSL:** linux-gcc-debug, linux-gcc-asan (with UBSan) and linux-clang-tsan each build and pass all five suites with
  the same counts and no sanitizer report (`crd-ceir-gpu-tests` builds and runs device-free on Linux).
- **Checks:** strict tidy is clean on the 4 changed C++ sources. `clang-format --dry-run`: its one line-wrap finding on
  new code was taken by hand; what remains is the repository's hand alignment, the one-line `case` style the sibling
  `provenance_gap_name` uses, and the unreflowed comment block it extends. The Allman check, the 8 ctest guards,
  check-master-plan and check-repository pass.

## Hosted CI must later show

`crd-ceir-tests` (542 cases) and `crd-ceir-gpu-tests` (134 cases) green on all six lanes, including the new `diag 8a`
cases, and `crd-ceir-host-tests`, `crd-ceir-cook-tests` and `crd-chir-tests` still green (`provenance.hpp` is reached
through `context.hpp`).

## Not covered

- The crd-jobs provider is not tested on an intrinsic program here: it fails through the same `Interpreter::eval_op`
  path, and `render_op_site` reads only the op and the Context.
- The other host intrinsics (`rt.*`, `work.*`) have no host evaluator yet that could refuse; `native_binding` reads
  their binding from the same registration.

## Remaining on DIAG.8a

- CKIR node identity and a GPU validation error navigate to the CEIR dispatch origin (needs device runs).
