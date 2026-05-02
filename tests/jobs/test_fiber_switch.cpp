#include <catch2/catch_test_macros.hpp>
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/types.hpp>
#include <memory>

using crd::jobs::detail::FiberContext;
using crd::jobs::detail::fiber_switch;
using crd::jobs::detail::fiber_init_stack;

// ---------------------------------------------------------------------------
// Test 1: basic round-trip
// ---------------------------------------------------------------------------

static FiberContext g_main_ctx_1;
static FiberContext g_fiber_ctx_1;
static bool g_fiber_ran_1 = false;
static int  g_fiber_value_1 = 0;

static void fiber_entry_1()
{
    g_fiber_ran_1  = true;
    g_fiber_value_1 = 42;
    fiber_switch(&g_fiber_ctx_1, &g_main_ctx_1);
}

TEST_CASE("fiber_switch: basic round trip", "[jobs][fiber]")
{
    g_fiber_ran_1  = false;
    g_fiber_value_1 = 0;

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack = std::make_unique<crd::u8[]>(kStackSize);

    fiber_init_stack(g_fiber_ctx_1, stack.get(), kStackSize, fiber_entry_1);
    fiber_switch(&g_main_ctx_1, &g_fiber_ctx_1);

    REQUIRE(g_fiber_ran_1);
    REQUIRE(g_fiber_value_1 == 42);
}

// ---------------------------------------------------------------------------
// Test 2: multiple re-entries (switch back and forth 3 times)
// ---------------------------------------------------------------------------

static FiberContext g_main_ctx_2;
static FiberContext g_fiber_ctx_2;
static int g_reentry_count = 0;

static void fiber_entry_2()
{
    ++g_reentry_count;                          // = 1
    fiber_switch(&g_fiber_ctx_2, &g_main_ctx_2);

    ++g_reentry_count;                          // = 2
    fiber_switch(&g_fiber_ctx_2, &g_main_ctx_2);

    ++g_reentry_count;                          // = 3
    fiber_switch(&g_fiber_ctx_2, &g_main_ctx_2);
}

TEST_CASE("fiber_switch: multiple re-entries", "[jobs][fiber]")
{
    g_reentry_count = 0;

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack = std::make_unique<crd::u8[]>(kStackSize);

    fiber_init_stack(g_fiber_ctx_2, stack.get(), kStackSize, fiber_entry_2);

    fiber_switch(&g_main_ctx_2, &g_fiber_ctx_2);
    REQUIRE(g_reentry_count == 1);

    fiber_switch(&g_main_ctx_2, &g_fiber_ctx_2);
    REQUIRE(g_reentry_count == 2);

    fiber_switch(&g_main_ctx_2, &g_fiber_ctx_2);
    REQUIRE(g_reentry_count == 3);
}

// ---------------------------------------------------------------------------
// Test 3: stack-local data survives the switch
//   Verifies that the fiber's own stack frame is preserved between switches.
// ---------------------------------------------------------------------------

static FiberContext g_main_ctx_3;
static FiberContext g_fiber_ctx_3;
static int g_fiber_local_result = 0;

static void fiber_entry_3()
{
    // Local variable lives on the fiber's stack.
    volatile int local = 100;
    fiber_switch(&g_fiber_ctx_3, &g_main_ctx_3);

    // After re-entry: stack frame must still be intact.
    local += 23;
    g_fiber_local_result = local;
    fiber_switch(&g_fiber_ctx_3, &g_main_ctx_3);
}

TEST_CASE("fiber_switch: fiber stack-local data survives switch", "[jobs][fiber]")
{
    g_fiber_local_result = 0;

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack = std::make_unique<crd::u8[]>(kStackSize);

    fiber_init_stack(g_fiber_ctx_3, stack.get(), kStackSize, fiber_entry_3);

    fiber_switch(&g_main_ctx_3, &g_fiber_ctx_3);  // fiber runs to first switch
    REQUIRE(g_fiber_local_result == 0);            // not yet set

    fiber_switch(&g_main_ctx_3, &g_fiber_ctx_3);  // fiber resumes, sets result
    REQUIRE(g_fiber_local_result == 123);
}

// ---------------------------------------------------------------------------
// Test 4: caller-side locals survive a fiber switch
//   Verifies that the main context's callee-saved registers are restored.
//   If RBX/RBP/R12-R15 are corrupted, the sentinel value or 'done' flag
//   will not match, and the test will fail (or crash before the CHECK).
// ---------------------------------------------------------------------------

TEST_CASE("fiber_switch: caller callee-saved regs restored", "[jobs][fiber]")
{
    static FiberContext mc;
    static FiberContext fc;
    static bool done = false;

    struct LocalFiber
    {
        static void entry()
        {
            done = true;
            fiber_switch(&fc, &mc);
        }
    };

    done = false;

    // 'sentinel' is likely held in a callee-saved register across the switch.
    const int sentinel = 0x600D'C0DE;
    volatile const int& ref = sentinel;

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack = std::make_unique<crd::u8[]>(kStackSize);

    fiber_init_stack(fc, stack.get(), kStackSize, LocalFiber::entry);
    fiber_switch(&mc, &fc);

    REQUIRE(done);
    REQUIRE(ref == 0x600D'C0DE);
}

// ---------------------------------------------------------------------------
// Test 5: two independent fibers, each with their own context
// ---------------------------------------------------------------------------

static FiberContext g_main_ctx_5;
static FiberContext g_fiber_ctx_5a;
static FiberContext g_fiber_ctx_5b;
static int g_order_5 = 0;

static void fiber_entry_5a()
{
    g_order_5 = 1;
    fiber_switch(&g_fiber_ctx_5a, &g_main_ctx_5);
}

static void fiber_entry_5b()
{
    g_order_5 = 2;
    fiber_switch(&g_fiber_ctx_5b, &g_main_ctx_5);
}

TEST_CASE("fiber_switch: two independent fibers", "[jobs][fiber]")
{
    g_order_5 = 0;

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack_a = std::make_unique<crd::u8[]>(kStackSize);
    auto stack_b = std::make_unique<crd::u8[]>(kStackSize);

    fiber_init_stack(g_fiber_ctx_5a, stack_a.get(), kStackSize, fiber_entry_5a);
    fiber_init_stack(g_fiber_ctx_5b, stack_b.get(), kStackSize, fiber_entry_5b);

    fiber_switch(&g_main_ctx_5, &g_fiber_ctx_5a);
    REQUIRE(g_order_5 == 1);

    fiber_switch(&g_main_ctx_5, &g_fiber_ctx_5b);
    REQUIRE(g_order_5 == 2);
}
