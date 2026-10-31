// The CPU level line of a board record (ADR-0134 §8). scripts/run-lane-bench.sh runs it under the CRD_CPU_LEVEL and
// the affinity of each measured program, whose selection is the same function of the CPUs it may run on, their
// OS-saved state, the variable and this build's kernel cap. It prints the selection line (crd::format_cpu_selection)
// and the cap; with --expect it fails when the selection is not the asked one, so a board never records a level it did
// not run. --levels prints a board's level list on these CPUs: detected, then every lower level that both the CPUs
// and the cap include, highest first, so a board measures each level the host runs and none twice.
//   cpu_level_record [--expect <level>|detected | --levels]
// Exit codes: 0 printed (as expected), 1 not the expected selection, 2 usage.

#include <crd/core/cpu_features.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    const char* expect = nullptr;
    const bool levels = argc == 2 && std::strcmp(argv[1], "--levels") == 0;
    if (argc == 3 && std::strcmp(argv[1], "--expect") == 0)
    {
        expect = argv[2];
    }
    else if (argc != 1 && !levels)
    {
        std::fprintf(stderr, "usage: cpu_level_record [--expect <level>|detected | --levels]\n");
        return 2;
    }
    const crd::CpuSelection& s = crd::cpu_selection();
    const crd::CpuLevel cap = crd::kernel_level_cap();
    if (levels)
    {
        // Highest first: the first level both include is the one the detected arm runs, so it is not listed again.
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        constexpr crd::CpuLevel order[] = {crd::CpuLevel::Neon,  crd::CpuLevel::Avx512, crd::CpuLevel::Avx2,
                                           crd::CpuLevel::Sse42, crd::CpuLevel::Sse2,   crd::CpuLevel::Portable};
        std::printf("detected");
        bool top = true;
        for (const crd::CpuLevel level : order)
        {
            if (!crd::cpu_level_includes(s.detected, level) || !crd::cpu_level_includes(cap, level))
            {
                continue;
            }
            if (!top)
            {
                std::printf(" %s", crd::cpu_level_name(level));
            }
            top = false;
        }
        std::printf("\n");
        return 0;
    }
    char line[512] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)crd::format_cpu_selection(s, crd::cpu_runtime_dispatch(), line, sizeof(line));
    std::printf("%s kernel_cap=%s\n", line, crd::cpu_level_name(cap));
    if (expect == nullptr)
    {
        return 0;
    }
    bool ran = s.override_status == crd::CpuOverride::None;
    if (std::strcmp(expect, "detected") != 0)
    {
        // A forced level above the build's cap would run the cap's kernels under the forced level's name.
        ran = s.override_status == crd::CpuOverride::Applied &&
              std::strcmp(crd::cpu_level_name(s.selected), expect) == 0 && crd::cpu_level_includes(cap, s.selected);
    }
    if (!ran)
    {
        std::fprintf(stderr, "cpu_level_record: the board asks the %s level, and this process runs: %s kernel_cap=%s\n",
                     expect, line, crd::cpu_level_name(cap));
        return 1;
    }
    return 0;
}
