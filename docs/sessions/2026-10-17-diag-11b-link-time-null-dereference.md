# DIAG.11b: link-time null dereferences, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Rules: [AGENTS](../../AGENTS.md) ("keep
> warnings zero"). Previous batch: [the warning set at the link](2026-10-17-diag-11b-link-time-warning-set.md), which
> queued follow-up (g) of this note.

## Goal

The previous batch put the compile set's warnings that GCC's link-time compiler (lto1) reads on the GCC link line,
except `-Wnull-dereference`: at the link it reported 70 sites and 1,974 lines on linux-gcc-shipping. Follow-up (g):
add it to `_crd_lto1_warnings` (root `CMakeLists.txt`) and bring linux-gcc-shipping, linux-gcc-release and
linux-gcc-relwithdebinfo back to zero, nothing suppressed, hot paths unslowed. Committed locally, not pushed.

## Collecting the sites

`cmake --build -- -k 0` on `~/cerid-build/linux-gcc-shipping` (GCC 13.3.0, the hosted compiler), round after round.
`lto-wrapper` stops a link at its first failing partition, so a round shows only part of a program's sites. For the
links that kept failing, the failed commands were rerun with `-flto-partition=one` added (diagnostic only, nothing
kept; `relink_one.py` in the session scratch), which reports every link-time diagnostic of the program at once.

