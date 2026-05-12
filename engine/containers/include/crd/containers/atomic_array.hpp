#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>
#include <new>
#include <type_traits>
#include <utility>

// MSVC C4324: structure padded due to alignment specifier — that's the whole
// point of CacheLinePadded.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::containers
{

/// Cache-line padded wrapper — give each element its own line so concurrent
/// read-modify-writes on adjacent elements don't false-share. Costs a full
/// cache line per element; only worth it when the contention is real (a serial
/// reduction or jobs::parallel_reduce is the cheaper alternative when you can
/// use it).
///
/// For an array of atomic counters that many fibers/threads fetch_add, the
/// pattern is `Array<CacheLinePadded<u32>>` plus `std::atomic_ref<u32>` for the
/// ops:
///
///     Array<CacheLinePadded<u32>> hits(num_buckets, &alloc);
///     hits.resize(num_buckets);                                  // zero-init
///     // ... in parallel:
///     std::atomic_ref<u32>(hits[b].value).fetch_add(1, std::memory_order_relaxed);
///     // ... after the join (quiescent): read hits[i].value directly.
///
/// Note: do NOT wrap `std::atomic<T>` (i.e. `CacheLinePadded<std::atomic<T>>`)
/// and put it in `Array` — `std::atomic` is not movable, and `Array` relocates
/// its buffer on growth, so that won't compile. Use the plain-T-plus-atomic_ref
/// shape above, or hand-manage a non-relocating block of `std::atomic<T>`.
template <typename T> struct alignas(64) CacheLinePadded
{
    T value{};

    CacheLinePadded() = default;
    explicit CacheLinePadded(const T& v) : value(v) {}
};

// -----------------------------------------------------------------------
// AtomicArray<T> — bounded, lock-free *append-only* vector (detour D-002 v4).
//
// Fixed capacity reserved at construction. A producer claims the next slot with
// a single `m_head.fetch_add(1)` and constructs its element there. Slots are
// never recycled and never moved — so the storage pointer and every element's
// address are stable for the life of the container, and a constructed slot is
// safe to read once its producer's push() has returned.
//
// The canonical use is "collect up to N results from a parallel pass": size N
// up front (you know it), have each fiber push() its result, then read the
// array back on the calling thread after the pass. Overflow (more pushes than
// capacity) is a sizing bug — push() returns `npos` and asserts in debug; the
// extra elements are simply not stored.
//
// Thread-safety:
//   - push / emplace are safe to call concurrently from any number of threads.
//   - Reading (operator[], data(), iterators, size()) is safe only when no
//     push is in flight — typically after the parallel pass has joined. During
//     the pass, size() over-counts by the number of in-flight (claimed but not
//     yet constructed) slots.
//   - clear() / reset_for_reuse() are NOT thread-safe vs push — call them only
//     between passes (a quiescent point), like Array::clear().
//
// Move/copy deleted (live concurrent structure).
// -----------------------------------------------------------------------
template <typename T> class AtomicArray
{
public:
    static constexpr usize npos = static_cast<usize>(-1);

    explicit AtomicArray(usize capacity, memory::IAllocator* alloc = memory::default_allocator())
        : m_alloc(alloc), m_capacity(capacity)
    {
        CRD_ASSERT(m_alloc != nullptr);
        CRD_ASSERT_MSG(capacity > 0U, "AtomicArray: capacity must be > 0");
        m_data = static_cast<T*>(m_alloc->allocate(sizeof(T) * m_capacity, alignof(T)));
    }

    ~AtomicArray()
    {
        destroy_constructed();
        if (m_data != nullptr && m_alloc != nullptr)
        {
            m_alloc->deallocate(m_data);
        }
        m_data = nullptr;
    }

    AtomicArray(const AtomicArray&) = delete;
    AtomicArray& operator=(const AtomicArray&) = delete;
    AtomicArray(AtomicArray&&) = delete;
    AtomicArray& operator=(AtomicArray&&) = delete;

    // ---- Append (any number of threads) --------------------------------

    /// Claim the next slot, construct `v` there, return its index — or `npos`
    /// if the array is full (a sizing bug; asserted in debug).
    [[nodiscard]] usize push(const T& v) { return emplace(v); }
    [[nodiscard]] usize push(T&& v) { return emplace(std::move(v)); }

    template <typename... Args> [[nodiscard]] usize emplace(Args&&... args)
    {
        const usize idx = m_head.fetch_add(1U, std::memory_order_relaxed);
        if (idx >= m_capacity)
        {
            CRD_ASSERT_MSG(false, "AtomicArray overflow — capacity exceeded; size the array correctly");
            return npos;
        }
        ::new (static_cast<void*>(m_data + idx)) T(std::forward<Args>(args)...);
        // No publish fence here: readers are expected to read after the parallel
        // pass has joined, and the join (e.g. jobs::wait) provides the
        // happens-before. size() reflects *claimed* slots, not *constructed*
        // ones — during a live pass an in-flight slot may not be readable yet.
        return idx;
    }

    // ---- Read back (after the parallel pass has joined) ----------------

    /// Number of slots claimed, clamped to capacity. Exact only when no push is
    /// in flight; over-counts in-flight slots during a concurrent pass.
    [[nodiscard]] usize size() const noexcept
    {
        const usize h = m_head.load(std::memory_order_acquire);
        return h < m_capacity ? h : m_capacity;
    }
    [[nodiscard]] usize capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0U; }
    [[nodiscard]] bool full() const noexcept { return m_head.load(std::memory_order_acquire) >= m_capacity; }

    [[nodiscard]] T& operator[](usize i) noexcept
    {
        CRD_ASSERT(i < size());
        return m_data[i];
    }
    [[nodiscard]] const T& operator[](usize i) const noexcept
    {
        CRD_ASSERT(i < size());
        return m_data[i];
    }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] T* begin() noexcept { return m_data; }
    [[nodiscard]] T* end() noexcept { return m_data + size(); }
    [[nodiscard]] const T* begin() const noexcept { return m_data; }
    [[nodiscard]] const T* end() const noexcept { return m_data + size(); }

    [[nodiscard]] memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // ---- Between-pass reuse (NOT thread-safe vs push) -----------------

    /// Destroy all elements and reset the head to 0 so the array can collect a
    /// fresh pass. Capacity is unchanged. Call only at a quiescent point.
    void clear() noexcept
    {
        destroy_constructed();
        m_head.store(0U, std::memory_order_relaxed);
    }

private:
    void destroy_constructed() noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            if (m_data == nullptr)
            {
                return;
            }
            const usize n = size();
            for (usize i = 0; i < n; ++i)
            {
                m_data[i].~T();
            }
        }
    }

    memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    usize m_capacity = 0;
    alignas(64) std::atomic<usize> m_head{0};
};

} // namespace crd::containers

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
