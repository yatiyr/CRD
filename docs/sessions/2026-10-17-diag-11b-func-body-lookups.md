# DIAG.11b: func.func body lookups that tests dereferenced, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Previous batch:
> [scene and öbek packs at the trust boundary](2026-10-17-diag-11b-scene-pack-trust.md), which queued follow-ups (k),
> (l) and (m). The accessor used here came from the
> [link-time null-dereference batch](2026-10-17-diag-11b-link-time-null-dereference.md).

## Goal

Follow-up (k). `func::func_body_block(op)` is the lookup that may miss: it returns nullptr for a null op or an op
without a body region. About 200 test sites, and one engine site, dereferenced its result without a check. Each was a
null path that the optimizer can inline into the dereference. GCC's link-time `-Wnull-dereference`, which the IPO lanes
now make fatal, then fails the link of whatever edit changes the inlining. That happened once already: an unrelated
edit to `test_inspect.cpp` failed crd-ceir-tests' link at `test_hello.cpp:59`
([previous note](2026-10-17-diag-11b-zstd-configure-and-pause-landing.md#a-link-time-site-the-edit-exposed)). Sites
that dereference should take `func::func_body`, the accessor that is fatal in every build when the body is missing.
Sites that test a missing body keep the lookup. Committed locally, not pushed.

## What changed

**The conversion (213 sites in 63 files).** A script rewrote each dereferenced lookup in place:

- `func_body_block(x)` used as a block pointer became `&func::func_body(*x)`;
- `func_body_block(x)->m` became `func::func_body(*x).m`.

It covered every namespace spelling (`func::`, `fn::`, `ce::func::`, `ceir::func::`, `crd::ceir::func::`). The 31
helpers that returned the lookup keep their `Block*` return type and now return `&func::func_body(*f)`, so no caller
changed. Every line keeps its length or gets shorter, so no 120-column limit is at risk.

| File | Sites |
| --- | --- |
| `tests/execution/ceir/test_plan.cpp` | 42 |
| `tests/execution/ceir-host/test_host_provider.cpp` | 32 |
| `tests/execution/ceir/test_exec.cpp` | 18 |
| `tests/execution/ceir-gpu-dx12/test_ceir_pipeline_dx12.cpp` | 18 |
| `tests/execution/ceir/test_program_asset.cpp` | 8 |
| `tests/execution/ceir/test_task.cpp` | 7 |
| `tests/execution/ceir/test_async.cpp`, `tests/execution/ceir-cook/test_hot_reload.cpp` | 6 each |
| `tests/media/audio/test_ceir_audio_render.cpp` | 5 |
| `test_host_parallel_bounds.cpp`, `test_reload_migration.cpp` | 4 each |
| `test_plan_perf.cpp`, `test_band10_gate.cpp` | 3 each |
| `test_tensor_fft.cpp`, `test_builder.cpp`, `test_host_session_record.cpp`, `test_host_replay.cpp`, `test_ceir_pipeline_cuda.cpp`, `test_inspect_host.cpp`, `test_band12_gate.cpp` | 2 each |
| 41 other test files, `tests/gpu/gpu-shared/ceir_render_triangle.hpp`, `engine/execution/chir/src/lower.cpp` | 1 each |

**The engine site.** `ChirLowerer`'s handler lowering (`engine/execution/chir/src/lower.cpp:448`) checked the op that
`create_func` returned, but not its body. It then passed the body to `lower_state_decl`, `lower_query`,
`lower_parallel_for` and `lower_await`, and all four dereference it. Because `create_func` has just made the op, the
body is an invariant here, so the site now takes `func::func_body`. The usage example in `crd/ceir/builder.hpp`'s
header comment now shows the same accessor. Only that one comment line changed, so the header's code and line numbers
are the same.

**The functions the bodies come from.** `func::create_func` returns nullptr on an empty or duplicate name. The previous
batch's rule for crd-ceir-tests' builders was to `REQUIRE` the func and take `func::func_body` for the body. This batch
applies that rule to the converted executables:

- 107 definitions directly through `create_func` now have `REQUIRE(f != nullptr)` before their first use. Three of
  them are in `test_hello.cpp` and `test_inspect.cpp`, which the previous batch had moved to `func_body` without the
  `REQUIRE`;
- so do two `ModuleBuilder::func` definitions (`test_builder.cpp`, `test_hello.cpp`) and the three files' `mkfunc`
  helpers (`test_plan.cpp`, `test_exec.cpp`, `test_host_provider.cpp`), which serve the other 89 sites;
- the Catch2-free shared header `ceir_render_triangle.hpp` stops with `CRD_FATAL`, as `corpus.hpp` and
  `rich_graph.hpp` already do.

Fourteen definitions were already checked.

**The kept lookups.** Four test sites test the lookup itself and keep it, each followed by `REQUIRE(<body> != nullptr)`:

- `test_execute.cpp:649`;
- `test_scene_provenance.cpp:140` (a deserialized module's first op);
- `test_dialect.cpp:78`;
- `test_func.cpp:71` (the func dialect's own cases).

The seven engine lookups that test their result also stay as they are:

- `exec.cpp:436` and `:485`;
- `plan.cpp:576`, `:1329` and `:1349`;
- `tensor_pipeline.cpp:562`;
- `hot_reload.cpp:663`.

**A test file no target compiled.** The strict tidy run on the converted files reported
`tests/execution/ceir/test_scene.cpp` as ungated: none of its includes resolved because no compile database has it.
The CEIR-17a scene-dialect cases were never added to crd-ceir-tests' source list, so their two cases (a well-formed
`scene.resolve_*` chain, and the mistyped-operand and phase rejections of `find_scene_misuse`) had never run. The file
is now listed with its CEIR-17a comment. Both cases pass on every lane below. With `find_scene_misuse` made blind (it
returns `None`), the rejection case fails five checks; restored, both cases pass again. A sweep of every
`tests/**/*.cpp` against the win-debug and linux-gcc-debug compile databases found no other unbuilt test source, apart
from these deliberate ones:

- the package consumer, built by `test-package-consumer.py` as its own project;
- the HIP and Metal backend tests, platform-gated;
- the two geometry-convex probes, which `CMakeLists.txt` keeps on disk on purpose, unbuilt.

**A recurrence guard.** `scripts/check_ceir_invariants.{sh,ps1}` (the `crd-ceir-invariants` CTest on every lane) gains
I7. Every `func_body_block(` call outside `func.hpp`/`func.cpp` in `engine`, `tests`, `tools`, `runtime` and `sandbox`
must test its result against nullptr: either on the call's own line, or through the assigned variable within the next
six lines. The rule is lexical. It catches the shape before an inlining change turns it into a link failure. GCC's
link-time `-Werror=null-dereference` on the three IPO lanes remains the gate that judges the program.

## Teeth

| Edit | Seen |
| --- | --- |
| Three unchecked shapes restored from byte-exact backups: an assigned lookup (`test_plan.cpp:101`), a helper's `return func::func_body_block(f)` (`test_compute.cpp:53`), and a chained `func_body_block(...)->arg(0U)` (`test_band8_gate.cpp:174`) | the guard failed (exit 1) under Git Bash, under WSL's gawk and mawk, and under Windows PowerShell 5.1, naming exactly those three lines; restored by copying back and touching, it passes |
| `REQUIRE(b != nullptr)` removed after a kept lookup (`test_execute.cpp:649`) | the guard failed on that line; restored, it passes |
| `func::func_body`'s own fatal path | `crd-ceir-fatal-func-body-missing` (the previous batch's child-process mode) passes on every lane below |
| `find_scene_misuse` made blind (win-debug, `// TEETH-SCENE`) | the newly built `ceir 17a` rejection case failed five checks (1 of 2 cases); restored from the backup, touched and rebuilt, 2 of 2 pass (38 assertions) |

The conversion itself cannot be shown failing site by site: the converted sites linked clean before it, because GCC
did not inline their null paths. Whether the IPO gate still catches this class in today's crd-ceir-tests was checked
at the one known reproducer. `build_hello_hand`'s body (`test_hello.cpp`) was put back to
`func::func_body_block(fn)` from a byte-exact backup (`// TEETH-LINK`), and crd-ceir-tests relinked on
linux-gcc-shipping. The link failed:

    ir.hpp:255:24: error: potential null pointer dereference [-Werror=null-dereference]
        inlined from 'build_hello_hand' at tests/execution/ceir/test_hello.cpp:60:15

Then `lto-wrapper` stopped the link. Restored by copying the backup back (`cmp` equal) and touching it, crd-ceir-tests
relinked with no warning and passed its 580 cases.

## Verification

"The touched suites" are the following, run through their CTest directories on Windows and as executables from a
scratch directory on Linux:

- crd-ceir-tests (580 cases with the two scene cases; 592 CTests with the fatal and drift tests);
- crd-ceir-cook-tests (115), crd-ceir-gpu-tests (140), crd-ceir-host-tests (55), crd-chir-tests (30),
  crd-audio-tests (30) and crd-frame-cook-tests (107);
- crd-ceir-gpu-vulkan-tests (60), on the real GPU on Windows and on lavapipe with the pinned validation layer on
  Linux;
- on Windows also crd-ceir-gpu-dx12-tests (47) and crd-ceir-gpu-cuda-tests (12).

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | every target (`all`), no warning; crd-ceir-tests again after the scene file joined it | the touched suites; the `crd-no-*`/`crd-check*` guards, `crd-ceir-invariants` and `crd-ceir-fatal-func-body-missing` |
| win-shipping | the ten touched test targets, no warning | the touched suites and the guards |
| win-clang-cl-shipping | the ten touched test targets (every link on the first try), no warning | the touched suites and the guards |
| linux-gcc-debug | the eight touched Linux targets, 0 warnings | the touched suites, the ceir fatal CTests and the guards (14) |
| linux-gcc-asan (ASan and UBSan) | the same, 0 warnings | the same, no report |
| linux-clang-tsan (the lane's `TSAN_OPTIONS`) | the same, 0 warnings | the same, no report |
| linux-gcc-shipping | every target (`cmake --build`, `-k 0`) under the link-time warning set with `-Werror`: 261 steps, 41 links (every touched executable and every other consumer of the changed headers), exit 0, no `warning:`, `lto-wrapper` or `FAILED:` line; crd-ceir-tests again for the link tooth and its restore | the touched suites and the guards |
| linux-gcc-release | every target, 261 steps, 41 links, exit 0, no `warning:` line | the touched suites and the guards |
| linux-gcc-relwithdebinfo (asserts compiled in) | every target, 261 steps, 41 links, exit 0, no `warning:` line | the touched suites and the guards |

win-asan was not run: no memory, jobs or sanitizer code changed. Also clean:

- strict tidy (LLVM 20.1.8) on all 66 changed C++ files, `test_scene.cpp` included once it had a compile command;
- `clang-format --dry-run`: no finding on any added or changed line, compared line by line against HEAD, and each
  file's finding count unchanged (no file was reformatted);
- `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py` and `test-repository-tools.py` (75 tests).

## Hosted evidence needed once pushed

Every touched test executable passes on every lane that builds it:

- crd-ceir-tests, crd-ceir-cook-tests, crd-ceir-gpu-tests, crd-ceir-host-tests, crd-chir-tests, crd-audio-tests and
  crd-frame-cook-tests;
- on the device lanes, crd-ceir-gpu-vulkan-tests and crd-ceir-gpu-dx12-tests (crd-ceir-gpu-cuda-tests where CUDA is
  configured).

`crd-ceir-invariants` passes on every lane and prints its I7 clause. linux-gcc-shipping, linux-gcc-release and
linux-gcc-relwithdebinfo link every one of these executables under the link-time warning set with no `warning:` line.

## Queued

- (l) the zstd 1.5.7 pin, once fetching its archive is approved (a user step), as queued in the
  [zstd note](2026-10-17-diag-11b-zstd-configure-and-pause-landing.md#follow-up-for-an-extra-batch).
- (m) the unchecked allocation results outside the scene, resources and memory modules, as listed in the
  [unchecked-allocations note](2026-10-17-diag-11b-unchecked-allocations.md#queued), without `SharedComponentPool`.
- (n) a recurrence guard against test sources that no target compiles. A lexical rule ("every `tests/**/*.cpp` is
  named by its directory's `CMakeLists.txt` unless that file globs") was probed. It leaves the 159 globbed files
  unchecked, and it misreads six sources that another directory's `CMakeLists.txt` names: `win32_test_window.cpp`
  (named by the two device suites), and the five `*_fatal_specimen.cpp` (each named by its owning suite through
  `${CMAKE_SOURCE_DIR}`). The guard therefore needs the configured trees' compile databases: the union of the
  Windows and Linux trees, the way this batch's sweep took it. It also needs a named list of the deliberate
  exclusions: the package consumer, the HIP and Metal backend tests, and the two geometry-convex probes.