| Collection | Reported lines | Distinct reported locations | Failed links |
| --- | --- | --- | --- |
| HEAD's tree (the previous batch's collection) | 1,974 | 70 | 64 |
| round 1, the first repairs half compiled in | 518 | 53 | 48 |
| round 2 | 35 | 5 | 9 |
| round 3 | 33 | 7 | 5 |
| the five failing links of round 3, one partition each | 78 | 7 | (diagnostic) |
| round 4 | 8 | 4 | 2 |
| rounds 5 and 6 (after the tidy fixes), and the restored tooth | 0 | 0 | 0 |
| rounds 7 and 8, after the relwithdebinfo repairs (round 7's one failure a compiler crash, below) | 0 | 0 | 0 |

80 distinct locations in all, GLFW's four not counted (GLFW is out of the link-time compile since the previous batch).
Most are inside shared headers (`ir.hpp`, `world.hpp`, `array.hpp`, `toml.hpp`) and resolve to the callers named in the
"inlined from" chains; those callers are what was repaired. Every one was read as a possible defect first.

## What the warnings were

GCC reports a path on which a pointer that a visible branch makes null reaches a load or store. Three shapes covered
almost everything.

**1. A lookup the caller's invariant makes certain, behind `CRD_ASSERT(p != nullptr)`.** The assert compiles out
without asserts and, with asserts, can return (an ignored assert), so the null arm was a real read through null in a
shipping build if the invariant ever broke. Each such lookup now has an invariant accessor next to the nullable one,
fatal in every build on a miss (`CRD_FATAL` ends in `__builtin_trap` on GCC, so the path is cut):

| Accessor (new) | Callers | Locations it removed |
| --- | --- | --- |
| `ComponentRegistry::require_info(id)` | `World::backend_for`/`backend_for_const`/`get_component`, `on_relation_registered`, `add_relation_impl`, `ArchetypeChunkStorage::insert`/`release_slot`/`move_shared_components`, `instantiate_obek`, the OBEK cooker, `Commands::add_component` | `world.hpp:863` (764 lines), `:871`, `:297`, `:308` chains, `archetype_chunk_storage.cpp:129/152/221`, `world.cpp:424`, `obek.cpp:752`, cooker `obek.cpp:767` |
| `ArchetypeGraph::require(id)` | storage `insert`/`remove`/`for_each_chunk`/`on_entity_destroyed`, `World::for_each_chunk` | `component.hpp:52` (definite), `archetype_graph.cpp:166`, `world.cpp:45`, `array.hpp:193` |
| `HashMap::at(key)` | relation reverse index, CEIR printer, plan compile, audio render, `repair_manifoldness`, hesap-sched `TaskGraph` | `print.cpp:145/178`, `plan.cpp:1375`, `ceir_audio_render.cpp:183/324`, `array.hpp:193/325` |
| `toml::node::child_at(i)` | `table_range`'s iterator (`items()`) | `toml.hpp:108/207/233` via the cooker, frame cook and toml tests |
| `func::func_body(op)` | `to_ceir_frame` and the CEIR test builders | `ir.hpp:235/253/254/255` chains |
| `InspectHost::loaded_generation()` (private) | `load`, `start`, `op_at_line`, `file_path` | `inspect_host.cpp:187/354/372` |
| the scene renderer's `group_mesh(group)` (file-local) | `write_slot`, the upload | `scene_renderer.cpp:5746/5747` |

The canonicalizer's `rewrite_reshape_of_reshape` (`ir.hpp:151`) now refuses, fatally, an op its match rejected; the
Vulkan failure store's slow-wait counter (`vulkan_execution.cpp:423`) indexes the slot the creating lookup always
returns, as `record_locked` does; `ResourceManager::load_sync/async/streamed` (`resource_manager.cpp:856/1058/1296`)
reuse the evicted block step 1 found under the same lock instead of looking it up a second time.

Hot paths gain no compare: each accessor's miss arm is the branch the nullable lookup already had. Measured on the
probe TU `probe_scene.cpp` (session scratch; `get_component`, `get_component_mut`, `has_component` and
`add_component` of `Transform`), compiled with linux-gcc-shipping's flags against HEAD's headers and against the
change's: 11 conditional branches in every probe both ways; the extra 5 or 6 instructions are the cold fatal block
(argument setup, the report call, the trap) after the return. At HEAD the same probe TU fails the compile set itself
(6 `-Wnull-dereference` errors); the change's compiles clean.

**2. An allocation result followed unchecked.** `memory::construct`/`construct_array` return nullptr for a refused
allocation (DIAG.3a), and sub-budget allocators (linear, stack, pool, growable linear) return nullptr from `allocate`
when exhausted; the `IAllocator` comment still said "out-of-memory is fatal", which is true only of the heap
allocators, and is corrected. Consumers that cannot continue followed the null:

- **The CEIR context's IR arena** (`context.cpp:4611` definite, `:4626`, `ir.hpp:128-131`): every node, block, value
  array, region, operand slot, string, symbol table and dialect record now comes from `construct.hpp`'s new
  `allocate_or_fatal`, `allocate_array_or_fatal`, `construct_or_fatal` and `construct_array_or_fatal`, which turn a
  refusal into the named fatal "ceir::Context: the IR arena is out of memory" (DIAG.3a's policy for an owning arena);
  a zero count is still the empty array.
- **Real: the containers adopted a null buffer.** `Array`'s growth allocated `sizeof(T) * new_cap` without a check and
  without overflow protection, then relocated into it and kept the capacity, so a refused growth wrote through small
  addresses; the definite hit in `test_mcmc.cpp:73` is GCC proving that path. `Array` growth, `String`
  construction and growth, `HashMap` rehash and the fixed buffers of `RingBuffer`, `SpscQueue`, `AtomicArray` and
  `ConcurrentQueue` are now fatal on a refusal (size overflow included); `shrink_to_fit` keeps the buffer when the
  smaller one is refused. `try_reserve`/`try_grow` still report instead.

**3. Real defects and test oracles.**

| Site | Verdict | Repair |
| --- | --- | --- |
| `transform_propagation.cpp` (`world.hpp:308`, `vec.hpp:752`, 319 lines) | real: the DFS from a dirty root visits every `ChildOf` child; a child without a `Transform` (an organisation node, which the code already handled as a parent) was dereferenced (an assert, or a null read in shipping); and a dirty leaf under a dirty organisation node was never recomputed and kept its flag | a node without a `Transform` ends the walk; a dirty entity is a root unless its parent is dirty and carries a `Transform`; the writers mark only entities that carry one |
| `World::set_translation` and the other writers (`world.cpp:954`, definite) | real: a dead entity or one without a `Transform` was written through null in shipping | fatal in every build (the writers' documented contract) |
| `Commands::add_component` of an unregistered type | real: the payload copy read a null `ComponentInfo` in shipping | `require_info`, fatal |
| `scene_renderer.cpp` skeleton and base-color texture lookups (`anim_resources.hpp:75`) | latent: `get()` was called twice and re-reads the block's state each time | one read |
| `frame_ceir.cpp:497` | latent: `create_func`'s refusal was appended | refused, as the function's other refusals are |
| `cubic_spline.hpp:260` | false for every input `build` lets through | the periodic builder states its `n >= 4` precondition |
| `test_cluster_group.cpp:203` (definite) | false: `vc + 1U` wraps to 0 only for `vc = 2^32 - 1` | the count is formed in `usize` |
| `test_mcmc.cpp:73` (definite) | the test's gradient wrote `g[0]` whatever the dimension | it writes every dimension it is given |
| `test_planner.cpp`, `test_band12_gate.cpp` (definite), `test_band10_gate.cpp`, `test_ceir_add_vulkan.cpp`, `test_toml.cpp`, `test_frame_graph.cpp`, `test_solvers_cli.cpp`, `test_archetype_storage.cpp`, `test_index_framework.cpp`, `test_scene_storage_stress.cpp`, `test_diag_panel.cpp`, `test_scene_provenance.cpp`, `test_provenance.cpp`, `test_chir_provenance.cpp`, `test_cad.cpp`, `test_dcc.cpp`, `test_notebook.cpp`, `test_plugin.cpp`, `test_rewrite_driver.cpp`, `fuzz_diag_commands.cpp`, `sandbox/src/main.cpp` | oracles that dereferenced a nullable lookup: a missing result crashed the test instead of failing it | `REQUIRE` (or `CRD_FUZZ_REQUIRE`) or a small checked helper; the stress workers count misses (Catch2 is not thread-safe) |
| the CEIR test builders (21 test files, `create_func` then `func_body_block`, and the `find_op` walks of `test_compute.cpp`, `test_resource.cpp`, `test_transfer.cpp`) | the builders appended a possibly refused func and its possibly missing body | `REQUIRE` the func (`CRD_FUZZ_REQUIRE` in the differential fuzz target), `func::func_body` the body; the shared fixtures `corpus.hpp` and `rich_graph.hpp` use `CRD_FATAL`, since fuzz targets share them |
| `crash_capture_specimen.cpp:72`, `crash_specimen.cpp` | deliberate | the pointer itself is volatile, so the fault stays the program's own store: GCC had rewritten `*p = 42` into `movl $0x0,0x0; ud2` (objdump of both specimens on linux-gcc-release), and it no longer reports the deliberate fault |

**The release tree's one new link-time warning.** After the container changes, linux-gcc-release's
`bench_hesap_svds` link reported `'__builtin_memset' specified bound 18446744073709551612 exceeds maximum object size`
at `array.hpp:262` (the value-initializing loop of `Array::resize`) inlined from `dlasd1` (`svd_dc.hpp:837`). The
checked byte size had given the growth an upper bound, `4 * (2^62 - 1)` bytes, which GCC then took for a reachable
memset; the counts come from `int` sub-problem sizes the port never validated. Two repairs, both behaviour, not
wording: `memory::allocate_array` (and so every container's growth) refuses a byte size above `PTRDIFF_MAX`, which no
object can have (a pointer difference across it must be representable), and `dlasd1` now makes dlasd1.f's argument
checks (`NL < 1`, `NR < 1`, `SQRE` outside {0, 1}: INFO -1, -2, -3) before any work. Both have tests: the memory
partial-construction case asks for one element past `PTRDIFF_MAX` and requires that the allocator was never asked
(tooth: without the bound the request reaches the TLSF parent and ends in "TlsfAllocator: out of memory"), and a new
`dlasd1` case requires the three refusals with the inputs untouched.

**The relwithdebinfo tree.** linux-gcc-relwithdebinfo (`-O2 -g`, asserts compiled in) reads the same set and showed
what the asserted side hides: `CRD_ASSERT` can return (an ignored assert), so the read right after
`CRD_ASSERT(p != nullptr)` keeps its null arm in the program and GCC reports it there.

| Collection | Reported lines | Distinct reported locations | Failed links |
| --- | --- | --- | --- |
| round 1, after the shipping and release repairs | 78 | 40 | 22 |
| the 22 failing links, one partition each | 80 | 42 | (diagnostic) |
| round 2 | 3 | 1 | 1 |
| round 3 | 0 | 0 | 0 |

The one-partition relink of `crd-gpu-context-vulkan-tests` crashed lto1 itself (`internal compiler error: Segmentation
fault` in `cook_vertex_program_unchecked`, `vertex_asset.cpp:4305`): a toolchain crash of that diagnostic mode only.
Its round-1 site was `sparse_set_storage.cpp:205`, and the regular link of round 2 is green.

| Site | Verdict | Repair |
| --- | --- | --- |
| `TlsfAllocator`'s borrowing constructor (`tlsf_allocator.cpp:324/325`, definite) | real: a null buffer passed the assert's ignore arm and `init_pool` wrote the sentinels through it | fatal in every build, "TlsfAllocator: the borrowed buffer is null" |
| `SparseSetStorage::ensure_pool` (`sparse_set_storage.cpp:205`, in 15 programs) | the registry lookup behind an assert, then the pool's allocation followed unchecked (one of follow-up (h)'s sites) | `require_info`, and the pool comes from `construct_or_fatal`: "SparseSetStorage: out of memory for a component pool" |
| the OBEK and scene cookers (`obek.cpp:435/848`, `scene.cpp:534`), `World::instantiate_scene` (`scene_resource.cpp:525`) | the info of a component found by its FourCC, behind an assert | `require_info`; the cookers' helper takes the info by reference |
| `greedy_rewrite_rules`' build action (`ir.hpp:150` via `rewrite_rules.hpp:265`) | the walk `operand(i).def.operand(j)` needs the constraint `find_rewrite_misuse` pairs it with; a rule that skipped the lint followed a null definer | the missing definer is fatal (the driver's contract), as in the C++ canonicalizer |
| `build_gradient` (`ir.hpp:198` via `grad.cpp:288`) | false: the loss's definer was read twice around a call | read once |
| `BundleWriter::finish` (`memcpy` via `bundle.cpp:227`, definite) | false in practice: the header was copied into a buffer sized by `sizeof(header) + sections`, which GCC lets wrap to the empty, null buffer | the header and the sections go through the same appends as the sections themselves |
| `expand_ml_ops` (`rehash.constprop`, every member access of `hash_map.hpp:392-576` reported definite) | false: the plain expand passed a null lineage map and the insert was guarded, but IPA-CP had specialised `HashMap::insert`, `ensure_capacity_for_insert` and `rehash` for a constant null `this` (the `-fdump-ipa-cp-details` dump of the link names the call site) | the driver takes a recorder callable; the plain expand passes one that keeps nothing, so no copy of the map's code sees a null map |
| `test_component_registry.cpp`, `test_slot_map.cpp`, `test_hot_reload.cpp`, `test_reload_provenance.cpp`, `test_result_oracle.cpp`, `test_execute.cpp`, `test_grad.cpp`, `test_scene_provenance.cpp` | oracles that dereferenced a nullable lookup | a `REQUIRE`d local or a checked helper (`registered`, `installed_program`) |
| `smoke_mesh.cpp` (`array.hpp:194`) | real for the oracle: `CRD_VERIFY` reports only where asserts are compiled in, so the example's checks could never fail a release run, and the mesh was dereferenced after an ignorable check | each check reports and counts in every build; a missing mesh ends the run with exit 1 |

**Pre-existing tidy debt in the touched headers**, fixed so they gate clean: `[[nodiscard]]` on `toml.hpp`'s view and
result accessors and on `RingBuffer`'s, `AtomicArray::npos` renamed `kNpos`, a confusable `hl`/`h1` pair in
`cubic_spline.hpp`, a default member initializer in `corpus.hpp`; `toml.hpp`'s toml++ spellings and
`component_registry.hpp`'s standard trait names carry `NOLINT(readability-identifier-naming)` with the reason.

## The new tests and their teeth

- **Transform propagation, organisation nodes** (`test_transform.cpp`, three `[organisation]` cases). Teeth on win-debug,
  each restored byte for byte, rebuilt and rerun green: without the walk's stop the first case crashes
  (0xC0000005); without the root rule the third fails; with the writers marking every entity the first two fail.
- **Contract fatals in child processes.** `crd_diag_fatal_tests` (`cmake/CrdDiag.cmake`) registers one CTest per mode of
  a specimen that arms the expected `CRD_FATAL` message (`tests/support/diag/specimens/expected_fatal.hpp`) and passes
  only on its `CRD_DIAG_FATAL_OK <mode>` line; another assert, a crash, a hang or a clean return fails it.
  `crd-containers-fatal-*` (9: the seven containers' refusals and `HashMap::at`), `crd-scene-fatal-*` (6, the sparse
  pool's refusal among them), `crd-ceir-fatal-*` (5, `func_body` and the rewrite driver's missing definer among them)
  and `crd-toml-fatal-*` (1); the memory helpers are four new modes of `crd-diag-alloc-fatal-specimen` run by the memory
  tests, with a fifth for the borrowed null TLSF buffer, and an in-process case shows a grant passes through and a zero
  count is the empty array. Teeth on win-debug: without the check, `Array` growth, the CEIR arena and the transform
  writers SEGFAULT; `require_info`, `HashMap::at`, `child_at` and `allocate_or_fatal` fail their modes; without the
  `PTRDIFF_MAX` bound the memory case's request reaches the TLSF parent ("TlsfAllocator: out of memory"); every one
  restored, rebuilt and green. The relwithdebinfo round's three guards, the same way: with `TlsfAllocator`'s null-buffer
  fatal back to the assert, the memory harness case fails its `tlsf-borrowed-null` mode; without the rewrite driver's
  definer check `crd-ceir-fatal-rewrite-inner-op-missing` fails, and with the pool's raw allocation back
  `crd-scene-fatal-sparse-pool-refused` fails; each restored byte for byte, rebuilt and green.
- **The link gate.** Reverting one repair (`print.cpp`'s `m_ids.at` back to `*m_ids.find`) fails the crd-ceir-tests
  link on linux-gcc-shipping with `print.cpp:145:25: error: potential null pointer dereference
  [-Werror=null-dereference]`; the same revert under the previous link line (no `-Wnull-dereference`) links green; the
  repair and the line restored, the tree relinks with no warning and crd-ceir-tests passes (577 cases). A synthetic
  two-file tooth inside crd-perf-ui-tests did not bite: GCC folded the null arm into a plain load inside the Catch2
  case without isolating the path, although the same two files warn as a standalone program; the real revert is the
  tooth.

## Verification

Local only; nothing pushed. Suites ran from a scratch directory under the build tree, each bounded; the touched set is
the 33 suites of the scene, containers, memory, toml, CEIR (core, cook, host, gpu, gpu-vulkan), chir, audio, frame
cook, render graph, draw, perf, perf-ui, core, jobs, stress, hesap (dense, stats, interp, sched), mesh processing,
resources, cookers, ceridc, vulkan context, scene render, anim and eylem consumers.

| Lane | Build | Tests |
| --- | --- | --- |
| linux-gcc-shipping (full tree, GCC 13.3) | rounds 1-8 as above; round 8 builds and links every target with no `warning:` and no failure | the 21 contract-fatal CTests; the crash-specimen, alloc-fatal and memory CTests (45) |
| linux-gcc-release (full tree) | builds and links every target, no `warning:` | the 21 contract-fatal CTests; crash, alloc-fatal and memory (45) |
| linux-gcc-relwithdebinfo (full tree) | rounds 1-3 as above; round 3 links every target, no `warning:` | the 21 contract-fatal CTests; crash, alloc-fatal and memory (47) |
| win-debug (all targets) | clean | 33 of 33 suites; 21 fatal and 9 `crd-no-*`/`crd-check*` CTests; the 13 `crd-diag-overhead` CTests; `smoke_mesh` exits 0 |
| win-shipping (all targets) | clean | 33 of 33 suites; fatal, guard and overhead CTests (43) |
| win-clang-cl-shipping (touched targets) | clean, no `warning:` (second attempt, see below) | 33 of 33 suites; fatal, guard and overhead CTests (43) |
| win-asan (touched targets: memory, containers, scene, CEIR, CEIR gpu and cook, toml, perf, resources, cookers, stress, hesap dense) | clean (second attempt, see below) | 13 of 13 suites under the MSVC environment; the 21 fatal CTests |
| linux-gcc-debug (touched targets) | clean | 31 of 31 suites, the 21 fatal CTests |
| linux-gcc-asan (touched targets) | clean | 31 of 31 suites, the 21 fatal CTests; the only sanitizer reports in the logs are the deliberate specimens' that the memory, perf and core suites run and judge |
| linux-clang-tsan (touched targets, `setarch -R`) | clean | 31 of 31 suites after one rerun (below), the 21 fatal CTests; the one ThreadSanitizer report is the deliberate data-race specimen's, which the jobs suite judges |

On the first linux-clang-tsan run one case of crd-ceir-tests failed: "diag 8b: a pause request and a cancel stop a
long run safely" read `NoSuchValue` (4) for the paused op's value where it expects `NotYetComputed`. It passed six
isolated reruns and a rerun of the whole suite (577 cases), and the inspection session and the plan runner are
untouched by this change; recorded, and queued as follow-up (j) below.

`crd-anim-tests` reads `Fox.glb` through `CRD_ASSETS_SOURCE`, which its CTest sets; run outside CTest without it, it
fails its first case on both hosts, and with the variable it passes (5 cases). Host faults seen and rerun, none
reproducing: on WSL `cc1plus` aborted with heap corruption (`malloc_consolidate(): invalid chunk size`) compiling
`test_mip.cpp` during the shipping round 7 and compiled clean in round 8; on win-clang-cl-shipping the clang frontend
crashed on a signal compiling `fft.cpp`, a file this change does not touch, and compiled on the retry; on win-asan
MSVC stopped with C1001 (an access violation inside the compiler, at `xutility(1907)`) compiling
`containers_fatal_specimen.cpp` and compiled it on the retry. Earlier in the batch, a corrupt COMDAT (LNK1163) in one
win-debug object and two MSVC C1001 internal errors in win-shipping under heavy parallel load, each rebuilt clean.
Also clean: `tidy-files.py` on every changed C++ file (LLVM 20.1.8),
`clang-format --dry-run` on the added lines, `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

## Follow-up for an extra batch

(e) win-clang-cl-shipping's two zstd `-Wunused-function` (`HUF_isSorted`, `ZSTD_isAligned`), queued before this batch.

(h) Allocation results the compiler cannot see as nullable, followed unchecked in the same shape as the CEIR arena:
`ArchetypeGraph::archetype_for` (`m_archetype_pool.allocate` then placement new), `World::on_relation_registered`
(`RelationInfo`), `ResourceManager`'s control blocks (`make_failed_block` and the three load paths) and the async and
streamed load contexts (`new (raw)` after `m_alloc->allocate`; `smoke_mesh` runs the manager on a TLSF pool, which
refuses when exhausted). Each should take `construct_or_fatal` (or report) with a refusing-allocator mode in the
owning fatal specimen. `SparseSetStorage::ensure_pool` was repaired here (the relwithdebinfo round).

(i) The scene loaders trust a loaded file's descriptors in shipping. `World::instantiate_obek` and
`World::instantiate_scene` only `CRD_ASSERT` a descriptor's `size` against the registered size and against the
1024-byte staging buffer, its alignment and version, and each record's entity index, so a corrupt pack can overflow
the stack buffer or index past the entity table; and the SCEN loader's payload bound `record_count * d.size` is a
`u32` product that wraps, so a crafted descriptor passes the size check with a payload far shorter than its records.
The loaders should refuse such descriptors (a reported load failure), with corrupt-pack cases in the scene tests.

(j) `test_inspect.cpp`'s "diag 8b: a pause request and a cancel stop a long run safely" asserts `NotYetComputed`
for the value of whichever op the pause holds. If a pause can hold an op without a result (`func.call` of a void
callee, `core.yield`, `func.return`), the snapshot of its result 0 is `NoSuchValue`, which is what the one failure
read; establish where a pause can land and make the case read the status of a result-bearing op only (or state the
landing rule and assert it), never a retry.
