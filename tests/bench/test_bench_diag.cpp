// What the investigation runbooks' always-present mechanisms cost per call (DIAG.11b clause (9)): the generation
// check of a handle lookup, recording a run's trace (replay.record), and the boundary probes (float mode, value
// domain, pressure, lifetime ledger). Each case checks its answer before it times anything, so a lookup, trace or
// probe that computes something else fails the case. Run: crd-bench "[bench-diag]"; board:
// docs/bench/2026-10-24-diag-11b-checks-recording-probes.md.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/slot_map.hpp>
#include <crd/core/fp_environment.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/value_domain.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/scene/slot_map.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{

using crd::u32;
using crd::u64;
using crd::usize;

constexpr u32 kLookups = 4096U;

// The lookup order: every index once, shuffled by a fixed generator so the loads are not sequential.
crd::containers::Array<u32> shuffled_indices()
{
    crd::containers::Array<u32> order;
    for (u32 i = 0U; i < kLookups; ++i)
    {
        order.push_back(i);
    }
    u64 state = 0x11B9ULL;
    for (u32 i = kLookups - 1U; i > 0U; --i)
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const u32 j = static_cast<u32>((state >> 33U) % (i + 1U));
        const u32 tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    return order;
}

void nothing() {}

void register_dialects(crd::ceir::Context& ctx, void* user)
{
    (void)user;
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
}

} // namespace

TEST_CASE("bench diag: generation-checked lookups against a raw index", "[bench][bench-diag][!benchmark]")
{
    const crd::containers::Array<u32> order = shuffled_indices();

    crd::containers::SlotMap<u32> live;
    crd::containers::Array<crd::containers::SlotMap<u32>::Handle> handles;
    crd::containers::Array<u32> raw;
    for (u32 i = 0U; i < kLookups; ++i)
    {
        handles.push_back(live.insert(i));
        raw.push_back(i);
    }
    crd::containers::Array<crd::containers::SlotMap<u32>::Handle> live_order;
    for (const u32 i : order)
    {
        live_order.push_back(handles[i]);
    }

    // The same slots retired and reused: every old handle is one generation behind its slot.
    crd::containers::SlotMap<u32> reused;
    crd::containers::Array<crd::containers::SlotMap<u32>::Handle> stale;
    for (u32 i = 0U; i < kLookups; ++i)
    {
        stale.push_back(reused.insert(i));
    }
    for (const crd::containers::SlotMap<u32>::Handle h : stale)
    {
        REQUIRE(reused.erase(h));
    }
    for (u32 i = 0U; i < kLookups; ++i)
    {
        (void)reused.insert(i);
    }

    crd::scene::SlotMap entities;
    crd::containers::Array<crd::scene::EntityId> entity_order;
    {
        crd::containers::Array<crd::scene::EntityId> spawned;
        for (u32 i = 0U; i < kLookups; ++i)
        {
            spawned.push_back(entities.allocate());
        }
        for (const u32 i : order)
        {
            entity_order.push_back(spawned[i]);
        }
    }

    const u32* const values = raw.data();
    auto sum_raw = [&]()
    {
        u64 sum = 0U;
        for (const crd::containers::SlotMap<u32>::Handle h : live_order)
        {
            sum += values[h.index];
        }
        return sum;
    };
    auto sum_live = [&]()
    {
        u64 sum = 0U;
        for (const crd::containers::SlotMap<u32>::Handle h : live_order)
        {
            const u32* const v = live.get(h);
            sum += v != nullptr ? *v : 0U;
        }
        return sum;
    };
    auto count_stale = [&]()
    {
        u32 refused = 0U;
        for (const crd::containers::SlotMap<u32>::Handle h : stale)
        {
            refused += reused.get(h) == nullptr ? 1U : 0U;
        }
        return refused;
    };
    auto count_alive = [&]()
    {
        u32 alive = 0U;
        for (const crd::scene::EntityId e : entity_order)
        {
            alive += entities.is_alive(e) ? 1U : 0U;
        }
        return alive;
    };

    const u64 expected = (static_cast<u64>(kLookups) * (kLookups - 1U)) / 2U;
    REQUIRE(sum_raw() == expected);
    REQUIRE(sum_live() == expected);
    REQUIRE(count_stale() == kLookups);
    REQUIRE(count_alive() == kLookups);

    BENCHMARK("raw index x4096 (no check)")
    {
        return sum_raw();
    };
    BENCHMARK("containers SlotMap::get live x4096")
    {
        return sum_live();
    };
    BENCHMARK("containers SlotMap::get stale x4096 (refused)")
    {
        return count_stale();
    };
    BENCHMARK("scene SlotMap::is_alive x4096")
    {
        return count_alive();
    };
}

