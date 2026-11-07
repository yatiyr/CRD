// crd-net -- the state-hash exchange (NET.3; docs/design/network-foundation.md#state-hashes).

#include <crd/containers/checksum.hpp>
#include <crd/net/state_hash.hpp>
#include <crd/net/wire/session_control.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::net::state_hash_chain;
using crd::net::StateHashExchange;
using crd::net::StateHashReport;
using crd::net::StateHashVerdict;

struct Peer
{
    StateHashReport local[4];
    StateHashReport ahead[2];
    StateHashExchange exchange{local, ahead};
};

// A second peer's report of `step`, as its own record would have chained it.
struct Writer
{
    StateHashReport local[1];
    StateHashReport ahead[1];
    StateHashExchange exchange{local, ahead};

    StateHashReport report(u64 step, u64 hash)
    {
        StateHashReport out;
        REQUIRE(exchange.record(step, hash, out) == StateHashVerdict::Waiting);
        return out;
    }
};

u64 h(u64 step)
{
    return (step * 0x9E3779B97F4A7C15ULL) ^ 0x55U;
}
} // namespace

TEST_CASE("State hashes: the chain is the hash of the step and hash, seeded with the one before", "[net][state-hash]")
{
    const u8 bytes[16] = {0x01U, 0x02U, 0U, 0U, 0U, 0U, 0U, 0U, 0xEFU, 0xCDU, 0xABU, 0x89U, 0x67U, 0x45U, 0x23U, 0x01U};
    const u64 expected = crd::containers::xxh3_64(ConstSpan<u8>(bytes, sizeof(bytes)), 77U);
    CHECK(state_hash_chain(77U, 0x0201U, 0x0123456789ABCDEFULL) == expected);
    CHECK(state_hash_chain(78U, 0x0201U, 0x0123456789ABCDEFULL) != expected);
    CHECK(state_hash_chain(77U, 0x0202U, 0x0123456789ABCDEFULL) != expected);
    CHECK(state_hash_chain(77U, 0x0201U, 0x0123456789ABCDEEULL) != expected);
}

TEST_CASE("State hashes: a record chains and fills what StateHash carries", "[net][state-hash]")
{
    Peer peer;
    StateHashReport first;
    REQUIRE(peer.exchange.record(5U, h(5U), first) == StateHashVerdict::Waiting);
    CHECK(first.step == 5U);
    CHECK(first.hash == h(5U));
    CHECK(first.chain == state_hash_chain(0U, 5U, h(5U)));
    StateHashReport second;
    REQUIRE(peer.exchange.record(7U, h(7U), second) == StateHashVerdict::Waiting);
    CHECK(second.chain == state_hash_chain(first.chain, 7U, h(7U)));

    // A step that does not rise changes nothing and fills nothing.
    StateHashReport untouched;
    untouched.step = 99U;
    CHECK(peer.exchange.record(7U, 1U, untouched) == StateHashVerdict::Stale);
    CHECK(peer.exchange.record(6U, 1U, untouched) == StateHashVerdict::Stale);
    CHECK(untouched.step == 99U);
    StateHashReport third;
    REQUIRE(peer.exchange.record(8U, h(8U), third) == StateHashVerdict::Waiting);
    CHECK(third.chain == state_hash_chain(second.chain, 8U, h(8U)));

    // What StateHash carries is exactly this.
    crd::net::wire::session_control::StateHash message;
    message.step = third.step;
    message.hash = third.hash;
    message.chain = third.chain;
    u8 out[64];
    usize written = 0U;
    REQUIRE(crd::net::wire::encode(message, 0U, out, written) == crd::net::wire::WireError::Ok);
    crd::net::wire::session_control::StateHash read;
    REQUIRE(crd::net::wire::decode(ConstSpan<u8>(out, written), 0U, read) == crd::net::wire::WireError::Ok);
    CHECK(read.step == 8U);
    CHECK(read.chain == third.chain);
}

