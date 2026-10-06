# DIAG.3b TLSF and ring poisoning, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.3b](../ROADMAP.md#slice-diag.3b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-3b). Rules: [AGENTS](../../AGENTS.md).
> Earlier batch the same day: [pool and arena poisoning](2026-10-07-diag-3b-pool-and-arena-poisoning.md).

## Goal

Close the second of the two local items the pool/arena batch left on DIAG.3b: TLSF and growable-TLSF free blocks,
and the ring allocator's retired space. The streaming allocator's resident heap and staging ring are built on these
two, so it is covered by them. Container live ranges remain.

## What changed

- `TlsfAllocator` (`engine/foundation/memory/src/allocators/tlsf_allocator.cpp`). Under ASan every byte of the pool
  that is not a live payload is poisoned: each block's 16-byte header, the sentinels, and each free block's payload
  with its free-list links. Every header access goes through `meta_load`/`meta_store`, which open exactly the 8-byte
  word, touch it and restore its previous state (a word that was addressable, such as a remainder header written into
  a live payload during an in-place shrink, stays addressable until the shadow is re-derived). `block_apply_shadow`
  sets a block's whole shadow from its free flag; `try_allocate`, `deallocate` and both in-place `reallocate` paths
  call it on the blocks they leave behind, so splits, merges, the leading alignment remainder and coalesced headers
  need no per-step bookkeeping. `init_pool` poisons the whole region; the destructor unpoisons exactly the region in
  both ownership modes, which is what `GrowableTlsfAllocator` relies on (it destroys each chunk's non-owning heap,
  then frees the pool). Without ASan the accessors are plain loads and stores.
- `RingAllocator`. The constructor poisons the staging buffer and the destructor unpoisons it before returning it to
  the parent. A claim opens the granules it overlaps after its CAS wins; alignment padding and wrap waste stay
  poisoned. `retire` poisons the granules wholly inside the retired span (split at the wrap) before the tail CAS
  publishes it. In ASan builds only, retirers take a small spin lock (`m_retire_lock`, declared under `CRD_MEM_ASAN`),
  because two concurrent retirers could otherwise let the slower one poison bytes a producer had already reclaimed
  after the faster one's publication. Producers never take it; the non-ASan algorithm is unchanged.
- `tests/foundation/memory/test_allocator_contracts.cpp`: the seeded TLSF metadata-corruption test opens the forged
  header word with `asan_unpoison` before stomping it. The walker oracle is unchanged.
