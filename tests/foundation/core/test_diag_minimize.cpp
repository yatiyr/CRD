// DIAG.9c -- the shared minimizer (crd/core/minimize.hpp). Each reproduction here is a predicate written in the test,
// so every expected result is known in advance and checked against the predicate itself, never against the helper:
// the 1-minimal subsequence of a failure that needs two elements, an input that does not reproduce left as given, a
// race (a candidate that fails on some runs only) refused by the all-K-runs oracle, a run budget that stops early
// with a result that still reproduces, and integers shrunk toward 0 in both signs. ASCII test names (ctest by-name).

#include <crd/core/minimize.hpp>

#include <crd/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace
{
using crd::i64;
using crd::u64;
using crd::usize;
using crd::containers::Array;
namespace mz = crd::minimize;

Array<int> items_of(std::initializer_list<int> values)
{
    Array<int> out;
    for (const int v : values)
    {
        out.push_back(v);
    }
    return out;
}

bool contains(const Array<int>& items, int v)
{
    return std::ranges::any_of(items, [v](int x) { return x == v; });
}

bool same(const Array<int>& a, std::initializer_list<int> b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    usize i = 0U;
    for (const int v : b)
    {
        if (a[i] != v)
        {
            return false;
        }
        ++i;
    }
    return true;
}

// The failure: the candidate holds both 3 and 7.
bool needs_3_and_7(const Array<int>& c)
{
    return contains(c, 3) && contains(c, 7);
}
} // namespace

TEST_CASE("diag 9c minimize: a failure that needs two elements shrinks to exactly those two, 1-minimal",
          "[core][diag9c][minimize]")
{
    Array<int>  items = items_of({1, 2, 3, 4, 5, 6, 7, 8});
    mz::Budget  budget;
    mz::Stats   stats;
    u64         calls    = 0U;
    const auto  run_once = [&calls](const Array<int>& c)
    {
        ++calls;
        return needs_3_and_7(c);
    };
    const mz::Status s = mz::minimize_sequence(items, run_once, budget, stats);
    CHECK(s == mz::Status::Minimal);
    CHECK(same(items, {3, 7}));
    CHECK(stats.runs == calls);
    CHECK(stats.flaky == 0U);
    CHECK(stats.reproduced >= 7U); // the input and the six drops
    // Independently of the helper: the result reproduces and dropping either element does not.
    CHECK(needs_3_and_7(items));
    for (usize i = 0U; i < items.size(); ++i)
    {
        Array<int> dropped(items);
        dropped.erase(i);
        CHECK_FALSE(needs_3_and_7(dropped));
    }
}

TEST_CASE("diag 9c minimize: an input that does not reproduce is left as given", "[core][diag9c][minimize]")
{
    Array<int>       items = items_of({1, 2, 3});
    mz::Stats        stats;
    const mz::Status s = mz::minimize_sequence(items, needs_3_and_7, mz::Budget{}, stats);
    CHECK(s == mz::Status::NotReproduced);
    CHECK(same(items, {1, 2, 3}));
    CHECK(stats.candidates == 1U);
    CHECK(stats.runs == 1U);

    i64        value = 5;
    mz::Stats  zstats;
    const auto at_least_100 = [](i64 v)
    {
        return v >= 100;
    };
    CHECK(mz::minimize_toward_zero(value, at_least_100, mz::Budget{}, zstats) == mz::Status::NotReproduced);
    CHECK(value == 5);
}

TEST_CASE("diag 9c minimize: a candidate that fails on some runs only is a race, not a reproduction",
          "[core][diag9c][minimize]")
{
    // Without 7 the failure appears on two calls in every three: a race. With 7 it is certain. Any three consecutive
    // runs of a candidate without 7 therefore include one that does not fail, so the all-K oracle (K = 3) never keeps
    // such a candidate, however lucky its first runs are.
    Array<int> items = items_of({1, 2, 3, 4, 5, 6, 7, 8});
    u64        calls = 0U;
    const auto racy  = [&calls](const Array<int>& c)
    {
        const u64 call = calls++;
        return contains(c, 3) && (contains(c, 7) || call % 3U != 2U);
    };
    mz::Budget budget;
    budget.runs_per_candidate = 3U;
    mz::Stats        stats;
    const mz::Status s = mz::minimize_sequence(items, racy, budget, stats);
    CHECK(s == mz::Status::Minimal);
    CHECK(same(items, {3, 7}));
    CHECK(stats.flaky >= 1U);
    CHECK(stats.runs == calls);
}

TEST_CASE("diag 9c minimize: a run budget stops early with a result that still reproduces", "[core][diag9c][minimize]")
{
    Array<int> items = items_of({1, 2, 3, 4, 5, 6, 7, 8});
    mz::Budget budget;
    budget.runs_per_candidate = 2U;
    budget.max_runs           = 7U; // the input (2 runs) and two more candidates, never a candidate on one run
    mz::Stats        stats;
    const mz::Status s = mz::minimize_sequence(items, needs_3_and_7, budget, stats);
    CHECK(s == mz::Status::Budget);
    CHECK(stats.runs <= budget.max_runs);
    CHECK(needs_3_and_7(items));
    CHECK(items.size() > 2U);

    // A budget smaller than K judges nothing at all.
    Array<int> untouched = items_of({3, 7, 9});
    mz::Budget tiny;
    tiny.runs_per_candidate = 3U;
    tiny.max_runs           = 2U;
    mz::Stats nothing;
    CHECK(mz::minimize_sequence(untouched, needs_3_and_7, tiny, nothing) == mz::Status::Budget);
    CHECK(same(untouched, {3, 7, 9}));
    CHECK(nothing.runs == 0U);
}

TEST_CASE("diag 9c minimize: integers shrink toward 0 to the last reproducing value in both signs",
          "[core][diag9c][minimize]")
{
    struct Case
    {
        i64  start;
        i64  expected;
        bool (*fails)(i64);
    };
    const Case cases[] = {
        {1000, 100, [](i64 v) { return v >= 100; }},
        {-5000, -37, [](i64 v) { return v <= -37; }},
        {7, 1, [](i64 v) { return v != 0; }},
        {12345, 0, [](i64 /*v*/) { return true; }},
        {std::numeric_limits<i64>::min(), -11, [](i64 v) { return v < -10; }},
        {std::numeric_limits<i64>::max(), 4096, [](i64 v) { return v >= 4096; }},
    };
    for (const Case& c : cases)
    {
        INFO(c.start);
        i64              value = c.start;
        mz::Stats        stats;
        const auto       run_once = [&c](i64 v) { return c.fails(v); };
        const mz::Status s        = mz::minimize_toward_zero(value, run_once, mz::Budget{}, stats);
        CHECK(s == mz::Status::Minimal);
        CHECK(value == c.expected);
        // Independently: it reproduces, and so does neither 0 nor the value one step closer to 0.
        CHECK(c.fails(value));
        if (value != 0)
        {
            CHECK_FALSE(c.fails(0));
            CHECK_FALSE(c.fails(value > 0 ? value - 1 : value + 1));
        }
        CHECK(stats.candidates <= 2U + 64U); // the start, 0 and at most one probe per magnitude bit
    }
}