TEST_CASE("bench diag: recording a run's trace", "[bench][bench-diag][!benchmark]")
{
    crd::containers::String text;
    REQUIRE(crd::platform::fs::read_file_text(crd::platform::fs::Path(CRD_BENCH_DIAG_PROGRAM), text));

    crd::ceir::Context cook_ctx(crd::memory::default_allocator());
    register_dialects(cook_ctx, nullptr);
    const crd::ceir::cook::CookResult cooked =
        crd::ceir::cook::cook_program_text(cook_ctx, crd::containers::StringView{text.data(), text.size()},
                                           crd::containers::StringView{CRD_BENCH_DIAG_PROGRAM}, 0x00A20011U,
                                           crd::memory::default_allocator(), crd::memory::default_allocator());
    REQUIRE(cooked.ok());

    crd::ceir::Context ctx(crd::memory::default_allocator());
    crd::ceir::cook::ReplayProgram program(crd::memory::default_allocator());
    crd::ceir::cook::load_replay_program(ctx, {cooked.blob.data(), cooked.blob.size()}, "main", &register_dialects,
                                         nullptr, program);
    REQUIRE(program.ok());

    constexpr crd::i64 steps = 20000; // main(n) runs its step loop n times, returns n + 1
    const crd::i64 args[1] = {steps}; // NOLINT(modernize-avoid-c-arrays)
    const crd::containers::ConstSpan<crd::i64> arguments{args, 1U};

    auto run_plain = [&]()
    {
        const crd::ceir::plan::RunResult r =
            crd::ceir::plan::run(program.compiled.plan, arguments, crd::memory::default_allocator());
        return r.ok() && r.values.size() == 1U ? r.values[0] : -1;
    };
    // replay.record's run: the trace bound, the host input seam through the recorder (no live source).
    auto run_recorded = [&](u32 max_events, crd::ceir::cook::ReplayTrace& trace)
    {
        crd::ceir::cook::InputRecorder inputs(trace, nullptr);
        crd::ceir::cook::run_traced(program, arguments, max_events, nullptr, trace, inputs.source());
        return trace.results.size() == 1U ? trace.results[0] : -1;
    };

    REQUIRE(run_plain() == steps + 1);
    crd::ceir::cook::ReplayTrace probe(crd::memory::default_allocator());
    REQUIRE(run_recorded(crd::ceir::cook::kReplayMaxEvents, probe) == steps + 1);
    REQUIRE(probe.error == crd::ceir::plan::RunError::None);
    // Three dispatches a step and four around the loop: the bound keeps every event, the default the first 4096.
    REQUIRE(probe.events_total == (3U * static_cast<u64>(steps)) + 4U);
    REQUIRE(probe.events.size() == probe.events_total);
    const u64 dispatches = probe.events_total;

    // The record replay.record writes, held in memory: the encode is its cost before the file write.
    auto make_record = [&](u32 max_events, crd::ceir::cook::ReplayRecord& record)
    {
        crd::ceir::cook::ReplayTrace trace(crd::memory::default_allocator());
        REQUIRE(run_recorded(max_events, trace) == steps + 1);
        REQUIRE(trace.events_total == dispatches);
        REQUIRE(trace.events.size() == (dispatches < max_events ? dispatches : max_events));
        record.build = crd::ceir::cook::current_build(crd::memory::default_allocator());
        record.program_path.append(CRD_BENCH_DIAG_PROGRAM);
        record.content_hash = program.content_hash;
        for (const crd::u8 b : cooked.blob)
        {
            record.program.push_back(b);
        }
        record.entry.append("main");
        record.args.push_back(steps);
        record.max_events = max_events;
        record.events_total = trace.events_total;
        record.events = static_cast<crd::containers::Array<crd::ceir::cook::ReplayEvent>&&>(trace.events);
        record.results = static_cast<crd::containers::Array<crd::i64>&&>(trace.results);
    };
    crd::ceir::cook::ReplayRecord small(crd::memory::default_allocator());
    crd::ceir::cook::ReplayRecord large(crd::memory::default_allocator());
    make_record(crd::ceir::cook::kReplayDefaultMaxEvents, small);
    make_record(crd::ceir::cook::kReplayMaxEvents, large);
    crd::containers::Array<crd::u8> bytes;
    crd::ceir::cook::encode_record(small, bytes);
    REQUIRE(crd::ceir::cook::is_replay_record({bytes.data(), bytes.size()}));
    WARN("dispatches " << dispatches << ", record bytes " << bytes.size());
    crd::ceir::cook::encode_record(large, bytes);
    REQUIRE(crd::ceir::cook::is_replay_record({bytes.data(), bytes.size()}));
    WARN("dispatches " << dispatches << ", record bytes " << bytes.size());

    BENCHMARK("plan::run main(20000)")
    {
        return run_plain();
    };
    BENCHMARK("run_traced main(20000), 4096 events kept (replay.record's default)")
    {
        crd::ceir::cook::ReplayTrace trace(crd::memory::default_allocator());
        return run_recorded(crd::ceir::cook::kReplayDefaultMaxEvents, trace);
    };
    BENCHMARK("run_traced main(20000), every event kept (the 65536 bound)")
    {
        crd::ceir::cook::ReplayTrace trace(crd::memory::default_allocator());
        return run_recorded(crd::ceir::cook::kReplayMaxEvents, trace);
    };
    BENCHMARK("encode_record, 4096 events")
    {
        crd::containers::Array<crd::u8> out;
        crd::ceir::cook::encode_record(small, out);
        return out.size();
    };
    BENCHMARK("encode_record, every event (60004)")
    {
        crd::containers::Array<crd::u8> out;
        crd::ceir::cook::encode_record(large, out);
        return out.size();
    };
}

