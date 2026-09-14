# Pinned inputs: one registry, verified acquisition and build-owned patches

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.6](../ROADMAP.md#slice-repo.dev.6); contract:
> [pinned inputs](../design/pinned-inputs.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [CI tiers](2026-09-14-ci-tiers.md).

## User direction

The standing direction is unchanged: carry the REPO.DEV slices on "one by one until AUD-2". REPO.DEV.6 followed
REPO.DEV.5 in the same session with the advisor at the design gate; the increment ran directly. Hosted run
34780682504 (`18651d5`) still had four lanes in progress and nothing red throughout.

## What the repository acquired before

The Windows Vulkan SDK installer (307 MB) was downloaded and executed on every cache miss without a checksum; the
CPM bootstrap was trusted whenever the file was non-empty; Catch2, glfw, tomlplusplus, zstd, cgltf, imgui, Eigen and
OpenBLAS followed tags; three Khronos files were fetched by tag from raw.githubusercontent.com and Vulkan-Headers
was cloned by tag; every action was a floating major tag; checkouts persisted their token; the imgui Vulkan patch
was written into the downloaded tree, which CI shares across presets through the cache, and it silently did nothing
when its anchor stopped matching. The WARP package, the Linux validation layer and actionlint were already pinned by
SHA-256 in three separate places.

## What changed

- [cmake/pins.json](../../cmake/pins.json): the registry (schema `cerid-pins/1`): ten packages (commit, commit-addressed
  archive, SHA-256, license), nine tools (CPM bootstrap, both SDK files with LunarG's `files.json` as provenance,
  Vulkan-Headers archive, three SPIRV files, WARP, actionlint, LLVM), seven actions (tag and commit), three runner
  labels, and the declared apt gap. Every digest was computed from a fresh download; the CPM digest matched the local
  copy and the Linux SDK digest matched the validation installer's existing pin.
- [cmake/CrdPins.cmake](../../cmake/CrdPins.cmake): `crd_pin()`, `crd_pinned_source()` (`CRD_INPUT_ARCHIVES`),
  `crd_add_pinned_package()` (CPM with `URL` and `URL_HASH`), `crd_patched_copy()` and `crd_patch_replace()`.
  [cmake/CPM.cmake](../../cmake/CPM.cmake) verifies the bootstrap by `EXPECTED_HASH` through a `.partial` file and
  re-verifies an existing copy. The root build file adds the ten packages through the registry; the in-place patch
  block is gone; [cmake/patches/imgui-vulkan-whole-size.cmake](../../cmake/patches/imgui-vulkan-whole-size.cmake)
  holds the two replacements and `engine/ui/imgui` compiles the build-owned copy.
- [scripts/pins.py](../../scripts/pins.py) (loader, `fetch`, `show`),
  [install-vulkan-sdk.py](../../scripts/install-vulkan-sdk.py) (Windows installer: verify, run, check layout),
  [install-vulkan-headers.py](../../scripts/install-vulkan-headers.py) (Linux: archive `include/` subtree and the
  three files by digest); [install-vulkan-validation.py](../../scripts/install-vulkan-validation.py) and
  [install-warp.py](../../scripts/install-warp.py) read their pins from the registry.
- [scripts/check-pins.py](../../scripts/check-pins.py): registry, workflow, build-file and helper agreement; the
  `crd-pins` CTest, a preflight step, a repository step and a tooling test.
- [ci.yml](../../.github/workflows/ci.yml): every `uses:` at its commit with the tag comment, `persist-credentials:
  false` on all eleven checkouts, `windows-2025`/`ubuntu-24.04` instead of `-latest`, Python 3.12 on every lane, the
  SDK helper in the six Windows lanes, the headers helper in the two Linux lanes, actionlint fetched through
  `pins.py`, a `dpkg-query` census of the apt packages recorded into the Linux evidence bundle (`--packages`).
  [ci-tiers.json](../../.github/ci-tiers.json) runners follow.
- Tests: six `PinnedInputs` cases in `test-repository-tools.py` (registry and workflow agreement, guard failure modes,
  `fetch` semantics, SDK installer, header extraction, and the cache fixture: two consumers on one seeded
  `CPM_SOURCE_CACHE` with an identical tree hash, pristine cached file, patched build copies, no rewrite of an
  unchanged patch, wrong digest and truncated archive failing before extraction, drifted anchor failing by name).
  The two existing installer cases follow the shared `fetch` (timeout 120 s, no partial file left behind).
- Docs: [design/pinned-inputs.md](../design/pinned-inputs.md); BUILDING (6,998 bytes) names `CRD_INPUT_ARCHIVES`;
  repository layout and scripts README name the registry; two memory records; ROADMAP row 052; the pointer moves to
  REPO.DEV.7.

## Verification

- **Guards.** `check-pins.py` PASS (10 packages, 9 tools, 7 actions, 3 runners); `check-ci-tiers.py` PASS;
  actionlint 1.7.12 (WSL, pinned archive) clean on the rewritten workflow; `test-repository-tools.py` 36/36;
  `check-repository.py` PASS; `check-master-plan.py` PASS; `git diff --check` clean.
- **Default configuration unchanged.** `build/win-debug` reconfigured through the helper: every archive downloaded
  and verified, configure 30.0 s; the File API graph has the same 292 targets with identical types, directories and
  dependencies; the compile database (1,813 entries) and the Ninja build statements (5,786) differ only in the one
  imgui Vulkan backend path, now `build/win-debug/patched/imgui/backends/imgui_impl_vulkan.cpp`. The cached
  `_deps/imgui-src` copy carries no patch marker; the build copy carries both; `crd-imgui-vendor` built (9 objects,
  zero diagnostics).
- **Linux.** WSL `linux-gcc-debug` reconfigured over the 9p mount with the pinned archives (configure 201.8 s,
  generate 26.2 s); cached backend pristine, build copy patched.
- **Shared cache.** `CRD_MODULES=imgui` configured into four scratch directories (Debug, Release, Debug,
  RelWithDebInfo) against one `CPM_SOURCE_CACHE`; the cache tree (1,191 files) hashed `264cfc21…217b6d` after the
  third consumer and identically after the fourth; the cached backend carries no patch marker and every build
  tree's copy carries both replacements. The gated Eigen and OpenBLAS pins: `-DCRD_BUILD_HESAP_VS_REFERENCE=ON`
  with `CRD_MODULES=hesap` fetched and verified the Eigen 3.4.0 and OpenBLAS 0.3.27 archives into the same cache
  and configured in 29.7 s; the OpenBLAS build itself was not run.
- **Fixture.** The tooling fixture ran the cache contract end to end with a local archive (no network) in 5.5 s.

| Proof | Result |
|---|---|
| Windows `win-debug` full | 292 targets identical; 1,813 compile commands and 5,786 Ninja statements identical except the patched imgui path |
| Windows `crd-imgui-vendor` | 9 objects built from the patched copy, zero diagnostics |
| Linux `linux-gcc-debug` | reconfigured with verified archives; patch in the build tree only |
| Shared `CPM_SOURCE_CACHE` | four consumers of `CRD_MODULES=imgui` (Debug, Release, Debug, RelWithDebInfo); 1,191 cached files, tree hash `264cfc21…` unchanged across consumers; gated Eigen and OpenBLAS verified into the same cache, configure only |
| Registry guard | PASS on the working tree; every mutation case in the tooling tests fails as intended |

## State

REPO.DEV.6 is Needs CI on this evidence: the next push (workflow, scripts, `cmake/`, register) resolves to the
complete tier, where every lane acquires its inputs through the registry for the first time. Local caches and the
first hosted run re-download the archives once because the cache key includes `CMakeLists.txt`. The apt packages
stay OS-provided and are now part of the Linux evidence. Action commits are refreshed by hand with `gh api`.
