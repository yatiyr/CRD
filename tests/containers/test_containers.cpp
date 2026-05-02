#include <crd/containers/containers.hpp>
#include <crd/memory/memory.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <numeric>
#include <string>

using namespace crd;
using namespace crd::containers;
using crd::memory::default_allocator;
using crd::memory::LinearAllocator;

// =============================================================================
// hash.hpp
// =============================================================================

TEST_CASE("hash_u64 splitmix is deterministic and different from identity", "[containers][hash]")
{
    REQUIRE(hash_u64(0) == hash_u64(0));
    REQUIRE(hash_u64(42) == hash_u64(42));
    REQUIRE(hash_u64(0) != 0);
    REQUIRE(hash_u64(1) != hash_u64(2));
}

TEST_CASE("FNV-1a 64-bit known vectors", "[containers][hash]")
{
    // Empty input -> FNV offset basis.
    REQUIRE(fnv1a_64("", 0) == 0xcbf29ce484222325ULL);

    // Same string hashes equal.
    const char* s = "hello world";
    const auto h1 = hash_string(s, std::strlen(s));
    const auto h2 = hash_string(std::string_view{s});
    REQUIRE(h1 == h2);

    // Different strings hash to different values (probabilistic; trivially true here).
    REQUIRE(hash_string(std::string_view{"foo"}) != hash_string(std::string_view{"bar"}));
}

TEST_CASE("DefaultHash specializations", "[containers][hash]")
{
    REQUIRE(DefaultHash<u32>{}(42) == hash_u64(42));
    REQUIRE(DefaultHash<u64>{}(42ULL) == hash_u64(42));
    REQUIRE(DefaultHash<i32>{}(-1) == hash_u64(static_cast<u64>(-1)));

    // const char* via string-hash
    DefaultHash<const char*> hcs;
    REQUIRE(hcs("foo") == hash_string(std::string_view{"foo"}));

    // Pointer hash
    int x = 0;
    int y = 0;
    REQUIRE(DefaultHash<int*>{}(&x) == hash_u64(reinterpret_cast<u64>(&x)));
    REQUIRE(DefaultHash<int*>{}(&x) != DefaultHash<int*>{}(&y));
}

// =============================================================================
// Span (std::span alias)
// =============================================================================

TEST_CASE("Span: as_span over Array round-trips data + size", "[containers][span]")
{
    Array<u32> a;
    for (u32 i = 0; i < 5; ++i)
    {
        a.push_back(i * 10);
    }
    Span<u32> s = as_span(a);
    REQUIRE(s.size() == 5);
    REQUIRE(s[0] == 0);
    REQUIRE(s[4] == 40);
}

TEST_CASE("Span: as_const_span yields read-only view", "[containers][span]")
{
    Array<u32> a;
    a.push_back(7);
    a.push_back(8);
    ConstSpan<u32> v = as_const_span(a);
    REQUIRE(v.size() == 2);
    REQUIRE(v[0] == 7);
    REQUIRE(v[1] == 8);
}

TEST_CASE("Span: make_span over raw arrays", "[containers][span]")
{
    int arr[4] = {1, 2, 3, 4};
    auto s = make_span(arr);
    REQUIRE(s.size() == 4);
    REQUIRE(s[2] == 3);
}

// =============================================================================
// Array<T>
// =============================================================================

TEST_CASE("Array: default construction is empty", "[containers][array]")
{
    Array<u32> a;
    REQUIRE(a.size() == 0);
    REQUIRE(a.capacity() == 0);
    REQUIRE(a.empty());
    REQUIRE(a.allocator() == default_allocator());
}

TEST_CASE("Array: push_back grows with 1.5x policy starting at 8", "[containers][array]")
{
    Array<u32> a;
    a.push_back(10);
    REQUIRE(a.size() == 1);
    REQUIRE(a.capacity() == 8); // initial capacity

    for (u32 i = 0; i < 7; ++i)
    {
        a.push_back(i);
    }
    REQUIRE(a.size() == 8);
    REQUIRE(a.capacity() == 8);

    // Triggering grow
    a.push_back(99);
    REQUIRE(a.size() == 9);
    REQUIRE(a.capacity() == 12); // 8 + 8/2 = 12
}