TEST_CASE("State hashes: peers agree in either order", "[net][state-hash]")
{
    StateHashReport a_local[8];
    StateHashReport a_ahead[8];
    StateHashExchange a{a_local, a_ahead};
    StateHashReport b_local[8];
    StateHashReport b_ahead[8];
    StateHashExchange b{b_local, b_ahead};

    // A runs ahead of B, B's reports find A's records at once, and A's wait for B.
    StateHashReport from_a[4];
    StateHashReport from_b[4];
    for (u64 step = 0U; step < 4U; ++step)
    {
        REQUIRE(a.record(step + 10U, h(step), from_a[step]) == StateHashVerdict::Waiting);
    }
    for (u64 step = 0U; step < 4U; ++step)
    {
        CHECK(b.on_report(from_a[step]) == StateHashVerdict::Waiting);
    }
    for (u64 step = 0U; step < 4U; ++step)
    {
        CHECK(b.record(step + 10U, h(step), from_b[step]) == StateHashVerdict::Agreed);
        CHECK(a.on_report(from_b[step]) == StateHashVerdict::Agreed);
        CHECK(from_b[step].chain == from_a[step].chain);
    }
    CHECK_FALSE(a.diverged());
    CHECK_FALSE(b.diverged());
    CHECK(a.divergence().any_agreed);
    CHECK(a.divergence().agreed_through == 13U);
    CHECK(b.divergence().agreed_through == 13U);
    CHECK(b.dropped() == 0U);
}

TEST_CASE("State hashes: the first difference is named once", "[net][state-hash]")
{
    SECTION("the hashes differ at the step reported")
    {
        Peer peer;
        Writer other;
        StateHashReport mine;
        REQUIRE(peer.exchange.record(1U, h(1U), mine) == StateHashVerdict::Waiting);
        REQUIRE(peer.exchange.record(2U, h(2U), mine) == StateHashVerdict::Waiting);
        CHECK(peer.exchange.on_report(other.report(1U, h(1U))) == StateHashVerdict::Agreed);
        const StateHashReport wrong = other.report(2U, h(2U) ^ 1U);
        CHECK(peer.exchange.on_report(wrong) == StateHashVerdict::Diverged);
        CHECK(peer.exchange.diverged());
        const crd::net::StateHashDivergence& found = peer.exchange.divergence();
        CHECK(found.any_agreed);
        CHECK(found.agreed_through == 1U);
        CHECK(found.step == 2U);
        CHECK(found.at_this_step);
        CHECK(found.local_hash == h(2U));
        CHECK(found.peer_hash == (h(2U) ^ 1U));

        // Said once: nothing is compared after it, and records still give what the peer needs to see it too.
        CHECK(peer.exchange.on_report(other.report(3U, h(3U))) == StateHashVerdict::AlreadyDiverged);
        StateHashReport later;
        later.step = 0U;
        CHECK(peer.exchange.record(3U, h(3U), later) == StateHashVerdict::AlreadyDiverged);
        CHECK(later.step == 3U);
        CHECK(peer.exchange.divergence().step == 2U);
    }

    SECTION("only the chain differs: a step between two reports did")
    {
        Peer peer;
        Writer other;
        StateHashReport mine;
        for (u64 step = 1U; step <= 3U; ++step)
        {
            REQUIRE(peer.exchange.record(step, h(step), mine) == StateHashVerdict::Waiting);
        }
        CHECK(peer.exchange.on_report(other.report(1U, h(1U))) == StateHashVerdict::Agreed);
        StateHashReport skipped = other.report(2U, h(2U) ^ 1U); // never sent
        CHECK(skipped.step == 2U);
        CHECK(peer.exchange.on_report(other.report(3U, h(3U))) == StateHashVerdict::Diverged);
        CHECK_FALSE(peer.exchange.divergence().at_this_step);
        CHECK(peer.exchange.divergence().step == 3U);
        CHECK(peer.exchange.divergence().agreed_through == 1U);
    }

    SECTION("a report waiting for this side's step shows it at the record")
    {
        Peer peer;
        Writer other;
        CHECK(peer.exchange.on_report(other.report(1U, h(1U) ^ 1U)) == StateHashVerdict::Waiting);
        StateHashReport mine;
        CHECK(peer.exchange.record(1U, h(1U), mine) == StateHashVerdict::Diverged);
        CHECK_FALSE(peer.exchange.divergence().any_agreed);
        CHECK(peer.exchange.divergence().step == 1U);
    }

    SECTION("a restart forgets everything, chains included")
    {
        Peer peer;
        Writer other;
        StateHashReport mine;
        REQUIRE(peer.exchange.record(1U, h(1U), mine) == StateHashVerdict::Waiting);
        CHECK(peer.exchange.on_report(other.report(1U, h(1U) ^ 1U)) == StateHashVerdict::Diverged);
        peer.exchange.restart();
        CHECK_FALSE(peer.exchange.diverged());
        CHECK_FALSE(peer.exchange.divergence().any_agreed);
        // The same step counts again, and the chain starts from zero.
        REQUIRE(peer.exchange.record(1U, h(1U), mine) == StateHashVerdict::Waiting);
        CHECK(mine.chain == state_hash_chain(0U, 1U, h(1U)));
        StateHashReport fresh;
        fresh.step = 1U;
        fresh.hash = h(1U);
        fresh.chain = mine.chain;
        CHECK(peer.exchange.on_report(fresh) == StateHashVerdict::Agreed);
    }
}

