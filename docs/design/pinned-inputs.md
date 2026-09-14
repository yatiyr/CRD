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
| `packages` | the ten CPM sources (Catch2, glfw, tomlplusplus, zstd, stb, cgltf, MikkTSpace, imgui, and the gated Eigen and OpenBLAS) | version, repository, ref, commit, commit-addressed archive `url`, local `file` name, `sha256`, license, optional `patches` |
| `tools` | CPM bootstrap, Vulkan SDK (Windows installer, Linux archive with the validation members), Vulkan-Headers archive, the SPIRV-Reflect and SPIRV-Headers files, WARP package and member, actionlint, sccache (Windows and Linux archives with the binary member), LLVM | version, `url`, `file`, `sha256` (per file where a tool is several files), license, provenance |
| `actions` | the seven workflow actions | tag `ref` and the 40-hex commit `sha` the workflow must use |
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
verifies, renames, and verifies an existing file before reuse; `--archive` (or `--archive-dir`) verifies a handed-in
copy instead. [install-vulkan-sdk.py](../../scripts/install-vulkan-sdk.py) runs the verified Windows installer and
checks the installed layout; [install-vulkan-headers.py](../../scripts/install-vulkan-headers.py) assembles the
Linux header tree from the Vulkan-Headers archive (only its `include/` subtree, every member path validated) and the
three SPIRV-Reflect files; [install-vulkan-validation.py](../../scripts/install-vulkan-validation.py) and
[install-warp.py](../../scripts/install-warp.py) read their pins from the registry. Every hosted lane installs
Python 3.12 before calling them.

## Patches on a build-owned copy

A patch is a versioned spec in `cmake/patches/<name>.cmake`, a list of `crd_patch_replace(<from> <to>)` steps.
`crd_patched_copy(<out> <package> <file> <name>)` reads the pristine file from the downloaded tree, applies the
steps, and writes the result under `${CMAKE_BINARY_DIR}/patched/<package>/` only when its content changes; the
target compiles that copy. An anchor that no longer matches upstream is a configure error naming the patch, where
the former in-place `file(WRITE)` into the source tree silently did nothing on drift. The imgui Vulkan backend
(`imgui-vulkan-whole-size`) is the one patch today.

The tooling suite proves the cache contract on a fixture: two consumers configured against one `CPM_SOURCE_CACHE`
from a local archive leave the cache tree's content hash unchanged, the cached file keeps the upstream text while
each build tree holds the patched copy, an unchanged patch is not rewritten, a wrong digest and a truncated archive
fail before anything is extracted, and a drifted anchor fails naming the patch.

## Workflow trust

`permissions` stays `contents: read`; every checkout passes `persist-credentials: false`, so no step and no build
product holds a token; every `uses:` names a commit with its tag in a comment; every job runs on an explicit image
label. Pull-request caches are isolated by GitHub (a pull request may read the default branch's caches, never the
reverse), the `restore-keys` prefixes only reach caches written on the same branch or the default branch, no job uses
`pull_request_target`, and no consumer of an artifact holds more than read access. There are no self-hosted runners;
the development workstation is never connected as a runner for public pull requests. Hardware workers, when they
come, need their own trust boundary and cleanup before they qualify anything.

## Declared gaps

The apt packages of the Linux image are not pinned; their installed versions are part of every Linux lane's
evidence. GitHub generates commit archives on demand; their digests have been stable since 2023, and a change would
fail the configure loudly rather than substitute bytes. LLVM 20.1.8 is acquired by a pinned action rather than by
digest.

## Not in scope

Compiler caching is measured and applied by [build performance](build-performance.md) (REPO.DEV.7); relocatable
package export is REPO.DEV.8.
