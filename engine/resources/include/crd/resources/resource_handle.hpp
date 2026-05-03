#pragma once

#include <crd/resources/load_state.hpp>
#include <crd/resources/resource_control_block.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

class ResourceManager;

// Non-templated base that holds the control block pointer and performs all
// type-erased operations (refcounting, state query, id query, move/copy).
// ResourceHandle<T> inherits this and adds the typed get().
class ResourceHandleBase
{
public:
    ResourceHandleBase() noexcept = default;

    ResourceHandleBase(const ResourceHandleBase& o) noexcept : m_block(o.m_block)
    {
        if (m_block)
        {
            m_block->add_ref();
        }
    }

    ResourceHandleBase(ResourceHandleBase&& o) noexcept : m_block(o.m_block)
    {
        o.m_block = nullptr;
    }

    ~ResourceHandleBase()
    {
        release_block();
    }

    ResourceHandleBase& operator=(const ResourceHandleBase& o) noexcept
    {
        if (this != &o)
        {
            release_block();
            m_block = o.m_block;
            if (m_block)
            {
                m_block->add_ref();
            }
        }
        return *this;
    }

    ResourceHandleBase& operator=(ResourceHandleBase&& o) noexcept
    {
        if (this != &o)
        {
            release_block();
            m_block    = o.m_block;
            o.m_block  = nullptr;
        }
        return *this;
    }

    [[nodiscard]] LoadState state() const noexcept
    {
        if (!m_block)
        {
            return LoadState::Unloaded;
        }
        return m_block->state.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_ready() const noexcept { return state() == LoadState::Ready; }

    [[nodiscard]] ResourceId id() const noexcept
    {
        return m_block ? m_block->id : kNullResourceId;
    }

    [[nodiscard]] crd::u32 generation() const noexcept
    {
        return m_block ? m_block->generation.load(std::memory_order_relaxed) : 0U;
    }

    // Fiber-cooperative wait (v1d+). In v1c this is a no-op synchronous return.
    LoadState wait_ready()
    {
        return state();
    }

protected:
    friend class ResourceManager;

    explicit ResourceHandleBase(ResourceControlBlock* b) noexcept : m_block(b) {}

    [[nodiscard]] ResourceControlBlock* block() const noexcept { return m_block; }

private:
    ResourceControlBlock* m_block = nullptr;

    void release_block() noexcept
    {
        if (!m_block)
        {
            return;
        }
        if (m_block->release() == 0U && !m_block->permanent)
        {
            // Non-permanent (failed) block: free it.
            if (m_block->payload && m_block->loader)
            {
                m_block->loader->unload(m_block->payload);
                m_block->payload = nullptr;
            }
            crd::memory::IAllocator* alloc = m_block->alloc;
            m_block->~ResourceControlBlock();
            alloc->deallocate(m_block);
        }
        m_block = nullptr;
    }
};

// Typed resource handle. sizeof == sizeof(ResourceHandleBase) == one pointer.
//
// get() casts the type-erased payload to const T*. The cast is safe only when
// the loader registered for the matching type_fourcc allocates a T (by convention).
template <typename T>
class ResourceHandle : public ResourceHandleBase
{
public:
    ResourceHandle() noexcept = default;

    // Copy / move / destructor — all delegated to ResourceHandleBase.
    ResourceHandle(const ResourceHandle&)            = default;
    ResourceHandle(ResourceHandle&&)                 = default;
    ResourceHandle& operator=(const ResourceHandle&) = default;
    ResourceHandle& operator=(ResourceHandle&&)      = default;

    // Non-blocking. Returns nullptr until state() == Ready (or Placeholder).
    [[nodiscard]] const T* get() const noexcept
    {
        const ResourceControlBlock* b = block();
        if (!b)
        {
            return nullptr;
        }
        const LoadState s = b->state.load(std::memory_order_acquire);
        if (s != LoadState::Ready && s != LoadState::Placeholder)
        {
            return nullptr;
        }
        return static_cast<const T*>(b->payload);
    }

private:
    friend class ResourceManager;

    explicit ResourceHandle(ResourceControlBlock* b) noexcept : ResourceHandleBase(b) {}
};

} // namespace crd::resources
