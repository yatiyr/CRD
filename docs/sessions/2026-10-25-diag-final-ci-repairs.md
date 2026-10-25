# DIAG.12: repairs after complete run 40150944943, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contracts:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [the zstd 1.5.7 pin and clang-tidy 20 on the WSL host](2026-10-25-diag-11b-zstd-and-wsl-tidy.md).

## CI read

Complete-tier run [40150944943](https://github.com/yatiyr/CRD/actions/runs/40150944943) on `b0f112ad` failed 16
jobs (the failed-job logs, saved locally, were searched, not read whole). Three causes:

| Jobs | Failure | Cause |
|---|---|---|
| 12 Windows jobs (win-debug, win-debug-sse2, win-debug-scalar, win-release, win-relwithdebinfo, win-asan, win-public-checks, win-clang-cl, win-shipping, win-shipping-profile, win-clang-cl-shipping, win-vs) and Repository checks (windows-2025-vs2026) | `crd-repository-tools`: `test_clang_tidy_installer_keeps_the_resource_layout_and_checks_digests_count_and_version`, `runneradmin` path `!=` `RUNNER~1` path | the test compared the installer's resolved path with its own short spelling |
| linux-gcc-release, linux-gcc-shipping | `test_diag_crash_capture.cpp:76: 'read_marker' defined but not used [-Werror=unused-function]` | Linux reads the marker only in a case that needs asserts |
| linux-gcc-asan | seeded-fault step: `lsan-suppression-names-cerid-source` SURVIVED | the seed suppressed `leak_now`, a site renamed to `crd_diag_leak_site` |

linux-gcc-asan's mikktspace oracle failure is the registered third-party failure TP-5; the lane's gate passed with
exactly the registered third-party failures. Not chased.

## 1. The clang-tidy installer test and the 8.3 temp path

`scripts/install-clang-tidy.py` resolves its destination, like every other installer here (`install-warp.py`,
`install-genmc.py`, `install-clang-build-analyzer.py`). The test built its expectation from the unresolved
`tempfile.TemporaryDirectory()` path, which on a hosted runner is the 8.3 short name `C:/Users/RUNNER~1/...`. The
test was wrong. It now starts from `Path(temp).resolve()`, as the ClangBuildAnalyzer installer test already did.

Teeth: with `TEMP` and `TMP` set to `C:\Users\abici\CERID~1\EIGHTD~1` (the 8.3 spelling of a long-named directory
under `%USERPROFILE%\.cerid`), the old test failed with the hosted assertion (long path `!=` `CERID~1/EIGHTD~1`
path); the fixed test passed. The whole `scripts/test-repository-tools.py` passed with the short `TEMP` (122 tests;
3 skip because CMake and Ninja are not on that shell's PATH) and with the normal one.

## 2. `read_marker` on Linux without asserts

Commit 39dddbe6 moved `read_marker` into the shared helpers. Windows reads it in every configuration (the
`[crash-capture]` cases outside `#if CRD_ENABLE_ASSERTS`). Linux reads it only in the injected-write-failure case,
which sits inside `#if CRD_ENABLE_ASSERTS`; linux-gcc-release and linux-gcc-shipping set
`CRD_ENABLE_ASSERTS=OFF`, so they compiled a helper with no caller. The helper is now defined under
`#if defined(_WIN32) || (defined(__linux__) && CRD_ENABLE_ASSERTS)`: exactly where it is used. No
`[[maybe_unused]]`, and no case changed.

Teeth, on the WSL mirror trees: the old source's object failed on both lanes with the hosted error
(`test_diag_crash_capture.cpp:76:5: error: ... defined but not used [-Werror=unused-function]`); with the fix,
`crd-core-tests` built and linked on linux-gcc-release (a new tree) and linux-gcc-shipping.

## 3. The LSan suppression seed

Commit 63cef643 renamed the leak specimen's site `leak_now` to `crd_diag_leak_site` (the `CRD_DIAG_SITE` sites a
catch must name). The seed still added `leak:leak_now`, which matched nothing, so the leak case still caught the leak
and the seed survived. The specimen is right; the seed was stale. It now adds `leak:crd_diag_leak_site`.

The registry check had no teeth for this: `stale_edit` held a seed's edits and `from` files to the tree but not the
entries a seed writes into a suppression file. Now each such entry (other than a third-party `lib*.so*`) must name a
file under `tests/support/diag/specimens` or a whole-word symbol in one of its sources, or the seed is
`REGISTRY_STALE`. A new verdict test covers a gone symbol, a gone file, a prefix of a real symbol, an entry with no
kind, and a file that is not a suppression file. With the old seed text, `diag-seeded-faults.py --validate` (the
`crd-diag-seeded-faults` check) now reports `REGISTRY_STALE: lsan-suppression-names-cerid-source: file
lsan-suppressions.txt: leak:leak_now names no specimen source or symbol under tests/support/diag/specimens`.

On the linux-gcc-asan mirror tree:

- the seed alone, `--expect DETECTED`: the control passed with `ERROR: LeakSanitizer: detected memory leaks` and the
  frame `crd_diag_leak_site .../leak_specimen.cpp:40` (the case still catches a real leak); seeded, LSan printed
  `Suppressions used: 1 1024 crd_diag_leak_site` and the case failed with `verdict=Unexpected`: DETECTED.
- the whole lane, as the hosted step runs it: 7 seeds DETECTED, 5 not applicable, matching the pinned
  linux-gcc-asan count of 7 in `scripts/test-diag-seeded-faults.py`.

## Checks

On win-debug: `ctest -R "^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$"`,
`scripts/check-master-plan.py` and `scripts/check-repository.py` passed.

## Hosted only

The next complete-tier run must show `crd-repository-tools` green on every Windows job and Repository checks (the
real `RUNNER~1` temp directory), linux-gcc-release and linux-gcc-shipping building, and the linux-gcc-asan seeded
step DETECTED for all 7 seeds, together with every other "Hosted once pushed" clause of DIAG.12.
