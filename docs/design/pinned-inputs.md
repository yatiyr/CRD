# Pinned inputs and cache integrity

<!-- doc-role: reference -->
> Contract for REPO.DEV.6. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.6); the accepted research is
> [pinned inputs and cache integrity](../research/2026-09-12-large-cpp-development-and-ci.md#pinned-inputs-and-cache-integrity);
> the registry is [cmake/pins.json](../../cmake/pins.json) and the guard [check-pins.py](../../scripts/check-pins.py).

Purpose: every external input of the build and the hosted workflow is named once with its version, source, SHA-256
and license; every acquisition verifies the bytes before anything is extracted, installed or compiled; a patch never
touches a shared downloaded tree; workflow actions and runner images are immutable references; tokens stay
read-only. A mismatch anywhere is a failure before compilation, never a silent substitution.

## Registry

[cmake/pins.json](../../cmake/pins.json) (schema `cerid-pins/1`) has four pinned sections and one declared gap:

| Section | Entries | Fields |
|---|---|---|
| `packages` | the thirteen CPM sources (Catch2, glfw, zstd, stb, cgltf, MikkTSpace, imgui, the gated Eigen and OpenBLAS, the gated Boost release, the gated HPTT, the gated Highway release and the gated Tracy client) | version, repository, ref, commit, commit-addressed archive `url`, local `file` name, `sha256`, license, optional `patches` |
| `tools` | CPM bootstrap, Vulkan SDK (Windows installer, Linux archive with the validation members), Vulkan-Headers archive, the SPIRV-Reflect and SPIRV-Headers files, WARP package and member, actionlint, sccache (Windows and Linux archives with the binary member), mold (the Linux archive with the binary member), the ClangBuildAnalyzer source archive, the clang-tidy 20.1.0 wheel (the Linux developer hosts' strict-analysis tool: binary member and builtin-header directory), LLVM 20.1.8 (the Windows release archive: the hosted clang-tidy lane's `clang-tidy.exe` member and builtin-header directory), Ninja 1.11.1 (Windows and Linux archives with the binary member), the GenMC source archive (commit-addressed, built against the pinned LLVM major), the Tracy 0.14.1 Windows release archive with its `tracy-capture` and `tracy-csvexport` members (the peer board's capture client and trace reader), Intel SDE 10.13.1 (the Windows and Linux kits, extracted whole: the avx512 arms' emulator), the own-CI image bake's installers (`bake-*`: Python 3.12.10, Git for Windows, CMake 4.4.5 for Windows and both Linux architectures, the Visual Studio 2026 Build Tools bootstrapper and Google's Windows GPU driver installer, each checked by SHA-256 inside the bake machine, [gce_vm.py](../../ci/executors/gce_vm.py)) | version, `url`, `file`, `sha256` (per file where a tool is several files), license, provenance |
| `actions` | the five workflow actions, every one on Node 24 (2026-10-26) | tag `ref` and the 40-hex commit `sha` the workflow must use |
| `runners` | the hosted images | explicit labels (`windows-2025`, `ubuntu-24.04`, `windows-2025-vs2026`), never `-latest` |
| `unpinned` | the apt packages of the Linux image | recorded by `dpkg-query` into every Linux lane's census; the MSVC toolset of the Windows image is recorded by the evidence bundle |

The human-readable view of the registry, with every license, is the generated
[dependency and license manifest](../generated/dependency-licenses.md) (`scripts/gen_license_manifest.py`;
`--check` is a CTest and a lane step, REPO.DEV.10).

Refreshing an input is one edit here: resolve the tag to its commit (`gh api repos/<owner>/<repo>/git/ref/tags/<tag>`),
take the archive at that commit, compute its SHA-256, and update the entry; the guard and the tests then require
the workflow, the build and the helpers to follow. Action commits cannot be re-verified offline, so refreshing them
is a reviewed manual step, not an automatic one.

[check-pins.py](../../scripts/check-pins.py) is the `crd-pins` CTest, a preflight step, a repository step and a
tooling test. It fails when an entry lacks a field or a well-formed digest, when a GitHub package is not addressed by
its commit, when a declared patch spec is missing, when the workflow references an action by anything but its
pinned commit (with the `# <ref>` comment), runs on a label outside the registry, checks out with persisted
credentials, names a tool version the registry does not, or fetches from an SDK or raw-file host directly, when the
root build file acquires a package outside `crd_add_pinned_package()` or omits a registered one, and when a helper
or the workflow carries a literal digest.

