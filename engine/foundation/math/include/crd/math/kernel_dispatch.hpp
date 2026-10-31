// The kernel determinism mode of the math and hesap kernel families (SIMD-DISPATCH, ADR-0134 §7). The dispatch
// mechanism, the KernelLevel tables and their selection, is crd-core's (crd/core/kernel_dispatch.hpp).

#pragma once

#include <crd/core/kernel_dispatch.hpp>
#include <crd/math/determinism_mode.hpp>

namespace crd::math
{

// The determinism mode the process's kernel families plan under (ADR-0134 §7), Default until set. Under Default each
// CPU level runs the plan its family measured fastest there, within the family's declared tolerance across levels;
// under CrossPlatform and BackwardCompat every level runs the family's one canonical plan, so a bit-exact family gives
// the same bits on every level (replays, lockstep, golden tests). A plan reads the mode when it is built; code sets it
// (no environment variable), before it builds the plans whose bits must match; a scene's eylem configuration names its
// own mode and does not set this one.
[[nodiscard]] DeterminismMode kernel_determinism() noexcept;
void set_kernel_determinism(DeterminismMode mode) noexcept;

} // namespace crd::math
