// HW-RESILIENCE.3: state-hash ledgers and the divergence check (crd/perf/state_hash.hpp) -- the hash and chain bits,
// the bounded ledger, the payload and its refusals, and the monitor's verdicts, culprits and causes, online and over
// decoded ledgers.

#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/state_hash.hpp>

#include <catch2/catch_test_macros.hpp>

namespace cont = crd::containers;
namespace cp = crd::perf;
using crd::u32;
using crd::u64;
using crd::u8;

namespace
{

cont::StringView view(const cont::String& s)
{
    return cont::StringView{s.data(), s.size()};
}

bool contains(const cont::String& s, cont::StringView part)
{
    return view(s).find(part) != cont::StringView::npos;
}

// A step's state in a run: a pure function of the step, or a different value from `from` on (a diverging machine).
u64 good_hash(u64 step)
{
    return 0x9000U + step * 3U;
}

u64 run_hash(u64 step, u64 from)
{
    return step >= from ? good_hash(step) + 1U : good_hash(step);
}

void record_run(cp::StateHashLedger& l, u64 first, u64 last, u64 diverge_from = ~u64{0})
{
    for (u64 s = first; s <= last; ++s)
    {
        REQUIRE(l.record_hash(s, run_hash(s, diverge_from)));
    }
}

cp::HardwareIdentity machine(cont::StringView vendor, u32 signature, cont::StringView model, u32 microcode)
{
    cp::HardwareIdentity h;
    h.flags = cp::kHardwareCpuKnown | cp::kHardwareMicrocodeKnown;
    h.cpu_vendor.append(vendor);
    h.cpu_signature = signature;
    h.cpu_model.append(model);
    h.microcode = microcode;
    return h;
}

u64 fold(u64 chain, const cp::StateHashLedger& l, u64 first, u64 last)
{
    for (u64 s = first; s <= last; ++s)
    {
        const cp::StateHashEntry* e = l.find(s);
        chain = cp::chain_state_hash(chain, s, e != nullptr ? e->hash : good_hash(s));
    }
    return chain;
}

} // namespace

TEST_CASE("state hash and chain have the documented bits", "[perf][state-hash]")
{
    const u8 state[] = {1U, 2U, 3U, 4U, 5U};
    CHECK(cp::state_hash({state, sizeof(state)}) == cont::xxh3_64({state, sizeof(state)}));

    // chain_n = xxh3_64(LE64(chain) ++ LE64(step) ++ LE64(hash)), seed 0.
    const u8 le[24] = {
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U, // chain
        0x2AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, // step
        0xEFU, 0xBEU, 0xADU, 0xDEU, 0x00U, 0x00U, 0x00U, 0x80U, // hash
    };
    CHECK(cp::chain_state_hash(0x0102030405060708ULL, 42U, 0x80000000DEADBEEFULL) == cont::xxh3_64({le, sizeof(le)}));
}

TEST_CASE("a ledger keeps its last steps and the chain of all of them", "[perf][state-hash]")
{
    cp::StateHashLedger l("host", 4U);
    CHECK(l.recorded() == 0U);
    CHECK(l.chain() == 0U);
    record_run(l, 10U, 12U);
    CHECK_FALSE(l.record_hash(12U, 1U)); // not after the last step
    CHECK_FALSE(l.record_hash(3U, 1U));
    CHECK(l.size() == 3U);
    CHECK(l.base_chain() == 0U);
    record_run(l, 13U, 17U);
    CHECK(l.recorded() == 8U);
    CHECK(l.size() == 4U);
    CHECK(l.first_step() == 10U);
    CHECK(l.entry(0U).step == 14U);
    CHECK(l.entry(3U).step == 17U);
    CHECK(l.find(13U) == nullptr);
    REQUIRE(l.find(15U) != nullptr);
    CHECK(l.find(15U)->hash == good_hash(15U));
    CHECK(l.base_chain() == fold(0U, l, 10U, 13U));
    CHECK(l.chain() == fold(0U, l, 10U, 17U));

    cp::StateHashLedger state_steps("state", 0U); // capacity 0 keeps one step
    const u8 bytes[] = {7U, 7U};
    CHECK(state_steps.capacity() == 1U);
    CHECK(state_steps.record(1U, {bytes, sizeof(bytes)}));
    CHECK(state_steps.record(2U, {bytes, 1U}));
    CHECK(state_steps.size() == 1U);
    CHECK(state_steps.entry(0U).hash == cont::xxh3_64({bytes, 1U}));
}