- The negative control `crd-diag-allocator-poison-specimen` gains ten modes (25 in all): `tlsf-freed` (a freed
  block's first link byte), `tlsf-overrun` (one byte past a live 48-byte block whose neighbour is live),
  `tlsf-underrun` (the byte before a live block), `tlsf-aligned` (a 256-aligned block in a 256-aligned caller pool:
  the last byte of the free leading remainder), `tlsf-large` (the middle of a freed 64 KiB block), `tlsf-shrink`
  (byte 32 of a block reallocated in place from 256 to 32), `gtlsf-freed` (a freed block in a growable heap's second
  chunk), `ring-retired`, `ring-overrun` (unclaimed space after a claim) and `ring-underrun` (alignment padding before
  a 64-aligned claim). A layout the mode did not get (a shrink that moved, a heap that did not grow) exits 95.
- `tests/foundation/memory/test_allocator_asan.cpp`: two new in-process cases assert the shadow directly under ASan
  (and its absence elsewhere): TLSF headers, freed and coalesced blocks, odd 13-byte and 4099-byte, 64 KiB and
  256-aligned blocks, in-place shrink and grow, a caller buffer returned addressable, a growable heap across chunks;
  and a ring through padding, retirement, a wrapping claim and a retired span that wraps.

## Decisions

- **Every TLSF header is poisoned, not only free ones.** Headers are the only bytes between payloads, so this is
  what makes one-byte over- and underruns detectable whatever the neighbour's state. It is stronger than the pool
  policy, where an overrun into a live neighbour slot is a raw-pointer limit.
- **Restore, not force, the word's state.** An unconditional re-poison after a header access would poison live user
  bytes when `deallocate` is handed an interior pointer (the DIAG.3a/3d refusal reads a "header" inside a live
  payload) and when an in-place shrink writes the remainder header into the live block.
- **Ring granularity.** `try_claim` accepts alignment 1, so two producers can claim neighbouring bytes of one 8-byte
  granule concurrently. ASan updates a partial granule's shadow by read-modify-write, so exact claim boundaries could
  race into a false report. Claims therefore open whole granules (two claims sharing a granule write the same value)
  and retirement closes only whole granules, so no claim and retirement write one shadow byte. Detection is
  byte-exact for claims whose offset and size are multiples of 8, which the default 16-byte alignment gives for
  8-multiple sizes.
- **Serialise retirers only under ASan.** Poisoning before the tail publication is what keeps a producer from
  reclaiming a byte before its poison lands; a lock among retirers closes the remaining overlap. Retirement is
  consumer-side and rare, and the shipping path keeps its lock-free CAS.

## Evidence

- win-asan: the memory suite passes, 160 test cases and 824,325 assertions, and all 25 specimen modes end in
  `use-after-poison` (exit 42). Because TLSF backs many suites, the whole win-asan tree was rebuilt and its full CTest
  run passed: 7,221 tests, 0 failed (one test, `CEIR-31b-3-c-ii`, reports Skipped as before this change).
- Teeth on win-asan, with `block_apply_shadow`, the TLSF `init_pool` poison, the ring constructor poison and the
  ring retirement poison turned into unpoisoning: all ten new modes exit 0, and 3 test cases with 50 assertions fail
  (the specimen case and the two new in-process cases). After restoring (rewritten, so the mtime moved) and
  rebuilding win-asan, the suite passes again with 25 `use-after-poison` reports.
- win-debug: the whole tree builds (`ring_allocator.hpp` reaches the streaming and resource code); memory suite 160
  cases, 824,325 assertions; the specimen reports `InstrumentAbsent` in all 25 modes.
  win-shipping and win-clang-cl-shipping: 159 cases (the debug-only stats case is not registered), all green.
- WSL reference host, one build at a time: linux-gcc-asan passes 160 cases and its log holds 25 `use-after-poison`
  reports, one per mode. linux-gcc-debug and linux-clang-tsan pass 160 cases (the TSan lane exercises the unchanged
  lock-free retire path).
- The seeded TLSF corruption test now opens the forged word first; without that it faults under ASan, which is the
  intended detection of a metadata stomp.
- Strict `tidy-files.py` is clean on the six changed C++ files. `clang-format --dry-run --Werror` adds no violation
  (the two allocator sources keep their older aligned-assignment findings, 10 and 16, as at HEAD).
  `check-allman-braces.py`, the `crd-no-*`/`crd-check*` CTest guards, `check-master-plan.py` and
  `check-repository.py` pass.
- A watchdog note for later runs: the box's `CRD-Loop-Watchdog` reaps scratchpad `cmd`/`bash` wrappers older than
  20 minutes. It killed the wrapper around the 66-minute CTest run, but `ctest.exe` itself kept running and wrote
  its full log; waits are kept under 10 minutes each.

## Remaining on DIAG.3b

- Container live ranges: annotate `crd::containers::Array` (and the other contiguous engine containers) with
  `__sanitizer_annotate_contiguous_container`, so unused capacity is poisoned, plus a specimen mode that reads past
  `size()` inside `capacity()`. `array.hpp` is included everywhere, so this needs a win-debug `all` build.
- Hosted: the first change-tier run after the push must show the memory suite green on win-asan and linux-gcc-asan,
  with `allocator asan: every poisoned access is a use-after-poison report` passing (25 modes caught), the full
  win-asan and linux-gcc-asan CTest green (TLSF backs many suites), and on the non-ASan lanes the same case passing
  as `InstrumentAbsent`.
