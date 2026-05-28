# ADR-0085 — Virtual-memory + streaming allocator cluster

**Date:** 2026-05-27
**Status:** Accepted
**Tags:** [memory] [platform] [rhi] [streaming] [resources]

## Context

Cerid targets open-world games + large-data domains (scene streaming, high-res
texture sets, big simulation/scientific data). The current allocator set —
`MallocAllocator`, `TlsfAllocator` (≤4 GB pool), `GrowableTlsfAllocator`
(unbounded chain of TLSF chunks, added 2026-05-27), `PoolAllocator`,
`GrowablePoolAllocator`, `LinearAllocator`, `StackAllocator` — covers bounded
and growable general allocation but **lacks the three things open-world
streaming needs**: (1) a virtual-memory reservation/commit allocator (huge
stable-address space, sparse physical residency), (2) a thread-safe fence-gated
staging/ring allocator for the async load path, and (3) a GPU device-memory
suballocator with residency + defragmentation.

These were planned, not built: **ADR-0003 (memory-v1) Phase B** explicitly lists
`BuddyAllocator, RingAllocator, StreamingAllocator, GPUAllocator`; **ADR-0022
(streaming-pipeline)** states streaming is a *pipeline* (allocator + jobs +
async I/O + resource-manager/streamer) and that the streaming allocator alone is
"wasted work" without pieces 2–4. Those prerequisites are now shipped:
`crd-jobs` ✅, `crd-platform` async filesystem I/O (ADR-0041) ✅, `crd-resources`
ResourceManager + CRDR + 2Q-LRU eviction ✅.

**Scope decision (2026-05-27, user-directed, eyes-open).** The assistant
recommended building the CPU substrate now and **deferring the GPU
suballocator/defrag** until a renderer-streaming consumer exists (per
PRINCIPLES.md "real workload before optimization" + `feedback_ship_at_consumer_
template_from_day_one`: GPU residency/eviction/staging policy differs sharply
between Clustered-Forward+, deferred, and compute-heavy hesap-GPU workloads, so
designing it consumer-less risks rework). The user reviewed that conflict
explicitly and chose to build the **full cluster including GPU now**, accepting
the design-without-consumer risk. This ADR therefore includes the GPU allocator,
but **mitigates the risk by (a) building it against the real `crd-rhi` /
`crd-rhi-vulkan` device-memory surface and (b) making residency + defrag POLICY
pluggable** (`IResidencyPolicy` / `IDefragPolicy`) so a future render path tunes
policy without rewriting the mechanism. Sequencing: this sub-phase runs **before
hesap v5** (also user-directed); v5's RESUME-HERE pointer is deferred accordingly.

## Decision

A six-allocator cluster + a platform virtual-memory layer, composing with the
existing streaming pipeline (ADR-0022) rather than replacing it.

### 1. `crd-platform` virtual-memory API (`crd::platform::vm`)
Backend-neutral reserve/commit primitives:
`reserve(bytes) -> VmRegion` · `commit(ptr, bytes)` · `decommit(ptr, bytes)` ·
`release(VmRegion)` · `protect(ptr, bytes, Access)` · `page_size()` ·
`large_page_size()`. Windows = `VirtualAlloc(MEM_RESERVE/MEM_COMMIT)` /
`VirtualFree(MEM_DECOMMIT/MEM_RELEASE)` / `VirtualProtect`; POSIX =
`mmap(PROT_NONE)` + `mprotect` (commit) + `madvise(MADV_DONTNEED)` (decommit) +
`munmap` (release). Vendor types do not leak (ADR principle).

> **Amendment (2026-05-27, S2):** the vm primitives were **relocated from
> `crd-platform` to a new `crd-vm` leaf module** (`crd::vm`, depends on
> `crd-core` + `crd-log`). Reason: S2's `VirtualMemoryAllocator` lives in
> `crd-memory`, and `crd-platform` already PUBLICly links `crd-memory`, so the
> allocator's page source had to sit *below* `crd-memory` to avoid a dependency
> cycle. A new leaf (chosen over folding into `crd-core`) keeps `crd-core` a pure
> compile-time-only root. Both `crd-memory` and `crd-platform` may depend on
> `crd-vm`. S1's tests moved to `tests/vm/`; `smoke_virtual_memory` re-links
> `crd-vm`. Research: `docs/research/cerid-streaming-allocators.md`.

