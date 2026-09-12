#include <crd/containers/spsc_queue.hpp>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

using namespace crd;
using namespace crd::containers;

// ============================================================================
// SpscQueue<T> — single-threaded correctness
// ============================================================================

TEST_CASE("SpscQueue basic push/pop round-trip", "[containers][spsc_queue]")
{
    SpscQueue<u32> q(8U);
    REQUIRE(q.capacity() == 7U);
    REQUIRE(q.empty());
    REQUIRE(!q.full());

    REQUIRE(q.try_push(10U));
    REQUIRE(q.try_push(20U));
    REQUIRE(q.try_push(30U));
    REQUIRE(q.size() == 3U);

    u32 v = 0;
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 10U);
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 20U);
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 30U);

    REQUIRE(q.empty());
    REQUIRE(!q.try_pop(v));
}

TEST_CASE("SpscQueue fills to capacity then refuses", "[containers][spsc_queue]")
{
    SpscQueue<int> q(4U); // capacity() == 3

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.full());
    REQUIRE(!q.try_push(4)); // no room

    int v = 0;
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 1);
    REQUIRE(!q.full());
    REQUIRE(q.try_push(4)); // room freed
}

TEST_CASE("SpscQueue try_emplace constructs in place", "[containers][spsc_queue]")
{
    SpscQueue<std::string> q(8U);
    REQUIRE(q.try_emplace("hello"));
    REQUIRE(q.try_emplace(5U, 'x')); // std::string(5,'x') = "xxxxx"

    std::string s;
    REQUIRE(q.try_pop(s));
    REQUIRE(s == "hello");
    REQUIRE(q.try_pop(s));
    REQUIRE(s == "xxxxx");
}

TEST_CASE("SpscQueue FIFO ordering", "[containers][spsc_queue]")
{
    SpscQueue<u32> q(16U);
    for (u32 i = 0; i < 15U; ++i)
    {
        REQUIRE(q.try_push(i));
    }
    for (u32 i = 0; i < 15U; ++i)
    {
        u32 v = 0;
        REQUIRE(q.try_pop(v));
        REQUIRE(v == i);
    }
}

TEST_CASE("SpscQueue wrap-around correctness", "[containers][spsc_queue]")
{
    SpscQueue<u32> q(4U); // 3 usable slots; tests wrap-around at mask=3

    REQUIRE(q.try_push(1U));
    REQUIRE(q.try_push(2U));

    u32 v = 0;
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 1U);

    REQUIRE(q.try_push(3U));
    REQUIRE(q.try_push(4U));

    REQUIRE(q.try_pop(v));
    REQUIRE(v == 2U);
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 3U);
    REQUIRE(q.try_pop(v));
    REQUIRE(v == 4U);
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue destructor runs non-trivial elements", "[containers][spsc_queue]")
{
    static int s_dtor_count = 0;
    struct Counted
    {
        ~Counted() { ++s_dtor_count; }
    };

    s_dtor_count = 0;
    {
        SpscQueue<Counted> q(4U);
        REQUIRE(q.try_emplace());
        REQUIRE(q.try_emplace());
        // Let destructor drain them
    }
    REQUIRE(s_dtor_count == 2);
}

// ============================================================================
// SpscQueue<T> — concurrent producer / consumer
// ============================================================================

TEST_CASE("SpscQueue concurrent producer/consumer 1M items", "[containers][spsc_queue][concurrent]")
{
    constexpr u32 total = 1'000'000U;
    SpscQueue<u32> q(1024U);

    std::thread producer([&]
    {
        for (u32 i = 0; i < total; ++i)
        {
            while (!q.try_push(i))
            {
                // spin until space
            }
        }
    });

    u64 sum_actual   = 0;
    u64 sum_expected = 0;
    for (u32 i = 0; i < total; ++i)
    {
        sum_expected += i;
    }

    for (u32 consumed = 0; consumed < total; )
    {
        u32 v = 0;
        if (q.try_pop(v))
        {
            sum_actual += v;
            ++consumed;
        }
    }

    producer.join();
    REQUIRE(sum_actual == sum_expected);
}