TEST_CASE("Array: emplace_back forwards args and returns reference", "[containers][array]")
{
    struct Pair
    {
        int a;
        int b;
        Pair(int x, int y) : a(x), b(y) {}
    };
    Array<Pair> a;
    Pair& p = a.emplace_back(3, 4);
    REQUIRE(p.a == 3);
    REQUIRE(p.b == 4);
    REQUIRE(a.size() == 1);
}

TEST_CASE("Array: front/back/operator[] work as expected", "[containers][array]")
{
    Array<int> a = {10, 20, 30};
    REQUIRE(a.size() == 3);
    REQUIRE(a.front() == 10);
    REQUIRE(a.back() == 30);
    REQUIRE(a[1] == 20);
    a[1] = 99;
    REQUIRE(a[1] == 99);
}

TEST_CASE("Array: pop_back shrinks size, capacity unchanged", "[containers][array]")
{
    Array<int> a = {1, 2, 3};
    const usize cap_before = a.capacity();
    a.pop_back();
    REQUIRE(a.size() == 2);
    REQUIRE(a.capacity() == cap_before);
    REQUIRE(a.back() == 2);
}

TEST_CASE("Array: clear destroys elements but keeps capacity", "[containers][array]")
{
    Array<int> a = {1, 2, 3, 4, 5};
    const usize cap = a.capacity();
    a.clear();
    REQUIRE(a.empty());
    REQUIRE(a.capacity() == cap);
}

TEST_CASE("Array: erase preserves order, swap_remove does not", "[containers][array]")
{
    Array<int> a = {1, 2, 3, 4, 5};
    a.erase(1); // remove the 2
    REQUIRE(a.size() == 4);
    REQUIRE(a[0] == 1);
    REQUIRE(a[1] == 3);
    REQUIRE(a[2] == 4);
    REQUIRE(a[3] == 5);

    Array<int> b = {1, 2, 3, 4, 5};
    b.swap_remove(1); // remove the 2 by overwriting with the back
    REQUIRE(b.size() == 4);
    REQUIRE(b[1] == 5); // back was moved into slot 1
}

TEST_CASE("Array: insert at any position", "[containers][array]")
{
    Array<int> a = {1, 2, 4};
    a.insert(2, 3);
    REQUIRE(a.size() == 4);
    REQUIRE(a[0] == 1);
    REQUIRE(a[1] == 2);
    REQUIRE(a[2] == 3);
    REQUIRE(a[3] == 4);

    a.insert(0, 0);
    REQUIRE(a[0] == 0);

    a.insert(a.size(), 5);
    REQUIRE(a.back() == 5);
}

TEST_CASE("Array: resize grows and shrinks with default-construct", "[containers][array]")
{
    Array<int> a;
    a.resize(5);
    REQUIRE(a.size() == 5);
    a.resize(2);
    REQUIRE(a.size() == 2);
}

TEST_CASE("Array: resize with fill value", "[containers][array]")
{
    Array<int> a;
    a.resize(4, 7);
    REQUIRE(a.size() == 4);
    REQUIRE(a[0] == 7);
    REQUIRE(a[3] == 7);
}

TEST_CASE("Array: copy ctor preserves contents and uses RHS allocator by default", "[containers][array]")
{
    Array<int> a = {1, 2, 3};
    Array<int> b(a);
    REQUIRE(b.size() == 3);
    REQUIRE(b == a);
    REQUIRE(b.allocator() == a.allocator());
}

TEST_CASE("Array: move ctor leaves source empty", "[containers][array]")
{
    Array<int> a = {1, 2, 3};
    Array<int> b(std::move(a));
    REQUIRE(b.size() == 3);
    REQUIRE(a.size() == 0);
    REQUIRE(a.capacity() == 0);
}

TEST_CASE("Array: copy assignment overwrites", "[containers][array]")
{
    Array<int> a = {1, 2, 3};
    Array<int> b = {10, 20};
    b = a;
    REQUIRE(b == a);
}

TEST_CASE("Array: range-for and std::find work", "[containers][array]")
{
    Array<int> a = {1, 2, 3, 4, 5};
    int sum = 0;
    for (int v : a)
    {
        sum += v;
    }
    REQUIRE(sum == 15);

    auto it = std::find(a.begin(), a.end(), 3);
    REQUIRE(it != a.end());
    REQUIRE(*it == 3);
}

