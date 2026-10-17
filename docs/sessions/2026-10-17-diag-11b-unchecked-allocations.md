# DIAG.11b: allocation refusals in the scene and the resource manager, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [DIAG.3a partial construction](../design/runtime-diagnostics.md#diag-3a). Previous batch: [zstd's clang-cl and
> configure warnings; where a pause lands](2026-10-17-diag-11b-zstd-configure-and-pause-landing.md), which queued
> follow-ups (h), (i), (k) and (l).

## Goal

Follow-up (h): allocation results followed unchecked in the archetype graph, the relation table and the resource
manager's control blocks and load contexts, each to become a named out-of-memory fatal with a refusing-allocator mode
in its module's fatal specimen. Before the repair the scene, resources and memory modules were swept for the same
shape (an allocation from a caller-supplied allocator followed by a write or a placement new, guarded at most by an
assert that a build without asserts drops); the sweep's finds in those modules are repaired here too. Committed
locally, not pushed. (i), (k) and (l) stay queued, and the same sweep over the rest of the engine queued (m).

## Why a refusal reaches these sites

`IAllocator::allocate` is fatal on exhaustion only for the heap allocators (malloc, TLSF, virtual memory, growable
TLSF). The sub-budget ones (linear, stack, pool, growable linear) return nullptr when exhausted, by contract
([allocator.hpp](../../engine/foundation/memory/include/crd/memory/allocator.hpp)). A `World`, a `SparseSetStorage`, a
`SharedComponentPool` and a `ResourceManager` take any `IAllocator*`, so a refused request is a normal event there, and
code that writes through the result unchecked follows null in every build without asserts (shipping, release).

## Sites

