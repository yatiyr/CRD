// The forced arm's own check (ADR-0134 §8): crd_add_kernel_level_tests (cmake/CrdSimd.cmake) links this case into
// every kernel suite, and an arm that must run at its level, such as an emulated avx512 arm under Intel SDE, selects
// it beside the suite's filter. CRD_CPU_LEVEL must then be the selected level: a refused level would run the detected
// one and the suite would pass without ever reaching the forced level's tables. Where the C runtime dispatches on its
// own (MSVC x64), its level must not exceed the selected one either. The tag hides it from every other run.

#include <crd/core/cpu_features.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("forced arm: the CPU level CRD_CPU_LEVEL asks for is the selected one", "[.][cpu-level-applied]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    INFO("override " << crd::cpu_override_name(s.override_status) << ", detected " << crd::cpu_level_name(s.detected)
                     << ", selected " << crd::cpu_level_name(s.selected));
    CHECK(s.override_status == crd::CpuOverride::Applied);

    const crd::CpuRuntimeDispatch crt = crd::cpu_runtime_dispatch();
    if (crt.crt_level >= 0)
    {
        // The CRT's levels (isa_availability.h): 1 SSE2, 2 SSE4.2, 5 AVX2, 6 AVX-512.
        const bool below_avx2 = !crd::cpu_level_includes(s.selected, crd::CpuLevel::Avx2);
        crd::i32 cap = 6;
        if (!crd::cpu_level_includes(s.selected, crd::CpuLevel::Sse42))
        {
            cap = 1;
        }
        else if (below_avx2)
        {
            cap = 2;
        }
        else if (!crd::cpu_level_includes(s.selected, crd::CpuLevel::Avx512))
        {
            cap = 5;
        }
        INFO("crt level " << crt.crt_level << ", ucrt fma3 " << crt.ucrt_fma3);
        CHECK(crt.crt_level <= cap);
        CHECK_FALSE((below_avx2 && crt.ucrt_fma3));
    }
}
