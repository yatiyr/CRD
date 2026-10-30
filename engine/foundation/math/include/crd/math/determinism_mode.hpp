// The engine's determinism modes (ADR-0063; ADR-0134 §7). eylem's scene configuration names them
// (crd::eylem::DeterminismMode), and the process's kernel families plan under one of them
// (crd::math::kernel_determinism, crd/math/kernel_dispatch.hpp). This header includes only the core types.

#pragma once

#include <crd/core/types.hpp>

namespace crd::math
{

enum class DeterminismMode : crd::u8
{
    // Same machine, same build: bit-exact replay. Each CPU level may run its own fastest plan of a kernel family.
    Default = 0,
    // Bit-exact across MSVC, Clang and GCC, x64 and ARM64 and every CPU level: the crd::math::deterministic
    // substrate, commutative cross-thread merges, and each kernel family's one canonical plan on every level.
    CrossPlatform = 1,
    // Reserved: serialise-stable across engine versions. Kernel families plan as under CrossPlatform.
    BackwardCompat = 2
};

} // namespace crd::math