TEST_CASE("a ledger payload round-trips and refuses malformed bytes", "[perf][state-hash]")
{
    for (const u32 capacity : {64U, 5U})
    {
        cp::StateHashLedger l("peer 2", capacity);
        record_run(l, 100U, 111U);
        const cont::Array<u8> bytes = cp::encode_state_hash_ledger(l);
        const cp::StateHashLedgerRead r = cp::decode_state_hash_ledger({bytes.data(), bytes.size()});
        REQUIRE(r.ok);
        CHECK(r.ledger.name() == cont::StringView{"peer 2"});
        CHECK(r.ledger.capacity() == capacity);
        CHECK(r.ledger.recorded() == l.recorded());
        CHECK(r.ledger.first_step() == 100U);
        CHECK(r.ledger.size() == l.size());
        CHECK(r.ledger.base_chain() == l.base_chain());
        CHECK(r.ledger.chain() == l.chain());
        CHECK(r.ledger.entry(0U).step == l.entry(0U).step);

        // Every prefix and one trailing byte are refused.
        for (crd::usize n = 0U; n < bytes.size(); ++n)
        {
            CHECK_FALSE(cp::decode_state_hash_ledger({bytes.data(), n}).ok);
        }
        cont::Array<u8> longer(bytes);
        longer.push_back(0U);
        const cp::StateHashLedgerRead trailing = cp::decode_state_hash_ledger({longer.data(), longer.size()});
        CHECK_FALSE(trailing.ok);
        CHECK(trailing.error == cont::StringView{"trailing bytes after the ledger"});
    }

    cp::StateHashLedger l("p", 3U);
    record_run(l, 1U, 5U);
    const cont::Array<u8> good = cp::encode_state_hash_ledger(l);
    // magic 0, version 4, flags 8, name 12 (2 + 1), capacity 15, first 19, recorded 27, base 35, count 43, steps 47.
    const auto refused = [&](crd::usize at, u8 value, cont::StringView why)
    {
        cont::Array<u8> b(good);
        b[at] = value;
        const cp::StateHashLedgerRead r = cp::decode_state_hash_ledger({b.data(), b.size()});
        CHECK_FALSE(r.ok);
        CHECK(r.error == why);
    };
    refused(0U, 0U, "not a state-hash ledger");
    refused(4U, 2U, "unknown state-hash ledger version");
    refused(8U, 1U, "unknown state-hash ledger flags");
    refused(12U, 65U, "ledger name over its bound or past the payload");
    refused(15U, 2U, "ledger capacity or step count over its bound");    // capacity 2 below the 3 kept
    refused(18U, 0x80U, "ledger capacity or step count over its bound"); // capacity past kMaxStateLedgerSteps
    refused(27U, 3U, "ledger counts disagree");                          // all kept, with a non-zero base chain
    refused(27U, 2U, "ledger counts disagree");                          // more kept than recorded
    refused(47U, 9U, "ledger steps out of order");                       // the oldest kept step after the next
    refused(19U, 3U, "ledger steps out of order");                       // the oldest kept step not after the first

    cp::StateHashLedger all("q", 8U);
    record_run(all, 4U, 6U);
    cont::Array<u8> b = cp::encode_state_hash_ledger(all);
    b[19U] = 5U; // first step 5, but the oldest of all recorded steps is 4
    CHECK(cp::decode_state_hash_ledger({b.data(), b.size()}).error == cont::StringView{"ledger steps out of order"});
}

