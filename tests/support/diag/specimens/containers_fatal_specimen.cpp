// Container contract fatals (crd-containers-fatal-* CTests, tests/foundation/containers). Heap allocators are fatal on
// their own when out of memory, but a sub-budget allocator (an arena, a pool) returns nullptr from allocate when it is
// exhausted. A container cannot continue without its buffer, so that refusal must end in the container's own
// out-of-memory fatal in every build, never in a null buffer adopted with a capacity (writes through it would land
// at small addresses). HashMap::at is the lookup for a key the caller's invariant says is present: a miss is fatal,
// never a null pointer followed. One mode per run (argv[1]); see expected_fatal.hpp for the exit protocol:
//   array-grow-refused     Array::push_back past its capacity on a refusing allocator
//   string-grow-refused    String::append past the inline buffer on a refusing allocator
//   string-init-refused    String construction from a long text on a refusing allocator
//   hashmap-grow-refused   HashMap::insert into an empty table on a refusing allocator
//   hashmap-at-missing     HashMap::at for an absent key
//   ring-buffer-refused    RingBuffer construction on a refusing allocator
//   spsc-queue-refused     SpscQueue construction on a refusing allocator
//   atomic-array-refused   AtomicArray construction on a refusing allocator
//   concurrent-queue-refused  ConcurrentQueue construction on a refusing allocator

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "specimen_common.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/atomic_array.hpp>
#include <crd/containers/concurrent_queue.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/ring_buffer.hpp>
#include <crd/containers/spsc_queue.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace
{
namespace cont = crd::containers;

// An allocator that refuses every request, as an exhausted arena does: allocate returns nullptr.
class RefusingAllocator final : public crd::memory::IAllocator
{
public:
    RefusingAllocator() noexcept { m_name = "containers-fatal-refusing"; }

    void* allocate(crd::usize /*size*/, crd::usize /*alignment*/) override { return nullptr; }
    [[nodiscard]] void* try_allocate(crd::usize /*size*/, crd::usize /*alignment*/) override { return nullptr; }
    void deallocate(void* /*p*/) noexcept override {}
    [[nodiscard]] bool owns(const void* /*p*/) const noexcept override { return false; }
};

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    RefusingAllocator refusing;
    if (is(mode, "array-grow-refused"))
    {
        crd_diag_fatal::arm(mode, "Array: out of memory growing the buffer");
        cont::Array<crd::u64> a(&refusing);
        a.push_back(1U);
        return true;
    }
    if (is(mode, "string-grow-refused"))
    {
        crd_diag_fatal::arm(mode, "String: out of memory");
        cont::String s(&refusing);
        s.append("past the twenty-three inline characters of the small buffer");
        return true;
    }
    if (is(mode, "string-init-refused"))
    {
        crd_diag_fatal::arm(mode, "String: out of memory");
        const cont::String s("past the twenty-three inline characters of the small buffer", &refusing);
        return s.size() != 0U;
    }
    if (is(mode, "hashmap-grow-refused"))
    {
        crd_diag_fatal::arm(mode, "HashMap: out of memory growing the table");
        cont::HashMap<crd::u64, crd::u64> m(&refusing);
        (void)m.insert(1U, 2U);
        return true;
    }
    if (is(mode, "hashmap-at-missing"))
    {
        crd_diag_fatal::arm(mode, "HashMap::at: the key is not present");
        cont::HashMap<crd::u64, crd::u64> m;
        (void)m.insert(1U, 2U);
        return m.at(crd::u64{7}) == 0U;
    }
    if (is(mode, "ring-buffer-refused"))
    {
        crd_diag_fatal::arm(mode, "RingBuffer: out of memory for the buffer");
        const cont::RingBuffer<crd::u64> r(8U, &refusing);
        return r.capacity() != 0U;
    }
    if (is(mode, "spsc-queue-refused"))
    {
        crd_diag_fatal::arm(mode, "SpscQueue: out of memory for the buffer");
        cont::SpscQueue<crd::u64> q(8U, &refusing);
        return q.capacity() != 0U;
    }
    if (is(mode, "atomic-array-refused"))
    {
        crd_diag_fatal::arm(mode, "AtomicArray: out of memory for the buffer");
        cont::AtomicArray<crd::u64> a(8U, &refusing);
        return a.capacity() != 0U;
    }
    if (is(mode, "concurrent-queue-refused"))
    {
        crd_diag_fatal::arm(mode, "ConcurrentQueue: out of memory");
        cont::ConcurrentQueue<crd::u64> q(8U, &refusing);
        return q.capacity() != 0U;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