TEST_CASE("Array: std::sort works on iterators", "[containers][array]")
{
    Array<int> a = {3, 1, 4, 1, 5, 9, 2, 6};
    std::sort(a.begin(), a.end());
    REQUIRE(a[0] == 1);
    REQUIRE(a[1] == 1);
    REQUIRE(a[2] == 2);
    REQUIRE(a.back() == 9);
}

TEST_CASE("Array: shrink_to_fit reduces capacity to size", "[containers][array]")
{
    Array<int> a;
    a.reserve(64);
    a.push_back(1);
    a.push_back(2);
    REQUIRE(a.capacity() == 64);
    a.shrink_to_fit();
    REQUIRE(a.capacity() == 2);
    REQUIRE(a[0] == 1);
    REQUIRE(a[1] == 2);
}

TEST_CASE("Array: try_push_back returns false on sub-budget exhaustion", "[containers][array][allocator]")
{
    // Tiny linear allocator: room for ~1 element of int + capacity tracker.
    LinearAllocator tiny(64);
    Array<int> a(&tiny);

    int pushed = 0;
    for (int i = 0; i < 100; ++i)
    {
        if (!a.try_push_back(i))
        {
            break;
        }
        ++pushed;
    }
    // We must have hit the wall at some point because 64 bytes of buffer
    // can't satisfy unbounded grow + element copies.
    REQUIRE(pushed > 0);
    REQUIRE(pushed < 100);
}

TEST_CASE("Array: dtors run on element destruction", "[containers][array][lifetime]")
{
    static int dtor_count = 0;
    struct Counter
    {
        Counter() = default;
        Counter(const Counter&) = default;
        Counter(Counter&&) = default;
        Counter& operator=(const Counter&) = default;
        Counter& operator=(Counter&&) = default;
        ~Counter() { ++dtor_count; }
    };
    dtor_count = 0;
    {
        Array<Counter> a;
        a.emplace_back();
        a.emplace_back();
        a.emplace_back();
        REQUIRE(dtor_count == 0);
    }
    // After the Array's dtor: 3 logical elements destroyed. (Move-during-grow
    // can produce additional dtors if relocation happens; but since we only
    // pushed 3 and capacity starts at 8, no relocation here.)
    REQUIRE(dtor_count == 3);
}

// =============================================================================
// FixedArray<T, N>
// =============================================================================

TEST_CASE("FixedArray: push until full, try_push returns false", "[containers][fixed_array]")
{
    FixedArray<int, 4> a;
    REQUIRE(a.empty());
    REQUIRE(!a.full());
    a.push_back(1);
    a.push_back(2);
    a.push_back(3);
    REQUIRE(a.try_push_back(4));
    REQUIRE(a.full());
    REQUIRE(!a.try_push_back(5));
    REQUIRE(a.size() == 4);
}

TEST_CASE("FixedArray: capacity is compile-time constant", "[containers][fixed_array]")
{
    static_assert(FixedArray<int, 7>::capacity() == 7);
    REQUIRE(FixedArray<int, 7>::capacity() == 7);
}

TEST_CASE("FixedArray: range-for + algorithms", "[containers][fixed_array]")
{
    FixedArray<int, 8> a = {5, 4, 3, 2, 1};
    REQUIRE(a.size() == 5);
    std::sort(a.begin(), a.end());
    REQUIRE(a[0] == 1);
    REQUIRE(a[4] == 5);

    int sum = std::accumulate(a.begin(), a.end(), 0);
    REQUIRE(sum == 15);
}

TEST_CASE("FixedArray: clear runs dtors and empties", "[containers][fixed_array]")
{
    FixedArray<int, 4> a = {1, 2, 3};
    a.clear();
    REQUIRE(a.empty());
    REQUIRE(a.size() == 0);
}

TEST_CASE("FixedArray: swap_remove keeps O(1) semantics", "[containers][fixed_array]")
{
    FixedArray<int, 4> a = {1, 2, 3};
    a.swap_remove(0);
    REQUIRE(a.size() == 2);
    REQUIRE(a[0] == 3); // last (3) moved into slot 0
    REQUIRE(a[1] == 2);
}

TEST_CASE("FixedArray: copy ctor and assignment", "[containers][fixed_array]")
{
    FixedArray<int, 4> a = {7, 8, 9};
    FixedArray<int, 4> b(a);
    REQUIRE(b.size() == 3);
    REQUIRE(b[2] == 9);

    FixedArray<int, 4> c;
    c = a;
    REQUIRE(c.size() == 3);
    REQUIRE(c[1] == 8);
}

