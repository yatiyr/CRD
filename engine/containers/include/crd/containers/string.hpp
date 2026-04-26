#pragma once

#include <crd/containers/hash.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <compare>
#include <cstring>
#include <string_view>

namespace crd::containers
{
// -----------------------------------------------------------------------
// String — allocator-aware string with small-string optimisation.
//
// Layout (32 bytes total on 64-bit):
//   union {
//       struct { char buf[23]; u8 size_or_flag; } m_small;
//       struct { char* data; usize size; usize cap_and_flag; } m_heap;
//   };
//   IAllocator* m_alloc;          (8 bytes)
//
// SSO state discriminant is the LAST byte of the 24-byte payload union:
//   - In small mode it holds the live size (0..23).
//   - In heap mode it holds 0xFF (sentinel — chosen so it can never be a
//     valid small-mode size and never be the top byte of any reasonable
//     heap capacity, which is bounded to ~63 bits anyway).
//
// The last byte of m_small.size_or_flag and the top byte of
// m_heap.cap_and_flag share the same offset (byte 23), which is how we
// probe sso_state() with a single read.
//
// Notes on lifetime / aliasing:
//   - Active member tracking is manual via the flag byte. Reads always
//     go through the active member.
//   - Heap-mode `m_heap.size` does NOT include the trailing NUL; we
//     always reserve one extra byte for `\0` so c_str() is O(1).
// -----------------------------------------------------------------------
class String
{
public:
    // ---- Ctors ----------------------------------------------------

    explicit String(memory::IAllocator* alloc = memory::default_allocator()) noexcept : m_alloc(alloc) { init_empty(); }

    // EXPLICIT on purpose: an implicit conversion from `const char*` to String
    // would compete with the `String -> std::string_view` conversion when
    // resolving comparisons like `s == "literal"`, producing overload
    // ambiguity. Forcing direct-initialisation (`String s("x")`) keeps the
    // comparison rules unambiguous via the friend operator==.
    explicit String(const char* cstr, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        const usize n = cstr ? std::char_traits<char>::length(cstr) : 0;
        init_from(cstr, n);
    }

    String(const char* cstr, usize n, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        init_from(cstr, n);
    }

    explicit String(std::string_view sv, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        init_from(sv.data(), sv.size());
    }

    String(const String& other, memory::IAllocator* alloc = nullptr) : m_alloc(alloc ? alloc : other.m_alloc)
    {
        init_from(other.data(), other.size());
    }

    String(String&& other) noexcept : m_alloc(other.m_alloc)
    {
        // Bytewise copy of the union payload preserves either layout exactly.
        std::memcpy(&m_small, &other.m_small, sizeof(m_small));
        // Source becomes an empty SSO with its allocator preserved so it
        // stays usable as an empty string.
        other.init_empty();
    }

    ~String()
    {
        if (!sso_state())
        {
            m_alloc->deallocate(m_heap.data);
        }
    }

    // ---- Assignment -----------------------------------------------

    String& operator=(const String& other)
    {
        if (this == &other)
        {
            return *this;
        }
        assign(other.data(), other.size());
        return *this;
    }

    String& operator=(String&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (!sso_state())
        {
            m_alloc->deallocate(m_heap.data);
        }
        m_alloc = other.m_alloc;
        std::memcpy(&m_small, &other.m_small, sizeof(m_small));
        other.init_empty();
        return *this;
    }

    String& operator=(const char* cstr)
    {
        assign(cstr, cstr ? std::char_traits<char>::length(cstr) : 0);
        return *this;
    }

    String& operator=(std::string_view sv)
    {
        assign(sv.data(), sv.size());
        return *this;
    }

    // ---- Access ---------------------------------------------------

    const char* c_str() const noexcept { return data(); }

    char* data() noexcept { return sso_state() ? m_small.buf : m_heap.data; }

    const char* data() const noexcept { return sso_state() ? m_small.buf : m_heap.data; }

    usize size() const noexcept
    {
        return sso_state() ? static_cast<usize>(m_small.size_or_flag) : heap_size_internal();
    }

    usize capacity() const noexcept { return sso_state() ? kSsoCapacity : heap_capacity_internal(); }

    bool empty() const noexcept { return size() == 0; }
    bool is_small() const noexcept { return sso_state(); }

    // ---- Modifiers ------------------------------------------------

    void clear() noexcept
    {
        if (sso_state())
        {
            m_small.buf[0] = '\0';
            m_small.size_or_flag = 0;
        }
        else
        {
            m_heap.data[0] = '\0';
            set_heap_size(0);
        }
    }

    void reserve(usize n)
    {
        if (n <= capacity())
        {
            return;
        }
        grow_to(n);
    }

