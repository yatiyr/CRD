# DIAG.11b: the zstd 1.5.7 pin and clang-tidy 20 on the WSL host, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contracts:
> [pinned inputs](../design/pinned-inputs.md), [strict analysis](../design/developer-workflow.md#strict-analysis),
> [BUILDING](../BUILDING.md). Rules: [AGENTS](../../AGENTS.md).
> Previous zstd batch: [zstd's clang-cl and configure warnings](2026-10-17-diag-11b-zstd-configure-and-pause-landing.md).

DIAG.11b's operator items (l) and (o). Both are downloads the user approved in their own messages ("yes approve the
zstd download"; clang-tidy 20 for WSL, earlier). The operator fetched both files and checked their digests; this
batch used those files and fetched nothing else. Every configure used `-DCRD_INPUT_ARCHIVES` (the verified local
archive path of [pinned inputs](../design/pinned-inputs.md#acquisition)), so CMake downloaded nothing either.

## (l) zstd 1.5.7

The input: tag v1.5.7 (tag object `ac66b19e6bd6b83238bf008eecc1298105298532`) at commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`; the commit-addressed archive `zstd-f8745da6.tar.gz`, 2,453,329 bytes,
SHA-256 `4b0bd1f0cfb25e61b9103c35f27395530ff5b4c0d2513a00fd745849e85ea52c` (rechecked here); BSD-3-Clause OR
GPL-2.0-only. `cmake/pins.json` names it (version, ref, commit, url, file, sha256); the license manifest and its
provenance record were regenerated (`gen_license_manifest.py`, `check-generated.py --refresh`).

Each workaround, against 1.5.7's own source:

| Workaround | 1.5.7 source | Outcome |
|---|---|---|
| `CMAKE_POLICY_VERSION_MINIMUM` 3.10 | `build/cmake/CMakeLists.txt:10` declares `cmake_minimum_required(VERSION 3.10 FATAL_ERROR)`, then `cmake_policy(VERSION 3.13.0)` on CMake 3.13+ | removed |
| CMP0194 / `CMP194` `OLD` | still `project(zstd ... LANGUAGES C ASM CXX)` and `project(libzstd C ASM)` on every platform; under MSVC `lib/CMakeLists.txt` defines `ZSTD_DISABLE_ASM` and adds no `.S` (only other compilers get `huf_decompress_amd64.S`, and only with `ZSTD_HAS_NOEXECSTACK`) | kept, comment names 1.5.7 |
| `zstd-huf-is-sorted-unused` | `MEM_STATIC int HUF_isSorted(...)` at `huf_compress.c:544`, used only in `assert(HUF_isSorted(...))` at :664; anchor byte-identical | kept, "(1.5.7)" |
| `zstd-lazy-is-aligned-unused` | `MEM_STATIC int ZSTD_isAligned(...)` at `zstd_lazy.c:808`, used only in the two asserts of `ZSTD_row_prefetch` (:827-828); anchor byte-identical | kept, "(1.5.7)" |
| `ZSTD_LEGACY_SUPPORT OFF` | still an option defaulting ON (`ZSTD_LEGACY_LEVEL` 5) that adds `zstd_v01.c`..`zstd_v07.c` | kept |
| IPO off on `libzstd_static` | the target keeps that name; `AddZstdCompilationFlags.cmake` still adds `-Wa,--noexecstack` to the C flags | kept |

`MEM_STATIC` moved: 1.5.5 defined it in `lib/common/mem.h`, 1.5.7 in `lib/common/compiler.h` through `UNUSED_ATTR`,
still `__attribute__((unused))` only under `__GNUC__` (and IAR). The patch headers and the root comment say so now.
1.5.7 also dropped its directory-wide `include_directories(lib lib/common)`; the patched copies still compile with
`lib/compress` alone, their `../common/` includes resolving through it.

Teeth for the two patches, on win-clang-cl-shipping's own compile command for the copies (clang-cl 20.1.8,
`/clang:-Wall -Wextra ... /O2 -DNDEBUG`), objects into a scratch directory:

| File | Upstream 1.5.7 | Patched copy |
|---|---|---|
| `huf_compress.c` | `warning: unused function 'HUF_isSorted' [-Wunused-function]` (544,16) | no warning |
| `zstd_lazy.c` | `warning: unused function 'ZSTD_isAligned' [-Wunused-function]` (808,16) | no warning |

`llvm-objdump -d` of each pair is identical (5,694 and 50,910 lines): the copies change no code.

### Cooked data

The cooker writes zstd frames through `CrdrWriter::add_chunk_compressed` (level 3), so every committed file was
checked for a dependence on the compressor's output bytes:

- the three tracked containers, `assets/frame/sr_ts_{mtl,quad,tex}.crdr`, carry header flags 0 and no zstd frame
  magic: nothing compressed;
- the tests on compressed chunks (`test_cook.cpp`'s round trip, the scene pack-trust cases, the fuzz harness's
  `compressed_chunk` claims) compare decoded bytes and claimed sizes, never a compressed size or byte;
- no test, script or asset holds a hash of a cooked container (searched `tests/`, `assets/`, `tools/`, `scripts/`,
  `cmake/` for zstd, `crdr`, the frame magic and hash literals);
- the resources fuzz corpus does: five of its 34 seeds hold zstd frames (`pack-compressed-manifest`,
  `chunk-claim-past-bound`, `chunk-claims-wrap`, `chunk-claim-past-frame`, `chunk-claim-exact`), and the corpus is the
  replay's `--seed` output, named by content hash.

`crd-fuzz-resources-reload --seed <scratch>` with 1.5.5 (win-debug, before the pin) reproduced the committed corpus
byte for byte; with 1.5.7 it does too on win-debug, win-clang-cl-shipping and linux-gcc-shipping: the same 34 names,
every file identical. zstd 1.5.7 writes the same level-3 frames for these inputs, so nothing was regenerated and no
oracle changed.

### Verification

| Lane | Configure | Build (no warning) | Tests |
|---|---|---|---|
| win-debug (CMake 4.3.2) | no zstd deprecation, CMP0194/CMP194 or other warning | libzstd_static (the two patched objects), crd-resources, crd-resources-tests, crd-resources-fatal-specimen, crd-fuzz-resources-reload, crd-cooker-tests | 138 resources CTests (tests, 6 fatal modes, corpus replay), 30 cooker CTests |
| win-clang-cl-shipping (CMake 4.3.2) | same | the first five; no `-Wunused-function`, no lld-link failure | 138 resources CTests |
| linux-gcc-shipping (CMake 3.28.3, LTO) | same | all six, the `.S` assembled, no `warning:` line at compile or link | 138 resources CTests, 30 cooker CTests |
| linux-gcc-debug (CMake 3.28.3) | same | crd-resources-tests, crd-fuzz-resources-reload (in the dev check below) | 169 CTests of the dev check |

`check-pins.py` passes (9 packages, 15 tools), `gen_license_manifest.py --check` and `check-generated.py` pass, and
`test-repository-tools.py` (122 tests, 3 skipped) passes on Windows and, inside the dev check, on Linux.

## (o) clang-tidy 20 on the WSL host

The input: PyPI's clang-tidy 20.1.0, its only LLVM 20 wheel,
`clang_tidy-20.1.0-py2.py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl`, 38,429,166 bytes, SHA-256
`e205541e350044ffe0b0d1fcaf62b9953890f8ecba31458bbcdcfe73040e58b6` (rechecked in WSL). Its binary
`clang_tidy/data/bin/clang-tidy` has SHA-256 `8ad8c09fb4cf87b67a2dda1aed0036bb16a2daa78cd8eb930fcf2a90205b28e7` and
reports `LLVM version 20.1.0`; its builtin headers are 273 files under `clang_tidy/data/lib/clang/20/include/`
(282 entries with the nine directories). Licenses read from the wheel: LLVM's binary and headers under Apache-2.0 WITH
LLVM-exception, the wheel's packaging (ssciwr/clang-tidy-wheel) Apache-2.0 (`METADATA`, `LICENSE.md`, `COPYING.md`).

It is 20.1.0, not the 20.1.8 of Windows (`C:/LLVM-20.1.8`) and the hosted lanes. The gate
(`REQUIRED_TIDY_MAJOR = 20`) checks the major only, so it qualifies; the registry note records the difference.

- `cmake/pins.json` `tools/clang-tidy-linux`: url, file, sha256, `member`, `member_sha256`, `headers`,
  `header_count`, license and a note; `check-pins.py`'s helper list names the installer, so a literal digest there
  fails it.
- `scripts/install-clang-tidy.py`, in the style of `install-mold.py` and `install-clang-build-analyzer.py`: the wheel
  through `pins.fetch()` (default download directory `~/.cerid/downloads`, where the operator's copy was verified in
  place), the pinned binary member and its digest, every regular file under the pinned header directory (a symbolic
  link, an absolute path or a `..` is refused), the header count, the reported version, a stamp; the tree is assembled
  in `<destination>.partial` and renamed into place only when complete. The layout `bin/clang-tidy` plus
  `lib/clang/20/include` is the one clang finds its resource directory by. Linux only.
- `scripts/cerid_dev/environment.py`: `pinned_clang_tidy(environment, windows)` is the host's pinned install,
  `C:/LLVM-20.1.8/bin/clang-tidy.exe` on Windows and `$HOME/.cerid/tools/clang-tidy/bin/clang-tidy` elsewhere (from
  the passed environment's HOME); `resolve_clang_tidy` tries it before `clang-tidy-20` and `clang-tidy` on PATH, an
  explicit tool still the only candidate.
- Tests: `test_linux_hosts_find_the_pinned_install_under_home_without_path` (test-dev-workflow.py: the mapping on both
  branches, the installer's default path equal to the gate's, and on a Linux host a stub at the pinned path found
  with an empty PATH, ignored by the Windows branch, overridden by `CRD_CLANG_TIDY`);
  `test_clang_tidy_installer_keeps_the_resource_layout_and_checks_digests_count_and_version`
  (test-repository-tools.py: a fake wheel; refusals of a wrong binary digest, header count, wheel digest, a member
  outside `bin/`, a `..` member and a symbolic-link header, each leaving nothing installed; version 20.1.8 or none
  refused; a matching stamp reused without reading the wheel). The existing no-candidate case now also clears HOME:
  with the real install present it found it (the first Linux dev check below failed on exactly that).

The first real install refused the wheel: the pin said 282 headers, counting directories, and the wheel holds 273
files. The pin was corrected to 273 and the install then passed:
`Installed clang-tidy 20.1.0 and 273 builtin headers; wheel and binary SHA-256 verified.` A second run reuses it.

### Verification

- `wsl-lane.sh dev linux-gcc-debug --path tests/assets/resources/test_cook.cpp --path
  tests/assets/resources/fuzz/fuzz_resource_reload.cpp`: `doctor` resolves `/home/yatiyr/.cerid/tools/clang-tidy/bin/clang-tidy`
  (20.1.0, no rejection); the check builds crd-fuzz-resources-reload and crd-resources-tests, runs its 169 selected
  CTests, check-master-plan and check-repository, and tidies both files clean with 20.1.0: `Check: passed`. Before
  this batch the same route ended incomplete (tidy unavailable). Its first run here failed four CTests honestly: the
  license manifest and its provenance were stale after the tools entry was added, and the old no-candidate test found
  the new install; all three were fixed and the rerun passed.
- `tidy-files.py` on `engine/assets/resources/src/crdr.cpp`, `tests/assets/resources/test_cook.cpp` and
  `tests/assets/resources/fuzz/fuzz_resource_reload.cpp` (the translation units that include zstd's header or test
  its frames; this batch changes no C++): clean with 20.1.0 on linux-gcc-shipping (1,723 database entries) and with
  20.1.8 on win-debug. No finding differs between the two versions on these files.
- `test-dev-workflow.py` (60 tests) and `test-repository-tools.py` pass on Windows; both pass on Linux in the dev
  check.

## Close-out

`ctest -R "^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$"` on win-debug: 15 of 15 passed.
`check-master-plan.py` and `check-repository.py` pass.

Hosted once pushed: no Linux job prints zstd's configure deprecation (CMake 3.31.6 there); every lane builds zstd
1.5.7 with no warning, applies both patches, and passes crd-resources-tests and the resources corpus replay; the
repository job passes check-pins, the license manifest check and the new resolver and installer tests on Windows
and Linux. Left in DIAG.11b: the final complete-tier run and the user's elevated capture (8).
