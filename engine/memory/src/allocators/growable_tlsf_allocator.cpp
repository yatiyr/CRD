#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <crd/core/assert.hpp>

#include <cstring>
#include <new>

namespace crd::memory
{
namespace
{
// Just under the 2^32 = 4 GB single-pool TLSF cap, leaving headroom for the pool
// sentinels so a chunk of this size always initialises.
constexpr usize kMaxChunkBytes = (usize{4} << 30) - (usize{16} << 20);

inline usize round_up(usize a, usize b) noexcept
{
    return ((a + b - 1) / b) * b;
}
} // namespace

struct GrowableTlsfAllocator::Chunk
{
    TlsfAllocator tlsf; // NON-owning: manages `pool`; the GrowableTlsfAllocator frees `pool` itself
    void*         pool; // parent-allocated TLSF pool buffer (16-byte aligned, `cap` bytes)
    Chunk*        next;
    Chunk(void* pool_buf, usize cap, Chunk* nx) : tlsf(pool_buf, cap), pool(pool_buf), next(nx) {}
};

usize GrowableTlsfAllocator::max_chunk_bytes() noexcept
{
    return kMaxChunkBytes;
}

GrowableTlsfAllocator::GrowableTlsfAllocator(usize chunk_bytes, IAllocator* parent, const char* name) noexcept
    : m_parent(parent != nullptr ? parent : default_allocator()), m_chunk_bytes(chunk_bytes)
{
    m_name                = name;
    const usize floor_cap = TlsfAllocator::min_pool_size();
    if (m_chunk_bytes < floor_cap) { m_chunk_bytes = floor_cap; }
    if (m_chunk_bytes > kMaxChunkBytes) { m_chunk_bytes = kMaxChunkBytes; }
}

GrowableTlsfAllocator::~GrowableTlsfAllocator()
{
    Chunk* c = m_head;
    while (c != nullptr)
    {
        Chunk* nx   = c->next;
        void*  pool = c->pool;      // capture before destruction (TLSF is non-owning)
        c->~Chunk();                // destroy the (non-owning) TlsfAllocator: does NOT free pool
        m_parent->deallocate(pool); // free the TLSF pool buffer
        m_parent->deallocate(c);    // free the Chunk node itself
        c = nx;
    }
}

GrowableTlsfAllocator::Chunk* GrowableTlsfAllocator::grow(usize min_bytes)
{
    usize cap = m_chunk_bytes;
    if (min_bytes > cap) { cap = round_up(min_bytes, usize{4} << 10); } // 4 KB page granularity
    CRD_ASSERT_MSG(cap <= kMaxChunkBytes,
                   "GrowableTlsfAllocator: single allocation exceeds the per-chunk TLSF cap (~4 GB)");
    // Pull the TLSF pool buffer, then the Chunk node, via try_allocate — so a
    // fatal-on-OOM parent (VirtualMemoryAllocator) returns nullptr here instead of
    // aborting, and the non-throwing contract holds end-to-end. kDefaultAlignment
    // (16) satisfies TLSF's pool alignment requirement.
    void* pool = m_parent->try_allocate(cap, kDefaultAlignment);
    if (pool == nullptr) { return nullptr; }
    void* node = m_parent->try_allocate(sizeof(Chunk), alignof(Chunk));
    if (node == nullptr)
    {
        m_parent->deallocate(pool);
        return nullptr;
    }
    Chunk* c = new (node) Chunk(pool, cap, m_head);
    m_head   = c;
    ++m_num_chunks;
    return c;
}

void* GrowableTlsfAllocator::try_allocate(usize size, usize alignment)
{
    if (size == 0) { return nullptr; }
    for (Chunk* c = m_head; c != nullptr; c = c->next)
    {
        if (void* p = c->tlsf.try_allocate(size, alignment)) { return p; }
    }
    // No existing chunk fits — grow one large enough for this request. TLSF's
    // mapping_search rounds the request UP to the next second-level size sub-class
    // (≈ size / kSlIndexCount = size/32) before searching, so the chunk's free
    // block must be at least size·33/32. Add that round-up, the alignment
    // allowance, the empty-pool sentinel overhead, and a small fixed header slack.
    const usize search_round = (size >> 5) + 1; // ~size/32: the SL sub-class round-up
    const usize need = size + search_round + alignment + TlsfAllocator::min_pool_size() + (usize{4} << 10);
    if (need > kMaxChunkBytes) { return nullptr; }
    Chunk* c = grow(need);
    if (c == nullptr) { return nullptr; } // parent exhausted (grow used try_allocate)
    return c->tlsf.try_allocate(size, alignment);
}

void* GrowableTlsfAllocator::allocate(usize size, usize alignment)
{
    void* p = try_allocate(size, alignment);
    if (p == nullptr && size != 0)
    {
        CRD_FATAL("GrowableTlsfAllocator: out of memory (single allocation larger than the per-chunk cap?)");
    }
    return p;
}

void GrowableTlsfAllocator::deallocate(void* p) noexcept
{
    if (p == nullptr) { return; }
    for (Chunk* c = m_head; c != nullptr; c = c->next)
    {
        if (c->tlsf.owns(p))
        {
            c->tlsf.deallocate(p);
            return;
        }
    }
    CRD_ASSERT_MSG(false, "GrowableTlsfAllocator: deallocate of a pointer not owned by any chunk");
}

bool GrowableTlsfAllocator::owns(const void* p) const noexcept
{
    for (Chunk* c = m_head; c != nullptr; c = c->next)
    {
        if (c->tlsf.owns(p)) { return true; }
    }
    return false;
}

usize GrowableTlsfAllocator::allocation_size(const void* p) const noexcept
{
    for (Chunk* c = m_head; c != nullptr; c = c->next)
    {
        if (c->tlsf.owns(p)) { return c->tlsf.allocation_size(p); }
    }
    return 0;
}

void* GrowableTlsfAllocator::reallocate(void* p, usize old_size, usize new_size, usize alignment)
{
    if (p == nullptr) { return allocate(new_size, alignment); }
    if (new_size == 0)
    {
        deallocate(p);
        return nullptr;
    }
    // Find the owning chunk; grow within it if possible, else move across chunks
    // (alloc-copy-free). We avoid TlsfAllocator::reallocate here because it is
    // fatal-on-OOM and we want to fall back to a different/new chunk instead.
    for (Chunk* c = m_head; c != nullptr; c = c->next)
    {
        if (!c->tlsf.owns(p)) { continue; }
        const usize copy_n = old_size < new_size ? old_size : new_size;
        if (void* q = c->tlsf.try_allocate(new_size, alignment)) // same chunk has room
        {
            std::memcpy(q, p, copy_n);
            c->tlsf.deallocate(p);
            return q;
        }
        void* q = allocate(new_size, alignment); // another (possibly new) chunk
        std::memcpy(q, p, copy_n);
        c->tlsf.deallocate(p);
        return q;
    }
    CRD_FATAL("GrowableTlsfAllocator: reallocate of a pointer not owned by any chunk");
    return nullptr;
}
} // namespace crd::memory
