#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>

#include <atomic>

// MSVC C4324: structure padded due to alignment specifier. Intentional — m_head
// and m_tail sit on separate cache lines so producers and the consumer don't
// ping-pong a shared line (same suppression as crd::containers::ConcurrentQueue).
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::memory
{
class IAllocator;

// RingAllocator — thread-safe, epoch/fence-gated FIFO staging allocator (ADR-0085 S4).
//
// The async-load staging path: a producer (any worker thread) claims a byte region,
// writes streamed/decompressed data into it, and hands it to a consumer (a GPU
// transfer or another job). The region is recycled only once the consumer's work
// has RETIRED — gated by a monotonic u64 fence (a Vulkan timeline-semaphore value
// in the GPU case; crd-memory stays backend-agnostic — S6/S7 map u64 -> VkSemaphore).
//
// Model — N epochs in flight (the classic GPU upload-ring):
//   - try_claim() advances a monotonic head with a single lock-free CAS. No
//     per-claim bookkeeping — the hot path is one atomic, multi-producer safe.
//   - Claims belong to the CURRENT epoch (a fence value, initially 0).
//   - begin_epoch(fence) closes the current epoch (records the head as its end)
//     and opens a higher one. Up to `max_in_flight_epochs` may be unretired.
//   - retire(completed_fence) frees every epoch with fence <= completed_fence by
//     advancing the reclaim tail to that epoch's recorded end.
//
// THREAD-SAFETY (ADR-0085 D3): try_claim is safe from any number of producer
// threads concurrently. begin_epoch is producer-side (samples head). retire is
// consumer-side. The asymmetry is deliberate: producers stage, the consumer
// retires. Data VISIBILITY between producer writes and the consumer read is
// guaranteed by the EXTERNAL fence (timeline semaphore / job completion) the
// caller waits on — the ring's atomics guard space accounting, not the payload.
//
// PORTABLE: pure std::atomic, no OS primitives — Windows / POSIX / WASM alike.
// (WASM needs 64-bit atomics: Cerid's wasm32-wasi target with the atomics proposal
// provides them; bare wasm32 without atomics is not a supported target.)
//
// Backing buffer comes from `parent` (default: process allocator) committed once
// at construction; pass a VirtualMemoryAllocator* for a stable-address, malloc-free
// staging arena. This is NOT an IAllocator (no per-pointer deallocate; reclamation
// is epoch-collective), so it stands apart from the IAllocator family by design.
class RingAllocator final
{
public:
    // Compile-time ceiling on epochs in flight (keeps the mark ledger inline).
    static constexpr usize kMaxInFlightEpochs = 16;

    // capacity: staging byte budget (claims never exceed it). parent: backing-buffer
    //   source (nullptr -> default_allocator()). max_in_flight_epochs: K, a power of
    //   two in [2, kMaxInFlightEpochs] — how many un-retired epochs may coexist.
    explicit RingAllocator(usize capacity, IAllocator* parent = nullptr, usize max_in_flight_epochs = 4,
                           const char* name = "RingAllocator");
    ~RingAllocator();

    RingAllocator(const RingAllocator&)            = delete;
    RingAllocator& operator=(const RingAllocator&) = delete;

    // ---- Producer (any thread) -----------------------------------------
    // Reserve `size` bytes at >= `alignment`. Returns a pointer into the staging
    // buffer, or nullptr if the live span (head - tail) won't fit it (caller waits
    // for retire / uses a bigger ring). A span that would straddle the buffer end
    // wraps to offset 0, wasting the tail bytes (standard upload-ring trick).
    // `alignment` must be a power of two <= kCachelineSize (the buffer's alignment).
    [[nodiscard]] void* try_claim(usize size, usize alignment = kDefaultAlignment) noexcept;

    // Close the current epoch and open `fence` (must strictly increase). Records
    // the current head as the closing epoch's end. Fatal if more than
    // max_in_flight_epochs epochs are unretired (raise K or retire sooner).
    //
    // THREADING CONTRACT: begin_epoch is the PRODUCER-SIDE demarcation in the
    // natural pattern `begin_epoch(f) -> claims -> submit -> begin_epoch(f+1)`. It
    // is safe to run concurrently with try_claim, but the epoch boundary may then
    // "fuzz forward" — a claim whose CAS lands between begin_epoch sampling head and
    // writing the mark is retired one epoch later (never earlier), which is always
    // safe (the consumer waits on the higher fence). It expects the prior
    // occupant of its mark slot — epoch (fence - K) — to be retired already (the
    // matching retire() having completed); call begin_epoch after that retire.
    void begin_epoch(u64 fence) noexcept;

    // ---- Consumer ------------------------------------------------------
    // Free every epoch with fence <= completed_fence (advance the reclaim tail).
    void retire(u64 completed_fence) noexcept;

    // ---- Diagnostics ---------------------------------------------------
    [[nodiscard]] usize capacity() const noexcept { return m_capacity; }
    [[nodiscard]] usize in_use_bytes() const noexcept; // head - tail (snapshot; races with claim/retire)
    [[nodiscard]] u64   current_epoch() const noexcept { return m_latest_fence.load(std::memory_order_acquire); }
    [[nodiscard]] const char* name() const noexcept { return m_name; }

private:
    // Per-epoch boundary mark. `fence` == kEmptyFence means "slot never used".
    struct Mark
    {
        std::atomic<u64> fence{0};
        std::atomic<u64> end_head{0};
    };

    IAllocator* m_parent;
    const char* m_name;
    u8*         m_buffer      = nullptr;
    usize       m_capacity    = 0;
    usize       m_epoch_mask  = 0; // K - 1 (K power-of-two)

    // Hot counters on separate cache lines (producers bump head, consumer bumps
    // tail — keep them off each other's line).
    alignas(kCachelineSize) std::atomic<u64> m_head{0}; // monotonic bytes claimed
    alignas(kCachelineSize) std::atomic<u64> m_tail{0}; // monotonic bytes reclaimed
    std::atomic<u64> m_latest_fence{0};                 // the currently-open epoch
    Mark m_marks[kMaxInFlightEpochs];                   // boundary ledger, indexed by fence & m_epoch_mask
};
} // namespace crd::memory

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
