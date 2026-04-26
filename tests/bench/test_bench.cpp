#include <crd/containers/containers.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/memory.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace crd;
using namespace crd::containers;
using namespace crd::log;

CRD_DEFINE_LOG_CHANNEL(g_log_bench, "Bench", crd::log::LogLevel::Trace)

namespace
{
struct BenchLoggerScope
{
    explicit BenchLoggerScope(LoggerConfig cfg = {})
    {
        if (is_initialized())
        {
            shutdown();
        }
        clear_sinks();
        init(cfg);
        add_sink(std::make_unique<NullSink>());
        g_log_bench.runtime_level = LogLevel::Trace;
    }

    ~BenchLoggerScope() { shutdown(); }
};

CRD_FORCEINLINE void disabled_trace_call() noexcept
{
    CRD_LOG_TRACE(g_log_bench, "disabled trace {}", 42);
}
} // namespace

TEST_CASE("Disabled CRD_LOG_TRACE cost", "[bench][log]")
{
    BenchLoggerScope scope{};

    BENCHMARK("disabled trace call")
    {
        disabled_trace_call();
        return 0;
    };
}

TEST_CASE("Async producer push cost", "[bench][log]")
{
    LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1 << 16;
    cfg.drop_on_overflow = false;
    BenchLoggerScope scope{cfg};

    BENCHMARK("async log push")
    {
        CRD_LOG_INFO(g_log_bench, "bench message {}", 7);
        return 0;
    };

    flush();
}

TEST_CASE("Array push_back amortised 1k", "[bench][containers]")
{
    BENCHMARK("Array<u32>::push_back 1k")
    {
        Array<u32> values;
        for (u32 i = 0; i < 1024; ++i)
        {
            values.push_back(i);
        }
        return values.size();
    };
}

TEST_CASE("HashMap integer workloads", "[bench][containers]")
{
    constexpr u32 kCount = 1u << 20;

    BENCHMARK("HashMap<u32,u32> insert 1M")
    {
        HashMap<u32, u32> map;
        for (u32 i = 0; i < kCount; ++i)
        {
            (void)map.insert(i, i + 1);
        }
        return map.size();
    };

    HashMap<u32, u32> seeded;
    seeded.reserve(kCount);
    for (u32 i = 0; i < kCount; ++i)
    {
        (void)seeded.insert(i, i + 1);
    }

    BENCHMARK("HashMap<u32,u32> find 1M")
    {
        u64 sum = 0;
        for (u32 i = 0; i < kCount; ++i)
        {
            const u32* value = seeded.find(i);
            sum += value ? *value : 0u;
        }
        return sum;
    };

    BENCHMARK("HashMap<u32,u32> erase 1M")
    {
        HashMap<u32, u32> map = seeded;
        for (u32 i = 0; i < kCount; ++i)
        {
            (void)map.erase(i);
        }
        return map.size();
    };
}

TEST_CASE("String SSO vs heap workloads", "[bench][containers]")
{
    BENCHMARK("String SSO construct+assign")
    {
        String s("cerid");
        s.append(StringView{"-bench"});
        return s.size();
    };

    BENCHMARK("String heap construct+assign")
    {
        String s("this-string-is-definitely-longer-than-the-sso-boundary");
        s.append(StringView{"-and-it-grows-even-more"});
        return s.size();
    };
}