### 2. `VirtualMemoryAllocator` (crd-memory) — the page/region allocator
Reserves one large address range (tens of GB on 64-bit; reservation is free,
physical commit is the budget); commits pages on first touch / on `allocate`,
decommits on `release`/`reset`. **Stable addresses**: streaming chunks in/out by
commit/decommit *without invalidating any pointer* — fragmentation collapses to
OS page granularity. This is the primary CPU open-world strategy; **no relocating
CPU heap**. Doubles as a page-source `parent` for other allocators.

### 3. `GrowableTlsfAllocator` re-parented onto `VirtualMemoryAllocator`
Optional `parent` already exists; pointing it at a VM allocator makes its chunks
come from `reserve/commit` instead of malloc — **removes malloc-at-the-root**
(the goal of the 2026-05-27 sweep). `default_allocator()` (malloc) stays only as
the ultimate bootstrap fallback.

> **Amendment (2026-05-27, S3):** to make the composition honor the non-throwing
> contract end-to-end, **`try_allocate` was promoted to a `virtual` on
> `IAllocator`** (appended at the END of the vtable per the vtable-stability rule;
> default impl delegates to `allocate`, which already-non-fatal bump/stack/pool
> allocators inherit; `MallocAllocator`/`TlsfAllocator`/`VirtualMemoryAllocator`/
> `GrowableTlsfAllocator` override with the real nullptr path). `GrowableTlsfAllocator`
> now pulls each chunk's pool + node via `parent->try_allocate` and holds its TLSF
> **non-owning** (the growable allocator owns/frees the pool buffer), so exhausting a
> `VirtualMemoryAllocator` parent yields `nullptr` rather than a `CRD_FATAL`. No new
> debt — the limitation was solved in-slice.

### 4. `RingAllocator` (crd-memory) — thread-safe, fence-gated staging
FIFO ring for the async load path (read compressed CRDR chunk → stage →
decompress → hand off → recycle). Reclamation is **epoch/fence-gated**: a region
is reusable only once the job/GPU transfer reading it has retired its epoch.
Explicitly thread-safe (multi-worker producers) — lock-free claim or per-worker
sub-rings.