## Acquisition

[cmake/CrdPins.cmake](../../cmake/CrdPins.cmake) reads the registry with `string(JSON)`. The CPM bootstrap
([cmake/CPM.cmake](../../cmake/CPM.cmake)) downloads to a `.partial` file with `EXPECTED_HASH`, renames only after
the digest matched, and verifies an already present copy on every configure. `crd_add_pinned_package(<name> ...)`
calls `CPMAddPackage` with the commit-addressed archive and `URL_HASH`, so CMake verifies the archive before
extraction and a moved tag or altered archive fails the configure; the CPM source cache keys the extracted tree by
URL and hash, so no two pins share a directory. CPM's single-argument shorthand implied `SYSTEM YES` and
`EXCLUDE_FROM_ALL YES`; the long form implies neither, so the packages that used the shorthand (Catch2, glfw,
tomlplusplus, imgui) pass both explicitly: third-party headers stay system includes and never fail the warning
gate (the first complete-tier run, 34821419392, failed clang-cl inside a Catch2 header when they did not). `CRD_INPUT_ARCHIVES=<dir>` substitutes a local file named like the
entry's `file` for its URL (the same hash applies), which is the offline setup path.

The Python helpers share [scripts/pins.py](../../scripts/pins.py): `fetch()` downloads through a `.partial` file,
verifies, renames, and verifies an existing file before reuse; a transfer that fails in the network (a stalled
read, a reset, a truncated body, a 5xx, 408 or 429 status) is tried again after 5, 15 and 45 s, never a 4xx or a
checksum mismatch (hosted run 40649898318: a stalled read of the validation layer archive); `--archive` (or `--archive-dir`) verifies a handed-in
copy instead. [install-vulkan-sdk.py](../../scripts/install-vulkan-sdk.py) runs the verified Windows installer and
checks the installed layout; [install-vulkan-headers.py](../../scripts/install-vulkan-headers.py) assembles the
Linux header tree from the Vulkan-Headers archive (only its `include/` subtree, every member path validated) and the
three SPIRV-Reflect files; [install-vulkan-validation.py](../../scripts/install-vulkan-validation.py) and
[install-warp.py](../../scripts/install-warp.py) read their pins from the registry;
[install-ninja.py](../../scripts/install-ninja.py) puts the pinned Ninja on every lane's `PATH` (its binary member
verified and its `--version` checked), and [install-clang-tidy.py](../../scripts/install-clang-tidy.py) installs the
clang-tidy lane's LLVM 20.1.8 `clang-tidy.exe` and builtin headers from LLVM's release archive in one pass over the
tar (the binary's digest, the header count and the reported version checked).
[install-sde.py](../../scripts/install-sde.py) extracts Intel SDE 10.13.1's whole kit below the pin's root through
tarfile's `data` filter (the Linux kit's inner links kept, anything escaping refused), checks `-version` and
exports `CRD_SDE` for the emulated avx512 arms of win-release and linux-gcc-release. Every hosted lane installs
Python 3.12 before calling them.

### Cold acquisition on every complete run

The archive caches hide acquisition. Every lane restores a CPM cache through the restore-key fallback, so before
2026-10-06 no hosted run had downloaded the packages cold ([audit](../sessions/2026-10-06-needs-ci-audit.md)). Two
complete-tier lanes therefore never restore an archive cache: `win-debug-scalar` and `linux-gcc-debug-scalar`, named
in the workflow's `CRD_COLD_PRESETS`.

On those lanes:
- [check-cold-acquisition.py](../../scripts/check-cold-acquisition.py) `--before` fails unless the CPM source cache
  is empty;
- the configure downloads every package and verifies each `URL_HASH`;
- `--after` reads the configure's own `CPM_PACKAGES` and fails unless every one is pinned and now in the cache. It
  names the pins the configuration did not select (the opt-in `eigen` and `OpenBLAS` bench, the opt-in `tracy` peer). With
  `--fetch-unselected DIR` (both cold lanes pass it) it then downloads each of those archives through `pins.download`
  and fails on a SHA-256 mismatch or a download error. No hosted preset builds them, but their pinned bytes are proven
  acquirable on every complete run.