TEST_CASE("the monitor names the participant that leaves the majority", "[perf][state-hash]")
{
    const cp::HardwareIdentity intel =
        machine("GenuineIntel", 0x000A0671U, "11th Gen Intel(R) Core(TM) i7-11700K @ 3.60GHz", 0x62U);
    const cp::HardwareIdentity amd = machine("AuthenticAMD", 0x00A60F12U, "AMD Ryzen 9 7950X 16-Core Processor", 7U);
    cp::StateHashLedger a("alpha");
    cp::StateHashLedger b("beta");
    cp::StateHashLedger c("gamma");
    cp::StateDivergenceMonitor m;
    REQUIRE(m.add(a, &intel) == 0U);
    REQUIRE(m.add(b, &intel) == 1U);
    REQUIRE(m.add(c, &amd) == 2U);
    CHECK(m.update().verdict == cp::StateDivergenceVerdict::Waiting);

    // Online: each step is judged as soon as all three reached it; gamma's step 7 differs.
    for (u64 s = 1U; s <= 9U; ++s)
    {
        REQUIRE(a.record_hash(s, run_hash(s, ~u64{0})));
        REQUIRE(b.record_hash(s, run_hash(s, ~u64{0})));
        CHECK(m.update().compared == s - 1U); // gamma has not reached it
        REQUIRE(c.record_hash(s, run_hash(s, 7U)));
        const cp::StateDivergence& r = m.update();
        CHECK(r.compared == s);
        CHECK(r.verdict == (s < 7U ? cp::StateDivergenceVerdict::Agree : cp::StateDivergenceVerdict::Diverged));
    }
    const cp::StateDivergence& r = m.result();
    CHECK(r.step == 7U);
    CHECK_FALSE(r.before);
    CHECK(r.chains_compared);
    CHECK(r.expected_known);
    CHECK(r.expected.hash == good_hash(7U));
    REQUIRE(r.values.size() == 3U);
    CHECK(r.values[0].agrees);
    CHECK(r.values[1].agrees);
    CHECK_FALSE(r.values[2].agrees);
    CHECK(r.values[2].hash == good_hash(7U) + 1U);
    CHECK(r.cause == cp::StateDivergenceCause::ProcessorDifference);
    CHECK(r.last_step == 9U);

    const cont::String text = cp::state_divergence_message(m);
    CHECK(contains(text, "The states diverged at step 7: gamma (AMD Ryzen 9 7950X 16-Core Processor, microcode 0x7)"));
    CHECK(contains(text, "where the majority computed 0x9015"));
    CHECK(contains(text, "code whose results depend on the processor"));

    // Participants and the reference are fixed once a step is judged.
    cp::StateHashLedger late("late");
    CHECK(m.add(late, nullptr) == cp::kNoStateParticipant);
    CHECK_FALSE(m.set_reference(0U));
}

