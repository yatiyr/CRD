// DIAG.4c -- wait-graph parked-fiber evidence. Verifies crd::jobs::wait_graph_snapshot reports a fiber that is
// blocked inside wait(), identifying the counter it waits on by task id. The snapshot is asserted only while
// the pool is DETERMINISTICALLY parked: a child job spins on a test-held gate so it never decrements its
// counter, which keeps the root fiber parked on that counter for as long as the test needs.
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) -- job callbacks are plain function pointers,
// so the gate and the recorded child id must be reachable through globals.
std::atomic<bool>     g_gate{false};       // closed until the test opens it; keeps the child (and thus the root) parked
std::atomic<crd::u64> g_child_task{0};     // the child records its own task id so the test can match the wait edge
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void gated_child(void* /*data*/) noexcept
{
    g_child_task.store(crd::jobs::current_task_id(), std::memory_order_relaxed);
    while (!g_gate.load(std::memory_order_acquire))
        std::this_thread::yield(); // spin, never decrement, until the test opens the gate
}

void parking_root(void* /*data*/) noexcept
{
    crd::jobs::JobDecl child{};
    child.fn = &gated_child;
    crd::jobs::run_and_wait(child); // parks this fiber on the child's counter until the gate opens
}
} // namespace

TEST_CASE("wait_graph: a parked fiber is reported waiting on its child's counter", "[jobs][diag][waitgraph]")
{
    g_gate.store(false, std::memory_order_relaxed);
    g_child_task.store(0, std::memory_order_relaxed);

    crd::jobs::Config cfg;
    cfg.num_threads = 4U; // >=2 so the root parks on a worker while the child runs on another
    crd::jobs::init(cfg);

    std::array<crd::jobs::WaitGraphNode, 32> nodes{};

    // Nothing in flight yet: the wait graph is empty.
    CHECK(crd::jobs::wait_graph_snapshot(nodes) == 0U);

    crd::jobs::JobDecl root{};
    root.fn                    = &parking_root;
    crd::jobs::Counter* handle = crd::jobs::run(root);

    // Poll until the root fiber has parked on the child's counter (child is spinning on the closed gate).
    // Bounded so a regression surfaces as a failure, never a hang.
    crd::usize parked   = 0U;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        parked = crd::jobs::wait_graph_snapshot(nodes);
        if (parked >= 1U && g_child_task.load(std::memory_order_relaxed) != 0U)
            break;
        std::this_thread::yield();
    }

    // Exactly the root fiber is parked -- the main thread waits on the non-fiber spin path, so it is not a node.
    REQUIRE(parked == 1U);
    const crd::u64 child_task = g_child_task.load(std::memory_order_relaxed);
    REQUIRE(child_task != 0U);
    CHECK(nodes[0].waiting_on_task_id == child_task);    // the edge points at the child's counter
    CHECK(nodes[0].waiting_on_parent_task_id != 0U);     // the child was submitted from the root fiber (id != 0)
    CHECK(nodes[0].waiting_on_remaining == 1U);          // one outstanding job -- the still-spinning child

    // Open the gate: the child completes, the root resumes, and everything drains.
    g_gate.store(true, std::memory_order_release);
    crd::jobs::wait(handle);

    // Drained: no fiber is parked, and the resumed root cleared its own wait-graph edge.
    CHECK(crd::jobs::wait_graph_snapshot(nodes) == 0U);

    crd::jobs::shutdown();
}

TEST_CASE("wait_graph: snapshot truncates but still returns the true parked count", "[jobs][diag][waitgraph]")
{
    // With an out span smaller than the number of parked fibers, the return value is the TRUE total (so a
    // caller can detect truncation and re-query with a larger buffer), while only out.size() nodes are written.
    g_gate.store(false, std::memory_order_relaxed);

    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    constexpr int kParkers = 3;
    // Three independent roots, each parking on its own gated child, give three wait-graph nodes; the shared
    // spin gate keeps all three children (and thus all three roots) parked at once.
    std::array<crd::jobs::Counter*, kParkers> handles{};
    for (int i = 0; i < kParkers; ++i)
    {
        crd::jobs::JobDecl root{};
        root.fn                              = &parking_root;
        handles[static_cast<std::size_t>(i)] = crd::jobs::run(root);
    }

    // Wait until all three roots are parked.
    std::array<crd::jobs::WaitGraphNode, 8> full{};
    crd::usize total    = 0U;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        total = crd::jobs::wait_graph_snapshot(full);
        if (total >= static_cast<crd::usize>(kParkers))
            break;
        std::this_thread::yield();
    }
    REQUIRE(total == static_cast<crd::usize>(kParkers));

    // A one-slot buffer: return value is still the true total; exactly one node written.
    std::array<crd::jobs::WaitGraphNode, 1> tiny{};
    const crd::usize truncated = crd::jobs::wait_graph_snapshot(tiny);
    CHECK(truncated == static_cast<crd::usize>(kParkers)); // honest total despite truncation
    CHECK(tiny[0].waiting_on_remaining == 1U);            // the one written node is a real parked edge

    g_gate.store(true, std::memory_order_release);
    for (crd::jobs::Counter* h : handles)
        crd::jobs::wait(h);

    crd::jobs::shutdown();
}