TEST_CASE("State hashes: repeats, old steps and a peer far ahead cost nothing", "[net][state-hash]")
{
    Peer peer;
    Writer other;
    StateHashReport mine;
    StateHashReport reports[8];
    for (u64 step = 1U; step <= 8U; ++step)
    {
        reports[step - 1U] = other.report(step, h(step));
    }

    // Records 1 to 6: the ring of four holds 3 to 6.
    for (u64 step = 1U; step <= 6U; ++step)
    {
        REQUIRE(peer.exchange.record(step, h(step), mine) == StateHashVerdict::Waiting);
    }
    CHECK(peer.exchange.on_report(reports[1]) == StateHashVerdict::NotHeld); // given up
    CHECK(peer.exchange.on_report(reports[1]) == StateHashVerdict::Stale);   // a repeat
    CHECK(peer.exchange.on_report(reports[0]) == StateHashVerdict::Stale);   // going back
    CHECK_FALSE(peer.exchange.divergence().any_agreed);
    CHECK(peer.exchange.on_report(reports[3]) == StateHashVerdict::Agreed);
    CHECK(peer.exchange.divergence().agreed_through == 4U);
    CHECK_FALSE(peer.exchange.diverged());

    // A step this side never recorded inside what it holds: not held, and a later chain says whether that matters.
    Peer sparse;
    REQUIRE(sparse.exchange.record(1U, h(1U), mine) == StateHashVerdict::Waiting);
    REQUIRE(sparse.exchange.record(3U, h(3U), mine) == StateHashVerdict::Waiting);
    CHECK(sparse.exchange.on_report(reports[1]) == StateHashVerdict::NotHeld);
    CHECK(sparse.exchange.on_report(reports[2]) == StateHashVerdict::Diverged);
    CHECK_FALSE(sparse.exchange.divergence().at_this_step);

    // Far ahead: two reports wait, the oldest gives way, and what is left is compared as this side reaches it.
    Peer behind;
    CHECK(behind.exchange.on_report(reports[0]) == StateHashVerdict::Waiting);
    CHECK(behind.exchange.on_report(reports[1]) == StateHashVerdict::Waiting);
    CHECK(behind.exchange.on_report(reports[2]) == StateHashVerdict::Waiting);
    CHECK(behind.exchange.dropped() == 1U);
    CHECK(behind.exchange.record(1U, h(1U), mine) == StateHashVerdict::Waiting); // its report was dropped
    CHECK(behind.exchange.record(2U, h(2U), mine) == StateHashVerdict::Agreed);
    CHECK(behind.exchange.record(3U, h(3U), mine) == StateHashVerdict::Agreed);
    CHECK(behind.exchange.divergence().agreed_through == 3U);

    // A report ahead of this side's record waits for it, also once it has records.
    Peer started;
    REQUIRE(started.exchange.record(1U, h(1U), mine) == StateHashVerdict::Waiting);
    CHECK(started.exchange.on_report(reports[0]) == StateHashVerdict::Agreed);
    CHECK(started.exchange.on_report(reports[1]) == StateHashVerdict::Waiting);
    CHECK(started.exchange.record(2U, h(2U), mine) == StateHashVerdict::Agreed);

    // A record that passes several waiting reports at once answers for the newest: the older one was given up.
    Peer leaps;
    CHECK(leaps.exchange.on_report(reports[0]) == StateHashVerdict::Waiting);
    CHECK(leaps.exchange.on_report(reports[1]) == StateHashVerdict::Waiting);
    CHECK(leaps.exchange.record(2U, 0U, mine) == StateHashVerdict::Diverged); // step 1 not held, step 2 differs
    CHECK(leaps.exchange.divergence().step == 2U);
    Peer leaps_on;
    Writer lone;
    const StateHashReport only = lone.report(1U, h(1U));
    CHECK(leaps_on.exchange.on_report(only) == StateHashVerdict::Waiting);
    CHECK(leaps_on.exchange.record(2U, h(2U), mine) == StateHashVerdict::NotHeld);
}