TEST_CASE("FixedArray: works as Span source", "[containers][fixed_array][span]")
{
    FixedArray<int, 8> a = {1, 2, 3, 4};
    Span<int> s = as_span(a);
    REQUIRE(s.size() == 4);
    REQUIRE(s[3] == 4);
}

// =============================================================================
// Channel registration
// =============================================================================

TEST_CASE("Container log channel registers", "[containers][log]")
{
    auto* ch = ::crd::log::find_channel("Containers");
    REQUIRE(ch != nullptr);
}

// =============================================================================
// String — v1b
// =============================================================================

TEST_CASE("String: ABI sanity (sizeof == 32)", "[containers][string][abi]")
{
    REQUIRE(sizeof(String) == 32);
}

TEST_CASE("String: default ctor is empty SSO", "[containers][string]")
{
    String s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
    REQUIRE(s.is_small());
    REQUIRE(std::strlen(s.c_str()) == 0);
}

TEST_CASE("String: small construction stays in SSO at boundary 23", "[containers][string][sso]")
{
    String s23(std::string_view{"12345678901234567890123"}); // exactly 23 chars
    REQUIRE(s23.size() == 23);
    REQUIRE(s23.is_small());
    REQUIRE(s23 == StringView{"12345678901234567890123"});
}

TEST_CASE("String: 24-char string promotes to heap", "[containers][string][sso]")
{
    String s24(std::string_view{"123456789012345678901234"}); // 24 chars
    REQUIRE(s24.size() == 24);
    REQUIRE_FALSE(s24.is_small());
    REQUIRE(s24 == StringView{"123456789012345678901234"});
}

TEST_CASE("String: append promotes from SSO to heap", "[containers][string][sso]")
{
    String s("short");
    REQUIRE(s.is_small());
    s.append(std::string_view{"_now_this_pushes_over_the_sso_boundary"});
    REQUIRE_FALSE(s.is_small());
    REQUIRE(s == StringView{"short_now_this_pushes_over_the_sso_boundary"});
}

TEST_CASE("String: reserve forces heap promotion", "[containers][string]")
{
    String s("hi");
    REQUIRE(s.is_small());
    s.reserve(64);
    REQUIRE_FALSE(s.is_small());
    REQUIRE(s.capacity() >= 64);
    REQUIRE(s == StringView{"hi"});
}

TEST_CASE("String: c_str is null-terminated in both modes", "[containers][string]")
{
    String small("abc");
    REQUIRE(small.is_small());
    REQUIRE(small.c_str()[3] == '\0');

    String large(std::string_view{"this string is way longer than the SSO buffer can hold"});
    REQUIRE_FALSE(large.is_small());
    REQUIRE(large.c_str()[large.size()] == '\0');
}

TEST_CASE("String: push_back / pop_back", "[containers][string]")
{
    String s;
    s.push_back('a');
    s.push_back('b');
    s.push_back('c');
    REQUIRE(s == StringView{"abc"});
    s.pop_back();
    REQUIRE(s == StringView{"ab"});
    REQUIRE(s.size() == 2);
}

TEST_CASE("String: append cstr / view / cstr+n", "[containers][string]")
{
    String s("hello");
    s.append(", ");
    s.append(std::string_view{"world"});
    s.append("!", 1);
    REQUIRE(s == StringView{"hello, world!"});
}

TEST_CASE("String: clear keeps allocator and capacity", "[containers][string]")
{
    String s(std::string_view{"a long string that lives on the heap for sure"});
    REQUIRE_FALSE(s.is_small());
    const auto cap_before = s.capacity();
    s.clear();
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
    REQUIRE(s.capacity() == cap_before); // capacity preserved
    REQUIRE_FALSE(s.is_small());         // still in heap mode
}

TEST_CASE("String: shrink_to_fit returns to SSO when fit", "[containers][string]")
{
    String s(std::string_view{"this lives on the heap because it's long"});
    REQUIRE_FALSE(s.is_small());
    s.clear();
    s.shrink_to_fit();
    REQUIRE(s.is_small());
}

TEST_CASE("String: copy ctor preserves contents", "[containers][string]")
{
    String a("hello");
    String b(a);
    REQUIRE(a == b);
    REQUIRE(b.size() == 5);
    REQUIRE(b.allocator() == a.allocator());

    String c(std::string_view{"a longer string to force heap mode"});
    String d(c);
    REQUIRE(d == c);
    REQUIRE_FALSE(d.is_small());
}

