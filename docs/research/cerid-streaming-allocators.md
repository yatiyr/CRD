# Research — 2026-05-27 — Streaming allocators (virtual-memory substrate)

> **Outcome:** **adopted** — Phase 2.2 S1–S8 closed 2026-05-28 (ADR-0085); the allocator parity later re-shipped on gpu-context (RET-4). *(stamped 2026-08-07, doc-hygiene pass)*

> Long-form substrate dossier for **Phase 2.2 (ADR-0085)** — the virtual-memory +
> streaming allocator cluster that runs before hesap v5. Primary depth on **S2
> `VirtualMemoryAllocator`** (the next slice); spans S3 (re-parent TLSF), S4
> (`RingAllocator`), S6/S7 (GPU suballocation + defrag) because the design is
> architecturally coupled. Read this before implementing any S2+ slice.

## Question

What is the cutting-edge design for an open-world streaming memory substrate, and
specifically: what should Cerid's S2 `VirtualMemoryAllocator` (crd-memory) look
like so the whole cluster (S2→S7) is performant, elegant, and elite rather than a
naive reserve/commit wrapper? Triggered by ADR-0085: S1 (`crd::platform::vm`)
shipped the OS primitives; S2 is the first allocator built on them and becomes the
page-source `parent` that S3 re-parents `GrowableTlsfAllocator` onto.

## TL;DR

Every bullet maps to a concrete S2 decision (settled with the user 2026-05-27):

- **Shape = bump arena + region parent, NOT a general free-list allocator.** The
  entire AAA/systems corpus (Fleury, gingerBill, raddbg, UE pools) converges on:
  *reserve huge contiguous address space once, bump-commit forward, free in bulk
  via `reset()`/`pop()`*. `deallocate()` is a no-op; fine-grained free is delegated
  to a `TlsfAllocator` parented on top (S3). This is what "stable addresses, no
  relocating CPU heap, page-source parent" in ADR-0085 D1 actually means.
- **Decommit = manual `purge()` + decommit-on-`reset()`/`release()`, NOT eager and
  NOT decay-timed in S2.** jemalloc's history is the lesson: it *abandoned*
  ratio/eager purging for a two-phase time-decay precisely because eager decommit
  thrashes a syscall on every oscillation. But decay needs a clock + pressure
  context that lives in S5 (`StreamingAllocator`), not the raw arena. S2 stays
  deterministic and syscall-free on the hot path; auto-purge layers in at S5.
- **Reserve = configurable, 64 GiB default per arena.** Reservation is free address
  space on 64-bit (256 TiB available); only committed pages cost RAM. UE Binned3
  reserves ~1 GiB *per size-class pool*; an open-world arena wants generous headroom
  so it never re-reserves mid-stream (re-reserve would break the stable-address
  contract). Commit granularity = a tunable **commit block** (default 64 KiB =
  Windows allocation granularity) amortizing the commit syscall.

## The three settled forks (user-directed 2026-05-27)