    [[nodiscard]] bool try_reserve(usize n) noexcept
    {
        if (n <= capacity())
        {
            return true;
        }
        return try_grow_to(n);
    }

    void resize(usize n, char fill = '\0')
    {
        const usize old = size();
        if (n > old)
        {
            reserve(n);
            char* p = data();
            std::memset(p + old, static_cast<unsigned char>(fill), n - old);
            p[n] = '\0';
        }
        else
        {
            data()[n] = '\0';
        }
        set_size(n);
    }

    void push_back(char c)
    {
        const usize old = size();
        reserve(old + 1);
        char* p = data();
        p[old] = c;
        p[old + 1] = '\0';
        set_size(old + 1);
    }

    void pop_back() noexcept
    {
        const usize old = size();
        CRD_ASSERT(old > 0);
        char* p = data();
        p[old - 1] = '\0';
        set_size(old - 1);
    }

    void append(std::string_view sv) { append(sv.data(), sv.size()); }

    void append(const char* cstr) { append(cstr, cstr ? std::char_traits<char>::length(cstr) : 0); }

    void append(const char* s, usize n)
    {
        if (n == 0)
        {
            return;
        }
        const usize old = size();
        reserve(old + n);
        char* p = data();
        std::memcpy(p + old, s, n);
        p[old + n] = '\0';
        set_size(old + n);
    }

    void shrink_to_fit()
    {
        if (sso_state())
        {
            return;
        }
        const usize n = heap_size_internal();
        if (n <= kSsoCapacity)
        {
            // Fits inline — copy back to small buffer and free heap.
            char* heap_data = m_heap.data;
            memory::IAllocator* alloc = m_alloc;
            std::memcpy(m_small.buf, heap_data, n);
            m_small.buf[n] = '\0';
            m_small.size_or_flag = static_cast<u8>(n);
            alloc->deallocate(heap_data);
            return;
        }
        // Already in heap mode and won't fit inline; shrink heap allocation.
        if (n + 1 < heap_capacity_internal())
        {
            char* new_buf = static_cast<char*>(m_alloc->allocate(n + 1, alignof(char)));
            std::memcpy(new_buf, m_heap.data, n + 1);
            m_alloc->deallocate(m_heap.data);
            m_heap.data = new_buf;
            set_heap_capacity(n + 1);
        }
    }

    // ---- Conversion / comparison ---------------------------------

    operator std::string_view() const noexcept { return std::string_view{data(), size()}; }

    // Comparisons. We provide TWO explicit pairs:
    //   (String, String)         — exact match, beats every rewrite
    //   (String, std::string_view) — handles String<->StringView,
    //                                 String<->const char*, and the reversed
    //                                 forms via C++20 rewrite rules.
    // String's ctors from const char* / string_view are explicit (see top of
    // class), so no implicit `const char* -> String` competes with
    // `const char* -> string_view` and overload resolution stays clean.
    friend bool operator==(const String& a, const String& b) noexcept
    {
        return std::string_view{a.data(), a.size()} == std::string_view{b.data(), b.size()};
    }