TEST_CASE("the monitor points at the machine when its processor agrees elsewhere", "[perf][state-hash]")
{
    // A Raptor Lake desktop part before the fix diverges; another with the same processor agrees.
    const cp::HardwareIdentity raptor =
        machine("GenuineIntel", 0x000B0671U, "13th Gen Intel(R) Core(TM) i9-13900K", 0x125U);
    cp::StateHashLedger good("good", 16U);
    cp::StateHashLedger bad("bad", 16U);
    cp::StateHashLedger other("other", 16U);
    record_run(good, 0U, 20U);
    record_run(bad, 0U, 20U, 12U);
    record_run(other, 0U, 20U);
    cp::StateDivergenceMonitor m;
    (void)m.add(good, &raptor);
    (void)m.add(bad, &raptor);
    (void)m.add(other, nullptr);
    const cp::StateDivergence& r = m.update();
    REQUIRE(r.verdict == cp::StateDivergenceVerdict::Diverged);
    CHECK(r.step == 12U);
    CHECK(r.skipped == 0U); // all three 16-step windows start at step 5
    CHECK(r.compared == 16U);
    CHECK(r.cause == cp::StateDivergenceCause::Machine);
    REQUIRE(r.values[1].advisory != nullptr);
    CHECK(r.values[1].microcode == cp::MicrocodeState::BeforeFix);
    CHECK(r.values[0].advisory == nullptr); // judged only for a diverging machine
    const cont::String text = cp::state_divergence_message(m);
    CHECK(contains(text, "bad (13th Gen Intel(R) Core(TM) i9-13900K, microcode 0x125) computed 0x9025"));
    CHECK(contains(text, "the diverging machine is the likely cause"));
    CHECK(contains(text, "older than the fix 0x12f"));

    // An unrecorded diverging machine leaves the cause unknown.
    cp::StateDivergenceMonitor unknown;
    (void)unknown.add(good, &raptor);
    (void)unknown.add(bad, nullptr);
    (void)unknown.add(other, &raptor);
    CHECK(unknown.update().cause == cp::StateDivergenceCause::Unknown);
    CHECK(contains(cp::state_divergence_message(unknown), "bad (an unrecorded machine)"));
}

TEST_CASE("the monitor judges against a reference, and names no culprit without one", "[perf][state-hash]")
{
    cp::StateHashLedger recorded("recorded");
    cp::StateHashLedger replay("replay");
    record_run(recorded, 1U, 6U);
    record_run(replay, 1U, 6U, 4U);

    cp::StateDivergenceMonitor two;
    (void)two.add(recorded, nullptr);
    (void)two.add(replay, nullptr);
    const cp::StateDivergence& u = two.update();
    CHECK(u.verdict == cp::StateDivergenceVerdict::Undecided);
    CHECK(u.step == 4U);
    CHECK_FALSE(u.expected_known);
    CHECK(contains(cp::state_divergence_message(two), "neither can be named"));

    cp::StateDivergenceMonitor ref;
    (void)ref.add(replay, nullptr);
    (void)ref.add(recorded, nullptr);
    CHECK_FALSE(ref.set_reference(2U));
    REQUIRE(ref.set_reference(1U));
    const cp::StateDivergence& d = ref.update();
    CHECK(d.verdict == cp::StateDivergenceVerdict::Diverged);
    CHECK(d.step == 4U);
    CHECK_FALSE(d.values[0].agrees);
    CHECK(d.values[1].agrees);
    CHECK(contains(cp::state_divergence_message(ref), "where the reference recorded computed 0x900c"));

    // Three different states and no reference: no majority.
    cp::StateHashLedger fourth("fourth");
    for (u64 s = 1U; s <= 6U; ++s)
    {
        REQUIRE(fourth.record_hash(s, s < 4U ? good_hash(s) : 5U));
    }
    cp::StateDivergenceMonitor none;
    (void)none.add(recorded, nullptr);
    (void)none.add(replay, nullptr);
    (void)none.add(fourth, nullptr);
    CHECK(none.update().verdict == cp::StateDivergenceVerdict::NoMajority);
    CHECK(contains(cp::state_divergence_message(none), "no state is held by more than half"));
}

TEST_CASE("a step one participant did not record is a divergence", "[perf][state-hash]")
{
    cp::StateHashLedger a("a");
    cp::StateHashLedger b("b");
    cp::StateHashLedger c("c");
    record_run(a, 1U, 8U);
    record_run(c, 1U, 8U);
    for (u64 s = 1U; s <= 8U; ++s)
    {
        if (s != 5U)
        {
            REQUIRE(b.record_hash(s, good_hash(s)));
        }
    }
    cp::StateDivergenceMonitor m;
    (void)m.add(a, nullptr);
    (void)m.add(b, nullptr);
    (void)m.add(c, nullptr);
    const cp::StateDivergence& r = m.update();
    REQUIRE(r.verdict == cp::StateDivergenceVerdict::Diverged);
    CHECK(r.step == 5U);
    CHECK_FALSE(r.values[1].present);
    CHECK_FALSE(r.values[1].agrees);
    CHECK(contains(cp::state_divergence_message(m), "b (an unrecorded machine) computed no state"));
}