> **Amendment (2026-05-27, S4):** shipped as the **N-epochs-in-flight** model (the
> GPU upload-ring), chosen over per-claim fences (a batch retires collectively, so
> per-claim fences add cost with no return) and over per-worker sub-rings (which
> would couple `crd-memory` to `crd-jobs` for `worker_index()`). `try_claim` is a
> single lock-free atomic head CAS (multi-producer); `begin_epoch(u64 fence)` closes
> the open epoch (records head), `retire(u64 completed_fence)` advances the reclaim
> tail; a fixed K-slot mark ledger (no per-claim ledger write). Token = `u64`
> timeline-fence (rhi-free; S6/S7 map to `VkSemaphore`). Backing buffer from an
> `IAllocator* parent` (VM-backed for stable malloc-free staging). Pure `std::atomic`
> — no OS primitives, WASM-friendly. The **library stays free of any `crd-jobs`
> dependency**; the concurrency *test* drives producers through `crd-jobs`
> `parallel_for` (the engine's fiber job system, not `std::thread`).

### 5. `StreamingAllocator` — the policy layer
Composes `RingAllocator` (transient staging) + `VirtualMemoryAllocator` (resident
store) + the existing `ResourceManager` (residency/eviction/2Q-LRU/budgets).
Per-category byte budgets; commit/decommit (CPU) + evict (GPU) under pressure.
Mechanism here; residency policy injected.

> **Amendment (2026-05-27, S5):** shipped as a **standalone `crd-memory` class with
> NO `crd-resources` dependency**. Resident store = a `GrowableTlsfAllocator` over a
> `VirtualMemoryAllocator` (real O(1) per-payload free + stable addresses — a
> resource streams in/out individually; user-chosen over a bump+collective-purge
> store). Staging = `RingAllocator`. Per-`CategoryId` (`u32`, caller-assigned) soft
> budgets; injected **`IResidencyPolicy`** + a `NullResidencyPolicy` default. Pressure
> protocol: over-budget/heap-full → `policy.evict()` in a loop **called outside the
> resident mutex** (the policy calls back `release_resident`, which re-locks — holding
> would deadlock) → retry → graceful `nullptr`. Resident path mutex-guarded (load
> path, not per-frame hot path); staging lock-free. **The `ResourceManager`
> integration (routing loader payloads through the resident store + driving the policy
> from the 2Q-LRU) is deferred to the first real streaming consumer (Phase 2.7
> texture/mesh), where the integration shape is known — this is a consumer-pulled
> path, NOT debt.** Real residency-policy implementations arrive with that consumer.

### 6. `GpuAllocator` (crd-rhi + crd-rhi-vulkan) — device-memory suballocation
Suballocate `VkDeviceMemory` (large blocks carved into sub-blocks; dedicated
allocations above a threshold; separate device-local vs host-visible pools).
**Defrag** = handle-based access so device pools compact by transfer-queue copy +
handle-table update, gated on GPU/transfer idle. **Residency** = evict
device-local→host-visible under VRAM pressure. **Both policies pluggable**
(`IResidencyPolicy`, `IDefragPolicy`); the allocator provides mechanism only.
Tested via `crd::rhi::ValidationCapture` (0 validation errors) + determinism of
the resident bytes.

> **Amendment (2026-05-27, S6 — suballocation shipped; defrag/residency = S7):** the
> suballocation kernel is **`crd::memory::OffsetAllocator`** (O(1), external metadata
> — the property that lets it manage device-local VRAM, which TLSF can't), a
> Cerid-idiom port of Aaltonen's algorithm, fully CPU-unit-tested (769K-assertion
> no-overlap stress). The Vulkan `GpuAllocator` (in `crd-rhi-vulkan`, replacing the
> v1e one-`vkAllocateMemory`-per-resource helper) carves `min(256 MiB, heap/8)`
> blocks per **`(memoryTypeIndex, linear)`** pool — the linear/non-linear split
> sidesteps `bufferImageGranularity` cleanly; **dedicated allocations ≥16 MiB**;
> host-visible blocks persistently mapped; **non-coherent host memory rounds
> suballocation alignment up to `nonCoherentAtomSize`**. `create_buffer`/`create_image`
> route through it; swapchain non-owning images stay on the no-allocation path; the
> allocator frees all blocks **before `vkDestroyDevice`** (member-dtor-order fix). No
> Vulkan types leak into `crd-rhi`; OffsetAllocator lives in `crd-memory`.
> **S6 = suballocation only.** Handle-based **defrag** (transfer-queue relocation) +
> **residency** (device↔host eviction) + the pluggable `IResidencyPolicy`/
> `IDefragPolicy` are **S7** — the `VulkanAllocation` handle is already indirection-
> friendly (`block_index` + `suballoc`) so S7 can relocate without an API break.

> **Amendment (2026-05-27, S7 — defrag + residency shipped, FULL build per user).** The
> pluggable seams (`IDefragPolicy`/`IResidencyPolicy`/`IGpuResidencyContext`) live in
> backend-neutral `crd/rhi/gpu_residency.hpp` (null defaults). **Defrag** is idle-gated
> (one `vkDeviceWaitIdle` + one submit per pass): a live-resource registry feeds
> recreate-at-compacted-location + transfer-copy + internal-handle swap + generation
> bump + `on_relocated` callback (fired OUTSIDE the allocator mutex, hot-reload style).
> **Buffers** rely on the re-fetch-handle-per-frame contract; **images** do
> subresource-correct `vkCmdCopyImage` over all mips/layers with layout transitions +
> view recreation, restored to the original layout (consumers see no layout change).
> **Residency** (user chose the full build over interface-only): a device↔host
> relocation primitive + an **auto-pressure eviction loop in `sub_allocate`** that
> mirrors the S5 StreamingAllocator (soft device-local budget → injected policy evicts
> device-local resources to host → retry, cap 16) + `make_resident` re-promote. The
> allocator implements `IGpuResidencyContext`. Accepted-risk note (this ADR's
> Consequences): the image-defrag descriptor-invalidation contract + the residency
> trigger/victim policy are designed ahead of a renderer-streaming consumer; the
> `on_relocated`/generation seam + the injected policies confine the expected churn.
> 5 ValidationCapture GPU tests; full rhi-vulkan suite + bootstrap smoke green.

### Pinned decisions (D(mem-stream)-1..7)
- **D1 — stable-address VM-reserve is the CPU strategy.** No relocating CPU heap;
  commit/decommit at page granularity; addresses never move.
- **D2 — determinism (ADR-0063).** Streaming/residency is strictly OUT of the
  deterministic sim path. Timing is async/non-deterministic; resident *data* is
  bit-identical. Addresses are process-private, never part of sim/replay state.
- **D3 — thread-safety is per-allocator + explicit.** `RingAllocator` +
  `GpuAllocator` are thread-safe (jobs/transfer threads). `VirtualMemoryAllocator`
  + TLSF stay single-threaded per arena (one arena per thread/subsystem).
- **D4 — GPU policy is injected, not baked.** `IResidencyPolicy`/`IDefragPolicy`
  so the eventual render path (Clustered-Forward+ first, ADR-0016) chooses
  eviction granularity/order/staging; the allocator is render-path-agnostic.
- **D5 — malloc removed at the root.** VM allocator is the page source;
  `default_allocator()` (malloc) is bootstrap-only. The `crd-no-malloc-allocator`
  guard (2026-05-27) continues to hold.
- **D6 — no owning STL / always crd types / units at API surface** as everywhere.
- **D7 — pipeline reuse, not replacement.** Budgets/eviction extend the shipped
  `ResourceManager`; async reads use the shipped `crd-platform` async I/O; jobs
  use `crd-jobs`. This ADR adds the *allocators* the pipeline was missing.

## Slice plan (sub-phase before hesap v5)
- **S1** — `crd-platform::vm` reserve/commit/decommit/protect/page_size + tests (Win + WSL).
- **S2** — `VirtualMemoryAllocator` + tests (large reserve, sparse commit, decommit residency, stable-address invariant, OOM).
- **S3** — re-parent `GrowableTlsfAllocator` onto VM (parent = VM) + tests (malloc-free root path); confirm `crd-no-malloc-allocator` still green.
- **S4** — `RingAllocator` (thread-safe, fence/epoch-gated) + tests (wraparound, fence reclamation, concurrent producers, determinism of contents).
- **S5** — `StreamingAllocator` policy layer + ResourceManager budget/eviction integration + synthetic stream test (load→stage→commit→evict under budget).
- **S6** — `GpuAllocator`: `VkDeviceMemory` suballocation (device-local/host-visible pools, dedicated allocs) on `crd-rhi`/`crd-rhi-vulkan` + ValidationCapture tests.
- **S7** — GPU defrag (handle-based relocation via transfer queue, idle-gated) + residency (device↔host eviction) with pluggable `IResidencyPolicy`/`IDefragPolicy` + tests.
- **S8** — close: ADR lock, docs (`docs/systems/`), 18-config sweep, cluster no-leak + determinism gates.

## Consequences
- Open-world-scale CPU residency with stable addresses; malloc removed at the
  allocator root; a thread-safe staging path for streaming; GPU suballocation +
  defrag + residency. Multi-week effort.
- **Accepted risk:** the GPU residency/defrag policy is designed ahead of a real
  renderer-streaming consumer; `IResidencyPolicy`/`IDefragPolicy` confine the
  expected churn to policy objects, but the mechanism may still need revision when
  the consumer lands. Recorded per the user's explicit eyes-open override.

## Closure (S8, 2026-05-27)

Phase 2.2 (S1–S7) shipped: `crd::vm` (S1) · `VirtualMemoryAllocator` (S2) ·
GrowableTlsf-on-VM malloc-free heap (S3) · `RingAllocator` (S4) ·
`StreamingAllocator` (S5) · `OffsetAllocator` + Vulkan `GpuAllocator` suballocation
(S6) · GPU defrag + residency with pluggable policies (S7). Pinned decisions
**D(mem-stream)-1..7 are LOCKED** as written above. Open-world-scale CPU residency
with stable addresses, malloc removed at the allocator root, thread-safe fence-gated
staging, and GPU suballocation + defrag + residency are all in. The accepted
design-ahead-of-consumer risk (S5 residency policy, S7 image-defrag descriptor
contract + residency victim policy) is confined to the injected
`IResidencyPolicy`/`IDefragPolicy` objects; the mechanism is shipped + tested. The
ResourceManager integration (routing loader payloads through the resident store +
driving the policies from the 2Q-LRU) lands with the first real streaming consumer
(Phase 2.7 texture/mesh).

## References
- ADR-0003 (memory-v1, Phase B roadmap) · ADR-0022 (streaming pipeline) ·
  ADR-0041 (platform async filesystem I/O) · ADR-0036 (resources module) ·
  ADR-0016 (Clustered-Forward+ render path) · ADR-0063 (determinism contract).
- `docs/phases/phase-2.2-streaming-allocators.md` (slice ledger).
- Memory: `project_no_malloc_sweep_before_v5` (the GrowableTlsfAllocator + guard this builds on).