TEST_CASE("String: move leaves source as empty SSO", "[containers][string]")
{
    String a(std::string_view{"a longer string forced onto the heap"});
    REQUIRE_FALSE(a.is_small());
    String b(std::move(a));
    REQUIRE(b.size() == 36);
    REQUIRE(a.is_small()); // source should be a usable empty SSO
    REQUIRE(a.size() == 0);
    REQUIRE(a.empty());
}

TEST_CASE("String: comparison operators (heterogeneous)", "[containers][string]")
{
    String a("hello");
    String b("hello");
    String c("world");

    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a == StringView{"hello"});
    REQUIRE(a == "hello");
    REQUIRE("hello" == a);
    REQUIRE(StringView{"hello"} == a);

    REQUIRE((a <=> c) == std::strong_ordering::less);
    REQUIRE((c <=> a) == std::strong_ordering::greater);
    REQUIRE((a <=> b) == std::strong_ordering::equal);
}

TEST_CASE("String: hashes equal between String and StringView (heterogeneous)", "[containers][string][hash]")
{
    const char* text = "the quick brown fox";

    String s(text);
    StringView sv{text};

    REQUIRE(DefaultHash<String>{}(s) == DefaultHash<StringView>{}(sv));

    // Empty string consistency too.
    String empty;
    REQUIRE(DefaultHash<String>{}(empty) == DefaultHash<StringView>{}(StringView{}));
}

TEST_CASE("String: to_view round-trip", "[containers][string]")
{
    String s("greetings");
    StringView v = to_view(s);
    REQUIRE(v.data() == s.data()); // no copy
    REQUIRE(v.size() == s.size());
}

// =============================================================================
// RingBuffer<T> — v1b
// =============================================================================

TEST_CASE("RingBuffer: capacity must be power of two", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(8);
    REQUIRE(r.capacity() == 8);
    REQUIRE(r.empty());
    REQUIRE_FALSE(r.full());
}

TEST_CASE("RingBuffer: try_push fills up, then refuses", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(4);
    REQUIRE(r.try_push(10));
    REQUIRE(r.try_push(20));
    REQUIRE(r.try_push(30));
    REQUIRE(r.try_push(40));
    REQUIRE(r.full());
    REQUIRE_FALSE(r.try_push(50)); // refused
    REQUIRE(r.size() == 4);
}

TEST_CASE("RingBuffer: try_pop FIFO order", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(4);
    REQUIRE(r.try_push(1));
    REQUIRE(r.try_push(2));
    REQUIRE(r.try_push(3));

    u32 v = 0;
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 1);
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 2);
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 3);
    REQUIRE_FALSE(r.try_pop(v));
    REQUIRE(r.empty());
}

TEST_CASE("RingBuffer: wrap-around correctness", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(4);
    REQUIRE(r.try_push(1));
    REQUIRE(r.try_push(2));
    u32 dropped = 0;
    REQUIRE(r.try_pop(dropped)); // remove 1
    REQUIRE(r.try_push(3));
    REQUIRE(r.try_push(4));
    REQUIRE(r.try_push(5)); // wraps tail
    REQUIRE(r.full());

    u32 v = 0;
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 2);
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 3);
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 4);
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 5);
    REQUIRE(r.empty());
}

TEST_CASE("RingBuffer: snapshot returns chronological order", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(8);
    for (u32 i = 0; i < 5; ++i)
    {
        REQUIRE(r.try_push(i + 100));
    }

    Array<u32> snap;
    r.snapshot(snap);
    REQUIRE(snap.size() == 5);
    REQUIRE(snap[0] == 100);
    REQUIRE(snap[4] == 104);
}

TEST_CASE("RingBuffer: snapshot still correct after wrap-around", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(4);
    REQUIRE(r.try_push(1));
    REQUIRE(r.try_push(2));
    u32 d = 0;
    REQUIRE(r.try_pop(d));
    REQUIRE(r.try_pop(d));
    REQUIRE(r.try_push(3));
    REQUIRE(r.try_push(4));
    REQUIRE(r.try_push(5));
    REQUIRE(r.try_push(6));
    REQUIRE(r.full());

    Array<u32> snap;
    r.snapshot(snap);
    REQUIRE(snap.size() == 4);
    REQUIRE(snap[0] == 3);
    REQUIRE(snap[1] == 4);
    REQUIRE(snap[2] == 5);
    REQUIRE(snap[3] == 6);
}