TEST_CASE("a divergence older than every window is seen through the chains", "[perf][state-hash]")
{
    // Gamma differs only at step 3, which no ledger keeps any more: the first judged step's hashes agree, its
    // chains do not.
    cp::StateHashLedger a("a", 4U);
    cp::StateHashLedger b("b", 4U);
    cp::StateHashLedger c("c", 4U);
    record_run(a, 1U, 10U);
    record_run(b, 1U, 10U);
    for (u64 s = 1U; s <= 10U; ++s)
    {
        REQUIRE(c.record_hash(s, s == 3U ? 0U : good_hash(s)));
    }
    cp::StateDivergenceMonitor m;
    (void)m.add(a, nullptr);
    (void)m.add(b, nullptr);
    (void)m.add(c, nullptr);
    const cp::StateDivergence& r = m.update();
    REQUIRE(r.verdict == cp::StateDivergenceVerdict::Diverged);
    CHECK(r.step == 7U);
    CHECK(r.before);
    CHECK(r.skipped == 0U); // all three windows start at step 7
    CHECK_FALSE(r.values[2].agrees);
    CHECK(contains(cp::state_divergence_message(m), "The states diverged at or before step 7: c"));

    // A longer window walks its older steps without a judgement, and its chain still covers them.
    cp::StateHashLedger wide("wide", 64U);
    record_run(wide, 1U, 10U);
    cp::StateDivergenceMonitor mixed;
    (void)mixed.add(wide, nullptr);
    (void)mixed.add(b, nullptr);
    (void)mixed.add(c, nullptr);
    const cp::StateDivergence& w = mixed.update();
    REQUIRE(w.verdict == cp::StateDivergenceVerdict::Diverged);
    CHECK(w.skipped == 6U); // steps 1 to 6 only the wide window keeps
    CHECK(w.step == 7U);
    CHECK(w.before);
    CHECK(w.compared == 4U);

    // Different first steps make the chains incomparable: only the step hashes are judged.
    cp::StateHashLedger d("d", 4U);
    record_run(d, 2U, 10U);
    cp::StateDivergenceMonitor late;
    (void)late.add(a, nullptr);
    (void)late.add(d, nullptr);
    const cp::StateDivergence& agree = late.update();
    CHECK(agree.verdict == cp::StateDivergenceVerdict::Agree);
    CHECK(agree.compared == 4U);
}

TEST_CASE("decoded ledgers are judged as the live ones", "[perf][state-hash]")
{
    cp::StateHashLedger recorded("recorded", 8U);
    cp::StateHashLedger rerun("rerun", 8U);
    record_run(recorded, 1U, 30U);
    record_run(rerun, 1U, 30U, 27U);
    const cont::Array<u8> rb = cp::encode_state_hash_ledger(recorded);
    const cont::Array<u8> eb = cp::encode_state_hash_ledger(rerun);
    const cp::StateHashLedgerRead r1 = cp::decode_state_hash_ledger({rb.data(), rb.size()});
    const cp::StateHashLedgerRead r2 = cp::decode_state_hash_ledger({eb.data(), eb.size()});
    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    cp::StateDivergenceMonitor m;
    (void)m.add(r1.ledger, nullptr);
    (void)m.add(r2.ledger, nullptr);
    REQUIRE(m.set_reference(0U));
    const cp::StateDivergence& r = m.update();
    CHECK(r.verdict == cp::StateDivergenceVerdict::Diverged);
    CHECK(r.step == 27U);
    CHECK_FALSE(r.before);
    CHECK(r.compared == 8U); // steps 23 to 30; the first divergence stays latched
}