    friend std::strong_ordering operator<=>(const String& a, const String& b) noexcept
    {
        const std::string_view sa{a.data(), a.size()};
        const std::string_view sb{b.data(), b.size()};
        if (sa < sb)
        {
            return std::strong_ordering::less;
        }
        if (sa > sb)
        {
            return std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    }

    friend bool operator==(const String& a, std::string_view b) noexcept
    {
        return std::string_view{a.data(), a.size()} == b;
    }

    friend std::strong_ordering operator<=>(const String& a, std::string_view b) noexcept
    {
        const std::string_view sa{a.data(), a.size()};
        if (sa < b)
        {
            return std::strong_ordering::less;
        }
        if (sa > b)
        {
            return std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    }

    memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    // SSO buffer holds 23 visible chars (one byte is the size/flag). The
    // 24th byte (size_or_flag) is the discriminant.
    static constexpr usize kSsoCapacity = 23;
    static constexpr u8 kHeapFlag = 0xFFu;

    union
    {
        struct
        {
            char buf[kSsoCapacity];
            u8 size_or_flag; // 0..23 = small; 0xFF = heap sentinel
        } m_small;

        struct
        {
            char* data;
            usize size;         // not counting the trailing '\0'
            usize cap_and_flag; // top byte = 0xFF when heap mode, lower
                                // bits = capacity (incl. NUL slot)
        } m_heap;
    };

    memory::IAllocator* m_alloc = nullptr;

    // ---- Discriminant helpers --------------------------------------
    bool sso_state() const noexcept { return m_small.size_or_flag != kHeapFlag; }

    // Internal heap-mode field accessors. Capacity uses the low 56 bits,
    // top byte is reserved for the sentinel (0xFF).
    static constexpr usize kCapMask = (~static_cast<usize>(0)) >> 8;

    usize heap_size_internal() const noexcept { return m_heap.size; }

    usize heap_capacity_internal() const noexcept { return m_heap.cap_and_flag & kCapMask; }

    void set_heap_size(usize n) noexcept { m_heap.size = n; }

    void set_heap_capacity(usize cap) noexcept
    {
        m_heap.cap_and_flag = (cap & kCapMask) | (static_cast<usize>(kHeapFlag) << ((sizeof(usize) - 1) * 8));
    }

    void set_size(usize n) noexcept
    {
        if (sso_state())
        {
            CRD_ASSERT(n <= kSsoCapacity);
            m_small.size_or_flag = static_cast<u8>(n);
        }
        else
        {
            set_heap_size(n);
        }
    }

    // ---- Init helpers ----------------------------------------------
    void init_empty() noexcept
    {
        m_small.buf[0] = '\0';
        m_small.size_or_flag = 0;
    }

    void init_from(const char* s, usize n)
    {
        if (n <= kSsoCapacity)
        {
            if (n > 0)
            {
                std::memcpy(m_small.buf, s, n);
            }
            m_small.buf[n] = '\0';
            m_small.size_or_flag = static_cast<u8>(n);
            return;
        }
        // Heap.
        const usize cap = n + 1;
        char* p = static_cast<char*>(m_alloc->allocate(cap, alignof(char)));
        std::memcpy(p, s, n);
        p[n] = '\0';
        m_heap.data = p;
        set_heap_size(n);
        set_heap_capacity(cap);
        // Touch top byte to set the sentinel — covered by set_heap_capacity().
    }

    void assign(const char* s, usize n)
    {
        if (n <= capacity())
        {
            // Fits in current storage.
            char* p = data();
            if (n > 0)
            {
                std::memmove(p, s, n);
            }
            p[n] = '\0';
            set_size(n);
            return;
        }
        // Need to grow. If currently heap, free first.
        if (!sso_state())
        {
            m_alloc->deallocate(m_heap.data);
        }
        init_from(s, n);
    }

    // Grow capacity to at least `target` characters (excluding NUL).
    void grow_to(usize target)
    {
        const usize new_cap_chars = next_capacity(target);
        const usize old_size = size();
        char* new_buf = static_cast<char*>(m_alloc->allocate(new_cap_chars + 1, alignof(char)));
        if (old_size > 0)
        {
            std::memcpy(new_buf, data(), old_size);
        }
        new_buf[old_size] = '\0';

        if (!sso_state())
        {
            m_alloc->deallocate(m_heap.data);
        }
        m_heap.data = new_buf;
        set_heap_size(old_size);
        set_heap_capacity(new_cap_chars + 1);
    }

    [[nodiscard]] bool try_grow_to(usize target) noexcept
    {
        const usize new_cap_chars = next_capacity(target);
        const usize old_size = size();
        char* new_buf = static_cast<char*>(m_alloc->allocate(new_cap_chars + 1, alignof(char)));
        if (!new_buf)
        {
            return false;
        }
        if (old_size > 0)
        {
            std::memcpy(new_buf, data(), old_size);
        }
        new_buf[old_size] = '\0';

        if (!sso_state())
        {
            m_alloc->deallocate(m_heap.data);
        }
        m_heap.data = new_buf;
        set_heap_size(old_size);
        set_heap_capacity(new_cap_chars + 1);
        return true;
    }

    // 1.5x growth, minimum step that exceeds SSO so we never shrink to
    // SSO accidentally during grow paths.
    usize next_capacity(usize target) const noexcept
    {
        const usize current = capacity();
        const usize grown = current + (current >> 1);
        usize next = grown < kSsoCapacity * 2 ? kSsoCapacity * 2 : grown;
        if (target > next)
        {
            next = target;
        }
        return next;
    }
};

// ABI sanity: 24-byte union payload + 8-byte allocator pointer = 32.
static_assert(sizeof(String) == 32, "String layout drifted");

// The friend `operator==(String, std::string_view)` picks up:
//   - String == String   (RHS converts via String's std::string_view conv)
//   - String == StringView
//   - String == const char*
//   - swapped operands by the symmetric synthesis rules in C++20

// ---- Heterogeneous hash --------------------------------------------
//
// DefaultHash<String> delegates to DefaultHash<StringView> so a String
// and a StringView holding the same bytes hash to the same u64. v1c's
// HashMap heterogeneous lookup depends on this.
inline u64 DefaultHash<String>::operator()(const String& s) const noexcept
{
    return DefaultHash<StringView>{}(StringView{s.data(), s.size()});
}
} // namespace crd::containers