TEST_CASE("State hashes: what each ring holds past its wrap", "[net][state-hash]")
{
    constexpr u64 steps = 12U;
    // The chain of an honest peer, step by step.
    u64 chains[steps + 1U] = {};
    for (u64 step = 1U; step <= steps; ++step)
    {
        chains[step] = state_hash_chain(chains[step - 1U], step, h(step));
    }
    const auto report_of = [&](u64 step)
    {
        return StateHashReport{step, h(step), chains[step]};
    };

    for (const usize size : {usize{1}, usize{3}, usize{4}, usize{5}, usize{7}})
    {
        StateHashReport local[8];
        StateHashReport ahead[8];
        const crd::containers::Span<StateHashReport> local_ring(local, size);
        const crd::containers::Span<StateHashReport> ahead_ring(ahead, size);

        // Every step, against records 1 to 12: the newest `size` agree, and anything older is given up.
        for (u64 step = 1U; step <= steps; ++step)
        {
            StateHashExchange exchange(local_ring, ahead_ring);
            StateHashReport mine;
            for (u64 record = 1U; record <= steps; ++record)
            {
                REQUIRE(exchange.record(record, h(record), mine) == StateHashVerdict::Waiting);
                CHECK(mine.chain == chains[record]);
            }
            const bool held = step + size > steps;
            CHECK(exchange.on_report(report_of(step)) == (held ? StateHashVerdict::Agreed : StateHashVerdict::NotHeld));
            CHECK_FALSE(exchange.diverged());
        }

        // Reports overflowing the ring that waits: the newest `size` of them are compared, each as this side reaches
        // it.
        StateHashExchange exchange(local_ring, ahead_ring);
        for (u64 step = 1U; step <= steps; ++step)
        {
            CHECK(exchange.on_report(report_of(step)) == StateHashVerdict::Waiting);
        }
        CHECK(exchange.dropped() == steps - size);
        StateHashReport mine;
        for (u64 step = 1U; step <= steps; ++step)
        {
            const bool waiting = step + size > steps;
            CHECK(exchange.record(step, h(step), mine) ==
                  (waiting ? StateHashVerdict::Agreed : StateHashVerdict::Waiting));
        }
        CHECK(exchange.divergence().agreed_through == steps);

        // A restart keeps the ring working from wherever it stood, and step zero counts again.
        exchange.restart();
        CHECK(exchange.dropped() == 0U);
        CHECK_FALSE(exchange.divergence().any_agreed);
        StateHashReport zero;
        zero.step = 0U;
        zero.hash = 5U;
        zero.chain = state_hash_chain(0U, 0U, 5U);
        CHECK(exchange.on_report(zero) == StateHashVerdict::Waiting);
        CHECK(exchange.record(0U, 5U, mine) == StateHashVerdict::Agreed);
    }
}