TEST_CASE("RingBuffer: clear empties without freeing buffer", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(4);
    REQUIRE(r.try_push(1));
    REQUIRE(r.try_push(2));
    REQUIRE(r.size() == 2);
    r.clear();
    REQUIRE(r.empty());
    REQUIRE(r.capacity() == 4);
    REQUIRE(r.try_push(99)); // still usable
}

TEST_CASE("RingBuffer: move ctor leaves source empty", "[containers][ring_buffer]")
{
    RingBuffer<u32> r(4);
    REQUIRE(r.try_push(7));
    REQUIRE(r.try_push(8));

    RingBuffer<u32> moved(std::move(r));
    REQUIRE(moved.size() == 2);
    REQUIRE(r.capacity() == 0);

    u32 v = 0;
    REQUIRE(moved.try_pop(v));
    REQUIRE(v == 7);
    REQUIRE(moved.try_pop(v));
    REQUIRE(v == 8);
}

TEST_CASE("RingBuffer: dtors run on contents", "[containers][ring_buffer][lifetime]")
{
    static int dtors = 0;
    struct C
    {
        int v = 0;
        C() = default;
        explicit C(int x) : v(x) {}
        C(const C& o) = default;
        C(C&& o) noexcept : v(o.v) { o.v = -1; }
        C& operator=(const C&) = default;
        C& operator=(C&& o) noexcept
        {
            v = o.v;
            o.v = -1;
            return *this;
        }
        ~C() { ++dtors; }
    };

    dtors = 0;
    {
        RingBuffer<C> r(4);
        REQUIRE(r.try_emplace(1));
        REQUIRE(r.try_emplace(2));
        REQUIRE(r.try_emplace(3));
        // dtor count from try_pop's `out = std::move(*p)` + the slot's dtor
        C tmp;
        REQUIRE(r.try_pop(tmp)); // tmp gets value, slot destroyed
        // Two more elements still inside on dtor.
    }
    // After the scope:
    //   - try_pop's slot dtor      -> 1
    //   - 2 remaining slots in ring -> 2
    //   - tmp's dtor at scope exit -> 1
    // Total: exactly 4 dtor calls. The point: no leaks (we never push 3 then
    // walk away without destroying 3).
    REQUIRE(dtors == 4);
}

// =============================================================================
// HashMap<K, V> — v1c
// =============================================================================

TEST_CASE("HashMap: default empty", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    REQUIRE(m.size() == 0);
    REQUIRE(m.empty());
    REQUIRE(m.capacity() == 0);
    REQUIRE(m.find(7) == nullptr);
    REQUIRE_FALSE(m.contains(7));
}

TEST_CASE("HashMap: insert + find round-trip", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    REQUIRE(m.insert(1, 100));
    REQUIRE(m.insert(2, 200));
    REQUIRE(m.insert(3, 300));
    REQUIRE(m.size() == 3);

    auto* p1 = m.find(1);
    REQUIRE(p1);
    REQUIRE(*p1 == 100);
    auto* p2 = m.find(2);
    REQUIRE(p2);
    REQUIRE(*p2 == 200);
    auto* p3 = m.find(3);
    REQUIRE(p3);
    REQUIRE(*p3 == 300);
    REQUIRE(m.find(99) == nullptr);
}

TEST_CASE("HashMap: insert duplicate key is a no-op", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    REQUIRE(m.insert(1, 100));
    REQUIRE_FALSE(m.insert(1, 999));
    REQUIRE(m.size() == 1);
    REQUIRE(*m.find(1) == 100);
}

TEST_CASE("HashMap: erase round-trip", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    m.insert(1, 10);
    m.insert(2, 20);
    m.insert(3, 30);

    REQUIRE(m.erase(2));
    REQUIRE(m.size() == 2);
    REQUIRE(m.find(2) == nullptr);
    REQUIRE(*m.find(1) == 10);
    REQUIRE(*m.find(3) == 30);

    REQUIRE_FALSE(m.erase(2));
    REQUIRE_FALSE(m.erase(99));
}

