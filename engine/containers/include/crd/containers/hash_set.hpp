#pragma once

#include <crd/containers/hash_map.hpp>

namespace crd::containers
{
// -----------------------------------------------------------------------
// HashSet<K> — set of unique keys.
//
// Implemented as a thin wrapper over `HashMap<K, EmptySetValue>`. Same
// probing, same backshift, same heterogeneous lookup. The value side is
// a 1-byte empty struct so the storage cost is only the keys + dist
// table (essentially the same as a hand-rolled HashSet).
//
// Heterogeneous: `find`/`contains`/`erase` accept any key the hasher
// and KeyEqual accept.
// -----------------------------------------------------------------------

namespace detail
{
struct EmptySetValue
{
};
} // namespace detail

template <typename K, typename Hash = DefaultHash<K>, typename KeyEqual = std::equal_to<>> class HashSet
{
    using MapT = HashMap<K, detail::EmptySetValue, Hash, KeyEqual>;

public:
    using key_type = K;
    using size_type = usize;

    explicit HashSet(memory::IAllocator* alloc = memory::default_allocator()) noexcept : m_map(alloc) {}

    HashSet(usize initial_capacity_hint, memory::IAllocator* alloc = memory::default_allocator())
        : m_map(initial_capacity_hint, alloc)
    {
    }

    HashSet(const HashSet& other, memory::IAllocator* alloc = nullptr) : m_map(other.m_map, alloc) {}

    HashSet(HashSet&&) noexcept = default;
    HashSet& operator=(const HashSet&) = default;
    HashSet& operator=(HashSet&&) noexcept = default;
    ~HashSet() = default;

    // ---- Capacity --------------------------------------------------
    usize size() const noexcept { return m_map.size(); }
    usize capacity() const noexcept { return m_map.capacity(); }
    bool empty() const noexcept { return m_map.empty(); }
    f32 load_factor() const noexcept { return m_map.load_factor(); }
    void reserve(usize n) { m_map.reserve(n); }
    void clear() noexcept { m_map.clear(); }

    memory::IAllocator* allocator() const noexcept { return m_map.allocator(); }

    // ---- Modifiers -------------------------------------------------
    bool insert(const K& key) { return m_map.insert(key, detail::EmptySetValue{}); }

    template <typename Q> bool erase(const Q& key) { return m_map.erase(key); }

    // ---- Lookup (heterogeneous) -----------------------------------
    template <typename Q> bool contains(const Q& key) const noexcept { return m_map.contains(key); }

    // ---- Iteration -------------------------------------------------
    // Iterators expose only keys (set-like). Wraps the map's iterator.
    class const_iterator
    {
    public:
        explicit const_iterator(typename MapT::const_iterator it) noexcept : m_it(it) {}
        const_iterator& operator++() noexcept
        {
            ++m_it;
            return *this;
        }
        const K& operator*() const noexcept { return m_it.key(); }
        bool operator==(const const_iterator& o) const noexcept { return m_it == o.m_it; }
        bool operator!=(const const_iterator& o) const noexcept { return m_it != o.m_it; }

    private:
        typename MapT::const_iterator m_it;
    };

    const_iterator begin() const noexcept { return const_iterator{m_map.begin()}; }
    const_iterator end() const noexcept { return const_iterator{m_map.end()}; }

private:
    MapT m_map;
};
} // namespace crd::containers
