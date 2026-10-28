// The forced arm's own check (ADR-0134 §8): crd_add_kernel_level_tests (cmake/CrdSimd.cmake) links this case into
// every kernel suite, and an arm that must run at its level, such as an emulated avx512 arm under Intel SDE, selects
// it beside the suite's filter. CRD_CPU_LEVEL must then be the selected level: a refused level would run the detected
// one and the suite would pass without ever reaching the forced level's tables. The tag hides it from every other run.

#include <crd/core/cpu_features.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("forced arm: the CPU level CRD_CPU_LEVEL asks for is the selected one", "[.][cpu-level-applied]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    INFO("override " << crd::cpu_override_name(s.override_status) << ", detected " << crd::cpu_level_name(s.detected)
                     << ", selected " << crd::cpu_level_name(s.selected));
    CHECK(s.override_status == crd::CpuOverride::Applied);
}