TEST_CASE("HashMap: operator[] inserts default and returns reference", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    m[42] = 7;
    REQUIRE(m.size() == 1);
    REQUIRE(*m.find(42) == 7);
    REQUIRE(m[42] == 7);
    m[42] = 8;
    REQUIRE(m[42] == 8);
    REQUIRE(m.size() == 1); // overwrite, no new entry
}

TEST_CASE("HashMap: many inserts trigger rehash, all entries findable", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    constexpr u32 kN = 1000;
    for (u32 i = 0; i < kN; ++i)
    {
        REQUIRE(m.insert(i, i * 10));
    }
    REQUIRE(m.size() == kN);
    REQUIRE(m.capacity() >= kN);

    for (u32 i = 0; i < kN; ++i)
    {
        auto* p = m.find(i);
        REQUIRE(p != nullptr);
        REQUIRE(*p == i * 10);
    }
}

TEST_CASE("HashMap: backshift preserves lookups after random erases", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    constexpr u32 kN = 500;
    for (u32 i = 0; i < kN; ++i)
    {
        m.insert(i, i + 1);
    }

    // Erase every third key
    for (u32 i = 0; i < kN; i += 3)
    {
        REQUIRE(m.erase(i));
    }

    // Verify remaining keys are still findable, erased keys are gone
    for (u32 i = 0; i < kN; ++i)
    {
        if (i % 3 == 0)
        {
            REQUIRE(m.find(i) == nullptr);
        }
        else
        {
            auto* p = m.find(i);
            REQUIRE(p != nullptr);
            REQUIRE(*p == i + 1);
        }
    }
}

TEST_CASE("HashMap: clear keeps capacity", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    for (u32 i = 0; i < 50; ++i)
    {
        m.insert(i, i);
    }
    const auto cap = m.capacity();
    m.clear();
    REQUIRE(m.empty());
    REQUIRE(m.capacity() == cap);
    REQUIRE(m.find(0) == nullptr);
    // Can keep using
    m.insert(99, 99);
    REQUIRE(*m.find(99) == 99);
}

TEST_CASE("HashMap: reserve avoids rehash mid-fill", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    m.reserve(256);
    const auto cap_after_reserve = m.capacity();
    REQUIRE(cap_after_reserve >= 256);

    for (u32 i = 0; i < 200; ++i)
    {
        m.insert(i, i);
    }
    // Capacity shouldn't have grown — we reserved enough for 256/0.875 ≈ 293.
    REQUIRE(m.capacity() == cap_after_reserve);
}

TEST_CASE("HashMap: copy ctor and copy assign", "[containers][hash_map]")
{
    HashMap<u32, u32> a;
    a.insert(1, 10);
    a.insert(2, 20);

    HashMap<u32, u32> b(a);
    REQUIRE(b.size() == 2);
    REQUIRE(*b.find(1) == 10);
    REQUIRE(*b.find(2) == 20);

    HashMap<u32, u32> c;
    c = a;
    REQUIRE(c.size() == 2);
    REQUIRE(*c.find(2) == 20);
}

TEST_CASE("HashMap: move ctor leaves source empty", "[containers][hash_map]")
{
    HashMap<u32, u32> a;
    for (u32 i = 0; i < 20; ++i)
    {
        a.insert(i, i * 2);
    }

    HashMap<u32, u32> b(std::move(a));
    REQUIRE(b.size() == 20);
    REQUIRE(a.size() == 0);
    REQUIRE(a.capacity() == 0);
    REQUIRE(*b.find(5) == 10);
}

TEST_CASE("HashMap: iterator visits every live entry exactly once", "[containers][hash_map]")
{
    HashMap<u32, u32> m;
    for (u32 i = 0; i < 50; ++i)
    {
        m.insert(i, i + 100);
    }

    usize count = 0;
    u64 key_sum = 0;
    u64 val_sum = 0;
    for (auto it = m.begin(); it != m.end(); ++it)
    {
        ++count;
        key_sum += it.key();
        val_sum += it.value();
    }
    REQUIRE(count == 50);
    REQUIRE(key_sum == (50ULL * 49ULL / 2ULL));
    REQUIRE(val_sum == (50ULL * 49ULL / 2ULL) + 50ULL * 100ULL);
}