| Site | Before | Now |
| --- | --- | --- |
| `World::on_relation_registered` (a relation's `RelationInfo`, the reverse index) | `CRD_ASSERT(mem != nullptr)`, then placement new | `construct_or_fatal`: "World: out of memory for a relation's reverse index" |
| `SparseSetStorage::Pool::grow_dense` | `CRD_ASSERT(new_dense != nullptr)`, then live slots moved into it; unchecked `new_cap * size` | checked size (fatal on overflow) and `allocate_or_fatal`: "... out of memory for a component pool's dense array" |
| `SparseSetStorage::insert_shared` (the lazily created `SharedComponentPool`) | assert, then placement new | `construct_or_fatal`: "SparseSetStorage: out of memory for a shared component pool" |
| `SharedComponentPool::grow` | assert, then the old entries copied into it; unchecked size | checked size and `allocate_or_fatal`: "SharedComponentPool: out of memory for its entries" |
| `ArchetypeGraph::archetype_for` | `GrowablePoolAllocator::allocate`, whose own fatal ("GrowablePoolAllocator: out of memory") already stopped a refused page; the assert after it was dead | `try_allocate`, and the graph's own fatal: "ArchetypeGraph: out of memory for an archetype" |
| `ChunkAllocator::allocate` | the same pool fatal, dead assert | `try_allocate` and "ChunkAllocator: out of memory for a chunk" |
| `ResourceManager`: `make_failed_block` and the first block of `load_sync`, `load_async`, `load_streamed` | four copies of an unchecked allocate and placement new | one `new_control_block` helper over `construct_or_fatal`: "ResourceManager: out of memory for a resource control block" |
| `ResourceManager`: the async and the streamed load context | unchecked allocate, then placement new | `allocate_or_fatal`: "... out of memory for an async load context" / "... a streamed load context" |
| `LinearAllocator`, `StackAllocator`, `PoolAllocator` borrowing constructors | `CRD_ASSERT(buffer != nullptr)`; the pool then linked its free list through the buffer | fatal in every build ("<Arena>: the borrowed buffer is null"), as the TLSF one became in the previous batch; the borrowing pool also gets the owning constructor's checked stride and span (the destructor unpoisons `slot_size * slot_count` bytes) |

The control block is what the manager reports through (a failed load is a Failed block too), so a refused block cannot
be turned into a report; out of memory is the honest result, the DIAG.3a rule for a caller that cannot continue. The
loaders' payload allocations (`try_allocate` on the payload heap, a graceful Failed or Placeholder) are unchanged.
Left as they are after reading: `ArchetypeChunkStorage::insert`'s and `SparseSetStorage::insert`'s
`CRD_ASSERT(data != nullptr)` (a caller's argument, not an allocation), and the `default_allocator()` sites
(`platform/input.hpp`, `vulkan_gpu_allocator.cpp`), whose `MallocAllocator` is fatal on exhaustion.

## Tests and teeth

New child-process modes, each refusing exactly the allocation it is about:

- `crd-scene-fatal-*` (six more, 12 in all): `sparse-dense-refused`, `shared-pool-refused`, `shared-grow-refused`,
  `relation-index-refused`, `archetype-pool-refused`, `chunk-pool-refused`. Each grants the setup and refuses from the
  violating call on, whose first allocation is the one the mode names (the relation mode registers one component
  first, so the registry's tables already have their first buckets when the relation registers).
- `crd-resources-fatal-*` (new specimen `crd-resources-fatal-specimen`, owner `crd-resources-tests`):
  `failed-block-refused`, `sync-block-refused`, `async-block-refused`, `streamed-block-refused`,
  `async-context-refused`, `streamed-context-refused`. The allocator refuses by request shape, not by count: a
  control block is the only request of its size and alignment (strings ask for byte alignment), so the block modes
  refuse exactly it, and the context modes grant it and refuse from there on, after a warm-up load has given the
  in-flight table its buckets. The two-entry pack is written to the temporary directory and deleted once mounted.
- crd-memory-tests' arena case (renamed "an arena refused its backing memory or handed an unusable buffer reaches its
  fatal") runs the alloc-fatal specimen's new `linear-borrowed-null`, `stack-borrowed-null`, `pool-borrowed-null` and
  `pool-borrowed-overflow` modes.

Teeth (win-debug): the nine engine sources set back to HEAD together and every specimen rebuilt. Each of the 16 new
modes failed: the four scene growth modes printed `CRD_DIAG_FATAL_OTHER` (the old ignorable assert), the archetype and
chunk modes `CRD_DIAG_FATAL_OTHER` with the pool's generic fatal, all six resources modes crashed (SegFault, the
placement new into the refused pointer), the three borrowed-null modes exited 97 (the old assert) and the overflow mode
crashed (0xC0000005). The six earlier scene modes passed throughout. Restored from byte-exact copies, touched, rebuilt:
18 of 18 and the four memory harness cases pass. Simultaneous reversion is sound here because in every mode the target
is the run's first refused allocation, so no other reverted site is reached with a refusal.

## Verification

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | crd-memory-tests, crd-scene-tests, crd-resources-tests and the three specimens, no warning | the three suites (165, 283, 131 cases); the 33 contract-fatal CTests (containers 9, scene 12, ceir 5, toml 1, resources 6) and the 9 `crd-no-*`/`crd-check*` guards |
| win-shipping | the same | the three suites; the 33 fatal CTests |
| win-clang-cl-shipping | the same (every link on the first try) | the three suites; the 18 scene and resources fatal CTests |
| win-asan (under vcvars) | the same | the three suites; the 33 fatal CTests |
| linux-gcc-debug | the same, 0 warnings | the three suites; the 33 fatal CTests |
| linux-gcc-asan | the same, 0 warnings | the three suites (the only reports are the allocator-poison specimens' deliberate ones); the 33 fatal CTests |
| linux-clang-tsan | the same, 0 warnings | the three suites, no TSan report; the 33 fatal CTests |
| linux-gcc-shipping | every target (`cmake --build`, `-k 0`): 188 links, the GCC link-time set with `-Werror` on every engine link line (the new specimen's included), exit 0, no `warning:` line | the three suites; the 33 fatal CTests and the 8 guards |
| linux-gcc-relwithdebinfo (asserts compiled in) | every target: 189 links under the set, exit 0, no `warning:` line | the three suites; the 33 fatal CTests |
| linux-gcc-release | every target: 189 links under the set, exit 0, no `warning:` line | the three suites; the 33 fatal CTests |

Static checks: strict tidy (LLVM 20.1.8) clean on the 13 changed C++ files; clang-format `--dry-run` reports nothing on
any added line (the new specimen is clean whole); `check-allman-braces.py`, `check-master-plan.py` and
`check-repository.py` pass.

Workspace repair: `build/project-sync/registrations-*.json` still named `build/scratch-cfg/after-win-vs`, the scratch
Visual Studio tree the previous batch configured and deleted, so every configure of win-debug stopped with "Registered
native build is missing". The stale entry was removed, leaving `build/win-vs-debug` as before that batch.

## Hosted evidence needed once pushed

- `crd-scene-fatal-*` (12) and `crd-resources-fatal-*` (6) pass on every lane that runs CTest, each by its
  `CRD_DIAG_FATAL_OK <mode>` line.
- crd-memory-tests' arena case passes with the four borrowed modes on every lane.
- The three link-time-optimized Linux lanes link `crd-resources-fatal-specimen` and every changed consumer with no
  `warning:` line (locally: full builds of all three, above).

## Queued

- (m) The same sweep over the rest of the engine, by module, with the convention each module already has (a status
  where it reports one, as `hesap-tensor`'s `TensorStatus::AllocFailed`, otherwise the named fatal):
  - execution: `ceir-cook/src/hot_reload.cpp:253`, `:255` (`ReloadSet::alloc_generation`), `:494` (`cook_source`'s
    `Context`); `ceir-cook/src/plan_cache.cpp:118`, `:129` (the artifact and dependency copies; the second size is an
    unchecked product); `ceir-host/src/host_provider.cpp:689` (`PooledToken`);
    `ceir/include/crd/ceir/diagnostic.hpp:75`, `:84`, `:154` (the diagnostic arena's note and fix-it arrays, unchecked
    products, and `copy_str`);
  - numerics: `hesap-fft` `fft.hpp` 32 sites (256-258, 424, 429-430, 444-445, 469, 471, 518, 546-547, 595, 607-608,
    649-652, 672-673, 692-693, 715, 717, 732-737), `bluestein.hpp:35`, `nd_fft.hpp:51`, `:57`, `sparse_fft.hpp:52`;
    `hesap-dense` `blas3.cpp:159`, `:160`, `:281`, `:434`, `:435`, `detail/syrk_microkernel.hpp:154`, `:155`,
    `matrix.hpp:54`, `matrix_catalog.hpp:95`, `matrix_types.hpp:43`, `vector.hpp:52`; `hesap-dsp` `convolution.hpp:127`,
    `spectral.hpp:71`, `:360`; `hesap-wavelet` `cwt.hpp:528`; `hesap-autodiff` `vhvp.hpp:56`, `:59`; `hesap-tensor`
    `sparse.hpp:595`;
  - kir: `ckir_eval.hpp:145`, `:1012`, `ckir_grad.hpp:53`;
  - the capacity arithmetic next to a repaired site: `SharedComponentPool::grow` doubles a `u32` capacity in a
    `while (new_capacity < min_capacity)` loop that wraps to zero and never ends once the capacity passes 2^31 (the
    entry index is a `u32`, `kInvalidIdx` its last value). `SparseSetStorage::Pool::grow_dense` was read too: its
    doubling wraps the same way, but it then falls back to `min_capacity`, so it stays correct. The pool's case is not
    reachable without about 2^31 entries, so the repair needs a capacity helper tested on its own (the doubling and an
    index-range fatal), not a test of the pool itself.
- (i), (k) and (l) as the previous batch queued them.
