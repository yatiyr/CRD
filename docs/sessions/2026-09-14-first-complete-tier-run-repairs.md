# The first complete-tier run: two repairs

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.DEV.5](../ROADMAP.md#slice-repo.dev.5) (the native preset),
> [REPO.DEV.6](../ROADMAP.md#slice-repo.dev.6) (the pinned packages), [REPO.3d](../ROADMAP.md#slice-repo.3d)
> (the published evidence). Rules: [AGENTS](../../AGENTS.md). Preceding batch:
> [infrastructure audit](2026-09-14-infrastructure-audit.md).

## User direction

The maintainer pushed `repository hardening v4` (run
[34821419392](https://github.com/yatiyr/CRD/actions/runs/34821419392)), the first hosted run of the tiers
workflow; it resolved to the complete tier. The direction for the red lanes: diagnose now, fix once, push the fixes
batched after the run has finished. A red lane preempts slice work.

## Run state at the time of writing

Preflight and both repository jobs green; `clang-cl (win-clang-cl)`, `windows-clang-cl-shipping` and
`windows-native (win-vs)` red within twelve minutes; eighteen lanes still running (every Windows and Linux build
lane, `win-asan`, `win-tidy`, the two public-check presets). The Linux verdict is appended below when it lands.

## Repair 1: third-party headers had stopped being system includes

**Symptom.** Both clang-cl lanes fail compiling `tests/execution/ceir-gpu/test_grad.cpp`: inside Catch2's
`catch_tostring.hpp`, `implicit conversion between pointer-to-function and pointer-to-object is a Microsoft
extension [-Werror,-Wmicrosoft-cast]`, raised while Catch2 stringifies a `VjpRuleFn` function pointer in a `CHECK`.
The previous green run compiled the same file with the same Clang 22.1.3.

**Cause.** CPM 0.40.2's single-argument shorthand (`CPMAddPackage("gh:catchorg/Catch2@3.7.1")`) appends
`EXCLUDE_FROM_ALL YES; SYSTEM YES` to its arguments (`CPM_0.40.2.cmake`, "The shorthand syntax implies
EXCLUDE_FROM_ALL and SYSTEM"). REPO.DEV.6's `crd_add_pinned_package()` calls the long form (`NAME`, `VERSION`,
`URL`, `URL_HASH`), which implies neither, so Catch2, glfw, tomlplusplus and imgui lost their system-include
status and every warning inside their headers began to count against the repository's `-Werror`. clang-cl's
`-Wmicrosoft-cast` fires in Catch2's stream-insertability probe for any function pointer under a `CHECK`. A local
keep-going build of the `win-clang-cl` preset found the full extent, which the lane could not show because Ninja
stops at the first failure: 19 more test files (log, six hesap CLI suites, gpu-context-vulkan, platform, resources,
scene, anim, cooker), 58 diagnostics, all this one class. The REPO.DEV.6 graph comparison did not detect the change
on MSVC either: `-external:I` and `-external:W0` had disappeared from 996 `win-debug` compile commands.

**Fix.** The four former-shorthand packages pass `SYSTEM YES EXCLUDE_FROM_ALL YES` explicitly in the root
`CMakeLists.txt`; the [pinned-inputs design](../design/pinned-inputs.md) states the rule: third-party headers stay
system includes and never fail the warning gate. The first attempt, double-parenthesising the six `test_grad.cpp`
comparisons, treated a symptom and was reverted. The pins fixture in `test-repository-tools.py` now matches the
glfw call by pattern rather than by its old literal text.

**Proof.** `win-clang-cl` reconfigured and rebuilt with `-k 0`: 0 failed translation units (19 before), Catch2 now
reaches clang-cl through `-imsvc`. `win-debug` reconfigured through the MSVC helper: 1,822 compile commands, 996
changed by exactly the restored `-external:I` pairs for Catch2 and glfw plus `-external:W0`, nothing added or
removed; tomlplusplus and imgui already carried that form through their interface targets. `check-pins.py` PASS
(a first comment wording tripped its shorthand scan, which is the guard working); `test-repository-tools.py` 51.

## Repair 2: the native preset never passed its platform

**Symptom.** `windows-native (win-vs)` fails at configure: `CRD_NATIVE_PROFILES requires a native MSVC x64 Visual
Studio generator` (`cmake/CrdBuildProfiles.cmake:8`) with generator `Visual Studio 18 2026`, MSVC 19.51 and CMake
4.4.3.

**Cause.** The `win-vs` preset gave `architecture` as the plain string `"x64"`. A string form inherits its parent's
strategy, and `win-base` declares `external` (so that Ninja presets take the architecture from the environment), so
no `-A x64` reached CMake and `CMAKE_GENERATOR_PLATFORM` was empty; the generator still built for x64
(`CMAKE_VS_PLATFORM_NAME` was `x64`). A probe project with the same inheritance shows the string form empty and the
object form `x64` on both CMake 4.3.2 and 4.4.3, so this was never version-dependent. REPO.DEV.5's local proof of
`win-vs` had run over `build/win-vs-debug`, whose cache already held `CMAKE_GENERATOR_PLATFORM:INTERNAL=x64` from an
earlier configure, which is why it passed: a proof of a preset needs a fresh directory.

**Fix.** The preset sets `architecture` and `toolset` as objects with `strategy: set`, and the guard tests the
effective `CMAKE_VS_PLATFORM_NAME` (naming generator and platform in its message) instead of what `-A` happened to
pass.

**Proof.** The runner's CMake 4.4.3 (the PyPI wheel) on a fresh directory: the configure fails before the fix with
the lane's exact message and after it configures (47.2 s) and generates (17.3 s) the full solution. A first retry
in the deleted probe directory hit the project-sync preconfigure's own conflict guard ("Permission denied" on its
`generation-ready.json`, a local artefact of deleting the tree under a running synchronizer), and a new directory
passed. `test-project-sync-native.py --generator "Visual Studio 18 2026"` PASS on the edited preset;
`test-native-build-profiles.py` OK; `check-ci-tiers.py` PASS; the presets file parses.

## Repair 3: the fuzz replay harness let an exception escape `main`

**Symptom.** `clang-tidy (win-tidy)` fails on `tests/support/fuzz/src/replay_main.cpp:94` in both CEIR replay
targets: `an exception may be thrown in function 'main' which should not throw exceptions
[bugprone-exception-escape]`; Ninja stopped there, so the two CKIR targets built from the same file would have
followed. REPO.DEV.9 had qualified the harness on MSVC, clang and GCC but never through the tidy preset.

**Fix.** The body became `run()` inside the file's anonymous namespace; `main` calls it under `try` and turns any
exception into exit code 2 with its reason on stderr, which is the harness's existing failure contract.

**Proof.** `win-tidy` (the pinned clang-tidy 20.1.8) built the four fuzz targets with `-k 0`: 106 translation
units including the four `replay_main.cpp` and four `fuzz_*.cpp` objects, zero diagnostics, zero failures. The
`win-debug` replay executables rebuilt with the new `main` pass the four corpus CTests (0.28 s).

## Repair 4: the fuzz harness's inner allocator tripped the no-malloc guard

**Symptom.** `linux-gcc (linux-gcc-debug)` passed 6,517 of 6,518 tests, `linux-gcc-debug-sse2` and
`linux-gcc-debug-scalar` 6,514 of 6,515, `linux-gcc-relwithdebinfo` 6,517 of 6,518, `linux-gcc-release` and
`linux-gcc-shipping` 6,430 of 6,431 and `linux-gcc-public-checks`
6,518 of 6,519 (its public-header shims compiled hosted for the first time); `windows (win-public-checks)` passed
6,799 of 6,800 with the 1,118 MSVC shims built hosted, `win-debug-sse2` and `win-debug-scalar` 6,795 of 6,796 and
`win-debug` 6,798 of 6,799 (its 37 jobs-suite tests all passed, the hosted evidence REPO.3c's end-hook repair was
waiting for), the PowerShell twin failing the same way; the one failure on each is the repository guard
`crd-no-malloc-allocator` (`scripts/check_no_malloc_allocator.sh`, with a PowerShell twin on the Windows lanes):
`tests/support/fuzz/include/crd/fuzz/harness.hpp:150: memory::MallocAllocator m_inner;` is a `MallocAllocator`
reference outside the allowed scopes. The guard is a whole-tree CTest that REPO.DEV.9's scoped local runs never
executed against the new file.

**Cause and decision.** The budget allocator wraps the system allocator on purpose: ASan and libFuzzer's malloc
limit then see every block one by one, which a TLSF arena would hide. The guard provides a justified-exception
marker for exactly this case, so the line carries `crd-lint-allow-malloc-allocator` with the reason; the rule
itself (no `MallocAllocator` as a working allocator) is unchanged.

**Proof.** Both guard twins PASS on the working tree.

## Repair 5: three UBSan findings surfaced by the full suite under non-recovering UBSan

**Symptom.** `linux-gcc-asan` (the one lane pairing ASan with UBSan) reported four failures; one is the no-malloc
guard above, the other three are UBSan runtime errors the whole suite never hit hosted before, because the old
workflow's asan lane ran a narrower set. The three files did not change between the last green revision and this one;
the tiers workflow simply runs the complete suite under `-fno-sanitize-recover=undefined` for the first time.

| Finding | Where | Cause | Disposition |
|---|---|---|---|
| signed integer overflow | `tests/foundation/time/test_stopwatch.cpp:51` | a burn loop accumulates `0..99999` into a `volatile int` sink, overflowing `int` | sink is now `volatile unsigned` (defined wraparound); the value is never read |
| null passed to `memcpy` | `engine/numerics/hesap-tensor/include/crd/hesap/tensor/sparse.hpp:206` | an empty sparse tensor passes null perm arrays and `memcpy(null, null, 0)` is undefined | guarded with `n != 0U` |
| shift exponent 32 | `mikktspace.c:1667` (vendored tangent oracle) | the edge-sort pivot PRNG computes `uSeed >> (32 - (uSeed & 31))`, undefined when the mask is 0; the stack is entirely MikkTSpace, no engine frame | third-party register entry [TP-5](../third-party-defects.md#tp-5) on `linux-gcc-asan`; the oracle stays the unmodified upstream file |

**Proof.** On the `linux-gcc-asan` preset (WSL2, GCC 13.3, ASan+UBSan no recover): `Stopwatch: stop() freezes
elapsed` passes, `sparse: boundary adversaries - empty single-nnz duplicates and rejections` passes, and the
mikktspace oracle still aborts at `:1667` with no engine frame. The registered-failure gate returns PASS for
`linux-gcc-asan` with exactly the one registered failure and FAIL when any other test fails, so the lane goes green
with the sanitizer unsuppressed. `win-asan` is ASan-only and never hit the three UBSan findings.

## Verification

- `check-master-plan.py`, `check-repository.py`, `check-ci-tiers.py`, `check-pins.py` PASS; `test-repository-tools.py`
  51 cases; `git diff --check` clean.
- Local builds and configures named above; the scratch probe trees, the CMake wheel and the keep-going logs stay
  in the session scratchpad, not in the repository.

## Handoff

One batched diff: `CMakeLists.txt` (four package calls), `CMakePresets.json` (`win-vs`), `cmake/CrdBuildProfiles.cmake`
(the guard), `tests/support/fuzz/src/replay_main.cpp` (the catching `main`), `tests/support/fuzz/include/crd/fuzz/harness.hpp`
(the guard marker), `tests/foundation/time/test_stopwatch.cpp` and
`engine/numerics/hesap-tensor/include/crd/hesap/tensor/sparse.hpp` (the two UBSan fixes),
`docs/third-party-defects.md` (TP-5), `docs/design/pinned-inputs.md`, `scripts/test-repository-tools.py`, this record
and the ROADMAP links. Every failure of run 34821419392 is now resolved locally: four build/config causes and three
UBSan findings, twelve build-and-test lanes converging on the single no-malloc guard that the marker clears. A
separate, unrelated runtime-diagnostics slice (`DIAG.0`, ADR-0133) is untracked in the tree and not part of this
batch; it references a ROADMAP row that does not yet exist and must not be committed with these repairs.
