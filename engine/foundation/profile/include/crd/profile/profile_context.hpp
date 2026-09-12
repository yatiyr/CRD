#pragma once

#include <crd/core/types.hpp>

namespace crd::profile
{
// Closed enums for v1n's predicate axes (ADR-0060 §2). New enum entries
// APPEND only — never insert — so the on-disk byte representation stays
// stable across versions. The `Unknown = 0` sentinel is the default for
// unconstructed contexts; predicates never match `Unknown`.

enum class OperatingSystem : crd::u8
{
    Unknown = 0,
    Windows = 1,
    Linux   = 2,
    MacOS   = 3,
};

enum class GpuTier : crd::u8
{
    Unknown = 0,
    Low     = 1,
    Mid     = 2,
    High    = 3,
    Ultra   = 4,
};

enum class ProjectDomain : crd::u8
{
    Unknown    = 0,
    Game       = 1,
    Simulation = 2,
    Daw        = 3,
    Cinematic  = 4,
};

enum class AppMode : crd::u8
{
    Unknown  = 0,
    Editor   = 1,
    Runtime  = 2,
    Headless = 3,
};

// ProfileContext — runtime detection bag fed to the resolver (ADR-0060 §4,
// §9). v1n5 ships the type and its defaults; the actual detection helpers
// (detect_os / detect_gpu_tier / etc.) and the resolver land in v1n6.
struct ProfileContext
{
    OperatingSystem os         = OperatingSystem::Unknown;
    GpuTier         gpu_tier   = GpuTier::Unknown;
    ProjectDomain   domain     = ProjectDomain::Unknown;
    AppMode         mode       = AppMode::Unknown;
    crd::i32        target_fps = 60;
    crd::i32        cpu_cores  = 1;
};

} // namespace crd::profile