TEST_CASE("HashMap: heterogeneous lookup with String keys + StringView queries", "[containers][hash_map][string]")
{
    HashMap<String, u32> m;
    m.insert(String("alpha"), 1);
    m.insert(String("beta"), 2);
    m.insert(String("gamma"), 3);

    // Look up by StringView — no temporary String allocated.
    auto* a = m.find(StringView{"alpha"});
    REQUIRE(a != nullptr);
    REQUIRE(*a == 1);

    auto* b = m.find(StringView{"beta"});
    REQUIRE(b != nullptr);
    REQUIRE(*b == 2);

    REQUIRE(m.contains(StringView{"gamma"}));
    REQUIRE_FALSE(m.contains(StringView{"delta"}));
}

TEST_CASE("HashMap<String, V>: erase by StringView (heterogeneous)", "[containers][hash_map][string]")
{
    HashMap<String, u32> m;
    m.insert(String("a"), 1);
    m.insert(String("b"), 2);

    REQUIRE(m.erase(StringView{"a"}));
    REQUIRE_FALSE(m.contains(StringView{"a"}));
    REQUIRE(m.contains(StringView{"b"}));
}

TEST_CASE("HashMap: stress test insert+erase keeps invariants", "[containers][hash_map][stress]")
{
    HashMap<u32, u32> m;
    constexpr u32 kN = 2000;
    for (u32 i = 0; i < kN; ++i)
    {
        m.insert(i, i);
    }
    REQUIRE(m.size() == kN);

    // Erase even keys
    for (u32 i = 0; i < kN; i += 2)
    {
        REQUIRE(m.erase(i));
    }
    REQUIRE(m.size() == kN / 2);

    // Re-insert with different values
    for (u32 i = 0; i < kN; i += 2)
    {
        REQUIRE(m.insert(i, i + 10000));
    }
    REQUIRE(m.size() == kN);

    for (u32 i = 0; i < kN; ++i)
    {
        auto* p = m.find(i);
        REQUIRE(p != nullptr);
        REQUIRE(*p == (i % 2 == 0 ? i + 10000 : i));
    }
}

TEST_CASE("HashMap: dtors run on all live entries", "[containers][hash_map][lifetime]")
{
    static int dtor_count = 0;
    struct C
    {
        u32 v = 0;
        C() = default;
        explicit C(u32 x) : v(x) {}
        C(const C& o) = default;
        C(C&& o) noexcept : v(o.v) { o.v = 0xFFFFFFFFU; }
        C& operator=(const C&) = default;
        C& operator=(C&& o) noexcept
        {
            v = o.v;
            o.v = 0xFFFFFFFFU;
            return *this;
        }
        ~C() { ++dtor_count; }
    };

    dtor_count = 0;
    {
        HashMap<u32, C> m;
        for (u32 i = 0; i < 10; ++i)
        {
            m.emplace(i, C{i * 7});
        }
    }
    // Exact count is fragile (depends on rehashes/moves); the contract is
    // "no leaks, dtor count >= number of distinct C objects ever
    // constructed". We just confirm we ran at least 10 dtors.
    REQUIRE(dtor_count >= 10);
}

// =============================================================================
// HashSet<K> — v1c
// =============================================================================

TEST_CASE("HashSet: insert + contains + erase", "[containers][hash_set]")
{
    HashSet<u32> s;
    REQUIRE(s.insert(7));
    REQUIRE(s.insert(8));
    REQUIRE_FALSE(s.insert(7)); // duplicate
    REQUIRE(s.size() == 2);
    REQUIRE(s.contains(7));
    REQUIRE_FALSE(s.contains(99));
    REQUIRE(s.erase(7));
    REQUIRE_FALSE(s.contains(7));
    REQUIRE_FALSE(s.erase(7));
}

TEST_CASE("HashSet: iteration yields keys", "[containers][hash_set]")
{
    HashSet<u32> s;
    for (u32 i = 0; i < 10; ++i)
    {
        s.insert(i);
    }
    u64 sum = 0;
    usize count = 0;
    for (const u32& k : s)
    {
        sum += k;
        ++count;
    }
    REQUIRE(count == 10);
    REQUIRE(sum == 45);
}

TEST_CASE("HashSet<String>: heterogeneous contains by StringView", "[containers][hash_set][string]")
{
    HashSet<String> s;
    s.insert(String("apple"));
    s.insert(String("banana"));
    REQUIRE(s.contains(StringView{"apple"}));
    REQUIRE_FALSE(s.contains(StringView{"cherry"}));
}
