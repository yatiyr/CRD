# 2026-10-31 — OWN.1: FNV-1a moves onto crd-core

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the first audit move after the
[registry](2026-10-31-own-capability-registry.md). Built and tested on `cerid-track-b`.

## The owner

The registry first named crd-containers, where `hash.hpp` held `fnv1a_64`. One of the copies is in crd-memory's
diagnostic allocator, and crd-memory sits below crd-containers, so the owner is crd-core
([design decision 7](../design/capability-ownership.md#decisions)). `crd/core/fnv1a.hpp`:

- `fnv1a_64` and `fnv1a_32` over bytes (`const void*`) and over text (`const char*`, a constant expression: the
  `void*` form cannot be read in one, which is why `frame_asset.hpp` and CEIR's `id.hpp` had copied the loop);
- every form continues from a running state, so a field-by-field id needs no scratch buffer;
- `fnv1a_64_le` and `fnv1a_32_le`: an integer's bytes least significant first, by shifts (a persisted identity is the
  same on every host);
- `fnv1a_64_word`: one FNV-1a step over a whole word, for runtime keys only.

crd-containers re-exports `fnv1a_64` for `hash_string`. `test_fnv1a.cpp` (crd-core-tests, four cases): the published
FNV-1a-32 and -64 values for "", "a" and "foobar" (checked at compile time), bytes above 0x7F in the text form,
the running-state identity at every split, the folds against a byte loop at every width and for a negative value, and
the word step against its formula.

## The sites

35 files (33 engine, 2 asset cooker); the design's [FNV-1a move](../design/capability-ownership.md#the-fnv-1a-move)
lists them by kind. Bit-exact: every cooked or stable identity (lighting, vertex-layout, CKIR variant key and content,
LOD policy, sparse topology, tensor artifact, CEIR program and plan target, obek GUID, spec set, scene signature,
frame executor ids, CEIR compile-time ids, the cooker's content and path hashes). Two details kept bits that a naive
move would have changed: the local `hash_u64` helpers keep their `u64` parameter (a 32-bit field still folds eight
bytes), and vertex-cook's trailing prime multiply is the string's NUL, now folded as `size() + 1` bytes of `c_str()`.
The CKIR technique key starts from 0, not the offset basis; it keeps that seed.

Changed values, all runtime keys nothing persists or pins: nine sites started from 1469598103934665603 (the offset
basis with its last digit lost) and scene-render's cull signature multiplied before xoring; they use the true offset
and the word step. frame-cook's work extraction now keys resources by `pass_param_id`, the identity the frame runtime
uses for the same resources, instead of its own truncated hash. Its consumers resolve `source_param` by hashing the
resource name again: the Vulkan and D3D12 wavefront gates (`test_vulkan_rt.cpp`, `test_dx12_rt.cpp`) carried copies of
the truncated hash, and the Vulkan gate failed on the first run until both called `pass_param_id`, the documented
identity, instead of a copy.

Also found: the asset cooker and the sandbox are first-party product code, so the guard now scans `tools/` and
`sandbox/` beside `engine/` (tests and the `runtime/examples` measurement programs keep their reference copies as
oracles and peers). The sandbox's BVH-viewer fingerprint mixes the FNV constants into SplitMix64 and moves with the
seed generators. The golden-ratio `hash_combine` appears twice (LOD policy identity, archetype graph) and is now a
registered capability (`hash.combine`, crd-core, pending).

## Tooling defect found on the way

`ci/bench_vm.py sync` failed on every sync after a rebase: the machine fetched `refs/bench/sync:refs/bench/sync`,
which git refuses as a non-fast-forward when the branch was rebased since the last sync, and the error surfaced only
as plink's exit 1. Another track fixed it the same hour (7459c35b, a forced per-machine ref), so this batch's own fix
was dropped at the rebase. A failed sync leaves the machine's checkout at the previous commit, and a build started
after it builds the old tree: check the commit the sync prints first.

The strict tidy helper (`tidy-files.py`, LLVM 20.1.8, on the machine's tree) also reported findings in five
hesap-eigen headers and two hesap-resources headers this move touched: residual norms named `rn` beside `m`
(confusable), several declarations per statement, a nested conditional, a switch with two identical branches and the
FourCC constants outside the house `NOLINT` block. They are fixed (`res_norm`, one declaration per line, the
`NOLINTBEGIN` block crdr.hpp uses); hesap-eigen (44 cases), hesap-resources (10), hesap-tensor (53) and hesap-sparse
(114) pass, and every changed file is clean except two the Linux tree does not compile (kir-cuda's backend and the
D3D12 ray-tracing test), which the CUDA and Windows lanes cover.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- Full tree build, no warnings.
- Test executables, each from a scratch directory: crd-core (98 cases), crd-containers (128), crd-memory (184),
  crd-frame-cook (108), crd-light-cook (13), crd-vertex-cook (25), crd-ceir (580), crd-ceir-cook (115), crd-chir (30),
  crd-lod (18), crd-kir (314), crd-perf-gpu-bridge (14), crd-hesap-autodiff (131), crd-hesap-eigen (44),
  crd-hesap-resources (10), crd-hesap-sparse (114), crd-scene (300), crd-cooker (30), crd-kir-vulkan (31): all pass.
  The bit-exact sites are exact by construction: each folds the same bytes in the same order through a function
  `test_fnv1a.cpp` proves equal to the byte loop it replaced, and the cooked-identity suites (light, vertex, LOD, CKIR
  variants, CEIR binary and program hashes, obek) pass unchanged.
- crd-perf, crd-scene-render and crd-gpu-context-vulkan need the pinned Vulkan validation layer and `CRD_ASSETS_DIR`
  (CTest sets the latter): the first run had neither on the new machine and failed only on those checks; the rerun with
  `install-vulkan-validation.py`'s layer and the variable is recorded below.
- `check-capability-owners.py`: 63 capabilities, the FNV-1a pending list empty but the sandbox entry.
