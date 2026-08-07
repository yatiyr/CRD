// GrowableLinearAllocator — gold-standard gate. Boundary adversaries (SANITY #3): chunk growth, oversized alloc
// gets its own chunk, exact fill-to-tail, alignment, reset()-reuse, no-per-alloc-malloc-within-a-chunk, deallocate
// no-op, reallocate-copies, and destructor-frees-every-chunk.

#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
// Counts BOTH parent allocations and deallocations (a pass-through wrapper) so we can prove per-op arena behaviour
// and full cleanup at destruction.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* parent) noexcept : m_parent(parent) { m_name = "test-counting"; }
    void* allocate(crd::usize size, crd::usize align) override { ++m_allocs; return m_parent->allocate(size, align); }
    void  deallocate(void* p) noexcept override
    {
        if (p != nullptr) { ++m_deallocs; }
        m_parent->deallocate(p);
    }
    bool  owns(const void* p) const noexcept override { return m_parent->owns(p); }
    [[nodiscard]] crd::u64 allocs() const noexcept { return m_allocs; }
    [[nodiscard]] crd::u64 deallocs() const noexcept { return m_deallocs; }

private:
    crd::memory::IAllocator* m_parent;
    crd::u64                 m_allocs   = 0;
    crd::u64                 m_deallocs = 0;
};
} // namespace

using crd::memory::GrowableLinearAllocator;

TEST_CASE("GrowableLinearAllocator: basic allocate, owns, alignment", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    GrowableLinearAllocator      a(1024, &root);

    void* p = a.allocate(16, 8);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 8U == 0U);
    REQUIRE(a.owns(p));

    int stack = 0;
    REQUIRE_FALSE(a.owns(&stack));

    void* q = a.allocate(1, 64);
    REQUIRE(reinterpret_cast<std::uintptr_t>(q) % 64U == 0U);
}

TEST_CASE("GrowableLinearAllocator: grows across chunks; each new chunk is exactly one parent alloc", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    CountingAllocator            counting(&root);
    GrowableLinearAllocator      a(1024, &counting); // ctor reserves the first chunk

    const crd::u64 after_ctor = counting.allocs();
    REQUIRE(after_ctor == 1U);      // exactly the first chunk
    REQUIRE(a.num_chunks() == 1U);

    // Fill within the first chunk: NO new parent allocation.
    for (int i = 0; i < 8; ++i) { (void)a.allocate(64, 8); } // ~512 B of a ~1000 B chunk
    REQUIRE(counting.allocs() == after_ctor);
    REQUIRE(a.num_chunks() == 1U);

    // Overflow the first chunk → exactly one more parent alloc, one more chunk.
    for (int i = 0; i < 16; ++i) { (void)a.allocate(64, 8); }
    REQUIRE(a.num_chunks() >= 2U);
    REQUIRE(counting.allocs() == after_ctor + (a.num_chunks() - 1U));
}

TEST_CASE("GrowableLinearAllocator: an oversized allocation gets its own right-sized chunk", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    GrowableLinearAllocator      a(1024, &root);

    void* big = a.allocate(4096, 16); // > chunk_bytes
    REQUIRE(big != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(big) % 16U == 0U);
    REQUIRE(a.owns(big));
    REQUIRE(a.bytes_reserved() >= 4096U);
}

TEST_CASE("GrowableLinearAllocator: exact fill-to-tail then one more grows (boundary)", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    GrowableLinearAllocator      a(1024, &root);

    // Fill the reserved bytes of the first chunk exactly to its tail, then one more byte forces a new chunk.
    const crd::usize room = a.bytes_reserved(); // capacity incl. header of the single chunk
    REQUIRE(a.num_chunks() == 1U);
    // Allocate a run of 1-byte cells until the first chunk is full; the next allocation must land in a 2nd chunk.
    crd::usize count = 0;
    while (a.num_chunks() == 1U && count < room + 8U)
    {
        (void)a.allocate(1, 1);
        ++count;
    }
    REQUIRE(a.num_chunks() == 2U); // the tail was reached and growth happened cleanly
}

TEST_CASE("GrowableLinearAllocator: reset() rewinds and REUSES the chunks (no new parent alloc)", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    CountingAllocator            counting(&root);
    GrowableLinearAllocator      a(1024, &counting);

    for (int i = 0; i < 40; ++i) { (void)a.allocate(64, 8); } // grows to several chunks
    const crd::usize chunks_before = a.num_chunks();
    REQUIRE(chunks_before >= 2U);
    const crd::u64 allocs_before = counting.allocs();

    a.reset();
    REQUIRE(a.bytes_used() == 0U);

    // Re-fill the SAME amount: the chunks are reused, so NO new parent allocation and the chunk count is unchanged.
    for (int i = 0; i < 40; ++i) { (void)a.allocate(64, 8); }
    REQUIRE(a.num_chunks() == chunks_before);
    REQUIRE(counting.allocs() == allocs_before);
}

TEST_CASE("GrowableLinearAllocator: deallocate is a no-op; reallocate copies", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    GrowableLinearAllocator      a(1024, &root);

    auto* p = static_cast<unsigned char*>(a.allocate(4, 1));
    p[0] = 0xDE;
    p[1] = 0xAD;
    p[2] = 0xBE;
    p[3] = 0xEF;

    a.deallocate(p); // no-op — must not crash, and subsequent allocation still works
    void* after = a.allocate(8, 8);
    REQUIRE(after != nullptr);

    auto* grown = static_cast<unsigned char*>(a.reallocate(p, 4, 16, 1));
    REQUIRE(grown != nullptr);
    REQUIRE(grown[0] == 0xDE);
    REQUIRE(grown[1] == 0xAD);
    REQUIRE(grown[2] == 0xBE);
    REQUIRE(grown[3] == 0xEF);
}

TEST_CASE("GrowableLinearAllocator: the destructor frees every chunk", "[memory][arena]")
{
    crd::memory::MallocAllocator root;
    CountingAllocator            counting(&root);
    {
        GrowableLinearAllocator a(1024, &counting);
        for (int i = 0; i < 50; ++i) { (void)a.allocate(64, 8); } // several chunks
        REQUIRE(a.num_chunks() >= 2U);
    } // destroyed here
    // Every chunk allocated from the parent was returned to it.
    REQUIRE(counting.deallocs() == counting.allocs());
    REQUIRE(counting.allocs() >= 2U);
}
