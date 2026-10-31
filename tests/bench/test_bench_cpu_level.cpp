// The CPU level line of every crd-bench run (ADR-0134 §8): the bvh, gjk, quickhull and simd benchmarks run the
// dispatched kernels of the selected level, so the record starts with the selection and the build's kernel cap.

#include <crd/core/cpu_features.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdio>

namespace
{

struct CpuLevelListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        char line[512] = {}; // NOLINT(modernize-avoid-c-arrays)
        (void)crd::format_cpu_selection(crd::cpu_selection(), crd::cpu_runtime_dispatch(), line, sizeof(line));
        std::printf("%s kernel_cap=%s\n", line, crd::cpu_level_name(crd::kernel_level_cap()));
        (void)std::fflush(stdout);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(CpuLevelListener)
