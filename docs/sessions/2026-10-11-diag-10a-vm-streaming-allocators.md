# DIAG.10a virtual-memory and streaming allocators, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a host provider parallel range](2026-10-11-diag-10a-host-parallel-range.md).

## Goal

The allocator operation-sequence target left out the two allocators that reserve address space, the
`VirtualMemoryAllocator` (the bump arena over `crd::vm`) and the `StreamingAllocator` (budgets and a residency
policy over a growable TLSF on that arena, plus a staging ring). This slice puts both into `crd-fuzz-memory-allocators`
under the same rules: only contract-valid operations reach them, malformed requests are generated on purpose, and the
oracles are an independent model. Committed locally, not pushed.

## What changed

- `tests/foundation/memory/fuzz/fuzz_allocator_ops.cpp`: two kinds appended to the kind enum, `VirtualMemory` and
  `Streaming`. The first byte of every committed input (17 written seeds, 6 adopted inputs) is below 11, the old count,
  so each keeps its kind under the new modulus; no input was rewritten. Each input reserves at most 16 MiB, commits
  only what it bumps through and releases the range with the allocator.
  - The arena runner holds the allocator to an exact model: the bump offset (the bump arenas' `bump_fits`), the
    committed high-water mark (the next multiple of the documented commit block, the configured block rounded up to
    whole pages, capped at the reservation; a purge back to the page holding the live top), the most recent allocation,
    and the end of the bytes the harness has written since their pages were last committed: every byte past it must
    read zero when handed out. Under ASan the committed tail above the live top must be poisoned (checked from a granule
    boundary) and every live block unpoisoned. `owns()` is bounded by the live top, `bytes_in_use` equals the offset.
    Configurations cover a commit block of 0, any byte count, page multiples (most not a power of two) and values near
    `SIZE_MAX`, and an initial commit of nothing, any size (past the end too) and `SIZE_MAX`. `reallocate` runs only
    where the model shows neither fatal path can be reached: in place for the most recent allocation when it already has
    the requested alignment (and the new size fits the reservation), otherwise a bump-copy that fits.
  - The streaming runner scripts a residency policy per request: shed blocks of the requested category, of any
    category, one block, nothing, or lie (claim bytes it never freed). The policy releases only blocks the model knows
    to be resident, after checking their payload, and checks that each release un-charges exactly what the allocation
    charged. Every category's `used()` must equal the model after every operation; a success must be charged at least
    the request and less than 64 bytes more, and must leave the category within its budget; a request nothing can serve
    (malformed, larger than its budget, larger than the reservation, aligned past every address in it) must be refused
    without consulting the policy and without committing anything; a valid failure must have consulted the policy (once
    for the null policy, exactly 17 times for the liar) unless the request is within that 64-byte charge of its
    budget. Staging claims run under the ring's epoch discipline and never overlap a resident block; allocations and
    releases also go through `StreamingCategoryAllocator`.
  - `run_heap` (TLSF and the growable TLSF) now also requires that a refused request grows no chunk.
  - 14 written seeds: one for the growable TLSF, seven for the arena, six for the streaming layer.
- `engine/foundation/memory/src/virtual_memory_allocator.cpp`, `allocators/growable_tlsf_allocator.cpp` and
  `allocators/streaming_allocator.cpp`: the fixes below. No header changed.
- `tests/foundation/memory/test_streaming_allocator.cpp`: one `[memory][streaming][diag10a]` case.

## Findings

1. **The arena's `try_allocate` asserted on a non-power-of-two alignment** instead of refusing it, the DIAG.3a contract
   every other allocator's `try_allocate` keeps (the TLSF had the same defect, fixed in the first allocator batch).
   Seed `vm-non-pow2-alignment-refused`.
2. **A commit block that is a page multiple but not a power of two was rounded with the power-of-two mask.**
   `align_up(target, 12 KiB)` clears the bits of `12 KiB - 1`, so the arena committed page multiples that were not block
   multiples (a 5,000-byte pre-commit committed 16 KiB instead of 12 KiB). The header promises "the next multiple of
   this"; the round-up now divides, capped at the reservation. Seed `vm-commit-block-not-pow2`.
3. **A commit block no page multiple can hold wrapped to 0 and became one page.** A block near `SIZE_MAX` is larger than
   any reservation; it now commits the whole reservation at once. Seed `vm-commit-block-past-size-max`.
4. **A reallocate of the top block asking for a stricter alignment returned it in place, misaligned.** The in-place path
   ignored `alignment`; a top block at offset 16 asked for 4,096 came back at offset 16. It now moves like any other
   block. Seed `vm-realloc-alignment`.
5. **A request no resident heap state can serve reached the eviction loop.** A size larger than the whole reservation,
   an alignment past any chunk or a non-power-of-two alignment failed in the heap, and the streaming layer then asked the
   policy to shed: a cooperative policy released live resident data for a request that could never succeed, up to 17
   times. Such requests are now refused first. Seeds `stream-unservable-sheds-nothing` and `stream-non-pow2-alignment`.
6. **`GrowableTlsfAllocator::try_allocate` grew a chunk for a non-power-of-two alignment and then refused it.** Every
   chunk refuses the alignment, so the walk ended in `grow()`, which took a new chunk from the parent; the new chunk
   refused it too and stayed. Under a virtual-memory parent the chunk is committed and its range never returns (the
   arena's free is a no-op), and through the streaming loop each such request could leave 17. It is now refused before
   the walk. Seed `growable-tlsf-refusal-grows-nothing`.
7. **The streaming budget checked the request but charged the heap's block.** 999 bytes against a budget of 1,000 were
   charged 1,008, carrying the category past its budget (the unit test "enforces a per-category budget" states the
   budget is never exceeded; its 10 KiB requests round to nothing). The budget now holds the charge: a block rounded
   past the remaining budget goes back to the heap and the request takes the eviction path, and a block larger than the
   whole budget is refused without consulting the policy, since no eviction can make it fit. Seed
   `stream-budget-charge` and the new unit case.

The charge bound the target checks on every success (less than 64 bytes over the request) restates the TLSF block
rule, 16-byte granules and a trailing remainder kept only when it is too small to be a free block; it held on every
seed and libFuzzer input here.

## Teeth

Each break applied alone on win-debug, the target rebuilt, the named seed or case run, the original bytes restored and
the file's time stamp bumped, then both targets rebuilt and the corpus and suite green again.

| Break | Fails |
| --- | --- |
| T1 the arena's `try_allocate` asserts on the alignment | `vm-non-pow2-alignment-refused` (the assertion) |
| T2 the mask round-up of the commit block | `vm-commit-block-not-pow2` (committed bytes) |
| T3 the commit block wraps to one page | `vm-commit-block-past-size-max` (committed bytes) |
| T4 the in-place reallocate ignores the alignment | `vm-realloc-alignment` (alignment) |
| T5 the growable TLSF walks and grows for a non-power-of-two alignment | `growable-tlsf-refusal-grows-nothing` |
| T6 the streaming layer sends unservable requests to the policy | `stream-unservable-sheds-nothing`, `stream-non-pow2-alignment` |
| T7 the streaming layer keeps a block rounded past the budget | `stream-budget-charge` (budget) |
| T8 the policy is consulted for a block larger than the whole budget | the `[diag10a]` unit case |
| T9 the pressure loop allows 21 attempts | `stream-liar-bounded` (retry bound) |
| T10 purge keeps the pages it gives up | `vm-markers-purge-recommit` (recommitted bytes read zero) |

One more break on win-asan (run inside vcvars): T11, the arena no longer poisons freshly committed pages, fails
`vm-exhaust-exact` and `vm-markers-purge-recommit` on the ASan poison check. On win-asan the target was rebuilt from the
restored source and the corpus and the `[streaming]`/`[vmalloc]` cases passed again.

## libFuzzer

One single-process round of 600 s on `linux-clang-fuzz` (`scripts/fuzz.py run`, working corpus in a scratch
directory): 33,828 executions in 574 s (56 per second; each input reserves and releases address space twice), 1,633
new units (123 of the arena kind, 152 of the streaming kind among the 1,163 kept), no artifact. The units were not merged
into the committed corpus.

## Verification

- **win-debug:** the whole tree built (189 steps). The full CTest inside vcvars passed 7,549 of 7,549 (one test skipped
  by its own gate, as before). After the last harness edit (a tidy fix that changes no outcome) the target was rebuilt,
  the corpus replays 37 inputs, and the 8 `crd-no-*`/`crd-check*` guards and the corpus CTest pass.
- **win-shipping and win-clang-cl-shipping:** `crd-memory-tests`, `crd-fuzz-memory-allocators`, `crd-perf-tests` and
  `crd-resources-tests` built (clang-cl linked first time) and pass: memory 162 cases, perf 133, resources 128, the
  corpus 37 inputs; `--seed` writes the 31 seeds byte for byte.
- **win-asan (inside vcvars):** the same targets; memory 163, perf 266, resources 128, the corpus 37, seeds identical.
  The only sanitizer reports come from the existing specimens that fault on purpose (DIAG.3b poison and the perf crash
  specimens).
- **WSL, one lane at a time:** linux-gcc-debug and linux-gcc-asan (corpus replayed 3 times each) and linux-clang-tsan
  with `setarch -R` (10 times): memory 163, perf 264, resources 128, the corpus 37, seeds identical; no sanitizer report
  beyond the deliberate specimens on linux-gcc-asan. linux-clang-fuzz built both executables and replays the corpus.
- **Repository checks:** strict tidy is clean on the five changed C++ files (one nested conditional fixed);
  clang-format differs only in the repository's hand alignment, include grouping and one line it would join; the
  Allman check, test-repository-tools (65), check-master-plan and check-repository pass.

## Not done here

- The arena's and the streaming allocator's header comments still describe the old behaviour in two places (the
  in-place reallocate condition; `try_allocate`'s refusals); the headers were left alone because a change would rebuild
  most of the tree on every lane. The behaviour is documented in the sources and the design.
- The remaining DIAG.10a items: the unknown-origin CEIR and CKIR artifacts, the concurrent-load corruption and the
  hosted run.