The Windows cold lane also installs the Vulkan SDK and the pinned WARP through their verifying helpers. The Linux lanes
download the Vulkan headers and the validation layer fresh on every run. A real cold configure on the WSL reference
host acquired all seven selected packages and passed both checks.

## The Tracy peer (opt-in)

`tracy` (Tracy Profiler 0.14.1, tag commit `30997d5c…`, BSD-3-Clause read from the archive's LICENSE; its client
bundles rpmalloc, moodycamel's concurrentqueue, SPSCQueue, LZ4 and libbacktrace under their own permissive terms, named
in the entry's note) is gated by `CRD_DIAG_PEER_TRACY`, off in every preset and every hosted lane. When it is on, the
root build file adds the archive `DOWNLOAD_ONLY` and builds the client's one unit (`public/TracyClient.cpp`) as
`tracy-client` (and `tracy-client-osclock`, the same client timed by the operating system's clock), without
link-time optimization and outside the engine's warning set; only `crd-diag-overhead-tracy` and
`crd-diag-overhead-tracy-osclock` link it (`tests/support/diag-overhead`), never an engine module. The client's
definitions are the comparison's: on demand, manual lifetime (with `TRACY_DELAYED_INIT`, which 0.14.1 still reads;
without it the constructor and `StartupProfiler` would both start the worker threads), localhost and IPv4 only, no
broadcast, sampling, system tracing, call stacks, frame images, code transfer or crash handler. The client compiles
from a build-owned copy of the archive's `public/` tree (its unity unit includes its siblings by relative path) with
two registered patches, `tracy-profiler-checked-calls` (a checked `pipe()` and bounded host-description writes in
`TracyProfiler.cpp`) and `tracy-syspower-checked-scan` (a checked `fscanf` in `TracySysPower.cpp`): the three GCC 13
warnings the pristine unit drew on linux-gcc-shipping, each a defect 0.14.1 still has. The Windows tools are
two `tools` entries over the one release archive (`tracy-capture-windows`, `tracy-csvexport-windows`), each with its
member digest: `scripts/diag-overhead.py --tracy-tools <archive or directory>` extracts or checks them through
`pins.install_member`/`pins.verify` and refuses any other bytes. Offline, `CRD_INPUT_ARCHIVES` names the directory
holding `tracy-30997d5c.tar.gz` (the operator's download of 2026-10-25 under `build/pin-tracy`), as for every pin
([bench](../bench/2026-10-26-diag-11b-tracy-peer.md)).

## Patches on a build-owned copy

A patch is a versioned spec in `cmake/patches/<name>.cmake`, a list of `crd_patch_replace(<from> <to>)` steps.
`crd_patched_copy(<out> <package> <file> <name>)` reads the pristine file from the downloaded tree, applies the
steps, and writes the result under `${CMAKE_BINARY_DIR}/patched/<package>/` only when its content changes; the
target compiles that copy. An anchor that no longer matches upstream is a configure error naming the patch, where
the former in-place `file(WRITE)` into the source tree silently did nothing on drift. Each patch is listed under its
package's `patches` in the registry (check-pins fails a listed patch without its spec). Eight exist (the two of the opt-in Tracy peer are described above). Two are Catch2
headers that every assertion reaches (2026-10-18): in C++20 `<chrono>` was the build's most expensive header, almost
all of it through them ([board](../bench/2026-10-18-build-speed-compile-time.md)). `catch2-chrono-stringmaker-opt-in`
puts `catch_tostring.hpp`'s `std::chrono` printers and their `<chrono>` behind `CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER`
again, as Catch2 v2 had them: assertions are unchanged, and only a failure message of a duration differs (C++20's
`operator<<`, or `{?}`). `catch2-clock-chrono-subset` gives `benchmark/catch_clock.hpp`, which the assertion path
reaches through `IResultCapture`'s benchmark types, the standard library's own duration-and-clock header
(`<__msvc_chrono.hpp>`, libstdc++'s `<bits/chrono.h>`; `<chrono>` on any other library) in place of the whole of
`<chrono>`: the same declarations. A header has no target source to swap, so the copies' directory comes first on
Catch2's include path, as a system directory like Catch2's own, and `crd-check-include-budget` fails when the
assertion path reaches `<chrono>` or `<format>` again. CMake writes the copies with CRLF on Windows (the zstd copies
too); compilers accept it, and an unchanged patch is not rewritten by a reconfigure (checked). The third Catch2 patch,
`catch2-posix-signal-refault` (2026-10-26), is a source: Catch2's POSIX fatal-condition handler re-raised every signal,
so a sanitizer reported a fault inside a test case at the sender's `{pid, uid}`. A fault the kernel raised now
re-executes under the restored handler, and the library compiles the copy in place of the upstream source (exactly
one, or the configure fails); ThreadSanitizer trees set `CATCH_CONFIG_NO_POSIX_SIGNALS`
([session](../sessions/2026-10-26-catch2-fault-address.md)). The others are the imgui Vulkan
backend (`imgui-vulkan-whole-size`), and zstd's `huf_compress.c` and `zstd_lazy.c` (`zstd-huf-is-sorted-unused`,
`zstd-lazy-is-aligned-unused`), which give an assert-only `MEM_STATIC` helper the unused attribute upstream gives it
under GCC but not under clang-cl, whose `-Wunused-function` was the one third-party warning win-clang-cl-shipping
printed. zstd's CMake builds its library from a glob, so the root `CMakeLists.txt` swaps the two upstream paths in
`libzstd_static`'s sources for the copies (exactly two, or the configure fails) and gives the copies zstd's
`lib/compress` for their same-directory includes; the objects are the upstream objects (identical code; with asserts
compiled, only the asserts' line numbers move) ([session](../sessions/2026-10-17-diag-11b-zstd-configure-and-pause-landing.md)).

zstd's own CMake drew configure warnings on every lane. 1.5.5 declared `cmake_minimum_required(VERSION 2.8.12)`,
raised to 3.10 through `CMAKE_POLICY_VERSION_MINIMUM` on CMake 4, still deprecated before 4.0 (the hosted Linux image's
3.31.6); the 1.5.7 pin (2026-10-25) declares 3.10 itself, so that setting is gone and no CMake deprecates zstd's
configure
([session](../sessions/2026-10-25-diag-11b-zstd-and-wsl-tidy.md)). Its unconditional `project(... ASM)`, still in
1.5.7, meets CMP0194 under MSVC, answered `OLD` for zstd's directory only (the
behaviour of every earlier configure; zstd assembles nothing under MSVC), under both the `CMP0194` name and the
`CMP194` spelling CMake 4.1 to 4.3 read the default under.

The tooling suite proves the cache contract on a fixture: two consumers configured against one `CPM_SOURCE_CACHE`
from a local archive leave the cache tree's content hash unchanged, the cached file keeps the upstream text while
each build tree holds the patched copy, an unchanged patch is not rewritten, a wrong digest and a truncated archive
fail before anything is extracted, and a drifted anchor fails naming the patch.

## Workflow trust

`permissions` stays `contents: read`; every checkout passes `persist-credentials: false`, so no step and no build
product holds a token; every `uses:` names a commit with its tag in a comment; every job runs on an explicit image
label. Only GitHub's own actions remain (checkout v7.0.1, setup-python v7.1.0, cache v6.1.0, upload-artifact v7.0.2,
github-script v8), all on Node 24; the three third-party actions that had no Node 24 release were replaced on
2026-10-26 by the repository's own steps: Ninja and clang-tidy by the pinned helpers above, and the Visual Studio
developer environment by [msvc-dev-env.py](../../scripts/msvc-dev-env.py) (vswhere, `vcvars64.bat`, every changed
variable to `$GITHUB_ENV`) ([session](../sessions/2026-10-26-actions-node24.md)). Pull-request caches are isolated by GitHub (a pull request may read the default branch's caches, never the
reverse), the `restore-keys` prefixes only reach caches written on the same branch or the default branch, no job uses
`pull_request_target`, and no consumer of an artifact holds more than read access. There are no self-hosted runners;
the development workstation is never connected as a runner for public pull requests. Hardware workers, when they
come, need their own trust boundary and cleanup before they qualify anything.

## Declared gaps

The apt packages of the Linux image are not pinned; their installed versions are part of every Linux lane's
evidence. GitHub generates commit archives on demand; their digests have been stable since 2023, and a change would
fail the configure loudly rather than substitute bytes. The Ninja 1.11.1 release assets predate GitHub's recorded
asset digests, so their SHA-256 values were computed from the downloaded files (LLVM's matches GitHub's record).

## Not in scope

Compiler caching is measured and applied by [build performance](build-performance.md) (REPO.DEV.7); relocatable
package export is REPO.DEV.8.
