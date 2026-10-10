# ceridc test cases get their own working directories, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [sandbox host records](2026-10-09-diag-9a-sandbox-host-records.md), which found the defect.

## Goal

`ctest -R ceridc -j 4` failed 1 or 2 of the 15 ceridc cases on win-debug, varying between runs, with a Windows
sharing violation in a child ceridc process; run serially, all passed. CTest registers each Catch2 case of
`crd-ceridc-tests` as its own test and runs them at the same time, all in one working directory. This batch gives
every case its own working directory. It is committed locally and not pushed.

## Cause

The cases and the ceridc processes they start write their files in the working directory (`--root .`,
`--diag-root .`, redirected stdout, the cooker's `cook.log.toml`). Most names are per case, but four cases in
`test_ceridc_inspect.cpp` (`inspect --record`, `inspect --seed`, the clock flags and `inspect --events`) all redirect
the child's report into `ceridc_inspect_replay_out.json`. When two of them run at once, the shell's redirect in one
child cannot open the file the other holds ("the process cannot access the file because it is being used by another
process"), and the case fails on the missing `"written":true` report. Unique names would only fix today's collision;
the next shared name would bring it back.

## What changed

- `cmake/CrdTestDiscovery.cmake`: `crd_discover_tests` takes a new option, `CASE_DIRECTORIES`. The generated wrapper
  that CTest loads (after Catch2's PRE_TEST list) gives each discovered case the working directory
  `<WORKING_DIRECTORY>/cases/<first 12 hex digits of the SHA-1 of the case name>` and creates it there, so it exists
  on every lane and configuration without a build step. A hash keeps the long case names (quotes, colons, `--seed`)
  out of the path. Registrations without the option are unchanged.
- `tests/tools/ceridc/CMakeLists.txt`: `crd-ceridc-tests` registers with `CASE_DIRECTORIES`. No test source changed:
  every path a case uses is relative to its working directory or absolute (`CRD_REPO_DIR`, `CRD_CERIDC_EXE`).
- `scripts/test-dev-workflow.py`: the discovery integration fixture (real Catch.cmake and CTest) registers `consumer`
  with `CASE_DIRECTORIES` and checks that its working directory is an existing `cases/<12 hex>` directory under the
  build directory while `setup` keeps the default; `consumer` then runs through CTest in it.
- `docs/design/developer-workflow.md` names the option.

## Verification

- **Before** (win-debug, HEAD `f68da53f`, `ceridc` and `crd-ceridc-tests` rebuilt): `ctest -R ceridc -j 8` failed in
  20 of 20 runs, 2 or 3 cases each (54 failures). In the run read in full, the `--seed`, clock and `--events` cases
  failed; the `--seed` case's output shows the child's sharing violation before the missing `"written":true`.
- **After**, `--show-only=json-v1` lists 15 distinct working directories (20 for the whole executable); the old shared
  directory was emptied first and no case wrote into it again. 20 runs of `ctest -R ceridc -j 8` and a serial run of
  all 20 cases (`ctest --test-dir tests/ceridc -j 1`):
  - win-debug: 20 of 20 parallel runs passed (15/15 each); 20 more runs of all 20 cases at `-j 8` passed; serial 20/20.
  - win-shipping: 20 of 20 parallel runs passed; serial 20/20.
  - win-clang-cl-shipping: 20 of 20 parallel runs passed; serial 20/20 (both targets linked first time).
  - linux-gcc-debug (WSL): 15 distinct directories (same hashes as on Windows); 20 of 20 parallel runs passed;
    serial 20/20. The Linux lane was not measured before the change; POSIX opens do not refuse a shared file, so
    the collision there would show as a wrong report rather than a sharing violation.
- `test-dev-workflow.py --integration --catch-dir <Catch2 extras>` (Ninja, inside the MSVC environment): 54 unit tests
  OK and all three integration passes. Tooth: with `CASE_DIRECTORIES` removed from the fixture's `consumer`
  registration, the run fails with "Case working directories were not applied as registered"; restored, it passes.
- `test-repository-tools.py` OK; `ctest -R "^crd-no-|^crd-check" -j 8` 8/8 on win-debug; `check-master-plan.py` and
  `check-repository.py` PASS; `git status` shows only the intended files. No C++ changed, so tidy, clang-format and
  the Allman check have nothing new to read.
