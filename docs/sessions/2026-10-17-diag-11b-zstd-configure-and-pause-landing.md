# DIAG.11b: zstd's clang-cl and configure warnings, and where a pause lands, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Rules: [AGENTS](../../AGENTS.md) ("keep
> warnings zero"). Previous batch: [link-time null dereferences](2026-10-17-diag-11b-link-time-null-dereference.md),
> which queued follow-ups (e), (h), (i) and (j).

## Goal

Two of the queued follow-ups: (e) win-clang-cl-shipping's two zstd `-Wunused-function`, and (j) the DIAG.8b
pause-and-cancel case that read `NoSuchValue` for the held op once on linux-clang-tsan. Before choosing the repair for
(e), every hosted job log of complete run 38640651640 was swept for warnings, and the configure warnings that sweep
found are repaired here as well. Committed locally, not pushed. (h) and (i) remain queued.

## What the hosted lanes print besides the repaired compile and link warnings

Run 38640651640, 21 build jobs, after removing what the earlier DIAG.11b batches already repaired locally (the GCC
link-time diagnostics, D9025, the zstd legacy decoders' warnings):

| Lines | Jobs | Source | Repaired here |
| --- | --- | --- | --- |
| `Compatibility with CMake < 3.10 will be removed` at zstd's `build/cmake/CMakeLists.txt:10` | all 21 | zstd 1.5.5's `cmake_minimum_required(VERSION 2.8.12)` | on CMake 4 (every Windows job); see below for CMake 3 |
| `Policy CMP0194 is not set: MSVC is not an assembler for language ASM` | the 10 MSVC jobs | zstd's `project(... C ASM CXX)` | yes |
| `Manually-specified variables were not used by the project: _NOTE_*` | win-clang-cl-shipping, win-shipping-profile | two notes kept as preset cache variables | yes |
| `unused function 'HUF_isSorted'`, `'ZSTD_isAligned'` (`-Wunused-function`) | win-clang-cl-shipping | zstd's `MEM_STATIC` under clang-cl | yes |
| Node `DEP0040` (punycode) and `DEP0169` (`url.parse`) deprecations | every job | the pinned actions' own runtime (setup-python, cache, ninja, upload) | no: outside the repository's code; recorded on [REPO.3](../ROADMAP.md#slice-repo.3) |
| `QFont::setPixelSize: Pixel size <= 0` | win-debug-scalar | the LunarG Vulkan SDK installer on the cold lane | no: the same |

All four repository-side ones reproduce locally: fresh configures of win-debug, win-clang-cl-shipping and
win-shipping-profile into scratch trees (`CRD_INPUT_ARCHIVES` pointing at the archives the existing trees already
held, verified against their SHA-256) printed the deprecation on all three, CMP0194 on the two MSVC ones (CMake 4.3.2
spells it `CMP194`, below) and `_NOTE_lto_isolation` on clang-cl; building `libzstd_static` in the clang-cl tree printed
the two `-Wunused-function`.

## (e) The two clang-cl warnings

`HUF_isSorted` (`huf_compress.c`) and `ZSTD_isAligned` (`zstd_lazy.c`) are `MEM_STATIC` helpers used only inside
`assert()`, so a build without asserts never references them. zstd's `mem.h` gives `MEM_STATIC`
`__attribute__((unused))` only under `__GNUC__`, and clang-cl does not define `__GNUC__`: there `MEM_STATIC` is a plain
`static inline`, and zstd's own `/clang:-Wall` reports both. No zstd release fixes it: 1.5.5 (pinned), v1.5.7 and the
dev branch all gate the attribute on `__GNUC__` (`UNUSED_ATTR` likewise), and both helpers are unchanged there, so a
pin bump would not help.

The repair is two registered patches on build-owned copies (`cmake/patches/zstd-huf-is-sorted-unused.cmake`,
`zstd-lazy-is-aligned-unused.cmake`, listed under zstd's `patches` in `cmake/pins.json`). Each inserts, before its
helper's definition line (the anchor),

```c
#if defined(__clang__) && !defined(__GNUC__) /* crd-patch: clang-cl's MEM_STATIC lacks the unused attribute */
__attribute__((unused))
#endif
```

which is exactly the attribute upstream gives the same helper under GCC and Clang-on-Linux; MSVC and GCC preprocess
the copy to the upstream text. zstd's CMake builds `libzstd_static` from a glob, so the root `CMakeLists.txt` swaps
the two upstream paths in the target's `SOURCES` for the copies (a configure error unless exactly two are replaced)
and gives the copies zstd's `lib/compress` as an include directory, because their same-directory includes (`hist.h`,
`zstd_lazy.h`, `zstd_compress_internal.h`) no longer resolve beside them. An anchor that stops matching fails the
configure, as for the imgui patch.

The objects are the upstream objects. Original against patched copy, `llvm-objdump -d` (Windows) and `objdump -d`
(WSL):

| Lane | `huf_compress` | `zstd_lazy` |
| --- | --- | --- |
| win-clang-cl-shipping | disassembly and bytes identical (5,679 instructions) | identical (50,642) |
| win-shipping-profile (MSVC) | identical | identical |
| linux-gcc-shipping | identical | identical |
| win-debug, linux-gcc-debug, linux-clang-tsan (asserts compiled) | same length; every difference is an assert's line constant, +3 (the three inserted lines) | the same |

`libzstd_static` built with no warning line on win-debug, win-shipping, win-shipping-profile, win-release,
win-clang-cl-shipping, win-clang-cl, win-asan, a Visual Studio 18 scratch tree (`win-vs`; the project compiles the two
copies), linux-gcc-debug, -asan, -shipping, -release, -relwithdebinfo and linux-clang-tsan. Its consumers pass:
crd-resources-tests (131 cases) on win-debug, win-shipping, win-clang-cl-shipping, win-asan, linux-gcc-debug,
-asan and -shipping, and the `crd-fuzz-resources-reload-corpus` replay on win-debug, win-shipping,
win-clang-cl-shipping, linux-gcc-debug and -shipping.

## The configure warnings

- **zstd's minimum version.** `CMAKE_POLICY_VERSION_MINIMUM` goes from 3.5 to 3.10 around zstd's
  `crd_add_pinned_package`. zstd's next line sets `cmake_policy(VERSION 3.13.0)` itself, so no policy zstd runs under
  changes (v1.5.7 declares 3.10 itself). The variable exists from CMake 4.0. CMake 3.27 to 3.31 issue the same
  deprecation unconditionally (`cmPolicies::ApplyPolicyVersion` at v3.31.6 checks no variable; a scoped
  `CMAKE_WARN_DEPRECATED` does not reach it, probed on WSL's 3.28.3), so the hosted Linux lanes (the runner image's
  CMake 3.31.6) and WSL (3.28.3) still print it once per configure. The repair for those is the zstd 1.5.7 pin, which
  declares 3.10; fetching its archive is a download the user approves (see the follow-up).
- **CMP0194.** zstd enables ASM on every platform but assembles nothing under MSVC (`ZSTD_DISABLE_ASM`, no `.S`
  globbed there). `CMAKE_POLICY_DEFAULT_CMP0194` is `OLD` around zstd's add: the answer every configure before CMake
  4.1 gave (cl.exe stays the nominal, unused assembler; NEW would make `enable_language(ASM)` take whatever assembler
  PATH offers, or fail). zstd's dev branch enables ASM only where it assembles. CMake 4.1 to 4.3 register the policy
  as `CMP194` (`Source/cmPolicies.h` at v4.3.2) and read its default under that name; 4.4 corrected it to `CMP0194`.
  A probe project on this machine's 4.3.2 kept the warning with `CMAKE_POLICY_DEFAULT_CMP0194` and read `OLD` with
  `CMAKE_POLICY_DEFAULT_CMP194`, so both names are set. The assembler found is unchanged (cl.exe on MSVC, clang-cl on
  clang-cl).
- **The preset notes.** `_NOTE_lto_isolation` (win-clang-cl-shipping) and `_NOTE_crd_perf_under_ltcg`
  (win-shipping-profile) were cache variables no code reads; their text is now each preset's `description`.

After: the same three fresh configures print no CMake warning, and the clang-cl tree's `libzstd_static` none either.

## (j) Where a pause request lands

The compiled plan offers a safe point before every dispatched instr (`plan.cpp` `safe_point`; terminators and latches
are not instrs), and `Session::on_plan` decides there. A pause request therefore lands before whichever instr the
executing thread reaches next, in any frame: in the inspection program that is one of main's constants, its two
`addi`, the call, the helper's `muli` (depth 1), `core.for`, or the loop body's `addi`. `core.for` defines no result,
and `plan::read_value` answers `NoSuchValue` for a result index past the instr's results before it looks at the path
(the documented status, the same one the breakpoint case asserts for `addi`'s result 1). The case's oracle,
`NotYetComputed` for whatever op the pause held, was wrong for that one landing: on a slow executing thread (TSan) the
request can land before the loop is entered. A test-oracle defect, not an executor defect.

- The racing case now asserts the landing rule: the held op is the op of the instr at `stop.at`, and it reads
  `NotYetComputed` when that instr defines a result and `NoSuchValue` when it defines none.
- New case "diag 8b: a pause request lands at the next safe point whatever instr it holds" pins three landings with
  the session's safe-point observer (`Session::run(..., observer)`, called before the session decides): `LandingGate`
  holds the executing thread at its first arrival before a chosen instr until the controller has requested the pause,
  so the request lands exactly there. Before `core.for`: the stop holds `core.for`, its op reads `NoSuchValue`, the
  call before it is `Available` (36), the loop body is `OutOfScope`. Before the loop body's first instr: its op is
  `NotYetComputed`, the outer `%s` reads 6. Before the helper's `muli`: depth 1, its op `NotYetComputed`, the caller's
  `%s` `OutOfScope`. Each is cancelled at the pause and the run's fault names the held instr.
- Teeth, each on win-debug, restored byte for byte, rebuilt and rerun green: the old oracle in the `core.for` landing
  fails (T1); an executor that lets a pause request pass resultless instrs stops at the loop body instead and fails
  the landing's position checks (T2, `inspect.cpp`); a `read_value` that counts the held instr as run reads it
  `Available` and fails the two result-bearing landings (T3, `plan.cpp`).
- linux-clang-tsan: the seven DIAG.8b cases 30, 30 and 40 times (after each rebuild), no failure and no report.

### A link-time site the edit exposed

The edit shifted crd-ceir-tests' link-time partitions on linux-gcc-shipping, and the link failed with
`-Werror=null-dereference` at `ir.hpp:255` inlined from `test_hello.cpp:59`: `build_hello_hand` appended to
`func::func_body_block(fn)`, the lookup that may miss, without a check. A relink in one partition listed no other
site in the program. That site and its sibling in `build_hello_builder`, and the two in this case's own program, now
use `&func::func_body(*fn)`, the invariant accessor the previous batch added. The same unchecked pattern stands at
about 200 test sites in other executables, which link clean today only because GCC does not inline the null path
there: follow-up (k).

## Verification

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | `libzstd_static`, crd-resources-tests, crd-fuzz-resources-reload, crd-ceir-tests | crd-ceir-tests (578 cases, 18,374 assertions), crd-resources-tests (131), the corpus replay, the teeth; the `crd-no-*`/`crd-check*` guards |
| win-shipping | the same | the same suites and the replay |
| win-clang-cl-shipping | the same (every link on the first attempt) | the same suites and the replay |
| win-asan | `libzstd_static`, crd-resources-tests, crd-ceir-tests | both suites under vcvars |
| win-shipping-profile, win-release, win-clang-cl | `libzstd_static` | |
| win-vs (Visual Studio 18 scratch tree) | `libzstd_static` (Debug) | |
| linux-gcc-shipping, -release, -relwithdebinfo | `libzstd_static`, crd-ceir-tests (shipping also crd-resources-tests and the fuzz target), links under the full link-time warning set | crd-ceir-tests; shipping also crd-resources-tests and the replay |
| linux-gcc-debug | `libzstd_static`, crd-ceir-tests, crd-resources-tests, crd-fuzz-resources-reload | crd-ceir-tests, crd-resources-tests, the replay |
| linux-gcc-asan | `libzstd_static`, crd-ceir-tests, crd-resources-tests | crd-ceir-tests, crd-resources-tests |
| linux-clang-tsan (the lane's `TSAN_OPTIONS`) | `libzstd_static`, crd-ceir-tests | crd-ceir-tests and the DIAG.8b repeats, no report |

Also clean: `tidy-files.py` on both changed C++ files (LLVM 20.1.8), `clang-format --dry-run` on the added lines (each
file's finding count is unchanged; the two modified lines in `test_inspect.cpp`'s program builder keep their block's
hand alignment as before), `check-allman-braces.py`, `check-pins.py`, `test-repository-tools.py`,
`check-master-plan.py`, `check-repository.py`. No workflow changed. The root `CMakeLists.txt` and `CMakePresets.json`
edits resolve the eventual push to the complete tier.

## Hosted evidence this batch needs

win-clang-cl-shipping prints no zstd warning. No Windows job prints the zstd deprecation, CMP0194 or an unused
`_NOTE_*` variable; the Linux jobs (CMake 3.31.6) still print the one zstd deprecation until the 1.5.7 pin. Every lane
configures the two zstd patches ("Patched zstd/lib/compress/... (1 replacements)") and passes crd-resources-tests and
the resources corpus replay; windows-native builds the patched copies. crd-ceir-tests passes on every lane with the
new DIAG.8b case, and linux-gcc-shipping, -release and -relwithdebinfo link it under the link-time warning set.

## Follow-up for an extra batch

(h) and (i) as queued in the [previous note](2026-10-17-diag-11b-link-time-null-dereference.md#follow-up-for-an-extra-batch).

(k) Test code dereferences `func::func_body_block(...)` unchecked at about 200 sites outside crd-ceir-tests's two
repaired files (`test_plan.cpp` 42, `ceir-host/test_host_provider.cpp` 32, `test_exec.cpp` 18,
`ceir-gpu-dx12/test_ceir_pipeline_dx12.cpp` 18, `test_task.cpp` 7, `test_async.cpp` 6, audio, ceir-cook, ceir-gpu and
others, and 31 helpers that `return func::func_body_block(x)`). Each dereferencing site is a
link-time `-Wnull-dereference` waiting for an inlining change, as `test_hello.cpp` was here. Sites that dereference
should take `func::func_body` (sites that test a missing body keep the lookup), and every touched executable must link
under the link-time set on the three IPO trees.

(l) The zstd 1.5.7 pin, once its archive may be fetched: it declares `cmake_minimum_required(VERSION 3.10)`, which ends
the deprecation on CMake 3 hosts (hosted Linux). Re-anchor both patches (their definition lines are unchanged in
v1.5.7), keep the CMP0194 setting (1.5.7 still enables ASM unconditionally), and check cooked outputs whose bytes
depend on the compressor.
