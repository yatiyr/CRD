// The jet probe, compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5): this level's
// JetPackD over every operation and slope at every probe width, and its gradient and Jacobian drivers on ragged tiles
// (../jet_probe_ops.hpp). The table is this level's (CRD_KERNEL_TABLE); everything else lives in the level namespace.

#include "../jet_level_probe.hpp"

#include "../jet_probe_ops.hpp"

#include <crd/core/kernel_level.hpp>

namespace crd::test::jet_probe::inline CRD_MATH_LEVEL_NS
{

constexpr auto kOps = &ops;
constexpr auto kDrivers = &drivers;

} // namespace crd::test::jet_probe::inline CRD_MATH_LEVEL_NS

namespace crd::test::jet_probe
{

const Table CRD_KERNEL_TABLE(kJetProbe) = {
    CRD_MATH_LEVEL_NS::kOps,
    CRD_MATH_LEVEL_NS::kDrivers,
    {CRD_MATH_LEVEL_NS::layout<1>(), CRD_MATH_LEVEL_NS::layout<3>(), CRD_MATH_LEVEL_NS::layout<4>(),
     CRD_MATH_LEVEL_NS::layout<5>(), CRD_MATH_LEVEL_NS::layout<7>(), CRD_MATH_LEVEL_NS::layout<8>(),
     CRD_MATH_LEVEL_NS::layout<9>(), CRD_MATH_LEVEL_NS::layout<15>(), CRD_MATH_LEVEL_NS::layout<16>(),
     CRD_MATH_LEVEL_NS::layout<17>(), CRD_MATH_LEVEL_NS::layout<32>()},
};

} // namespace crd::test::jet_probe