| Fork | Decision | Why |
|---|---|---|
| **Allocator shape** | Bump arena + region parent | O(1) push, `deallocate`=no-op, `reset`/`pop` reclaim; TLSF (S3) handles arbitrary free on top. Matches Fleury/UE/raddbg; avoids reintroducing the fragmentation the stable-address design exists to kill. |
| **Decommit policy** | Manual `purge()` + decommit on `reset()`/`release()` | Deterministic, no hidden syscall in hot path. Decay-based auto-purge deferred to S5 where budget/pressure context exists (jemalloc's own evolution endorses this split). |
| **Reserve size** | Configurable, 64 GiB default | "Reserve huge, commit sparse" thesis; 64-bit makes it nearly free; never re-reserves → stable addresses hold. |

## Recommendation for Cerid

### S2 — `VirtualMemoryAllocator` (crd-memory)

A **stable-address bump arena over `crd::platform::vm`**, implementing `IAllocator`
so it drops into the existing ecosystem and can parent other allocators.

**State (single arena, single reservation):**
- `vm::VmRegion m_region` — one `vm::reserve(reserve_bytes)` at construction
  (default 64 GiB, ctor-overridable; reservation rounded to allocation granularity).
- `usize m_commit_pos` — high-water mark of *committed* bytes (page-aligned).
- `usize m_alloc_pos` — current bump offset (≤ `m_commit_pos`).
- `usize m_commit_block` — commit chunk size (default 64 KiB; ≥ `vm::page_size()`,
  rounded to a multiple of it). Amortizes the `commit()` syscall: a bump that
  crosses `m_commit_pos` commits the next `ceil` multiple of `m_commit_block`.
- `usize m_prev_alloc_pos` — offset of the most-recent allocation, for `reallocate`
  in-place grow (Fleury `prev_offset`).

**API surface (proposed):**
```cpp
class VirtualMemoryAllocator final : public IAllocator
{
public:
    using Marker = usize; // opaque bump offset returned by mark()

    struct Config
    {
        usize reserve_bytes        = usize{64} << 30; // 64 GiB address reservation
        usize commit_block         = usize{64} << 10; // commit granularity (>= page_size)
        usize initial_commit_bytes = 0;               // pre-warm [0, this) up front; 0 = lazy
    };
    explicit VirtualMemoryAllocator(const Config& cfg = {}, const char* name = "VirtualMemoryAllocator");
    ~VirtualMemoryAllocator() override; // release()s the reservation

    // IAllocator -------------------------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override; // bump + commit-on-demand
    void  deallocate(void* p) noexcept override;          // NO-OP (bump arena); asserts ownership in debug
    [[nodiscard]] bool owns(const void* p) const noexcept override; // p in [base, base+alloc_pos)
    // reallocate(): in-place grow iff p is the most-recent allocation (Fleury arena_resize); else bump-copy.
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override; // 0 (bump arenas don't track per-alloc)

    // Bulk lifetime ----------------------------------------------------------
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment); // nullptr on OOM/commit-fail
    Marker mark() const noexcept;            // capture alloc_pos (scratch/temp-arena pattern)
    void   reset_to(Marker m) noexcept;      // pop back to a marker (does NOT decommit)
    void   reset() noexcept;                 // alloc_pos = 0 (keeps pages committed for reuse)
    void   purge() noexcept;                 // decommit everything above alloc_pos -> RSS drops
    void   reset_and_purge() noexcept;       // reset() + purge() — hand RAM back to the OS

    // Diagnostics
    [[nodiscard]] usize committed_bytes() const noexcept { return m_commit_pos; }
    [[nodiscard]] usize used_bytes() const noexcept { return m_alloc_pos; }
    [[nodiscard]] usize reserved_bytes() const noexcept { return m_region.size; }
};
```

Key behaviors and why:
- **`allocate` = align `m_alloc_pos` → if the new top exceeds `m_commit_pos`,
  `vm::commit` the next `commit_block` multiple → bump.** O(1) amortized; a commit
  syscall only every ~`commit_block` bytes. Newly committed pages read as zero (S1
  guarantee) so callers needing zeroed memory pay nothing extra.
- **`deallocate` is a no-op** (bump arena). In debug it asserts `owns(p)` to catch
  cross-allocator frees. This is the deliberate contract, not a missing feature —
  the doc comment must say so loudly (mirror `LinearAllocator`'s existing comment).
- **`mark()`/`reset_to()` = the scratch/temp-arena pattern** (Fleury
  `ArenaTempBegin`/`End`, gingerBill `Temp_Arena_Memory`). This is how transient
  per-frame / per-task scratch is freed without decommitting — the dominant
  open-world usage. **Decommit (`purge`) is separate from pop (`reset_to`)** because
  popping is hot (every frame) and decommitting is a syscall you do rarely.
- **`reset()` keeps pages committed**; `purge()`/`reset_and_purge()` decommit. This
  is the explicit two-tier the user chose: cheap logical reset on the hot path,
  deliberate physical release when you actually want RSS back.
- **`reallocate` in-place grow** only when `p` is the most-recent allocation (Fleury
  `arena_resize`): lets `crd::containers::Array` grow without copy when it's the
  arena top. Otherwise bump-copy.

**Performance contract (the "elite" bar — make S2 falsifiable).** Targets the S2
test/bench must hold (i9-14900K, win-release):
- `allocate` hot path (no commit crossing): **≤ 20 ns**, branch-light, no syscall.
- commit syscall amortized to **1 per `commit_block`** (default 1 per 64 KiB bumped),
  never per allocation.
- `mark()` / `reset_to()` / `reset()`: **≤ 5 ns** (pointer arithmetic only; no syscall).
- `purge()`: O(1) syscall count (one `decommit` of the `[alloc_pos, commit_pos)` span),
  not per-page.
- zero heap allocation and zero locking on every path (single-threaded-per-arena).

**OOM contract (must decide at S2, affects S5).** `IAllocator::allocate` says OOM is
fatal, but `vm::commit` can *legitimately* fail (commit charge / physical RAM
exhausted) even though address space remains. Resolution: **`allocate` fatals
(`CRD_FATAL`) on commit failure** (honors the interface contract), and a parallel
**`try_allocate` returns `nullptr`** for the streaming/pressure path that must
handle failure gracefully. S5's residency logic calls `try_allocate` and
evicts-then-retries; engine code that "can't fail" calls `allocate`. This mirrors
the existing `GrowableTlsfAllocator::try_allocate` split exactly.

**Thread-safety (ADR-0085 D3): single-threaded per arena.** One arena per
thread/subsystem; no locking on the bump path (a bump arena's whole value is the
branch-free hot path). `RingAllocator` (S4) and `GpuAllocator` (S6) are the
thread-safe members of the cluster; the VM arena is not, by design.

### S3 — re-parent `GrowableTlsfAllocator` onto a `VirtualMemoryAllocator`

`GrowableTlsfAllocator` already takes an `IAllocator* parent`; its chunks come from
`parent->allocate(chunk_bytes)`. Point that parent at a `VirtualMemoryAllocator`
and **the malloc-at-the-root disappears** (ADR-0085 D5; keeps `crd-no-malloc-allocator`
green). Each TLSF chunk is then a committed slab of the giant VM reservation —
stable address, sparse physical residency. **Caveat:** the VM arena never reclaims a
freed TLSF chunk's address range (bump arena), so `GrowableTlsfAllocator` should
return chunks to the VM arena's *top* in LIFO order or simply let them ride until
the arena `reset_and_purge()`s. For the long-lived "process heap" role this is fine;
document that a VM-parented growable TLSF is a grow-mostly structure.

### S4 — `RingAllocator` (informed by S2's commit model + GPU fence-gating)

- Back the ring with a **VM reservation committed once up front** (rings are
  fixed-capacity FIFOs; no need for on-demand commit). Stable base, wrap by modulo.
- **Fence-gating = epoch tokens, and the modern primitive is a Vulkan timeline
  semaphore, not per-frame binary fences.** A region is reclaimable once the
  consuming job/transfer's monotonically-increasing timeline value has been signaled
  (`vkWaitSemaphores` / `vkGetSemaphoreCounterValue`). This is cleaner than the
  N-binary-fences-per-frame pattern and crosses cleanly into S6/S7 (the GPU upload
  path reads from this ring). S4 is CPU-side but its retire-epoch contract must be
  the same token type S6/S7 consume.
- Thread-safe: lock-free claim (atomic head CAS) or per-worker sub-rings. Per-worker
  sub-rings sidestep contention entirely and fit `crd::jobs::num_workers()` sizing —
  prefer them (consistent with the jobs lessons in MEMORY.md).

### S6/S7 — GPU suballocation + defrag (VMA is the reference, OffsetAllocator the kernel)

- **Suballocation kernel = OffsetAllocator-style** (sebbbi): 256 bins, 8-bit float
  bin sizing (3-bit mantissa + 5-bit exponent), two-level bitfield + 2× `LZCNT` →
  **O(1) allocate/free with ≤12.5% (avg 6.25%) internal fragmentation**, vs power-of-
  two's up to +100%. Metadata stored *separately* from the managed memory — exactly
  why it suits `VkDeviceMemory` sub-allocation (you can't store a free-list node
  inside device-local VRAM cheaply). This is the elite kernel for both GPU pools and
  any CPU sub-allocator that wants O(1) with tight fragmentation.
- **Defrag = handle-based indirection + transfer-queue copy, idle-gated**, modeled on
  VMA's `vmaBeginDefragmentation` → loop(`BeginPass` → app does `vkCmdCopyBuffer`/
  `vkCmdCopyImage` → `EndPass`) → end. Moves are reported as
  `{src, dstTmp}` pairs; the app performs the GPU copy and the allocator swaps the
  handle table. **Incremental**: cap `maxBytesPerPass`/`maxAllocationsPerPass` so a
  defrag pass fits a frame budget. Pluggable `IDefragPolicy` (ADR-0085 D4) chooses
  *which* allocations to move and the per-pass budget; the allocator is mechanism-only.
- **Residency** = evict device-local→host-visible under VRAM pressure, driven by an
  injected `IResidencyPolicy`. D3D12 exposes this as `MakeResident`/`Evict` +
  `IDXGIAdapter3::QueryVideoMemoryInfo` budget; VMA exposes `VmaBudget`. The policy
  reads the budget and picks victims; the allocator moves bytes.

## What we read

Sources actually fetched and read this pass (2026-05-27):

- [Ryan Fleury — *Untangling Lifetimes: The Arena Allocator*](https://www.rfleury.com/p/untangling-lifetimes-the-arena-allocator)
  — the canonical reserve-huge / commit-on-demand arena. `ArenaPush`/`ArenaPop`/
  `ArenaClear`, `ArenaTempBegin/End` scratch pattern, `arena_resize` in-place grow
  for the most-recent allocation, linked-block growth. 48-bit address space (256 TiB)
  framing; decommit on pos-retreat is *implicit* (he doesn't quantify a threshold) —
  which is exactly why Cerid makes `purge()` explicit.
- [gingerBill — *Memory Allocation Strategies: Linear/Arena Allocators*](https://www.gingerbill.org/article/2019/02/08/memory-allocation-strategies-002/)
  — `arena_alloc`/`arena_alloc_align`/`arena_resize`/`arena_free_all`; `prev_offset`
  for in-place resize; `align_forward` power-of-two alignment; default alignment
  `2*sizeof(void*)`. Confirms `arena_free` is a no-op for API completeness — Cerid's
  `deallocate` no-op has precedent.
- [UE5 *FMallocBinned3* docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FMallocBinned3)
  + [donaldwuid/unreal_source_explained — memory.md](https://github.com/donaldwuid/unreal_source_explained/blob/master/main/memory.md)
  + [ikrima ue4guide — allocators-malloc](https://ikrima.dev/ue4guide/engine-programming/memory/allocators-malloc/)
  — Binned3 reserves a **contiguous VM range (Pool) per size-class (Bin)**; default
  **1 GiB per Pool** (512 MiB if `USE_512MB_MAX_MEMORY_PER_BLOCK_SIZE`); commits/
  decommits in **Blocks (≥1 page)**; each Block packs N Bins to minimize tail waste
  (16 B bins → 256 per 4 KiB page, zero waste); Block residency tracked by a **bit
  tree**; large allocs go straight to the OS. UE uses `mmap`/`VirtualAlloc` over
  `malloc` specifically because *"free() on some platforms reduces RSS but not VSS"*
  — the same address-stability argument as ADR-0085 D1.
- [jemalloc #325 — *decay-based dirty page purging*](https://github.com/jemalloc/jemalloc/issues/325)
  + [Meta Engineering — *Scalable memory allocation using jemalloc*](https://engineering.fb.com/2011/01/03/core-infra/scalable-memory-allocation-using-jemalloc/)
  — jemalloc *replaced* ratio/eager purging with **two-phase time-decay**: pages go
  dirty → muzzy → clean; dirty→muzzy via `madvise(MADV_FREE)` after `dirty_decay_ms`,
  muzzy→clean (demand-zeroed) via `MADV_DONTNEED`. The lesson Cerid takes: **eager
  decommit thrashes; decay is better but needs a clock** → keep S2 manual, add decay
  at S5.
- [Sebastian Aaltonen — *OffsetAllocator*](https://github.com/sebbbi/OffsetAllocator)
  — 256 bins, 8-bit float distribution (3-bit mantissa + 5-bit exp), 2× `LZCNT`
  two-level bitfield → O(1) alloc/free, ≤12.5% (avg 6.25%) waste vs power-of-two's
  +100%. Metadata stored separately → suits GPU heap suballocation. The S6/S7 kernel.
- [AMD VMA — *Defragmentation*](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/defragmentation.html)
  — `VmaDefragmentationInfo{pool,flags}`; begin → loop(`BeginPass` returns
  `VmaDefragmentationMove{srcAllocation,dstTmpAllocation}` → app `vkCmdCopy*` + sync →
  `EndPass`) → end until not `VK_INCOMPLETE`. Per-move ops: default / `IGNORE` /
  `DESTROY`. Incremental via `maxBytesPerPass`/`maxAllocationsPerPass`. The S7
  defrag-loop blueprint.

Read as background (PDF/archive did not parse cleanly this pass — design from prior
knowledge, flagged as secondary):
- **mimalloc** (Leijen, Microsoft Research TR, 2019) — segment (≈4 MiB) of pages
  (≈64 KiB) model; **free-list sharding** (thread-local free + concurrent thread-free
  list) for low contention; **delayed/lazy decommit** with a reset option
  (`MEM_RESET`/`MADV_FREE`-style) rather than eager munmap. Lesson: lazy decommit +
  reuse-before-return is the throughput-friendly default — reinforces S2's
  reset()-keeps-committed choice.
- **Our Machinery — *Virtual Memory Tricks*** (Niklas Gray; site defunct, archive
  blocked) — reserve-then-commit growing arrays with stable pointers; **guard pages**
  (PROT_NONE pages bracketing live ranges) for overrun detection; poison-on-decommit.
  Lesson below in Pitfalls.

## Decommit policy — the single biggest S2 design choice (expanded)

The spectrum, with the cost each pays:

| Policy | When pages return to OS | Cost | Verdict for S2 |
|---|---|---|---|
| **Eager** (decommit on every shrink/free) | immediately | a syscall on every oscillation; TLB shootdowns; re-commit zero-faults on regrow | ❌ jemalloc abandoned this; thrashes an oscillating working set |
| **Manual** (`purge()` + decommit on reset/release) | only when the caller says so | one syscall per deliberate purge; zero hot-path cost | ✅ **S2 choice** — deterministic, simple, fast |
| **Decay** (timed dirty→muzzy→clean) | after `dirty_decay_ms` of disuse | needs a clock + background tick; best RSS/throughput balance | ⏭ **S5** — layer it where budget/pressure/time context lives |

**MEM_RESET vs MEM_DECOMMIT (Windows) / MADV_FREE vs MADV_DONTNEED (POSIX).** S1 only
exposes hard decommit (`VirtualFree(MEM_DECOMMIT)` / `madvise(MADV_DONTNEED)` →
next-touch zero-fill). There is a *softer* tier worth knowing but **not S2's job**:
- `MEM_RESET` (Win) / `MADV_FREE` (Linux/BSD) tells the OS "these pages are
  discardable; reclaim under pressure but don't fault me if I touch them first."
  Pages **stay committed** (RSS may not drop until pressure) — it's a hint, not a
  release. This is exactly jemalloc's *muzzy* tier and mimalloc's *reset*.
- **Decision:** keep S2 to hard commit/decommit (matches S1's surface). The soft
  reset tier belongs in **S5's decay/pressure logic**, where "reclaimable under
  pressure" has meaning. If we add it, extend `crd::platform::vm` with an explicit
  `reset(ptr,bytes)`/`MADV_FREE` primitive first (S1 amendment), don't smuggle it in.
- **MADV_FREE vs MADV_DONTNEED tradeoff to flag:** `DONTNEED` (what S1 uses) zeroes
  on next touch and drops RSS immediately — deterministic, the right S2 default.
  `FREE` is lazy (RSS lingers, can obscure stats) but cheaper. S2 wants the
  deterministic one; the decay layer (S5) is where lazy `FREE` earns its keep.

## Alternatives considered

- **General free-list region allocator at S2** (arbitrary `deallocate`): rejected.
  Reintroduces internal fragmentation that the stable-address VM design exists to
  eliminate, and duplicates TLSF's job. The clean composition is *bump arena (S2) →
  TLSF for fine-grained free (S3)*. (User-confirmed.)
- **Mode flag (Linear | FreeList) on one class**: rejected — two code paths to test,
  brushes against the "no dual paths" rule. One class, one job.
- **Buddy allocator** (ADR-0003 Phase B listed it): rejected as the S2 primary. Buddy
  is O(log n) with up to 2× internal fragmentation; OffsetAllocator's float-bins give
  O(1) at ≤12.5% — strictly better for the GPU sub-allocation role buddy was meant
  for. Keep buddy out; reach for OffsetAllocator-style bins at S6.
- **Relocating/compacting CPU heap** (move live objects to defragment): rejected by
  ADR-0085 D1. Stable addresses are the whole point on the CPU side; relocation
  breaks every raw pointer and fights determinism. Relocation is GPU-only (S7), where
  handles indirect every access anyway.
- **Eager decommit / decay-in-S2**: rejected per the table above (user-confirmed
  manual).

## Pitfalls / gotchas

- **`deallocate` no-op is a contract, not a bug.** Loudly document it (as
  `LinearAllocator` does). A consumer that frees individual allocations and expects
  RSS to drop is using the wrong allocator — it wants the TLSF parented on top (S3).
- **Re-reserve breaks stable addresses.** If an arena exhausts its 64 GiB it must
  *not* silently reserve a second region at a new base — that violates D1. Either
  size the reservation for the worst case (64 GiB default is generous) or `CRD_FATAL`
  with a clear "raise reserve_bytes" message. Never relocate.
- **Commit-charge OOM ≠ address-space OOM.** `vm::commit` can fail with address space
  to spare (Windows commit charge, Linux overcommit off). Hence the `allocate`-fatal
  / `try_allocate`-nullptr split. Test this explicitly (S2 test: reserve big, commit
  past a small `CRD_TEST` commit cap, assert `try_allocate`→nullptr, `allocate`→fatal).
- **Alignment vs commit granularity.** Bump alignment padding can push the top across
  a commit block mid-allocation — commit the *aligned* top, not the raw one. Off-by-a-
  page here is a latent fault that only fires when an allocation straddles a block.
- **ASan integration is mandatory for win-asan.** On a bump arena, poison the gap
  between `m_alloc_pos` and `m_commit_pos` so a read past the live top trips ASan:
  `__asan_poison_memory_region(base+alloc_pos, committed-alloc_pos)` after each
  bump/reset; `__asan_unpoison_memory_region` the freshly handed-out range in
  `allocate`. Without this, win-asan can't catch arena overruns (the bytes are
  legitimately committed). Gate behind `CRD_OS`/`__SANITIZE_ADDRESS__`.
- **Guard pages (debug-only, optional polish).** A `PROT_NONE` page (`vm::protect`)
  just past `m_commit_pos` turns an overrun into an immediate segfault even without
  ASan — UE/Our Machinery do this in debug. Cheap; consider as an S2 debug-build flag,
  not required for close.
- **Large pages: report, don't use (yet).** S1 exposes `large_page_size()`. Windows
  large pages need `SeLockMemoryPrivilege` (brutal to provision, often unavailable);
  Linux THP is unpredictable. S2 should **not** request large pages by default — TLB
  win is real for huge resident sets but the provisioning cost and failure modes
  aren't worth it without a measured consumer. Leave a `Config` door, default off.
- **`reset()` does not zero.** Re-handed memory after `reset()` (without `purge()`)
  retains old bytes — only freshly-*committed* pages are zero. A caller relying on
  zero-init after reset is a bug; document that `reset()` is logical, not a memset.
- **ASLR vs "stable" addresses.** Addresses are stable *within a process run*, not
  across runs (ASLR randomizes the reservation base). ADR-0085 D2 already says
  addresses are process-private and never part of sim/replay state — consistent, but
  worth restating so nobody serializes a VM pointer.
- **Determinism (ADR-0063).** Commit/decommit *timing* is non-deterministic; resident
  *bytes* are bit-identical (S1 zero-on-commit guarantees this). Keep streaming/purge
  strictly out of the fixed-step sim path.

## Open questions

- **S4 epoch token type.** Should the CPU `RingAllocator`'s retire-epoch be a plain
  `u64` monotonic counter that S6/S7 map onto a Vulkan timeline semaphore value, or
  should the token be RHI-defined from the start? Leaning `u64` in crd-memory with the
  RHI mapping at S6 (keeps crd-memory free of an rhi dependency). Resolve at S4.
- **GrowableTlsf chunk return to a VM arena.** A bump-arena VM parent can't reclaim a
  non-top freed chunk's address range. Is grow-mostly acceptable for the process-heap
  role, or does S3 need a small free-list of fixed-size chunk slots in the VM arena?
  Measure at S3; likely grow-mostly is fine for long-lived heaps.
- **Per-arena vs shared reservation.** Many subsystems × 64 GiB reservations = large
  VAD/page-table count (cheap but not zero). Is there a case for one big shared
  reservation sub-divided, or is per-arena isolation worth it? Default per-arena;
  revisit if VAD count shows up in profiling.
- **Paired thread-local scratch convention.** The standard Fleury pattern is *two*
  thread-local scratch arenas per worker, so a callee's scratch can't collide with
  its caller's (`scratch_begin(conflicts...)`). Cerid has `crd::jobs::num_workers()`;
  a `crd::memory::scratch_arena()` convention layered on S2 would be elite ergonomics.
  **Out of scope for S2** (no consumer yet) — note it, build it when a consumer asks;
  do not widen S2 for it now.
- **mimalloc TR re-read.** The PDF didn't parse this pass; if S5's decay policy needs
  the exact segment/page decommit-delay numbers, re-fetch as HTML or read the source
  `options.c` (`decommit_delay`).

## Used by

- Phase 2.2 **S2 (`VirtualMemoryAllocator`) — SHIPPED 2026-05-27** to this design
  (bump arena + region parent · manual purge · 64 GiB reserve · ASan poison · vm
  relocated to the `crd-vm` leaf). Local green on win-debug/asan/tidy/clang-cl.
- Phase 2.2 **S3 — SHIPPED 2026-05-27**: `GrowableTlsfAllocator` re-parented onto
  `VirtualMemoryAllocator` (malloc-free heap). Solved the try-contract gap in-slice
  (no debt): `try_allocate` promoted to a `virtual` on `IAllocator`; `grow()` uses
  `parent->try_allocate` + non-owning TLSF. Local green on win-debug/asan/tidy/clang-cl.
- Phase 2.2 **S4 — SHIPPED 2026-05-27**: `RingAllocator` epoch/fence-gated staging
  (N-epochs-in-flight; lock-free `try_claim`; `u64` timeline-fence token; VM-backed;
  pure `std::atomic`). Lib jobs-free; concurrency test via `crd-jobs parallel_for`.
  Local green on win-debug/asan/tidy/clang-cl.
- Phase 2.2 **S5 — SHIPPED 2026-05-27**: `StreamingAllocator` policy layer
  (composes VM resident + GrowableTlsf per-payload-free store + Ring staging;
  per-`CategoryId` budgets; injected `IResidencyPolicy` + null default; pressure
  protocol via the `try_allocate` chain). Standalone in crd-memory; ResourceManager
  wiring deferred to Phase 2.7's first streaming consumer. Local green on
  win-debug/asan/tidy/clang-cl.
- Phase 2.2 **S6 — SHIPPED 2026-05-27**: `crd::memory::OffsetAllocator` (O(1)
  external-metadata kernel — VRAM-capable) + Vulkan `GpuAllocator` (per-(type,linear)
  block pools, dedicated ≥16 MiB, persistent map, nonCoherentAtom-aligned,
  granularity-safe; replaces the v1e per-resource allocator). 4 ValidationCapture GPU
  tests + 769K-assertion CPU stress. Local green on win-debug/asan/tidy/clang-cl.
- Phase 2.2 **S7 — SHIPPED 2026-05-27** (full build): GPU defrag (buffers + images,
  idle-gated recreate+copy+swap, `on_relocated` callback) + residency (device↔host
  relocation + auto-pressure eviction loop, S5-pattern) + pluggable
  `IDefragPolicy`/`IResidencyPolicy`/`IGpuResidencyContext`. 5 ValidationCapture GPU
  tests; full rhi-vulkan suite + smoke green on win-debug/asan/tidy/clang-cl.
- Phase 2.2 **S8 — cluster close** (ADR lock, systems docs, 18-config sweep) next.
