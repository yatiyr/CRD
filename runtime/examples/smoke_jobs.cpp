#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cstdio>
#include <memory>

using crd::jobs::detail::FiberContext;
using crd::jobs::detail::fiber_switch;
using crd::jobs::detail::fiber_init_stack;

// ---------------------------------------------------------------------------
// Fiber 1: single switch back to main
// ---------------------------------------------------------------------------

static FiberContext g_main_ctx;
static FiberContext g_fiber1_ctx;
static bool        g_fiber1_ran   = false;
static int         g_fiber1_value = 0;

static void fiber1_entry()
{
    std::printf("[smoke_jobs] fiber1 running\n");
    g_fiber1_ran   = true;
    g_fiber1_value = 40 + 2;
    fiber_switch(&g_fiber1_ctx, &g_main_ctx);
}

// ---------------------------------------------------------------------------
// Fiber 2: multiple re-entries carrying stack-local state
// ---------------------------------------------------------------------------

static FiberContext g_fiber2_ctx;
static int         g_fiber2_phase = 0;

static void fiber2_entry()
{
    volatile int local = 10;

    g_fiber2_phase = 1;
    std::printf("[smoke_jobs] fiber2 phase 1 (local=%d)\n", static_cast<int>(local));
    fiber_switch(&g_fiber2_ctx, &g_main_ctx);

    local += 5;
    g_fiber2_phase = 2;
    std::printf("[smoke_jobs] fiber2 phase 2 (local=%d)\n", static_cast<int>(local));
    fiber_switch(&g_fiber2_ctx, &g_main_ctx);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    std::printf("=== smoke_jobs (crd-jobs v1a — fiber context switch) ===\n");

    constexpr crd::usize kStackSize = 64u * 1024u;  // 64 KB

    // --- Test 1: basic round trip ---
    {
        auto stack = std::make_unique<crd::u8[]>(kStackSize);
        fiber_init_stack(g_fiber1_ctx, stack.get(), kStackSize, fiber1_entry);

        std::printf("[smoke_jobs] switching to fiber1...\n");
        fiber_switch(&g_main_ctx, &g_fiber1_ctx);
        std::printf("[smoke_jobs] returned from fiber1\n");

        CRD_ASSERT(g_fiber1_ran);
        CRD_ASSERT(g_fiber1_value == 42);
        std::printf("[smoke_jobs] fiber1 value = %d  [expected 42]\n", g_fiber1_value);
    }

    // --- Test 2: multiple re-entries ---
    {
        auto stack = std::make_unique<crd::u8[]>(kStackSize);
        fiber_init_stack(g_fiber2_ctx, stack.get(), kStackSize, fiber2_entry);

        std::printf("[smoke_jobs] switching to fiber2 (phase 1)...\n");
        fiber_switch(&g_main_ctx, &g_fiber2_ctx);
        CRD_ASSERT(g_fiber2_phase == 1);

        std::printf("[smoke_jobs] switching to fiber2 (phase 2)...\n");
        fiber_switch(&g_main_ctx, &g_fiber2_ctx);
        CRD_ASSERT(g_fiber2_phase == 2);

        std::printf("[smoke_jobs] fiber2 completed all phases\n");
    }

    const bool ok = g_fiber1_ran && (g_fiber1_value == 42) && (g_fiber2_phase == 2);
    std::printf("[smoke_jobs] result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