TEST_CASE("bench diag: boundary probes", "[bench][bench-diag][!benchmark]")
{
    const crd::FpEnvironment declared = crd::declared_fp_environment();
    REQUIRE(crd::fp_differences(crd::read_fp_environment(), declared) == 0U);

    constexpr usize block = 1024U; // a 512-frame stereo audio block
    crd::containers::Array<float> samples;
    for (usize i = 0U; i < block; ++i)
    {
        samples.push_back(static_cast<float>(i % 64U) / 64.0F);
    }
    const crd::FloatDomain domain = crd::finite_float_domain();
    REQUIRE(crd::probe_values(domain, samples.data(), samples.size()).checked == block);
    REQUIRE(crd::probe_values(domain, samples.data(), samples.size()).violating == 0U);

    crd::LifetimeLedger& ledger = crd::lifetime_ledger();
    const u64 live_before = ledger.snapshot().of(crd::LifetimeClass::OsObject).live;

    crd::jobs::init();
    REQUIRE(crd::jobs::pressure_snapshot().counters.read);

    // A boundary probe brackets a call the compiler cannot see into (a driver entry, a foreign deleter), which is
    // what keeps its reads from being hoisted: here an empty function called through a volatile pointer.
    void (*volatile foreign)() = &nothing;
    BENCHMARK("an opaque empty call")
    {
        foreign();
        return 0;
    };
    BENCHMARK("call_keeping_fp_environment around the same call (two reads, a compare)")
    {
        return crd::call_keeping_fp_environment([&]() { foreign(); });
    };
    BENCHMARK("probe_values, 1024 finite f32")
    {
        return crd::probe_values(domain, samples.data(), samples.size()).violating;
    };
    BENCHMARK("jobs::pressure_snapshot")
    {
        return crd::jobs::pressure_snapshot();
    };
    BENCHMARK("lifetime ledger snapshot")
    {
        return ledger.snapshot();
    };
    BENCHMARK("lifetime ledger acquired + released")
    {
        ledger.acquired(crd::LifetimeClass::OsObject);
        return ledger.released(crd::LifetimeClass::OsObject);
    };

    crd::jobs::shutdown();
    REQUIRE(ledger.snapshot().of(crd::LifetimeClass::OsObject).live == live_before);
}
